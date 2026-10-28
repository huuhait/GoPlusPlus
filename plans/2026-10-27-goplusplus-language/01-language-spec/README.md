# P01 — Language specification v0

**Goal:** a written specification (`docs/spec/`) precise enough to implement P02–P05. It is defined as *the Go specification plus a numbered list of deltas*, and validated by ten sample programs written before any implementation.
**Depends on:** P00 · **Unblocks:** P02–P05, P07–P09, P12 · **Size:** L · **Milestone:** M0 (grammar), M2 (memory model)

This is the most important phase: it fixes how Go++ code feels. The memory model (D1.3–D1.7) answers the hardest question in the project: how to keep Go's look without a GC.

## Decisions

**D1.1 — The base is the Go spec; Go++ is written as deltas.**
- The base is the **Go 1.25** specification (installed on the dev machine).
- The P03 types2 port and the P04 differential tests use the same Go version.
- Moving to a newer Go needs an ADR.

Everything not mentioned stays as in Go:
- packages, imports, visibility by capitalization;
- declarations, `:=`, constants and `iota`;
- control flow, `switch`/`select`, labels/`goto`;
- methods, embedding, structural interfaces;
- generics syntax, `init`, build constraints, testing conventions.

Each delta has EBNF, semantics, an example and a rationale.
*Why:* maximum transfer of Go knowledge, and a small spec to maintain.

**D1.2 — Lexical deltas.**
- Source files are `.gopp`, UTF-8.
- New keyword: `macro`. Two contextual words: `ref` (special only where a type is expected, so `ref := "main"` still works) and `move` (the parameter mark, as in `func Push(move s string)`; it is also the function `move(x)`, D1.5).
- New predeclared identifiers:
  - the functions `move`, `clone`, `new_unique`, `new_shared`, `new_weak`;
  - the generic types `Unique[*T]`, `Shared[*T]`, `Weak[*T]`;
  - the built-in constraints `Copy` and `Clone`, written like `comparable`: `[T Copy]`, `[T Clone]` (D1.10);
  - the std macros, such as `#Assert` and `@Derive` (D1.14).
- Go's `new` is removed.
- `MustError` is not predeclared: it lives in package `errors` (`errors.MustError`, D1.8).
- New tokens: `#` (macro invocation) and `@` (attribute or attached macro). The existing `!` token also marks functions that can fail (`func load!(…)`) and unwrapping calls (`call!()`), D1.8.
- Semicolon insertion: unchanged from Go. A `call!()` always ends in `)`, so no new rule is needed.

**D1.3 — Type categories.**

| Category | Types | Copy | End of life |
|---|---|---|---|
| Trivial | bool, numbers, raw `*T`, `unsafe.Pointer`, `ref string`, `ref []T`, arrays/structs of trivial types | implicit, bitwise | nothing |
| Owning | `string`, `[]T`, `map[K]V`, `Unique[*T]`, types with a `drop` method, arrays/structs containing an owning type, C++ classes with a non-trivial copy/move/destructor | move-only; `x.Clone()` copies | drop glue runs |
| Shared handle | `Shared[*T]`, `Weak[*T]`, `chan T`, interface values (including `error`), func values, arrays/structs whose non-trivial fields are all shared handles | implicit; copying retains (atomic) | release; the last strong reference frees |

**D1.4 — Ownership rules.**
1. **One owner.** Every value has exactly one owner: a variable, field, element or temporary. When the owner's lifetime ends, the value is destroyed: its `drop` method runs (if any), then its owning fields are destroyed in reverse declaration order.
2. **Destruction order.** Locals are destroyed at the end of their block, in reverse declaration order, except locals kept alive by an executed `defer`, which are destroyed at function exit (D1.9). Temporaries are destroyed at the end of the full statement; for temporaries in a `for … range`, `if` or `switch` header, see D1.6.
3. **By-value use moves.** A by-value use of an owned local moves it: `b := a`, `x.f = a`, passing `a` to a `move` parameter, `return a`, `ch <- a`, `T{F: a}`. Any later use of `a` is a compile error until `a` is assigned again.

   **3a. Reading from other places** (decides Q2).
   - Reading an owned value from any other place (a field, element, map entry, package variable, pointer target or borrowed parameter) borrows it where a borrow is enough: operands, borrowed arguments (including interface parameters), receivers, the `range` source.
   - Where ownership is needed (`:=`, `=`, a `move` argument, `return`, a channel send, a composite literal), it is a compile error with two fix-its: `p.Clone()` (copy it) and `move(p)` (take it, leaving the zero value). For a borrowed parameter, the fix-its are `p.Clone()` and making the parameter `move`, because a borrowed parameter can't be moved out of (D1.5).
   - Maps:
     - `move(m[k])` removes the entry and returns it.
     - `v, ok := clone(m[k])` and `v, ok := move(m[k])` keep the comma-ok form.
     - `m[k].f = x` stays illegal (as in Go).
   - A swap that reads and writes the same places in one statement (`a[i], a[j] = a[j], a[i]`) is allowed and moves.
   - Built-in `min`/`max` on owned strings return a copy.
4. **Explicit and implicit moves.**
   - `move(place)` takes a value out of any place and leaves the zero value.
   - The move is implicit when the same place is passed to a `move` parameter and also receives the result, in one statement: `p = append(p, …)`, `buf = strconv.AppendInt(buf, n, 10)`. So `x.items = append(x.items, v)` works exactly as in Go.
   - Anywhere else, write `append(move(p), …)` or `append(p.Clone(), …)`.
   - Spread (`append(a, b...)`) and `copy(dst, src)` with non-trivial elements need `move` or `Clone` on the source: `move(b)` or `b.Clone()`, `move(src)` or `src.Clone()`.
   - Moves out of places happen after all other arguments are evaluated.
5. **Zero values.** Every type has a zero value (Go), except the pointer kinds, funcs and interfaces with methods other than `error` (D1.7: they are never nil) and the types of rules 9 and 11. A moved-from place holds the zero value; for C++ class types see rule 9, and a moved-from place of a rule 11 type holds the all-zero bytes, which only drop and assignment may touch (use after move is an error). A `drop` method must therefore accept the zero value; `gopp vet` checks this. Drop glue skips a type's `drop` when the value is all zero bytes, so dropping a zero value behaves the same in every build mode.
6. **Copies are explicit: `x.Clone()`.**
   - Every type that can be cloned has a method `Clone() T`, built-in types too (`string`, `[]T`, `map`, `Unique[*T]`, numbers, …). `clone(x)` stays as a short form that calls it. `move(x)` stays a built-in.
   - A type whose `Clone` method has another signature is not cloneable (compile error).
   - Clone is structural when all fields are cloneable. A trivial `x` clones as a plain copy, and `u.Clone()` of a `Unique[*T]` allocates a new `T` holding `(*u).Clone()`.
   - Shared handles (D1.3) copy by retaining.
   - A type with a `drop` method is cloneable only if it defines `Clone() T`, which prevents silently duplicating unique resources such as file descriptors. There is no `Clone` derive: a type with `drop` writes `Clone()` by hand on purpose.
   - A type that mixes owning fields with raw `*T` fields needs a hand-written `Clone()`, because an automatic clone would copy the raw pointers (for example, tree nodes with parent pointers, like `Node` in D1.7).
   - Imported C++ types clone through their copy constructor.
