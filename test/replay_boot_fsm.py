#!/usr/bin/env python3
"""Run actual boot callbacks, replacing the outdated Python retry model.
Usage: python test/replay_boot_fsm.py --cc /path/to/zig cc
"""
import sys
from run_boundary_tests import main

if __name__ == '__main__':
    main(['--only', 'boot_timeout', *sys.argv[1:]])
