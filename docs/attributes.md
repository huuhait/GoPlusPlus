# Attributes cheat sheet

Every `@` name in Go++ on one page. The rules are in the spec; this page only says what each one does and where it goes. The full table is [MAC-8](spec/13-macros-surface.md#132-rules).

Where to write them: before a declaration on the line above (`@Strict` then `type User struct {…}`), and on a struct field after the field's type (`ID int @Required`), before any Go tag ([MAC-12](spec/13-macros-surface.md#132-rules)).

```go
@Strict
type User struct {
    ID   int
    Note string @Option
}

@Virtual func (a *Animal) Speak() string { return "..." }
```

## Built-in attributes (the compiler handles them)

**Required values** ([ADR-0016](adr/0016-required-fields.md), [ADR-0019](adr/0019-strict-structs.md))

| `@` | On | Does |
|---|---|---|
| `@Required` | struct field | must be set in every literal; the struct has no zero value |
| `@Strict` | struct type | every field is required, except `Option[T]`, `@Option` and `_` fields |
| `@Option` | field of a `@Strict` struct | may be left out; gets its zero value |

**Inheritance with `extends`** ([ADR-0023](adr/0023-single-inheritance.md))

| `@` | On | Does |
|---|---|---|
| `@Virtual` | pointer method | a child may replace it with `@Override` |
| `@Abstract` | pointer method, or type | method: no body, every child must replace it; type: may leave abstract methods open |
| `@Override` | pointer method | replaces a parent's `@Virtual` / `@Abstract` method; required for that |

A method with no mark is final.

**Operators** ([ADR-0024](adr/0024-operator-overloading.md))

| `@` | On | Does |
|---|---|---|
| `@Right` | binary operator method | allows the value on the left: `2.0 * v` |
| `@Convert` | method with no parameters, one result | allows the conversion `float64(x)` through the method |

**Units** ([ADR-0025](adr/0025-units-of-measure.md))

| `@` | On | Does |
|---|---|---|
| `@Symbol("km/h")` | `unit` declaration | the text `fmt` prints for the unit |

**Memory and safety**

| `@` | On | Does |
|---|---|---|
| `@View` | struct type | the struct may hold `View[…]` fields ([OWN-V5](spec/04-ownership.md#44-views-d16)) |
| `@Pinned` | type | cannot be moved or copied after first use ([OWN-R10](spec/04-ownership.md#42-ownership-rules-d14)) |

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
| `@Ref` | removed (replaced by `View[…]`, ADR-0020); using it is an unknown-attribute error |

There is no `@Final`: a method without a mark is already final.
