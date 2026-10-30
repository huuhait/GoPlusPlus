# Writes a package whose variables initialize each other in a chain of N links:
# `var v0 = v1 + 1` ... `var v<N-1> = 1`.
import sys

n = int(sys.argv[1])
out = ["package main", ""]
out += [f"var v{i} = v{i + 1} + 1" for i in range(n - 1)]
out += [f"var v{n - 1} = 1", "", "func main() { println(v0) }"]
print("\n".join(out))
