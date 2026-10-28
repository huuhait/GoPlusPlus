# ADR-0009: Constant floats are exact rationals on llvm::APInt, with a 512-bit binary float for large values

- **Status:** accepted
- **Date:** 2026-10-27
- **Plan reference:** P03.02, D3.2 (constant arithmetic; the big-float spike)

## Context

The Go spec says untyped numeric constants are exact, and a compiler may
approximate: integers need at least 256 bits, floats a mantissa of at least
256 bits and an exponent of at least 16 bits. Go's `go/constant` uses
`math/big`: exact rationals (`big.Rat`) while numerator and denominator stay
small, and a 512-bit `big.Float` otherwise. Real Go code depends on the exact
part: `const eps = 0.3 - 0.1 - 0.2` is exactly `0`, and `0.1+0.2 == 0.3` is
true for untyped constants.

D3.2 named libbf as the candidate. C++ has no standard big number type; the
libraries we already link are LLVM (`APInt`, `APFloat`). `APFloat` only has
fixed IEEE formats (up to quad, 113 bits), which is too small.

## The spike

Three implementations were written and compared (P03.01-P03.07, three coders):

| Representation | `0.3-0.1-0.2 == 0` | `1.0/3.0*3.0 == 1.0` | `1e-400*1e400 == 1` |
|---|---|---|---|
| 512-bit binary float only | **false** (1.86e-155) | true (rounding luck) | true |
| Exact rationals, rounded to 512 bits past 1024 bits | true | true | true |
| Exact rationals, rounded to 512 bits past 4096 bits (go/constant's limit) | true | true | true |

(Go: true in all three columns.)

The binary-float-only version differs from Go on ordinary programs, which D3.1
asks us to avoid. Exact rationals cost little: constant expressions are small,
and the size limit bounds the cost of chains such as `x/3*3` or `1e100000`.

## Decision

- Integers are signed `llvm::APInt`, as wide as the value needs.
- Floats are `Real` (compiler/include/gopp/Sema/Constant.h): an exact
  rational `Num/Den` of two `APInt`s, reduced by their gcd, while both fit in
  4096 bits (go/constant's limit). Past that, and for literals whose decimal
  exponent is above 1200, the value is a `BigFloat`: a sign, an `APInt`
  mantissa of exactly 512 bits and an `int64_t` binary exponent, rounded to
  nearest-even. An operation on two rationals is exact; an operation with a
  `BigFloat` rounds at 512 bits.
- Complex numbers are two `Real`s.
- Rounding to `float32` and `float64` (typed constants, `representable`) is
  done once, from the exact value, to the target precision, subnormals
  included (`BigFloat::toIEEE`): no double rounding through `float64`.
- No library is vendored: no new dependency, no license question (LLVM is
  Apache-2.0 with LLVM exception).

## Consequences

- Untyped float constants give Go's results where Go is exact
  (tests/unit/Sema/ConstantTest.cpp, ported from go/constant's
  `value_test.go`, plus the cases above).
- Code to maintain (compiler/lib/Sema/Constant.cpp): rationals, the 512-bit
  float, decimal parsing and printing, IEEE rounding.
- The binary exponent of a `BigFloat` is limited to 2^20 (Go: 2^31), so a
  literal like `1e100000000` is "malformed" here while Go accepts it. One
  conformance file (`fixedbugs/issue20583.go`) shows this; raising the limit
  needs a cheaper decimal printer than the current power-of-ten scaling.

## Alternatives

- **Vendor libbf** (Fabrice Bellard, MIT): binary floats only, so it has the
  same `0.3-0.1-0.2` problem, plus a new third-party dependency, its own
  build rules, and the glue for Go's decimal parsing and rounding. Rejected.
- **GMP/MPFR**: LGPL, and a system dependency in the Nix flake. Rejected.
- **A 512-bit binary float only**: simpler, but not exact where Go is (see the
  spike). Rejected.
- **long double / `APFloat` quad**: too few bits (64 or 113), fails the
  spec's 256-bit minimum.
