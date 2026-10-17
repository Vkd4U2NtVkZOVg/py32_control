import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import tres_host as host
from test_probe import Device


class HostDevice(Device):
    def __init__(self):
        super().__init__()
        self.temp = [0] * 4
        self.closed = False

    def close(self):
        self.closed = True

    def write_i2c_block_data(self, addr, reg, data):
        if reg == 0x20:
            self.temp = data[:]
            self.writes.append((reg, data[:]))
        else:
            super().write_i2c_block_data(addr, reg, data)

    def read_reg(self, reg, n=1):
        return self.temp[:] if reg == 0x20 else super().read_reg(reg, n)


def run(dev, args):
    with patch.object(host, 'Tres', return_value=dev), contextlib.redirect_stdout(io.StringIO()):
        host.main(args)


class HostTests(unittest.TestCase):
    def test_readonly_default_and_close(self):
        dev = HostDevice()
        run(dev, [])
        self.assertEqual(dev.writes, [])
        self.assertTrue(dev.closed)

    def test_set_save_and_negative_temperature(self):
        dev = HostDevice()
        run(dev, ['set', 'PID_KP', '2.0', '--save'])
        self.assertEqual(dev.flash[0], 131072)
        run(dev, ['temp', '-5500'])
        self.assertEqual(bytes(dev.temp), b'\xff\xff\xea\x84')

    def test_backup_restore(self):
        dev = HostDevice()
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as tmp:
            path = str(Path(tmp) / 'backup.json')
            run(dev, ['backup', path])
            expected = dev.ram[:]
            dev.ram = [0] * len(expected)
            run(dev, ['restore', path, '--save'])
            self.assertEqual(dev.flash, expected)
            with self.assertRaises(FileExistsError):
                run(dev, ['backup', path])

    def test_malformed_backup_rejected_before_bus_open(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as tmp:
            path = Path(tmp) / 'bad.json'
            for data in ([], {'format': 'tres-params-v1', 'interface': 3,
                               'raw': [True] * len(host.NV_PARAMS)}):
                path.write_text(json.dumps(data), encoding='utf-8')
                with patch.object(host, 'Tres') as constructor, self.assertRaises(ValueError):
                    host.main(['restore', str(path)])
                constructor.assert_not_called()

    def test_publisher_reads_actual_source_and_stops_on_error(self):
        dev = HostDevice()
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as tmp:
            source = Path(tmp) / 'temp'
            source.write_text('45000', encoding='ascii')
            with contextlib.redirect_stdout(io.StringIO()):
                host.publish_temperature(dev, source, 1, once=True)
            self.assertEqual(int.from_bytes(dev.temp, 'big'), 45000)
            writes = len(dev.writes)
            source.write_text('unavailable', encoding='ascii')
            with self.assertRaises(ValueError):
                host.publish_temperature(dev, source, 1, once=True)
            self.assertEqual(len(dev.writes), writes)
            with self.assertRaises(ValueError):
                host.publish_temperature(dev, source, 5, once=True)

    def test_flash_failure_propagates_and_closes(self):
        dev = HostDevice()
        dev.fail_save = True
        with self.assertRaisesRegex(RuntimeError, 'FLASH'):
            run(dev, ['save'])
        self.assertTrue(dev.closed)


if __name__ == '__main__':
    unittest.main()
