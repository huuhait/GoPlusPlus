# 10 minicalc

A recursive-descent calculator. The AST lives in an arena, number nodes record trivial byte spans into the source string, and parse and eval errors use `!`.

## Spec rules exercised

- [ERR-1, ERR-2, ERR-5, ERR-6](../../09-errors-and-panics.md): `func (p *Parser) expr!() *Node`, methods that can fail; `p.term!()` returns the error to the caller; `calc!(&arena, "6 * 7")` in `main` panics on failure; `calc(&arena, src)` is the form with the error as a result.
- [ERR-3](../../09-errors-and-panics.md): `return n` is a success; `return nil, p.fail(...)` and `return 0, errors.New(...)` fail explicitly. The `nil` in `return nil, p.fail(...)` stands for the unset `*Node` result, which no caller can read (pointers are never nil, ADR-0018).
- [OWN-P9, OWN-P10](../../04-ownership.md): `L, R Option[*Node]` (a missing child is `nil`); `operand!` checks `o == nil` and then uses `o` as a `*Node`.
- [ERR-2](../../09-errors-and-panics.md): `fail` returns `error` as its last result, so it is a normal function that returns an error value.
- [OWN-C1, OWN-C2, OWN-B1, OWN-B5](../../04-ownership.md#45-borrow-checking-adr-0021): `Node` (`Kind`, `int64`, `Span` and `Option[*Node]`) has no destructor glue and is borrow-carrying inside `arena`, so `mem.ArenaNew[Node]` bulk-frees all nodes when `arena` resets or drops; `Parser` is a stack borrow-carrying struct holding `src string` and `arena *mem.Arena`.
- [OWN-S1](../../04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032): `p.src[p.pos:p.pos+1]` slices `p.src` in $O(1)$ for `%q` with zero heap allocation.
- [PAR-1](../../05-parameters.md): `src string` is a by-value parameter; a string copy shares the immutable bytes, so string literals and range strings pass without copying bytes.
- [SLC-3](../../08-slices-and-range.md), [OWN-B2](../../04-ownership.md#45-borrow-checking-adr-0021), [UB-5](../../16-undefined-behavior.md): `mem.Arena` frees its chunks automatically on drop (`SLC-3`); `arena.Reset()` reuses chunks per expression and ends borrows of the previous tree (checked in safe code by `OWN-B2`).
- [PAN-3](../../09-errors-and-panics.md): `defer arena.Free()` evaluates `&arena` at the `defer`, and `arena` lives to the end of `main`; later `arena.Reset()` calls are legal and seen by the deferred call.
- [RNG-1](../../08-slices-and-range.md): `range exprs`: the body only reads `src`, so the string element is not copied.
