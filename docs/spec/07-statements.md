# 7. Statements

Base: Go spec [Statements](https://go.dev/ref/spec#Statements), [Assignment statements](https://go.dev/ref/spec#Assignment_statements), [Short variable declarations](https://go.dev/ref/spec#Short_variable_declarations), [If statements](https://go.dev/ref/spec#If_statements), [Switch statements](https://go.dev/ref/spec#Switch_statements), [For statements](https://go.dev/ref/spec#For_statements), [Return statements](https://go.dev/ref/spec#Return_statements), [Send statements](https://go.dev/ref/spec#Send_statements), [Select statements](https://go.dev/ref/spec#Select_statements), [Go statements](https://go.dev/ref/spec#Go_statements), [Defer statements](https://go.dev/ref/spec#Defer_statements), [Break statements](https://go.dev/ref/spec#Break_statements), [Continue statements](https://go.dev/ref/spec#Continue_statements), [Goto statements](https://go.dev/ref/spec#Goto_statements).

Statement syntax is unchanged. Only the effects on ownership are new. `range` is in [chapter 8](08-slices-and-range.md), `defer` and `return` in `!` functions are in [chapter 9](09-errors-and-panics.md), `go` and `select` are in [chapter 12](12-concurrency.md).

## 7.1 Rules

**STM-1.** *Assignment moves.* `a = b` where `b` is an owned local moves `b` into `a`. The old value of `a` is dropped first (after the right side is evaluated). `a = a` is a no-op.

> **Open (P01 review):** the plan does not give the order for assigning an owned value (STM-1) or the effect of `_ = f()` (STM-9). This spec reads them with [OWN-R1](04-ownership.md#42-ownership-rules-d14): evaluate the right side, then destroy the old value, then store the new one; `_ = f()` destroys the result at the end of the statement.

```go
s := "one"
t := "two"
s = t            // "one" is dropped; t moved into s; t unusable until assigned
```

**STM-2.** *Short variable declarations.* `x := e` declares an owner. If `e` is a place that is not a local, the ownership rules apply ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)): `x := p.name` is an error, `x := clone(p.name)` is fine, `x := p.name[:2]` is a view.

**STM-3.** *Blocks end lives.* When control leaves a block by any path (fall-through, `break`, `continue`, `goto`, `return`, panic), the block's locals are destroyed in reverse declaration order ([OWN-R1, OWN-R2](04-ownership.md#42-ownership-rules-d14)).

**STM-4.** *Statement headers.* Temporaries created in the header of `if`, `switch` or `for … range` live until the end of the whole statement, body included. The `init` statement of `if`/`switch` declares variables that are destroyed at the end of the whole statement.

```go
if f := open!(path); f.Valid() {      // f lives for the whole if / else
    use(f)
}                                     // f destroyed here
```

**STM-5.** *`goto`.* As in Go, `goto` must not jump over a variable declaration into its scope. Jumping out of a block destroys that block's locals (STM-3).

**STM-6.** *Return.* `return a` of an owned local moves it to the caller, and builds it in the caller's result slot, so it never moves again ([OWN-R10](04-ownership.md#42-ownership-rules-d14)). `return p.field` needs `clone` or `move` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). Locals destroyed after the result is built, but a deferred call runs first ([PAN-4](09-errors-and-panics.md)).

**STM-7.** *Send.* `ch <- a` moves `a` into the channel. See [CON-4](12-concurrency.md).

**STM-8.** *IncDec and op-assign* on trivial operands work as in Go. `s += "x"` on an owned string grows it in place.

**STM-9.** *Expression statements.* A call that returns an owned value and discards it destroys the value at the end of the statement. Writing `_ = f()` does the same.

**STM-10.** *Switch.* `switch` and `select` work as in Go. The tag expression is a borrowed operand; a temporary tag lives for the whole statement (STM-4).

**STM-11.** *Labels, `break`, `continue`* work as in Go.

## 7.2 Examples

```go
func demo() {
    a := Doc{Title: "a"}
    b := Doc{Title: "b"}
    for i := 0; i < 2; i++ {
        c := Doc{Title: "c"}    // destroyed at the end of every iteration
        if i == 1 { break }     // c destroyed on this path too
    }
}                               // b is destroyed first, then a
```

Compared with Go: the control flow is identical. The only difference is that destruction is visible when a type has a `drop` method.

## 7.3 Rationale

- Keeping statement syntax identical keeps Go code readable and the grammar small.
- Header temporaries living for the whole statement fix a classic C++ trap (a range over a temporary's inner data) at no cost.
