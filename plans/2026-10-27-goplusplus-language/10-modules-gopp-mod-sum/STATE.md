# P10 Modules: gopp.mod / gopp.sum: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27 (Q2 decided)
Task definitions: [../TODO.md](../TODO.md) (section P10) · Design: [README.md](README.md)

## Tasks

- [ ] P10.01 gopp.mod parser/printer
- [ ] P10.02 gopp.sum
- [ ] P10.03 Versions
- [ ] P10.04 MVS
- [ ] P10.05 Fetchers
- [ ] P10.06 Module cache
- [ ] P10.07 Commands
- [ ] P10.08 Tamper & offline tests
- [ ] P10.09 Modules reference doc
- [ ] P10.10 cdep tree format
- [ ] P10.11 cdep versions
- [ ] P10.12 Build profile blocks

## Open questions

- [x] Q1 Default cache dirs (decided for Linux, D10.5)
- [x] Q2 Consuming Go modules (decided: not in v1; after v1.0, needs a dual engine)

## Exit criteria

- [ ] Multi-module sample resolves and builds
- [ ] `gopp mod tidy` is idempotent
- [ ] cdep rules across modules (`// indirect` blocks written by `tidy`; identity conflicts name the modules)
- [ ] Tampering detected where P06 D6.10 checks it (changed archive at download, edited cache by `verify`, moved tag by `get`/`tidy`)
- [ ] Offline builds from cache and `vendor/`
- [ ] Canonical `gopp mod edit -fmt` output
- [ ] cdep tree with symlinks, exec bits and a submodule: same `/cdep h1:` from git and from a proxy
- [ ] `profile` blocks round-trip; ignored in dependency modules

## Decision changes

- 2026-10-27 (ADR-0006, P01.15 owner decisions): gopp.mod gets the module-wide directives `cxxstd`, `cflags` and `ldflags` (replacing the removed source directives).
- 2026-10-27 (ADR-0007, build profiles): gopp.mod gets `profile` blocks (with `inherits` and `deps ( … )`) and a `profile ( … )` pin inside `cdep` blocks (new D10.11). Only the main module's profiles count.

## Notes / blockers

- Port Go's MVS test cases and dirhash vectors early (P10.02/P10.04). They are cheap and lock down compatibility with Go's well-tested behavior.
