# ADR-0024: Operator overloading, "full minus unsafe"

- **Status:** accepted
- **Status note (2026-10-29, ADR-0023):** an inherited operator method of an untagged parent `P` runs on the parent part, like `Dup()` in ADR-0023 decision 16.
- **Date:** 2026-10-28
- **Plan reference:** replaces the non-goal "no user-defined operator overloading" (spec 1.7, plan README non-goals); spec new section 6.10 (EXP-16 to EXP-30), new TYP-27, LEX-13, RNG-6b, CON-22, GEN-13, changes EXP-4, MAC-8 and the grammar in chapters 2, 3 and 10; builds on ADR-0018, ADR-0020, ADR-0021, ADR-0022 and ADR-0023; new tasks P03.24 and P04.23; P14 note.

## Context

Go has no operator overloading. A program that needs a vector, a matrix, a big number or a unit type must write `a.Add(b).Mul(c)`. This is hard to read, and it is the main reason numeric code in Go looks worse than in C++, Rust or Zig. Generic code is hurt too: `Sum[T]` can work for `int` but not for `Vec2`, because there is no way to say "T has `+`".

C++ has the most overloadable operators, including the unsafe ones (`=`, `&`, `,`, implicit conversions, `->`). Rust has only math, compare, index, deref and loops, and each one is a trait with boilerplate.

On 2026-10-28 the owner decided: Go++ gets **full operator overloading minus the unsafe operators**. The operator itself is the method name. The unsafe ones are a compile error to declare. All the safety rules (borrow checking, views, never-nil, ownership) apply to operator methods as they do to every other method.

## Decision

1. **Syntax (TYP-27, EXP-16).** The operator is the method name.
   - `func (a Vec2) + (b Vec2) Vec2 { … }`: binary.
   - `func (a Vec2) - () Vec2 { … }`: unary. The number of parameters tells binary from unary.
   - A different right-hand type is fine: `func (a Vec2) * (s float64) Vec2`. Several methods with the same operator name and different parameter types form an **overload set** (EXP-17). This is the one place where a type may have several methods with one name.
   - Compound forms (`c += b`) are made from the binary one: `c = c + b` (EXP-19). A type may also define an in-place method with a pointer receiver, `func (m *Matrix) += (b Matrix)`. If it exists, `m += b` uses it and allocates nothing.
2. **What can be overloaded (EXP-16).**
   - Binary `+ - * / % & | ^ << >> &^`, and the optional in-place compound forms `+= -= *= /= %= &= |= ^= <<= >>= &^=` (EXP-19).
   - Unary `- + ^ !`.
   - `==` (gives `!=`) and `<=>` returning `int` (gives `< <= > >=`) (EXP-20). `<=>` is also built in for numbers and strings, like `cmp.Compare` (EXP-20).
   - `&&` and `||` on user types. The method may take `func() T` as its right side to keep lazy evaluation (EXP-21).
   - Indexing: `[]` (read; one or more indexes, `m[i, j]`) and `[]=` (write) (EXP-22).
   - Call `()` (EXP-23).
   - Channel `<-`: send `func (q *Queue) <- (v Job)`, receive `func (q *Queue) <- () Job` (CON-22).
   - Explicit conversion with the attribute `@Convert` on an ordinary method: `@Convert func (b BigInt) Float64() float64` enables `float64(b)`. The method keeps its name and can still be called by name. Never implicit (EXP-25).
   - `range`: `func (t *Tree) range () iter.Seq[int]`, so `for v := range t {}` works (RNG-6b).
   - `++` and `--`, as statements, as in Go (EXP-19).
   - Dereference `*x` for user pointer-like types: read access only; the method must return `View[T]`, so there is no write through it (EXP-24).
3. **A number on the left (EXP-26).** The attribute `@Right` on a binary method makes `s op a` work (`s` is the parameter type). It is allowed on every binary operator method except `&&` and `||`.
   - On `+ * & | ^ ==` the order does not matter, so the one method serves `a op s` and `s op a`.
   - On every other operator the method is **written for the swapped order**: with `@Right`, `func (a Vec2) / (s float64) Vec2` is the body of `s / a` (`a` is the right operand), so `1.0 / v` calls it. It does not serve `a / s`. For that, write a second method without `@Right`.
4. **Blocked: a compile error to declare (EXP-29).**
   - `=` (assignment): ownership and moves depend on it.
   - `.` (member access).
   - `&x` (address-of): the borrow checker depends on it.
   - The comma.
   - Implicit conversions: they cause silent bugs. The only conversion hook is the explicit `@Convert`.
   - Also an error, with a hint: `!=` (write `==`), `< <= > >=` (write `<=>`). (The compound forms `+=` and so on may be declared, but only as the optional in-place method of decision 1.)
