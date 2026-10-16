"""Compile/run the fan.c soft-ramp host test with real production code.

Usage: python test/run_fan_ramp_test.py --cc gcc
Add --syntax-only to only syntax-check (cross compilers cannot run).
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--cc', nargs='+', default=['gcc'])
parser.add_argument('--syntax-only', action='store_true')
args = parser.parse_args()

fan = (root / 'Core/SRC/fan.c').read_text(encoding='utf-8')
fan = re.sub(r'^#include.*$', '', fan, flags=re.M)
test = (root / 'test/fan_ramp_test.c').read_text(encoding='utf-8')

# fan.c's include of qport_py32.h is stripped; the test file declares
# QOS_Tick_Get and the runner provides the body between the two fragments.
impl = '#include "fan_impl.inc"\n' + fan
source = test.replace('#include "fan_impl.inc"', fan)

with tempfile.TemporaryDirectory(dir=root) as directory:
    src = Path(directory) / 'fan_ramp_test.c'
    src.write_text(source, encoding='utf-8')
    command = args.cc + ['-std=c99', '-Wall', '-Wextra', str(src)]
    if args.syntax_only:
        command += ['-fsyntax-only']
    else:
        exe = Path(directory) / ('fan_ramp_test.exe' if args.cc[0] != 'arm-none-eabi-gcc'
                                 else 'fan_ramp_test.elf')
        command += ['-o', str(exe)]
    subprocess.run(command, check=True)
    if not args.syntax_only:
        subprocess.run([str(exe)], check=True)
    print('fan ramp host test passed', flush=True)