7. **Destructors are a method named `drop`** with a pointer receiver: `func (f *File) drop()`. It can't be called directly (like `init`).
8. **No borrow checker.** Refs and raw pointers can dangle, as in C++.
   - The compiler does catch: returning `&local`; returning a `ref` into a local of the same function (D1.6); storing a borrow of a temporary; a `defer` or `go` that borrows something that dies first (D1.9, D1.13); use after move. (A borrowed parameter lives until return, so a `defer` may borrow it.)
   - The cases it doesn't catch are listed in the UB chapter (P01.14). They include refs or raw pointers kept past their owner, and a borrowed parameter whose owner the callee frees through another path (`u.SetName(u.Name)`).
   - `-asan` catches the rest at runtime. The owner chose no extra safety layer for v1.
9. **C++ class types.**
   - A C++ type has a zero value only if it has a default constructor.
   - Where Go needs a zero value and there is none, it is a compile error: `make([]T, n)` (a constant length `0` is fine: `make([]T, 0, c)` creates no value; ADR-0016), `m[k]` and `<-ch` outside a comma-ok form (Q9), a receive from a closed channel, `var zero T` in generic code, named results. A failing `!` leaves such results unset, and `v, err := f()` can then use `v` only where `err == nil` is known (flow typing in P03, ADR-0018 amendment point 4).
   - Every move of a C++ value, implicit or `move(x)`, runs its move constructor and leaves the C++ "moved-from" state, not zero. The source is destroyed at the end of its scope.
   - Only containers relocate values, with the type's relocate operation (P05 D5.5): `memcpy` only for Go++ types and trivially relocatable C++ types.
10. **Pinned values.**
    - `gopp vet` reports moving a local after its address, or a ref into it, has been stored.
    - Types marked `@Pinned` (the `sync` types) can't be moved or copied after first use: a compile error, like Go vet's copylocks.
    - `return local` builds the value in the caller's result slot, so it never moves.
