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
  - Receives syntax (plus read-only type information, for attached macros) and returns syntax built with the `gopp/macro` API.
- **Invocation:**
  - `#Name(args)` for expression, statement and declaration macros.
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
- the hashes of any files it embedded.

**D9.7 — Tooling sees expansions.**
- `gopp expand <pkg|file>` prints the expanded source.
- Diagnostics inside expansions carry a macro backtrace ("in expansion of `#Assert` at main.gopp:12").
- Debug info maps expanded code back to the invocation site.
- The LSP uses the same expansion (P14).

**D9.8 — Standard macros.**
- Expression and statement macros: `#Assert(cond, msg)`, `#Stringify(expr)`, `#File()`, `#Line()`, `#Embed("path")` (a static, immutable `ref string`), `#Todo()`, `#Unreachable()`.
- Derives are kept small, because without them:
  - printing works on every type;
  - `==` and map keys work on comparable types;
  - `x.Clone()` works on every cloneable type, and `clone(x)` is its short form (P01 D1.4 rule 6).
- The derives:
  - `@Derive(JSON)` generates `MarshalJSON`/`UnmarshalJSON`. It is optional: `encoding/json` works on any type and prefers these methods when they exist (like `easyjson` in Go).
  - `@Derive(Equal)` generates `Equal(o T) bool` for structs that `==` can't compare (they contain slices or maps).
  - No `Clone` derive: a type with `drop` writes `Clone()` by hand on purpose.
- Tests keep Go's `TestXxx(t *testing.T)` convention; there is no `@Test`.

**D9.9 — What macros are not.**
- Not a conditional-compilation mechanism. Use `//gopp:build` and file suffixes.
- Not a way to invent syntax beyond the `#`/`@` forms. This keeps `gopp fmt`, tree-sitter and third-party tools working on every file.

**D9.10 — Budget (provisional).** `@Derive(JSON, Equal)` on 1,000 structs adds ≤ 200 ms cold and ≤ 20 ms warm.

**D9.11 — Macro errors.**
- A procedural macro declared with `!` (`macro func Name!(…)`) can return an error.
- An error, or a panic inside a macro, becomes a compile error at the use site, with the macro backtrace (D9.7).

## Procedural macro sketch (the API is finalized in P09.04)

```go
// package derive (std), used as @Derive(Equal)
macro func Equal!(ctx *macro.Ctx, t macro.TypeDecl) []macro.Decl {
    st := t.Struct!()                                    // not a struct → returns the error; compile error at the @Derive site (D9.11)
    b := macro.NewFuncBuilder(ctx, t, "Equal", "func(o "+t.Name()+") bool")
    for _, f := range st.Fields() {
        // derive.FieldEqual: == for comparable fields, element by element for slices and maps
        b.Line(`if !derive.FieldEqual(x.%s, o.%s) {`, f.Name(), f.Name())
        b.Line(`    return false`)
        b.Line(`}`)
    }
    b.Line(`return true`)
    return b.Decls()
}
```

## Deliverables

- `docs/design/macros.md`.
- Declarative expansion, the GIR interpreter, and the `gopp/macro` API.
- Procedural and attached macros.
- The standard macros, and `@Derive(JSON, Equal)`.
- The expansion cache, `gopp expand`, macro backtraces, and debug-info mapping.

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
- **Q2** Quoting API: a builder with format strings (as sketched) or syntax quasi-quotes (`macro.Quote { … $x … }`)?
- **Q3** Should macros be allowed in type position (`var x #Vec(int)`)? Proposed: not in v0. (Moved here from P02 Q2.)
- **Q4** Compile-time function evaluation for constants (`const N = #Run(f())`)?
