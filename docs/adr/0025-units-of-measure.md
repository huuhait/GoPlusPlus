# ADR-0025: Units of measure in the language, written `[UNIT]`

- **Status:** accepted
- **Date:** 2026-10-28
- **Plan reference:** extends ADR-0024 decision 15 (no literal suffixes); new spec chapter 17 (UNI-1 to UNI-16), new EXP-31 to EXP-37 (section 6.12), TYP-29, LEX-14, GEN-14, `@Symbol` in MAC-8, changes LEX-2, LEX-7, LEX-12 and the grammar in chapters 2, 3, 10 and 13; builds on ADR-0018, ADR-0020 and ADR-0024; new tasks P03.25 and P04.24; P13 and P14 notes.

## Context

ADR-0024 decision 15 says units are written the Go way: typed constants times a number, like `30 * time.Second` and `10*units.Km + 500*units.M`. That is safe against mixing `Meters` and `Seconds`, but it does no unit math. `distance / time` has no unit unless the programmer writes an operator method for every pair; `Km + M` needs a conversion by hand; `Meters * Meters` is `Meters`, not square meters. A wrong-unit bug is only found by a test.

F# has units of measure in the language. The compiler checks them and they cost nothing at run time. Rust (`uom`) and C++ (`mp-units`) get most of the safety from libraries, with long types and long error messages. Go has nothing.

On 2026-10-28 the owner decided: Go++ gets **units of measure in the language, like F#**, written with `[UNIT]` (not F#'s `<km>`).

## Decision

1. **Declarations (UNI-1 to UNI-4).** `unit` is a contextual keyword at the start of a top-level declaration.
   - Base unit: `unit M`. Derived unit: `unit KM = 1000 M` (a constant factor and a unit expression) or `unit N = KG * M / S^2` (a pure unit expression).
   - Unit expressions allow `*`, `/` and integer powers `^N`, negative ones too. This is a separate small grammar; its `^` is not Go's xor.
   - A unit is exported by an uppercase first letter, like any Go name. Other packages use `units.KM`; after a dot import, `KM`.
   - A unit cycle is an error. Every unit has a dimension (the exponents of the base units) and an exact rational factor. A derived unit needs a non-empty dimension and a factor greater than zero.
2. **Unit types (UNI-6 to UNI-8, TYP-29).** `T[U]`, where `T` is a predeclared numeric type (all `int` and `uint` kinds, `byte`, `rune`, `float32`, `float64`, `complex64`, `complex128`; not `uintptr`).
   - They work in aliases (`type Speed = float64[M/S]`) and in defined types (`type Distance float64[M]`).
   - `T[U]` with a non-numeric `T`, or with a user type, is an error in v1. A user type gets a unit by a generic parameter: `type Vec2[U unit] struct{ X, Y float64[U] }` (decision 9, accepted by the owner).
   - *How the checker tells a unit from a type argument:* the parser already reads `float64[KM]` as a generic shape. The checker looks at the **base name**. A predeclared numeric type (or an alias of one) means the bracket holds one unit expression. A generic type or function takes a unit for a parameter whose constraint is `unit`. Anything else is the normal generic shape, or an error with a hint (UNI-7). The parser reads the unit grammar (so `^` is the power mark) right after a numeric type name, a number literal and `.In`. **Documented limit:** in an *expression* type argument, such as the explicit call `Avg[M/S^2](xs)`, the expression parser reads `^` as xor, so the checker rejects `^` there with the hint "name the unit with a `unit` declaration, or let inference find it" (type positions, literals and `.In` are not affected).
