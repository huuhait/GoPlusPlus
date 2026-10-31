#!/usr/bin/env python3
"""Run a gdb script on a linux/arm64 program under user-mode QEMU.

usage: gdb-qemu.py SYSROOT EXE SCRIPT

QEMU runs EXE (-L SYSROOT) with its gdb stub on a Unix socket of a fresh
directory (no port to collide with another test), and gdb attaches to it
with SCRIPT, whose `run` becomes `continue` (the program has started). The
output is gdb's, then the program's. Every wait has a time limit.
"""
import os
import subprocess
import sys
import tempfile
import time

sysroot, exe, script = sys.argv[1:4]

with tempfile.TemporaryDirectory() as tmp:
    sock = os.path.join(tmp, "gdb.sock")
    cmds = os.path.join(tmp, "script.gdb")
    with open(script) as f, open(cmds, "w") as out:
        out.write("set sysroot %s\ntarget remote %s\n" % (sysroot, sock))
        for line in f:
            out.write("continue\n" if line.strip() == "run" else line)
    qemu = subprocess.Popen(["qemu-aarch64", "-L", sysroot, "-g", sock, exe],
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 20
        while not os.path.exists(sock):
            if qemu.poll() is not None or time.monotonic() > deadline:
                sys.exit("qemu-aarch64 did not open its gdb socket")
            time.sleep(0.05)
        gdb = subprocess.run(["gdb", "-batch", "-nx", "-q", "-x", cmds, exe],
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=60)
        program = qemu.communicate(timeout=30)[0]
    finally:
        if qemu.poll() is None:
            qemu.kill()
            qemu.wait()
    sys.stdout.write(gdb.stdout.decode(errors="replace"))
    sys.stdout.write(program.decode(errors="replace"))
