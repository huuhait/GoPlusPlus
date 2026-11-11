# ADR-0041: A value is a copy; moving and sharing are written out

- **Status:** accepted (owner decision). Rules 8 to 14 below are proposed by this ADR and used by the spec; the owner may change them.
- **Date:** 2026-11-11
- **Plan reference:** P01.19; reverses part of [ADR-0003](0003-memory-model-direction.md) (see Context); replaces [ADR-0040](0040-unshared-is-proven-at-compile-time.md) and the shared-buffer part of [ADR-0032](0032-shared-buffers-and-borrow-engine.md); changes OWN-C2, OWN-R3, OWN-R3d, OWN-P1, OWN-S2, PAR-1, SLC-6 to SLC-9, STM-7, TYP-6, TYP-7, CON-1, CON-4, CON-14

## Context

The spec gave slices and maps Go's behavior: `t := s` makes a second name for the same elements, with a reference count underneath. Every other everyday value either is copied (`int`, plain structs), behaves like a copy (`string`), or is moved (`Unique[T]`, files). Slices and maps were the only values where two names could change one thing without the program saying so.

That one exception produced a growing set of rules: when a shared slice may be sent to another goroutine (ADR-0040), what shortening a shared slice does, which elements a shared buffer releases when it dies, and Go's own surprise that `t := s` shares the elements but not the length.

The owner decided to remove the exception rather than refine it, and to do it the way C++ does.

**This reverses an earlier decision.** [ADR-0003](0003-memory-model-direction.md), the founding memory model, made slices and maps shared handles, said that every heap allocation is spelled out and that deep copies are explicit (`Clone()`), and listed "C++-style implicit deep copies: hidden allocations" among the rejected alternatives. The spec repeats it ("Go++ never performs a hidden deep copy", TYP-6). This ADR chooses that alternative on purpose: a rule with no exception is worth more to the owner than copies that are always visible. The rest of ADR-0003 stands: no garbage collector, deterministic destruction, single-owner types, borrow checking without annotations.

## Decision

1. **A value is a copy.** `t := s`, `t = s`, storing into a field or an element, returning, and passing an argument each give the destination its own value, for every type. After `t := s`, nothing done to `t` is seen through `s`.
2. **Moving is written `move(x)`.** It hands the value of `x` to the destination without copying. `x` cannot be read afterwards until it is assigned again; reading it is a compile error.
3. **Sharing is written with a type.** `p := &x` gives a pointer to the same variable (`*p` is `x`); `NewShared(x)` gives shared ownership. `*[]T` and `*map[K]V` are ordinary pointer types.
4. **A parameter is the function's own copy.** Changing it does not change the caller's variable. A function that changes the caller's value takes a pointer, and the caller writes `&`:
   ```go
   func addOne(xs []int)  { xs[0]++ }        // changes its copy
   func addOne(xs *[]int) { xs[0]++ }        // changes the caller's slice

   addOne(nums)     // nums is unchanged
   addOne(&nums)    // nums changes, and the call shows it
   ```
5. **A copy that cannot be noticed is not made.** When the source is not used again, the copy is done as a move. When a function only reads a parameter, the argument is passed without copying. A program cannot tell the difference and must not depend on how many copies are made.
6. **A type that cannot be copied** (a file, a mutex, anything with cleanup and no way to duplicate) makes a plain `t := s` a compile error. The fix-its are `move(s)` and `&s`.
7. **A pointer to a slice, a map or an array is used without writing `*`** wherever only the pointed-to value fits: indexing (`xs[0]`, `m[k]`), slicing (`xs[i:j]`), `range xs`, `len`, `cap`, `delete` and `clear`. This is what field access and method calls already do for a pointer to a struct. Only the whole value is written with the star: `*xs = append(*xs, 4)`, `*xs = nil`.

What a copy is, by type:

| Type | Copy |
|---|---|
| numbers, `bool`, structs and arrays of them | the bytes |
| `string` | the same bytes; they never change, so sharing them cannot be seen |
| `[]T`, `map[K]V` | a new buffer with a copy of every element |
| struct, array | each field or element by its own rule |
| `*T`, `Shared[T]`, `Weak[T]`, `chan T`, function values | the handle; what it refers to is shared, which is what these types are for |

## Consequences