5. **Ownership and safety (EXP-18).**
   - Operands are **borrowed**. The result is a new owned value. All safety rules apply: borrow checking (ADR-0021), views (ADR-0020), never-nil (ADR-0018), `Send` and `Sync` (ADR-0022).
   - Allocation inside an operator is allowed (`BigInt`, `Matrix`). Go++ has no hidden-allocation rule for methods, and operators are methods.
6. **Generics (GEN-13).** A constraint can require operators: `[T interface{ + (T) T }]`. One `Sum` works for `int`, `Vec2` and `BigInt`. Built-in types satisfy a constraint with their built-in operators, `<=>` included: `int`, `float64` and `string` satisfy `<=> (T) int`, so one ordered constraint serves built-in and user types. `cmp.Ordered` stays.
7. **Vet (EXP-20).** `gopp vet` warns if `==` and `<=>` disagree in definition, or if only one of them is defined.
8. **Precedence and associativity (EXP-28).** The same as Go for every operator. The new `<=>` has the level of the comparison operators (level 3).
9. **Go mode.** `.go` files are unchanged: no operator methods, no `<=>`, no `@Convert`.
10. **`select` and user channel types (CON-22).** A type with `<-` methods cannot appear in a `select` case; only built-in channels can. A `select` needs a try-send, a try-receive and a wait-queue protocol that a plain `<-` method does not give.
11. **Map keys and `comparable` (EXP-20, GEN-13).** They use built-in equality, never a user `==` method (hashing needs the bytes). `gopp vet` warns when a type with a user `==` is used as a map key or in a `comparable` instantiation, because the two can disagree.
12. **Constraint-only interfaces (TYP-27).** An interface with an operator element can be used only as a constraint in v1, like an interface with a union.
13. **No `!` on operator methods (EXP-30).** A failing operator returns an explicit result or panics, like integer division by zero.
14. **`<=>` on built-in types (EXP-20).** For numbers and strings the result is -1, 0 or 1, the same as `cmp.Compare`. NaN follows `cmp.Compare`: NaN is less than every other value and equal to NaN.
15. **No literal suffixes (owner decision 2026-10-28).** Go++ has no user literal suffixes (`10.5_km`). Units are written the Go way, like `time.Duration`: typed constants times a number. `30 * time.Second`, `10*units.Km + 500*units.M` (with `type Meters float64; const Km Meters = 1000`, and a `String()` method for printing), and with operators `10 * money.USD`. No new rule is needed (EXP-27 is removed; example in EXP-27). *Amended the same day by [ADR-0025](0025-units-of-measure.md): units of measure are now in the language (`unit KM = 1000 M`, `float64[KM]`, `100.0[KM]`) with real unit math. The no-suffix decision stays: `100.0[KM]` is not a user suffix and runs no user code.* Imported units are ordinary imported constants, so there is no question about packages, exports or dot imports.

## Example

```go
type Vec2 struct{ X, Y float64 }

func (a Vec2) + (b Vec2) Vec2    { return Vec2{a.X + b.X, a.Y + b.Y} }
func (a Vec2) - (b Vec2) Vec2    { return Vec2{a.X - b.X, a.Y - b.Y} }
@Right
func (a Vec2) * (s float64) Vec2 { return Vec2{a.X * s, a.Y * s} }   // also handles s * a
func (a Vec2) - () Vec2          { return Vec2{-a.X, -a.Y} }
func (a Vec2) == (b Vec2) bool   { return a.X == b.X && a.Y == b.Y }

@Right
func (a Vec2) / (s float64) Vec2 { return Vec2{s / a.X, s / a.Y} }   // written for the swapped order: this is s / a

a := Vec2{1, 2}
b := Vec2{3, 4}
c := a + b           // Vec2{4, 6}
d := -a * 2.0        // Vec2{-2, -4}
e := 2.0 * a         // ok: @Right
f := 1.0 / a         // ok: @Right on /, the method body is s / a (Vec2{1, 0.5})
c += b               // c = c + b: Vec2{7, 10}
ok := a != b         // !(a == b)
```

A generic function, and a type with allocation:

