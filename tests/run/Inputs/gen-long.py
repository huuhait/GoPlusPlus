#!/usr/bin/env python3
"""usage: gen-long.py N OUT_GOPP OUT_EXPECTED -- a program printing N characters."""
import sys

n = int(sys.argv[1])
text = "".join(chr(ord("a") + i % 26) for i in range(n))
open(sys.argv[2], "w").write('package main\n\nfunc main() {\n\tprintln("%s")\n}\n' % text)
open(sys.argv[3], "wb").write((text + "\n").encode())
