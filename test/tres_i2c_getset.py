#!/usr/bin/env python3
"""TRES I2C get/set tool (run on the Linux/SDM845 host).

i2cget/i2cset-style access to the PY32F030 TRES register file
(tres_i2c_slave.c), plus a self-test for the host-writable SDM845
temperature window at 0x20 (publishes only on a complete single-
transaction write of all 4 bytes from the window base).

Protocol (tres_i2c_slave.h):
  write: [regPtr] [data ...]      first byte selects the register pointer,
                                  following bytes are data (auto-advance).
  read : [regPtr] (write) RS [data ...]   register pointer auto-increments.

Registers:
  0x00 CMD          W 3-byte command mailbox ([0xA5][cmd][param]) - see
                    tres_i2c_probe.py, not used here.
  0x10 STATUS       R   0x11 RETRIES R   0x12 PINS R
  0x13 LASTERR      R   0x14 VERSION R (0x03)
  0x20 SDM845_TEMP  RW 4-byte window: SDM845 temperature as int32
                    milli-degrees Celsius, MSB first (big endian). The value
                    publishes atomically once all 4 bytes have landed.

Usage:
  python3 tres_i2c_getset.py get 0x10 5          # i2cget-style block read
  python3 tres_i2c_getset.py set 0x20 0xFF 0xFF 0xEA 0x84
                                                 # i2cset-style block write
  python3 tres_i2c_getset.py temp -5500          # write -5.5 C, verify readback
  python3 tres_i2c_getset.py temp --celsius 25.5 # convenience conversion
  python3 tres_i2c_getset.py test                # full self-test of the window

Exit status is non-zero when the bus cannot be opened, VERSION is wrong, or
any temperature readback mismatches what was written.
"""

import argparse
import struct
import sys
import time

TRES_REG_STATUS = 0x10
TRES_REG_RETRIES = 0x11
TRES_REG_PINS = 0x12
TRES_REG_LASTERR = 0x13
TRES_REG_VERSION = 0x14
TRES_REG_SDM845_TEMP = 0x20
TRES_REG_SDM845_TEMP_LEN = 4

TRES_IF_VERSION = 0x03

# milli-degree Celsius values exercised by the "test" subcommand: negative
# (two's complement over the bus), zero, and positive extremes.
SELFTEST_VALUES_MILLIC = (-5500, 0, 25500, -40000, 85000)


class Tres:
    """Minimal register-file access (same wire protocol as tres_i2c_probe)."""

    def __init__(self, bus, addr):
        from smbus2 import SMBus, i2c_msg
        self._SMBus = SMBus
        self._i2c_msg = i2c_msg
        self.bus = SMBus(bus)
        self.addr = addr

    def get(self, reg, n=1):
        """i2cget-style: write regPtr, repeated start, read n bytes."""
        wr = self._i2c_msg.write(self.addr, struct.pack("B", reg))
        rd = self._i2c_msg.read(self.addr, n)
        self.bus.i2c_rdwr(wr, rd)
        return list(rd)

    def set(self, reg, data):
        """i2cset-style: write regPtr then the data bytes in one transaction."""
        self.bus.write_i2c_block_data(self.addr, reg, list(data))

    def get_temp(self):
        """Read the 4-byte window and assemble it (big endian, signed)."""
        raw = self.get(TRES_REG_SDM845_TEMP, TRES_REG_SDM845_TEMP_LEN)
        return struct.unpack(">i", bytes(raw))[0]

    def set_temp(self, millic):
        """Write the window MSB first; the slave publishes on the 4th byte."""
        self.set(TRES_REG_SDM845_TEMP, struct.pack(">i", millic))


def check_version(t):
    version = t.get(TRES_REG_VERSION)[0]
    if version != TRES_IF_VERSION:
        sys.exit("VERSION mismatch: got 0x{:02X}, expected 0x{:02X} - "
                 "is the PY32 running the current firmware?".format(
                     version, TRES_IF_VERSION))


def cmd_get(t, args):
    data = t.get(args.reg, args.n)
    print("get 0x{:02X} [{}]: {}".format(
        args.reg, args.n, " ".join("{:02X}".format(b) for b in data)))
    print("bytes: " + " ".join(str(b) for b in data))


def cmd_set(t, args):
    t.set(args.reg, args.data)
    print("set 0x{:02X} <- {}".format(
        args.reg, " ".join("{:02X}".format(b) for b in args.data)))
    if args.reg == TRES_REG_SDM845_TEMP:
        if len(args.data) != TRES_REG_SDM845_TEMP_LEN:
            print("note: temperature window needs exactly 4 bytes "
                  "to publish; partial writes are ignored")
        else:
            time.sleep(0.05)  # let the ISR finish before reading back
            got = t.get_temp()
            print("readback: {} mC (raw {})".format(
                got, " ".join("{:02X}".format(b) for b in t.get(
                    TRES_REG_SDM845_TEMP, TRES_REG_SDM845_TEMP_LEN))))


