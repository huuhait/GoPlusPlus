# ADR-0009: Constant floats are exact rationals on `num_bigint::BigInt`, with a 512-bit binary float for large values

- **Status:** accepted
- **Date:** 2026-11-11
- **Plan reference:** P03.02, D3.2 (constant arithmetic), [ADR-0001](0001-implementation-language.md)

## Context

The Go spec says untyped numeric constants are exact, and a compiler may
approximate: integers need at least 256 bits, floats a mantissa of at least
256 bits and an exponent of at least 16 bits. Go's `go/constant` uses
`math/big`: exact rationals (`big.Rat`) while numerator and denominator stay
small, and a 512-bit `big.Float` otherwise. Real Go code depends on exact
decimal arithmetic: `const eps = 0.3 - 0.1 - 0.2` is exactly `0`, and
`0.1 + 0.2 == 0.3` is `true` for untyped constants.

`gopp` uses `num-bigint = "0.5.1"` (`BigInt` / `BigUint`, 100% safe Rust) as the
arbitrary-precision integer backing both untyped integer constants and `Real`.

## Decision

- Untyped integers are signed `num_bigint::BigInt`, as wide as the value needs.
- Untyped floats are `Real` (`crates/gopp-sema/src/constant.rs`): an exact
  rational `num / den` of two `BigInt`s (`den > 0`), reduced by their Euclidean
  `gcd`, while both fit in **4096 bits** (`go/constant`'s limit). Past 4096
  bits, and for literals whose decimal exponent exceeds 1200, the value
  transitions to `BigFloat`: a sign, a `BigUint` mantissa of 512 bits, and an
  `i64` binary exponent, rounded to nearest-even. Operations on two rationals
  are exact; operations involving a `BigFloat` round at 512 bits.
- Complex constants are pairs of `Real`s (`{re, im}`).
- Rounding to `float32` and `float64` (typed constants, `representable`, and
  exact C23 hexadecimal float emission `0x1.<hex>p+N` in `gopp-codegen-c`) is
  performed in a single step from the exact value to the target IEEE-754
  precision, including subnormals, with no double rounding through `float64`.

## Consequences

- Untyped float constants match Go's exact arithmetic (`0.3 - 0.1 - 0.2 == 0`,
  `1.0 / 3.0 * 3.0 == 1.0`, `1e-400 * 1e400 == 1.0`, and single-step IEEE
  rounding).
- Constant folding stays 100% pure Rust inside `crates/gopp-sema` with zero C
  library dependencies.
- The binary exponent of `BigFloat` is bounded at `2^20`, keeping pathological
  power-of-ten constant chains fast and bounded in memory.