11. **Required fields** (owner decision; ADR-0016).
    - `@Required` after a struct field's type (`ID int @Required`) means every composite literal of the struct must set it, by name or by position: `User{Email: "x"}` is a compile error. It is allowed only on struct fields (embedded ones too).
    - A struct with a required field has **no zero value**, like the C++ types of rule 9, with the same errors: `var u User`, `make([]User, n)` (except a constant `0`), `var a [3]User`, array literals with missing elements (`[3]User{{…}}`), keyed slice literals with gaps (`[]User{5: {…}}`), `clear(s)` on a `[]User` (use `s = s[:0]`; `clear(m)` on a map is fine), named results, and so on.
    - It spreads: a struct or array containing such a type has no zero value either, so a `User` field inside `Team` must be set in every `Team` literal. For a generic type it belongs to the instance: `Box[User]` has no zero value.
    - **Never nil:** on a pointer field, a func field or a field of an interface with methods (other than `error`) `@Required` is an error, because those are never nil anyway (D1.7, ADR-0018 and its amendment point 3). A `@Required` field whose type can be nil (`error`, `any`, maps, channels) is **never nil**, checked at compile time: a value stored in it (in a literal or an assignment) must be known non-nil (the list is in spec TYP-15: a literal, `make(…)`, a local checked with `!= nil`, any value of a never-nil type converted to `error` / `any`, or a read of another such field); storing `nil` or a maybe-nil value is an error; `move` out of the field is an error (use `mem.Replace(&c.Owner, v)`, which returns the old value; `v` must be known non-nil); `&c.Log` is an error except as `mem.Replace`'s first argument; on a slice, an `unsafe.Pointer`, an `Option` or a type-parameter field it only means "must be set" (a nil slice is empty; an `unsafe.Pointer` is unsafe code). `drop` never sees such a field nil (a moved-from value is all zero and drop glue skips it).
    - JSON decoding builds a new value (`json.Decode[User](data)`); there is no `json.Unmarshal` (P13 D13.4).
    - **`@Strict`** (owner decision 2026-10-28; ADR-0019) on a struct type makes every field required, except `Option[T]` fields, fields marked `@Option` and `_`; `@Option` (after the field's type, only in a `@Strict` struct, only on a type that has a zero value) lets a field be left out; `@Required` inside a `@Strict` struct is an error only where redundant (on an `Option[T]` field it keeps "must be set"); the required bits must match in a struct conversion (spec TYP-15a, EXP-9).
    - **Lookups that can fail** (rules 9 and 11; Q9): a map index `m[k]` or a receive `<-ch` whose value type has no zero value may be used only in a comma-ok form (`v, ok := m[k]`, `v, ok := clone(m[k])`, `v, ok := move(m[k])`, `v, ok := <-ch`, `case v, ok := <-ch:` in a `select`), as the target of `m[k] = v`, in `delete(m, k)`, or as a bare `move(m[k])` statement that discards the value; every other use (`v := m[k]`, `u := clone(m[k])`, `u := move(m[k])`, `m[k].Email`, `f(m[k])`, `T{F: m[k]}`, `v := <-ch`, `case v := <-ch:`, `(<-ch).Email`) is an error, because a missing key or a closed channel would give zero bytes; the comma-ok forms (`u, ok := x.(T)`, `u, ok := m[k]`, `u, ok := clone(m[k])`, `u, ok := move(m[k])`, `u, ok := <-ch`) work, and `u` is usable only where `ok` is known to be true (flow typing in P03, task P03.18; spec OWN-R9, OWN-P10). A type switch and `u := x.(T)` work as in Go.
    - **Moves:** a moved-from local holds zero bytes and is tracked by use after move (rule 5). `move` out of any place other than a local variable is an error for a Go++ type with no zero value (a slice, array or map element, an `&` range alias, a struct field, `*p`), because the place would be left as zero bytes that later code reads as a real value; use `mem.Replace(&place, v)`, `delete(m, k)` or `move(m[k])` (which remove the entry), `s = s[:n]` (which drops elements), or `range move(xs)`. A moved local is tracked by the use-after-move check. C++ types are not affected: their moved-from state is a valid C++ object. Locals are declared variables, comma-ok results of map and channel lookups, owning range variables and `move` parameters, not type-assertion bindings (borrowed) or captured variables inside a closure; swaps (`xs[i], xs[j] = xs[j], xs[i]`) are allowed; in a map range, `v` counts as moved after `move(m[k])` of its entry (OWN-R4, RNG-3).

*Why:* deterministic, GC-free memory with C++-compatible object semantics, which C++ interop (P08) needs anyway, while keeping Go's zero values. Zero-on-move means no drop flags and no partially dead variables; moved-from state is always defined.
*Rejected:* tracing GC (excluded by the requirements); ARC everywhere (hidden cost, cycles); a Rust-style borrow checker (cost to compile speed and simplicity; reconsider after v1); C++-style implicit copies (hidden allocations).

**D1.5 — Parameters: owning types borrow, trivial types copy.**
- **Borrowing.** Parameters and receivers of **owning** types *borrow* their argument. They are read-only at the top level: you can't assign them (or their fields), move out of them, or append to them. Data reached through slices, maps and pointers stays mutable, as in Go: elements of `[]T`, map entries, pointer targets.
- **Re-pointing refs.** One exception: a borrowed `string`/`[]T` parameter is a local ref, so it can be re-pointed (`p = p[n:]`). It still can't be appended to.
- **No `&param`.** `&param` on a borrowed owning parameter is a compile error (fix-it: take a `*T` parameter).
- **Trivial parameters are copies.** Trivial parameters (`int`, raw pointers, trivial structs) are ordinary copies, as in Go; the callee may change them. The compiler may skip the copy only when nothing can write through an alias.
- **`move` takes ownership.** `move` goes before the parameter name: `func (l *List[T]) Push(move v T)`, `func (move b Builder) Finish() string`, `func Join(move parts ...string)`. Parameters without it still borrow, so calls look like Go.
  - Passing an owned local to a `move` parameter moves it automatically. A field, element or map value needs `move(x)` or `x.Clone()` (rule 3a). `list.Push(name)`, `list.Push(name.Clone())`, `list.Push(move(u.Name))`.
  - Inside the callee, a `move` parameter is an ordinary owned local (mutable).
  - `move` is allowed on parameters, receivers and function types only. A method with a `move` receiver is not in any interface's method set: you can't use up a value through a shared interface box. `move` and `clone` are not function values.
  - A variadic parameter of an owning type must be written `move ...T`; a plain `...T` with an owning `T` is a compile error. The caller writes `x.Clone()` to keep its value: `filepath.Join(dir.Clone(), "x")`.
  - `&` and `*` keep their Go meanings.
- **`*T` mutates.** `*T` parameters and receivers mutate, as in Go.
- **Shared handles work like Go.** Parameters of shared-handle types (`Shared[*T]`, `Weak[*T]`, `chan`, interfaces including `error`, funcs) can be copied, stored and returned by the callee, and each copy retains (`return err` works as in Go). The call itself passes them without a retain.
- **ABI.**
  - Trivial parameters follow the C ABI (copies).
  - Borrowed owning parameters pass by hidden pointer.
  - `string` and `[]T` pass as `(ptr, len)`, so callers can pass owned values, refs and literals at zero cost.
  - Go++ → Go++ calls may use the internal convention (P04 D4.4).

*Why:* generic code such as `func Index[T comparable](xs []T, v T) int` works for owning `T` with no moves or clones, and call sites look exactly like Go. This is the C++ `const T&` / Swift / Mojo convention. Trivial parameters stay real copies, so Go code such as `for b != 0 { a, b = b, a%b }` works unchanged.
*Rejected:* move-by-default parameters (Rust without `&`, hostile to Go-style generic APIs); copy-by-default owning parameters (hidden allocations); read-only trivial parameters (they reject ordinary Go code, and passing large ones by pointer gives wrong results when an argument aliases memory the callee writes).

**D1.6 — Refs.**
- `ref string` (immutable bytes) and `ref []T` (mutable elements, fixed length) are `(ptr, len)` borrows.
- Slicing produces a ref without allocating: `s[i:j]` gives a `ref string`, `xs[i:j]` a `ref []T`, and `:=` infers that. The exception is re-slicing an owned slice into itself (`s = s[i:j]`), which shrinks it in place (D1.20).
- Owned → ref conversion is implicit wherever a ref is expected. Ref → owned is explicit and copies: `string(v)`, `[]T(v)` (`[]T(v)` needs elements that satisfy `Clone`). Unlike Go, where slice conversions never copy, the copy is how you get ownership.
- Maps accept a `ref` key for lookups, so `counts[w]` with `w ref string` doesn't allocate. Inserting a new entry clones the key, whether it is a `ref` key or a borrowed one (OWN-V4a).
- Temporaries live until the end of the full statement. In the header of a `for … range`, `if` or `switch`, they live until the end of the whole statement, body included.
- Refs may be stored and returned at the programmer's risk, like C++ `string_view`/`span`. `gopp vet` warns about ref-typed struct fields unless the struct is marked `@Ref` (for example, tokens pointing into a source buffer).
- Returning a `ref` into a local of the same function, or into an owned value that dies at the return, is a compile error (`func first() ref string { s := "hi"; return s[:1] }`). A ref into a borrowed parameter is fine. Less obvious cases stay undefined behavior (`-asan` finds them).

**D1.7 — Pointers and heap allocation.** Every heap allocation is spelled out; Go's `new` is removed. (The runtime's own allocations are not spelled in source: for example the defer records of deferred calls in loops, which hold extended locals.)

| Type | Created with | Meaning |
|---|---|---|
| `*T` | `&x` (address of a variable) | raw, non-owning, **never nil** (Go syntax, no GC) |
| `Unique[*T]` | `new_unique(v)` | single owner; the value is dropped and freed when the pointer is destroyed; never nil |
| `Shared[*T]` | `new_shared(v)` | shared ownership, atomically refcounted; one allocation holds the counts and the value (like `make_shared`) |
| `Weak[*T]` | `new_weak(s)`, from a `Shared[*T]` | doesn't own and doesn't allocate; `w.Lock()` returns an `Option[Shared[*T]]`, `nil` if the value is gone |
| `Option[T]` | `nil`, or any `T` (converts automatically) | "maybe nothing", for **any** type; zero value `nil`; comparable when `T` is (both nil, or both set and equal), so it can be a map key |

