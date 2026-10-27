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
MacroArgs       = ExpressionList .               /* argument forms for stmt/block/type/tokens kinds: P09 */

TopLevelDecl   = /* Go alternatives */ | MacroDecl | ProcMacroDecl | MacroInvocation | AttributedDecl .
AttributedDecl = Attribute { [ ";" ] Attribute } [ ";" ] ( Declaration | FunctionDecl | MethodDecl ) .
Operand        = /* Go alternatives */ | MacroInvocation .
```

- A macro invocation in statement position is an `ExpressionStmt` whose expression is a `MacroInvocation` (an `Operand`). It is not a separate `SimpleStmt`, so there is only one parse. Whether the expansion is a statement list or an expression is decided by P09 expansion, not by the parser. At top level, `MacroInvocation` is a declaration.
- The optional `;` after an attribute is the semicolon that [semicolon insertion](02-lexical.md) adds at a line end.
- `macro` is a keyword ([LEX-2](02-lexical.md)).
- Fragment kinds and their meaning are in P09 D9.1.

> **Open (P01 review):** D1.14 does not say where attributes are allowed besides "before a declaration". This spec allows them before top-level declarations (type, func, method, var, const). Attributes on struct fields or statements are not defined in v0. The "Placed on" column of the attribute table in MAC-8 is also this spec's reading; the plan only lists the names.

> **Open (P01 review):** how a `stmt`, `block`, `type` or `tokens` argument is written at an invocation (`#Name({ … })`?) is not defined by D1.14. It is left to P09.

## 13.2 Rules

**MAC-1.** *Definitions.* `macro Name(p kind, …) { … }` defines a **declarative** macro. `macro func Name(…) …` defines a **procedural** macro, written in Go++. A procedural macro that can fail is declared `macro func Name!(…)` (P09 D9.11).

**MAC-2.** *Invocation.* `#Name(args)` can be used in expression, statement or declaration position. `#File()` with no arguments is written with empty parentheses.

**MAC-3.** *Attachment.* `@Name` or `@Name(args)` before a declaration attaches a macro or attribute to it. An attached macro may add declarations. It never changes or removes the one it is attached to (P09 D9.1).

**MAC-4.** *Names.* Every macro and attribute name starts with a capital letter (`#Assert`, `@Derive`, `@Extern`), built-in or user-defined.

**MAC-5.** *Across packages.* A macro from another package is qualified like a function: `#pkg.Name(…)`, `@pkg.Name`.

**MAC-6.** *Predeclared.* The std macros `#Assert`, `#Stringify`, `#File`, `#Line`, `#Embed`, `#Todo`, `#Unreachable` and `@Derive` need no import.

**MAC-7.** *`@Derive(X)`* finds `X` in std package `derive`, unless qualified: `@Derive(sql.Table)`. Several can be listed: `@Derive(JSON, Equal)`.

**MAC-8.** *Built-in attributes* use the same syntax. They are not macros: the compiler handles them.

| Attribute | Placed on | Meaning | Specified in |
|---|---|---|---|
| `@Extern("C")` | function | the function is defined in C | P07 |
| `@Export("C")` | function | emit a C-ABI symbol | P07 |
| `@Inline`, `@NoInline` | function | inlining hint | P04 |
| `@Packed` | struct type | no padding | P07 |
| `@Align(N)` | type | minimum alignment `N` | P07 |
| `@View` | struct type | the struct may hold view fields without a vet warning ([OWN-V5](04-ownership.md#44-views-d16)) | this spec |
| `@NoBounds` | function | turn bounds checks off ([EXP-7](06-expressions.md)) | P05 D5.8 |
| `@Pinned` | type | cannot be moved or copied after first use ([OWN-R10](04-ownership.md#42-ownership-rules-d14)) | this spec |
| `@Unwind` | callback function | the callback may unwind through its C callers | P07 |

`@Derive(…)` is a std macro, not a built-in attribute (P09).

**MAC-9.** *Errors.* An error or panic inside a macro becomes a compile error at the use site (P09 D9.11).

**MAC-10.** *`!` never means "macro".* This keeps `!` unambiguous as Must ([LEX-4](02-lexical.md)).

**MAC-11.** *Not a conditional-compilation tool and no new syntax.* Macros only use the `#` and `@` forms (P09 D9.9). Use `//gopp:build` for conditional compilation.

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

// (needs `import "example.com/mymacros"` in the import block at the top of the file)
@mymacros.Table("users")                        // qualified attached macro
type User struct{ ID int }

n := #Line()                                    // expression position
```

Compared with Go: Go uses `go generate` and `reflect` for this. Go++ keeps everything inside the compiler, visible to tools (`gopp expand`).

## 13.4 Rationale

- One invocation syntax (`#`) and one attachment syntax (`@`) keep `gopp fmt`, tree-sitter and third-party tools working on every file.
- Capital letters keep macros apart from functions.
- `!` is reserved for errors, so there is no `foo!()` macro form as in Rust.
