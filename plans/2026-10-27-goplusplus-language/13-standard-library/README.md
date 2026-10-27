# P13 — Standard library v1

**Goal:** a standard library shaped like Go's, adapted to ownership, written in pure Go++ on top of libc. It should be enough to write CLI tools, network servers and data-processing programs.
**Depends on:** P05, P07, P09, P11, P12 · **Size:** L · **Milestone:** M9

## Decisions

**D13.1 — Follow Go's std shape.** Keep Go's package names and APIs wherever ownership doesn't force a change, so Go knowledge transfers. Document every deviation per package.

**D13.2 — Ownership conventions in APIs** (written up in `docs/std-conventions.md`).
- **Parameters borrow.**
- **Results that allocate are owned.**
- **Results that alias an input are views, and the signature says so.** Example: `strings.Fields(s string) []view string` returns an owned slice of views into `s`.
- **Zero-allocation paths append to a caller buffer** (`strconv.AppendInt`, `fmt.Appendf`), as Go already does. `Append*` functions take `dst own []T` (P01 D1.20), so `buf = strconv.AppendInt(buf, n, 10)` is written as in Go.
- **Functions that can fail are declared with `!`** (e.g. `func Open!(name string) Unique[File]`, which returns `(Unique[File], error)`), so callers choose between `!` and Go-style handling. Std never panics on ordinary errors.
- **Errors work like Go** (P01 D1.19). Sentinel errors are package-level `errors.New` values, compared with `==` (`err == io.EOF`).

**D13.3 — Std is pure Go++ plus libc.**
- Std contains no third-party C/C++, which keeps it portable and hermetic.
- Packages backed by C/C++ (regex via RE2, TLS via BoringSSL, zstd, …) live in an extended set, `gopp.dev/x/...`. That set is also the showcase for P07/P08/P11.

**D13.4 — `fmt` and `encoding/json` work on any type, as in Go, without a `reflect` package.**
- Both read the compact field tables the compiler emits for types converted to an interface (P01 D1.12).
- `fmt.Println(p)` prints `{1 2}` and `%+v` prints `{X:1 Y:2}`. `Stringer`, `Formatter` and `error` implementations take precedence, as in Go.
- `@Derive(JSON)` is an optional fast path. It generates `MarshalJSON`/`UnmarshalJSON`, which `encoding/json` prefers (like `easyjson` in Go). Output is identical either way.

**D13.5 — Tiers, built in this order.**
- **T1** (everything depends on these):
  - core: errors, unsafe, runtime, mem, math, math/bits, unicode/utf8;
  - text: strings, bytes, strconv, fmt;
  - collections: slices, maps, sort, container/deque (fast FIFO queues; owner decision, see P01 D1.20);
  - I/O and OS: io, bufio, os, path, path/filepath, flag;
  - time, sync, sync/atomic, testing.
- **T2:**
  - containers: container/list, container/heap;
  - hashes and crypto: hash/fnv, hash/crc32, hash/maphash, crypto/sha256, crypto/sha512, crypto/md5, crypto/rand;
  - encoding: binary, hex, base64, json, csv;
  - context, a small log/slog;
  - net (TCP, UDP, DNS), os/exec, os/signal;
  - regexp (a pure Go++ port of Go's engine), compress/flate, compress/gzip.
- **T3** (after v1, or in `x/`): net/http, crypto/tls, database/sql, image/*.

**D13.6 — Port, don't rewrite.**
- Port Go's implementations and tests (BSD-3; keep the notices) and adapt them to ownership.
- Redesign the APIs that assume a GC:
  - `sync.Pool` (Q2);
  - finalizers and `runtime.SetFinalizer` (removed; use `drop`).

**D13.7 — Allocator awareness only where it pays off.** Std uses the default allocator. Variants that accept an allocator or arena are added only where profiling shows a need, e.g. `strings.Builder`, `bytes.Buffer`.

## Deliverables

- T1 and T2 packages, with ported tests passing under ASan.
- `docs/std-conventions.md`, API docs and runnable examples.
- At least one `x/` package wrapping a C library through a cdep, e.g. `x/zstd`.

## Exit criteria

- T1 is complete, and its ported tests pass, including under ASan/LSan.
- T2 is complete, with tests.
- Every package has a runnable example, checked by `gopp test`. (Rendering the std docs is checked in P14.09.)
- `x/zstd` (or an equivalent) builds from a locked cdep and passes its tests.

## Risks

- **Go APIs that assume a GC** → the explicit redesign list in D13.6, and views where Go returned aliasing slices.
- **Scope creep** → strict tiers; T3 stays out of v1.

## Open questions

- **Q1** (decided 2026-10-27) `%v` works on every struct, as in Go, through compiler-emitted field tables (D13.4).
- **Q2** What replaces `sync.Pool`? Open; task P13.13 decides it.
- **Q3** Should the time zone database be embedded, or taken from the OS? Open; task P13.13 decides it.
