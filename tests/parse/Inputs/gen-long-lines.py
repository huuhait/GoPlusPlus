#!/usr/bin/env python3
"""Prints a file with two very long lines, each with one error far from the
start (used by diag-long-line.test)."""
print("package p")
print("var a = " + "x + " * 30 + ")" + " + y" * 300)
print("var c = " + "q + " * 100 + ";")