3. **Literals (EXP-31).** A number literal directly followed by `[UnitExpr]` has that unit: `100.0[KM]`, `3[M/S]`. Indexing a number literal is never legal, so there is no clash. For an expression that is not a literal, use the conversion `float64[KM](x)`. The constant is untyped in its number part (`3[M/S]` defaults to `int[M/S]`).
4. **Arithmetic and identity (UNI-5, EXP-33 to EXP-36).**
   - `*` and `/` combine units and the result is simplified to a **canonical form**: the base units sorted with their exponents, plus the exact factor. So `KM/H` written in any way is one type (decision 13 keeps a display form for printing). A result with an empty dimension (`M/M`, `KM/M`) is the plain numeric type and **keeps the factor**: the value is multiplied by it, so `5.0[KM] / 2.0[M]` is the plain `2500.0`.
   - A scalar may multiply or divide a unit value (`2 * d`, `d / 2`).
   - `+`, `-`, `%`, `min`, `max` and the comparisons need the same dimension. If the scales differ, **both operands are converted to the finer unit** (the smaller factor) at compile time, and the result has that unit, in either order: `100.0[KM] + 500.0[M]` is `100500.0[M]`, and `int[KM] + int[M]` and `int[M] + int[KM]` both give `int[M]`. This holds for every numeric base type. For integers the conversion multiplies by the ratio of the factors; if the ratio is not an integer it is an error ("convert to float first"). Compound assignment `x op= y` always keeps the unit of `x` and converts `y` to it: for floats and complex always (`x` in `KM`, `x += 500.0[M]` adds `0.5`); for integers only when exact (`int[M] += int[KM]` is fine, `int[KM] += int[M]` is an error with a hint).
   - A plain number cannot be added to a unit value (`d + 1.0`, `d > 1.0`, `d++` are errors). **The one exception is the untyped constant `0`**: it is accepted wherever a unit value is expected (comparison, assignment, `+`/`-`, arguments, return), because zero means the same in every unit. `d > 0` and `var d float64[KM] = 0` work; `float64(0)` and any other number do not.
   - Assignment, arguments, returns, composite fields, channel sends, map keys and the like need the **exact** unit: same dimension and same factor. Changing the scale takes `.In[U]()` or a conversion, so an API boundary never rescales silently.
   - `%` by a plain number is an error. Shifts by a plain count keep the unit. `& | ^ &^` and unary `^` are errors.
5. **Conversions (EXP-32).** `float64[KM](v)` gives a plain value a unit, or converts a unit value of the same dimension. `float64(d)` removes the unit (explicit). `d.In[M/S]()` converts to another unit of the same dimension. An explicit conversion of an integer to a coarser unit is allowed and truncates toward zero, like Go's integer division (`int[KM](int[M](1999))` is `int[KM](1)`); only implicit conversions are exact-only. `In` is a built-in member of unit values, not a method.
6. **Erasure (UNI-9).** Units exist at compile time. A `float64[KM]` is a `float64` at run time, with the same layout and ABI. A scale conversion is one multiplication by a constant, folded where possible. The **dynamic type** in an interface keeps the unit (a type switch and `fmt` need it); this costs one type descriptor per unit type that is boxed.
7. **Generics (UNI-10, GEN-14).** `[U unit]` is a unit type parameter: `func Avg[U unit](xs []float64[U]) float64[U]`. Unit expressions can use parameters (`float64[U/S]`). Inference is exact because the other units are known. Type-set constraints stay unitless: `[T ~float64]` does **not** accept `float64[M]`. Operator constraints (ADR-0024) do: `float64[M]` satisfies `+ (T) T`, `- () T`, `<=> (T) int`, `* (float64) T` and so on, but not `* (T) T`.
8. **Math (UNI-11).** `math.Sqrt` and the other `math` functions stay unitless: the program writes `float64[M](math.Sqrt(float64(area)))`. A unit-generic `units.Sqrt` needs inference through powers and is an open point.
9. **User types (UNI-6, GEN-14).** A generic type with a unit parameter is the way (`Vec2[U unit]`). Writing `Vec2[M]` for a non-generic `Vec2` is an error. Automatic units on any user type stays an open point.
10. **Printing (UNI-12, UNI-13).** `fmt` prints the value and its unit with `%v`, `Print` and `Println`: `50 KM/H`. The numeric verbs (`%f`, `%d`) print the number only. The unit name is the declared name; `@Symbol("km")` on the `unit` declaration gives a display text (added, MAC-8).
11. **Std (UNI-14).** P13 gets a `units` package with SI base and common derived units. `time.Duration` stays the Go-style typed type (the owner chose the V1 style for std earlier); moving it to units is an open point (UNI-16).
12. **Interplay (UNI-15).**
    - ADR-0024: user operator methods cannot be declared on unit types (named ones too); the operators are the built-in ones of this ADR. Ordinary methods on a named unit type are fine.
    - ADR-0024 decision 15 (no literal suffixes) **stays**. `100.0[KM]` is not a user suffix: it names a declared unit and runs no user code. This does NOT bring back user suffix functions (`10.5_km`) or any lexer rule.
    - Unit types are plain numbers: `Copy`, trivial, no pointers, `Send` and `Sync`; views of them are fine.
    - Export data (P03.11) holds units, unit types, unit parameters and display forms.
    - Go mode: not applicable.
