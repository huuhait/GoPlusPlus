# P16 — Release (v0.1 alpha → v1.0)

**Goal:** ship. Define what alpha, beta and 1.0 mean, the compatibility promises, the documentation, distribution and the name. Start the self-hosting track.
**Depends on:** all phases · **Size:** M · **Milestone:** M9

## Decisions

**D16.1 — Release train.**
- v0.1 alpha ships at M9.
- After that, minor releases are time-based. The cadence is decided at v0.1.
- v1.0 ships when spec v1 is frozen. The compatibility promise starts then.

**D16.2 — Compatibility.**
- Before 1.0, breaking changes are allowed, with `gopp fix` migrations where possible.
- From 1.0, a Go 1–style promise applies: source compatibility for the language and std.
- The language version is gated by the `gopp` directive in gopp.mod.

**D16.3 — Platform tiers at v0.1.** v0.1 is Linux only. There is no macOS in v0.1 (owner decision).

| Tier | Platforms | Meaning |
|---|---|---|
| 1 | linux/amd64, linux/arm64 | full CI, releases, complete test suite |
| 2 | none at v0.1 | CI and releases |
| 3 | darwin/arm64 (C++ via libc++), windows/amd64 (MSVC ABI, SEH, clang-cl, CMake VS toolchains), wasm32-wasi | after v0.1 |

**D16.4 — Release engineering.**
- Toolchain builds are reproducible.
- Archives are signed and checksummed.
- Each release ships an SBOM listing the bundled LLVM, Clang, LLD, libc++, musl and compiler-rt versions and their licenses.
- The project flake also exports `gopp` as a package (`nix run`), because the release archives need nix-ld on NixOS.

**D16.5 — Documentation set.**
- Tour and tutorial.
- "Go++ for Go programmers" (mostly about ownership).
- "Go++ for C++ programmers".
- The spec and the std docs.
- An interop cookbook: C, C++ and CMake recipes.

**D16.6 — Trademark check before the first public release.** The owner chose Go++ and `gopp` (P00 D0.8). Before any public artifact exists, an ADR confirms that "Go++" can be used, or picks a new name.

**D16.7 — Self-hosting track (research, after v0.1).**
- Rewrite compiler components in Go++, importing LLVM/Clang C++ headers through P08.
- Start with `gopp fmt` and the parser.
- Success criterion: a reproducible bootstrap.
- This is the strongest possible test of R7.

## Deliverables

- `RELEASE.md` with the v0.1 checklist.
- Signed, reproducible archives for the tier-1 platforms, plus the SBOM.
- The docs set.
- Community files: CONTRIBUTING, Code of Conduct, issue templates, SECURITY.
- The v0.1 tag and announcement.
- `docs/design/self-hosting.md`.

## Exit criteria

- v0.1 is tagged and binaries are published for tier 1.
- On a clean Linux machine, installing and completing the tutorial takes under 10 minutes.
- Two independent toolchain builds are bit-identical.
- The issue tracker, contribution guide and security policy are live.

## Risks

- **A late forced rename (name or trademark)** → decide in P16.02, before any public artifact exists.
- **A premature release hurts first impressions** → the v0.1 checklist requires M0–M8 and the docs set.

## Open questions

None.
