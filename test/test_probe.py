"""Offline host-tool tests: no smbus dependency and no hardware access."""
import contextlib
import io
import unittest
from unittest.mock import patch

import tres_i2c_probe as probe


class Device(probe.Tres):
    def __init__(self):
        self.bus = self
        self.addr = 0x50
        self.ram = [100 + i for i in range(len(probe.NV_PARAMS))]
        self.flash = self.ram[:]
        self.writes = []
        self.done = self.error = self.retries = 0
        self.fail_save = False
        self.busy = False

    def write_i2c_block_data(self, addr, reg, data):
        self.writes.append((reg, data[:]))
        if reg == 0:
            magic, cmd, _ = data
            self.error = 0
            if magic != probe.TRES_CMD_MAGIC:
                self.error = 2
            elif cmd == 3:
                self.retries = (self.retries + 1) & 255
            elif cmd == 4:
                if self.fail_save:
                    self.error = 3
                    self.fail_save = False
                else:
                    self.flash = self.ram[:]
            elif cmd == 5:
                self.ram = [0] * len(self.ram)
            elif cmd == 6:
                self.ram = self.flash[:]
            # RESET_MCU never acknowledges: the MCU resets before the parser
            # can advance COMPLETED (and the reboot clears the counter). A
            # host that waits for it must time out.
            if cmd != 7:
                self.done = (self.done + 1) & 255
        else:
            self.ram[(reg - probe.TRES_REG_NV_BASE) // 4] = \
                int.from_bytes(data, 'big')

    def read_reg(self, reg, n=1):
        values = {0x10: 2 | (0x20 if self.busy else 0) | (0x80 if self.error else 0),
                  0x11: self.retries, 0x13: self.error, 0x14: 4, 0x15: self.done}
        for i, val in enumerate(self.ram):
            for j, byte in enumerate(val.to_bytes(4, 'big')):
                values[probe.TRES_REG_NV_BASE + i * 4 + j] = byte
        values[0x36] = (3000 >> 8) & 0xFF
        values[0x37] = 3000 & 0xFF
        for off, b in enumerate((30000).to_bytes(4, "big", signed=True)):
            values[0x38 + off] = b
        for off, b in enumerate((50000).to_bytes(4, "big", signed=True)):
            values[0x3C + off] = b
        for off, b in enumerate((18750).to_bytes(4, "big", signed=True)):
            values[0x40 + off] = b
        return [values.get(reg + i, 0) for i in range(n)]


def run_main(device, *args):
    with patch.object(probe, 'Tres', return_value=device), \
         patch('sys.argv', ['probe', *args]), \
         patch.object(probe.time, 'sleep'), \
         contextlib.redirect_stdout(io.StringIO()) as out:
        probe.main()
        return out.getvalue()


class ProbeTests(unittest.TestCase):
    def test_default_is_read_only(self):
        dev = Device()
        run_main(dev)
        self.assertEqual(dev.writes, [])

    def test_status_reports_dew_point(self):
        dev = Device()
        out = run_main(dev)
        self.assertIn("DEW=+18.8 C", out)

    def test_explicit_command_test(self):
        dev = Device()
        run_main(dev, '--commands')
        self.assertEqual(dev.retries, 2)

    def test_params_restores_all_and_reloads_flash(self):
        dev = Device()
        before = dev.flash[:]
        run_main(dev, '--params')
        self.assertEqual(dev.flash, before)
        self.assertEqual(dev.ram, before)
        self.assertEqual(sum(reg == 0 and data[1] == 6 for reg, data in dev.writes), 2)

    def test_failed_save_is_reported_and_backup_restored(self):
        dev = Device()
        before = dev.flash[:]
        dev.fail_save = True
        with self.assertRaisesRegex(RuntimeError, 'FLASH'):
            run_main(dev, '--params')
        self.assertEqual(dev.flash, before)

    def test_busy_does_not_send(self):
        dev = Device()
        dev.busy = True
        with self.assertRaisesRegex(RuntimeError, 'busy'):
            dev.write_cmd(0xa5, 4)
        self.assertEqual(dev.writes, [])

    def test_completion_counter_wraps(self):
        dev = Device()
        dev.done = 255
        dev.write_cmd(0xa5, 4)
        self.assertEqual(dev.done, 0)

    def test_reset_mcu_sends_without_waiting_for_completion(self):
        # The device never advances COMPLETED for cmd 7 (real reset behaviour),
        # so write_cmd() would time out; the probe must use send_cmd().
        dev = Device()
        out = run_main(dev, '--reset-mcu')
        self.assertEqual([data for reg, data in dev.writes if reg == 0],
                         [[0xA5, 7, 0]])
        self.assertIn("responding again", out)

    def test_reset_mcu_rejected_when_mailbox_busy(self):
        dev = Device()
        dev.busy = True
        with self.assertRaisesRegex(RuntimeError, 'busy'):
            dev.send_cmd(0xa5, 7)
        self.assertEqual(dev.writes, [])

    def test_set_save(self):
        dev = Device()
        run_main(dev, '--set', '0', '1234', '--save')
        self.assertEqual(dev.flash[0], 1234)

    def test_target_offset_reads_back_translated(self):
        dev = Device()
        slot = probe.NV_SLOT_BY_NAME['target_offset']
        dev.ram[slot] = 0xFFFFEC78          # -5000 in two's complement
        out = run_main(dev)
        self.assertIn('TARGET_OFFSET -5,000 mC', out)
        self.assertIn('LOWERED', out)

    def test_target_offset_positive_raises(self):
        dev = Device()
        slot = probe.NV_SLOT_BY_NAME['target_offset']
        dev.ram[slot] = 5000
        out = run_main(dev)
        self.assertIn('TARGET_OFFSET 5,000 mC', out)
        self.assertIn('RAISED', out)

    def test_target_offset_flags_out_of_window_slot(self):
        # A slot value the firmware would have rejected: say so instead of
        # printing a plausible-looking compensation.
        dev = Device()
        slot = probe.NV_SLOT_BY_NAME['target_offset']
        dev.ram[slot] = 30000
        out = run_main(dev)
        self.assertIn('OUTSIDE the firmware window', out)

    def test_target_offset_set_in_range(self):
        dev = Device()
        slot = probe.NV_SLOT_BY_NAME['target_offset']
        out = run_main(dev, '--set', 'TARGET_OFFSET', '-5000', '--save')
        self.assertEqual(dev.flash[slot], 0xFFFFEC78)   # two's complement
        self.assertIn('TARGET_OFFSET', out)

    def test_target_offset_out_of_range_rejected_before_write(self):
        dev = Device()
        slot = probe.NV_SLOT_BY_NAME['target_offset']
        before = dev.ram[slot]
        for bad in ('20001', '-20001', '100000', '-100000'):
            with self.assertRaises(SystemExit):
                run_main(dev, '--set', 'TARGET_OFFSET', bad, '--save')
        self.assertEqual(dev.ram[slot], before)
        self.assertEqual(dev.flash[slot], before)
        self.assertEqual(dev.writes, [])

    def test_target_offset_bounds_are_accepted(self):
        dev = Device()
        slot = probe.NV_SLOT_BY_NAME['target_offset']
        for good, wire in (('0', 0), ('20000', 20000), ('-20000', 0xFFFFB1E0)):
            run_main(dev, '--set', 'TARGET_OFFSET', good)
            self.assertEqual(dev.ram[slot], wire)

    def test_negative_value_on_a_signed_slot(self):
        # --help advertises "a negative decimal such as -20000"; the wire form
        # is two's complement, so -20000 must land as 0xFFFFB1E0.
        dev = Device()
        slot = probe.NV_SLOT_BY_NAME['temp_valid_min']
        run_main(dev, '--set', 'TEMP_VALID_MIN', '-20000', '--save')
        self.assertEqual(dev.ram[slot], 0xFFFFB1E0)
        self.assertEqual(dev.flash[slot], 0xFFFFB1E0)

    def test_negative_value_rejected_on_unsigned_slot(self):
        # EN_OFF_PCT is an unsigned percentage: a negative value must not reach
        # the device (it would otherwise be reinterpreted as two's complement).
        dev = Device()
        slot = probe.NV_SLOT_BY_NAME['en_off_pct']
        before = dev.ram[slot]
        with self.assertRaises(SystemExit):
            run_main(dev, '--set', 'EN_OFF_PCT', '-8')
        self.assertEqual(dev.ram[slot], before)

    def test_unparseable_value_reports_and_writes_nothing(self):
        dev = Device()
        with self.assertRaises(SystemExit):
            run_main(dev, '--set', 'TARGET_OFFSET', 'abc')
        self.assertEqual(dev.writes, [])

    def test_q16_numbers(self):
        scale = 1 / 65536
        for text, expected in [('0x1e', 30), ('0xDEAD', 57005), ('2.0', 131072),
                               ('1e-3', 66), ('131072', 131072), ('1e9999', None)]:
            self.assertEqual(probe.nv_value_to_raw(text, scale), expected)

    def test_negative_numbers_are_parsed_not_mistaken_for_errors(self):
        # -1 used to be the "unparseable" sentinel, colliding with a legal -1.
        for text, expected in [('-1', -1), ('-20000', -20000), ('0x10', 16)]:
            self.assertEqual(probe.nv_value_to_raw(text, 1), expected)


if __name__ == '__main__':
    unittest.main()
