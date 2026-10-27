# P14 Tooling: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P14) · Design: [README.md](README.md)

## Tasks

- [ ] P14.01 CLI consolidation
- [ ] P14.02 `gopp test`
- [ ] P14.03 Bench & fuzz
- [ ] P14.04 Coverage
- [ ] P14.05 LSP core
- [ ] P14.06 LSP advanced
- [ ] P14.07 Debugger support
- [ ] P14.08 Editors
- [ ] P14.09 `gopp doc`
- [ ] P14.10 Distribution & `toolchain`

## Open questions

- [ ] Q1 `gopp tool`: how third-party tools are versioned and run

## Exit criteria

- [ ] VS Code works as a daily driver on samples and std (diagnostics, completion, go-to-definition, formatting, debugging)
- [ ] LLDB and GDB display owned strings, slices and maps correctly
- [ ] Coverage and fuzzing work; fuzzing finds a seeded bug
- [ ] LSP completion p95 < 100 ms on a 100k-line workspace

## Decision changes

- 2026-10-27 (ADR-0006, P01.15 owner decisions): `view` is renamed `ref` in hover text and pretty printers.

## Notes / blockers

- A minimal `gopp lsp` (diagnostics from `gopp check` + formatting) is worth shipping right after P03. Dogfooding the editor experience early catches diagnostic-quality issues.
- The early LSP uses the P04 source mode (P04 D4.11).
