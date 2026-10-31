# ADR-0030: amd64 Go++ calling convention: keep swiftcc

- **Status:** accepted
- **Date:** 2026-10-30
- **Plan reference:** P04 D4.4 (Go++ to Go++ calls return `(T, error)` in registers); task P04.18 (review R4); P04.07 (panics, closures); P04.12 (debug info); P15 (benchmarks); `docs/design/calling-convention.md`.

## Context

P04.18 makes every Go++ function `swiftcc`, because LLVM's C convention returns at most two values in registers. Measured with LLVM 23.1 (`GoABITest.ReturnRegistersAreWhatLLVMHas`):

- **arm64:** swiftcc returns eight integer and eight float values (x0-x7, v0-v7).
- **amd64:** it returns only four of each (rax, rdx, rcx, r8; xmm0-3). A fifth integer makes the whole result a hidden pointer.

A Go++ string is three words (ADR-0026). So on amd64 `(int, error)` (three words) comes back in registers, but `(string, error)` (five) and `([]T, error)` (six) come back through memory. D4.4 promises registers. Review finding R4 said so.

LLVM's `x86_regcallcc` (Intel's regcall) returns eleven integer values (rax, rcx, rdx, rdi, rsi, r8, r9, r12-r15) and sixteen float values (xmm0-15) on x86-64 Linux. A twelfth integer makes the result a hidden pointer. `sret`, `byval`, `zeroext`, `signext` and `i1` lower correctly. The owner tried it twice.

## History

### B: regcall on amd64

The owner first chose regcall for Go++ functions on amd64, swiftcc on arm64. Measuring it found a correctness problem.

regcall keeps rbx, rbp, r12-r15 **and xmm8-15** across a call. The unwinders restore no XMM register: libgcc's x86-64 unwinder has no XMM columns, and LLVM's libunwind restores them on Win64 only (libunwind is not in the flake, so this was not tested). So if an exception passes through a regcall callee into a landing pad of the caller, any value the caller kept in xmm8-15 across that call is lost.

The test held doubles in xmm8/xmm9 across an `invoke`. At `-O1` to `-O3` the landing pad read what the throwing code left there and printed 21; the right answer is 37. At `-O0`, and with the C convention, it printed 37.

### B2: regcall plus a workaround

The owner then chose regcall with a workaround. Every call that can unwind into a landing pad of or in a regcall function gets the call-site attribute `"no_callee_saved_registers"`, so LLVM keeps no value in a register across it. With the attribute the same test printed 37 at every level. A check after lowering made a missing attribute an internal error.

B2 was built and measured against A (swiftcc) on 2026-10-30. The machine was loaded (load average 20-100), so the numbers are a sanity check only.

| What | A (swiftcc) | B2 (regcall) |
|---|---|---|
| `(string, error)` result | through memory (five stores) | in registers (rax, rcx, rdx, rdi, rsi) |
| 400M calls returning `(int, error)` + `(string, error)`, opt 2, callee not inlined | 307 ms | 303 ms |
| 200M calls of a Go++ function that calls C, opt 2 | 308 ms | 357 ms (+16%: saves and restores xmm8-15) |
| build, 11 run programs, opt 0 | 1015 ms | 1052 ms (+3.6%) |
| build, same programs, opt 2 | 5539 ms | 5600 ms (+1.1%) |
| `.text`, same programs, opt 2 / opt 0 | 1060 KB / 735 KB | 1120 KB (+5.7%) / 815 KB (+10.9%) |
| run, same programs, opt 2 | 306 ms | 304 ms |
| FastISel, `Inputs/cc/calls1` (89 functions), call fallbacks to SelectionDAG | 188 | 1361 (FastISel selects no regcall call) |

Other findings for B2:

- regcall functions still tail-call each other at opt 2.
- gdb 17.2 shows backtraces through regcall frames.
- LLVM writes `DW_CC_LLVM_X86RegCall` only when a function's debug type carries it.

## Decision: A (final)

**Go++ calls Go++ with `swiftcc` on both targets** (`goabi::Policy::CC`, per target so a later target can differ). The owner chose it after the B2 measurements:

