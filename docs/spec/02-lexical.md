# 2. Lexical elements

Base: Go spec [Lexical elements](https://go.dev/ref/spec#Lexical_elements), [Tokens](https://go.dev/ref/spec#Tokens), [Keywords](https://go.dev/ref/spec#Keywords), [Operators and punctuation](https://go.dev/ref/spec#Operators_and_punctuation), [Semicolons](https://go.dev/ref/spec#Semicolons), [Predeclared identifiers](https://go.dev/ref/spec#Predeclared_identifiers).

## 2.1 Delta D1.2: lexical changes

### Source files

**LEX-1.** Source files end in `.gopp` and are UTF-8, as in [Source code representation](https://go.dev/ref/spec#Source_code_representation).

### Keywords

**LEX-2.** One new keyword is reserved: `macro`. One more word is *contextual*: `base` ([LEX-12](#lex-12)). It is not reserved, so it stays usable as a name. Inheritance has no keyword: it is written with the attribute `@Extends` ([TYP-16](03-types.md#35-inheritance-adr-0023)).

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
| functions | `clone`, `move`, `NewUnique`, `NewShared`, `NewWeak` |
| generic types | `Unique[T]`, `Shared[T]`, `Weak[T]`, `Option[T]`, `Local[T]` |
| constraints | `Copy`, `Clone`, `Send`, `Sync` |
| std macros | `#Assert`, `#Stringify`, `#File`, `#Line`, `#Embed`, `#Todo`, `#Unreachable`, `#Profile`, `#Setting`, `@Derive` |

- Like Go's predeclared names, these can be redeclared in any scope, package level included (`type Option int` is accepted). The declaration hides the predeclared name in its scope. Do not do it.
- `Send` and `Sync` are constraints only ([GEN-11](10-generics.md), [CON-12](12-concurrency.md)): bare `Send` or `Sync` in a type position (`var x Send`) or with type arguments is a compile error. `Local[T]` is a predeclared generic type for `func` and `interface` types only (`Local[func()]`, `Local[any]`, [CON-12](12-concurrency.md)), opting out of `Send`/`Sync` for single-goroutine callbacks and interfaces.
- In Go mode (`.go` files) none of the Go++ predeclared names exist: `Option`, `Unique`, `Shared`, `Weak`, `Local`, `Send`, `Sync`, `Copy`, `Clone`, `clone`, `move`, `NewUnique`, `NewShared` and `NewWeak` are ordinary names there (`Option[int]` is "undefined: Option").
- In Go mode `base` is an ordinary name too ([TYP-26](03-types.md#35-inheritance-adr-0023)).
- The macro names live in their own namespace (they are only used after `#` or `@`).

**LEX-8.** Go's `new` is not predeclared. Using it is a compile error "undefined: new" with a fix-it that suggests `NewUnique(T{})` or `NewShared(T{})`.

**LEX-9.** `MustError` is **not** predeclared. It lives in package `errors` as `errors.MustError` (see [ERR-9](09-errors-and-panics.md)).

**LEX-10.** Macro and attribute names start with a capital letter, built-in or user-defined (D1.14). This keeps them apart from functions and makes them easy to see.

### Directive comments

**LEX-11.** A line comment that starts with `//gopp:` is a directive. The only directive is `//gopp:build`. To the lexer it is an ordinary comment. Besides OS, architecture and tags, a `//gopp:build` term can be the name of a build profile (`//gopp:build debug`, [FOR-10](14-foreign-surface.md)). See [14](14-foreign-surface.md). (The C/C++ settings `cxxstd`, `cflags` and `ldflags` are directives of `gopp.mod`, not of source files; see [FOR-10](14-foreign-surface.md).) A leftover `//gopp:cflags`, `//gopp:cxxstd` or `//gopp:ldflags` in a source file is a compile error. The error carries a note with the `gopp.mod` line to add (`cflags …`); it has no fix-it, because a fix-it cannot move text into another file and deleting the line would lose the flags.

### Contextual words

<a id="lex-12"></a>
**LEX-12.** *`base` is contextual.* `base` is special only inside the methods of a type that has parents (`@Extends`, [TYP-16](03-types.md#35-inheritance-adr-0023)), where `base.M(…)` or `base[Parent].M(…)` calls a direct parent's method ([TYP-20](03-types.md#35-inheritance-adr-0023)). A local, parameter, result or receiver named `base` is an error there, with a rename hint. In any other function, method or scope `base` is an ordinary identifier.


## 2.2 Examples

```go
func load!(path string) []byte {      // `!` after the name: can fail
    f := os.Open!(path)               // `!` after the callee: unwrap
    return io.ReadAll!(&f)            // a File cannot be copied: ReadAll gets it for the call
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
x := new(int)     // error: undefined: new (use NewUnique(0))
```

## 2.3 Where the grammar is defined

Each changed production is defined in exactly one place. [Appendix B](appendix-b-grammar.md) repeats them inside the whole grammar:

| Production | Defined in |
|---|---|
| `FunctionDecl`, `MethodDecl`, `FunctionLit`, `FunctionType`, `MethodElem`, `PrimaryExpr "!" Arguments` (the `!` forms and generic methods) | [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18) |
| `RangeClause`, `RangeVars`, `RangeVar` (`&`) | [8.3](08-slices-and-range.md#83-range-loops-d121) |
| `FieldAttributes` | [13.1](13-macros-surface.md) |
| `Type`, `TypeLit`, `FieldDecl` (no embedded fields) | [3.1](03-types.md) |
| `MacroDecl`, `ProcMacroDecl`, `MacroInvocation`, `Attribute`, `TopLevelDecl`, `Operand` | [13.1](13-macros-surface.md) |
| `ImportPath` forms | [14.1](14-foreign-surface.md) |

## 2.4 Rationale

- Only one keyword is reserved (`macro`). `base` is contextual so it stays usable as a name and old Go code keeps compiling. Inheritance is an attribute (`@Extends`), not a keyword, so it needs no new word and no grammar of its own ([ADR-0034](../adr/0034-extends-attribute.md)). `Unique`, `Shared`, `Weak`, `Option` and `Local` are predeclared generic types, not keywords.
- `#` and `@` were free in Go, so no old code changes meaning.
- `!` stays near the function name where the reader looks first. An earlier draft used `call()!` and needed a special semicolon rule and a `!=` caveat (`f()!==y`). It was rejected.
- `NewUnique`, `NewShared` and `NewWeak` use Go's `NewXxx` constructor casing ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)), matching `sync.NewMutex` while starting with a capital letter so they stand out from Go's lowercase built-ins.
