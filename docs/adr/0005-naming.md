# ADR-0005: Naming: the language is Go++, everything else is `gopp`

- **Status:** accepted (owner decision, 2026-10-27)
- **Date:** 2026-10-27
- **Plan reference:** D0.8, Q3 (trademark check: P16 D16.6, P16.02)

## Context

The language needs a name, and the tools, file extensions and environment
variables need ASCII identifiers that are safe to type and to put in file
names. `Go++` is not a valid identifier or file name on every system.

## Decision

- The language is **Go++** (codename GoPlusPlus).
- Everything else is `gopp`: the CLI `gopp`, source files `.gopp`, manifests
  `gopp.mod` / `gopp.sum`, export data `.gopx`, directives `//gopp:...`, and
  environment variables `GOPPCACHE`, `GOPPMODCACHE`, `GOPPPROXY`, `GOPPOS`,
  `GOPPARCH`, and so on.
- **Why not `gpp`:** Linux distributions already ship an unrelated `gpp`
  binary, the "generic preprocessor". Installing both would conflict.

## Consequences

- No clash with existing packages named `gpp`.
- **Known risk: the "Go" trademark.** "Go" is a trademark of Google. The owner
  chose to keep the name Go++. A trademark check runs before the first public
  release (P16 D16.6); if it fails, a new ADR supersedes this one.

## Alternatives

- **`gpp`:** dropped (the unrelated generic preprocessor).
- **Keeping `Go++` everywhere:** not usable in identifiers and file names.
