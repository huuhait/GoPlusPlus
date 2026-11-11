# Attributes cheat sheet

Every `@` name in Go++ on one page. The rules are in the spec; this page only says what each one does and where it goes. The full table is [MAC-8](spec/13-macros-surface.md#132-rules).

Where to write them: before a declaration on the line above (`@Pinned` then `type Lock struct {…}`), and on a struct field after the field's type (`ID int @Required`), before any Go tag ([MAC-12](spec/13-macros-surface.md#132-rules)).

```go
type User struct {
    ID   int @Required
    Note Option[string]
}

@Virtual func (a *Animal) Speak() string { return "..." }
```

## Built-in attributes (the compiler handles them)

**Required values** ([ADR-0016](adr/0016-required-fields.md))

| `@` | On | Does |
|---|---|---|
| `@Required` | struct field | must be set in every literal; the struct has no zero value (compile error on `Option[T]` and never-nil pointer/func/interface fields) |

**Inheritance** ([ADR-0023](adr/0023-inheritance.md), [ADR-0034](adr/0034-extends-attribute.md))

| `@` | On | Does |
|---|---|---|
| `@Extends(Parent)` | struct or interface type declaration | names one parent; write one per parent, in order. Struct: inherits fields and methods. Interface: a constraint for types with that ancestor |
| `@Virtual` | pointer method | a child may replace it with `@Override` |
| `@Abstract` | pointer method, or type | method: no body, every child must replace it; type: may leave abstract methods open |
| `@Override` | pointer method | replaces one or more parents' `@Virtual` / `@Abstract` methods; required for that |

A method with no mark is final.

**Operators** ([ADR-0024](adr/0024-operator-overloading.md))

| `@` | On | Does |
|---|---|---|
| `@Operator` | Rust-style operator method (`Add`, `Sub`, `Mul`, `Eq`, `Cmp`, `Index`, `SetIndex`, `Iter`, …) | overloads the corresponding operator (`a + b`, `a == b`, `a < b`, `a[i]`, `range a`) |
| `@Right` | binary `@Operator` method | allows the parameter type on the left: `2.0 * v` |
| `@Convert` | method with no parameters, one result | allows the explicit conversion `float64(x)` through the method |

**Compile-time evaluation** ([ADR-0033](adr/0033-const-functions-and-composite-constants.md))

| `@` | On | Does |
|---|---|---|
| `@Const` | function or method | pure compile-time-capable function/method: runs at compile time in `const` expressions and normally at runtime ([EXP-32](spec/06-expressions.md#611-compile-time-evaluation-and-composite-constants-adr-0033)) |

**Memory and safety**

| `@` | On | Does |
|---|---|---|
| `@Pinned` | type | a value cannot be moved or copied once it is initialized in a place ([OWN-R10](spec/04-ownership.md#42-ownership-rules-d14)) |

**C and C++ interop, low level**

| `@` | On | Does |
|---|---|---|
| `@Extern("C")` | function | the body is in C |
| `@Export("C")` | function | emit a C-ABI symbol so C can call it |
| `@Unwind` | callback | a panic may unwind through its C callers |
| `@Packed` | struct type | no padding between fields |
| `@Align(N)` | type | at least `N`-byte alignment |
| `@Inline`, `@NoInline` | function | inlining hint |
| `@NoBounds` | function | bounds checks off (fast, unsafe) |

**Build switch**

| `@` | On | Does |
|---|---|---|
| `@If(cond)` | any top-level declaration | always type-checked; emitted only when the constant `cond` is true ([MAC-15](spec/13-macros-surface.md#132-rules)) |

## Not built in

| `@` | What it is |
|---|---|
| `@Derive(JSON)` | a standard-library macro (P09) that generates code |
| `@JSON("name")` and other field options | read by a macro such as `@Derive`; the macro defines which it accepts |

There is no `@Final`: a method without a mark is already final.
