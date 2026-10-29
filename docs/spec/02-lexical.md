# 2. Lexical elements

Base: Go spec [Lexical elements](https://go.dev/ref/spec#Lexical_elements), [Tokens](https://go.dev/ref/spec#Tokens), [Keywords](https://go.dev/ref/spec#Keywords), [Operators and punctuation](https://go.dev/ref/spec#Operators_and_punctuation), [Semicolons](https://go.dev/ref/spec#Semicolons), [Predeclared identifiers](https://go.dev/ref/spec#Predeclared_identifiers).

## 2.1 Delta D1.2: lexical changes

### Source files

**LEX-1.** Source files end in `.gopp` and are UTF-8, as in [Source code representation](https://go.dev/ref/spec#Source_code_representation).

### Keywords

**LEX-2.** One new keyword is reserved: `macro`. Three more words are *contextual*: `move`, `extends` and `base` ([LEX-12](#lex-12)). They are not reserved, so they stay usable as names. `ref` is not special at all: it is an ordinary identifier.

```
keyword = /* the 25 Go keywords */ | "macro" .
```

- A keyword cannot be used as an identifier.
- Go code that uses `macro` as a name must be renamed. This is accepted: source compatibility with Go is a non-goal.

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
| generic types | `Unique[*T]`, `Shared[*T]`, `Weak[*T]`, `Option[T]`, `View[X]` |
| constraints | `Copy`, `Clone` |
| std macros | `#Assert`, `#Stringify`, `#File`, `#Line`, `#Embed`, `#Todo`, `#Unreachable`, `#Profile`, `#Setting`, `#Run`, `@Derive` |

- Like Go's predeclared names, these can be redeclared in any scope, package level included (`type Option int` is accepted). The declaration hides the predeclared name in its scope. Do not do it. This holds for `View` as for `Option`.
- `View` is a generic type name, not a keyword. `View[X]` is valid for every type `X` except the pointer kinds (`*T`, `Unique[*T]`, `Shared[*T]`, `Weak[*T]`), `unsafe.Pointer` and `View[…]` itself; those are compile errors that name the allowed forms ([OWN-V1](04-ownership.md#44-views-d16)). Named types are fine (`View[User]`, `View[Bytes]`).
- In Go mode (`.go` files) none of the Go++ predeclared names exist: `Option`, `Unique`, `Shared`, `Weak`, `View`, `Copy`, `Clone`, `move`, `clone`, `new_unique`, `new_shared` and `new_weak` are ordinary names there (`Option[int]` is "undefined: Option").
- In Go mode `extends` and `base` are ordinary names too ([TYP-26](03-types.md#35-single-inheritance-adr-0023)).
- The macro names live in their own namespace (they are only used after `#` or `@`).

**LEX-8.** Go's `new` is not predeclared. Using it is a compile error "undefined: new" with a fix-it that suggests `new_unique(T{})` or `new_shared(T{})`.

**LEX-9.** `MustError` is **not** predeclared. It lives in package `errors` as `errors.MustError` (see [ERR-9](09-errors-and-panics.md)).

**LEX-10.** Macro and attribute names start with a capital letter, built-in or user-defined (D1.14). This keeps them apart from functions and makes them easy to see.

### Directive comments

**LEX-11.** A line comment that starts with `//gopp:` is a directive. The only directive is `//gopp:build`. To the lexer it is an ordinary comment. Besides OS, architecture and tags, a `//gopp:build` term can be the name of a build profile (`//gopp:build debug`, [FOR-10](14-foreign-surface.md)). See [14](14-foreign-surface.md). (The C/C++ settings `cxxstd`, `cflags` and `ldflags` are directives of `gopp.mod`, not of source files; see [FOR-10](14-foreign-surface.md).) A leftover `//gopp:cflags`, `//gopp:cxxstd` or `//gopp:ldflags` in a source file is a compile error. The error carries a note with the `gopp.mod` line to add (`cflags …`); it has no fix-it, because a fix-it cannot move text into another file and deleting the line would lose the flags.

### Contextual words

<a id="lex-12"></a>
**LEX-12.** *`move`, `extends` and `base` are contextual.* Each is special only in the places below. Everywhere else it is an ordinary identifier. (`ref` was contextual before ADR-0020. It is now an ordinary identifier everywhere, in `.gopp` and in `.go` files.)
- `extends` is special only in a type declaration, after the type name and its optional type parameters: `type Card extends Payment { … }` ([TYP-16](03-types.md#35-single-inheritance-adr-0023)). Everywhere else it is an ordinary identifier (`extends := 1`, `func extends()`, a field named `extends`).
- `base` is special only inside the methods of a type declared with `extends`, where `base.M(…)` calls the parent's method ([TYP-20](03-types.md#35-single-inheritance-adr-0023)). A local, parameter or result named `base` is an error there, with a rename hint. In any other function, method or scope `base` is an ordinary identifier.
- `move(x)` is a predeclared function ([EXP-1](06-expressions.md)). `move` is also the mark of a parameter or receiver ([PAR-5](05-parameters.md)), but only at the start of a parameter declaration, followed by a parameter name and then the type: `move s string`. A `move` followed by a type alone (`move T`, `move pkg.T`, `move G[K]`, `move []int`) is read as in Go: a parameter *named* `move` of that type. See the EBNF in [5.1](05-parameters.md#51-syntax).


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
var macro = 1     // error: `macro` is a keyword
x := new(int)     // error: undefined: new (use new_unique(0))
ref := "main"     // ok: `ref` is an ordinary identifier
```

## 2.3 Where the grammar is defined

Each changed production is defined in exactly one place:

| Production | Defined in |
|---|---|
| `FunctionDecl`, `MethodDecl`, `FunctionLit`, `FunctionType`, `MethodElem`, `PrimaryExpr "!" Arguments` (the `!` forms and generic methods) | [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18) |
| `ParameterDecl` (`move`) | [5.1](05-parameters.md) |
| `RangeClause`, `RangeVars`, `RangeVar` (`&`) | [8.3](08-slices-and-range.md#83-range-loops-d121) |
| `FieldAttributes` | [13.1](13-macros-surface.md) |
| `Type`, `TypeLit`, `ExtendsDecl`, `FieldDecl` (no embedded fields) | [3.1](03-types.md) |
| `MacroDecl`, `ProcMacroDecl`, `MacroInvocation`, `Attribute`, `TopLevelDecl`, `Operand` | [13.1](13-macros-surface.md) |
| `ImportPath` forms | [14.1](14-foreign-surface.md) |

## 2.4 Rationale

- Only one keyword is reserved (`macro`). `move`, `extends` and `base` are contextual. `move` must also stay a function name because `move(x)` is a built-in; `extends` and `base` stay usable as names so old Go code keeps compiling. Owning pointers, `Option` and `View` are predeclared generic types, not keywords.
- `#` and `@` were free in Go, so no old code changes meaning.
- `!` stays near the function name where the reader looks first. An earlier draft used `call()!` and needed a special semicolon rule and a `!=` caveat (`f()!==y`). It was rejected.
- `new_unique` uses snake case on purpose: it is a built-in that is not a Go name, so it is easy to spot.
