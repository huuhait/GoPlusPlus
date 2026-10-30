# Targets and linking

Status: P04.02, P04.03. Decisions: P04 D4.5 (layouts), D4.6 (unwind tables), D4.7 (line tables), D4.9 (the bundled LLD as `gopp lld`), P00 D0.12 (host sysroot discovery). Code: `compiler/include/gopp/CodeGen/{Target,CodeGen}.h`, `compiler/lib/CodeGen/{Target,Emit,LowerToLLVM,Linker}.cpp`.

## Targets

One table, `TargetSpec` (`Target.h`); nothing else in the compiler writes a triple.

| Go name | Triple | CPU | LLD `-m` | Loader (FHS) |
|---|---|---|---|---|
| `linux/amd64` | `x86_64-unknown-linux-gnu` | `x86-64` | `elf_x86_64` | `/lib64/ld-linux-x86-64.so.2` |
| `linux/arm64` | `aarch64-unknown-linux-gnu` | `generic` | `aarch64linux` | `/lib/ld-linux-aarch64.so.1` |

`gopp build -target=linux/arm64` selects a target; the default is `host`. The CPU is the architecture's baseline, never the host's, so a build gives the same code on every machine. Each architecture is built natively on its own CI runner (P04.03); cross builds without a C library of the target come with musl (P07.14). Only the two back ends are registered (`initializeTargets`), not "all targets".

The data layout of each target is pinned in the table (D4.5 relies on it: 64-bit little-endian pointers, `i64` aligned to 8):

- amd64: `e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128`
- arm64: `e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32`

`createTargetMachine` fails when the LLVM that gopp is built against gives another layout: a newer LLVM that changes it is something to look at, not to follow silently (`Targets.DataLayoutsArePinnedAndChecked`). The target machine is PIC (the code links into an executable now and into a PIE later). Its code generation level and instruction selector come from the pipeline (`createTargetMachine(Target, PipelineSpec)`, P04.13, `docs/design/pipelines.md`): `-O0` with FastISel for `opt 0` on x86-64 and for `-pipeline=fastisel` on both targets, `-O0` with GlobalISel for `opt 0` on arm64 (ADR-0028 addendum), and the level of the matching LLVM pipeline for `opt 1` to `opt z`.

## Code

`lowerToLLVM` verifies the GIR it is given, lowers it (one `alloca` per local, one LLVM block per GIR block, the Go semantics of D4.12 in the operations) and runs the LLVM verifier on the result, so `-emit=llvm` output is checked too. Every function Go++ defines has unwind tables (`uwtable`, D4.6: a stack trace works through any frame); a `NoUnwind` function is `nounwind`, a `NoReturn` one `noreturn`. A call of a C function follows the target's C ABI as Clang classifies it (P04.11, `docs/design/c-abi.md`): on amd64 a bool and an integer narrower than 32 bits are widened (`zeroext`/`signext`, at the declaration and at the call), on arm64 they are not (AAPCS64 leaves it to the callee), and structs travel in registers or memory as Clang says. DWARF 5 line tables come from the GIR spans (D4.7; `-no-debug` leaves them out).

## Linking

`gopp build` writes the object to a temporary file and runs `gopp lld <args>` as a subprocess of itself (the LLD library is linked into gopp: one file to ship, and a linker crash or exit cannot take the compiler down). Library-mode LLD is a later option (D4.9).

The link line is `lldArguments`, a pure function of its input (`Linker.LinkLineIsAFunctionOfItsInput`):

```
-o <out> -m <emulation> --eh-frame-hdr --hash-style=gnu --build-id=none -z relro
--dynamic-linker=<loader> <crt1.o> <crti.o> <objects...> -L<libdir> -lc <crtn.o>
```

- `--build-id=none` and the absence of any time, path or environment in the line make the executable a function of its input: the same program gives the same bytes (`tests/run/deterministic.test` compares objects and executables built into different directories).
- `--eh-frame-hdr` gives the unwinder its frame index (D4.6: panics are C++ exceptions); `-z relro` and the GNU hash table are what the system C compiler's link line has (`tests/run/link-flags.test`).
- The executable is dynamic, not PIE, against the C library: crt1.o's `_start` calls the C entry point `main` that the module defines (it calls `main.main` and returns 0). crtbegin/crtend (C++ static constructors, frame registration) arrive with the EH runtime (P04.07).

### Where the C library comes from

