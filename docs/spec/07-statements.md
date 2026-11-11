# 7. Statements

Base: Go spec [Statements](https://go.dev/ref/spec#Statements), [Assignment statements](https://go.dev/ref/spec#Assignment_statements), [Short variable declarations](https://go.dev/ref/spec#Short_variable_declarations), [If statements](https://go.dev/ref/spec#If_statements), [Switch statements](https://go.dev/ref/spec#Switch_statements), [For statements](https://go.dev/ref/spec#For_statements), [Return statements](https://go.dev/ref/spec#Return_statements), [Send statements](https://go.dev/ref/spec#Send_statements), [Select statements](https://go.dev/ref/spec#Select_statements), [Go statements](https://go.dev/ref/spec#Go_statements), [Defer statements](https://go.dev/ref/spec#Defer_statements), [Break statements](https://go.dev/ref/spec#Break_statements), [Continue statements](https://go.dev/ref/spec#Continue_statements), [Goto statements](https://go.dev/ref/spec#Goto_statements).

Statement syntax is unchanged. Only the effects on ownership are new. `range` is in [chapter 8](08-slices-and-range.md), `defer` and `return` in `!` functions are in [chapter 9](09-errors-and-panics.md), `go` and `select` are in [chapter 12](12-concurrency.md).

## 7.1 Rules

**STM-1.** *Assignment.* `a = b` evaluates the right side first, drops or releases the old value of `a` ([OWN-R1, OWN-R2](04-ownership.md#42-ownership-rules-d14)), and stores the new value:
- for trivial (`OWN-C1`) types, `b` is copied bitwise;
- for shared/value handles (`OWN-C2`: `string`, `[]T`, `map[K]V`, `Shared[T]`, etc.), `a` shares `b`'s handle (retaining if `b` is read again later, or auto-moving at 0 retain/release if this is `b`'s last read);
- for single-owner (`OWN-C3`: `Unique[T]`, `File`) types, `b` moves into `a`, and reading local `b` before reassignment is a compile error (`E0451`, [OWN-R5](04-ownership.md#42-ownership-rules-d14)).
`a = a` is a no-op.

```go
s := "one"
t := "two"
s = t            // "one" released; t auto-moves into s if unused later (or retains if t is still used)

u1 := NewUnique(1)
u2 := NewUnique(2)
u1 = u2          // old *u1 freed; u2 moves into u1; u2 unusable until reassigned
```

**STM-2.** *Short variable declarations.* `x := e` declares `x`. If `e` is a non-local place (`p.name`, `xs[i]`):
- `OWN-C1` copies and `OWN-C2` retains/shares the handle (`x := p.name` and `x := p.name[:2]` both produce `string` values in $O(1)$);
- `OWN-C3` single-owner slots cannot be implicitly moved out (`x := p.file` is an error; use `mem.Take(&p.file)`, `mem.Replace(&p.file, v)` or `p.file.Clone()`, [OWN-R3a, OWN-R4](04-ownership.md#42-ownership-rules-d14)).

**STM-3.** *Blocks end lives.* When control leaves a block by any path (fall-through, `break`, `continue`, `goto`, `return`, panic), the block's live locals are dropped in reverse declaration order ([OWN-R1, OWN-R2](04-ownership.md#42-ownership-rules-d14)), except locals kept alive by an executed `defer` ([PAN-3](09-errors-and-panics.md)), which are dropped at function exit ([PAN-4](09-errors-and-panics.md)).

**STM-4.** *Statement headers.* Temporaries created in the header of `if`, `switch` or `for … range` live until the end of the whole statement, body included. The `init` statement of `if`/`switch` declares variables that are dropped at the end of the whole statement.

```go
if f := open!(path); f.Valid() {      // f lives for the whole if / else
    use(&f)
}                                     // f dropped here
```

**STM-5.** *`goto`.* As in Go, `goto` must not jump over a variable declaration into its scope. Jumping out of a block drops that block's locals (STM-3).

**STM-6.** *Return.* `return a` transfers `a` to the caller's result slot (`OWN-C1` copies, `OWN-C2` local moves on last use at 0 retain/release, `OWN-C3` local moves and builds in the caller's result slot; see [OWN-R10](04-ownership.md#42-ownership-rules-d14)). `return p.field` retains `p.field` when `p.field` is `OWN-C2` (`string`, `[]T`, `map`), and requires `mem.Take(&p.field)` or `p.field.Clone()` when `p.field` is single-owner `OWN-C3` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). Locals are dropped after the result is built, and deferred calls run first ([PAN-4](09-errors-and-panics.md)).

**STM-7.** *Send.* `ch <- a` transfers `a` into the channel (`OWN-C1` copies; immutable `OWN-C2` retains or moves on last use; mutable-container `OWN-C2`—`[]T`, `map[K]V`, or a struct containing one—moves in $O(1)$ on unshared `refcount == 1` last use, and is a compile error requiring `a.Clone()` if `a` is still used afterwards or aliased; `OWN-C3` moves). See [CON-4](12-concurrency.md).

**STM-8.** *IncDec and op-assign* on trivial operands work as in Go. `s += "x"` on a `string` appends in place when `s`'s backing buffer is uniquely referenced (`refcount == 1`) and `(s.ptr - backing.data) + len(s) + len("x") <= backing.byte_cap` (keeping the trailing NUL byte), and allocates a fresh backing buffer when shared or out of capacity.

**STM-9.** *Expression statements.* A call that returns an owned value and discards it destroys the value at the end of the statement. Writing `_ = f()` does the same: it frees the result at once.

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