- One rule for every type. Two names change one thing only when a type says so: a pointer, `Shared[T]`, a channel.
- **A slice or a map always has exactly one owner.** Their buffers need no reference count. Sending one on a channel, passing it to `go` and storing it in an interface are plain copies or moves, so the "unshared" rule of ADR-0040 and OWN-R3d is no longer needed.
- Shortening a shared slice, and which elements a shared buffer releases, are no longer questions: there is no shared slice.
- **The promise "never a hidden deep copy" and "every heap allocation is spelled out" no longer holds** for assignment and argument passing; it is removed from the spec with the rewrite.
- **Copies can be large and are not visible in the source.** `t := s` on a big slice copies it when `s` is used again. A tool should be able to list the copies a program makes.
- **Go code changes meaning in two places, without an error.** A function that changes a slice or map it received by value now changes a copy (`sort.Ints(nums)` becomes `sort.Ints(&nums)`, `file.Read(buf)` becomes `file.Read(&buf)`), and two variables assigned from each other no longer see each other's changes. The library's signatures take pointers where Go's functions change their argument.
- The samples in `docs/spec/examples` and their notes must be re-read against this rule.

## Rules the documents follow

These spell the decision out far enough to rewrite the spec. Rules 8 to 14 answer points the owner had not decided when the rewrite was asked for; they are this ADR's proposal and the owner may change any of them.

**Copying**

1. *Where a copy happens.* Initialization and assignment (`t := s`, `t = s`), storing into a field, an element or a map entry, passing an argument to a by-value parameter or receiver, returning, sending on a channel, passing to `go`, and converting to an interface each give the destination its own value.
2. *What a copy is.* Plain data: the bytes. `string`: the same immutable bytes, counted. `[]T` and `map[K]V`: a new buffer and a copy of every element or entry, by these same rules. Structs, arrays and `Option`: each part by its own rule. `*T`, `Shared[T]`, `Weak[T]`, `chan T` and function values: the handle. An interface value: the same box, which is immutable and counted. `Unique[T]`: a new allocation holding a copy of what it points to. A C++ class with a usable copy constructor: that constructor. A copy of a slice or map lives in the allocator of its source; its capacity is at least its length and otherwise not specified.
3. *Which types can be copied.* A type with a `drop` method can be copied only if it defines `Clone() T`; the copy is then that method, also when the type holds a part that could not be copied by itself. `@Pinned` types, C++ classes without a usable copy constructor, and any type that holds a part that cannot be copied cannot be copied. A plain copy of such a value is a compile error; the fix-its are `move(x)` and `&x`. Returning a local or a by-value parameter, and using a temporary (a call result, a literal), need no `move`. This holds on the last use too: for a value that cannot be copied, the hand-over is always written.
4. *Copies that are not made.* The compiler skips a copy when the program cannot tell: the source is a local that is not read again (its last use), so the value is handed over; or the function only reads a by-value parameter and the argument cannot change during the call, so the argument is passed as it is. `s = append(s, x)`, `s = s[i:j]` and `m[k] = append(m[k], v)` therefore work in place; they copy nothing, so they are also allowed on a slice whose elements cannot be copied. A user-defined `Clone` may be skipped the same way, so it must not have effects a program relies on.
5. *`.Clone()` and `clone(x)`* remain. They do what `=` does and are the way to write "this copy is meant". The constraint `Clone` means "can be copied"; `Copy` means "is copied by copying its bytes".

**Moving**

6. *`move(x)`* is a predeclared function. Its operand is a local variable or a parameter. The result is the value; no copy is made. `x` cannot be read afterwards until it is assigned again (`E0451`). Taking a value out of a field, an element or a map entry stays `mem.Take` and `mem.Replace`.

**Sharing**

7. *Pointers.* `&x` gives a `*T` that is `x`. `*[]T` and `*map[K]V` are ordinary pointer types. A pointer to a slice, map or array is used without `*` for indexing, slicing, `range`, `len`, `cap`, `delete` and `clear`; the whole value is written `*p`. The same holds through `Unique[T]` and `Shared[T]`, as it does for field access. `NewShared(x)` gives shared ownership.
8. *Part of a slice as the same elements.* `&s[i:j]` (and `&arr[i:j]` on an array) is a `*[]T` that covers elements `i` to `j` of `s` itself. It has no spare capacity (`cap == len`), it is checked like every borrow (it cannot outlive `s`, and `s` cannot be assigned, grown or moved while it is in use), and growing or replacing `*p` never changes `s`. `s[i:j]` without `&` is a value: a new slice with copies of those elements, subject to rule 4. On a `string`, `s[i:j]` shares the immutable bytes as before.
9. *A function that changes its copy to no effect.* Writing to an element or entry of a by-value slice or map parameter (assignment, `delete`, `clear`, an `append` assigned back to the parameter) when the parameter is not read, returned or passed on afterwards is a compile error: the write is lost. The fix-its are a pointer parameter and returning the value.
10. *Built-ins and the library take a pointer where they change their argument:* `copy(&dst, src)`, `sort.Ints(&xs)`, `r.Read(&buf)`, `r.Read(&buf[n:])`. `append(s, x)` takes `s` by value and returns the result.