- **The host target:** P00.12 discovery (`probeHostToolchain`: ask `cc`, else the Nix environment) gives crt1.o, crti.o, crtn.o and the dynamic linker; the `-L` directory is the one that holds crt1.o (it also holds `libc.so` on NixOS and on Debian). On NixOS the loader is the one in the Nix store.
- **Another target:** `-crt-dir=<dir>` names a directory with crt1.o, crti.o, crtn.o and libc (`libc.so` or `libc.a`); the loader is the target's FHS path from the table, or `-dynamic-linker=<path>`. Without `-crt-dir` the build stops with an error that names the option; `-emit=obj` always works.

`findLinkInputs` checks every piece before the linker runs (`checkLinkInputs`): a missing crt object, a library directory without `libc.so` or `libc.a`, or an unknown loader is an error that names the piece and where it was looked for (`Linker.MissingPiecesGiveAClearError`, `tests/codegen/crt-dir-missing.test`), never a crash and never a bare linker message. A link that fails anyway (a C function no library defines) reports the linker's own message and `err_codegen` (E0441), and leaves no executable (`tests/codegen/link-error.test`).

## What runs where

| Check | linux/amd64 | linux/arm64 |
|---|---|---|
| LLVM IR, objects, assembly (`llvm-readelf`, `llvm-objdump`) | `tests/codegen` | `tests/codegen` |
| link against a stand-in C library | `tests/run` (the host's) | `tests/codegen/arm64-link.test` (stub crt and libc assembled for aarch64) |
| the executable runs | `tests/run` | on an x86-64 machine: `tests/run/arm64-qemu-*.test` under QEMU (below); on the arm64 CI runner: the same `tests/run` tests run natively (they require the host's architecture) |
| Debian / FHS (no Nix) | `tests/run/debian.test` on a Debian host; from NixOS `tools/run-hello-in-debian.sh` (`tests/run/debian-container.long.test`, opt-in with `GOPP_TEST_DEBIAN_IMAGE`) | CI |

## Running arm64 programs locally (owner decision M6)

Two things are added to the dev shell (`flake.nix`, x86-64 hosts only):

- `pkgs.qemu-user` (10.2): `qemu-aarch64`, the user-mode emulator. It was chosen over the full `qemu` package because it is the part that runs one Linux binary and is a much smaller download.
- the aarch64 glibc of `pkgs.pkgsCross.aarch64-multiplatform.glibc`, exported as `GOPP_AARCH64_SYSROOT`. It has crt1.o, crti.o, crtn.o, `libc.so`, `libc.so.6` and `ld-linux-aarch64.so.1` in its `lib` directory, so it is both the `-crt-dir` (`$GOPP_AARCH64_SYSROOT/lib`) and the `-L` sysroot of QEMU (which finds `/lib/ld-linux-aarch64.so.1` under it). Both are binary-cache downloads, not source builds: about 46 MiB compressed (qemu-user 36.5, glibc 9.5), 164 MiB unpacked.

**Why.** A developer on an x86-64 machine can then run what the arm64 target produces, and see a wrong constant, a wrong calling convention or a wrong `/` at once, instead of finding it on the CI runner after the push. **The alternative** is to run arm64 programs on the arm64 CI runner only: no new dependency, but every arm64 failure comes one push later, and the inspection-only test (`codegen/arm64-link.test`) cannot see a wrong result.

The lit feature `qemu-aarch64` is set when `qemu-aarch64` is on `PATH` and `GOPP_AARCH64_SYSROOT` has a `lib/crt1.o`; the substitution `%aarch64_sysroot` is its value. `tests/run/arm64-qemu-hello.test` and `arm64-qemu-features.test` build hello and the feature programs of the other run tests (exit code, D4.12 semantics, `println` of every argument kind, non-ASCII strings, evaluation order) for `linux/arm64` against that sysroot, run them as `qemu-aarch64 -L <sysroot>` under `timeout 60` (`tests/run/Inputs/run-compare-arm64.py`), and require the same stdout, stderr and exit code as the amd64 run. They take 0.1 s and 0.4 s. The stub-sysroot link test stays: it needs neither QEMU nor the download.

## The static Release build

A Release build links LLVM, Clang and LLD statically (`cmake/modules/GoppLLVM.cmake`). It broke from P04.01 to P04.11 without any test noticing (`library not found: LLVMx86`: the CodeGen library named its target components `x86 aarch64`, and LLVM's component names are case-sensitive, `X86 AArch64`; the shared Dev build links `libLLVM.so` and never looks at them). Two checks now:

- `gopp_llvm_libs` maps and checks the component names in every configure, shared or static, so a wrong name fails the Dev configure at once;
- the link itself, in CI's `release` job and by hand: `nix develop -c cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && nix develop -c ninja -C build-release gopp check` (a build directory of its own; `ldd build-release/bin/gopp` shows no `libLLVM` or `libclang-cpp`).