13. **Display form (UNI-5).** The canonical form decides identity, but the program also keeps, for each unit type, the unit names as written (or as combined by the arithmetic), to print `KM/H` and not `5/18 M/S`. When several spellings exist for one type, a fixed order (fewest names, then text order) picks the printed one, so the result never depends on compile order.

## Example

```go
unit M
unit S
unit KG
unit KM = 1000 M
unit H  = 3600 S
unit N  = KG * M / S^2

func Travel(speed float64[M/S], t float64[S]) float64[M] { return speed * t }

d := 100.0[KM]                // float64[KM]
t := 2.0[H]
speed := d / t                // float64[KM/H]: the compiler does the unit math
area  := d * d                // float64[KM^2]
total := d + 500.0[M]         // float64[M]: 100500.0, both operands in the finer unit, when compiling
ratio := 5.0[KM] / 2.0[M]     // float64: 2500.0, the factor is kept
var z float64[KM] = 0         // ok: the untyped constant 0 fits every unit
x := float64[KM](v)           // give a plain value a unit
raw := float64(d)             // remove the unit
ms := speed.In[M/S]()         // float64[M/S]: one constant multiplication
fmt.Println(speed)            // 50 KM/H
var f float64[N] = 2.0[KG] * 9.81[M/S^2]   // ok: the same canonical type

func Avg[U unit](xs []float64[U]) float64[U] { /* … */ }
```

### What the compiler stops

```go
unit A = 2 B
unit B = 3 A                  // error: unit cycle A -> B -> A
unit X = 100                  // error: a unit needs a dimension
d + t                         // error: cannot add KM and H
d + 1.0                       // error: cannot add a number without a unit (d + 0 is fine)
var e float64[M] = d          // error: exact unit needed; write d.In[M]()
var v float64 = d             // error: write float64(d)
var s string[KM]              // error: units apply to numeric types only
var m Meters[KM]              // error: Meters is not a built-in numeric type
2[KM] + 3[M]                  // ok: int[M], 2003; 3[M] + 2[KM] is the same
a + b                         // error when a is int[IN], b is int[CM], unit IN = 2.54 CM: the ratio 127/50 cannot be exact for an int
d > 1.0                       // error: a plain number other than 0 (d > 0 is fine)
d % 2.0                       // error: % by a plain number
d & e                         // error: no bit operators on values with a unit
func (d Distance) + (e Distance) Distance   // error: operators on unit types are built in
Sum([]float64[M]{1.0[M]})     // error: float64[M] is not in the type set of ~float64 (func Sum[T ~float64](xs []T) T)
```

## Comparison

| | Go | F# units of measure | Rust `uom` | C++ `mp-units` | Go++ (this ADR) |
|---|---|---|---|---|---|
| Where | none (typed constants, `time.Duration`) | in the language | library | library | in the language |
| Check | by named types, no unit math | at compile time | at compile time, type-level | at compile time, type-level | at compile time |
| Run-time cost | none | none (erased) | none | none | none (erased; dynamic type keeps the unit in an interface) |
| Write a value | `10 * units.Km` | `10.0<km>` | `Length::new::<kilometer>(10.0)` | `10.0 * km` | `10.0[KM]` |
| Product, quotient | by hand per pair | automatic | automatic, long types | automatic, long types | automatic, canonical form |
| Scale conversion | by hand | no (explicit factors) | automatic by quantity | automatic | `+` and comparisons convert both operands to the finer unit; assignment exact; `.In[U]()` |
| Error messages | simple | short | long, deep generics | long | short ("cannot add KM and H") |
| Declare a unit | `const Km Meters = 1000` | `[<Measure>] type km` | system and macro | `inline constexpr struct km …` | `unit KM = 1000 M` |
| Generic over units | no | `<'u>` | generic dimension | templates | `[U unit]` |
| Printing | by `String()` method | no | by unit marker | by `std::format` | `fmt` prints `50 KM/H` |

