# ADR-0024: Rust-style named operator methods (`@Operator`)

- **Status:** accepted (integrated with [ADR-0018](0018-never-nil-pointers-and-option.md), [ADR-0021](0021-borrow-checking.md), [ADR-0022](0022-race-free-concurrency.md), [ADR-0023](0023-inheritance.md) and [ADR-0032](0032-shared-buffers-and-borrow-engine.md))
- **Status note (2026-10-29, [ADR-0023](0023-inheritance.md)):** an inherited operator method of an untagged parent `P` runs on that parent part inside the child (ADR-0023 decision 16).
- **Date:** 2026-10-28 (updated to Rust-style named `@Operator` methods)
- **Status note (2026-11-11, [ADR-0041](0041-values-are-copied.md)):** decision 5's separate passing rule for operands that can be copied is replaced: like any by-value receiver or parameter, such an operand is the method's own copy, which is not made when the method only reads it; in decision 3, `=` is a copy, not a move; in decision 10, `SetIndex(int, E)` is satisfied by `*[]E` and `*[N]E` and not by a by-value `[]E` (open point 6); the operator table, overload resolution and the other rules stand.
- **Status note (2026-11-11, [ADR-0043](0043-no-star-to-write-through-a-pointer.md)):** in decision 4, arithmetic on a pointer no longer keeps pointer semantics: an operand of type `*T` is read or written as the value it points to, so `p + q`, `-p` and `p += v` reach the operator methods of `T`; `p == q` on two pointers and `*p` keep the pointer's meaning.
- **Plan reference:** replaces the non-goal "no user-defined operator overloading" (spec 1.7); spec section 6.10 (EXP-16 to EXP-20, EXP-22, EXP-25 to EXP-30), TYP-27, RNG-6b, GEN-13, `@Operator` / `@Right` / `@Convert` in MAC-8; tasks P03.24 and P04.23.

## Context

Go has no operator overloading. Vector, matrix, `BigInt` and `Decimal` code must write `a.Add(b).Mul(c)`, and generic algorithms like `Sum[T]` or `Dot[T]` cannot accept both `float64` and `Vec2`.

C++ allows symbol-named `operator+` and even overloads unsafe or control-flow operators (`=`, `&`, `,`, `&&`, `||`, `*x`, `()`, implicit conversions). Rust restricts operator overloading to a clean, predictable set of named methods (`Add`, `Sub`, `Mul`, `Div`, `Rem`, `Neg`, `Not`, `Eq`, `Cmp`, `Index`, …) with zero control-flow or pointer surprises.

