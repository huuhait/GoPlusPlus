# ADR-0039: libFuzzer through `cargo-fuzz` is the coverage-guided fuzz engine

- **Status:** accepted (owner decision)
- **Date:** 2026-11-11
- **Plan reference:** D17.8, P17.21; related [fuzzing.md](../design/fuzzing.md), [ADR-0002](0002-clang-version-policy.md)

## Context

The byte-level fuzz targets (lexer, parser, symbol demangler, file-format readers) need a coverage-guided engine: one that watches which code each input reaches and keeps the inputs that reach new code. The engine has to instrument the compiler's Rust code, and the toolchain is pinned to stable Rust (ADR-0002).

Four engines were tried on 2026-11-11 on one target (lex, parse and dump a file), with the ten samples of the spec as the starting corpus, on one core, with rustc 1.95.0 in the Nix shell, one run each. A bug was planted that needs parts of two samples in one input.

| Engine | On stable 1.95.0 | Runs per second | Planted bug |
|---|---|---|---|
| libFuzzer, `cargo-fuzz` 0.13.1 | builds with `-s none` | 4,312 | found in 2 s |
| AFL++, `cargo-afl` 0.18.2 | builds; its runtime build needs `llvm-config` of LLVM 22, not 23 | 3,595 | not found in 120 s |
| honggfuzz, `honggfuzz` 0.5.62 | did not build in four tries | – | – |
| LibAFL, `libafl_libfuzzer` 0.16.1 | compiles, then fails to link | – | – |

By default `cargo-fuzz` passes `-Zsanitizer=address`, which only a nightly compiler accepts. That is its one nightly-only flag; `-s none` leaves it out.

## Decision

1. **The coverage-guided engine is libFuzzer, driven by `cargo-fuzz`, with the sanitizer off** (`cargo fuzz run -s none <target>`).
2. `cargo-fuzz` comes from the pinned nixpkgs and is part of the dev shell. The fuzz targets use the `libfuzzer-sys` crate, version 0.4; both are for development only and nothing the compiler ships depends on them.
3. **A target stays a plain function over bytes** (fuzzing.md): the libFuzzer entry point only calls it. A crash is a file of bytes, and a normal test replays it by passing the bytes to the same function, so findings do not depend on the engine.
4. The engine runs on Linux. The byte-level targets exercise code that is the same on every platform.

## Consequences

- No second toolchain and no workaround: the engine builds with the compiler and the Clang the project already pins.
- No address sanitizer under the engine. The crates these targets reach forbid `unsafe`, so little is lost; code that does use `unsafe` is covered by the sanitizer runs of the generated-program fuzzing instead.
- The dev shell gains one package.
- The evaluation was small: one run per engine and one planted bug. The two engines that worked were close in speed; the choice rests on what worked without help, not on the speed numbers.

## Alternatives

- **AFL++:** works on stable, with good tooling for sorting crashes, but its runtime build refuses the project's LLVM 23 and needs a second LLVM, it is not in nixpkgs as a cargo tool, and it does not run on Windows.
- **honggfuzz, LibAFL:** did not build here in the time given. Each may work with packaging of its own or, for LibAFL, a hand-written fuzzer; that is more work than the problem needs.
