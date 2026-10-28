# ADR-0015: Time zone database source

- **Status:** accepted
- **Date:** 2026-10-27
- **Plan reference:** P13 Q3 (task P13.13); new P13 D13.8; task P13.07.

## Context

`time.LoadLocation("Asia/Ho_Chi_Minh")` needs the IANA time zone database (about 450 KB, and its rules change several times a year). Most Linux systems ship it in `/usr/share/zoneinfo`, but minimal container images (`scratch`, distroless) don't, and those are a common home for Go++'s static binaries. Go reads the OS copy and embeds a fallback only on request (`import _ "time/tzdata"` or `-tags timetzdata`); forgetting that is a well-known source of failures in `scratch` images.

The options were: the OS only, always embedded, Go's opt-in fallback, or an automatic fallback. The owner chose the automatic fallback on 2026-10-27.

## Decision

- **The OS copy comes first:** `$ZONEINFO` if set, then `/usr/share/zoneinfo` (and the other standard Linux locations Go checks).
- **An embedded copy is the fallback, added automatically:** a program that can reach `time.LoadLocation` (or anything that calls it) links an embedded copy of the database, from the tzdata release bundled with that gopp version. A program that never loads a location by name gets no extra bytes; the linker drops the data.
- **Opt out with `-tags notzdata`:** then there is no embedded copy, and `LoadLocation` fails when the OS has no data, as in Go without `time/tzdata`.
- `import _ "time/tzdata"` is accepted for Go compatibility and does nothing extra.
- `time.UTC` and `time.Local` from the `TZ` variable work as in Go.

## Consequences

- Static binaries work in `scratch` images with no extra step.
- A program that uses named zones is about 450 KB larger unless built with `-tags notzdata`.
- When the OS has newer rules, they win; the embedded copy can be stale until gopp is updated, and the release notes name the bundled tzdata version.
- P13.07 tests the fallback (no OS data → the embedded copy is used), the OS-first order, and `-tags notzdata`.

## Alternatives

- **OS only** (C/glibc). Rejected: fails in `scratch` and distroless images.
- **Always embedded** (Rust `chrono-tz`). Rejected: rules frozen at build time even when the OS has newer ones, and 450 KB in every program that uses time.
- **Opt-in fallback** (Go). Rejected: users forget the import, and the failure only shows up in production containers.
