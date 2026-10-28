# ADR-0012: Built-in `host` profile for generator tools

- **Status:** accepted
- **Date:** 2026-10-27
- **Plan reference:** P11 Q4, D11.12, D11.13; P06 D6.12; P10 D10.11. Closes the open point in ADR-0007.

## Context

In a cross build, generator tools that run during the build (protoc, tblgen) get a host build of their cdep (D11.12). ADR-0007 left open which profile settings that host build uses. The target profile's settings often don't fit a host tool: `sanitize` slows the tool and breaks the build when it finds a bug in the tool, a `pgo` file and a `cpu` name describe the target, and `lto` only makes the tool slower to build.

The owner chose, on 2026-10-27, a fixed default that can still be changed.

## Decision

- **A third built-in profile, `host`,** is used for every host build of a generator tool, whatever profile the main build uses. Its settings:

  | `opt` | `debuginfo` | `lto` | `panic` | `sanitize` | `strip` | `cpu` | `pgo` | `link` |
  |---|---|---|---|---|---|---|---|---|
  | `2` | `none` | `off` | `unwind` | `none` | `false` | `baseline` | `off` | `static` |

  `cpu baseline` and `native` refer to the host machine here.
- **It can be changed like `debug` and `release`:** a `profile host ( … )` block in the main module's gopp.mod changes only the lines it writes (P10 D10.11), for example `opt 0` and `debuginfo full` to debug protoc itself.
- **It is only for host tools.** `-profile=host` is an error, and no profile may `inherits host`. `deps` rows and cdep `profile` pins don't apply to host builds.
- Because the host build doesn't depend on the main profile, `debug` and `release` builds share one cached host build of each tool. The cache key (D11.5) uses the `host` profile's settings.

## Consequences

- Host tools are fast, built once, and never pick up target-only settings by accident.
- Someone who needs a different tool build writes a `profile host` block, with no new syntax.
- P06 D6.12 lists three built-in profiles, and P10 D10.11 accepts `host` as a built-in block name.
- P11.12 tests that debug and release cross builds reuse one host build.

## Alternatives

- **Copy `opt` and `debuginfo` from the main profile** (the old proposal). Rejected: the tool is rebuilt for every profile, and it can't be changed on its own.
- **Fixed settings with no override.** Rejected: there is no way to debug or tune the tool when it is needed.
- **Copy every setting.** Rejected: sanitizers, PGO data and target CPU names don't fit a host tool.
