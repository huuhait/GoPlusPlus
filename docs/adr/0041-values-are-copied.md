# ADR-0041: A value is a copy; moving and sharing are written out

- **Status:** accepted (owner decision). The spec chapters named under "To do" still describe the earlier model until they are rewritten.
- **Date:** 2026-11-11
- **Plan reference:** P01.19; replaces [ADR-0040](0040-unshared-is-proven-at-compile-time.md) and the shared-buffer part of [ADR-0032](0032-shared-buffers-and-borrow-engine.md); changes OWN-C2, OWN-R3, OWN-R3d, OWN-P1, OWN-S2, PAR-1, SLC-6 to SLC-9, STM-7, TYP-6, TYP-7, CON-1, CON-4, CON-14

## Context

The spec gave slices and maps Go's behavior: `t := s` makes a second name for the same elements, with a reference count underneath. Every other everyday value either is copied (`int`, plain structs), behaves like a copy (`string`), or is moved (`Unique[T]`, files). Slices and maps were the only values where two names could change one thing without the program saying so.

That one exception produced a growing set of rules: when a shared slice may be sent to another goroutine (ADR-0040), what shortening a shared slice does, which elements a shared buffer releases when it dies, and Go's own surprise that `t := s` shares the elements but not the length.

The owner decided to remove the exception rather than refine it, and to do it the way C++ does.

## Decision

1. **A value is a copy.** `t := s`, `t = s`, storing into a field or an element, returning, and passing an argument each give the destination its own value, for every type. After `t := s`, nothing done to `t` is seen through `s`.
2. **Moving is written `move(x)`.** It hands the value of `x` to the destination without copying. `x` cannot be read afterwards until it is assigned again; reading it is a compile error.
3. **Sharing is written with a type.** `p := &x` gives a pointer to the same variable (`*p` is `x`); `NewShared(x)` gives shared ownership. `*[]T` and `*map[K]V` are ordinary pointer types.
4. **A parameter is the function's own copy.** Changing it does not change the caller's variable. A function that changes the caller's value takes a pointer, and the caller writes `&`:
   ```go
   func addOne(xs []int)  { xs[0]++ }        // changes its copy
   func addOne(xs *[]int) { (*xs)[0]++ }     // changes the caller's slice

   addOne(nums)     // nums is unchanged
   addOne(&nums)    // nums changes, and the call shows it
   ```
5. **A copy that cannot be noticed is not made.** When the source is not used again, the copy is done as a move. When a function only reads a parameter, the argument is passed without copying. A program cannot tell the difference and must not depend on how many copies are made.
6. **A type that cannot be copied** (a file, a mutex, anything with cleanup and no way to duplicate) makes a plain `t := s` a compile error. The fix-its are `move(s)` and `&s`.

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
- **Copies can be large and are not visible in the source.** `t := s` on a big slice copies it when `s` is used again. A tool should be able to list the copies a program makes.
- **Go code changes meaning in two places, without an error.** A function that changes a slice or map it received by value now changes a copy (`sort.Ints(nums)` becomes `sort.Ints(&nums)`, `file.Read(buf)` becomes `file.Read(&buf)`), and two variables assigned from each other no longer see each other's changes. The library's signatures take pointers where Go's functions change their argument.
- The samples in `docs/spec/examples` and their notes must be re-read against this rule.

## Open points

Each needs a decision before the chapter that holds it is rewritten.

1. **Part of a slice without a copy.** Go reads into and writes part of a buffer through a sub-slice (`io.ReadFull(r, buf[n:])`, `copy(dst[i:], src)`). With copies, `buf[n:]` in a new variable is a new slice. A way to name a part of a slice as the same elements is needed, for example `&buf[n:]`.
2. **What a closure captures.** In Go a closure sees the variable itself. Either that stays (a capture is like `&x`), or a capture is a copy.
3. **A change to a copy that has no effect.** A function that writes to a by-value slice or map parameter and never uses it again changes nothing. Decide whether that is a compile error or a `gopp vet` warning.
4. **`Unique[T]`.** Whether `u2 := u` copies what `u` points to, or `Unique[T]` cannot be copied.
5. **Interface values.** The box an interface points to is immutable (TYP-6), so copying the interface can share it, as for strings.
6. **The layout of a slice** once its buffer has no reference count (ADR-0029 gave it four words with a `backing` header).
7. **`.Clone()` and `clone`.** They now say what `=` already does. Keep them as the spelling for "I mean this copy", and for the `Clone` constraint.

## To do

Rewrite, in this order: chapter 4 (ownership), 5 (parameters), 8 (slices and range), 3 (interface conversion), 7, 11 and 12 (send, closures, goroutines), the generics wording of `Copy` and `Clone` in chapter 10; then `gir.md`, `calling-convention.md` and `debug-info.md` for the slice layout and how arguments are passed; then the samples.

## Alternatives

- **Keep Go's shared slices and maps:** closest to Go, but keeps the one exception and every rule that guards it.
- **Move by default, `Clone()` to copy** (Rust): no hidden copy, and code that would behave differently from Go fails to compile instead. Rejected by the owner in favor of copies by default with an explicit `move`.
- **One shared object, length included** (Python, Java): `t := s` would be the same list, and `s = s[:2]` would shorten it for every holder. Considered and dropped: it gives `s = s[:2]` a meaning no other language gives that syntax.
- **Copy on first write** (Swift): looks like a copy, but the copy happens at a moment the source does not show and needs a count read at run time.
