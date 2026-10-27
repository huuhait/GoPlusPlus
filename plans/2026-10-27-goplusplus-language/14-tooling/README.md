# P14 — Tooling

**Goal:** a Go-grade developer experience: one `gopp` command, an LSP server, debugger support, test/bench/fuzz/coverage, docs, editor integrations and distribution.
**Depends on:** P03, P06, P13 (individual features also depend on other phases) · **Size:** L · **Milestone:** M9

## Decisions

**D14.1 — One binary with Go-style subcommands.**
- `gopp` bundles the driver, the compiler, LLVM/Clang/LLD and the LSP server.
- Subcommands:
  - new projects: `new` (creates the directory with gopp.mod, a `main.gopp` and a `.gitignore` that lists `target/`, P06 D6.12);
  - build and run: `build`, `run`, `check`, `clean`, `list`. They take the build-profile flags (`-release`, `-profile=NAME`, one-off overrides; P06 D6.12);
  - quality: `test`, `vet`, `fmt`, `fix`;
  - inspection: `parse`, `expand`;
  - docs and modules: `doc`, `mod`, `get`, `cdep`;
  - environment: `env`, `version`, `tool`;
  - C/C++ drivers: `cc`, `c++`, `ar`;
  - editor integration: `lsp`.

**D14.2 — Testing follows Go's model.**
- `_test.gopp` files contain:
  - `TestXxx(t *testing.T)`;
  - `BenchmarkXxx(b *testing.B)`;
  - `FuzzXxx(f *testing.F)`;
  - examples with `// Output:`.
- Flags: parallel tests, `-run`, `-count`, `-race`, `-asan`.
- Fuzzing runs on LLVM libFuzzer, using Go's corpus layout (`testdata/fuzz`).
- Coverage uses LLVM source-based coverage: `-cover`, `-coverprofile`, and an HTML report.

**D14.3 — `gopp lsp` is built on the compiler libraries.**
- It re-parses incrementally per file and re-checks per package. Dependencies come from export data.
- It uses the overlays (unsaved editor buffers) and check sessions of P06 D6.11.
- Clang runs in a worker process (clangd-style) or under `CrashRecoveryContext`, so a Clang crash doesn't kill the LSP.
- Each snapshot has its own source-offset space (P02 D2.4).
- The early minimal LSP uses the P04 source mode (P04 D4.11), which loads all packages from source.
- Features:
  - diagnostics, completion and formatting;
  - hover with ownership info ("owned", "ref", "moved at line 12");
  - go-to-definition, including into C/C++ headers;
  - references and rename;
  - code actions from fix-its;
  - a macro expansion view;
  - inlay hints for moves and borrows.

**D14.4 — Debugging.**
- DWARF comes from P04.
- LLDB and GDB pretty printers (Python) cover `string`, refs, slices, maps, interfaces, `Unique`/`Shared`/`Weak` pointers and channels.
- A `gopp goroutines` LLDB command lists goroutines through a runtime debug API.

**D14.5 — Editors.**
- A VS Code extension: TextMate grammar, LSP client, debug configuration.
- A tree-sitter grammar, forked from tree-sitter-go.
- Configs for Neovim, Helix and Zed.

**D14.6 — Docs.** `gopp doc` in the terminal, plus a static HTML generator for module docs (pkgsite-lite).

**D14.7 — Distribution.**
- Release archives per platform contain:
  - `gopp`;
  - runtime and std sources;
  - libc/libc++ sources or prebuilt per-target archives;
  - compiler-rt.
- Tier-1 toolchain parts are prebuilt in the release archive: the runtime, libc++, compiler-rt and musl for the tier-1 targets (P16 D16.3). The P06 D6.8 budgets assume them.
- A `toolchain` directive in gopp.mod makes `gopp` download and run the requested version (Go 1.21 style), hash-verified.

## Deliverables

- The CLI with help text, including `gopp new`.
- `gopp test` with bench, fuzz and cover support.
- `gopp lsp`.
- Debugger pretty printers.
- The VS Code extension and the tree-sitter grammar.
- `gopp doc`.
- Release archives and toolchain switching.

## Exit criteria

- VS Code works as a daily driver on the samples and std: diagnostics, completion, go-to-definition, formatting, debugging.
- LLDB and GDB display owned strings, slices and maps correctly.
- Coverage and fuzzing work on the samples, and fuzzing finds a seeded bug.
- LSP completion p95 is under 100 ms on a 100 k-line workspace (provisional).

## Risks

- **LSP speed with large C++ imports** → the shared import-unit cache (P07) and lazy translation.
- **The tree-sitter grammar drifting from the real one** → test it against the parser corpus in CI.

## Open questions

- **Q1** `gopp tool`: how are third-party tools versioned and run?
