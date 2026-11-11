# ADR-0030: Internal Go++ calling convention via Clang-23 `swiftcall`

- **Status:** accepted
- **Date:** 2026-11-11
- **Plan reference:** P04 D4.4 (Go++ to Go++ calls return `(T, error)` in registers); P04.07 (closures); P04.12 (debug info); `docs/design/calling-convention.md`; [ADR-0001](0001-implementation-language.md).

## Context

Platform C calling conventions return at most two words in registers (`rax, rdx`
on SysV `amd64`, `x0, x1` on `arm64`), which would force every `(int, error)`
(three words: `{int64_t, itab*, data*}`) onto the stack through a hidden return
pointer. Go++ needs multi-word register returns for `(T, error)` and multi-value
functions, plus a dedicated register for closure environments (`{code, env}`),
while preserving standard C callee-saved registers so stack unwinding through
`-fexceptions` + `__attribute__((cleanup(fn)))` restores every caller register
reliably.

Clang 23 provides `__attribute__((swiftcall))` and
`__attribute__((swift_context))` directly in GNU C23 (`-std=gnu23`).

## Decision

1. **Go++-to-Go++ functions use `__attribute__((swiftcall))` on `amd64` and `arm64`.**
   - **`amd64`:** returns up to **4 integer scalars** (`rax, rdx, rcx, r8`) and
     **4 float scalars** (`xmm0`–`xmm3`) in registers. `(int, error)` (3 words)
     and any `(T, error)` where `T` is at most 2 words return entirely in
     registers; larger results such as `(string, error)` (5 words) or
     `([]T, error)` (6 words) pass via Clang's hidden return pointer.
   - **`arm64`:** returns up to **8 integer scalars** (`x0`–`x7`) and **8 float
     scalars** (`v0`–`v7`) in registers, keeping `(int, error)`, `(string, error)`
     (5 words), and `([]T, error)` (6 words) in registers.
   - **Targets without `swiftcall` support in Clang** (`linux/riscv64`,
     `wasip1/wasm32`) emit standard target C prototypes uniformly across all
     Go++ functions (`TargetLayout::supports_swiftcall == false`).
2. **Closure environments use `__attribute__((swift_context))`.**
   - Every Go++ closure body or func-value adapter (`Type::has_env`, `V078`)
     declares parameter 0 as `__attribute__((swift_context)) void *_gopp_env`
     on `swiftcall` targets (`r13` on `amd64`, `x20` on `arm64`), keeping the
     argument registers free for user parameters.
3. **Unwind safety.**
   - `swiftcall` preserves the exact same callee-saved registers as the platform
     C ABI (no callee-saved XMM registers on `amd64`), so DWARF, Compact-Unwind,
     and SEH unwinders restore all registers across panics without any call-site
     spill workarounds.
4. **C interop boundary.**
   - `@Extern("C")` declarations, `@Export("C")` thunks, C callbacks (`fn "C"`),
     and the program entry point `int main(void)` omit `swiftcall` and use the
     exact platform C ABI (`docs/design/c-abi.md`).

## Consequences

- `crates/gopp-codegen-c` emits `GOPP_CALL` (`__attribute__((swiftcall))` on
  `amd64`/`arm64`, empty on `riscv64`/`wasm32`) on every `CallConv::Go` function
  prototype, definition, and function-pointer `typedef`.
- `gopp_gir::verify_module` (`V075`, `V078`) checks that every direct and
  indirect call matches its callee's calling convention and closure-environment
  flag before C23 code generation.
- `gdb` and `lldb` `finish` read 1- and 2-word returns accurately out of the box;
  3- and 4-word `swiftcall` register returns require the Phase-3 debugger plugin
  (`P14.07`).
