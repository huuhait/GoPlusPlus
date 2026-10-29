# 17. Units of measure

Base: no Go counterpart. Go has typed constants (`30 * time.Second`), which name a unit but do no unit math.

Delta from ADR-0025 (owner decision 2026-10-28). The compiler knows units: it checks them, does the unit math, and converts scales at compile time. At run time a value with a unit is a plain number ([UNI-9](#172-rules)). Expression rules (literals, conversions, `.In`, arithmetic) are in [6.12](06-expressions.md#612-unit-expressions-adr-0025), EXP-31 to EXP-37. This chapter has the declarations, the types, identity, generics and printing.

## 17.1 Syntax

```
UnitDecl     = "unit" ( UnitSpec | "(" { UnitSpec ";" } ")" ) .          /* top level only, UNI-1 */
UnitSpec     = identifier [ "=" [ Factor ] UnitExpr ] .                  /* no "=": a base unit */
Factor       = int_lit | float_lit | "(" Expression ")" .                /* an untyped numeric constant, > 0 */

UnitExpr     = UnitTerm { ( "*" | "/" ) UnitTerm } .                     /* left to right, UNI-4 */
UnitTerm     = UnitFactor [ "^" [ "-" ] int_lit ] .
UnitFactor   = UnitName | "1" | "(" UnitExpr ")" .
UnitName     = identifier | PackageName "." identifier .                 /* or a unit type parameter, GEN-14 */

UnitType     = NumericName "[" UnitExpr "]" .                            /* UNI-6, TYP-29 */
NumericName  = "int" | "int8" | "int16" | "int32" | "int64" | "uint" | "uint8" | "uint16" | "uint32" | "uint64"
             | "byte" | "rune" | "float32" | "float64" | "complex64" | "complex128" .
UnitLiteral  = ( int_lit | float_lit | imaginary_lit ) "[" UnitExpr "]" . /* EXP-31 */
```

- `unit` is a contextual keyword ([LEX-12](02-lexical.md#lex-12)): special only at the start of a top-level declaration, when an identifier or `(` follows (the group form). It is not reserved anywhere else.
- `UnitExpr` is its own small grammar. Its `^` is the power mark and is **not** Go's `^` (xor): `S^2` is "S squared", and `KG*M/S^2` is `(KG*M)/(S^2)`. A power binds tighter than `*` and `/`.
- `Factor` comes first and is separated from the unit expression by a space only: `unit KM = 1000 M`. A `Factor` that is more than one literal (a name, an operator) is written in parentheses, so it cannot be mixed up with the unit expression: `unit MI = (1609344 / 1000) M`.
- A unit expression is read by the unit grammar in these places only (the parser rule is UNI-7): after a number literal (EXP-31); after a predeclared numeric type name (`float64[KM/H]`, also in a conversion); after `.In` (EXP-32); in a type bracket list, for an element that has a `/` or `^`, or a `*` right after a name (a type never looks like that); and as the right side of a `unit` declaration.

## 17.2 Rules

**UNI-1.** *Declarations.* `unit` declares a unit at **package level** only. A unit in a function is a compile error with a hint to move it.
- A **base unit** has no `=`: `unit M`, `unit S`, `unit KG`. A base unit is its own dimension.
- A **derived unit** is `unit KM = 1000 M`, a factor times a unit expression, or a pure unit expression such as `unit N = KG * M / S^2` (factor 1).
- A group `unit ( M; S; KM = 1000 M )` is the same as separate declarations. An attribute (`@Symbol`, UNI-13) goes before a single-unit declaration; inside a group it is not defined in v1.
- A unit lives in the package block, with types, functions, variables and constants. Redeclaring a name is the usual Go error. Units are **exported by an uppercase first letter**, like every Go name. They are used as `units.KM` from another package, or as `KM` after a dot import (`import . "…/units"`); both forms work inside `[ ]`.
- A unit is not a type and not a value. `x := KM` is an error ("KM is a unit, not an expression"); the name is valid only inside the unit grammar.

**UNI-2.** *Dimension and factor.* Every unit has a **dimension**: the exponents of the base units it is made of (`N` is `KG^1 M^1 S^-2`), and a **factor**: the exact rational number that turns one of the unit into the same amount written in the base units (`KM` is `1000 M`, `H` is `3600 S`, `KM/H` is `5/18 M/S`). Two units with the same dimension differ only by a constant factor. Factors are exact rationals, computed like Go constants (no rounding while compiling the declaration chain).
- The factor expression is an untyped numeric constant expression. It must be greater than zero. Zero, a negative number, a typed constant, a string and a complex constant are errors.
- The unit expression must have a non-empty dimension. `unit X = 100` (a pure factor) and `unit Y = M/M` are errors. A dimensionless quantity such as an angle is made from a base unit: `unit RAD`, `unit DEG = 0.017453292519943295 RAD`.
- Units have no offset. Temperature in Celsius or Fahrenheit (`C = K - 273.15`) cannot be written; `units.K` is the temperature unit.

**UNI-3.** *Cycles and checking.* A unit that depends on itself, directly or through other units (`unit A = 2 B; unit B = 3 A`), is a compile error that lists every unit on the cycle. A name in a unit expression that is not a unit is an error ("X is a variable, not a unit"). A unit declaration is checked once per package. Units are resolved by an iterative walk (no recursion), so a chain of 10 000 derived units does not overflow the stack (P03.23 lesson), and a unit that is used in a cycle does not hide the other errors.

**UNI-4.** *Unit expressions.*
- Operators: `*`, `/` and a power `^N` with a written integer `N` (negative powers too: `S^-1`). `*` and `/` have the same level and go left to right: `KG/M*S` is `(KG/M)*S`; use parentheses for anything else. `1` stands for "no unit" (`1/S`). A power of a parenthesized expression is fine: `(M/S)^2`.
- No fractional powers (`M^(1/2)`), no other operators, no numbers except `1` and the exponents. A zero power (`M^0`) is the same as `1`.
- A name may be a unit, a package-qualified unit, or a unit type parameter ([GEN-14](10-generics.md#102-rules)).

**UNI-5.** *Canonical form and identity.* The compiler simplifies every unit expression to one **canonical form**: the base units sorted by name with their exponents (zero exponents removed), plus the exact factor. Two unit types are **identical** if their canonical forms are equal. So `KM/H`, `KM*H^-1`, `(KM)/(H)` and `1000 M / (3600 S)` are one type, and `unit K2 = 1000 M` makes `K2` and `KM` the same unit. Two units with the same dimension but another factor (`KM/H` and `M/S`) are two different types.
- A product or quotient whose dimension is empty (`M/M`, `KM/M`) is the **plain numeric type**. If its factor is not 1, the value is multiplied by the factor, so the result is the plain physical ratio ([EXP-33](06-expressions.md#612-unit-expressions-adr-0025)).
- The canonical form is made once per distinct expression and interned, so identity is a pointer compare, and the result does not depend on which thread or package builds it first.
- *Display form.* Equal types may be written differently, but the program has one type. For printing ([UNI-12](#172-rules)) the compiler keeps with each unit type its **display form**: the unit names as written, or as combined by the arithmetic from the display forms of the operands (`KM` divided by `H` is `KM/H`), with equal names merged and cancelled. When several display forms exist for one canonical type, the one printed is chosen by a fixed order: fewest unit names (counted with exponents), then the smaller text by qualified name. It never depends on compile order.

**UNI-6.** *Unit types.* `T[U]` is a type, where `T` is a predeclared numeric type (all `int` and `uint` kinds including `byte` and `rune`, `float32`, `float64`, `complex64`, `complex128`) and `U` is a unit expression with a non-empty dimension. `uintptr` is not allowed.
- The base kind is `T`: `float64[KM]` has the arithmetic, range and zero value of `float64`. The zero value is `0` in the unit.
- `T[U]` is a different type from `T` and from `T[V]` for another unit V (identity is UNI-5). Assignability needs identical types ([EXP-35](06-expressions.md#612-unit-expressions-adr-0025)).
- `T[U]` with a non-numeric `T` (`string[KM]`, `bool[M]`, and a unit on a slice type, `([]float64)[KM]`) is an error. (`[]float64[KM]` is valid: a slice of `float64[KM]` elements.) A **user type** is an error in v1: `Meters[KM]`, `Vec2[M]` where `Vec2` is not generic. The way to get a vector with a unit is a generic type with a unit parameter ([GEN-14](10-generics.md#102-rules)): `type Vec2[U unit] struct{ X, Y float64[U] }`. Automatic units on any user type is an open point (ADR-0025).
- A unit type may be used anywhere a type may: variables, fields, parameters, results, elements of slices, arrays, maps and channels, map keys, `chan`, generic arguments, receivers of named types.

**UNI-7.** *Unit argument or type argument.* The parser already reads `float64[KM]` in the shape of a generic instantiation. The checker decides by the **base name**, then reads the bracket content:
- If the name resolves to a **predeclared numeric type** (the type, or an alias of it), the bracket is a unit: exactly one `UnitExpr`. Zero or several elements are errors.
- If the name is a generic type or function, each argument whose type parameter has the constraint `unit` ([GEN-14](10-generics.md#102-rules)) is a unit expression; the other arguments are types.
- Otherwise it is the normal generic shape. A numeric named type (`Meters[KM]`) or a non-generic type is the UNI-6 error, with a hint: "declare `type Distance float64[KM]`".
- *Parser rule.* After a predeclared numeric type name, after a number literal and after `.In`, the parser reads `[` … `]` with the unit grammar at once. In a type bracket list a type element that is followed by `/`, `^`, or `*` is continued as a unit expression (a type is never followed by these, so there is no clash). If a program redeclares `float64` as another type, the parser still reads a unit, and the checker reports that this name is not a numeric type.
- *Where `^` is not available.* In an expression (not a type), a generic call with an explicit unit argument, `Avg[M/S](xs)`, is parsed as an index expression, so `^` there would be Go's xor. The checker accepts `*`, `/` and parentheses in such an argument and rejects `^` with the hint "name the unit with a `unit` declaration, or let inference find it". Inference ([GEN-14](10-generics.md#102-rules)) is the normal way.

**UNI-8.** *Aliases and named types.*
- `type Speed = float64[M/S]` is an alias: the same type as `float64[M/S]`.
- `type Distance float64[M]` is a defined (named) type with the underlying type `float64[M]`. It has its own methods and identity, like any named numeric type. `Distance(x)` follows the conversion rule ([EXP-32](06-expressions.md#612-unit-expressions-adr-0025)) for `float64[M]`.
- Arithmetic follows Go: `Distance + Distance` is `Distance`; `Distance + float64[M]` is an error (two different types). A product or quotient of named types gives the **unnamed** unit type (`Distance / float64[S]` is `float64[M/S]`); a scalar keeps the type (`Distance * 2` is `Distance`).
- A named type with a unit underlying type is a unit type in all the rules below.

**UNI-9.** *Erasure and zero cost.* Units exist at compile time. A value of `float64[KM]` has the representation, layout, alignment and calling convention of `float64`; arrays, slices and structs of it have the same layout as of `float64`. The value is the number **in its own unit** (100 `KM` is stored as `100.0`, not as 100 000). A scale conversion is one multiplication by a constant (`x * 1000`), folded when `x` is a constant and often into the neighbouring arithmetic; a conversion that does not change the scale costs nothing.
- The dynamic type in an interface is **not** erased. `any(d)` has the dynamic type `float64[KM]`, so a type switch `case float64[KM]:` matches it, `case float64:` does not, and `fmt` can print the unit ([UNI-12](#172-rules)). The type descriptor holds the canonical form and the display form. This costs one descriptor per unit type that is converted to an interface, and nothing for values that are not.
- A slice `[]float64[M]` is not convertible to `[]float64` (Go has no such conversion for different element types). Copy it element by element, or keep the unit type. A bulk strip is an open point (ADR-0025).

**UNI-10.** *Generic code and operators.* In a generic body (checked once, [GEN-5](10-generics.md#102-rules)) a type parameter `T` with an operator constraint ([GEN-13](10-generics.md#102-rules)) is satisfied by a unit type through its built-in operators:
- `float64[M]` satisfies `+ (T) T`, `- (T) T`, `- () T`, `+ () T`, `== (T) bool`, `<=> (T) int`, `* (float64) T`, `/ (float64) T` (a scalar of the base type) and the in-place forms of these. It does **not** satisfy `* (T) T` or `/ (T) T`: they change the unit.
- It satisfies `comparable`, `Copy`, `Clone`, `any`, `Send` and `Sync`.
- It does **not** satisfy a type-set constraint such as `~float64`, `cmp.Ordered` or a user `Number` interface made of `~float64 | …`: the underlying type of `float64[M]` is itself, not `float64`. Write a unit-generic function instead (GEN-14) or use operator constraints. `Sum[T interface{ + (T) T }]` works for `int`, `Vec2`, `BigInt` and `float64[M]` alike.

**UNI-11.** *Math functions.* In v1 `math.Sqrt`, `math.Abs`, `math.Pow` and the other `math` functions take plain `float64`. A unit value is passed with the explicit strip, and the result gets its unit with a conversion:
```go
r := float64[M](math.Sqrt(float64(area)))   // area is float64[M^2]
```
A unit-generic `units.Sqrt` would need inference of `U` from `U^2` (exponents and factors with an exact root). That is an open point (ADR-0025). `min` and `max` take unit values of one dimension and follow the `+` rule (all arguments converted to the finest unit among them, the result has that unit, EXP-34).

**UNI-12.** *Printing.* `fmt.Println(d)`, `Print`, `Sprint` and the verb `%v` (also `%+v` and `%#v`) print the number as the same plain type would, then a space, then the unit: `100 KM`, `50 KM/H`, `9.81 M/S^2`. The unit text is built from the display form (UNI-5):
- positive exponents first, names in order, joined by `*`; then `/` and the negative ones. More than one negative name is put in parentheses: `KG/(M*S)`. No positive part: `1/S`. Exponent 1 is not written, others as `^N`.
- The unit is the **declared name** (no package qualifier), or the `@Symbol` text if the unit has one ([MAC-8](13-macros-surface.md#132-rules)).
- The numeric verbs (`%d`, `%f`, `%e`, `%g`, `%x`, `%.2f` and so on) print the number only, without the unit, so formatting stays in the program's hands: `fmt.Printf("%.1f %v\n", float64(d), "km")`. (A verb that prints the unit with a number format is an open point.)
- A named type with a `String()` or `Error()` method is printed by that method, as in Go.
- `complex` units: the unit follows the whole value: `(1+2i) A`.
- `%T` prints the type, `float64[KM]`.

**UNI-13.** *`@Symbol`.* The attribute `@Symbol("km")` on a `unit` declaration sets the text that `fmt` prints for that unit. It has no effect on identity or on how the unit is written in source ([MAC-8](13-macros-surface.md#132-rules)).

**UNI-14.** *Standard library.* P13 provides a `units` package with the SI base units `M`, `S`, `KG`, `A`, `K`, `MOL`, `CD` and common derived units (`MM`, `CM`, `KM`, `G`, `MS`, `US`, `NS`, `MIN`, `H`, `DAY`, `HZ`, `N`, `PA`, `J`, `W`, `V`, `L` and so on), with `@Symbol` texts. The package is ordinary Go++ source built from `unit` declarations. `time.Duration` stays the Go-style typed type for now (UNI-16).

**UNI-15.** *Interplay.*
- **Operator methods (ADR-0024).** A user operator method cannot be declared on a unit type, named ones included (`type Distance float64[M]`): the operators of a unit type are the built-in ones of this chapter (error with the reason). Ordinary methods on a named unit type are fine (`func (d Distance) String() string`).
- **No literal suffixes (ADR-0024 decision 15).** `100.0[KM]` is not a user suffix: the `[ ]` form names a declared unit and runs no user code. User suffix functions (`10.5_km`) stay out of the language.
- **Ownership and concurrency.** Unit types are plain numbers: `Copy`, `Clone`, trivial, no pointers, `Send` and `Sync` ([OWN-C1](04-ownership.md#41-type-categories-d13)). A `View` of one is allowed like a `View` of a number.
- **Zero value and required fields.** The zero value is `0` in the unit; a `@Required` unit field is checked like any field ([TYP-15](03-types.md#34-required-fields-d14-rule-11)).
- **Export data.** Units (name, kind, exact factor, canonical form, `@Symbol`), unit types in signatures, `unit` type parameters and display forms are part of export data (P03.11), so an importer gets the same identity and printing.
- **Go mode.** Not applicable. In `.go` files `unit` is an ordinary name, `float64[KM]` is the normal error ("float64 is not a generic type"), and `100.0[KM]` is an error (indexing a constant).

**UNI-16.** *`time.Duration`.* It stays an ordinary `int64` named type with typed constants (`30 * time.Second`), as in Go. The owner chose the V1 style for the std library earlier. Moving `Duration` to units (`int64[NS]`) is an open point (ADR-0025).

## 17.3 Examples

```go
unit M
unit S
unit KG
unit KM = 1000 M
unit H  = 3600 S
unit N  = KG * M / S^2

func Travel(speed float64[M/S], t float64[S]) float64[M] { return speed * t }

d := 100.0[KM]                      // float64[KM]
t := 2.0[H]                         // float64[H]
speed := d / t                      // float64[KM/H]: prints "50 KM/H"
area := d * d                       // float64[KM^2]
total := d + 500.0[M]               // float64[M]: 100500.0, both operands in the finer unit, when compiling
ms := speed.In[M/S]()               // float64[M/S]: speed * (5.0/18.0), a constant multiplication
d2 := Travel(ms, 3600.0[S])         // float64[M]
x := float64[KM](v)                 // give a plain value a unit
raw := float64(d)                   // 100.0, the unit is removed (the number in KM)
force := 2.0[KG] * 9.81[M/S^2]      // float64[KG*M/S^2], the same type as float64[N]
var f float64[N] = force            // ok: identical types
r := d / 500.0[M]                   // float64: 200, plain, the factor 1000 is applied
q := 5.0[KM] / 2.0[M]               // float64: 2500.0, the factor is kept
var z float64[KM] = 0               // ok: the untyped constant 0 is accepted for any unit

type Speed = float64[M/S]           // alias
type Distance float64[M]            // named type

func Avg[U unit](xs []float64[U]) float64[U] {
    var s float64[U]
    for _, x := range xs { s += x }
    return s / float64(len(xs))     // a plain scalar divides
}
Avg([]float64[KM]{1.0[KM], 3.0[KM]})   // 2 KM: U inferred as KM
```

### What the compiler stops

```go
unit A = 2 B                           // ok
unit B = 3 A                           // error: unit cycle A -> B -> A
unit X = 100                           // error: a unit needs a dimension
unit Y = 0 M                           // error: the factor must be greater than zero
func f() { unit Z }                    // error: units are declared at package level
x := KM                                // error: KM is a unit, not an expression
var s string[KM]                       // error: units apply to numeric types only
var m Meters[KM]                       // error: Meters is not a built-in numeric type; declare `type Distance float64[KM]`
var u uintptr[M]                       // error: uintptr cannot have a unit
d + t                                  // error: cannot add KM and H (different dimensions)
d + 1.0                                // error: cannot add a plain number to a value in KM (d + 0 is fine)
d = speed                              // error: cannot assign float64[KM/H] to float64[KM]
var e float64[M] = d                   // error: assignment needs the exact unit; write d.In[M]() or float64[M](d)
n := 5[M]                              // int[M]
k := 2[KM]
n + k                                  // ok: int[M] (k is converted to M, factor 1000)
k + n                                  // ok: int[M], the same in either order
unit IN = 2.54 CM
var a int[IN]
var b int[CM]
a + b                                  // error: the ratio 127/50 cannot be exact for an int; convert to float first
var km int[KM]
var m int[M]
km += m                                // error: compound assignment keeps KM and the conversion would truncate; write km += int[KM](m)
m += km                                // ok: km is converted to M (x1000)
int[KM](int[M](1999))                  // ok: an explicit conversion truncates toward zero, int[KM](1)
d += 500.0[M]                          // ok for floats: adds 0.5 to d (stays KM)
d > 0                                  // ok: the untyped constant 0 is accepted
d > 1.0                                // error: cannot compare a number without a unit with KM
d.In[H]()                              // error: KM and H have different dimensions
d % 2.0                                // error: % on a unit value needs a value of the same dimension
d & e                                  // error: bitwise operators are not defined on values with a unit
d++                                    // error: ++ adds a plain 1; write d += 1.0[KM]
var v float64 = d                      // error: float64[KM] is not float64; write float64(d)
```

## 17.4 Rationale

- Units are a compile-time check that costs nothing at run time: wrong-unit bugs (the Mars Climate Orbiter) become type errors, and the machine code is the same as for plain numbers.
- Exact rational factors and a canonical form make `KM/H` one type however it is written, and make unit inference exact.
- Assignment, arguments and returns need the exact unit, so an API never rescales a value silently; `+`, `-`, `%`, `min`, `max` and comparisons convert both operands to the finer unit, so nothing is lost, integers stay exact and the order of the operands does not matter. Zero (the untyped constant) is the one plain number accepted everywhere, because it means the same in every unit.
- `unit` and `[UNIT]` keep Go's look: names are Go names (exported by case), brackets are already the Go++ way to give a type an argument, and the unit literal cannot be confused with indexing because a number is never indexed.
- It is not user-defined literals: no user code runs for `100.0[KM]` (ADR-0024 decision 15 stays).