```go
func Sum[T interface{ + (T) T; Clone }](xs []T) T {      // xs must not be empty
    acc := xs[0].Clone()
    for i := 1; i < len(xs); i++ { acc = acc + xs[i] }
    return acc
}
Sum([]int{1, 2, 3})                  // 6: int satisfies + (int) int
Sum([]Vec2{{1, 2}, {3, 4}})          // Vec2{4, 6}
Sum([]BigInt{x, y, z})               // BigInt: the + method allocates the result

type Matrix struct{ rows, cols int; data []float64 }
func (m Matrix) [] (i, j int) float64        { return m.data[i*m.cols+j] }
func (m *Matrix) []= (i, j int, v float64)   { m.data[i*m.cols+j] = v }
func (m Matrix) * (o Matrix) Matrix          { /* allocates the result */ }
func (m Matrix) + (o Matrix) Matrix          { /* allocates the result */ }
func (m *Matrix) += (o Matrix)               { /* adds in place, no allocation */ }

m[1, 2] = 5.0            // calls []=
x := m[1, 2]             // calls []
m[1, 2] += 1.0           // m[1, 2] = m[1, 2] + 1.0 : needs [] and []=
m += n                   // uses func (m *Matrix) += (b Matrix) if defined: no allocation; else m = m + n

type Queue struct{ /* … */ }
func (q *Queue) <- (v Job) { … }      // send
func (q *Queue) <- () Job  { … }      // receive
q <- job
j := <-q

type Meters float64
const (
    M  Meters = 1
    Km Meters = 1000
)
d := 10*Km + 500*M                    // Meters(10500), the Go way (like 30 * time.Second)
price := 10 * money.USD               // a Money value (Cents int64, Cur a Copy currency code) and a @Right `*` method (EXP-27)
```

### What the compiler stops

```go
func (a Vec2) = (b Vec2)               // error: `=` cannot be overloaded; ownership and moves depend on it
func (a Vec2) . ()                     // error: `.` cannot be overloaded
func (a Vec2) & () *Vec2               // error: unary `&` cannot be overloaded; the borrow checker depends on it
func (a Vec2) , (b Vec2) Vec2          // error: the comma cannot be overloaded
func (a Vec2) != (b Vec2) bool         // error: write `==`; `!=` is made from it
func (a Vec2) < (b Vec2) bool          // error: write `<=>`; `<` is made from it
func (a Vec2) += (b Vec2)              // error: an in-place `+=` needs a pointer receiver
func (a *Vec2) += (b Vec2) Vec2        // error: an in-place `+=` has no result
func (a Vec2) + (b Vec2, c Vec2) Vec2  // error: a binary operator takes one parameter
func (p Ptr) * () *Vec2                // error: a user `*` must return View[T] (read-only)
func (a Vec2) ! (b Vec2) Vec2          // error: `!` is unary only
func (a *Vec2) + (b Vec2) Vec2         // error: a pure operator needs a value receiver
func (a Vec2) []= (i int, v float64)   // error: `[]=` changes the receiver; it needs a pointer receiver
func (a Vec2) <=> (b Vec2) bool        // error: `<=>` must return int
func (a Vec2) == (b Vec2) int          // error: `==` must return bool
func (a Vec2) + (b Vec2) Vec2 { … }
func (a Vec2) + (b Vec2) Vec2 { … }    // error: duplicate operator method for the same parameter type
@Right
func (a Vec2) && (s func() bool) Vec2  // error: @Right is not allowed on && and ||: the swap would break laziness
@Right
func (a Vec2) * (b Vec2) Vec2          // error: @Right needs a parameter type different from the receiver
@Convert
func (b BigInt) Int64() int64          // ok: explicit conversion, int64(b)
@Convert
func (c Celsius) Float64() float64     // error: Celsius already converts to float64 (its underlying type)
@Convert
func (b BigInt) Parse(s string) int64  // error: @Convert takes no parameters and one result
@Convert
func (b BigInt) Other() int64          // error: BigInt already has a @Convert to int64
f := float64(b)                        // ok: calls the @Convert method
var f float64 = b                      // error: no implicit conversion. Write float64(b)
x := b.Int64()                        // ok: a @Convert method is a normal method and can be called by name
func (a Vec2) + [T any] (b T) Vec2     // error: an operator method has no type parameters of its own
func (a Vec2) +! (b Vec2) Vec2         // error: an operator method cannot be a `!` function
type F func(int) int
func (f F) () (x int) int              // error: F is a func type, so f(x) already means a call
type Stack []int
func (s Stack) range () iter.Seq[int]  // error: Stack already ranges (it is a slice); same for [] on a slice, + on a number with the same types
type Meters float64
func (a Meters) + (b Meters) Meters    // error: Meters already has + (Meters) Meters
func (a Meters) * (b Seconds) Speed    // ok: a different right-hand type
m[1, 2]++                              // error: m[1, 2] is not a place; write m[1, 2] += 1
select { case job := <-q: … }          // error: select works on built-in channels only (decision 10)
type M map[Vec2]int                    // ok, but vet warns: a map key uses built-in equality, not the == method (decision 11)
```

## Comparison

What each language lets the program overload. "Yes" means the language has the feature; Go++ marks which ones it blocks.

