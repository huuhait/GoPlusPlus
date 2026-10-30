#!/usr/bin/env python3
"""Run a program and print its standard output, for FileCheck.

usage: run-output.py [--real] --src PATH...

The fast runner (runlib.py) compiles and runs the sources; --real builds a
real binary. The status is 0 only if the program exited with status 0.
Standard error goes to this script's standard error.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runlib  # noqa: E402

opts, _ = runlib.split_args(sys.argv[1:])
res = runlib.run_sources(opts["src"], real=opts["real"], timeout=20)
if res.failure():
    sys.exit(res.failure())
sys.stdout.buffer.write(res.stdout)
sys.stdout.buffer.flush()
sys.stderr.buffer.write(res.stderr)
sys.exit(res.returncode)
