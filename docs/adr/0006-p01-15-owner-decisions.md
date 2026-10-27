# ADR-0006: P01.15 owner decisions that changed the plan

- **Status:** accepted
- **Date:** 2026-10-27
- **Plan reference:** P01 D1.2, D1.4, D1.5, D1.6, D1.7, D1.8, D1.9, D1.10, D1.11, D1.12, D1.14, D1.15, D1.21. Also P05 D5.4, P07 D7.3 and D7.9, P08 D8.1, D8.3 and D8.6, P10 D10.2, P04 D4.6 (D17), P09 (D22, D23) and P12 D12.5 and D12.7 (D22).
- **Related:** ADR-0003 (memory model) and ADR-0004 (`!`) were updated to the final text. The full list of decisions D1 to D25, each with an example, is in `plans/2026-10-27-goplusplus-language/01-language-spec/STATE.md` ("P01.15 owner decisions").

## Context

In P01.15 the owner reviewed all 42 `Open (P01 review)` notes of the spec and
decided each one. Most notes were accepted as written. Fifteen decisions
change the plan itself (the keywords, a rule, or a phase decision), so the plan
rule "any change to a decision needs an ADR" (D0.9) applies. This ADR records
them in one place. Decisions that only confirm a reading in the spec (D1, D3,
D8, D10, D12, D13, D14 in part, D16, D21) need no ADR; D14 and D16 are
mentioned here only where they extend D7.

Where two decisions conflicted, the later one won: D19 replaces D18's form, and
D11 updates D1's `own ...T` to `move parts ...T`.

## Decision

### D2: a map insert during `range` always panics
Before: the "map grew during iteration" panic fired only when the table had to
grow (P05 D5.4, P01 D1.21), so it depended on capacity and was not repeatable.
Now: inserting a **new key** into a map while any `range` over that map runs
always panics, whether or not the table has room ("map insert during
iteration"). Writing an existing key is not an insert. Inserts the compiler can
see stay compile errors. Changes P05 D5.4, D1.21, RNG-4.

### D4: `x.Clone()` is the copy method; `clone(x)` is the short form
Before: `clone(x)` was a built-in function. Now every cloneable type has a
method `Clone() T`, built-in types too (`string`, `[]T`, `map`, `Unique[T]`,
numbers). `clone(x)` stays as a short form that calls it, and `move(x)` stays a
built-in. A type whose `Clone` method has another signature is not cloneable
(compile error). Examples and fix-its use `x.Clone()`. A hand-written `Clone` has a value receiver. `Unique[T]` and `Shared[T]` keep their own `Clone` when `T` also has one (same tie-break as `IsNil` on these kinds, D9): `u.Clone()` deep-clones, `s.Clone()` retains. On a raw `*T` where `T` has a value-receiver `Clone() T`, `p.Clone()` is a compile error for ambiguity (fix-its: `(*p).Clone()` copies the T, `q := p` copies the pointer); without a `T.Clone` it copies the pointer. A `Clone` with another signature is legal but not cloneable. Changes D1.4 rule 6.

### D5: `view` is renamed `ref`; `@View` is renamed `@Ref`
`ref` is a contextual keyword: it is special only where a type is expected, so
`ref := "main"` still works. Only `macro` stays a reserved new keyword. Changes
D1.2 and every chapter. Rule IDs (`OWN-V*`) are unchanged.

### D6: `case string:` also matches a `ref string`
Before: `case string:` did not match a `ref string`; `case ref string:` was
needed. Now `case string:` matches both (and `case []T:` matches `ref []T`).
The bound value is a borrow either way. `case ref string:` is still allowed
when the difference matters; it must come before `case string:` to be reached.
Changes D1.12 and TYP-9.

### D7: the `&d` range form
New form `for _, &d := range xs`: `d` is the element itself, so writes change
the collection. It works for slices, arrays, map values (`for k, &v := range m`),
C++ containers (D14) and iterators that yield `*T` (D16). The key can't use
`&`. The plain form stays read-only for owning elements, and its error message
suggests `&d` first. `move(v)` is an error on a plain range variable; through
`&v` it takes the value out and leaves the zero value (D13). Growing the
collection in the loop stays a compile error. Changes D1.21, RNG-1, RNG-1a, the
`RangeClause` grammar, and OWN-R3b.

### D9: `IsNil()` on every pointer kind
Raw `*T`, `Unique[T]`, `Shared[T]` and `Weak[T]` all have `IsNil() bool`.
`p == nil` keeps working. For `Weak[T]` it is true when there is no target.
New rule OWN-P9; changes D1.7.

Follow-up decision (the tie-break): if `T` has its own `IsNil` method, then
- on `Unique[T]`, `Shared[T]` and `Weak[T]` the pointer kind's `IsNil` wins; call `T`'s as `(*p).IsNil()`.
- on a raw `*T` (method declared on `T` or on `*T`), `p.IsNil()` is a compile error for ambiguity. Fix-its: `(*p).IsNil()` calls T's method, `p == nil` checks the pointer.
- if `T` has no `IsNil`, `p.IsNil()` checks the pointer.

Why: in Go, `p.IsNil()` on a `*Opt` calls `Opt`'s method. Letting the
pointer's check win would silently change what copied Go code does (`true` vs
`false`, and a nil `p` that panics in Go would return `true`). Go has no
`Unique`/`Shared`/`Weak`, so nothing changes silently there. This matches the
raw `*T` `Clone` rule (D4, OWN-R6).