Go++ adopts **Rust-style named operator methods** marked with `@Operator`:
- Zero symbol-name or lexer grammar hacks (`MethodName` stays a normal Go `identifier`, and Go's tokens are unchanged).
- Normal Go method names (`Add`, `Sub`, `Mul`, `Eq`, `Cmp`, `Index`, `SetIndex`, `Iter`) that can be called both via operators (`a + b`, `m[r, c]`) and as ordinary methods (`a.Add(b)`, `m.Index(r, c)`).
- `@Operator` is explicit, so an ordinary method like `users.Add(u)` never accidentally enables `users + u`.
- Unsafe and confusing operators (`=`, `.`, `&x`, `,`, implicit conversions, `&&`, `||`, `<-`, unary `*x`, `()`, `++`, `--`) are not overloadable.

## Decision

1. **Syntax (`TYP-27`, `EXP-16`).** An operator method is a normal method with a fixed Rust-style identifier name and the `@Operator` attribute:
   ```go
   @Operator
   func (a Vec2) Add(b Vec2) Vec2 { return Vec2{a.X + b.X, a.Y + b.Y} }
   ```
   Putting `@Operator` on a non-method or on a method whose name is not in the operator table is a compile error listing the valid names. Without `@Operator`, a method named `Add` or `Index` is an ordinary Go method and does not overload any operator.

2. **Allowed operator methods (`EXP-16`).**

   | Expression | `@Operator` Method Name | Signature (after receiver) | Receiver |
   |---|---|---|---|
   | `a + b`, `a - b`, `a * b`, `a / b`, `a % b` | `Add`, `Sub`, `Mul`, `Div`, `Rem` | `(b U) R` | value |
   | `a & b`, `a \| b`, `a ^ b`, `a &^ b`, `a << n`, `a >> n` | `BitAnd`, `BitOr`, `BitXor`, `BitClear`, `Shl`, `Shr` | `(b U) R` | value |
   | `-a`, `!a`, `^a` | `Neg`, `Not`, `BitNot` | `() R` | value |
   | `a += b`, `a -= b`, `a *= b`, `a /= b`, `a %= b` | `AddAssign`, `SubAssign`, `MulAssign`, `DivAssign`, `RemAssign` | `(b U)` (no result) | pointer |
   | `a &= b`, `a \|= b`, `a ^= b`, `a &^= b`, `a <<= n`, `a >>= n` | `BitAndAssign`, `BitOrAssign`, `BitXorAssign`, `BitClearAssign`, `ShlAssign`, `ShrAssign` | `(b U)` (no result) | pointer |
   | `a == b`, `a != b` | `Eq` | `(b U) bool` | value |
   | `a < b`, `a <= b`, `a > b`, `a >= b` | `Cmp` | `(b U) int` | value |
   | `a[i]`, `a[i, j]` | `Index` | `(i I, …) R` | value or pointer |
   | `a[i] = v`, `a[i, j] = v` | `SetIndex` | `(i I, …, v V)` (no result) | pointer |
   | `for x := range a` | `Iter` | `() F` (`iter.Seq[V]` / `iter.Seq2[K, V]` / range func) | value or pointer |
   | `R(a)` *(explicit conversion)* | any method name with `@Convert` | `() R` | value |

3. **Not overloadable (`EXP-29`).** `=` (moves/ownership), `.` (member access), unary `&x` (borrow checker), `,`, implicit conversions, `&&` / `||` (always short-circuit `bool`), `<-` (always a real Go channel), unary `*x` (always a real pointer dereference), `()` (always a real function call or conversion), and `++` / `--` (built-in numbers only).

4. **Overload sets and resolution (`EXP-17`).**
   - Multiple `@Operator` methods on one type may share the same operator method name when their parameter types differ (for example `@Operator func (a Vec2) Mul(b Vec2) Vec2` and `@Operator @Right func (a Vec2) Mul(s float64) Vec2`), matching Rust's `impl Mul<Vec2>` and `impl Mul<f64>`.
   - Resolution matches the exact static parameter type (no implicit conversions); untyped constants try their default type first, then the unique representable overload.
   - Calling `a.Add(b)` or `a.Mul(2.0)` directly as a method resolves across the same overload set (except a non-commutative `@Right` method, which defines the swapped operator `s / a`).
   - Redeclaring an operator meaning that the underlying type already provides (`Add(Meters) Meters` on `type Meters float64`, `Index` on a slice/map/array/string, `Iter` on a slice/map/channel) is a compile error.
   - On a pointer `*T`, `Index`, `SetIndex` and `Iter` look through the pointer (`p[i]`, `p[i] = v`, `for x := range p`), while `==`, `*p` and arithmetic keep pointer semantics.

5. **Ownership and safety (`EXP-18`).** `OWN-C1`, `OWN-C2` and `OWN-C3` operands of pure operators (`Add`..`Shr`, `Neg`, `Not`, `BitNot`, `Eq`, `Cmp`, value-receiver `Index` and `Iter`) and `@Convert` methods are lent read-only without consuming the operand (`PAR-5`). All borrow, never-nil, and `Send`/`Sync` rules apply exactly as for ordinary method calls. `@Operator` methods cannot have own type parameters and cannot be `!` functions (`EXP-30`).

6. **Compound assignment (`EXP-19`).** `x += y` calls `(&x).AddAssign(y)` when `x` is a writable place whose type defines `@Operator AddAssign`, and otherwise desugars to `x = x + y` (`x` evaluated once). Overlapping in-place operands (`m += m`) are rejected by the borrow checker (`OWN-B4`, `OWN-B9`).

7. **Comparisons (`EXP-20`).** `a != b` is `!a.Eq(b)`. `a < b`, `a <= b`, `a > b`, `a >= b` test `a.Cmp(b) < 0`, `<= 0`, `> 0`, `>= 0`. Map keys and `comparable` always use built-in equality, never a user `Eq` method (`gopp vet` warns when a type with `@Operator Eq` is used as a map key or disagrees with `Cmp`).

8. **Indexing and `range` (`EXP-22`, `RNG-6b`).** `a[i, j]` calls `Index(i, j)` (returning a temporary or a borrowed `*T`); `a[i, j] = v` calls `(&a).SetIndex(i, j, v)`. `a[i]` on a custom `Index` type is not a memory place (`&a[i]` and `a[i].f = v` are errors, like Go maps). `for x := range a` calls `a.Iter()` and applies `RNG-6a` while keeping `a` borrowed for the loop (`RNG-3`).

9. **Explicit conversions (`@Convert`, `EXP-25`) and `@Right` (`EXP-26`).**
   - `@Convert` on a value-receiver 0-arg 1-result method (`@Convert func (b BigInt) Float64() float64`) enables explicit `float64(b)` (never implicit).
   - `@Right` alongside `@Operator` on any binary operator method enables `s op a`. For commutative operators (`Add`, `Mul`, `BitAnd`, `BitOr`, `BitXor`, `Eq`) one method serves both `a op s` and `s op a`; for non-commutative ones (`Sub`, `Div`, `Rem`, `BitClear`, `Shl`, `Shr`, `Cmp`) a `@Right` method is written for and serves only `s op a`.

10. **Generics (`GEN-13`).** Constraint interfaces write normal Go method signatures (`interface{ Add(T) T }`, `interface{ Eq(T) bool }`, `interface{ Cmp(T) int; Clone }`). Built-in numeric, string, slice/array (`Index(int) E`, and `SetIndex(int, E)` on `[]E` / `*[N]E`) and comparable (`Eq(T) bool`) types satisfy matching operator method signatures in constraints automatically, so `Sum[T interface{ Add(T) T }]` works for `int`, `float64`, `Vec2`, and `BigInt` alike!

## Example

```go
type Vec2 struct { X, Y float64 }

@Operator
func (a Vec2) Add(b Vec2) Vec2 { return Vec2{a.X + b.X, a.Y + b.Y} }

@Operator
func (a Vec2) Sub(b Vec2) Vec2 { return Vec2{a.X - b.X, a.Y - b.Y} }

@Operator
func (a Vec2) Neg() Vec2 { return Vec2{-a.X, -a.Y} }

@Operator
func (a Vec2) Mul(b Vec2) Vec2 { return Vec2{a.X * b.X, a.Y * b.Y} }

@Operator @Right
func (a Vec2) Mul(s float64) Vec2 { return Vec2{a.X * s, a.Y * s} }

@Operator
func (a Vec2) Eq(b Vec2) bool { return a.X == b.X && a.Y == b.Y }

@Operator
func (a Vec2) Cmp(b Vec2) int {
    return cmp.Compare(a.X*a.X+a.Y*a.Y, b.X*b.X+b.Y*b.Y)
}

func Sum[T interface{ Add(T) T; Clone }](xs []T, zero T) T {
    acc := zero
    for _, x := range xs {
        acc = acc + x
    }
    return acc
}
```
