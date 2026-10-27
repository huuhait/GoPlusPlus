# P10 Modules: gopp.mod / gopp.sum: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
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

## Open questions

- [x] Q1 Default cache dirs (decided for Linux, D10.5)
- [ ] Q2 Consuming Go modules (proposed: no)

## Exit criteria

- [ ] Multi-module sample resolves and builds
- [ ] `gopp mod tidy` is idempotent
- [ ] cdep rules across modules (`// indirect` blocks written by `tidy`; identity conflicts name the modules)
- [ ] Tampering detected where P06 D6.10 checks it (changed archive at download, edited cache by `verify`, moved tag by `get`/`tidy`)
- [ ] Offline builds from cache and `vendor/`
- [ ] Canonical `gopp mod edit -fmt` output
- [ ] cdep tree with symlinks, exec bits and a submodule: same `/cdep h1:` from git and from a proxy

## Decision changes

- None.

## Notes / blockers

- Port Go's MVS test cases and dirhash vectors early (P10.02/P10.04). They are cheap and lock down compatibility with Go's well-tested behavior.
