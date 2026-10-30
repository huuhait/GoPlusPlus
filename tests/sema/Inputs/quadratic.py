#!/usr/bin/env python3
"""A walk that is quadratic in N, for the controls of the near-linear scripts
(check-units-linear.py, check-gir-linear.py --control): the script must say it
is NOT near-linear. usage: quadratic.py N"""
import sys

n = int(sys.argv[1])
items = list(range(n))
found = 0
for u in items:
    for v in items:
        found += (u ^ v) == 1
print(found)
