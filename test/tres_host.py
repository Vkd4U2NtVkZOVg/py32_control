#!/usr/bin/env python3
"""Linux host CLI for TRES protocol v3. Default operation is read-only status."""
import argparse
import json
from pathlib import Path
import struct
import sys
import time

from tres_i2c_probe import (Tres, NV_PARAMS, NV_SIGNED_SLOTS, nv_raw_to_signed,
                            nv_slot_index, nv_value_to_raw, fmt_status)


def check_version(device):
    version = device.read_reg(0x14)[0]
    if version != 3:
        raise RuntimeError('requires firmware interface 0x03; got 0x%02x' % version)


def read_params(device):
    return [device.read_slot(i) for i in range(len(NV_PARAMS))]


def save_verified(device):
    expected = read_params(device)
    device.write_cmd(0xa5, 4)
    device.write_cmd(0xa5, 6)
    if read_params(device) != expected:
        raise RuntimeError('FLASH reload differs from the saved parameter table')


def write_temperature(device, value):
    if not -40000 <= value <= 200000:
        raise ValueError('temperature outside -40000..200000 milli-Celsius')
    device.bus.write_i2c_block_data(device.addr, 0x20, list(struct.pack('>i', value)))
    actual = struct.unpack('>i', bytes(device.read_reg(0x20, 4)))[0]
    if actual != value:
        raise RuntimeError('temperature readback mismatch')


def publish_temperature(device, source, interval, once=False):
    """Read the real sensor each time; exit on failure instead of replaying cache."""
    if not 0.1 <= interval <= 1.0:
        raise ValueError('interval must be 0.1..1.0 seconds (firmware expires at 5 s)')
    while True:
        started = time.monotonic()
        value = int(Path(source).read_text(encoding='ascii').strip(), 10)
        write_temperature(device, value)
        print(json.dumps({'temperature_mc': value}), flush=True)
        if once:
            return
        time.sleep(max(0, interval - (time.monotonic() - started)))


def load_backup(path):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    if not isinstance(data, dict) or data.get('format') != 'tres-params-v1' or data.get('interface') != 3:
        raise ValueError('unsupported backup format/interface')
    values = data.get('raw')
    if not isinstance(values, list) or len(values) != len(NV_PARAMS):
        raise ValueError('backup must contain exactly %d slots' % len(NV_PARAMS))
    if any(type(v) is not int or not 0 <= v <= 0xffffffff for v in values):
        raise ValueError('backup slots must be unsigned 32-bit integers')
    return values


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bus', type=int, default=3)
    p.add_argument('--addr', type=lambda s: int(s, 0), default=0x50)
    sub = p.add_subparsers(dest='action')
    sub.add_parser('status', help='read current state and diagnostics')
    sub.add_parser('params', help='read all parameter slots')
    backup = sub.add_parser('backup', help='export RAM parameter table as JSON')
    backup.add_argument('file')
    restore = sub.add_parser('restore', help='restore table to RAM; optional FLASH save')
    restore.add_argument('file')
    restore.add_argument('--save', action='store_true')
    setting = sub.add_parser('set', help='set one parameter in RAM')
    setting.add_argument('slot')
    setting.add_argument('value')
    setting.add_argument('--save', action='store_true')
    sub.add_parser('save', help='save ALL RAM slots to FLASH and reload to verify')
    sub.add_parser('reload', help='discard RAM changes and reload valid FLASH')
    sub.add_parser('defaults', help='replace RAM table with firmware defaults (no save)')
    sub.add_parser('reset', help='request policy-gated TRES reset')
    sub.add_parser('dfu', help='request policy-gated TRES DFU')
    temp = sub.add_parser('temp', help='write a single temperature sample')
    temp.add_argument('millic', type=int)
    publisher = sub.add_parser('publish-temp', help='publish a real Linux thermal sensor')
    publisher.add_argument('--source', required=True, help='thermal_zone*/temp, units: milli-Celsius')
    publisher.add_argument('--interval', type=float, default=1.0)
    publisher.add_argument('--once', action='store_true')
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    if args.bus < 0 or not 0x08 <= args.addr <= 0x77:
        raise ValueError('invalid bus or 7-bit I2C address')
    # Validate complete inputs before opening the bus or writing any slot.
    values = load_backup(args.file) if args.action == 'restore' else None
    if args.action == 'set':
        slot = nv_slot_index(args.slot)
        if slot < 0:
            raise ValueError('unknown parameter: ' + args.slot)
        value = nv_value_to_raw(args.value, NV_PARAMS[slot][2])
        if not 0 <= value <= 0xffffffff:
            raise ValueError('invalid parameter value')
    device = Tres(args.bus, args.addr)
    try:
        check_version(device)
        if args.action in (None, 'status'):
            status, retries, pins, error, version, completed, gaps = device.read_reg(0x10, 7)
            boot, boot_retries, alarms, aht, fan, tec = device.read_reg(0x30, 6)
            print(json.dumps(dict(state=fmt_status(status), status=status, retries=retries,
                pins=pins, error=error, interface=version, completed=completed, uart_gaps=gaps,
                boot_state=boot, boot_retries=boot_retries, tec_alarms=alarms,
                aht_status=aht if aht < 128 else aht - 256, fan_percent=fan,
                tec_percent=tec), indent=2))
        elif args.action == 'params':
            for i, raw in enumerate(read_params(device)):
                name, unit, scale = NV_PARAMS[i]
                # Signed mC slots are carried as two's complement in the
                # unsigned window: show the value the firmware will read back.
                value = nv_raw_to_signed(raw) if name in NV_SIGNED_SLOTS else raw
                print('%2d %-20s raw=%10d value=%s %s' % (i, name, raw, value * scale, unit))
        elif args.action == 'backup':
            data = dict(format='tres-params-v1', interface=3, raw=read_params(device))
            # Do not silently replace an earlier calibration backup.
            with Path(args.file).open('x', encoding='utf-8') as f:
                json.dump(data, f, indent=2)
                f.write('\n')
            print('RAM parameter table exported; no device changes')
        elif args.action == 'restore':
            for i, value in enumerate(values):
                device.write_slot(i, value)
            if read_params(device) != values:
                raise RuntimeError('RAM restore verification failed')
            if args.save:
                save_verified(device)
            print('restored to ' + ('FLASH (verified)' if args.save else 'RAM only'))
        elif args.action == 'set':
            device.write_slot(slot, value)
            if device.read_slot(slot) != value:
                raise RuntimeError('RAM write verification failed')
            if args.save:
                save_verified(device)
            print('written to ' + ('FLASH (verified)' if args.save else 'RAM only'))
        elif args.action == 'save':
            save_verified(device)
            print('whole parameter table saved and reloaded from FLASH')
        elif args.action in ('reload', 'defaults', 'reset', 'dfu'):
            device.write_cmd(0xa5, dict(reload=6, defaults=5, reset=1, dfu=2)[args.action])
            print('command executed: ' + args.action)
        elif args.action == 'temp':
            write_temperature(device, args.millic)
            print('sample published; expires in 5 seconds without another update')
        elif args.action == 'publish-temp':
            publish_temperature(device, args.source, args.interval, args.once)
    finally:
        device.bus.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('stopped', file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, ImportError) as error:
        print('ERROR: ' + str(error), file=sys.stderr)
        sys.exit(1)
