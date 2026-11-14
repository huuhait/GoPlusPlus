# ADR-0042: What a `Shared[T]` holds is read-only; changing it needs a `Mutex`

- **Status:** accepted (owner decision)
- **Date:** 2026-11-11
- **Plan reference:** P01.19 (open point 1 of [ADR-0041](0041-values-are-copied.md)); spec OWN-P8, OWN-B9 rule 5, CON-19, UB-2; replaces the "writable in one goroutine" `Shared[T]` of [ADR-0032](0032-shared-buffers-and-borrow-engine.md), of decision 6 of [ADR-0021](0021-borrow-checking.md) and of decision 1 of [ADR-0022](0022-race-free-concurrency.md)

## Context

`Shared[T]` is the type for several names that are the same object. The spec let a `Shared[T]` used in one goroutine be changed directly through any of its handles (`p.Children = append(p.Children, c)`), and required a `Mutex` only across goroutines.

That was safe because a slice buffer had a reference count: a pointer into the slice kept the old buffer alive when another handle grew it. ADR-0041 gave a slice one owner and no count, so this is no longer safe:

```go
a := NewShared(Node{Items: []int{1, 2, 3}})
b := a                          // the same Node

first := &a.Items[0]
b.Items = append(b.Items, 4)    // the slice moves to a new buffer; the old one is freed
first = 9                       // writes into freed memory
```

The compiler cannot catch it by looking for pointers, because the same thing happens inside a method that received an ordinary `*Node`: nothing there says that the node is shared.

## Decision

1. **What a `Shared[T]` (or a `Weak[T]`) holds is read-only through the handle, all the way down.** Reading a field or an element, `len`, `range`, copying a part out, calling a method with a value receiver, and calling a method with a pointer receiver that only reads through it are allowed. Assigning a field or an element, `append`, `delete`, `clear`, `mem.Take`, `mem.Replace` and calling a method that writes through its receiver are compile errors. A pointer taken into the value (`&a.f`) is a read-only binding ([OWN-R3b](../spec/04-ownership.md#42-ownership-rules-d14)).
2. **To change shared data it must be in a `Mutex[U]` (or `RWMutex[U]`, `Atomic[U]`, a channel), and the change is made while holding the lock.** Either the whole value (`Shared[Mutex[Node]]`) or the parts that change (`Children Mutex[[]Shared[Node]]`).
   ```go
   a := NewShared(sync.NewMutex(Node{Items: []int{1, 2, 3}}))
   b := a

   g := b.Lock()
   g.Items = append(g.Items, 4)
   ```
3. **Both are checked by the compiler.** A write through a `Shared[T]` does not compile, and a value inside a `Mutex` can be reached only through its guard.
4. **One rule for one goroutine and for several.** The special case of a `Shared[T]` that is writable in a single goroutine is removed. `Shared[T]` is `Send` and `Sync` when `T` is `Sync`, and its reference count is atomic exactly then.
5. **Any field type may sit in a `Shared[T]`.** The earlier ban on a `Unique[U]`, a value with `drop` or a C++ class directly inside a `Shared[T]` existed because another handle could overwrite the field; nothing can now.

## Consequences

- The dangling pointer of the example cannot be written: the `append` through `b` is a compile error, and with a `Mutex` the two uses need the same lock.
- **A tree or graph that is changed after it is built needs a `Mutex`, also in a single goroutine.** That is more to write, and an uncontended lock on every change.
- **Taking the same lock twice at the same time makes the program wait forever.** The memory stays valid. The compiler reports the cases it can see (CON-20); two handles to one object are not among them.
- Which pointer-receiver methods "only read" is a fact the compiler infers for each function and exports, as it does for parameters (ADR-0041 rule 4). A call through an interface or a function value has no such fact, so it cannot take a pointer into a `Shared[T]`.
- A `@Virtual` or `@Abstract` method is called on what a `Shared[P]` holds by what the method of `P` does (one without a body only reads). The method that runs is a child's, so a child whose method writes where the parent's only reads is not taken as that parent in a `Shared`: the error is where the `Shared[Child]`, or a pointer into it, is converted or lent as the parent, or a method of the parent is called on it. A child may write in its methods; with one owner it is used as its parent freely (decided 2026-11-14).
- Data that is built once and then only read is shared with no lock at all.

## Alternatives

- **Allow writes, forbid pointers into a slice inside a `Shared[T]`:** not enough. A method with a pointer receiver gets an ordinary pointer and can take an element pointer where the compiler cannot see the sharing.
- **Allow everything and stop the program** when a slice is grown or replaced while a pointer into it is in use: safe and little to write, but the error comes at run time.
- **Keep a reference count on slice buffers inside a `Shared[T]` only:** brings back the exception ADR-0041 removed.