F# is the model. It has no scale factors in measure types, and no conversion between `km` and `m` (the program multiplies by a constant). Go++ adds exact factors, so `KM` and `M` convert by the compiler. `uom` and `mp-units` reach a similar result in the library with long types and slow error messages. Go has nothing beyond the typed constant.

## Consequences

- Wrong-unit bugs become compile errors, and the machine code is the same as for plain numbers.
- The type checker gets a new kind of type argument, a unit table per package, exact rational arithmetic on factors, canonical forms with interning, and a unit solver for inference. Work is near-linear (P03.25 has a stress test).
- The parser needs the rule of UNI-7: after a numeric type name, a number literal and `.In`, the bracket is a unit expression; `^` there is the power mark.
- Code generation is almost nothing: erase the unit, fold scale conversions into constant multiplications, keep a descriptor for boxed unit values (P04.24).
- `fmt` needs the descriptor to print the unit, and the formatter, export data, `gopp doc` and the LSP know units.
- A type switch distinguishes `float64[KM]` from `float64`, so converting between `[]float64[KM]` and `[]float64` is not free (UNI-9).
- Library authors choose between a unit type (`float64[M]`) and a plain number at each API. The `units` package gives the SI names.
- `.go` files are unchanged.

## Alternatives

- **V1: Go-style typed constants (the state before this ADR; rejected by the owner for unit math).** `type Meters float64; const Km Meters = 1000; 10 * Km`. Simple, no language change. But there is no `Meters / Seconds`, no `Km + M` conversion, no wrong-unit error beyond named types, and every unit pair needs hand-written code.
- **V2: constructor functions.** `Km(10)`, `d.Meters()`. Hides the scale in functions. No unit math; every unit needs methods; no check of a product.
- **V3: hand-written operator pairs (ADR-0024).** `func (d Meters) / (t Seconds) Speed`. Works, and checks, but the number of methods grows with the square of the unit count, and nothing combines automatically (`Speed * Seconds` needs its own method, and `Meters*Meters` needs `Area`).
- **V4: generic `Quantity[D]` library.** One type with a dimension parameter. Types get long (`Quantity[Div[Length, Time]]`), `Div[L, T]` and `Mul[L, Inv[T]]` are different types for one meaning, so a library needs `Per` and `Times` helper functions and canonicalization that the generic system cannot do. Error messages are the instantiation, not the unit. Needs const generics or type-level numbers.
- **V4 plus generic operator methods.** `func (a Quantity[D1]) * [D2] (b Quantity[D2]) Quantity[Mul[D1, D2]]` with ADR-0024 operators. The ADR-0024 rule that an operator method has no type parameters of its own blocks it; lifting that rule gives the library style of `uom`, with the same long types and the same identity problem.
- **F#-style angle brackets, `10.0<km>` (rejected by the owner in favour of `[KM]`).** `<` and `>` are comparison operators in Go++, so `a<b>c` would need a lookahead rule; `[ ]` after a number literal is free because a number is never indexed.

## Open points

The owner accepted the choices that were listed here in the first draft (canonical identity with a display form, the factor kept for a dimensionless result, the dynamic type kept in an interface, the zero exception, the `^` limit, generic user types with a unit parameter, the finer-unit rule, and the choices on `~float64`, `math`, `%`, shifts, bit operators, `@Symbol`, fmt verbs and `time.Duration`). They are now in the Decision text. These stay open:

1. **Dimensionless and offset units.** `unit PCT = 0.01` and Celsius (`C = K - 273.15`) are not expressible. Angles use a base unit (`unit RAD`).
2. **`sqrt`, `pow` and fractional powers.** `math.Sqrt` is unitless; a `units.Sqrt` needs `U^2` inference and exact roots (UNI-11). `M^(1/2)` is not in the unit grammar.
3. **Units on any user type.** Only through a generic unit parameter in v1. Automatic units for a plain `Vec2` (`Vec2[M]`) is not in v1 (UNI-6).
4. **A `fmt` verb that prints the unit with a number format** (`%f` plus the unit) is not designed (UNI-12).
5. **Bulk conversion of `[]float64[M]` to `[]float64`** has no free form (UNI-9).
6. **`time.Duration` as `int64[NS]`** (UNI-16).