### D11: `own` is renamed `move` and goes before the parameter name
`func (l *List) Push(move s string)`, `func (move b Builder) Finish() string`,
`func Join(move parts ...string)`. Parameters without it still borrow (D1.5).
An owned local passed to a `move` parameter moves automatically; a field,
element or map value needs `move(x)` or `x.Clone()`. A variadic parameter of an
owning type must be `move ...T` (D1). `&` and `*` keep their Go meanings.
Because `move(x)` is also a built-in function, `move` is contextual, not
reserved. Changes D1.2, D1.4, D1.5, D1.7 and every chapter.

### D15: a `!` function cannot declare `error` as its last result
Before: `func f!() error` was allowed and `return e` was read by operand count.
Now `func check!() error` is a compile error ("`!` already adds the error;
remove it"). `func check!()` returns only an error. To return an error as a
normal value, use the Go form `func check() (error, error)`. `x!()` follows
the innermost function (accepted). New rule ERR-15; changes D1.8 and ADR-0004.

### D17: `defer` in a loop works like Go
Before: `defer f.Close()` on a loop-body variable was a compile error. Now it
is legal. The loop-body variable's lifetime is extended (nothing is moved or zeroed)
until its deferred call has run at function end, so the deferred calls run at function
exit exactly as in Go. This also covers a borrowed argument and a captured
variable of a deferred function literal. `&x` written directly in the `defer`
statement also counts as a use (`defer unlock(&mu)` in a loop is legal). A pointer
stored earlier (`p := &mu; defer unlock(p)`) does not extend `mu`; if `mu` dies
first, that is UB-1. Borrowed variables keep the error. `@Pinned` locals can be extended too, because nothing moves. The extended local gets its own storage per block execution, is kept alive by the defer record (if the `defer` did not run, it dies with its block, with no extra drop flag), and is destroyed at function exit after all deferred calls, in reverse order of creation (for `main`, after the other goroutines stop). Captured
variables live as long as the closure, and a closure stored in an outer
variable keeps its captures alive (accepted). Changes D1.9, PAN-3, CLO-3b.

### D19: `Clone` and `Copy` are built-in constraints
`[T Clone]` means `T` has `Clone() T` (every cloneable type, numbers
included). `[T Copy]` means `T` copies for free, like numbers. They replace
`clonable` and `trivial`. They are written like Go's `comparable`; they embed
in constraint interfaces, and Go's own rule keeps them out of `|` unions. This
replaces D18's form `[T Clone[T]]`. Changes D1.10, GEN-1, GEN-2.

### D20: no redeclaring a loop variable inside its loop
Declaring a variable with the same name as a loop variable inside that loop's
body (`u := u`, `u := u.Clone()`) is a compile error, not just a vet warning.
Old Go code that used `u := u` must rename. A closure that runs later can't
capture a borrowed loop variable; fix-its never reuse the name
(`go fetch(u.Clone())`, `mine := u.Clone()`, `range move(users)`). New rule
RNG-8.

### D22: channel lifetime, `sync.Scope`, field attributes, block arguments
A channel and its buffered values are freed automatically when nobody holds the
channel. The marker that lets `sync.Scope` goroutines borrow is designed in P12.
Attributes are allowed on struct fields in v0, written after the field's type
and before any Go tag (``Name string @JSON("name") `db:"name"` ``). A `block`
macro argument is written like a func literal, `#Repeat(3, func() { … })`;
`return`, `break` and `continue` inside it are compile errors because the code
is pasted inline. How `stmt`, `type` and `tokens` arguments are written stays
with P09. Changes D1.14, CON-4, CON-9, MAC-12, MAC-13.

### D23: macro arguments
Both macro kinds stay (D1.14). `@Name(args)` on top of a declaration receives
its arguments plus the declaration and returns new code. `#Name(args)` expands
in place and passes a code block as `func() { … }`. Arguments that a macro
reads as values (every attribute argument, and `#` parameters of a value kind)
must be known while compiling, so `@Retry(n)` with a runtime variable is a
compile error. Code-fragment parameters (`expr`, `ident`, `block`, `stmt`) are
pasted into the program and may name runtime variables, so
`#Assert(x > 0, "bad")` is fine. The exact procedural-macro API is designed in
P09. New rule MAC-14; changes D1.14.

### D24: C/C++ settings move from source files to `gopp.mod`
`//gopp:cflags`, `//gopp:cxxstd` and `//gopp:ldflags` are removed. The
module-wide directives `cxxstd c++20`, `cflags …` and `ldflags …` in gopp.mod
replace them, set once per project like Go's `go.mod`. cdeps keep taking their
flags from their own CMake target (P11). `//gopp:build` stays, like Go's
`//go:build` (above `package`). Changes D1.15, FOR-9, FOR-10, P07 D7.3 and
D7.9, P08 D8.1, P10 D10.2.

### D25: returning a `ref` into a local is a compile error
Before: returning a ref of a local was undefined behavior that was not caught.
Now returning a `ref` into a local of the same function, or into an owned value
that dies at the return, is a compile error, like Rust. A ref into a borrowed
parameter is fine. Less obvious cases (a ref that comes out of a call) stay
undefined behavior and `-asan` finds them. New rule OWN-V7; changes D1.4 rule 8
and UB-1.

## Consequences

- The spec, the plan READMEs, TODO.md, the samples and ADR-0003/0004 now use
  `ref`, `move` before the name, `x.Clone()`, `[T Clone]`/`[T Copy]` and the
  gopp.mod directives.
- P02 (parser) must handle two contextual words (`ref`, `move`), the `&` mark in
  range clauses, attributes after a field's type, and func-literal block
  arguments. P03 gets new checks: owning variadics, the loop-variable
  redeclaration error, `error` as the last result of a `!` function, returning a
  `ref` into a local, and `Clone()` method lookup.
- P05 changes the map runtime: the iterator count now panics on every new-key
  insert while an iterator is active.
- P10 adds three gopp.mod directives; P07 and P08 read their flags from there.
- Some Go code needs changes: `u := u` must be renamed, writes to a plain range
  variable need `&d`, and a parameter named `ref` of type `string` or a slice
  must be renamed.
- Deferring in a loop keeps Go's behavior and Go's trap (many open files until
  the function returns). Code that wants early close must not defer.
- Codegen (P04) gives defer-extended locals per-execution storage owned by the
  defer record, which skips the block-end drop; the record is a runtime
  allocation.

## Alternatives

- **D2:** panic only when the table must grow (the earlier draft). Rejected:
  the result depends on table capacity, so it is not repeatable.
- **D4:** only the method `x.Clone()`, or only the function `clone(x)`. The owner chose the method as the main form (it works in generic code and for built-in types) and kept `clone(x)` as a short form.
- **D6:** `case string:` does not match a `ref string` (the old D1.12). Changed by the owner: `case string:` matches both, and `case ref string:` is still allowed.
- **D9:** only `p == nil`, or only `IsNil()`. The owner chose both: `== nil` keeps working as in Go, and `IsNil()` exists on every pointer kind.
  Also rejected (tie-break): "the pointer's `IsNil` always wins, even on a raw `*T`", because it silently changes the meaning of copied Go code.
- **D15:** allow `func f!() error` and read `return e` by operand count (the earlier draft). Changed by the owner: it is a compile error, to remove the ambiguity.
- **D5/D11:** a Rust-style default, where a plain parameter takes ownership and
  `ref` or `&` borrows. Discussed and rejected: it puts a mark on most
  functions and reuses Go's `&` and `*` signs.
- **D7:** a writable borrow in the plain loop. Rejected: it would silently
  change the collection. The plain form stays read-only, like Rust's
  `for d in &docs`, and the `&d` mark makes the intent visible at the loop.
- **D19:** `Clone[T]` as a normal std interface (`[T Clone[T]]`, D18). Replaced
  by the `comparable`-style `[T Clone]`. The short form `[Clone[T]]` was
  rejected because it is not Go syntax (`go vet`: "missing type parameter name
  or invalid array length").
- **D17:** keep the compile error and let the destructor close each file at the
  end of its iteration (the earlier draft). Changed by the owner to follow Go
  ("like Go").
- **D20:** keep `u := u` as a vet warning only (the earlier draft). Changed by
  the owner: it is now a compile error.
- **D22:** an explicit `ch.Destroy()`. Rejected: forgetting it leaks, and
  calling it early crashes. Use `sync.Scope` when goroutines must finish first.
- **D23:** require every macro argument to be a compile-time value. Rejected: it would forbid `#Assert(len(data) > 0, …)`; the rule applies only to arguments read as values.
- **D24:** keep the directives in source files (the earlier draft). Changed by
  the owner: C/C++ settings are set once per project in gopp.mod, like
  `go.mod`.
- **D25:** leave it undefined behavior, with an optional vet warning (the
  earlier draft). Changed by the owner: it is a compile error, like Rust.