**Other forms**

11. *Closures* keep Go's rule: a closure uses the variable itself, as if through `&x`. The capture rules of chapter 11 stand; where they spoke of sharing or retaining a slice or map, the closure simply refers to the variable.
12. *`range`.* The plain variable is a copy of the element, not made when the body only reads it. For elements that cannot be copied the plain variable is a read-only view, as before. `for _, &x := range xs` is the element itself.
13. *Slice and map layout.* A `[]T` stays four words, `{ptr, len, cap, backing}`. `backing` points to a header `{alloc_cap, allocator}` at the start of the buffer; it is `nil` for a nil or empty slice and for a `&s[i:j]` view. There is no reference count: the one owner frees the buffer and releases exactly the elements `ptr` to `ptr + len`. Elements cut off by `s = s[i:j]` are released at that statement. A `map[K]V` stays one word, a pointer to a header without a reference count.
14. *Goroutines and channels.* `go f(x)` and `ch <- x` give the other side a copy, or the value itself with `move(x)` or on its last use. Two goroutines share data only through a type made for it (`Shared[T]` with a `sync` type, a channel, a `sync.Scope` borrow). A slice or map of `Send` elements is `Send`.

Reference counts remain for string buffers, `Shared[T]` and `Weak[T]`, interface boxes, function values and channels. The words "retain" and "release" apply only to those.

## Open points

Found while rewriting the documents. The spec keeps its earlier text on each until it is decided.

1. ~~A pointer to an element of a slice that lives inside a `Shared[T]`~~: decided on 2026-11-11, [ADR-0042](0042-shared-is-read-only.md). What a `Shared[T]` holds is read-only through the handle; a part that changes is in a `Mutex`. A slice reached through an arena alias is still open (OWN-B4).
2. **An interface value in two goroutines.** A copy of an interface value is the same box (rule 2). If the boxed type is not `Sync`, a sender that keeps its variable would share the box with the receiver. The chapters keep the earlier restriction in new words: such a value crosses only as a temporary, on its last use or with `move(x)` (CON-1, CON-4, CON-12, CON-14).
3. **Who makes the copy at a call.** Whether a function only reads a parameter is a fact about its body, but the calling convention is fixed by the signature, and a call through an interface or a function value sees no body. One function body has to serve both. What "only reads" covers (re-slicing the parameter, storing it, returning it) and whether it is the same analysis as "does not keep" for interface parameters (TYP-7) is part of this (PAR-1, `calling-convention.md`, `gir.md`).
4. **Where the slice record of a view lives.** `&s[i:j]` is a `*[]T`, so it points at a four-word record somewhere. If code appends through it, that record owns a new buffer, and something must free it.
5. **Assertion bindings.** `x := v.([]int)` stays a read-only binding for now (TYP-9, OWN-R3b); by rule 1 it could be a copy.
6. **Library shapes that depended on the old model:** what `Split` returns (two views), `Mutex[T].With` for a slice or map `T`, and a `[]E` satisfying a `SetIndex` constraint by value (CON-15, CON-16, GEN-13).
7. **`move(x)` in two corners:** its order among the other arguments of a call (`f(move(x), len(x))`), and a `select` send case that is not chosen (EXP-3, CON-5).
8. **Error codes** for a plain copy of a value that cannot be copied and for a write to a copy that is lost (rule 9). Both are "a compile error" until the checker assigns them.

## To do

The spec chapters, the ADRs that carry a status note, the design documents and the samples follow these rules as of 2026-11-11. Left: the open points above, the checker and the runtime (P03, P04, P05), and the library signatures (P13).

## Alternatives

- **Keep Go's shared slices and maps:** closest to Go, but keeps the one exception and every rule that guards it.
- **Move by default, `Clone()` to copy** (Rust): no hidden copy, and code that would behave differently from Go fails to compile instead. Rejected by the owner in favor of copies by default with an explicit `move`.
- **One shared object, length included** (Python, Java): `t := s` would be the same list, and `s = s[:2]` would shorten it for every holder. Considered and dropped: it gives `s = s[:2]` a meaning no other language gives that syntax.
- **Copy on first write** (Swift): looks like a copy, but the copy happens at a moment the source does not show and needs a count read at run time.
