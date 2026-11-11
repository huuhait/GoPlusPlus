# ADR-0035: The lowest layer of the runtime is written in C23

- **Status:** accepted (owner decision)
- **Date:** 2026-11-11
- **Plan reference:** D5.1, P05.01; related [ADR-0001](0001-implementation-language.md), [ADR-0002](0002-clang-version-policy.md), [ADR-0027](0027-mimalloc-global-allocator.md)

## Context

Every Go++ program links a runtime: program entry and exit, allocation, reference counts and buffers, panics and unwinding, bounds and nil failures, type tables, and later the scheduler and channels. The compiler is Rust and emits GNU C23 that the pinned Clang compiles for every target (ADR-0001). The docs did not say what the runtime itself is written in.

Three languages were possible for its lowest layer: C23, Rust without the standard library, or Go++.

## Decision

1. **The lowest layer of the runtime is GNU C23, in `runtime/`.** It holds only what cannot reasonably be Go++: entry and exit, the glue to the allocator, reference-count and buffer primitives, the panic machinery, the failure paths of checks, type-table lookup, and later the context switch and the core of the scheduler.
2. **It is kept as thin as possible. Everything above it is Go++, in `std/`.**
3. **It is built by the pinned Clang with the mandatory flags of generated code**, once per target and profile, and cached. `gopp` carries the runtime sources; no prebuilt archive is shipped.
4. **One header is shared** by the runtime and by generated code for every layout both sides touch (string and slice headers, buffers, type tables), and it carries the hot paths as `static inline` functions. The header's layouts come from the same table in the compiler that code generation uses.
5. **Runtime symbols start with `_gopp_`.**
6. The C is covered by sanitizer runs and by the runtime fuzzing of [fuzzing.md](../design/fuzzing.md).

## Consequences

- No toolchain beyond the Clang that every build already needs; cross-compiling the runtime is the same step as cross-compiling a program.
- Layouts cannot drift silently: the C compiler sees both users of the header.
- Hot paths (a reference-count update, a failed bounds check, the fast path of `append`) inline into user code with no link-time optimization across languages.
- Panics are raised with the unwinder that generated code already relies on (ADR-0001), and mimalloc (ADR-0027) is part of the same build.
- **The cost:** this layer has no memory safety from its language. Only its small size, the sanitizers and the fuzzer guard it.
- Moving pieces from C to Go++ may be looked at again after 1.0, when the compiler is mature.

## Alternatives

- **Rust (`no_std`):** safer on paper, but this layer is raw memory and unwinding, so most of it would be `unsafe`. It would need a Rust target per platform on the user's machine or a matrix of prebuilt archives (targets × profiles × sanitizers), mirrored layouts, cross-language LTO between two LLVM versions for inlining, and care at every boundary a panic unwinds through.
- **Go++ from the start:** a bootstrap problem. The layer implements what the ownership rules stand on, so it needs raw pointer arithmetic, code that must not allocate, stack switching and direct unwinder calls. The language would first need a set of runtime-only escape hatches.
