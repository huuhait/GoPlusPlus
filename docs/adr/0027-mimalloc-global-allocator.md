# ADR-0027: mimalloc is the global allocator

- **Status:** accepted
- **Date:** 2026-10-29
- **Plan reference:** decides P15 Q2 / P05 Q2 (task P15.10); P05 D5.2, D5.3 and P05.03; P01 D1.22 (nil allocator word); spec SLC-2, SLC-5; P07 D7.7.

## Context

Go++ programs make many small, short-lived allocations from many threads: `new_unique`, `append`, string building. The global allocator decides how fast this is. P05 first said "libc `malloc` in v0, evaluate mimalloc in P15.10".

On 2026-10-29 the owner decided: **mimalloc is the global allocator**.

## Decision

1. The global allocator of a Go++ program is **mimalloc**. For now it is the **only** one: no libc-malloc option and no flag to switch.
2. Everything that uses the global allocator uses mimalloc: `new_unique`, `new_shared`, `make`, string and slice growth, maps, and the nil allocator word in slices and maps (P01 D1.22).
3. These stay as they are:
   - `mem.Allocator` objects and `mem.Arena` (user-chosen allocators).
   - ASan builds keep the sanitizer's own allocator (P05: "ASan covers that need").
   - C and C++ code linked into a program keeps its own `malloc` and `new`.
4. Memory is freed by the allocator that made it. Go++ memory goes back through mimalloc, and C memory through C `free`.
   - **4a. Mixed allocators.**
     1. Memory that C/C++ returns is a raw C pointer that Go++ does not own. Free it with the C API that made it (`free`, `sqlite3_free`, ...). No Go++ owned type adopts it, and a Go++ drop never frees it. A future adopting API must copy into mimalloc.
     2. A buffer that C will `free()` or `realloc()` must come from C `malloc`: `ffi.CString(...).Release()` (P07 D7.7 allocates it with `malloc` on purpose; this is the deliberate exception) or a malloc-style helper from P07.07. A mimalloc pointer passed to C `free`, or the reverse, is undefined behavior (UB-5a).
     3. A C++ `new` object is freed with C++ `delete` through the P08 bindings.
5. mimalloc is linked **without** overriding `malloc`, `free` or `operator new`. That is what lets C keep its own `malloc` and ASan keep its allocator.
6. **Dependency:** the mimalloc source is pinned (a flake input or a vendored release), and the runtime build compiles it for each target and links it statically. nixpkgs' host library is not enough: cross targets (P07.14 arm64, musl) cannot link it. License: MIT.

```text
who frees what
  Go++ (new_unique, make, append, string)  ->  mimalloc free   (drop)
  C malloc / sqlite3_* / C++ new           ->  C free / sqlite3_free / C++ delete
  ffi.CString(s).Release()  (malloc, given to C) -> C free
  mimalloc pointer -> C free        : undefined behavior (UB-5a)
```

## Consequences

- Fast for many small objects across threads, with thread-local pools, so threads do not contend.
- The same behaviour on every OS.
- MIT-licensed. Its size is a guess: small, not measured yet.
- Passing memory between Go++ and C needs care: the side that allocated must free (rule 4). The FFI docs and tests cover this.
- No fallback to libc `malloc` for now. Benchmarks (P15) record mimalloc numbers; they do not re-decide this.

## Alternatives

- **glibc `malloc`.** No new dependency, but slower with many threads, and it differs per OS. Rejected.
- **jemalloc (Meta).** Good for long-running servers; bigger. Rejected.
- **tcmalloc (Google).** For large servers; bigger, with C++ build needs. Rejected.
- **A libc fallback flag.** Not now. It can be added later without changing the language.
