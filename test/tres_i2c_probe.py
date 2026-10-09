#!/usr/bin/env python3
"""TRES I2C slave probe (run on the Linux/SDM845 host).

Exercises the PY32F030 TRES register file (tres_i2c_slave.c) end to end:

  1. probe the slave and read the whole register window 0x10..0x14
  2. send CMD_REPORT_FAIL twice, verify the RETRIES counter increments
     (this path runs: I2C1 ISR -> mailbox -> parser task -> counter)
  3. optionally (--reset / --dfu) issue a real power command, which
     pulses the SDM845 reset line - off by default.

Protocol (tres_i2c_slave.h):
  write: [regPtr] [data ...]      first byte selects the register pointer,
                                  following bytes are data (auto-advance).
  read : [regPtr] (write) RS [data ...]   register pointer auto-increments.
  cmd  : reg 0x00 <- [0xA5] [cmdId] [param]

Registers:
  0x10 STATUS   bit[3:0]=state bit[4]=host_ready bit[7]=last_err
  0x11 RETRIES  CMD_REPORT_FAIL counter
  0x12 PINS     control-pin snapshot
  0x13 LASTERR  0 none / 1 illegal-in-state / 2 bad magic
  0x14 VERSION  0x02
  0x30-0x35 machine-state block: BOOTSTATE / BOOTRETRY / TECFLAGS /
                                 AHTSTAT / FANDUTY / TECDUTY

States (STATUS bit[3:0], TRES_State_t):
  0 PWR_UP, 1 BOOT_WAIT_NORMAL, 2 RUNNING, 3 PULSE_RESET_NORMAL,
  4 PULSE_RESET_DFU, 5 BOOT_WAIT_DFU, 6 DFU_ACTIVE

Usage:
  python3 tres_i2c_probe.py [--bus N] [--addr 0x50] [--reset] [--dfu]
"""

import argparse
import struct
import sys
import time

TRES_REG_CMD = 0x00
TRES_REG_STATUS = 0x10
TRES_REG_RETRIES = 0x11
TRES_REG_PINS = 0x12
TRES_REG_LASTERR = 0x13
TRES_REG_VERSION = 0x14
TRES_REG_BOOTSTATE = 0x30
TRES_REG_BOOTRETRY = 0x31
TRES_REG_TECFLAGS = 0x32
TRES_REG_AHTSTAT = 0x33
TRES_REG_FANDUTY = 0x34
TRES_REG_TECDUTY = 0x35

TRES_CMD_MAGIC = 0xA5
TRES_CMD_RESET = 1
TRES_CMD_ENTER_DFU = 2
TRES_CMD_REPORT_FAIL = 3

TRES_ERR_NAMES = {0: "NONE", 1: "ILLEGAL", 2: "MAGIC"}
TRES_STATE_NAMES = {
    0: "PWR_UP", 1: "BOOT_WAIT_NORMAL", 2: "RUNNING",
    3: "PULSE_RESET_NORMAL", 4: "PULSE_RESET_DFU",
    5: "BOOT_WAIT_DFU", 6: "DFU_ACTIVE",
}
SDM845_STATE_NAMES = {
    0: "THERMAL_PRECHECK", 1: "POWER_ON_WAIT", 2: "PWR_PRESS",
    3: "BOOT_CHECK", 4: "STAGE2_WAIT", 5: "STAGE3_WAIT",
    6: "STAGE4_WAIT", 7: "STAGE5_WAIT", 8: "BOOT_SUCCESS",
    9: "BOOT_RESET", 10: "PMIC_SETTLE", 11: "STRAP_FAIL",
}
TEC_FLAG_NAMES = (
    (0x01, "OVERTEMP"),
    (0x02, "HOT_WARNING"),
    (0x04, "OVERCURRENT"),
    (0x08, "FAN_FAULT"),
    (0x10, "THERMAL_WAIT"),
)


class Tres:
    def __init__(self, bus, addr):
        from smbus2 import SMBus, i2c_msg
        self._SMBus = SMBus
        self._i2c_msg = i2c_msg
        self.bus = SMBus(bus)
        self.addr = addr

    def read_reg(self, reg, n=1):
        """Combined transaction: write regPtr, repeated start, read n bytes."""
        wr = self._i2c_msg.write(self.addr, struct.pack("B", reg))
        rd = self._i2c_msg.read(self.addr, n)
        self.bus.i2c_rdwr(wr, rd)
        return list(rd)

    def write_cmd(self, magic, cmd, param=0):
        """One transaction: regPtr=0x00 then the 3 command bytes."""
        self.bus.write_i2c_block_data(self.addr, TRES_REG_CMD,
                                      [magic, cmd, param])

    def status(self):
        return self.read_reg(TRES_REG_STATUS)[0]


def fmt_status(v):
    state = v & 0x0F
    flags = []
    if v & 0x10:
        flags.append("HOST_READY")
    if v & 0x80:
        flags.append("LAST_ERR")
    return "{}(0x{:X}){}".format(TRES_STATE_NAMES.get(state, "?"), state,
                                 ("/" + ",".join(flags)) if flags else "")


def fmt_pins(v):
    """Decode TRES_Seq_GetPinsSnapshot() bit assignments."""
    names = (
        (0x01, "PWR_EN"),
        (0x02, "RST"),
        (0x04, "BOOT0"),
        (0x08, "VDD33USB"),
        (0x10, "SOM_BOOTED"),
    )
    return ",".join("{}={}".format(name, 1 if v & mask else 0)
                   for mask, name in names)


