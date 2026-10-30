"""Stands in for `gopp` in the batch (runner-batch-fallback.test): exits 0
but writes a results file that is not results."""
import sys

with open(sys.argv[sys.argv.index("-o") + 1], "w") as f:
    f.write("this is not a result\n")