**Pointers are never nil** (owner decision, ADR-0018). `*T`, `Unique[*T]`, `Shared[*T]` and `Weak[*T]` always hold a target and have no zero value (`var p *User` and `p == nil` are errors). Owning pointers name their pointer type: `Unique[*User]` (`Unique[User]` is an error). "Maybe nothing" is `Option[T]` for any type: zero value `nil`; for pointer kinds it has the size of a pointer; it has only `IsNil()` and `Get() (T, bool)`. An `Option` is used as its value only after a check: `if o != nil { o.Name }`, `if !o.IsNil() { … }`, an early `return` on `o == nil`, a `for o != nil` loop, or `v, ok := o.Get()`; narrowing works on locals and parameters. **Interfaces with methods and funcs are never nil too** (owner decision 2026-10-28, ADR-0018 amendment point 3): `var l Logger` and `var f func()` are errors, and the "maybe" forms are `Option[Logger]` and `Option[func()]`. **`error` and `any` are the exceptions** and keep Go's `nil` (`if err != nil`, `var x any` and `recover()` are unchanged); `any` here means the empty interface: `any`, `interface{}` and any interface type with an empty method set. A nil `Option[T]` converts to a nil `any` / `error`, and to a nil `Option[I]` for an interface `I` with methods. The `error` exception covers `error` and its aliases only (`type MyErr error` is an interface with methods, so never nil). `unsafe.Pointer`, maps, slices and channels stay nilable as in Go; a nil slice or map is empty (the first write to a nil map creates it), and a nil channel blocks (ADR-0018 amendment point 5). The same narrowing covers `v, err := f()` when `v`'s type has no zero value (`v` only where `err == nil`) and the comma-ok forms of types with no zero value (`v` only where `ok` is true) (amendment point 4). Task P03.18. C pointers import as `Option[*T]` (`*T` when Clang knows they are non-null); a C++ `T&` is a `*T`.

- `new_unique` and `new_shared` take the initial value and move it in: `new_unique(Node{Name: "a"})`, or `new_unique(Node{})` for a zero value.
- `new_weak` takes a `Shared[*T]`. A weak pointer can't own a new object, because it would be freed at once.
- `&T{…}` is a compile error with a fix-it ("allocation is explicit in Go++: use `new_unique(T{…})`"). In Go it allocated silently.
- Field access and method calls auto-dereference `*T`, `Unique[*T]` and `Shared[*T]`. `Weak[*T]` doesn't (its target can vanish; ADR-0017): read it with `if p := w.Lock(); p != nil { … }` (`Lock` returns an `Option`), and `w.Name` is a compile error.
- `Unique[*T]` and `Shared[*T]` have the methods of both `T` and `*T`.
- Pointers have no `IsNil()` method (they are never nil); `IsNil()` and `Get()` belong to `Option` (ADR-0018).
- `Unique[*T]` and `Shared[*T]` convert implicitly to `*T` (and so to `Option[*T]`) when borrowed from a place. They never convert from a temporary, because the target would be freed immediately.
- `make([]T, n)`, `make(map[K]V)` and `make(chan T, n)` work as in Go.

Ownership on one screen:

```go
type Node struct {
    Name     string          // owned
    Children []Unique[*Node]  // owned slice of owning pointers
    Parent   Option[*Node]   // raw back-pointer: does not own; nil for the root
}

func NewNode(name string, parent Option[*Node]) Unique[*Node] {
    return new_unique(Node{Name: name.Clone(), Parent: parent}) // name is borrowed: Clone it to own it
}

func (n *Node) Add(move child Unique[*Node]) {       // `move`: Add takes ownership
    child.Parent = n
    n.Children = append(n.Children, child)         // child moves into the slice
}

func main() {
    root := NewNode("root", nil)
    root.Add(NewNode("a", root))   // the temporary moves into Add
    b := NewNode("b", root)
    root.Add(b)                    // b moves
    // fmt.Println(b.Name)         // compile error: b used after move
    head := root.Name[:2]          // ref string: a borrow, no allocation
    fmt.Println(head, len(root.Children))
}                                  // root is dropped → the whole tree is freed
```

**D1.8 — Errors and `!`.**
- **Go error handling is unchanged.** `val, err := call()` followed by `if err != nil { … }` works exactly as in Go.
- **`func name!(…)` declares a function that can fail.** `func load!(path string) []byte` is the same function as `func load(path string) ([]byte, error)`, and callers see `([]byte, error)`. Methods and interface methods use the same form. The `!` sits right before the parameter list, as in calls: `func parse[T any]!(s string) T` is called as `parse[int]!(s)`.
- **`call!(args)` unwraps a call whose last result is `error`.** The value is the other results, without the error (zero, one or several). What happens on error depends on the function the call is in:
  - **inside a function that can fail** (declared with `!`, or whose last result is `error`; the innermost function counts, so a plain closure inside a `!` function panics), the error is returned to the caller, with zero values for the other results (results with no zero value are left unset; `return nil, err` writes `nil` for them). The `!` site is added to the error's trace. The trace never wraps the error (D1.19);
  - **anywhere else** (`main`, a goroutine body, any function without an error result), it panics with an `errors.MustError` value (fields `Err`, `Expr`, `Pos`; method `Unwrap()`). The message reads `panic: must: os.Open(path) at main.gopp:12:9: open /nope: no such file or directory`, followed by the error's trace.
- **Failure semantics.** On failure, `call!()` does exactly what `return <zero values>, err` does: named results are set, then deferred calls run. A `!` function can't name its error result; use the Go form `(T, err error)` when a deferred call must see the error.
- **Returning from a `!` function.** `return v` returns `v` with a nil error. To fail explicitly, add the error: `return nil, err`, or `return err` when there are no other results. The operand count decides: as many operands as declared results is a success; one more is a failure.
- **No `error` result on a `!` function.** A `!` function already adds the `error` result, so it can't also declare `error` as its last result: `func check!() error` is a compile error ("`!` already adds the error; remove it"). `func check!()` returns only an error. To return an error as a normal value, use the Go form `func check() (error, error)`. This removes the `return e` ambiguity.
- **Libraries don't panic on ordinary errors.** A library function that can fail is declared with `!`, so the `!` calls inside it return errors and the caller decides. `gopp vet` flags a `!` that would panic outside package `main`.
- **`(n, err)` calls.** `gopp vet` warns when `!` is used on `(n, err)` calls such as `Read` and `Write`, where `n` matters even when there is an error.
- **Syntax.** Grammar: `PrimaryExpr = … | PrimaryExpr "!" Arguments`. It works for functions, methods and generic functions (`f[T]!(x)`), and it chains: `os.Open!(p).Close!()`. Function literals are written `func!(x int) T { … }`, and function types `func!(int) T` (the same as `func(int) (T, error)`).
- **Cost.** One compare and a branch to a cold block, the same code as a hand-written `if err != nil { return …, err }`. The trace entry is written only on the error path.
- **`defer` and `go`.**
  - `go f!()` panics in the new goroutine, which has no caller to return to.
  - `defer f!()` inside a function that can fail keeps the other results and sets the error only if it is still nil. While the function is panicking, the deferred error is ignored. Anywhere else, `defer f!()` panics on error.
