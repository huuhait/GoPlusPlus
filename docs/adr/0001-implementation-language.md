# ADR-0001: The compiler is written in C++20

- **Status:** accepted
- **Date:** 2026-10-27
- **Plan reference:** D0.1

## Context

Go++ builds on LLVM and must import C and C++ code (P07, P08). Importing C++
needs more than a parser: overload resolution and template instantiation
(Clang Sema), inline functions and ABI lowering (Clang CodeGen),
`ASTRecordLayout` for struct layout, and `MangleContext` for names. libclang's
C API cannot do any of that. LLVM's own APIs are C++-first.

## Decision

The compiler (`gopp`) is written in C++20. We follow LLVM coding standards
(D0.11).

Later, once P08 can import LLVM and Clang headers directly, the compiler may be
rewritten in Go++ itself (P16.08). That would be the strongest test of the
C++ interop requirement (R7).

## Consequences

- We use Clang's C++ API directly, with no shim layer.
- We live with C++ build times and memory-safety risk in the compiler; the
  Debug+ASan/UBSan CI job (D0.7) and the assertions-enabled LLVM job (D0.4)
  mitigate that.
- Contributors need a C++20 toolchain; the Nix flake provides it.

## Alternatives

- **Rust.** The `inkwell` crate covers LLVM, but Clang Sema and CodeGen still
  need a C++ shim, so we would carry two languages.
- **Go.** TinyGo shows that LLVM through cgo works, but Clang's C++ API is out
  of reach.
- **Zig.** Same shim problem as Rust, with a less mature ecosystem.
