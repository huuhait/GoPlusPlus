# 2. Lexical elements

Base: Go spec [Lexical elements](https://go.dev/ref/spec#Lexical_elements), [Tokens](https://go.dev/ref/spec#Tokens), [Keywords](https://go.dev/ref/spec#Keywords), [Operators and punctuation](https://go.dev/ref/spec#Operators_and_punctuation), [Semicolons](https://go.dev/ref/spec#Semicolons), [Predeclared identifiers](https://go.dev/ref/spec#Predeclared_identifiers).

## 2.1 Delta D1.2: lexical changes

### Source files

**LEX-1.** Source files end in `.gopp` and are UTF-8, as in [Source code representation](https://go.dev/ref/spec#Source_code_representation).

### Keywords

**LEX-2.** Three new keywords are reserved: `own`, `view`, `macro`.

```
keyword = /* the 25 Go keywords */ | "own" | "view" | "macro" .
```

- A keyword cannot be used as an identifier.
- Go code that uses `own`, `view` or `macro` as a name must be renamed. This is accepted: source compatibility with Go is a non-goal.

### New tokens

**LEX-3.** Two new punctuation tokens: `#` and `@`.

```
operator_or_punct = /* Go tokens */ | "#" | "@" .
```

- `#` starts a macro invocation: `#Assert(x > 0)`.
- `@` starts an attribute or attached macro: `@Derive(JSON)`.
- Both are only valid in the places listed in [chapter 13](13-macros-surface.md). Neither can be part of an identifier.

**LEX-4.** The existing token `!` gets two new uses:

| Position | Meaning | Example |
|---|---|---|
| `func Name!(` | function can fail | `func load!(path string) []byte` |
| `func!(` | function literal or type that can fail | `func!(x int) T { … }` |
| `expr!(` | unwrapping call | `os.Open!(p)` |
| `macro func Name!(` | procedural macro that can fail (P09 D9.11) | `macro func Equal!(…)` |

Old uses stay: unary `!x` and the operator `!=`.

**LEX-5.** Lexing is greedy, as in Go. `!=` is one token. There is no clash:

- `f!(x)` is `f`, `!`, `(`. In `f != (x)` the `!` is joined with `=`.
- A `!` right after a primary expression and before `(` is the unwrapping mark (`PrimaryExpr "!" Arguments`; generic calls are `f[T]!(x)`, where `!` follows the `]`). A unary `!` can never follow an operand, so the parser always knows which it is.

### Semicolon insertion

**LEX-6.** Semicolon insertion is unchanged. `call!()` ends in `)`, so the existing rule already works. Two consequences:

- A line that ends in `!` does not get a semicolon (like any operator), so `f!` then a newline then `(x)` is read as `f!(x)`. `gopp fmt` never produces this form.
- An attribute line such as `@Pinned` or `@Derive(JSON)` ends in an identifier or `)`, so a semicolon is inserted. The grammar accepts it (see [13](13-macros-surface.md)).

### Predeclared identifiers

**LEX-7.** New predeclared names in the universe block:

| Kind | Names |
|---|---|
| functions | `move`, `clone`, `new_unique`, `new_shared`, `new_weak` |
| generic types | `Unique[T]`, `Shared[T]`, `Weak[T]` |
| constraints | `trivial`, `clonable` |
| std macros | `#Assert`, `#Stringify`, `#File`, `#Line`, `#Embed`, `#Todo`, `#Unreachable`, `@Derive` |

- Like Go's predeclared names, these can be shadowed by a declaration in an inner scope. Do not do it.
- The macro names live in their own namespace (they are only used after `#` or `@`).

**LEX-8.** Go's `new` is not predeclared. Using it is a compile error "undefined: new" with a fix-it that suggests `new_unique(T{})` or `new_shared(T{})`.

**LEX-9.** `MustError` is **not** predeclared. It lives in package `errors` as `errors.MustError` (see [ERR-9](09-errors-and-panics.md)).

**LEX-10.** Macro and attribute names start with a capital letter, built-in or user-defined (D1.14). This keeps them apart from functions and makes them easy to see.

### Directive comments

**LEX-11.** A line comment that starts with `//gopp:` is a directive (`//gopp:build`, `//gopp:cflags`, `//gopp:ldflags`, `//gopp:cxxstd`). To the lexer it is an ordinary comment. See [14](14-foreign-surface.md).

## 2.2 Examples

```go
func load!(path string) []byte {      // `!` after the name: can fail
    f := os.Open!(path)               // `!` after the callee: unwrap
    return io.ReadAll!(f)
}

@Pinned                               // `@` then a capital name
type Gate struct{ n int }

func check(x int) {
    #Assert(x > 0, "x must be positive")   // `#` macro call
    if x != 3 && !(x < 0) { }              // old `!=` and unary `!` are unchanged
}
```

Invalid:

```go
var own = 1       // error: `own` is a keyword
x := new(int)     // error: undefined: new (use new_unique(0))
```

## 2.3 Where the grammar is defined

Each changed production is defined in exactly one place:

| Production | Defined in |
|---|---|
| `FunctionDecl`, `MethodDecl`, `FunctionLit`, `FunctionType`, `MethodElem`, `PrimaryExpr "!" Arguments` (the `!` forms and generic methods) | [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18) |
| `ParameterDecl` (`own`) | [5.1](05-parameters.md) |
| `Type`, `TypeLit`, `ViewType` (`view`) | [3.1](03-types.md) |
| `MacroDecl`, `ProcMacroDecl`, `MacroInvocation`, `Attribute`, `TopLevelDecl`, `Operand` | [13.1](13-macros-surface.md) |
| `ImportPath` forms | [14.1](14-foreign-surface.md) |

## 2.4 Rationale

- Only three keywords are added. Owning pointers are predeclared generic types, not keywords.
- `#` and `@` were free in Go, so no old code changes meaning.
- `!` stays near the function name where the reader looks first. An earlier draft used `call()!` and needed a special semicolon rule and a `!=` caveat (`f()!==y`). It was rejected.
- `new_unique` uses snake case on purpose: it is a built-in that is not a Go name, so it is easy to spot.
