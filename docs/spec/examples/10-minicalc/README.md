# 10 minicalc

A recursive-descent calculator. The AST lives in an arena, number nodes keep views into the source, and parse and eval errors use `!`. Run: `go run main.go`.

## Spec rules exercised

- [ERR-1, ERR-2, ERR-5, ERR-6](../../09-errors-and-panics.md): `func (p *Parser) expr!() *Node`, methods that can fail; `p.term!()` returns the error to the caller; `calc!(&arena, "6 * 7")` in `main` panics on failure; `calc(&arena, src)` is the Go form.
- [ERR-3](../../09-errors-and-panics.md): `return n` is a success; `return nil, p.fail(...)` and `return 0, errors.New(...)` fail explicitly.
- [ERR-2](../../09-errors-and-panics.md): `fail` returns `error` as its last result, so it is a normal function that returns an error value.
- [OWN-C1, OWN-V5](../../04-ownership.md): `Node` and `Parser` are trivial; their view fields need `@View`.
- [OWN-V2](../../04-ownership.md): `p.src[start:p.pos]` is a view; `p.src[p.pos:p.pos+1]` is a view for `%q`.
- [PAR-2, PAR-10](../../05-parameters.md): `src string` is borrowed, so string literals and borrowed range strings pass without copies; the `Parser` keeps a view of it in a `@View` struct.
- [SLC-3](../../08-slices-and-range.md), [UB-5](../../16-undefined-behavior.md): the arena owns every `Node`; `arena.Reset()` makes the previous tree dangle (it is not used again).
- [PAN-3](../../09-errors-and-panics.md): `defer arena.Free()` evaluates `&arena` at the `defer`, and `arena` lives to the end of `main`; later `arena.Reset()` calls are legal and seen by the deferred call.
- [RNG-1](../../08-slices-and-range.md): `range exprs` borrows each string.

## Differences from the Go version

- Go allocates nodes with `&Node{}` and the GC frees them. Go++ uses `mem.ArenaNew[Node]` and one `Reset`.
- `Text` is a `string` in Go and a `view string` in Go++.
- Go returns `(*Node, error)` and checks `err` after every call; Go++ uses `!`.
- Output is identical.

## Assumptions about other phases

- `mem.Arena` has a usable zero value; `Reset` and `Free` take a pointer receiver (P05 D5.3 / P05.03).
- `mem.ArenaNew[Node]` accepts `Node` because it is trivial (D5.3).
- The package path is written `"mem"`.
