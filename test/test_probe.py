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
            self.done = (self.done + 1) & 255
        else:
            self.ram[(reg - 0x40) // 4] = int.from_bytes(data, 'big')

    def read_reg(self, reg, n=1):
        values = {0x10: 2 | (0x20 if self.busy else 0) | (0x80 if self.error else 0),
                  0x11: self.retries, 0x13: self.error, 0x14: 3, 0x15: self.done}
        for i, val in enumerate(self.ram):
            for j, byte in enumerate(val.to_bytes(4, 'big')):
                values[0x40 + i * 4 + j] = byte
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

    def test_set_save(self):
        dev = Device()
        run_main(dev, '--set', '0', '1234', '--save')
        self.assertEqual(dev.flash[0], 1234)

    def test_q16_numbers(self):
        scale = 1 / 65536
        for text, expected in [('0x1e', 30), ('0xDEAD', 57005), ('2.0', 131072),
                               ('1e-3', 66), ('131072', 131072), ('1e9999', -1)]:
            self.assertEqual(probe.nv_value_to_raw(text, scale), expected)


if __name__ == '__main__':
    unittest.main()
