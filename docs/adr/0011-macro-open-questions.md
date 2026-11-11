# ADR-0011: Macro open questions (building syntax and type position)

- **Status:** accepted (compile-time function evaluation is unified under `@Const` and composite `const` in [ADR-0033](0033-const-functions-and-composite-constants.md))
- **Date:** 2026-10-27
- **Plan reference:** P09 Q2, Q3 (task P09.11); P09 D9.1, D9.4, D9.5, D9.8; P02 Q2 (moved to P09 Q3); spec MAC-2, MAC-6.

## Context

P09 left two macro surface questions open:

1. **How a procedural macro writes the code it returns.** The P09 sketch used a builder that takes format strings (`b.Line("if !derive.FieldEqual(x.%s, o.%s) {", …)`). Two other options were syntax quasi-quotes (`#Quote(… $x …)`, like Rust's `quote!`) and building the syntax tree with one constructor per node (`macro.If(…)`, `macro.Call(…)`).
2. **Macros where a type goes** (`var x #Bits(12)`, `func load() #Tuple(int, string)`). P02 deferred this to P09.

## Decision

1. **Constructors only in v0.** The `gopp/macro` API builds syntax with one constructor per node: `macro.If`, `macro.Call`, `macro.Sel`, `macro.Return`, `macro.Method`, `macro.Param`, and so on. A macro is ordinary Go++ code that calls these. There is no format-string builder and no quote syntax in v0, so a macro's output is always a well-formed tree, and the macro itself is type-checked like any other code.
   - Names keep D9.2 hygiene: `ctx.Fresh("x")` makes a hidden name, `macro.Ref(ctx, "derive.FieldEqual")` names something where the macro is defined, and `macro.Ident(ctx, "Equal")` is the explicit way to break hygiene.
   - Quasi-quotes (`#Quote`) can come in v1 as a layer on top of these constructors. They would not change the constructor API.
2. **No macros where a type goes, in v0.** `#Name(…)` stays allowed only where an expression, a statement or a declaration goes (MAC-2). A `#` call where a type goes is a parse error, and the hint says to use a declaration macro that declares a named type (`#Gen(…, User)`, then `var u User`). A `type` fragment as a macro *argument* (`#SizeOf(int)`) is unaffected. Today this is a parse error, so allowing it in v1 breaks no code.
3. **Compile-time function evaluation uses `@Const` functions and composite `const` declarations ([ADR-0033](0033-const-functions-and-composite-constants.md)), not a macro.**

## Consequences

- P09.04 designs constructors instead of a quoting form. Writing a macro is longer than with quotes, but needs no new syntax; `gopp fmt`, tree-sitter and the LSP stay as they are.
- The std macros (`@Derive(JSON)`, `@Derive(Equal)`) are written with the constructors.
- The parser and every tool can rely on a type always being written out, never computed by a macro.

## Alternatives

- **Q2: a builder with format strings** (the old sketch). Rejected: the output is text, so mistakes in it are found only when someone uses the macro, and the editor doesn't treat it as code.
- **Q2: quasi-quotes in v0** (`#Quote(… $x …)`). Not now: it needs a quoting form, `$` holes and hygiene rules for quoted code. It is kept as a possible v1 layer.
- **Q3: allowing macros where a type goes.** Rejected for v0: the type isn't visible when you read the code; the editor would have to run macros to show a type; two identical calls raise the question of whether they make the same type; and D9.3 would need a new expansion step with possible cycles. The real uses are rare, and declaration macros cover them.
