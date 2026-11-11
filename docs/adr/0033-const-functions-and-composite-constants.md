# ADR-0033: `@Const` functions and composite `const` declarations

- **Status:** accepted
- **Date:** 2026-11-11
- **Status note (2026-11-11, [ADR-0041](0041-values-are-copied.md)):** in decision 1, "mutable shared backing" no longer describes `[]T` and `map[K]V`: each has one owner and its own heap buffer; they still cannot be `const`, and every rule here stands.
- **Plan reference:** spec section 6.11 (`EXP-31` to `EXP-34`), `@Const` in `MAC-8`, `01-overview.md`, `attributes.md`.

## Context

In Go, `const` declarations only accept `bool`, numeric types, and `string`, and constant expressions cannot call user functions. As a result:
- Pure value structs (`const Origin = Vec2{0, 0}`) and fixed lookup tables (`const CRCTable = makeCRCTable()`) must be declared as mutable package-level `var` globals initialized at startup (`init()`), adding startup cost and exposing global mutable state to data races.
- Compile-time hashes, bitmasks, and lookup tables cannot be computed by normal functions in the same package or used in constant positions (`[N]T` array lengths, `switch` cases, `@Align`, `@If`).

Go++ already has `const` declarations (both package-level and function-local) and built-in `@` attributes (`@Operator`, `@Inline`, `@Virtual`). Marking compile-time-capable functions and methods with `@Const` — while extending `const` declarations to hold structs, fixed arrays, and `Option`s of constant-capable types — gives Go++ the power of C++ `constexpr` and Rust `const fn` with **zero new keywords**.

## Decision

1. **Composite `const` declarations (`EXP-31`).**
   - `const` declarations (at package scope and inside functions) accept any **constant-capable type**:
     - `bool`, numeric types (`int`..`complex128`), `string` (typed or untyped, as in Go);
     - fixed-size arrays `[N]T` whose element type `T` is constant-capable;
     - structs (including untagged structs with parents and without `drop`) whose fields are all constant-capable;
     - `Option[T]` whose payload type `T` is constant-capable (`nil` or a constant `T`).
   - Types with runtime identity, mutable shared backing, or destructors (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`, `[]T`, `map[K]V`, `chan T`, `func(...)`, `interface`, `unsafe.Pointer`, `@Pinned`, types with `drop`, tagged types of a hierarchy, imported C++ classes) cannot be `const`.
   - Every `const` value is **immutable and non-addressable**: `Origin.X = 1`, `CRCTable[0] = 1`, `&Origin`, and `&CRCTable[0]` are compile errors.
   - Field access (`Origin.X`), array/string indexing (`CRCTable[i]`), `len`, `cap`, `Option.IsNil()` / `Option.Get()`, and `@Const` value-receiver methods/operators work directly on composite constants (pointer-receiver methods/operators require an addressable `var`); when all operands are constant expressions, the result is itself a constant expression.
   - Materialized composite constants live in read-only static storage (`.rodata`), deduplicated across the binary, with zero `init()` startup code and zero data races (`Sync`).

2. **`@Const` functions and methods (`EXP-32`).**
   - `@Const` is a built-in attribute (`MAC-8`) valid on top-level functions, value- or pointer-receiver methods, `@Operator` / `@Convert` methods, and generic functions/methods.
   - `@Const` is a compile error on `@Extern` functions, `@Virtual` / `@Abstract` / `@Override` methods, `drop` methods, and non-function declarations.
   - **Dual execution:**
     - In any constant context (`const` initializer at package or local scope, array length `[N]T`, `@Align(N)`, `@If(cond)`, macro `lit` arguments), calling a `@Const` function or method with constant arguments evaluates it at **compile time**.
     - In ordinary runtime expressions (`t := makeCRCTable()`, `h := hash(input)`), a `@Const` function compiles and runs as a normal Go++ function with zero overhead. Writing a local `const mask = buildMask(12)` inside a runtime function forces compile-time evaluation without needing a separate `const(expr)` form.
     - Calling a function or method **without** `@Const` inside a constant expression is a compile error.

3. **Checked once at the `@Const` declaration (`EXP-33`).**
   - A `@Const` body is purity-checked once when declared (not lazily per call site):
     - **Allowed:** local variables (`var`, `:=`), assignments, pointers `&local` to local places within the `@Const` call, arithmetic/bitwise/comparison/boolean/string operations, composite literals (structs, arrays, `Option`, and local `[]T` slices used during the computation), `if`, `switch`, `for` / `range`, `return`, built-ins (`len`, `cap`, `min`, `max`, `make([]T, ...)` for constant-capable `T`, `append`, `copy`, `clear`, `clone`, `complex`, `real`, `imag`), predeclared `Option.IsNil()` / `Option.Get()`, auto-generated `Clone()` when every field's `Clone()` is `@Const`, reading `const` declarations, calling other `@Const` functions/methods (same package or imported, including recursion and `@Const` generic constraint methods), `!` propagation over constant-capable value error types whose `Error() string` method is `@Const`, and `panic(...)`.
     - **Disallowed (compile error at the `@Const` declaration):** reading or writing any package-level `var`, calling non-`@Const` or `@Extern` / C / C++ functions, dynamic interface or `func`-value calls, `NewUnique` / `NewShared` / `NewWeak`, `map`, `chan`, `go`, `select`, `Mutex` / `RWMutex` / `Atomic`, `defer` / `recover`, `unsafe`, or types with `drop`.

4. **Deterministic bounded evaluation (`EXP-34`).**
   - Compile-time evaluation runs in a deterministic interpreter with Go++'s exact integer and IEEE-754 arithmetic (`EXP-6`, `EXP-8`) and bounded step (fuel) and memory limits.
   - Hitting `panic(...)`, an unhandled `!` error, division by zero, out-of-bounds access, or the step/memory limit during compile-time evaluation is a **compile error** at the `const` site showing the compile-time call stack.
   - `@Const` marks and checked bodies travel in package export data (`.gopx`), and constant evaluation results are cached by `(function, canonical arguments)`.

## Example

```go
type Vec2 struct{ X, Y float64 }

@Const @Operator
func (a Vec2) Add(b Vec2) Vec2 { return Vec2{a.X + b.X, a.Y + b.Y} }

@Const
func makeCRCTable() [256]uint32 {
    var t [256]uint32
    for i := 0; i < 256; i++ {
        c := uint32(i)
        for k := 0; k < 8; k++ {
            if c&1 != 0 {
                c = 0xedb88320 ^ (c >> 1)
            } else {
                c >>= 1
            }
        }
        t[i] = c
    }
    return t
}

const Origin   = Vec2{0, 0}
const UnitDiag = Origin + Vec2{1, 1}   // evaluated at compile time via @Const @Operator Add
const CRCTable = makeCRCTable()        // [256]uint32 in .rodata, 0 startup cost!

func hasFlag(x uint32) bool {
    const mask = uint32(1) << 12       // local const evaluated at compile time
    return (x & CRCTable[x&0xff] & mask) != 0
}
```
