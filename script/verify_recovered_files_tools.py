#!/usr/bin/env python3
"""Compatibility entry point; keep the adjacent recovery_verify package."""
from recovery_verify import __version__ as PROGRAM_VERSION
from recovery_verify.cli import main

if __name__ == '__main__':
    raise SystemExit(main())
