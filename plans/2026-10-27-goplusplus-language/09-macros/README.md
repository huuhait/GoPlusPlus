# P09 — Macros

**Goal:** hygienic, fast, sandboxed macros that replace Go's `go generate` and reflection patterns: declarative macros, procedural macros written in Go++ and run at compile time, and attached macros (`@Derive`). Tooling can see all of them.
**Depends on:** P03, P04 (the interpreter executes GIR), P06 (comptime GIR and expansions live in the build cache) · **Unblocks:** P13 (`#Assert`, the `@Derive(JSON)` fast path) · **Size:** L · **Milestone:** M6

## Decisions

**D9.1 — Two kinds of macro, one invocation syntax.**
- **Declarative:** `macro Name(param kind, …) { body }`.
  - Fragment kinds: `expr`, `stmt`, `block`, `type`, `ident`, `lit`, `decl`, `tokens`, and variadic `...kind`.
  - The body is ordinary Go++ in which parameter names are substituted as AST nodes.
  - Expanded right after parsing (D9.3); no code runs.
- **Procedural:** `macro func Name(ctx *macro.Ctx, …) …`.
  - Written in Go++, compiled to GIR, and executed by the compile-time interpreter.
  - Receives syntax (plus read-only type information, for attached macros) and returns syntax built with the `gopp/macro` API: one constructor per syntax node (`macro.If`, `macro.Call`, …), with no format strings or quote syntax in v0 (Q2, ADR-0011).
- **Invocation:**
  - `#Name(args)` for expression, statement and declaration macros. Not where a type goes (`var x #Bits(12)` is a parse error in v0); a declaration macro that declares a named type covers those cases (Q3, ADR-0011).
  - `@Name` or `@Name(args)` attached to a declaration. An attached macro receives its arguments plus the declaration it sits on, and returns new code. It may *add* declarations (methods, functions, types). It never modifies or removes the declaration it is attached to.
  - Attributes may also follow a struct field's type, before the Go tag: ``Name string @JSON("name") `db:"name"` `` (P01 D1.14). `@Derive(JSON)` reads them.
- **Arguments.** Arguments a macro reads as values must be known while compiling: every `@Name(args)` argument, and `#Name` parameters of a value kind (`lit`). A runtime variable there is a compile error (`@Retry(n)`). Code-fragment parameters (`expr`, `ident`, `block`, `stmt`) are pasted into the program and may name runtime variables (`#Assert(x > 0, "bad")`). A `block` argument is written as a func literal, `#Repeat(3, func() { … })`, and is pasted inline: `return`, `break` and `continue` inside it are compile errors. How `stmt`, `type` and `tokens` arguments are written, and the exact procedural-macro API, are designed in this phase.

```go
macro Assert(cond expr, msg lit) {
    if !(cond) {
        panic("assertion failed: " + #Stringify(cond) + ": " + msg)
    }
}

func checksum(data []byte) uint32 {
    #Assert(len(data) > 0, "empty input")
    …
}
```

`#Assert` needs no `fmt`, so the runtime and `strconv` can use it too.

**D9.2 — Hygiene.**
- Identifiers that a macro body introduces are renamed into a fresh scope.
- Free identifiers resolve where the macro is *defined* (its package scope), not where it is invoked.
- Breaking hygiene is always explicit: through an `ident` parameter, or `macro.Ident(ctx, "name")` in a procedural macro.

**D9.3 — Expansion order: deterministic, with no fixed-point iteration.**
1. Parse.
2. Expand expression, statement and declaration macros (`#…`). Their inputs are syntax only.
3. Collect package-level declarations and check signatures.
4. Run attached macros (`@…`). They may query the types of declarations that were *not* macro-generated. The declarations they generate are added to the package.
5. Type-check bodies.

- Each attached macro declares the names it adds (Swift-style), so step 3 knows these names before the macro runs.
- Interface checks, constraint checks and package-level variable initializers run after step 4, so they see the generated declarations.
- Declarations generated in step 4 don't trigger further attached macros in the same package. That rules out cycles and makes the result independent of order.

**D9.4 — Execution sandbox.**
- Procedural macros run inside the compiler on a register-based bytecode VM compiled from GIR.
- Execution is deterministic: no syscalls, clock, network or environment.
- VM built-ins (alloc, free, memcpy, panic) work on memory with the target's layout (its type sizes and alignment).
- A call to an `@Extern` function or to a header package inside macro code is a clear compile error.
- The only capabilities come from the `macro` API:
  - create syntax;
  - report diagnostics;
  - query types;
  - read files inside the package directory (`#Embed`). Files read are recorded as build inputs.
- Every expansion has a fuel limit (instruction count) and a memory limit. Exceeding either is a compile error, never a hang.
- LLVM ORC JIT for hot macros is a later optimization.