- **Only for calls.** Map lookups and channel receives keep Go's comma-ok form: `v, ok := m[k]`, `v, ok := <-ch`.

```go
func load!(path string) []byte {    // can fail: callers get ([]byte, error)
    f := os.Open!(path)             // on error, load returns it
    return io.ReadAll!(f)
}

func count!(path string) int {
    data := load!(path)             // on error, count returns it too; the trace now has both sites
    return len(data)
}

func main() {
    a := load!("a.txt")             // main can't return an error, so `!` panics here
    fmt.Println(len(a))

    b, err := load("b.txt")         // or handle the error like Go
    if err != nil {
        log.Fatal(err)
    }
    fmt.Println(len(b))
}
```

*Rejected:*
- Postfix `call()!` (an earlier draft): it needed a special semicolon-insertion rule and a `!=` lexing caveat (`f()!==y`). With `call!()`, the `!` sits next to the function name, where the reader looks first.
- `!` that always panics (an earlier draft): it pushed library code toward crashing.
- A `try` keyword or a `#Try(...)` macro: `!` already returns errors inside functions that can fail.

**D1.9 — panic, defer, recover.**
- **Same API as Go.** A panic unwinds with the platform unwinder (P04 D4.6), running destructors and deferred calls. `recover()` inside a deferred call stops it. An unrecovered panic prints the value and a stack trace and exits with code 2.
- **Panics in deferred calls** follow Go exactly. A panic raised in a deferred call replaces the current panic, and a later deferred `recover()` can recover it. An unrecovered chain prints every panic and exits with code 2.
- **`defer` stays function-scoped** (Go). Arguments are evaluated at the `defer` statement:
  - trivial values are copied;
  - owned values must be passed as `move(x)` or `x.Clone()`, or borrowed only if the borrowed variable lives until the function returns and isn't assigned after the `defer` (D1.11). Anything else is a compile error;
  - **`defer` in a loop works like Go.** `defer x.M()` inside a loop is legal. The loop-body variable's lifetime is extended (nothing is moved or zeroed) until its deferred call has run at function end, so the deferred calls run at function exit exactly as in Go:

  ```go
  for _, n := range files {
      f := os.Open!(n)
      defer f.Close() // ok, as in Go: f lives until the function returns and is closed then
  }
  ```
- **Function exit order:** deferred calls run LIFO, then function-scope locals and locals extended by an executed `defer` are destroyed together in reverse order of creation. Other block-scoped locals are destroyed at the end of their block. `main` is the one exception: its locals are dropped after the other goroutines stop (D1.18).
- **Panics in `drop`.** A panic that escapes a `drop` method aborts the program with a clear message. Drop glue never unwinds.
- **`move` arguments.** The callee owns its `move` arguments and drops them on every path, including panics.
- **`-panic=abort`** removes unwinding: panics print and abort. Binaries are smaller and compiles faster. `recover` always returns nil in this mode, and `gopp vet` warns about it. A C++ exception that reaches a Go++ frame aborts too. Unwind tables are kept, so stack traces still work.
- **C++ exceptions** entering Go++ frames unwind like panics, and `recover()` returns them as `ffi.Exception` (P08).
- **Forced unwinding and `longjmp`.** `recover()` and `ffi.Try` never stop forced unwinding (`pthread_exit`, thread cancellation). A C `longjmp` across Go++ frames is undefined behavior: it skips destructors and defers.

**D1.10 — Generics.** Go's design: type parameters, constraint interfaces with type sets, inference. Additions:
- built-in constraints `Copy` (types that copy for free, like numbers) and `Clone` (`[T Clone]` means `T` has `Clone() T`; every cloneable type, numbers included), written like `comparable`. No `[T Clone[T]]`. They embed in constraint interfaces like any Go interface (`interface{ Clone; cmp.Ordered }`), and, as in Go, can't be part of `|` unions;
- a generic body may copy a `T` only if `T` is `Copy`; otherwise it borrows, moves (`move` parameters) or calls `Clone` (which requires `Clone`);
- in a generic body, `return a` for a borrowed parameter of non-`Copy` `T` needs `a.Clone()`, which requires `Clone` (D1.4 rule 3a);
- the implementation is full monomorphization (P04);
- **generic methods are allowed** (owner decision; decides Q5): a method may have its own type parameters, as in `func (s Set) Map[U any](f func(int) U) []U`. Each use is monomorphized like a generic function. Interfaces can't declare generic methods, so a generic method never satisfies an interface method.

**D1.11 — Closures** (decides Q1).
- **Non-escaping closures capture by reference.** A closure that is only called locally, or passed to a parameter that the callee doesn't keep, doesn't escape. Assigning it to a local variable is not an escape.
- **Escaping closures share their variables.** An escaping closure keeps each captured variable in a shared, reference-counted cell. So all closures and the enclosing function see the same variable, exactly like Go. A closure escapes when it is:
  - returned;
  - stored in a field, element, package variable, map or channel;
  - passed to a `move` parameter;
  - passed to a parameter that the callee may keep (store, return, or pass on to a place that keeps it), as in `http.HandleFunc("/", func(…) { … })`;
  - used with `go`;
  - captured by an escaping closure;
  - stored in a local variable declared in an outer scope than some variable it captures.
