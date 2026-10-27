#!/usr/bin/env python3
"""Prints a Go++ file whose one expression is very tall: `levels` nested
parentheses, each around a chain of `chain` terms. Neither the nesting limit
nor a single chain is exceeded for the default options, but the tree is
levels * chain tall (the review finding R1: gopp parse --dump-ast crashed)."""
import sys

levels, chain = int(sys.argv[1]), int(sys.argv[2])
e = "a"
for _ in range(levels):
    e = "(" + e + "+a" * chain + ")"
sys.stdout.write("package p\nvar x = " + e + "\nvar ok = 1\n")
