# ADR-0004: Semantics of `!` (fallible functions and unwrapping calls)

- **Status:** accepted. Updated to the final text in P01.15 (the rules are in `docs/spec/09-errors-and-panics.md`; the change to `error` results is in ADR-0006).
- **Date:** 2026-10-27
- **Plan reference:** P01 D1.8

## Context

Go's `if err != nil { return ..., err }` is verbose, but Go++ must keep Go's
error values and the familiar `val, err := call()` form working unchanged. We
want a short form for the common "pass the error up" case without exceptions or
hidden control flow that costs more than the hand-written code.

## Decision

- **Go error handling is unchanged.** `val, err := call()` plus
  `if err != nil` works exactly as in Go.
- **`func name!(...) T` declares a function that can fail.** It is the same
  function as `func name(...) (T, error)`; callers see `(T, error)`. The `!`
  sits right before the parameter list, for functions, methods, interface
  methods, literals (`func!(x int) T {...}`) and function types.
- **A `!` function cannot declare `error` as its last result.** The `!` already
  adds the `error` result, so `func check!() error` is a compile error ("`!`
  already adds the error; remove it"). `func check!()` returns only an error.
  To return an error as a normal value, use the Go form
  `func check() (error, error)`. This also removes the `return e` ambiguity:
  as many operands as declared results is a success, one more is a failure.
- **`call!(args)` unwraps a call whose last result is `error`.** The value is
  the remaining results. On error, the innermost function around the call
  decides (a function literal is judged by its own signature):
  - inside a function that can fail (declared with `!`, or whose last result
    is `error`), the error is returned to the caller with zero values for the
    other results, and the `!` site is added to the error's trace (the error
    is never wrapped);
  - anywhere else (`main`, a goroutine body, a function without an error
    result, a plain closure inside a `!` function), it panics with an
    `errors.MustError` value.
- **Same behavior as `return <zero values>, err`**: named results are set, then
  deferred calls run.
- **Libraries do not panic on ordinary errors.** `gopp vet` flags a `!` that
  would panic outside package `main`, and warns about `!` on `(n, err)` calls
  such as `Read` and `Write`.
- **Only for calls.** Map lookups and channel receives keep the comma-ok form.
- **Cost.** One compare and a branch to a cold block, the same code as the
  hand-written check; the trace entry is written only on the error path.

## Consequences

- Error handling reads like Go but is much shorter in the common case.
- `errors.MustError` and error traces become part of the standard library
  (P13) and runtime.
- The lexer and parser need the extra grammar rule
  `PrimaryExpr "!" Arguments` (P02), and P03 needs the check that rejects
  `error` as the last result of a `!` function.
- `defer f!()` and `go f!()` need the special rules listed in D1.8.

## Alternatives

The plan text for D1.8 does not list rejected alternatives. The ones below are
the obvious options this design avoids; they are not recorded owner decisions.

- **Exceptions:** hidden control flow and a runtime cost; not Go-like.
- **A `Result[T]` type or Rust-style `?` operator:** changes every signature
  and breaks Go code and interop with Go-style APIs.
- **Do nothing (keep only `if err != nil`):** keeps Go's verbosity, which the
  language wants to reduce.
- **Allowing `func f!() error` and reading `return e` by operand count:**
  the earlier spec draft did this. The owner chose to forbid the form in
  P01.15 instead, because it was ambiguous (ADR-0006).