| Operator or feature | C++ | Rust | Go | Go++ |
|---|---|---|---|---|
| binary `+ - * / %` | yes | yes (`Add`, `Sub`, `Mul`, `Div`, `Rem`) | no | yes |
| bit operators `& \| ^ << >>` | yes | yes (`BitAnd`, `BitOr`, `BitXor`, `Shl`, `Shr`) | no | yes (`&^` too) |
| unary `-` `!` `^` `+` | yes (`~` for `^`) | `-` and `!` only (`Neg`, `Not`) | no | yes |
| compound `+=` | separate function | separate trait (`AddAssign`) | no | made from the binary one; an optional in-place method |
| `==` and `!=` | both are written (C++20 can rewrite `!=`) | `PartialEq` (`!=` comes free) | no | `==` (gives `!=`) |
| `< <= > >=` | four functions, or `<=>` in C++20 | `PartialOrd` (one method) | no | `<=>` returning `int` (gives all four); also built in for numbers and strings |
| `&&` and `\|\|` | yes, but the short-circuit is lost | no | no | yes; `func() T` keeps it lazy |
| index read and write | `operator[]` (one index; many in C++23), one function for both | `Index` and `IndexMut` | no | `[]` and `[]=`, many indexes |
| call `()` | yes | `Fn` traits are unstable | no | yes |
| dereference `*x` | yes (`operator*`) | `Deref` and `DerefMut` | no | `*x`, read access only |
| `->` | yes | no | no | no (not in the language) |
| conversion | `operator T()`, implicit unless `explicit` | `From` and `Into` (no operator) | no | explicit `@Convert` only |
| loop over a user type | `begin` and `end` | `IntoIterator` | range over func | `range` method |
| increment and decrement | yes (prefix and postfix) | no | no | yes (statements) |
| channel `<-` | no channels | no channels | no | yes |
| number on the left (`2.0 * v`) | a free function | `impl Mul<Vec2> for f64` | no | `@Right` (any binary operator) |
| assignment `=` | yes | no (moves) | no | **blocked** |
| address-of `&` | yes | no | no | **blocked** |
| member access `.` | no (`->` yes) | no (only through `Deref`) | no | **blocked** |
| comma | yes | no | no | **blocked** |
| implicit conversions | yes | no (only deref coercion) | no | **blocked** |
| `new` and `delete` | yes | no | no | not in the language |
| cost of one definition | a function | an `impl` block of 5 to 8 lines | | one method |

C++ has the most, including the unsafe ones. Rust has math, compare, index, deref and loops only. Go++ has everything in the safe set of C++, plus the Go-only ones (channels, `range` over a type).

## Consequences

- Numeric code reads like math. Generic algorithms can require operators, so `Sum`, `Dot`, `Lerp` and `Max` work for built-in numbers and user types, and `Max[T interface{ Clone; <=> (T) int }]` (returning `a.Clone()`, GEN-4) serves both.
- A call like `a + b` can now run user code, allocate and panic. Reviewers must know this. The language still shows what a line may do: the borrow, view and never-nil checks are the same as for a method call.
- `a != b` and `a < b` are made from one definition each, so they cannot disagree with `==` and `<=>`. `gopp vet` catches a type whose `==` and `<=>` disagree.
- The checker needs: operator method names and forms, overload sets, resolution of every operator expression, compound forms, `@Right`, constraint elements, `range`, `@Convert` and the `select` limit. The code generator lowers them to direct calls that inline (P04.23). The formatter, export data and the LSP know the new names.
- Imported C++ operators ([P08 D8.6](../../plans/2026-10-27-goplusplus-language/08-cpp-interop/README.md)) and Go++ operator methods use the same resolution in the checker, so `a + b` looks the same for both.
- `.go` files are unchanged.

## Alternatives

- **A: No overloading, like Go (rejected).** Nothing to do. But vectors, matrices, big numbers and units keep `a.Add(b)`, and generic numeric code stays impossible. This was the old non-goal.
- **B-limited: arithmetic only, no allocation (rejected).** Easy to review, but `BigInt` and `Matrix` are the main use cases and they allocate. The language already allows allocation in methods, so forbidding it only here has no safety value.
- **C++-full, including `=`, `&` and implicit conversions (rejected as unsafe).** Overloading `=` breaks the ownership rules (what does a move do?). Overloading `&` breaks the borrow checker (ADR-0021). Implicit conversions cause silent bugs and hidden copies. `.` and the comma have no safe use.
- **Rust's trait style (rejected).** Every operator needs a trait impl with an associated `Output` type: more boilerplate, and fewer operators (no call, no `&&`, no `++`). Go++ uses plain methods, which fits Go.
- **User literal suffixes, `10.5_km` (rejected by the owner, 2026-10-28).** The earlier draft had `func (km) _km (v float64) Meters` and the cross-package form `10_units.km`. It needs a new token rule, a name lookup inside the lexer's output, and export and dot-import rules. Go already has the idiom for units: typed constants times a number (`30 * time.Second`, decision 15). Decision 15 replaces it.

## Open points

None.
