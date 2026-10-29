# 13. Macro and attribute surface

Base: Go has no macros. Related Go spec parts: [Function declarations](https://go.dev/ref/spec#Function_declarations), [Declarations and scope](https://go.dev/ref/spec#Declarations_and_scope), [Qualified identifiers](https://go.dev/ref/spec#Qualified_identifiers), [Comments](https://go.dev/ref/spec#Comments).

Delta D1.14. This chapter fixes only the **surface**: syntax, names and the list of built-in attributes. What macros do (hygiene, expansion order, sandbox, the `gopp/macro` API) is specified in **P09** (`plans/…/09-macros/README.md`).

## 13.1 Syntax

```
MacroDecl      = "macro" MacroName "(" [ MacroParamList ] ")" Block .             /* declarative */
ProcMacroDecl  = "macro" "func" MacroName [ "!" ] Signature FunctionBody .         /* procedural */
MacroParamList = MacroParam { "," MacroParam } .
MacroParam     = identifier ( Kind | "..." Kind ) .
Kind           = "expr" | "stmt" | "block" | "type" | "ident" | "lit" | "decl" | "tokens" .

MacroName      = identifier .                     /* first letter must be upper case */
MacroRef       = [ PackageName "." ] MacroName .

MacroInvocation = "#" MacroRef "(" [ MacroArgs ] ")" .
Attribute       = "@" MacroRef [ "(" [ MacroArgs ] ")" ] .
MacroArgs       = ExpressionList .               /* a `block` argument is a func literal, see MAC-13 */

TopLevelDecl   = /* Go alternatives */ | MacroDecl | ProcMacroDecl | MacroInvocation | AttributedDecl .
AttributedDecl = Attribute { [ ";" ] Attribute } [ ";" ] ( Declaration | UnitDecl | FunctionDecl | MethodDecl ) .   /* UnitDecl: 17.1 */
FieldAttributes = Attribute { Attribute } .             /* used by FieldDecl, defined in 3.1 (03-types.md) */
Operand        = /* Go alternatives */ | MacroInvocation .
```

- A macro invocation in statement position is an `ExpressionStmt` whose expression is a `MacroInvocation` (an `Operand`). It is not a separate `SimpleStmt`, so there is only one parse. Whether the expansion is a statement list or an expression is decided by P09 expansion, not by the parser. At top level, `MacroInvocation` is a declaration.
- The optional `;` after an attribute is the semicolon that [semicolon insertion](02-lexical.md) adds at a line end.
- `macro` is a keyword ([LEX-2](02-lexical.md)).
- Fragment kinds and their meaning are in P09 D9.1.
- Attributes are allowed before top-level declarations (type, func, method, var, const, `unit`) and on struct fields ([MAC-12](#132-rules)). Attributes on statements are not defined in v0. The "Placed on" column of the attribute table in MAC-8 says where each built-in attribute is accepted.

> **Deferred to P09:** how a `stmt`, `type` or `tokens` argument is written at an invocation. Only the `block` kind is fixed here ([MAC-13](#132-rules)).

## 13.2 Rules

**MAC-1.** *Definitions.* `macro Name(p kind, …) { … }` defines a **declarative** macro. `macro func Name(…) …` defines a **procedural** macro, written in Go++. A procedural macro that can fail is declared `macro func Name!(…)` (P09 D9.11).

**MAC-2.** *Invocation.* `#Name(args)` can be used in expression, statement or declaration position. `#File()` with no arguments is written with empty parentheses. It cannot be used where a type goes: `var x #Bits(12)` is a parse error in v0 (P09 Q3, ADR-0011).

**MAC-3.** *Attachment.* `@Name` or `@Name(args)` on top of a declaration (a type, func, method, var or const) attaches a macro or attribute to it. An attribute after a struct field's type is different: it is data read by the enclosing type's attached macro ([MAC-12](#132-rules)), not a separately expanded macro. An attached macro receives its arguments in parentheses plus the declaration it sits on, and returns new code. It may add declarations. It never changes or removes the one it is attached to (P09 D9.1).

**MAC-4.** *Names.* Every macro and attribute name starts with a capital letter (`#Assert`, `@Derive`, `@Extern`), built-in or user-defined.

**MAC-5.** *Across packages.* A macro from another package is qualified like a function: `#pkg.Name(…)`, `@pkg.Name`.

**MAC-6.** *Predeclared.* The std macros `#Assert`, `#Stringify`, `#File`, `#Line`, `#Embed`, `#Todo`, `#Unreachable`, `#Profile`, `#Setting`, `#Run` and `@Derive` need no import.

**MAC-7.** *`@Derive(X)`* finds `X` in std package `derive`, unless qualified: `@Derive(sql.Table)`. Several can be listed: `@Derive(JSON, Equal)`.

**MAC-8.** *Built-in attributes* use the same syntax. They are not macros: the compiler handles them.

| Attribute | Placed on | Meaning | Specified in |
|---|---|---|---|
| `@Extern("C")` | function | the function is defined in C | P07 |
| `@Export("C")` | function | emit a C-ABI symbol | P07 |
| `@Inline`, `@NoInline` | function | inlining hint | P04 |
| `@Packed` | struct type | no padding | P07 |
| `@Align(N)` | type | minimum alignment `N` | P07 |
| `@View` | struct type | the struct may hold view fields; without it a view field is a compile error (`err_view_field_without_attr`) ([OWN-V5](04-ownership.md#44-views-d16)) | this spec |
| `@NoBounds` | function | turn bounds checks off ([EXP-7](06-expressions.md)) | P05 D5.8 |
| `@Pinned` | type | cannot be moved or copied after first use ([OWN-R10](04-ownership.md#42-ownership-rules-d14)) | this spec |
| `@Required` | struct field | the field must be set in every literal; the struct has no zero value ([TYP-15](03-types.md)) | ADR-0016 |
| `@Strict` | struct type | every field is required, as if marked `@Required`, except `Option[T]` fields, `@Option` fields and `_` ([TYP-15a](03-types.md)) | ADR-0019 |
| `@Option` | field of a `@Strict` struct | the field may be left out of a literal and gets its zero value ([TYP-15a](03-types.md)) | ADR-0019 |
| `@Virtual` | method (pointer receiver) | the method may be replaced in a child type with `@Override` ([TYP-18](03-types.md#35-single-inheritance-adr-0023)); an error on a value receiver or a non-method | ADR-0023 |
| `@Abstract` | method (pointer receiver), or type | on a method: no body, every child must override it ([TYP-18](03-types.md#35-single-inheritance-adr-0023)); the type that declares or inherits one is abstract ([TYP-19](03-types.md#35-single-inheritance-adr-0023)). On an `extends` type: the child may leave `@Abstract` methods unimplemented | ADR-0023 |
| `@Override` | method (pointer receiver) | replaces a `@Virtual` or `@Abstract` method of an ancestor; required for that; an error when there is nothing to override, when the ancestor method is final, or when the signature differs ([TYP-18](03-types.md#35-single-inheritance-adr-0023)) | ADR-0023 |
| `@Right` | binary operator method (every binary operator except `&&` and `\|\|`) | `s op a` is valid, where `s` has the parameter type. On `+ * & \| ^ ==` the method serves both orders; on every other operator it is written for the swapped order (the receiver is the right operand) and serves `s op a` only. An error on unary forms, on `&&`/`\|\|`, and when the parameter type equals the receiver type ([EXP-26](06-expressions.md#610-operator-methods-adr-0024)) | ADR-0024 |
| `@Convert` | method (value receiver, no parameters, one result) | enables the explicit conversion `R(x)` to the result type `R`; at most one per target type; an error when a built-in conversion exists already or on a non-method ([EXP-25](06-expressions.md#610-operator-methods-adr-0024)) | ADR-0024 |
| `@Symbol("text")` | `unit` declaration | the text `fmt` prints for the unit instead of its name; a non-empty string constant with no whitespace and none of `*`, `/`, `^`, `(`, `)`; one per unit; no effect on identity or on source spelling ([UNI-13](17-units.md#172-rules)); an error on anything but a `unit` declaration | ADR-0025 |
| `@Unwind` | callback function | the callback may unwind through its C callers | P07 |
| `@If(cond)` | type, func, method, var, const | the declaration is type-checked, but only emitted and exported when the constant `bool` `cond` is true ([MAC-15](#132-rules)) | P09 D9.12 |

`@Derive(…)` is a std macro, not a built-in attribute (P09).

**MAC-9.** *Errors.* An error or panic inside a macro becomes a compile error at the use site (P09 D9.11).

**MAC-10.** *`!` never means "macro".* This keeps `!` unambiguous as Must ([LEX-4](02-lexical.md)).

**MAC-11.** *Not a conditional-compilation tool and no new syntax.* Macros only use the `#` and `@` forms (P09 D9.9). Use `//gopp:build` to keep code from being parsed at all. `#Profile`, `#Setting` and `@If` ([MAC-15](#132-rules)) do not hide code: it is still type-checked.

**MAC-12.** *Attributes on struct fields.* Attributes are allowed on struct fields in v0. They are written **after the field's type**, before any Go struct tag. Top-level attributes stay on the line before the declaration.
- ``Name string @JSON("name") `db:"name"` ``: the attribute `@JSON("name")` first, then the tag.
- Several attributes are written one after the other on the same line: `Name string @A @B`.
- A field attribute is read by the macro or attribute of the type (for example `@Derive(JSON)` reads `@JSON("name")`). Which attributes a macro accepts is defined by that macro (P09). The exception is a built-in field attribute such as `@Required` or `@Option` (MAC-8), which the compiler itself handles.

**MAC-13.** *Block arguments.* A `block` argument of `#Name(args)` is written like a func literal: `#Repeat(3, func() { fmt.Println("hi") })`. It is not a closure. The code is pasted inline where the macro uses it. So `return`, `break` and `continue` inside it are **compile errors**: they would leave or continue the code around the macro in a way the reader cannot see. The exception is a nested func literal within the block argument.

**MAC-14.** *Compile-time arguments.* Arguments that a macro **reads as values** must be known while compiling: every argument of an attribute `@Name(args)`, and the arguments of a `#Name` macro for parameters of a value kind (`lit`): literals, names, types and constants. A runtime variable there is a compile error: `@Retry(n)` with a variable `n` is rejected, `@Retry(3)` is fine. Only `lit` parameters and `@` attribute arguments must be compile-time values. All other kinds (`expr`, `ident`, `stmt`, `block`, `decl`, `type`, `tokens`) are code fragments with no such limit: they are pasted into the program, so they may name runtime variables: `#Assert(len(data) > 0, "empty input")` and `#Swap(tmp, other)` are fine. How a procedural macro reads its arguments and returns code (the `gopp/macro` API) is designed in P09.

**MAC-15.** *Build profile access.* `#Profile()` is a string constant: the name of the main module's active build profile, the same in every package. `#Setting("name")` is a string constant: the value of one profile setting for the package being compiled; its argument must be a string literal that names a setting. Both can be used in constant expressions, so `#Profile() == "debug"` is a constant `bool`. In an `if` with a constant condition, the branch that cannot run is removed from the generated code but is still type-checked. `@If(cond)` on a declaration needs a constant `bool` ([MAC-14](#132-rules)); when it is false the declaration is type-checked but not emitted or exported, and using it from live code is a compile error (using it from a removed branch or from another disabled declaration is fine). Profiles are defined in P06 D6.12, the details in P09 D9.12.

**MAC-16.** *Compile-time calls.* `#Run(f(args))` runs one call to a function from another package while compiling. Its arguments must be compile-time values ([MAC-14](#132-rules)). A `bool`, numeric or string result is a constant of the function's result type (`const Seed = #Run(gen.Hash("crc32"))`). An array or struct made only of those may initialize a package-level `var`, and is stored in the binary. A call without `#Run` never runs at compile time. Details in P09 D9.13.

> **Deferred to P09:** the exact procedural-macro API.

## 13.3 Examples

```go
macro Assert(cond expr, msg lit) {            // declarative; body is ordinary Go++
    if !(cond) {
        panic("assertion failed: " + #Stringify(cond) + ": " + msg)
    }
}

func checksum(data []byte) uint32 {
    #Assert(len(data) > 0, "empty input")      // invocation in statement position
    …
}

@Derive(JSON, Equal)                           // attached macros; X is looked up in package derive
type Config struct {
    Name string
    Tags []string
}

@Pinned
@Packed                                         // two attributes: each line ends with an auto semicolon
type Header struct{ A uint8; B uint32 }

@Derive(JSON)
type User struct {
    ID   int
    Name string @JSON("name") `db:"name"`     // field attribute after the type, before the Go tag
}

@Retry(3)                                       // compile-time argument; the macro gets the declaration and adds a retrying wrapper next to it
func fetch!() []byte { … }

func demo() {
    #Repeat(3, func() { fmt.Println("hi") })    // a block argument: pasted inline, `return`/`break`/`continue` are errors
}

// (needs `import "example.com/mymacros"` in the import block at the top of the file)
@mymacros.Table("users")                        // qualified attached macro
type Account struct{ ID int }

n := #Line()                                    // expression position

@If(#Profile() == "debug")                       // type-checked in every profile, emitted only in `debug`
func dump(s string) { println(s) }

func trace(s string) {
    if #Profile() == "debug" {                  // constant condition: dead in other profiles, still type-checked
        dump(s)
    }
}
```

Compared with Go: Go uses `go generate` and `reflect` for this. Go++ keeps everything inside the compiler, visible to tools (`gopp expand`).

## 13.4 Rationale

- One invocation syntax (`#`) and one attachment syntax (`@`) keep `gopp fmt`, tree-sitter and third-party tools working on every file.
- Capital letters keep macros apart from functions.
- `!` is reserved for errors, so there is no `foo!()` macro form as in Rust.
