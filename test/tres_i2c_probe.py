#!/usr/bin/env python3
"""TRES I2C slave probe (run on the Linux/SDM845 host).

Exercises the PY32F030 TRES register file (tres_i2c_slave.c) end to end:

  1. probe the slave and read the whole register window 0x10..0x14
  2. optionally (--commands) send CMD_REPORT_FAIL twice (mutates RETRIES)
     (this path runs: I2C1 ISR -> mailbox -> parser task -> counter)
  3. optionally (--reset / --dfu) issue a real power command, which
     pulses the SDM845 reset line - off by default.
  4. optionally (--params) exercise NVRAM persistence: write a probe value
     into one slot, SAVE_PARAMS (RAM -> FLASH), DEFAULT_PARAMS (reload
     firmware defaults), then restore the WHOLE parameter table from a
     pre-test backup (every slot verified) - off by default.

Protocol (tres_i2c_slave.h):
  write: [regPtr] [data ...]      first byte selects the register pointer,
                                  following bytes are data (auto-advance).
  read : [regPtr] (write) RS [data ...]   register pointer auto-increments.
  cmd  : reg 0x00 <- [0xA5] [cmdId] [param]

Registers:
  0x10 STATUS   bit[3:0]=state bit[4]=host_ready bit[5]=cmd_busy bit[7]=last_err
  0x11 RETRIES  CMD_REPORT_FAIL counter
  0x12 PINS     control-pin snapshot
  0x13 LASTERR  0 none / 1 illegal / 2 bad magic / 3 FLASH / 4 BUSY
  0x14 VERSION  0x03
  0x15 COMPLETED  executed commands modulo 256
  0x16 RX_OVERRUN detected UART gaps modulo 256
  0x30-0x35 machine-state block: BOOTSTATE / BOOTRETRY / TECFLAGS /
                                 AHTSTAT / FANDUTY / TECDUTY
  0x40-0xCF NVRAM parameter window: 36 slots x 4-byte big-endian values

Commands (0x00 <- [0xA5][cmdId][param]):
  1 RESET, 2 ENTER_DFU, 3 REPORT_FAIL, 4 SAVE_PARAMS, 5 DEFAULT_PARAMS, 6 RELOAD_PARAMS

States (STATUS bit[3:0], TRES_State_t):
  0 PWR_UP, 1 BOOT_WAIT_NORMAL, 2 RUNNING, 3 PULSE_RESET_NORMAL,
  4 PULSE_RESET_DFU, 5 BOOT_WAIT_DFU, 6 DFU_ACTIVE

Usage:
  python3 tres_i2c_probe.py [--bus N] [--addr 0x50] [--reset] [--dfu]
                            [--params]
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
TRES_CMD_SAVE_PARAMS = 4
TRES_CMD_DEFAULT_PARAMS = 5

# NVRAM parameter window: slot index -> (name, unit, scale-for-display)
TRES_REG_NV_BASE = 0x40
TRES_REG_NV_LEN = 4
NVM_Q16_SCALE = 65536.0   # Q16 fixed point: raw = round(value * 65536)

# AES challenge-response authentication window (firmware: TRES_AES_ENABLE).
#   Placed above the NVRAM parameter window (0x40..0xCF for 36 slots).
#   0xD0..0xDF  CHALLENGE (W) 16 bytes big-endian
#   0xE0..0xEF  RESPONSE (R)  AES-128-ECB(device_key, challenge)
TRES_REG_CHAL_BASE = 0xD0
TRES_REG_RESP_BASE = 0xE0
TRES_AUTH_LEN = 16
# Must match AES_DEVICE_KEY_BYTES in Core/INC/aes.h (same per-product key).
AES_DEVICE_KEY = bytes.fromhex(
    "2B7E151628AED2A6ABF7158809CF4F3C")
NV_PARAMS = [
    ("PID_KP", "x1", 1 / 65536.0),
    ("PID_KI", "x1", 1 / 65536.0),
    ("PID_KD", "x1", 1 / 65536.0),
    ("TARGET_BASE", "mC", 1),
    ("HOT_OVERTEMP", "mC", 1),
    ("HOT_DANGER", "mC", 1),
    ("CUR_FEEDFORWARD", "mA", 1),
    ("CUR_DANGER", "mA", 1),
    ("FAN_MIN_RPM", "RPM", 1),
    ("CONTROL_PERIOD", "ms", 1),
    ("DAC_MAX_CTRL", "mV", 1),
    ("FAN_TEMP_BASE", "mC", 1),
    ("FAN_TEMP_FULL", "mC", 1),
    ("FAN_DUTY_BASE", "%", 1),
    ("CUR_SHUNT", "uOhm", 1),
    ("CUR_AMP_GAIN", "x1", 1),
    ("CUR_DIV_NUM", "x1", 1),
    ("CUR_DIV_DEN", "x1", 1),
    ("CUR_ADC_VREF", "mV", 1),
    ("TEMP_VALID_MIN", "mC", 1),
    ("TEMP_VALID_MAX", "mC", 1),
    ("CUR_VALID_MAX", "mA", 1),
    ("FAN_FAULT_MIN_DUTY", "%", 1),
    ("ENV_COMP_TEMP", "mC", 1),
    ("SOC_TIGHTEN_TEMP", "mC", 1),
    ("SOC_TIGHTEN_DELTA", "mC", 1),
    ("COLD_SOURCE", "-", 1),   # 0 = TEC_COLD ADC (PA4), 1 = SDM845
    ("DAC_STARTUP", "%", 1),  # first-enable startup point, % of full scale
    ("DAC_MIN_CTRL", "mV", 1), # FB-injection window bottom (FB=VFBTH point)
    ("RAMP_STEP", "%", 1),     # max duty change per control cycle
    ("DEWPOINT_MARGIN", "mC", 1),  # target floor = dew point + this
    ("FF_GAIN", "x1", 1 / 65536.0),  # COMMON-MODE feedforward %/°C (Q16)
    ("HOT_FB_GAIN", "x1", 1 / 65536.0),  # hot foldback: target lift per hot excess (Q16)
    ("HOT_FB_MAX", "mC", 1),   # hot foldback: max cold-target lift
    ("FF_ENABLE", "-", 1),     # feedforward master switch: 0 = PID only
    ("EN_OFF_PCT", "%", 1),    # anti-hiccup deadband: duty below this drops TEC_VEN
]

# Slot name -> index, case-insensitive (e.g. "target_base", "TARGET_BASE").
NV_SLOT_BY_NAME = {name.lower(): i for i, (name, _, _) in enumerate(NV_PARAMS)}

# Slots whose value is a SIGNED int32 carried in the unsigned 32-bit window
# (two's complement on the wire). Every milli-degree temperature can be
# negative - TEMP_VALID_MIN defaults to -20000 - so a raw read must be
# interpreted as signed before it is shown to the user or compared.
# The firmware stores uint32 and casts back to int32; there is no sign bit in
# the protocol, so this list is the contract. Keep it in step with the unit
# comments in Core/INC/nv_params.h.
NV_SIGNED_SLOTS = frozenset((
    "TARGET_BASE",
    "TEMP_VALID_MIN",
    "TEMP_VALID_MAX",
))


def nv_raw_to_signed(raw):
    """Reinterpret an unsigned 32-bit slot value as a signed int32."""
    raw &= 0xFFFFFFFF
    return raw - 0x100000000 if raw >= 0x80000000 else raw


def nv_slot_index(s):
    """Resolve a --set slot argument to an index: name (case-insensitive) or
    a decimal/hex number. Returns the index or -1 if unknown."""
    key = s.strip().lower()
    if key in NV_SLOT_BY_NAME:
        return NV_SLOT_BY_NAME[key]
    try:
        i = int(s, 0)
        if 0 <= i < len(NV_PARAMS):
            return i
    except (ValueError, OverflowError):
        pass
    return -1


def nv_value_to_raw(text, scale):
    """Convert a --set VALUE string to the raw uint32 stored in the slot.

    For Q16 slots (scale < 1, i.e. PID_KP/KI/KD): a decimal literal such as
    "2.0" is treated as the float value and converted with
    raw = round(value * 65536); a plain integer literal is taken as the raw
    Q16 code already (e.g. "131072"). For all other slots the value is the
    raw code itself (decimal or 0x-hex). Returns -1 on invalid input.
    """
    s = text.strip()
    try:
        if scale < 1 and not s.lower().lstrip('+-').startswith(('0x', '0b', '0o')) and ('.' in s or 'e' in s.lower()):
            return int(round(float(s) * NVM_Q16_SCALE))
        return int(s, 0)
    except (ValueError, OverflowError):
        return -1

TRES_ERR_NAMES = {0: "NONE", 1: "ILLEGAL", 2: "MAGIC", 3: "FLASH", 4: "BUSY"}
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
    (0x20, "DEWPOINT"),
    (0x40, "DAC_FAULT"),
    (0x80, "SENSOR"),
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
        """Single-writer protocol v3: wait for this command's completion."""
        before = self.read_reg(0x15)[0]
        if self.status() & 0x20:
            raise RuntimeError("MCU command mailbox busy; command not sent")
        self.bus.write_i2c_block_data(self.addr, TRES_REG_CMD, [magic, cmd, param])
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            status = self.status()
            done = self.read_reg(0x15)[0]
            if done == ((before + 1) & 0xff) and not (status & 0x20):
                error = self.read_reg(TRES_REG_LASTERR)[0]
                if error:
                    raise RuntimeError("command failed: " + TRES_ERR_NAMES.get(error, str(error)))
                return
            time.sleep(0.01)
        raise TimeoutError("MCU did not acknowledge command completion")

    def read_slot(self, slot):
        """Read one NVRAM slot as a big-endian uint32."""
        raw = self.read_reg(TRES_REG_NV_BASE + slot * TRES_REG_NV_LEN,
                            TRES_REG_NV_LEN)
        return int.from_bytes(raw, "big")

    def write_slot(self, slot, value):
        """Write one NVRAM slot (4 bytes big-endian, commits on byte 4)."""
        self.bus.write_i2c_block_data(
            self.addr, TRES_REG_NV_BASE + slot * TRES_REG_NV_LEN,
            [(value >> 24) & 0xFF, (value >> 16) & 0xFF,
             (value >> 8) & 0xFF, value & 0xFF])

    def status(self):
        return self.read_reg(TRES_REG_STATUS)[0]


