# The corpus of programs that do what Go does

Each program here is valid in Go and in Go++, and does the same in both ([differences-from-go.md](../../docs/design/differences-from-go.md)). The directories are its groups: `scalar`, `collection`, `generic`, `closure`, `interface`, `panic`, `init`.

- `name.gopp` is the program. Go builds the same text as `main.go`.
- `name.out` is what Go says the program does: what it writes to both streams, in the order of the writes, up to the trace of a panic, and a last line `exit status N` if it ends with another status than 0. Go writes this file; nothing else does.

The tests are in `crates/gopp-cli/tests/run.rs`:

```
nix develop -c timeout 60 cargo test -p gopp-cli --test run corpus
nix develop -c timeout 120 cargo test -p gopp-cli --test run the_files_of_the_corpus -- --ignored
```

The first builds every program with Go++ and compares it with its file. The second builds every program with the `go` of the development shell and compares that with the file; with `GOPP_CORPUS_UPDATE=1` it writes the files anew, which a new program needs once.

A program that Go++ refuses, or that does something else than in Go, is not put here: the first is a rule of the language, the second is a row of the table of differences or a bug.