- **No measured gain.** The loop of `(T, error)` calls took 307 ms with swiftcc and 303 ms with regcall, and the run programs 306 and 304 ms: no runtime difference.
- **Measured costs.** With regcall, a Go++ function that calls C was 16% slower, `.text` was 6-11% larger, opt 0 builds were slower, and FastISel fell back to SelectionDAG on every call.
- **An unwinding hazard.** regcall needs a workaround on every unwinding call, which spills every live value there.

swiftcc keeps the registers C keeps (LLVM 23.1, `X86RegisterInfo::getCalleeSavedRegs` and `getCallPreservedMask`: `CallingConv::Swift` takes the default `CSR_64`). On amd64 these are rbx, rbp and r12-r15, and no XMM register. The unwinder restores these registers from the CFI, as it does for C. The B test built with swiftcc frames prints 37 at `-O0` to `-O3` with no attribute. So there is no unwinding workaround, and an `invoke` needs nothing beyond what C++ needs.

The known limit, kept: on amd64 a result of more than four integer-class or four float-class scalars goes through a hidden first pointer. That is `(string, error)`, `([]T, error)`, and `(T, error)` for any T of more than two words. `(int, error)` and every T of at most two words stay in registers. On arm64 every result up to eight and eight stays in registers.

```text
A   (chosen):   swiftcc on both. amd64 returns 4 + 4 in registers; (string, error) through memory.
B   (rejected): regcall on amd64. Wrong values after an exception (21 for 37).
B2  (rejected): regcall + "no_callee_saved_registers" on every unwinding call. Correct, no gain, measured costs.
C   (rejected for now): a custom LLVM calling convention.
```

## Option C: rejected for now

A custom LLVM convention (swiftcc's rules with more result registers) would return `(string, error)` in registers without regcall's callee-saved XMM registers. It means an LLVM patch to rebase at every LLVM release, and a toolchain Go++ could no longer take from distributions.

**Trigger to revisit it:** a P15 benchmark showing that returning `(string, error)` through memory on amd64 costs something that matters in real Go++ programs (TODO P15).

## Consequences

- `GoABITest.ReturnRegistersAreWhatLLVMHas` pins swiftcc's budget on both targets: 4 + 4 on amd64, 8 + 8 on arm64. A newer LLVM that changes it fails that test.
- `tests/codegen/go-abi.test` and `go-abi-error.test` pin the limit on both sides:
  - `(int, error)` in registers on both targets;
  - `(string, error)` through the hidden pointer on amd64 and in registers on arm64.
- `Lowering::emitCall` stays the only place that makes a call.
- `gopp::checkCalls` keeps two checks: a direct call uses its callee's convention, and every function in static data (itabs, method tables, descriptors) has the Go++ convention of the target.
- A closure's environment (P04.07) is swiftcc's `swiftself` register on both targets.
- The debug type of a Go++ function carries `DW_CC_LLVM_Swift`, a C function's none (P04.18 on P04.12's `DebugInfoBuilder`, `tests/debug/place.test`).
- Debuggers read a Go++ result as if it were C's: `finish` is right for one and two words only (review R13, below).

### Debuggers and results (review R13)

Neither gdb nor lldb uses `DW_CC_LLVM_Swift` for a return value: `finish` reads a result as the SysV ABI returns it, so it is right only where swiftcc and SysV agree. Measured on amd64 at opt 0, 2026-10-30 (gdb 17.2, lldb 22):

| Result | Words | Where swiftcc puts it on amd64 | gdb 17 `finish` | lldb 22 `finish` |
|---|---|---|---|---|
| `int` | 1 | rax | right | right |
| `(int, int)` | 2 | rax, rdx | right | right |
| `(int, error)`, `(int, int, int)` | 3 | rax, rdx, rcx | "Cannot access memory at address 0x6" (reads rax as the address of a SysV memory result) | zeros |
| `(string, int)` | 4 | rax, rdx, rcx, r8 | garbage | garbage |
| `(string, error)` | 5 | memory (the hidden pointer) | right at opt 0 | right at opt 0 |

A one- or two-word result, the common case, is right. Three and four words come back in registers where SysV would use memory, so both debuggers show a wrong value. `DW_CC_nocall` does not help: gdb then refuses every result, also the right one- and two-word ones ("Cannot determine contents"), and lldb ignores it and prints the same values. So nothing makes both refuse only the wrong ones. TODO P14.07 gives the debuggers the convention (a plugin). arm64 (swiftcc returns up to eight words in registers where AAPCS64 uses memory above 16 bytes) is not measured.