**D9.5 — Macro packages are ordinary packages.**
- A procedural macro is a declaration exported from any package. Naming across packages follows P01 D1.14 (`#pkg.Name(…)`, `@pkg.Name`).
- Each package produces a "comptime GIR" artifact with every body. It is cached, and it is not part of the API fingerprint. The interpreter runs macros from these artifacts.
- A procedural macro can't be used in the package that defines it.
- There are no plugin dylibs and no build scripts. That means no host/target split when cross-compiling.

**D9.6 — Expansion caching.** Results are stored in the build cache. The key combines:
- the hash of all GIR the expansion executed (the macro and every function it called);
- the hash of the input syntax;
- the hash of any type information it queried;
- the hashes of any files it embedded;
- the build profile name or setting it read (`#Profile()`, `#Setting`, D9.12).

**D9.7 — Tooling sees expansions.**
- `gopp expand <pkg|file>` prints the expanded source.
- Diagnostics inside expansions carry a macro backtrace ("in expansion of `#Assert` at main.gopp:12").
- Debug info maps expanded code back to the invocation site.
- The LSP uses the same expansion (P14).

**D9.8 — Standard macros.**
- Expression and statement macros: `#Assert(cond, msg)`, `#Stringify(expr)`, `#File()`, `#Line()`, `#Embed("path")` (a static, immutable `ref string`), `#Todo()`, `#Unreachable()`, the two profile macros `#Profile()` and `#Setting("name")` (D9.12), and `#Run(f(args))` (D9.13).
- Derives are kept small, because without them:
  - printing works on every type;
  - `==` and map keys work on comparable types;
  - `x.Clone()` works on every cloneable type, and `clone(x)` is its short form (P01 D1.4 rule 6).
- The derives:
  - `@Derive(JSON)` generates `MarshalJSON` and a decode function that builds a new value. It is optional: `encoding/json` works on any type and uses these when they exist (like `easyjson` in Go). There is no `UnmarshalJSON`: decoding always returns a fresh value (`json.Decode[T]`, P13 D13.4), and a missing `@Required` field (P01 D1.4 rule 11, ADR-0016) is an error, as is a missing field of a `@Strict` struct unless it is `Option` or `@Option` (ADR-0019). The macro API reports the effective required bit (`field.Required()`, `type.Strict()`, `field.Option()`, P09.04).
  - `@Derive(Equal)` generates `Equal(o T) bool` for structs that `==` can't compare (they contain slices or maps).
  - No `Clone` derive: a type with `drop` writes `Clone()` by hand on purpose.
- Tests keep Go's `TestXxx(t *testing.T)` convention; there is no `@Test`.

**D9.9 — What macros are not.**
- Not a conditional-compilation mechanism that hides code from the compiler. Use `//gopp:build` and file suffixes for that. The profile macros and `@If` (D9.12) hide nothing: all code is parsed and type-checked in every profile.
- Not a way to invent syntax beyond the `#`/`@` forms. This keeps `gopp fmt`, tree-sitter and third-party tools working on every file.

**D9.10 — Budget (provisional).** `@Derive(JSON, Equal)` on 1,000 structs adds ≤ 200 ms cold and ≤ 20 ms warm.

**D9.11 — Macro errors.**
- A procedural macro declared with `!` (`macro func Name!(…)`) can return an error.
- An error, or a panic inside a macro, becomes a compile error at the use site, with the macro backtrace (D9.7).

**D9.12 — Build profile access: `#Profile()`, `#Setting("name")`, `@If(cond)`.** (ADR-0007; task P09.13.) Profiles are defined in P06 D6.12. All three follow MAC-14: their arguments are known while compiling, and so are their results.

| Form | What it is | Example |
|---|---|---|
| `#Profile()` | a string constant: the name of the **main module's** active profile. It is the same in every package, also in dependencies | `"debug"`, `"release"`, `"fast"` |
| `#Setting("opt")` | a string constant: the value of one setting for the **package being compiled**. The argument must be a string literal naming one of the nine settings; anything else is a compile error that lists them | `"0"`, `"line"`, `"unwind"`, `"address,undefined"`, `"none"` |
| `@If(cond)` | a built-in attribute (MAC-8) on a declaration. `cond` must be a constant `bool` | `@If(#Profile() == "debug")` |

- **Constants.** `#Profile()` and `#Setting(…)` expand (step 2 of D9.3) to string literals, so `#Profile() == "debug"` is a constant `bool` and can be used in `const` declarations and in `@If`. `#Setting` is per package because `deps` rows (P06 D6.12) can give a dependency another `opt` than the main module has.
- **Dead branches stay type-checked.** In `if #Profile() == "debug" { … } else { … }`, the branch that cannot run is removed from the generated code, but it is still parsed and type-checked, so it cannot rot. Imports and variables used only there count as used.
- **`@If(false)` on a declaration** (type, func, method, var or const) keeps the declaration type-checked, but it is not emitted and not exported. A use of it from live code is a compile error that names the `@If`. A use from dead code (a dead branch, or another disabled declaration) is fine. This makes the common pattern work:

```go
@If(#Profile() == "debug")
func dump(s *State) { … }

func step(s *State) {
    if #Profile() == "debug" {
        dump(s)                  // dead in release: removed, but still type-checked
    }
    …
}
```

- **Not for hiding code.** For files that must not even be parsed in some profile (they use an API that only exists there), use `//gopp:build debug` (P06 D6.12).
- **No new syntax.** The forms are `#` and `@` only (D9.9). `@If` on statements is not defined (P01 D1.14).
- **Expansion cache.** An expansion that read the profile name or a setting records it in its key (D9.6), so switching profile re-expands only those packages. `gopp expand` prints the constant.

**D9.13 — `#Run(f(args))`: run a call while compiling.** (Q4, ADR-0011; task P09.14.)
- The argument is one call to a function from **another package** (the D9.5 rule). Its arguments are compile-time values (MAC-14).
- It runs in the interpreter with the D9.4 sandbox and limits. A panic or an exceeded limit is a compile error at the `#Run`, with the backtrace (D9.11).
- A `bool`, numeric or string result is a constant of the function's result type. An array or struct made only of those may initialize a package-level `var`; the value is stored in the binary, and nothing runs at program start. Other result types are an error.
- The checker evaluates it when it checks the declaration (it needs types), not at step 2 of D9.3. The result is cached with the D9.6 key.
- A call without `#Run` never runs at compile time.

```go
import "crc/gen"

var table  = #Run(gen.MakeTable())   // [256]uint32, computed while compiling
const Seed = #Run(gen.Hash("crc32"))   // uint32 constant: usable in case, array sizes, other constants
```

## Procedural macro sketch (the API is finalized in P09.04)

```go
// package derive (std), used as @Derive(Equal)
macro func Equal!(ctx *macro.Ctx, t macro.TypeDecl) []macro.Decl {
    st := t.Struct!()                        // not a struct → returns the error; compile error at the @Derive site (D9.11)
    x, o := ctx.Fresh("x"), ctx.Fresh("o")   // hidden names (D9.2)
    fieldEqual := macro.Ref(ctx, "derive.FieldEqual") // == for comparable fields, element by element for slices and maps

    var body []macro.Stmt
    for _, f := range st.Fields() {
        body = append(body, macro.If(
            macro.Not(macro.Call(fieldEqual, macro.Sel(x, f.Ident()), macro.Sel(o, f.Ident()))),
            macro.Block(macro.Return(macro.False())),
        ))
    }
    body = append(body, macro.Return(macro.True()))

    return []macro.Decl{macro.Method(
        macro.Recv(x, t.Type()),
        macro.Ident(ctx, "Equal"),           // visible at the use site, declared by @Derive (D9.3)
        macro.Params(macro.Param(o, t.Type())),
        macro.Results(macro.BoolType()),
        macro.Block(body...),
    )}
}
```

## Deliverables

- `docs/design/macros.md`.
- Declarative expansion, the GIR interpreter, and the `gopp/macro` API.
- Procedural and attached macros.
- The standard macros, and `@Derive(JSON, Equal)`.
- The expansion cache, `gopp expand`, macro backtraces, and debug-info mapping.
- `#Profile()`, `#Setting("name")` and `@If` (D9.12).
- `#Run` (D9.13).

## Exit criteria

- M6: the standard macros and `@Derive(JSON, Equal)` work, and a P09 test package uses them. (The `encoding/json` output check is in P13.08.)
- The hygiene suite passes (capture and shadowing, in both directions).
- No-op rebuilds hit the expansion cache ≥ 95% of the time.
- The D9.10 budget is met, or an ADR documents the gap.

## Risks

- **Interpreter performance** → caching, fuel limits, JIT later.
- **Complexity of attached macros that query types** → the strict ordering of D9.3.
- **Tooling breakage** → `gopp expand` and LSP integration from the start.

## Open questions

- **Q1** (decided 2026-10-27) No `#Try` macro: `call!()` already returns the error inside functions that can fail (P01 D1.8).
- **Q2** (decided 2026-10-27, ADR-0011) Procedural macros build syntax with one constructor per node (`macro.If`, `macro.Call`, …). No format-string builder and no quotes in v0; `#Quote` may come in v1 as a layer on top (D9.1).
- **Q3** (decided 2026-10-27, ADR-0011) No macros where a type goes in v0; use a declaration macro that declares a named type (D9.1). (Moved here from P02 Q2.)
- **Q4** (decided 2026-10-27, ADR-0011) Yes, explicitly: `#Run(f(args))` runs one call from another package while compiling; nothing runs automatically (D9.13).