def cmd_temp(t, args):
    millic = int(round(args.celsius * 1000)) if args.celsius is not None \
        else args.millic
    t.set_temp(millic)
    time.sleep(0.05)  # ISR publishes on the 4th byte
    got = t.get_temp()
    print("temp: wrote {} mC ({:.1f} C), readback {} mC ({:.1f} C)".format(
        millic, millic / 1000.0, got, got / 1000.0))
    if got != millic:
        sys.exit("FAIL: readback {} != written {}".format(got, millic))
    print("OK")


def cmd_test(t, args):
    check_version(t)
    print("[1] static registers")
    status, retries, pins, lasterr = t.get(TRES_REG_STATUS, 4)
    print("    STATUS=0x{:02X} RETRIES={} PINS=0x{:02X} LASTERR={}".format(
        status, retries, pins, lasterr))

    print("[2] temperature window self-test "
          "(write MSB first, verify readback)")
    failures = 0
    for value in SELFTEST_VALUES_MILLIC:
        t.set_temp(value)
        time.sleep(0.05)
        got = t.get_temp()
        ok = (got == value)
        failures += 0 if ok else 1
        print("    write {:>7} mC -> read {:>7} mC  {}".format(
            value, got, "OK" if ok else "FAIL"))
        if not ok:
            raw = t.get(TRES_REG_SDM845_TEMP, TRES_REG_SDM845_TEMP_LEN)
            print("        raw readback: {}".format(
                " ".join("{:02X}".format(b) for b in raw)))

    print("[3] partial/splice write guard: incomplete writes must not publish")
    keep = t.get_temp()
    cases = (
        ("2 bytes from base",
         lambda: t.set(TRES_REG_SDM845_TEMP, [0x12, 0x34])),
        ("1 byte at base+3 (last-byte splice)",
         lambda: t.set(TRES_REG_SDM845_TEMP + 3, [0x55])),
        ("3 bytes from base, then 1 at base+3 (cross-transaction)",
         lambda: (t.set(TRES_REG_SDM845_TEMP, [0xAA, 0xBB, 0xCC]),
                  time.sleep(0.05),
                  t.set(TRES_REG_SDM845_TEMP + 3, [0xDD]))),
        ("3 bytes from base+1 (mid-window start)",
         lambda: t.set(TRES_REG_SDM845_TEMP + 1, [0x11, 0x22, 0x33])),
    )
    for name, attack in cases:
        attack()
        time.sleep(0.05)
        got = t.get_temp()
        ok = (got == keep)
        failures += 0 if ok else 1
        print("    {:<48} kept {:>7} -> {:>7} mC  {}".format(
            name, keep, got, "OK" if ok else "FAIL"))

    if failures:
        sys.exit("FAIL: {} of {} checks failed".format(
            failures, len(SELFTEST_VALUES_MILLIC) + len(cases)))
    print("OK")


def main():
    epilog = """
Examples:
  python3 tres_i2c_getset.py get 0x10 5
      i2cget-style block read of STATUS..VERSION.
  python3 tres_i2c_getset.py temp -5500
      Write -5.5 C into the SDM845 temperature window and verify readback.
  python3 tres_i2c_getset.py test
      Full self-test: static registers, 5 temperature round-trips
      (negative/zero/positive), and the partial/splice write guards.
"""
    ap = argparse.ArgumentParser(
        description="i2cget/i2cset-style access to the TRES register file, "
                    "plus the SDM845 temperature window self-test.",
        epilog=epilog,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bus", type=int, default=3, metavar="N",
                    help="Linux I2C bus number (default: 3)")
    ap.add_argument("--addr", default="0x50", metavar="ADDR",
                    help="7-bit slave address, decimal or hex (default: 0x50)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_get = sub.add_parser("get", help="read n bytes starting at REG")
    p_get.add_argument("reg", type=lambda x: int(x, 0), metavar="REG")
    p_get.add_argument("n", type=int, nargs="?", default=1, metavar="N")
    p_get.set_defaults(func=cmd_get)

    p_set = sub.add_parser("set", help="write DATA bytes starting at REG")
    p_set.add_argument("reg", type=lambda x: int(x, 0), metavar="REG")
    p_set.add_argument("data", type=lambda x: int(x, 0), nargs="+",
                       metavar="BYTE")
    p_set.set_defaults(func=cmd_set)

    p_temp = sub.add_parser("temp",
                            help="write the SDM845 temperature and verify")
    p_temp.add_argument("millic", type=int, nargs="?", default=None,
                        metavar="MILLIC",
                        help="temperature in milli-degrees Celsius")
    p_temp.add_argument("--celsius", type=float, default=None, metavar="C",
                        help="temperature in degrees Celsius (convenience)")
    p_temp.set_defaults(func=cmd_temp)

    sub.add_parser("test", help="full self-test of the temperature window"
                   ).set_defaults(func=cmd_test)

    args = ap.parse_args()

    try:
        t = Tres(args.bus, int(args.addr, 0))
    except ImportError:
        sys.exit("smbus2 missing: pip3 install smbus2")
    except (FileNotFoundError, OSError) as e:
        sys.exit("cannot open /dev/i2c{}: {}".format(args.bus, e))

    args.func(t, args)


if __name__ == "__main__":
    main()