def fmt_status(v):
    state = v & 0x0F
    flags = []
    if v & 0x10:
        flags.append("HOST_READY")
    if v & 0x20:
        flags.append("CMD_BUSY")
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
    description = """\
Probe the TRES I2C1 slave register file on the PY32F030 from Linux.

The tool talks to the TRES MCU over Linux I2C (smbus2) and exercises the
slave with read-only inspection by default. --commands and the optional
write modes below mutate device state:

  [1] probe:    read 0x10..0x14 (STATUS/RETRIES/PINS/LASTERR/VERSION)
  [2] status:   read the machine-state block 0x30..0x35
                (SDM845 boot state, TEC alarms, AHT20 status, fan/TEC duty)
  [3] --commands only: CMD_REPORT_FAIL x2 (changes RETRIES)
  [4] params:   dump the NVRAM parameter window 0x40..0xCF (36 slots x 4B)
  [5] action:   one of --reset / --dfu / --params / --set, or skipped

Protocol (write): first byte selects the register pointer, following bytes
are data (register pointer auto-advances for burst writes). A command is
three bytes at register 0x00: [0xA5][cmdId][param].
"""
    epilog = """\
Modes (mutually exclusive, all off by default):
  (none)       read-only probe: [1]..[4], then [5] skipped.
  --set NAME V write NVRAM slot NAME (case-insensitive, or slot number
               0..35) with value V into the RAM cache, then read it back.
               For the Q16 gain slots PID_KP/PID_KI/PID_KD a decimal value
               is converted automatically (e.g. 2.0 -> raw 131072); a plain
               integer is taken as the raw Q16 code. Add --save to also
               persist to FLASH.
  --reset      issue CMD_RESET (pulses the SDM845 reset line). Allowed only
               in RUNNING or DFU_ACTIVE; rejected otherwise.
  --dfu        issue CMD_ENTER_DFU (reboots into DFU/USB). Allowed only in
               RUNNING; rejected otherwise.
  --params     NVRAM persistence round-trip: flip SOC_TIGHTEN_DELTA by +1,
               SAVE_PARAMS (RAM -> FLASH), DEFAULT_PARAMS (reload firmware
               defaults), then restore the WHOLE table from a pre-test backup
               and verify every slot matches. Writes FLASH once per
               SAVE_PARAMS (page erase+program, ~10k cycles).
  --auth       AES challenge-response anti-clone check: write 16 random
               bytes to the CHALLENGE window (0xD0), read the RESPONSE
               window (0xE0) and verify AES-128-ECB(device_key, challenge)
               matches. Requires firmware built with TRES_AES_ENABLE=1 and
               the host cryptography library (pip3 install cryptography).

Commands (register 0x00 <- [0xA5][cmdId][param]):
  1 RESET          power-cycle pulse into normal boot (RUNNING/DFU_ACTIVE)
  2 ENTER_DFU      reset pulse with BOOT0=1, USB DFU (RUNNING)
  3 REPORT_FAIL    count-only failure report (any state)
  4 SAVE_PARAMS    persist the NVRAM RAM cache to FLASH
  5 DEFAULT_PARAMS reload firmware defaults into the NVRAM RAM cache

NVRAM parameter window (0x40..0xCF, 4-byte big-endian per slot):
  slot n lives at 0x40 + 4*n; the 4th byte written commits the value into
  the RAM cache. Persist with SAVE_PARAMS. --set accepts any of these names
  (case-insensitive); Q16 gain slots display as raw/65536:

    PID_KP / PID_KI / PID_KD          PID gains (Q16: --set accepts float
                                       e.g. 2.0 -> raw 131072; displays raw/65536)
    TARGET_BASE (mC)                  base TEC target
    HOT_OVERTEMP / HOT_DANGER (mC)    fan full-speed / TEC-off thresholds
    CUR_FEEDFORWARD / CUR_DANGER (mA) current feedforward knee / overcurrent alarm
    FAN_MIN_RPM (RPM)                 fan fault threshold
    CONTROL_PERIOD (ms)               control task period
    DAC_VREF / DAC_MAX_CTRL (mV)      DAC reference / control-window top
    FAN_TEMP_BASE / FAN_TEMP_FULL (mC) fan ramp start / full-speed hot temp
    FAN_DUTY_BASE (%)                 base airflow while TEC active
    CUR_SHUNT (uOhm) / CUR_AMP_GAIN (x1)
    CUR_DIV_NUM / CUR_DIV_DEN (x1)    current-sense divider
    CUR_ADC_VREF (mV)                 ADC reference for the sense chain
    TEMP_VALID_MIN / TEMP_VALID_MAX (mC)  temperature plausibility window
    CUR_VALID_MAX (mA)                current plausibility window
    FAN_FAULT_MIN_DUTY (%)            fan fault needs duty above this
    ENV_COMP_TEMP (mC)                ambient above this raises the target
    SOC_TIGHTEN_TEMP / SOC_TIGHTEN_DELTA (mC)  SoC compensation
    COLD_SOURCE (-)                   cold-side source: 0=TEC_COLD ADC, 1=SDM845

  Signed slots: the window is 4-byte unsigned, but TARGET_BASE,
  TEMP_VALID_MIN and TEMP_VALID_MAX are SIGNED int32 milli-degC (two's
  complement on the wire). Pass a negative decimal such as -20000 to --set;
  --dump and the probe display them as negative. Every other slot is
  non-negative by construction.

Examples:
  python3 tres_i2c_probe.py --bus 3                      # safe read-only probe
  python3 tres_i2c_probe.py --bus 3 --set TARGET_BASE 45000   # RAM only
  python3 tres_i2c_probe.py --bus 3 --set COLD_SOURCE 1 --save  # SDM845, FLASH
  python3 tres_i2c_probe.py --bus 3 --set PID_KP 2.0 --save       # KP=2.0 (auto Q16)
  python3 tres_i2c_probe.py --bus 3 --set PID_KI 0.1 --save       # KI=0.1 (auto Q16)
  python3 tres_i2c_probe.py --bus 3 --reset              # reset SDM845 (gated)
  python3 tres_i2c_probe.py --bus 3 --dfu                # boot DFU (gated)
  python3 tres_i2c_probe.py --bus 3 --params             # NVRAM SAVE round-trip
  python3 tres_i2c_probe.py --bus 3 --auth               # AES anti-clone check

Exit status:
  0  all exercised steps passed
  1  cannot open the I2C bus, VERSION mismatch, or a step failed
  2  a real command was rejected by the TRES policy (illegal in state)

Prerequisites:
  smbus2 (pip3 install smbus2) and /dev/i2c-<bus> readable by the user.
  The slave must be powered and the VERSION register must read 0x03.
"""
    ap = argparse.ArgumentParser(
        description=description,
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
    mode.add_argument("--params", action="store_true",
                      help="exercise NVRAM SAVE_PARAMS / DEFAULT_PARAMS "
                           "(writes FLASH; backs up and restores the whole "
                           "parameter table afterwards)")
    mode.add_argument("--set", nargs=2, metavar=("NAME", "VALUE"),
                      help="write one NVRAM slot into the RAM cache. NAME is a "
                           "slot name from the list below (case-insensitive, "
                           "e.g. TARGET_BASE) or a slot number 0..35; VALUE "
                           "is decimal or hex. Persist with --save.")
    mode.add_argument("--auth", action="store_true",
                      help="AES challenge-response: write 16 random bytes to "
                           "the CHALLENGE window, read RESPONSE back, and "
                           "verify AES-128-ECB(device_key, challenge) matches "
                           "(requires firmware built with TRES_AES_ENABLE=1)")
    ap.add_argument("--commands", action="store_true", help="mutating test: send REPORT_FAIL twice")
    ap.add_argument("--save", action="store_true",
                    help="with --set: also send SAVE_PARAMS to persist the "
                         "slot to FLASH (page erase+program)")
    args = ap.parse_args()
    if args.save and args.set is None:
        sys.exit("--save requires --set SLOT VALUE")
    if args.set is not None:
        slot = nv_slot_index(args.set[0])
        if slot < 0:
            sys.exit("unknown slot {!r}: use a name from --help "
                     "(e.g. TARGET_BASE) or a number 0..{}".format(
                         args.set[0], len(NV_PARAMS) - 1))
        args.set_slot = slot   # pre-validated, reused in step [5]
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
    if version != 0x03:
        sys.exit("    VERSION mismatch - slave not responding as expected")

    print("    UART receive gaps: {}".format(t.read_reg(0x16)[0]))
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

    if args.commands:
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
    else:
        print("[3] command writes skipped (enable with --commands)")

    print("[4] NVRAM parameter window: read 0x40..0x{:02X}".format(
        TRES_REG_NV_BASE + TRES_REG_NV_LEN * len(NV_PARAMS) - 1))
    raw = t.read_reg(TRES_REG_NV_BASE, TRES_REG_NV_LEN * len(NV_PARAMS))
    for i, (name, unit, scale) in enumerate(NV_PARAMS):
        val = int.from_bytes(raw[i * 4:(i + 1) * 4], "big")
        if name in NV_SIGNED_SLOTS:
            val = nv_raw_to_signed(val)   # signed int32 carried in a uint32 slot
        disp = (val * scale) if scale < 1 else val
        print("    [{:2}] {:<16} = {:<12} {}".format(
            i, name, int(disp) if scale >= 1 else disp, unit))
    print("    (Q16 slots PID_KP/KI/KD are stored as int(raw)/65536)")
    print("    (signed mC slots: {})".format(", ".join(sorted(NV_SIGNED_SLOTS))))

    if args.params:
        # NVRAM persistence round-trip: pick a slot that is safe to disturb,
        # write a probe value, SAVE_PARAMS (RAM -> FLASH), read back, then
        # DEFAULT_PARAMS (reload firmware defaults) and restore.
        #
        # SAVE_PARAMS and DEFAULT_PARAMS act on the WHOLE table, so the
        # entire parameter window is backed up before anything is sent and
        # every slot is restored and verified afterwards. Restoring only the
        # probe slot (the old behavior) re-flashed firmware defaults over
        # every other calibrated value - current-sense chain, PID gains, the
        # DAC FB-injection window, thresholds - a permanent, silent loss.
        # Uses slot 25 (SOC_TIGHTEN_DELTA); +1 disturbs nothing.
        slot = 25
        name, unit, _scale = NV_PARAMS[slot]
        print("[5] NVRAM persistence: slot {} ({}) via SAVE/DEFAULT_PARAMS".
              format(slot, name))
        backup = [t.read_slot(i) for i in range(len(NV_PARAMS))]
        orig = backup[slot]
        probe = orig + 1 if orig != 0 else 1
        print("    backed up all {} slots; original {} = {}".format(
            len(NV_PARAMS), name, orig))

        try:
            t.write_slot(slot, probe)
            t.write_cmd(TRES_CMD_MAGIC, TRES_CMD_SAVE_PARAMS)
            time.sleep(0.2)   # parser task persists RAM -> FLASH
            t.write_cmd(TRES_CMD_MAGIC, 6)  # RELOAD_PARAMS: read CRC-validated FLASH
            saved = t.read_slot(slot)
            print("    after SAVE_PARAMS: {} = {} (probe {})".format(
                name, saved, probe))
            if saved != probe:
                sys.exit("    FAIL: SAVE_PARAMS did not persist the value")

            t.write_cmd(TRES_CMD_MAGIC, TRES_CMD_DEFAULT_PARAMS)
            time.sleep(0.2)   # parser task reloads firmware defaults into RAM
            defaulted = t.read_slot(slot)
            print("    after DEFAULT_PARAMS: {} = {}".format(name, defaulted))
            if defaulted == probe:
                sys.exit("    FAIL: DEFAULT_PARAMS did not reload defaults")
        finally:
            # Whole-table restore + one SAVE_PARAMS. The finally block runs
            # even when a FAIL exit or Ctrl-C interrupts the steps above, so
            # an aborted run cannot leave probe/default values in FLASH.
            # (A bus that dies mid-restore fails the restore itself - the
            # one failure mode the host cannot recover from.)
            print("    restoring all {} slots from the backup...".format(
                len(NV_PARAMS)))
            for i, val in enumerate(backup):
                t.write_slot(i, val)
            t.write_cmd(TRES_CMD_MAGIC, TRES_CMD_SAVE_PARAMS)
            time.sleep(0.2)

        # Acceptance: EVERY slot must match the pre-test backup, not just
        # the probe slot. Reload validates FLASH before reading its RAM copy.
        t.write_cmd(TRES_CMD_MAGIC, 6)
        now = [t.read_slot(i) for i in range(len(NV_PARAMS))]
        mismatch = [i for i in range(len(NV_PARAMS)) if now[i] != backup[i]]
        if mismatch:
            sys.exit("    FAIL: post-test mismatch: " + ", ".join(
                "{}[{}] now {:#x} != original {:#x}".format(
                    NV_PARAMS[i][0], i, now[i], backup[i])
                for i in mismatch))
        print("    restored: all {} slots match the pre-test values".format(
            len(NV_PARAMS)))
        print("    OK: SAVE_PARAMS and DEFAULT_PARAMS both work")
    elif args.set:
        slot = args.set_slot
        name, unit, scale = NV_PARAMS[slot]
        value = nv_value_to_raw(args.set[1], scale)
        if value < 0:
            sys.exit("    FAIL: value {!r} is not a valid number for {}".format(
                args.set[1], name))
        if not (0 <= value <= 0xFFFFFFFF):
            sys.exit("    FAIL: raw value {} out of range (0..0xFFFFFFFF)".format(
                value))
        # display: Q16 slots show the float equivalent alongside the raw code
        if scale < 1:
            print("[5] set {} = {} (Q16 raw {:#x}){}".format(
                name, value * scale, value, " " + unit))
        else:
            print("[5] set {} = {} ({:#x}){}".format(
                name, value, value, " " + unit))

        t.write_slot(slot, value)
        readback = t.read_slot(slot)
        if readback != value:
            sys.exit("    FAIL: write did not land in the RAM cache "
                     "(read {} != {})".format(readback, value))
        if scale < 1:
            print("    written to RAM cache, read back {} "
                  "(= {})".format(readback, readback * scale))
        else:
            print("    written to RAM cache, read back {} ({:#x})".format(
                readback, readback))

        if args.save:
            t.write_cmd(TRES_CMD_MAGIC, TRES_CMD_SAVE_PARAMS)
            time.sleep(0.2)
            t.write_cmd(TRES_CMD_MAGIC, 6)
            if t.read_slot(slot) != value:
                sys.exit("FLASH reload mismatch")
            print("    FLASH save acknowledged and reloaded: {}".format(name))
        else:
            print("    RAM cache only - send --save to persist to FLASH "
                  "(or CMD_SAVE_PARAMS manually)")
    elif args.auth:
        # AES challenge-response: write 16 random bytes to CHALLENGE, read
        # RESPONSE, verify AES-128-ECB(device_key, challenge) matches.
        from cryptography.hazmat.primitives.ciphers import (
            Cipher, algorithms, modes)
        from cryptography.hazmat.backends import default_backend
        import secrets

        chal = secrets.token_bytes(TRES_AUTH_LEN)
        print("[5] AES challenge-response: write 16 B to 0x{:02X}, "
              "read from 0x{:02X}".format(TRES_REG_CHAL_BASE, TRES_REG_RESP_BASE))
        t.bus.write_i2c_block_data(t.addr, TRES_REG_CHAL_BASE, list(chal))
        resp = t.read_reg(TRES_REG_RESP_BASE, TRES_AUTH_LEN)

        enc = Cipher(algorithms.AES(AES_DEVICE_KEY), modes.ECB(),
                     backend=default_backend()).encryptor()
        expect = enc.update(chal) + enc.finalize()
        print("    challenge: {}".format(chal.hex().upper()))
        print("    response : {}".format(bytes(resp).hex().upper()))
        print("    expected : {}".format(expect.hex().upper()))
        if bytes(resp) != expect:
            sys.exit("    FAIL: response does not match AES(device_key, "
                     "challenge) - wrong key, or firmware without "
                     "TRES_AES_ENABLE")
        print("    OK: device authenticated (AES-128-ECB matches)")
    elif args.reset or args.dfu:
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
        print("[5] real command 0x{:02X} -> then watch STATUS".format(cmd))
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
        print("[5] skipped real power / NVRAM commands "
              "(pass --reset / --dfu / --params to exercise them)")

    print("OK")

if __name__ == "__main__":
    main()
