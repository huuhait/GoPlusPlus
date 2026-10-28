# Error recovery tests (P02.07)

Each `*.test` file here is a self-golden diagnostics test: the CHECK header is
the exact `gopp parse` output for the source below it (so positions are
positions in the file). Regenerate after a deliberate change with

    tests/update-goldens diag tests/parse/recovery/<name>.test

and read `git diff` before you commit it. `fixits-apply.test` applies every
fix-it of the table (`gopp parse -fixit-stdout`, and `-fixit` in place) and
checks that the fixed text parses without a diagnostic: a fix-it is offered
only where the fix is exact.

## The tailored messages

One table for all of them. Every row has its own entry in
`compiler/include/gopp/Basic/DiagnosticIDs.def` (each also has a numbered error
code, D3.11: `gopp explain E0087`). "Rule" rows are valid Go that is always an
error in Go++; they are reported only with `ParseOptions::CheckLanguageRules`
(the default; `gopp parse -language-rules=false` and `gopp fmt` turn it off,
so that Go sources can be read and such code can be formatted). None of the
messages fires on valid Go or valid Go++ (`while`, `let` are ordinary names; `while!(x)` is an unwrapping call): of the 7354 files of `$GOROOT/src` (Go 1.25.14,
`gopp parse -language-rules=false`), the parser rejects exactly the 44 that
gofmt rejects too (test data with syntax errors on purpose, an empty file) and
the 2 that use the Go++ words `macro` and `ref string` as names.

