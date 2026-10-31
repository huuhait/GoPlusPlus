#!/usr/bin/env python3
"""A walk that is cubic in N, for the control of check-linear.py --power 2:
the script must say it is NOT within the quadratic limit. usage: cubic.py N"""
import sys

n = int(sys.argv[1])
items = list(range(n))
found = 0
for u in items:
    for v in items:
        for w in items:
            found += (u ^ v ^ w) == 1
print(found)
