# 7. Statements

Base: Go spec [Statements](https://go.dev/ref/spec#Statements), [Assignment statements](https://go.dev/ref/spec#Assignment_statements), [Short variable declarations](https://go.dev/ref/spec#Short_variable_declarations), [If statements](https://go.dev/ref/spec#If_statements), [Switch statements](https://go.dev/ref/spec#Switch_statements), [For statements](https://go.dev/ref/spec#For_statements), [Return statements](https://go.dev/ref/spec#Return_statements), [Send statements](https://go.dev/ref/spec#Send_statements), [Select statements](https://go.dev/ref/spec#Select_statements), [Go statements](https://go.dev/ref/spec#Go_statements), [Defer statements](https://go.dev/ref/spec#Defer_statements), [Break statements](https://go.dev/ref/spec#Break_statements), [Continue statements](https://go.dev/ref/spec#Continue_statements), [Goto statements](https://go.dev/ref/spec#Goto_statements).

Statement syntax is unchanged. Only the effects on ownership are new. `range` is in [chapter 8](08-slices-and-range.md), `defer` and `return` in `!` functions are in [chapter 9](09-errors-and-panics.md), `go` and `select` are in [chapter 12](12-concurrency.md).

## 7.1 Rules

**STM-1.** *Assignment.* `a = b` evaluates the right side first, drops or releases the old value of `a` ([OWN-R1, OWN-R2](04-ownership.md#42-ownership-rules-d14)), and stores a copy of `b`, so nothing done to `a` afterwards is seen through `b` ([OWN-R3](04-ownership.md#42-ownership-rules-d14), [ADR-0041](../adr/0041-values-are-copied.md)):
- plain data (`OWN-C1`) is copied bitwise; a `string`, `Shared[T]`, `Weak[T]`, `chan T` or function value is copied as its handle (a retain); a `[]T` or a `map[K]V` gets a new buffer with a copy of every element or entry; an interface value gets a box of its own with a copy of the value it holds ([TYP-5](03-types.md)); a `Unique[T]` gets a new allocation; a struct, an array or an `Option` is copied part by part;
- the copy is skipped when `b` is a temporary or a local that is not read again (its last use), and with `a = move(b)`: the value itself is stored. After `move(b)`, reading `b` before reassignment is a compile error (`E0451`, [OWN-R5](04-ownership.md#42-ownership-rules-d14));
- a value that cannot be copied (a `File`, [OWN-C3](04-ownership.md#41-type-categories-d13)) is moved into `a` (`a = move(b)`, or a temporary); a plain `a = b` is a compile error, also on the last use of `b`.
`a = a` is a no-op.

```go
s := "one"
t := "two"
s = t            // "one" released; s holds the bytes of t (a retain, skipped if t is unused later)

xs := []int{1, 2}
var ys []int
ys = xs          // ys gets its own copy of the elements, because xs is used below
ys[0] = 9        // xs[0] is still 1
fmt.Println(xs)

u1 := NewUnique(1)
u2 := NewUnique(2)
u1 = move(u2)    // old *u1 freed; u2 moves into u1; u2 unusable until reassigned
```

**STM-2.** *Short variable declarations.* `x := e` declares `x` and gives it its own copy of `e`, as an assignment does (STM-1). If `e` is a non-local place (`p.name`, `xs[i]`):
- the value is copied by the rule of its type (`x := p.name` and `x := p.name[:2]` both produce `string` values in $O(1)$; `x := p.items` copies a slice with its elements);
- a value that cannot be copied cannot be read out (`x := p.file` is an error; use `&p.file`, `mem.Take(&p.file)` or `mem.Replace(&p.file, v)`, [OWN-R3a, OWN-R4](04-ownership.md#42-ownership-rules-d14)).

**STM-3.** *Blocks end lives.* When control leaves a block by any path (fall-through, `break`, `continue`, `goto`, `return`, panic), the block's live locals are dropped in reverse declaration order ([OWN-R1, OWN-R2](04-ownership.md#42-ownership-rules-d14)), except locals kept alive by an executed `defer` ([PAN-3](09-errors-and-panics.md)), which are dropped at function exit ([PAN-4](09-errors-and-panics.md)).

**STM-4.** *Statement headers.* Temporaries created in the header of `if`, `switch` or `for … range` live until the end of the whole statement, body included. The `init` statement of `if`/`switch` declares variables that are dropped at the end of the whole statement.

```go
if f := open!(path); f.Valid() {      // f lives for the whole if / else
    use(&f)
}                                     // f dropped here
```

**STM-5.** *`goto`.* As in Go, `goto` must not jump over a variable declaration into its scope. Jumping out of a block drops that block's locals (STM-3).

**STM-6.** *Return.* `return a` gives the caller's result slot its own value. Returning a local needs no `move`, also when the local cannot be copied: when the local is not read again no copy is made, and it is built in the caller's result slot when possible (see [OWN-R10](04-ownership.md#42-ownership-rules-d14)). `return p.field` copies `p.field` by the rule of its type (a `string` is retained; a `[]T` or a `map` is copied with its elements), and requires `mem.Take(&p.field)` or `mem.Replace(&p.field, v)` when `p.field` cannot be copied ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). Locals are dropped after the result is built, and deferred calls run first ([PAN-4](09-errors-and-panics.md)).

**STM-7.** *Send.* `ch <- a` gives the channel its own copy of `a` ([OWN-R3](04-ownership.md#42-ownership-rules-d14), [ADR-0041](../adr/0041-values-are-copied.md)): a `[]T`, a `map[K]V` or a struct that holds one is copied with its elements, so the sender and the receiver never share them. The copy is skipped, and the value itself is sent in $O(1)$, when `a` is a temporary, on the last use of a local, or with `ch <- move(a)`. A value that cannot be copied is moved into the channel (`move(a)`, or a temporary). See [CON-4 and CON-14](12-concurrency.md) for the `Send` rule.

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