| # | Mistake | Message | Fix-it | Diagnostic ID | Test |
|---|---------|---------|--------|---------------|------|
| 1 | `&T{…}` (rule, OWN-P4) | allocation is explicit in Go++: use new_unique(T{...}) instead of &T{...} | `&` -> `new_unique(`, `)` after `}` | err_addr_of_complit | addr-of-complit |
| 2 | `func f!() error` and every `!` form (rule, ERR-15) | '!' already adds the error result: remove 'error' from the result list | remove the result | err_fails_returns_error | fails-returns-error |
| 3 | `u := u`, `var u = u`, a closure parameter `u` inside the loop of `u` (rule, RNG-8) | 'u' redeclares a loop variable: give the copy another name, plus a note at the loop variable | none (the spec forbids reusing the name) | err_loop_var_redeclared | loop-redeclare |
| 4 | `//gopp:cflags`, `//gopp:cxxstd`, `//gopp:ldflags` in a source file (rule, LEX-11) | '//gopp:cflags' is not allowed in a source file: move it to gopp.mod, plus a note with the gopp.mod line | none (deleting the line would lose the flags) | err_gopp_directive_in_source | gopp-directive |
| 5 | `func f(a, move b int)` | 'a' needs its own type: a 'move' parameter does not share its type with the names before it (the tree gives `a` the type: no BadExpr) | insert the type after `a` (none when the type is `...T`) | err_move_type_not_shared | move-after-names, ../move-type-not-shared-ast |
| 6 | `s view string`, `view []T` | there is no 'view' type: a borrowed string or slice is spelled 'ref' | `view` -> `ref` | err_view_for_ref | view-own |
| 7 | `own s T`, `owned s T` | there is no 'own' mark: a parameter that takes ownership is marked 'move' | `own` -> `move` | err_own_for_move | view-own |
| 8 | `ref int`, `ref *T` | 'ref' applies to 'string' and slices only | none | err_ref_bad_elem | ref-bad-elem |
| 9 | `x := 1` at the top level | ':=' is not allowed outside a function: use 'var x = ...' | insert `var `, `:=` -> `=` | err_define_at_top_level | top-level-define |
| 10 | `println("hi")`, `if x {…}`, `x = 1` at the top level | non-declaration statement outside function body (Go's message; the statement is skipped as a whole) | none | err_stmt_outside_func | top-level-stmt |
| 11 | `}` newline `else` | 'else' must be on the same line as the closing '}' | join the lines | err_else_on_new_line | else-new-line |
| 12 | `for …` newline `{` | unexpected newline, expecting { after for clause | join the lines | err_newline_after_for_clause | for-newline-lbrace |
| 13 | `if x == 1 return 0`, `if x > 0` newline `return`, `for … i++ g()` | expected '{' after if condition, found 'return' (the statement becomes the body) | none | err_expected_lbrace_after | missing-lbrace, for-newline-lbrace |
| 14 | `if x = 1 {`, `for z = 1`, `switch w = 2` | cannot use an assignment as a condition: did you mean '=='? (`:=`: cannot use a short variable declaration ...) | `=` -> `==` | err_assign_in_condition, err_define_in_condition | assign-in-condition |
| 15 | `var x := 1`, `const c := 1` | ':=' is not allowed in a var declaration: use '=' | `:=` -> `=` | err_define_in_var_decl | var-define |
| 16 | `import ("a", "b")` | imports are separated by a newline or ';', not by ',' | `,` -> `;` | err_comma_between_imports | other-languages |
| 17 | `'hello'` | single quotes make a rune: use double quotes for a string | both quotes -> `"` | err_lex_single_quote_string | single-quote |
| 18 | `a ? b : c` | Go++ has no '?:' operator: use an if statement | none | err_lex_ternary | single-quote |
| 19 | `x = i++`, `y := ++j` | '++' is a statement in Go++, not an expression | none | err_incdec_expression | incdec-expression, prefix-incdec |
| 20 | `++i` as a statement (also the post statement of a for clause) | Go++ has no prefix '++': write it after the operand | `++i` -> `i++` | err_prefix_incdec | prefix-incdec |
| 21 | `while x < 3 {`, `while (x) {`, `while !x {`, `while !(x) {` (not `while(x)`, `while!(x)`, `while !(x)` newline: calls of a function named `while`, see while-bang-call) | Go++ has no 'while': use 'for' with a condition | `while` -> `for` (the parse goes on as `for`) | err_while_loop | habits |
| 22 | `for x in xs`, `for i, x in xs` | Go++ has no 'for ... in': write 'for _, x := range ...' | `x in` -> `_, x := range`; `x in range(…)` -> `x := range(…)` (the integer range keeps its parentheses) | err_for_in | other-languages, for-in-range |
| 23 | `} elif x {`, `elsif`, `elseif` | Go++ has no 'elif': write 'else if' | word -> `else if` | err_elif | else-new-line |
| 24 | `function f()`, `fn f()`, `def f()`, `fun f()` at the top level | functions are declared with 'func', not 'fn' | word -> `func` | err_fn_keyword | habits |
| 25 | `let x = 1`, `auto x = 1`, `val x = 1` | Go++ has no 'let': declare with 'var x = v' or 'x := v' | word -> `var` | err_let_decl | habits |
| 26 | `int x = 5`, `string s`, `x int` (two names start a statement) | 'int x' is not a declaration: write 'var name Type' or 'name := value' | `var x int` when a predeclared type shows which word is the type | err_decl_without_var | other-languages |
| 27 | `struct S { … }`, `class C { … }` at the top level | Go++ has no 'class' declaration: write 'type C struct { ... }' | `class C` -> `type C struct` | err_struct_decl | habits, other-languages |
| 28 | `func Map<T any>(…)` | type parameters use square brackets: 'func Map[T any](...)' | `<`, `>` -> `[`, `]`; every parameter gets its own constraint: `<T>` -> `[T any]`, `<T, U extends C>` / `<T, U: C>` -> `[T any, U C]` (C is one name or `pkg.Name`); none for other forms (keywords, `in`/`const`/`super`, predeclared type names, longer constraints) | err_angle_type_params | other-languages |
| 29 | `func f() -> int` | there is no '->' before the result type: write it after the parameters | remove `->` | err_arrow_before_result | other-languages |
| 30 | `func f(x: int)`, `a: int` in a struct, `var v: int` | no ':' between a name and its type: write 'x T' | remove `:` | err_colon_before_type | other-languages |
| 31 | `var a int[]` | array and slice brackets go before the element type: write '[]int' | `int[]` -> `[]int` | err_brackets_after_type | other-languages |

go/parser's messages that got a fix-it: `func f()` newline `{` (unexpected
semicolon or newline before {, join the lines; `newline-lbrace`), `if x` newline
`{` (unexpected newline, expecting { after if clause, join the lines), `for i,
&v = range` (`=` -> `:=`) and `for &i, v := range` (remove the `&`;
`range-alias`), `macro lower(...)` (`lower` -> `Lower`).

Not parser-detectable (a valid Go reading, so P03 reports them):

- `move a, b T` (list form): Go reads it as the parameters `(move a)` and `(b T)`
  (05-parameters.md); `func f(move Move, b Board)` is valid Go, so the parser
  cannot tell. P03 sees that `a` is not a type.
- `new(T)` (LEX-8, undefined `new`) and `*T` fields that should be `Unique[T]`
  need name resolution.
- `switch x` newline `{` is valid Go (`switch x; {`), so it is not reported.

A fix-it never deletes a comment (`fixit-keeps-comments`): where the text it
replaces holds a comment (rows 2, 22, 26, 27, 29), the error is reported
without the fix-it. The join-lines fix-its (rows 11, 12 and the like) only
replace blank gaps.

## Sync points

Recovery follows go/parser: after an error the parser skips to a token in a
sync set (`StmtSync`: `;`, `}`, the statement keywords and `func`, `macro` at
the top level; `DeclStart` for declarations; `ExprEnd` inside types) and
`advance` always makes progress. P02.07 added:

- a list (parameters, arguments, type arguments, composite literal elements)
  stops at a token that cannot continue it (`}` of an enclosing block, a
  statement keyword, `func name`, `type`, a `{` after a missing `)`): one
  "expected ')'" error instead of reading the rest of the file as elements,
  and the offending token is not consumed (`sync-lists.test`);
- a `{` after a bad operand in an `if`/`for`/`switch` header is kept as the
  body (`sync-statements.test`);
- an `if`/`for` body without `{` takes the next statement as its body;
- a statement at the top level is parsed and dropped as a whole, so that one
  misplaced line does not hide the declarations after it;
- a misspelled word is turned into the keyword it stands for (`while`, `fn`,
  `let`, `elif`, `for x in`) and the parse goes on as if it were right;
- a node that starts at a token the parser did not consume (recovery at the
  end of the file) is empty, never reversed, and no node ends past the file
  (found by the fuzzer).