def main():
    epilog = """
Examples:
  python3 tres_i2c_probe.py --bus 3
      Read the register file and verify REPORT_FAIL handling (safe).
  python3 tres_i2c_probe.py --bus 3 --reset
      Request a normal SDM845 reset. Only valid in RUNNING/DFU_ACTIVE.
  python3 tres_i2c_probe.py --bus 3 --dfu
      Request DFU boot. Only valid in RUNNING.

Exit status is non-zero when the bus cannot be opened, VERSION is wrong,
a command path fails, or a real command is rejected by the TRES policy.
"""
    ap = argparse.ArgumentParser(
        description="Probe the TRES I2C1 slave register file from Linux.",
        epilog=epilog,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bus", type=int, default=3, metavar="N",
                    help="Linux I2C bus number (default: 3)")
    ap.add_argument("--addr", default="0x50", metavar="ADDR",
                    help="7-bit slave address, decimal or hex (default: 0x50)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--reset", action="store_true",
                      help="issue CMD_RESET (pulses SDM845 reset; policy-gated)")
    mode.add_argument("--dfu", action="store_true",
                      help="issue CMD_ENTER_DFU (boots DFU/USB; policy-gated)")
    args = ap.parse_args()
    addr = int(args.addr, 0)

    try:
        t = Tres(args.bus, addr)
    except ImportError:
        sys.exit("smbus2 missing: pip3 install smbus2 "
                 "(or use i2cget/i2cset, see --help trailer)")
    except (FileNotFoundError, OSError) as e:
        sys.exit("cannot open /dev/i2c{}: {}".format(args.bus, e))

    print("[1] probe: read registers 0x10..0x14 (auto-increment)")
    regs = t.read_reg(TRES_REG_STATUS, 5)
    status, retries, pins, lasterr, version = regs
    print("    FSM: {}  STATUS=0x{:02X}".format(
        fmt_status(status), status))
    print("    GPIO: PINS=0x{:02X} [{}]".format(pins, fmt_pins(pins)))
    print("    RETRIES={}  LASTERR={}  VERSION=0x{:02X}".format(
        retries, TRES_ERR_NAMES.get(lasterr, "?"), version))
    if version != 0x02:
        sys.exit("    VERSION mismatch - slave not responding as expected")

    print("[2] machine-state block: read 0x30..0x35 (auto-increment)")
    bootstate, bootretry, tecflags, ahtstat, fanduty, tecduty = \
        t.read_reg(TRES_REG_BOOTSTATE, 6)
    print("    SDM845: {}({}) retry={}  FAN={}%  TEC={}%".format(
        SDM845_STATE_NAMES.get(bootstate, "?"), bootstate, bootretry,
        fanduty, tecduty))
    flags = "|".join(name for mask, name in TEC_FLAG_NAMES
                     if tecflags & mask) or "NONE"
    print("    TEC alarms: {}(0x{:02X})  AHT20 status: {}".format(
        flags, tecflags, ahtstat))

    print("[3] command path: CMD_REPORT_FAIL x2, RETRIES must go up by 2")
    before = t.read_reg(TRES_REG_RETRIES)[0]
    for _ in range(2):
        t.write_cmd(TRES_CMD_MAGIC, TRES_CMD_REPORT_FAIL)
        time.sleep(0.2)  # parser task is woken by the ISR
    after = t.read_reg(TRES_REG_RETRIES)[0]
    print("    RETRIES: {} -> {}".format(before, after))
    if after != (before + 2) & 0xFF:
        sys.exit("    FAIL: counter did not advance; "
                 "check STATUS bit7/LASTERR")

    if args.reset or args.dfu:
        cmd = TRES_CMD_RESET if args.reset else TRES_CMD_ENTER_DFU
        state = status & 0x0F
        allowed = ((cmd == TRES_CMD_ENTER_DFU and state == 2) or
                   (cmd == TRES_CMD_RESET and state in (2, 6)))
        if not allowed:
            action = "ENTER_DFU" if cmd == TRES_CMD_ENTER_DFU else "RESET"
            print("    FAIL: {} is illegal in current state {}".format(
                action, TRES_STATE_NAMES.get(state, "UNKNOWN")),
                  file=sys.stderr)
            print("    DFU requires RUNNING; RESET requires RUNNING or DFU_ACTIVE",
                  file=sys.stderr)
            sys.exit(2)

        err_before = t.read_reg(TRES_REG_LASTERR)[0]
        print("[4] real command 0x{:02X} -> then watch STATUS".format(cmd))
        t.write_cmd(TRES_CMD_MAGIC, cmd)
        last_status = status
        for _ in range(10):
            time.sleep(0.5)
            last_status = t.status()
            live_pins = t.read_reg(TRES_REG_PINS)[0]
            print("    FSM: {}  GPIO: PINS=0x{:02X} [{}]".format(
                fmt_status(last_status), live_pins, fmt_pins(live_pins)))

        err_after = t.read_reg(TRES_REG_LASTERR)[0]
        if err_after != err_before:
            print("    FAIL: command rejected, LASTERR {} -> {} ({})".format(
                err_before, err_after, TRES_ERR_NAMES.get(err_after, "?")),
                  file=sys.stderr)
            sys.exit(2)
        if last_status & 0x80:
            print("    FAIL: STATUS bit7 indicates a command error",
                  file=sys.stderr)
            sys.exit(2)
    else:
        print("[4] skipped real power commands "
              "(pass --reset / --dfu to exercise them)")

    print("OK")

if __name__ == "__main__":
    main()