- **Captured variables live as long as the closure.** A captured owned variable is destroyed when its cell is released for the last time. A closure stored in an outer variable keeps its captures alive.
- **Borrowed variables.** A closure that runs later (`go`, stored, returned) can't capture a borrowed variable (a borrowed parameter, a range variable over owning elements). Fix-its never reuse the name: `go fetch(u.Clone())`, `mine := u.Clone()`, or `range move(users)`.
- **Escape analysis.** Like Go, the compiler infers for every parameter whether the callee may keep it, and records the result in export data with the other inferred attributes (P06 D6.2). A call through a func value or an interface method assumes the parameter is kept. The choice between a cell and a plain reference never changes what the program does; it only removes allocations.
- **Method values.** A method value (`f := s.Handle`) is a closure over its receiver. For a value receiver of an owning type it is a compile error (no silent copies); write `s.Clone().Handle`.
- **Recursion.** A recursive escaping closure creates a reference cycle; `gopp vet` warns. Non-escaping recursive closures are fine.
- **`defer` arguments.** Deferred calls evaluate their arguments at the `defer` statement, as in Go. A borrowed argument whose variable is assigned after the `defer` is a compile error (fix-it: `x.Clone()`, so `defer show(d.Clone())` gives Go's snapshot). A deferred function literal may capture a variable of a nested block or loop body (D1.9). A closure body that takes a captured outer variable needs `move(x)` or `x.Clone()`.

*Why:* this is Go's behavior (Go moves such variables to the heap), so closures never silently diverge from Go.
*Rejected:* capture by value with a compile error on later mutation (an earlier draft): it rejected common Go code, such as returned closure pairs that share a counter. Explicit capture lists: not Go.

**D1.12 — Interfaces and runtime type information.**
- Go's structural interfaces.
- **Shared handles.** Interface values are shared handles (D1.3). Copying one shares the same dynamic value: an immutable, reference-counted box, or the pointer it holds. So `==`, type switches and copies behave as in Go.
- **Boxing.** Converting an owned value into an interface variable, field or element moves it into a box. Storing a `*T` in an interface stores a borrow.
- **Interface parameters.** An interface parameter (including `...any`) is a shared handle like any interface value (D1.5), so the callee may keep it: store it, return it or send it.
  - Passing an interface value (such as `err`) passes the same box.
  - Passing a non-interface owned value doesn't consume it: the callee gets a borrowed `(itab, &value)`, with no box and no allocation. Only if the callee keeps it is the value cloned into a box, at that moment. So `fmt.Println(name)` doesn't consume `name`.
  - A kept slice or map is then a copy that no longer shares elements with the caller's value. This is a listed difference from Go (P04 D4.10).
  - A value that can't be cloned (no `Clone`) can be passed as it is only when the callee doesn't keep the parameter (D1.11). Otherwise it must be passed with `move(x)`.
- **Inline values** (owner decision; decides Q6). Trivial values up to one word (8 bytes: `int`, `float64`, raw pointers) are stored inline, with no box, so interface values stay 2 words.
- **Type switches** and assertions use compile-time type descriptors. `case string:` also matches a `ref string` (and `case []T:` a `ref []T`); the bound value is a borrow either way. `case ref string:` is still allowed when the difference matters. Every name-binding form of an owning dynamic type binds a borrowed variable; `x.Clone()` gives an owned copy.
- For every type converted to an interface, the descriptor also carries a compact field table (field names, offsets, types, struct tags), emitted once per type (`linkonce_odr`). `fmt` and `encoding/json` read it, so printing and JSON work on any type as in Go.
- There is no user-facing `reflect` package in v1.

**D1.13 — Concurrency semantics.** `go`, `chan` and `select` work as in Go.
- `go f(a, b)`: trivial arguments are copied and owned ones are moved into the new goroutine. A raw pointer or ref into a local, or into the target of a local `Unique`, is a compile error outside `unsafe`. For example, `go srv.Run()` with a local `Unique[*Server]` is an error. Fix-it: `go func(){ srv.Run() }()`, whose captured variables live in shared cells (D1.11).
- Sending on a channel moves the value in; receiving moves it out. A channel and the values still buffered in it are freed automatically when nobody holds the channel; there is no `ch.Destroy()` (use `sync.Scope` when goroutines must finish first, P12).
- `go f(x)` moves owned locals; fields and borrowed variables need `x.Clone()` or `move(...)`.
- A send case in `select` moves its value only if that case is chosen. A temporary send value is dropped if its case is not chosen.
- The memory model is Go's happens-before model. **Data races are undefined behavior** (as in C++). `-race` (TSan) detects them.
- Program exit while goroutines are running: see D1.18.

**D1.14 — Macro and attribute surface** (semantics in P09).
- Definitions: `macro Name(p kind, …) { … }` (declarative) and `macro func Name(…) …` (procedural).
- Invocation: `#Name(args)` in expression, statement or declaration position. It expands in place. A code block is passed as a func literal, `#Repeat(3, func() { … })`; `return`, `break` and `continue` inside it are compile errors because the code is pasted inline. How `stmt`, `type` and `tokens` arguments are written is left to P09.
- Attachment: `@Name` or `@Name(args)` on top of a declaration (type, func, method, var or const) receives the arguments plus the declaration it sits on, and returns new code. Attributes are also allowed on struct fields, written after the field's type and before any Go tag: ``Name string @JSON("name") `db:"name"` ``.
- Arguments a macro reads as values must be known while compiling: every `@Name(args)` argument, and `#Name` parameters of a value kind (`lit`). A runtime variable there is a compile error (`@Retry(n)`). Code-fragment parameters (`expr`, `ident`, `block`, `stmt`) are pasted into the program and may name runtime variables (`#Assert(x > 0, "bad")`). The exact procedural-macro API is designed in P09.
- Names: every macro and attribute name starts with a capital letter (`#Assert`, `@Derive`, `@Extern`), whether built-in or user-defined.
- Across packages: a macro from another package is qualified like a function: `#pkg.Name(…)`, `@pkg.Name`.
- Predeclared: the std macros `#Assert`, `#Stringify`, `#File`, `#Line`, `#Embed`, `#Todo`, `#Unreachable`, `#Profile`, `#Setting` and `@Derive` need no import (`#Profile` and `#Setting` are specified in P09 D9.12).
- `@Derive(X)` finds `X` in std package `derive`, unless qualified (`@Derive(sql.Table)`).
- Built-in attributes use the same syntax: `@Extern("C")`, `@Export("C")`, `@Inline`, `@NoInline`, `@Packed`, `@Align(N)`, `@Ref`, `@NoBounds`, `@Pinned`, `@Unwind`, `@If(cond)` (P09 D9.12), `@Required` (struct fields, rule 11). `@Derive(…)` is a std macro (P09).
- Errors: a macro's error or panic becomes a compile error at the use site (P09 D9.11).
- `!` never means "macro", which keeps it unambiguous as Must.

**D1.15 — Foreign import surface** (semantics in P07/P08).
- **Imports.** Headers from C/C++ packages use the package path plus the header's path inside the package: `import "github.com/madler/zlib/zlib.h"`. System and standard-library headers use angle brackets inside the string, as in C: `import "<stdio.h>"`, `import "<vector>"`.
- **Classification.** The longest prefix over both `require` and `cdep` paths decides whether an import is a Go++ package or a header. The same path in both is an error.
- **Each header is a package named after the file**, used like any Go package. There is no `C.` or `cpp.` prefix:
  - `zlib.crc32(…)`, `stdio.printf(…)`, `vector.vector[int32]`, `algorithm.sort(…)`;
  - C/C++ names keep their spelling and are always accessible, whatever their case;
  - C++ namespaces are flattened into the package: `std::sort` from `<algorithm>` is `algorithm.sort` (P08 D8.1);
  - if the file name is a Go++ keyword or predeclared name (`<map>`, `<string>`, `<any>`), the import must be named: `import stdmap "<map>"`;
  - a file name that isn't an identifier (`foo-bar.h`) must be imported with a name too.
- **Order.** Header imports keep their source order. `gopp fmt` never reorders them, because C header order can matter. `import _ "<…>"` is allowed for order-only or side-effect imports.
- **No conversions in human code.**
  - C types are plain Go++ types (`int32`, `uint64`, `byte`, …; P07 D7.4).
  - String literals and owned strings pass to `const char*` parameters as they are: owned strings are kept NUL-terminated, so there is no copy. A `ref string`, a substring or a borrowed `string` parameter (which may be a substring) passes as a temporary NUL-terminated copy. Slices pass to pointer parameters (P07 D7.7).
- **Header extensions** are `.h`, `.hh`, `.hpp` and `.hxx`. This is the one list every phase uses.
- **Language.** For package headers, C or C++ comes from the package's CMake target (P11). For `<…>` headers, `.h` is C and anything else (`<vector>`, `.hpp`) is C++.
- **Headers inside a Go++ module** (yours or a dependency's) are header imports too: an import path inside a Go++ module that ends in a header extension names a header file, not a package (P10 D10.1). `.h` is C, the other extensions are C++. Flags come from the `cflags` and `cxxstd` directives in the gopp.mod of the module of the importing package (one Clang instance per package). A dependency's own `cxxstd` is a minimum: if the importer's is lower, the build fails with "module D needs c++23".
- **C/C++ settings live in `gopp.mod`, not in source files.** The module-wide directives `cxxstd c++20`, `cflags …` and `ldflags …` are set once per project, like Go's `go.mod` (P10). cdeps keep taking their flags from their own CMake target (P11). The only source directive left is `//gopp:build`, exactly like Go's `//go:build` (above `package`).

**D1.16 — Ten sample programs drive the spec.** Each is written in Go++ before implementation, alongside a Go counterpart and its expected output:

| # | Program | What it exercises |
|---|---|---|
| 1 | `hello` | the minimal program |
| 2 | `wc` | files, `!`, owned strings, refs |
| 3 | `kvparse` | parser that stores refs into one buffer; a `@Ref` token struct |
| 4 | `binarytrees` | allocation-heavy code; `Unique[*T]`, `new_unique`, arenas |
| 5 | `workers` | goroutine pool, channels, moves |
| 6 | `zcrc` | zlib through `import "github.com/madler/zlib/zlib.h"` |
| 7 | `vecsort` | `std::vector` + `std::sort` through `import "<vector>"` and `import "<algorithm>"` |
| 8 | `derive` | `@Derive`, `#Assert`, a user-defined `macro` |
| 9 | `orderedmap` | generic container with an owning `T` |
| 10 | `minicalc` | arena-allocated AST, refs into the source, `!` error handling |

Sample 8 (`derive`) needs P09 macros. P03 and M2 check the other samples.

**D1.17 — Safety defaults and layout.** These follow Go:
- integer overflow wraps;
- `int` is 64-bit on 64-bit targets;
- bounds and nil checks are on;
- integer division by zero panics.

Some operations are defined in Go but undefined in LLVM. Go++ gives them Go's results (P04 D4.12 lowers them):
- `MinInt / -1` gives `MinInt`, and `MinInt % -1` gives 0, with no trap.
- Shifts by ≥ the bit width give 0 (or -1 for `>>` of a negative signed value).
- Out-of-range float → int conversion saturates. The value is implementation-defined, never UB.
- Floating-point fusing (FMA) follows Go's compiler on each target, so results match Go.

Struct layout is declaration order with C alignment. That is C-compatible, which P07 requires, so fields are never reordered automatically.

**D1.18 — Program initialization and exit.**
- C/C++ static initializers from imported code run first, as part of C runtime startup.
- Then Go++ package `init`s run in import-dependency order, then `main.main`.
- **Exit.** After `main.main` returns and its deferred calls have run, the runtime:
  1. stops all other goroutines (P12 D12.12);
  2. drops `main`'s locals;
  3. calls C `exit()`, which runs atexit handlers, flushes stdio and runs C++ static destructors.

  This is safe because no Go++ code is running any more.
- **`os.Exit(code)`** skips defers and drops, stops the goroutines, and calls `exit(code)`.
- **Unrecovered panic.** The trampoline prints the panic, stops the other goroutines, and calls `exit(2)` (P04 D4.6). If the panic leaves `main.main`, the other goroutines are stopped before `main`'s locals are dropped, as at a normal exit.
- **Package-level variables are never destroyed** (as in Go). That avoids C++'s static-destruction-order problems. Buffered writers must be flushed explicitly, as in Go.

**D1.19 — Errors work like Go.**
- `error` is an interface, so it is a shared handle (D1.3, D1.12). `return ErrNotFound` shares the same error, and `err == io.EOF`, `errors.Is`, `%w` and type switches work as in Go.
- Package-level errors from `errors.New` are static: never freed, never counted. Other error values are reference-counted.
- **The `!` trace** is stored per goroutine, keyed by the error's identity, outside the error. It never changes `==`, type switches or `Error()`. It is printed by a `!` panic and by `%+v`. After an error crosses goroutines, its trace is best effort.

**D1.20 — Owned slices: re-slicing in place** (owner decision).
- `s = s[i:j]` on the same owned slice shrinks it in place: it drops the elements outside `[i, j)` and keeps the capacity.
- For `i > 0`, the remaining elements move down (O(n)), so a slice needs no extra offset field. Its layout is in D1.22.
- For fast FIFO queues, std has `container/deque`.
- Reset a buffer with `buf = buf[:0]`.
- Append-style functions take `move dst []T`: `buf = strconv.AppendInt(buf, n, 10)` moves implicitly (D1.4 rule 4).
- `cap()` of a borrowed slice equals its length.
- Re-slicing past the length (`s = s[:0]; s = s[:2]`) panics: the items were already freed.

**D1.21 — Range loops** (decides Q7).
- `for i, x := range xs` borrows each owning element, read-only (like Rust's `for d in &docs`). Trivial elements are copied, as in Go; shared-handle elements are copied with a retain.
- **`for _, &d := range xs`** is the new form: `d` is the element itself, so writes change the collection. It works for slices, arrays, map values (`for k, &v := range m`), C++ containers and iterators that yield `*T`. The key can't use `&`. The plain form's error message suggests `&d` first. `m[k].f = x` outside a loop stays illegal, as in Go.
- `move(v)` on a plain (read-only) range variable is an error (fix-its: `&v` or `v.Clone()`). Through `&v` it takes the value out and leaves the zero value in the element (for a map, the key stays).
- `for _, x := range move(xs)` moves the elements out one by one. If the loop stops early, the unreached items are freed in order.
- Inside the loop body, a statement that can reallocate the ranged slice or map is a compile error when the compiler can see it: appending to it, assigning it, inserting into the map, or calling `clear` on it. `delete` is allowed. Deleting the current map entry keeps `k` and `v` valid to the end of the turn. Growing a slice where the compiler can't see it stays undefined behavior; `gopp -asan` finds it.
- Writing the current item and then reading the borrowed (plain) loop variable is a compile error.
- **Map inserts.** Maps count their active iterators. Inserting a new key into a map while a `range` over it runs **always panics**, with "map insert during iteration", not only when the table must grow. Visible inserts stay a compile error.
- Go allows inserting into a map during `range`, so this is a listed difference from Go (P04 D4.10).
- **Arrays.** A range over an array of trivial elements copies it, like Go. A range over an array of owning elements is not copied.
- **C++ containers** work like slices: `for _, x := range vec` is a read-only borrow of each element, and `for _, &x := range vec` changes the real element (like C++ `auto&`). The one-variable form `for x := range c` gives the index for indexable containers and the key for associative ones (`std::map`); for containers with neither (`std::list`) it is a compile error, so use `for _, x := range c`. For associative containers the two-variable form gives the key and the mapped value, like a Go map (`for k, v := range cppmap`; `&v` aliases the mapped value).
- **Range over a function** follows the slice-loop rules: `for x := range seq` borrows each value read-only (`x.Clone()` to keep it). `for &x := range seq` changes the real item and is allowed only when the iterator yields pointers (`iter.Seq[*Doc]`). Calling `yield` after the loop has ended panics, as in Go.
- **No redeclared loop variable.** Declaring a variable with the same name as a loop variable inside the loop body (`u := u`, `u := u.Clone()`) is a compile error.

```go
for _, &d := range docs { d.Title = "x" }       // changes docs[i].Title
for k, &v := range m { v.Count++ }              // changes the map value in place
for _, d := range docs { u := d.Clone() }       // ok; `d := d` would be an error
```

**D1.22 — Slices and maps carry their allocator** (owner decision; decides Q4).
- An owned `[]T` is 4 words: pointer, length, capacity and allocator. A map header also stores its allocator.
- A nil allocator means the global allocator, so zero values stay valid. `make`, `append`, literals and conversions use the global allocator unless told otherwise. This nil is the hidden allocator word of the header, which Go++ code never sees. In Go++ code an allocator is a `mem.Allocator`, an interface with methods, so it is never nil (ADR-0018 amendment point 3); an allocator parameter or field that may be absent is written `Option[mem.Allocator]`, nil meaning the global allocator.
- `mem` functions create a slice or map in another allocator, such as a `mem.Arena` (P05 D5.3).
- Growth (`append`), `Clone` and drop use the value's own allocator; a clone stays in the same allocator.
- Refs and borrowed `[]T` parameters stay `(ptr, len)`: reading and writing elements needs no allocator. A `move` `[]T` parameter passes all 4 words.
- `string` always uses the global allocator, so strings stay 3 words.
- The allocator word points to an allocator object whose first word is its function table, so one pointer is enough.

*Why:* arena- and pool-backed collections keep the same `[]T` and `map` types everywhere.
*Cost:* one more word in every owned slice and map header.

## Deliverables

- `docs/spec/`: overview, lexical, types, ownership, parameters, expressions, statements, slices-and-range, errors-and-panics, generics, concurrency, macros-surface, foreign-surface, program-init, undefined-behavior.
- `docs/spec/examples/`: the ten samples, each with a Go counterpart and its expected output.
- ADR-0003 (memory model) and ADR-0004 (`!`) updated to the final text, and ADR-0006 recording the P01.15 owner decisions that changed the plan.

## Exit criteria

- Every delta D1.2–D1.21 has grammar, semantics, an example and a rationale in `docs/spec/`.
- All ten samples are written, annotated with expected output, and compared against their Go versions for readability.
- Q1–Q8 are decided, or explicitly deferred to a named phase.
- The list of undefined and implementation-defined behavior is written.

## Risks

- **The ownership model reads poorly in real code** → write the samples first. After M2 there is a spec-revision window (P05) before v1 freezes anything.
- **Spec ambiguities found during implementation** → the spec is versioned (`gopp 0.1` in gopp.mod), and fixes go through ADRs.
- **Too many new keywords** → only `macro` is a reserved keyword (`ref` and `move` are contextual); owning pointers are predeclared generic types (D1.7).

## Open questions (close in P01.12 unless noted)

- **Q1** (decided 2026-10-27) Closure capture rule: non-escaping closures capture by reference; escaping closures keep captured variables in shared cells, like Go (D1.11).
- **Q2** (decided 2026-10-27) Map element access with owning values: `m[k]` borrows where a borrow is enough; taking ownership needs `clone(m[k])` or `move(m[k])`, both with a comma-ok form (`v, ok := clone(m[k])`); `m[k].f = x` stays illegal, as in Go (D1.4 rule 3a).
- **Q3** (decided 2026-10-27) Owning pointers are predeclared generic types `Unique[*T]`, `Shared[*T]`, `Weak[*T]`, created with `new_unique`, `new_shared`, `new_weak` (D1.7).
- **Q4** (decided 2026-10-27) Allocators for `[]T`/`map`: every owned slice and map stores its allocator; nil means the global one (D1.22).
- **Q5** (decided 2026-10-27) Generic methods on concrete types: allowed; they can't satisfy interface methods (D1.10).
- **Q6** (decided 2026-10-27) Interface inline storage: one word (8 bytes), trivial values only (D1.12).
- **Q7** (decided 2026-10-27) Ranging over owning elements: `for _, x := range xs` borrows each element, and `for _, x := range move(xs)` moves them out (D1.21).
- **Q8** (deferred to P15, P15 Q1) Small-string optimization for `string`? Proposed: no in v0. The layout is ABI, so P15 decides before v1.0.
- **Q9** (decided 2026-10-27, ADR-0016) How is a map read (and a type assertion made) when the value type has no zero value? `m[k]` and `<-ch` may be used only in comma-ok forms, as a write target or in `delete`; the comma-ok forms work, and `u` counts as moved wherever `ok` is false, so the use-after-move check stops any read there (D1.4 rule 11).
