# GIR: the Go++ mid-level IR

GIR sits between the checked AST (`crates/gopp-sema`) and the Clang-23 C23/C++20 backend (`crates/gopp-codegen-c`, [ADR-0001](../adr/0001-implementation-language.md)). It is shaped like Rust's MIR and Swift's SIL: typed, a control-flow graph of basic blocks, values in *places*, computed by *rvalues*, and **not SSA** (Clang 23 builds SSA from emitted C23 stack locals when optimizing). This document is the contract for the middle-end crates: `crates/gopp-gir`, `crates/gopp-plugin-api`, and `crates/gopp-plugin-host`.

## 1. Where GIR sits

```text
 gopp_gir::Checked (the package, its files, the checker's tables)
        |  gopp_gir::lower_package       reads Info, never re-derives
        v
  gopp_gir::Module
        |-- verify_module (V001..V085)
        |-- MoveCheck + BorrowCheck + ConcurrencyCheck  (mandatory safety gate)
        |-- -fpass-plugin [PreDropElab]  --> verify_module
        |-- DropElaboration + gir-inline --> verify_module
        |-- -fpass-plugin [PostDropElab] --> verify_module
        v
  gopp_codegen_c::emit_c23  -->  clang-23 (-std=gnu23 -fuse-ld=lld)
        |
        +-- gopp_gir::gopx::{encode,decode}   (postcard + zstd + blake3, section 13; .gopx)
        +-- gopp_gir::print_module            (text, `gopp build -emit=gir`)
```

Rules that keep the layers apart:

- **`gopp-gir` has zero Clang, LLD, or FFI dependencies.** Cargo's crate graph enforces the boundary at compile time (`#![forbid(unsafe_code)]` in `gopp-gir`). Only `gopp-codegen-c` knows Clang-23 C23 syntax and driver flags, and only `gopp-cimport` touches `clang-sys`. The compile-time macro interpreter (`P09`) and external `-fpass-plugin` crates run directly on `gopp-gir`.
- **The builder reads the checker, it does not decide again.** The type of an expression is `Info::type_of`, a constant is `TypeAndValue::val`, and what a name denotes is `Info::uses` / `defs`. Nothing in GIR lowering looks a name up or works out a type. If a feature needs a fact the checker does not record, the fact is added to `Info`, never guessed in GIR.
- **One way to make a body:** `gopp_gir::FuncBuilder`. It computes the type of every rvalue in one place; `verify_module` checks every type independently.
- **Lowering runs side by side.** `lower_package` cuts the functions of a package into runs of at least `256 KiB` of source, lowers each run into a module of its own on a `rayon` worker, and merges the runs: the functions of the package first, in source order, then every type, string, global and declaration of another package where a walk over those functions first names it. A run asks the checker's shared tables once per type, callee and name and keeps the answers. The merged module is the same for any number of threads, and `verify_module` runs on it before it is returned.

## 2. Data structures

All nodes inside `crates/gopp-gir` refer to their parts by typed arena **index** (`la_arena::Idx`: `LocalId`, `BlockId`, `FuncId`, `TypeId`, `Spur`), never by raw pointer, so a body can be cloned, substituted, serialized, and compared deterministically. Structural types are interned in `gir::TypeInterner`.

```text
Module      Types, Name (package path), Files[], Strings[], Funcs[], Globals[]
Function    Name (symbol), Sig (a Func TypeId), Link, CC, NoUnwind, Inline, Loc
            Locals[] (0 = return place, 1..N = parameters, then the rest)
            Blocks[] (0 = entry; empty = a declaration)
            Scopes[]: the debug scopes, a tree (section 14)
Local       Kind (Return, Arg, Var, Temp), Ty, Name, Mutable, Owned, Glue, Loc
            Source (declared by the program), Scope, ArgNo, Origin, Deref
BasicBlock  Stmts[], Term, Cleanup, Catch, Cold
Statement   Assign(Dest place, Rvalue) | StorageLive(L) | StorageDead(L) | Nop
            Boundary: a copy made by GIR inlining (never read from a file)
            Prologue: entry-block setup emitted before body line numbers
            DropOld: store replaces a live owned place
            Scope: the debug scope it is in (section 14)
Terminator  Goto | Switch | Return | Unreachable | Call | Drop | Assert | Resume
            Scope, as a statement's
DebugScope  Block (a `{}` of the source) | Inlined (an inline site: Callee at Loc)
            Parent (an earlier scope; 0 = the function), Loc, Callee, Origin
Span        {File+1, Line, Col}: self-contained, needs no SourceManager
```

Design points:

- **Locals are stack slots, not SSA values.** A `Local` may be assigned many times; GIR needs no $\phi$ nodes.
- **Temporaries** (`Local::Temp`) hold intermediate results, carrying `StorageLive` / `StorageDead` and `Local::Owned` where lifetime and drop elaboration require them.
- **`StorageLive` / `StorageDead`** mark the extent of a source variable. The builder maintains a scope stack; leaving a scope by falling through, `return`, `break`, `continue`, or `goto` emits `StorageDead` for the locals of every crossed scope, innermost first (`exit_scopes`). Drop elaboration uses these exact exits (section 7).
- **Functions are separate objects with mangled symbols as names.** Calls name their callee as an operand (`Constant::Func` for direct calls, or a function-pointer `Place` for indirect calls). `gopp_mangle::symbol_of` (`crates/gopp-mangle`, [appendix A](../spec/appendix-a-symbol-names.md)) decides every symbol name in one place: `_GP...` for Go++ functions, methods, instances, closures, and glue; bare C names for `@Extern("C")` and `@Export("C")` thunks; `main` for the C entry point; and `_gopp_...` for compiler runtime helpers.
- **`NoUnwind` and `NoReturn`** sit on `Function` (lowered to `__attribute__((nothrow))` and `__attribute__((noreturn, cold))` in C23). A call to a `NoReturn` function may omit its return target (`Target == None`, verified by `V028`).
- **Calling convention (`CallConv`):** `Go` is the internal convention (`__attribute__((swiftcall))` on `amd64`/`arm64`, C fallback on `riscv64`/`wasm32` and for a signature with a value of 3, 5, 6 or 7 bytes after its whole words, [ADR-0030](../adr/0030-amd64-calling-convention.md), [calling-convention.md](calling-convention.md)); `C` is the target C ABI ([c-abi.md](c-abi.md)). `CallConv` is part of `Func` types (printed `fn "C"(...)` for C), and a function's `CC` equals its signature's (`V075`). A Go++ function reaches C only through a C-ABI thunk (`_GPTC...` or `@Export("C")`), and indirect `fn "C"` calls pass scalars only (`V076` until `P07.08`). Closure code pointers carry `has_env` (`fn env(...)`, `V078`), lowered via `__attribute__((swift_context))` on `amd64`/`arm64`.

## 3. Types

GIR types have fixed target width (`int` is resolved to `i64` on 64-bit targets or `i32` on `wasm32` via `TargetLayout`), carry nominal names only on structs and recursive `Rec` shells, and are cheap to intern, substitute, and serialize.

| Kind | Meaning | Language types it stands for |
|---|---|---|
| `Unit` | empty tuple, size 0 | `void` / no result; `struct{}` |
| `Bool` | 1 byte in memory (`bool` in C23) | `bool` |
| `Int(bits, signed)` | 8, 16, 32, 64 | all integer kinds; `int`/`uint`/`uintptr` take `TargetLayout::pointer_bits` |
| `Float(bits)` | 32, 64 | `float32`, `float64` |
| `String` | `{ptr *u8, len u64, backing *u8}`; fields 0, 1, 2 ([ADR-0026](../adr/0026-no-small-string-optimization.md) + [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md): no SSO; a literal or empty string points to rodata/null with `backing == null`, a heap string points `backing` at its `{refcount, byte_cap}` header with atomic `refcount` and a trailing NUL byte, `TYP-13`, `SLC-5`) | `string` |
| `Ptr(T)` | single pointer | `*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`; `unsafe.Pointer` is `*u8` |
| `Slice(T)` | `{ptr *T, len u64, cap u64, backing *u8}`; fields 0..3; 4 words ([ADR-0029](../adr/0029-slice-and-map-allocator-word.md) + [ADR-0041](../adr/0041-values-are-copied.md)). `backing` is null for a nil or empty slice and for a `&s[i:j]` view, and otherwise points to the header `{alloc_cap, alloc}` at the start of the buffer. One owner, no reference count (section 15) | `[]T` |
| `Array(T, N)` | `N` elements | `[N]T` |
| `Tuple(T...)` | C-aligned anonymous struct | multi-value results, `(T, error)`; `complex64`/`complex128` are `Tuple(f, f)` |
| `Option(T)` | `T` followed by a 1-byte `bool` flag, padded to `T`'s alignment; fields 0 (`T`) and 1 (`bool`) | `Option[T]` when `T` has no pointer/interface/func niche |
| `Struct(name)` | nominal struct; body set once (supports recursive pointers/slices) | defined structs and generic struct instances (`Type::type_args` carries generic arguments) |
| `Rec(name)` | self-naming nominal shell whose body may be any non-struct type referencing the `Rec`; canonicalized so identical recursive shapes share one `TypeId` | recursive non-struct definitions: `type S []S`, `type M map[string]M`, `type O Option[*O]`, `type A [2]Option[*A]`, `type L [1]*L` |
| `Func(P..., R, has_env, cc)` | function pointer type with calling convention (`Go` or `C`) and optional closure environment flag | code pointer of a `func` value; C callback (`fn "C"`) |

**Single mapping point (`lower_type`):** `Lowering::lower_type` is the only function mapping `sema::TypeId` to `gir::TypeId` (memoized per `sema::TypeId`):

| Checker type | GIR representation |
|---|---|
| `bool`, integers, floats, `string` (untyped constants resolved to default types) | Direct table entry |
| `*T` | `Ptr(T)` |
| Defined type over a non-recursive basic/slice/map type | Underlying GIR type (methods lower to functions) |
| Defined struct type | `Struct` named by the symbol of its type descriptor (`_GPTiN4mainE5Point`), with declaration-order fields and C alignment |
| Defined non-struct type reaching itself | `Rec`, named as a defined struct is |
| `[N]T`, `[]T` | `Array(T, N)`, `Slice(T)` |
| `Unique[T]`, `Shared[T]`, `Weak[T]` | `Ptr(T)` (1-word payload pointer `T*`; for `Shared`/`Weak`, `{strong, weak, alloc}` precedes `top = (byte*)p + vptr.offset_to_top`, `TYP-19`, `OWN-P3`, `OWN-P4`) |
| `map[K]V` | `Ptr(Struct("gopp.map"))` (key/value sizes and glue passed via `gopp.mapinfo`, section 15) |
| `chan T` | `Ptr(Struct("gopp.chan"))` |
| `interface` | `Struct("gopp.iface")` = `{itab *u8, data *u8}`; `Option[I]` for a non-nilable interface with methods uses the same type with null `itab` as `nil` |
| `func` value | `Tuple(Func(env *u8, ...; has_env), *u8)` = `{code, env}` (`16` bytes on 64-bit); `Option[F]` uses the same type with null `code` as `nil` |
| `Option[P]` (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`), `Option[I]`, `Option[F]` | Niche optimization: identical layout to `P` / `I` / `F` with null as `nil` (`Option` on already-nilable types like `[]T`, `map[K]V`, `chan T`, `any`, `error`, `unsafe.Pointer`, or `Option[U]` is rejected as `E0446`, `OWN-P9`) |
| `Option[T]` otherwise | `Option(T)` (`{value, set}`; padding and unset payloads never participate in `==` or hashing) |
| Unsubstituted type parameter | Unreachable in GIR (monomorphized per instance in section 10; internal error `E0440` if encountered) |

**Struct names.** A struct is found by its name, in a run and in the merge. A declared struct has the symbol of its type descriptor ([appendix A](../spec/appendix-a-symbol-names.md)): `_GPTiN4mainE5Point` for `main.Point`, `_GPTig0_1TF4mainE1f` for the first type `T` that the function `main.f` declares, `_GPTiIN4mainE3BoxiE` for `main.Box[int]`. That name is the same in every package and no other type has it; `gopp build -emit=gir | gopp demangle` reads it as `typeinfo for main.Point`. A struct without a name is `struct{a T; b U}` with the lowered field types. A defined non-struct type that reaches itself is reported as `E0439` until `Rec` lowering lands with the collections (`P04.13`).

Any construct not yet supported in a phase emits `E0439` (`err_gir_unsupported`) once at its span and substitutes a typed zero placeholder (`zero_of_expr`) so downstream statements produce no cascading errors; any missing checker invariant emits internal error `E0440`.

## 4. Places

A `Place` is a `LocalId` plus a `SmallVec` of projections; every projection records its resulting `TypeId`, so querying a place's type is $O(1)$.

| Projection | Meaning |
|---|---|
| `Deref` | `*p`; base must be `Ptr(T)` |
| `Field(N)` | field `N` of `String`, `Slice`, `Tuple`, `Option`, or `Struct` |
| `Index(L)` | element of `Array` or `Slice`, or `u8` byte of `String`; `L` is an integer `LocalId` (bounds check is an explicit preceding `Assert`) |
| `ConstIndex(N)` | constant element `N` of `Array` |
| `Downcast(N)` | variant payload `N` |

A **move path** (section 7) is a place containing only `Field`, `Deref`, and `ConstIndex` steps; runtime-indexed `Index(L)` places are never move paths.

## 5. Operands, constants, and rvalues

- **`Operand = Copy(Place) | Move(Place) | Const(Constant)`**
  - `Copy` reads a place without invalidating it (emitted for trivial `OWN-C1` values, for borrowed reads, for an argument passed as it is to a parameter the callee only reads, and for the source of a copy that is made, section 7).
  - `Move` transfers ownership without a copy (emitted for `move(x)`, `mem.Take` / `mem.Replace`, returning a local, and the last use of a local that can be copied, where the copy is skipped, [ADR-0041](../adr/0041-values-are-copied.md) rules 4 and 6).
- **`Constant`:** `Int` (truncated to bit-width), `Float` (IEEE-754 bits, rounded to `f32` for 32-bit floats), `Bool`, `Str` (index into `Module::Strings`), `Func` (`FuncId`), `Null`, `Zero` (all-zero value of any type), and `Global` (pointer to `Module::Globals[idx]`).
- **`Rvalue`:** `Use(Operand)`, `AddrOf(Place, Mutability)`, `Binary(BinOp, Operand, Operand)`, `Unary(UnOp, Operand)`, `Cast(CastKind, Operand)`, `Aggregate(AggKind, Vec<Operand>)`, `Len(Place)`.

All GIR operations carry **Go++ semantics** (lowered by `gopp-codegen-c` under `-fwrapv -fno-strict-aliasing -ffp-contract=off`):

- Signed `Div` / `Rem`: `MinInt / -1 == MinInt` and `MinInt % -1 == 0` (emitted via an explicit C23 helper check; non-constant zero divisors are guarded by a preceding `Assert(..., DivByZero)`).
- `Shl` / `Shr`: shift counts `>= width` yield `0` (or sign-fill for arithmetic `Shr`); signed negative counts are guarded by `Assert(..., NegativeShift)`.
- `Cast(FloatToInt)`: saturates on NaN (`0`) and out-of-range floats (`MinInt`..`MaxInt`, emitted via saturating C23 inline helpers).
- `Add`, `Sub`, `Mul`, `Neg`: wrap in two's complement (`-fwrapv`).
- Composite `==` and hashing (`String`, `Struct`, `Array`, `Complex`, `Option(T)`, `Interface`) lower to deduplicated `LinkOnceODR` glue functions (`add_eq_glue`, `add_hash_glue`).

## 6. Terminators, unwinding, cleanup, and `!`

```text
Goto(T)                                 Switch(op, [(v, T)...], otherwise)
Return                                  Unreachable
Call(callee, args, dest, T, unwind)     Drop(place, T, unwind)
Assert(cond, expected, kind, T, unwind) Resume
```

- **Calls and unwinding (`UnwindAction`):**
  - `Continue`: no local cleanup in this frame; unwind propagates to caller.
  - `Cleanup(B)`: unwind enters cleanup block `B`, which runs drops/defers and ends in `Resume` (emitted in C23 via `__attribute__((cleanup(...)))` scope guards under `-fexceptions`, or C++20 `try`/`catch (...)` frames when a function uses `defer`/`recover`).
  - `Terminate`: a panic inside a plain drop cleanup aborts the process.
  - `Unreachable`: callee is `NoUnwind` (`__attribute__((nothrow))`, including plain `@Extern("C")` functions and all calls under `-panic=abort`).
- **Cleanup & Catch blocks (`V023`–`V026`):**
  - Plain `Cleanup` blocks run only on unwind paths, enter only via `Cleanup(B)` edges (`V024`), cannot `Return`, and any calls/drops inside them use `Terminate` or `Unreachable` (`V025`).
  - `Catch` blocks (`BasicBlock::Catch`, implying `Cleanup`) implement `defer` + `recover()`: they run deferred calls LIFO, query `runtime.recover()`, and branch via `Switch` either back to normal return flow (when recovered) or to `Resume` (when unrecovered).
- **Fallible `!` propagation (`Info::Bangs`):** `f!(...)` lowers to `Call` followed by `Switch` on `err.itab == null`, branching on failure to a `BasicBlock::Cold` block (`__builtin_expect(..., 0)`):
  - `Return`: zeroes non-error return fields (`ERR-3`), writes `err`, and returns.
  - `Panic`: stores `(expr, pos, err)` into the function's shared `bang_panic` cold block, which calls `_gopp_panic_must(expr, pos, message)` with the message of the error (`__attribute__((noreturn, cold))`).
- **Runtime assertions (`Assert`):** Bounds, slice-bounds (`SliceAlen`..`Slice3C`, `SliceConvert`), nil, and division/shift checks carry message arguments (`V065`) and branch on failure to one shared cold panic block per check kind per function (`_gopp_panic_<kind>`). The first message argument of a bounds check is the index as the program wrote it, with its sign; the emitter also passes whether it is signed, so that a negative index reads as Go writes it ([runtime.md](runtime.md)).

## 7. Moves, drops, and drop elaboration

The GIR builder emits `Move` operands, `StorageLive`/`StorageDead`, and `Statement::DropOld` without inserting `Drop` terminators directly. After `MoveCheck`/`BorrowCheck`/`ConcurrencyCheck` (and `PreDropElab` plugins), `elaborate_drops` lowers ownership cleanups across the CFG:

- **Zero-on-move (no runtime drop flags for zeroable Go++ types, `OWN-R5`):** Owned non-parameter locals are zero-initialized in the entry block; moving a zeroable Go++ value copies its bytes and zeroes the source slot; every generated drop glue skips all-zero values and zeroes the slot on completion. Non-zeroable C++ values (`P08`) invoke their C++ move constructor and destructor.
- **What `elaborate_drops` rewrites:**
  1. Zero-initializes owned non-parameter locals in the entry block.
  2. Turns `StorageDead` of an owned local into `Drop` (innermost scope first, reverse declaration order).
  3. Turns `Statement::DropOld` (`x = rhs` on a live owned place) into evaluating `rhs` first (so `s = s` and `s = s[1:]` read `s` before it is dropped), dropping old `x`, then storing.
  4. Rewrites `Move(place)` into `Copy(place)` followed by zeroing `place`.
  5. Drops the by-value parameters the function owns (`OWN-C3` value parameters, plus any `OWN-C2` parameter that is the function's own value because the body does more than read it) at every `Return` and unwind path, while eliding known-zero drops within each basic block. A parameter the function only reads arrives without a copy and stays the caller's.
- **Category-aware transfers ([ADR-0041](../adr/0041-values-are-copied.md); [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md) for strings and the borrow engine):** every assignment, store, argument, return, send, `go` argument and interface conversion gives the destination its own value, unless the program cannot notice the copy.
  - `OWN-C1` (trivial): bitwise `Copy`, no glue.
  - `OWN-C2` (can be copied, and the copy is more than the bytes: `string`, `[]T`, `map[K]V`, `Unique[T]` over a `T` that can be copied, a type with `drop()` and `Clone() T`, `Shared[T]`, `Weak[T]`, `chan T`, `interface`, `func`, and aggregates of them): **passed as it is** (no copy, 0 retain/release) to a by-value parameter the callee only reads, when the argument cannot change during the call (when it is read from an aliasable place that the call could write, the copy is made first; for a counted value that is a caller temporary retain, `PAR-1`); **handed over** (`Move`, no copy, 0 retain/release) on the last read of a local, from a temporary, and by `move(x)`; **copied** when the source is read again (`OWN-C2`). A copy of a counted value (`string` backing, `Shared[T]`, `Weak[T]`, `chan T`, `func` value) is a retain (`+1`). A copy of a `[]T` or `map[K]V` is a new buffer with a copy of every element or entry, each by its own rule. A copy of an interface value is a box of its own holding a copy of the value, by the rule of that value's type (`TYP-5`). A copy of a `Unique[T]` is a new allocation holding a copy of what it points to, and a copy of a type with `drop()` is a call of its `Clone() T`. Heap `string` backings, `chan` and plain `func` closures/cells use atomic refcounts (with a `refcount == 1` relaxed fast path on final free), while non-`Sync` `Shared[T]` uses a non-atomic refcount (`CON-3`). A `[]T`, a `map[K]V` or an interface box has one owner and no refcount.
  - `OWN-C3` (cannot be copied: types with `drop()` and no `Clone() T`, `@Pinned` types, C++ classes without a usable copy constructor, or aggregates containing them): **moved** by `move(x)`, on returning a local, and from a temporary (`MoveCheck` enforces `E0451` use-after-move via a hash-consed persistent 64-ary radix trie `PathSets` with $O(\Delta)$ branch joins); a plain copy is a compile error, also on the last use (fix-its `move(x)` and `&x`); **borrowed read-only** for `*T`, pure `@Operator` methods, `@Convert` methods, `Clone() T`, and `[]T` parameters the function only reads (`PAR-5`).
- **Interface boxes, borrowed interface arguments, closures, swaps, and in-place string append:**
  - Values exceeding 1 word or carrying ownership (`map`, `Shared`, `Unique`, types with `drop()` and `Clone() T`) are heap-boxed; the box holds only the payload, with one owner and no refcount (`TYP-5`, `TYP-8`); an interface argument that the callee does not keep is passed as `(itab, &value)` with zero allocation: the address of the caller's own variable when the callee only reads the parameter, of a copy in the caller's frame otherwise (`TYP-7`). Only a kept parameter is boxed on the heap (`iface_take`). Converting a `[]T` or `map[K]V` to a kept interface is a copy like any other: the box gets its own copy, or the value itself on its last use or with `move(x)` (`TYP-6`). A value that cannot be copied (`OWN-C3`) is never converted to an interface (`TYP-6`). A type switch or assertion gives its binding a copy of the boxed value, skipped when the binding is only read (`TYP-9`).
  - Parallel swaps (`a, b, c = ...`) check aliasing via `swap_may_alias`: provably disjoint `OWN-C3` places swap with zero clones; potentially aliased `3+`-way swaps need a copy, which an `OWN-C3` type does not have: a compile error (`E0452`/`E0453`).
  - `s += x` on a uniquely owned heap string (`backing != null && refcount == 1` with spare `byte_cap`) appends in place with trailing NUL (`STM-8`, $O(\log n)$ allocations).

**Today: who owns a value, and where it ends.** Strings, slices, maps and what holds them are freed; nothing is freed while a panic unwinds (`P04.15`). A string shares its bytes, which a count in their buffer keeps: a copy of a string counts them once more (`_gopp_string_retain`), a string that ends once less (`_gopp_string_release`), and both are written with a test of `backing` before them, so that a literal calls nothing (`counted`, `Ends::Text`). A part `s[i:j]` counts the bytes too, and a `range` loop over a string holds a count of its own until it is left (`RNG-3a`). `s += x` is a call of `_gopp_string_append` with the address of `s` (`STM-8`): the string grows where it is when its buffer has one count and room behind its bytes, and gets a buffer half as large again otherwise, after which the old one is counted down; so a string that is built piece by piece is copied as often as its length grows by half, and a string that shares its bytes (a copy, a part, a literal) is never written to. An entry of a map is still joined into a new string. There is no `elaborate_drops` pass yet: the lowering writes every end itself, as a call of the **drop glue** of the type (`_GPTd<type>`: the parts of the value first, then `_gopp_slice_free` or `_gopp_map_drop`), and `Move` operands and `DropOld` are not in the module. The move check does not wait for the pass: it runs in the checker (below). The rules that the lowering keeps (`lower/slice.rs`, `lower/func.rs`):

- A value that holds what is freed has **one owner** (a string is such a value: it owns one count of its bytes): a variable or a parameter, a temporary, or the place inside another value.
- A **new value** (what a call gives, each of several results by itself, `make`, a literal, `append`, a conversion, a slice `x[i:j]`, a copy) is owned by its temporary (`fresh`, `FuncCx::temps`).
- Where a value is **given** (an assignment, a declaration, an argument, a receiver by value, a result, a part of a literal, an appended value) its temporary gives it up (`taken`). A place that the checker marks as copied gives a copy (`given`); a place marked as moved gives its value and is empty then, which is written after everything beside it is evaluated, so `return s, len(s)` reads `s` first; a place without a mark gives a copy, never its value. A local variable that is named directly in a `return` hands its value over instead of a copy: it ends with the function.
- An argument for a parameter that its function **only reads** is **lent** (`PAR-1`, `lower/facts.rs`): the caller stays the owner, nothing is copied or counted, and the function frees nothing. A function only reads a parameter if it never writes it or a part of it, takes no address of it, moves nothing out of it and does not return it; a copy that it gives away is a read. The fact is worked out for every function of the package before any is lowered (`Facts::lent`, `Shared::lent` by symbol), from its body alone, so it needs no fixed point. What the call could change is not lent as it is: only a temporary, or a place in a variable of the caller that nothing else reaches (`Facts::quiet`: no address of it is taken) and that is reached through no pointer; of any other place (a variable of the package, what a pointer leads to, a variable whose address is taken) the copy is made first and ends after the call. A function with a function literal, a `go` or a `defer` statement owns all its parameters. A call into another package knows no fact and gives every argument away, so the facts must go into the export data with `P06` before packages are linked together.
- A copy of a variable that is its **last use** gives the value itself (`Body::last_uses`, below), if the variable owns the value (it is no lent parameter, no element as it is, no other name of an element) and nothing else reaches it (`Facts::quiet`). The variable is empty from there on, like one that `move(x)` took the value of.
- What a temporary still owns when its **statement ends** is freed there; for a condition, before either way is taken; what is made to be walked by a `range` loop lives as long as the loop.
- A **variable** that owns is freed where its scope is left, by every way out (`exit_scopes`: the end of the block, `break`, `continue`, `return`, `goto`), the last declared first. A parameter by value that the function does more to than read is the function's own, since the caller gave a copy or the value itself. A plain range variable of a Go++ file owns nothing.
- A place that is **assigned to** ends its old value after the new one is evaluated and before it is stored; `s = append(s, ...)` and `s = s[i:j]` keep what `s` has, and the second ends the elements that it cuts off. `delete`, `clear`, `copy` and a write to a map entry end what they overwrite.
- Places that **exchange** their values (`a, b = b, a`, which the checker marks in `Body::swaps`, `OWN-R4`) make no value and end none: the bytes of all are read first and stored as they are (`exchange`). With two places that is right also where the two are one place (`xs[i], xs[j] = xs[j], xs[i]` with equal indexes). With three or more, the bytes change places only if the places are variables of the function, no two of which can be one; otherwise two of them may be one place, a value could be lost and another kept twice, so each place gets a copy and ends what it had, as in any other assignment, and so does an entry of a map. A value that cannot be copied is not supported there yet (`E0439`; the checker's own report is `E0452`). A Go file has no exchange: every value is copied.
- A drop of an empty value does nothing, so a place that was moved out of needs no flag.

`tests/run/owners.gopp`, `tests/run/moves.gopp`, `tests/run/last_use.gopp`, `tests/run/last_uses.go`, `tests/run/swaps.gopp` and `tests/run/swaps_go.go` go through these ways; the long test `no_program_of_tests_run_leaks_or_reads_what_is_freed` builds every run test with the address, leak and undefined-behaviour sanitizers and takes no report; `tests/run/string_owners.go` prints what Go prints.

**Today: the move check, the last use and the lost write.** All are worked out by the checker (`gopp-sema`, `typecheck/moves.rs`) for every body after it is checked, on the syntax tree, so `gopp check` reports them and they hold in Go files too. A value is moved as a whole variable only ([ADR-0041](../adr/0041-values-are-copied.md) rule 6), so the sets are sets of variables, not of paths. Each function, and each function literal by itself, is a graph of blocks; a block is a run of events in the order of evaluation: a variable is *read* (also when a part of it is written, when its address is taken, and where a literal that uses it is written), *given* (a copy of the whole variable that the checker marked in `var_modes`), *set* (assigned as a whole, or declared), *moved* (`move(x)` that the checker accepted), or an element of it is *written* (only for a parameter or receiver by value that is a slice or a map, in a Go++ file). The graph follows `if`, `for`, `range`, `switch` with `fallthrough`, `select`, labels, `break`, `continue`, `goto` and `return`; a `return` without values reads the results that have names, one with values sets them. Passes run over it with a work list, each only when the body has its events:

- **Forward, the variables that may be moved.** A read, a copy or a second `move` of one is `E0451` (`OWN-R5`), with a note at the `move`; one report per block and variable. A whole assignment gives the variable a value again.
- **Backward, the variables that are read later.** A *given* event after which its variable is not read on any way is a last use. Three things are never one: a copy in a statement that reads the variable a second time (one statement may read its operands in any order: `f(x, x)`, `g(x, len(x))`); a copy of what a `range` loop walks, inside the loop, and of what a `switch` is on, while its cases are compared (the statement reads the value where it is); and a copy of a variable whose address is taken or that a function literal uses.
- **Backward again, the parameters that are read later.** A *written* event after which its parameter is not read, given, moved or returned on any way is a lost write: `E0490` (`PAR-1`, ADR-0041 rule 9), reported once for a parameter, at its first such write. The events are `xs[i] = v` and every other assignment to a place under the parameter, `xs[i]++`, `xs[i] += v`, `delete`, `clear`, and `xs = append(xs, ...)`. None of them reads the parameter for this pass, so a loop that only writes is seen through. Three writes are never reported, because something may see them: one inside a `range` loop over the parameter that has a variable for the elements, one to a parameter that a function literal uses, and one to a parameter whose address is taken. A Go file has no such rule.

A `move(x)` of a variable that a statement around it still reads where it is (the `range` loop over it, the type switch on it) is `E0264` (`OWN-B2`), said in the walk: the loop would go on with a value that is gone. This is the one case of the borrow check that is built.

The sets of the passes are `Sets`: tries of 64 children per node in which equal sets are one node, so a join costs as much as its two sides differ and a fixed point is found by comparing two numbers. The steps grow with the source (`the_steps_of_moves_and_last_uses_grow_with_the_input_not_faster`: thousands of sibling blocks, long-lived variables, labels, and loops and branches nested 400 deep). Not done yet: a `move` of a variable that a function literal made earlier still uses is not refused (it waits for closures and the borrow check), and a copy that is the receiver of a method is never a last use.

**Today: `defer`.** A first part of `P04.15` is built: `defer f(args)` and `defer x.m(args)` for a function or a method of the package, on the ways out that are no panic. The function looks for its `defer` statements before its body is lowered (`find_defers`) and gets a flag for each, which its first block clears; that block is written last, because what a statement keeps is known when the statement is lowered. At the statement the receiver and the arguments are evaluated, each for an owner of its own (a copy where the checker marks one, the value itself from a temporary, on a last use or with `move`; a pointer receiver is the address), and put into slots of the function (`defer_call`, `Pending`); then the flag is set. Where the function is left, by a `return` or at its end, it goes to one block that all ways out share (`run_defers` notes the way in a slot and the block goes back by it, so the calls are written once). That block goes through the statements from the last to the first and calls those whose flag is set: a statement in no loop runs after those before it in the source, so that is the order of `PAN-4`. The call takes the slots; what it gives ends at once, and an argument for a parameter that the function only reads is ended by the statement after the call, since the function does not end it. The values of a `return` are worked out before the calls, and the variables of the function end after them. Not built yet, each reported as `E0439`: a `defer` in a loop or in a function with a label (the calls need a list that grows), in a function whose results have names (a deferred call may write them), of a built-in function, a conversion or a function literal, a deferred call that holds a variable of a block by its address (`PAN-3`: the variable must live as long as the function), and a call whose result may be a view. Nothing runs while a panic unwinds: a failed check ends the program as before.

**Today: copies.** Where the checker marks a place as copied for its new owner (`Info::var_modes`, in Go files too), the lowering gives the owner a copy: none for a value that is copied by its bytes, one more count for a string, `_gopp_slice_copy` for a slice of such values, `_gopp_map_copy` for a map, and for every other value the **clone glue** of its type. The glue is a function `_GPTc<type>` that takes the address of a value and makes the value a copy of its own: a slice gets its buffer and then each element its glue, a map its table (its record of the type carries the glue of the values), a struct each field that needs it, an array each element, an option what it holds. `lower/glue.rs` declares the glue where a function asks for it and checks then that every part of the type has one; the package makes each glue once, in a part of its own that is lowered after the functions and merged last, in the order of the symbols, so the module is the same for any number of threads. Glue is `LinkOnceODR` and has the convention of C, because the runtime calls it too (`_gopp_each` for the elements of a slice or an array, `_gopp_map_copy` for the values of a map). A struct type with a `drop` method (`OWN-R7`) is never copied by its bytes, whatever its fields are (`Memo::dropped`): its drop glue calls `drop` with the address of the value and then ends the fields that need it, and it is called for a value that was moved away too, which is all zeros and which `drop` must take (`OWN-R5`). If the type has the copy method `Clone() T` (`OWN-R6`), its clone glue calls it with the value and puts what it gives in its place; the method is lent its receiver, so one that does more to it than read is not supported yet. A type without the copy method is never copied: the checker refuses the copy, and a map with such values is not supported yet, because the copy of a map copies them. Not supported yet either (`E0439`): a `drop` method of a type that is no struct, a `drop` or a `Clone` method of another package, an interface value and a `Unique`. In a Go file a method named `drop` or `Clone` is a method like any other. `tests/run/drops.gopp` ends values by every way out of a block, in a struct, an array and a slice, after a `move`, and as temporaries; `tests/run/drop_method.go` prints what Go prints. **Equality and hash glue** (`lower/equal.rs`) are made the same way: `_GPTe<type>` takes two addresses and says whether the values are equal, `_GPTh<type>` takes an address and a seed and gives the hash. A value that is equal exactly when its bytes are (integers, booleans and pointers without padding and without a `_` field) is one `_gopp_mem_eq` or `_gopp_hash_bytes`; a struct is compared field by field, leaving the first difference at once and skipping the fields named `_`, and is hashed with each field seeded by the hash of the fields before it; an array goes through `_gopp_each_eq` and `_gopp_hash_each` with the glue of its elements; a float is compared as a float and hashed so that the two zeros have one hash. `==` and `!=` on structs and arrays and the cases of a `switch` over them call the equality glue with the addresses of two temporaries; a map whose keys the runtime cannot compare itself carries both functions in the record of its type. Interface values have neither yet. The other places that copy elements call the glue as well: `x[i:j]` as a value, `append(s, t...)`, `copy`, `v, ok := m[k]`. A plain range variable is a copy of its element in a Go file, which may write it; in a Go++ file it is only read, so it is the element as it is (`RNG-1`).

## 8. Ownership, borrow, and concurrency checks — plus `-fpass-plugin`

`MoveCheck`, `BorrowCheck`, and `ConcurrencyCheck` run per function over the GIR CFG before drop elaboration, using `Info` ownership/borrow modes (which copies are made, which are skipped on a last use, and each `move(x)`) and the cross-package function summaries kept in export data: the borrow summaries (`OWN-B3`, `OWN-B9`, and for each pointer parameter whether the function borrows into what the pointee owns, `OWN-B10`) and, for each by-value parameter, whether it is **borrowed** or **owned** ([ADR-0041](../adr/0041-values-are-copied.md) rule 4):

- Every variable access is an explicit `Place` operand or destination; `AddrOf(place, mut)` is the sole borrow-creation rvalue; backward liveness of borrow locals detects conflicting writes/moves/drops in linear time; `go` and `s.Go` closure aggregates expose exact capture modes (`CON-1`–`CON-7`).
- **One fact per by-value parameter.** A parameter is **borrowed** when the function only reads it: it never writes to it, moves out of it, stores it, returns it or sends it. Anything else makes it **owned**. The caller makes any copy that is needed. It passes the argument as it is to a borrowed parameter and stays its owner; the callee does not destroy it. It passes a copy, or the value itself on a last use, with `move(x)` or for a temporary, to an owned parameter; the callee owns it and destroys it. A call that cannot see the body (through an interface, a `func` value, an itab or method-value thunk) treats every by-value parameter as owned; a function with borrowed parameters is reached from such a call through a generated thunk that calls the body and then destroys the arguments the body only read ([calling-convention.md](calling-convention.md)). An interface parameter has a second fact, **kept**: the function stores, returns or sends it (`TYP-7`). Only a kept parameter needs a box.

### 8.1 Middle-IR Pass Plugins (`-fpass-plugin`)

External GIR optimization, analysis, instrumentation, and equality-saturation (`egglog` `3.0.0`) passes plug into the middle end via Clang-style CLI flags ([ADR-0001](../adr/0001-implementation-language.md)):

- **CLI flags:** `-fpass-plugin=<path.{so,dylib,dll,wasm}>` (repeatable, executed in CLI order) and `-fplugin-arg-<plugin_name>-<key>=<value>`.
- **Stable ABI (`crates/gopp-plugin-api`):** Exposes `#[repr(C)]` `abi_stable::StableAbi` types (`GirPassPlugin_TO`, `PassStage::{PreDropElab, PostDropElab}`, `PluginMetadata`, `PluginDiagnostic`) and `GOPP_PLUGIN_API_VERSION`. Native shared libraries export an `#[export_root_module]` root module loaded by `crates/gopp-plugin-host` via `abi_stable` (`0.11.3`) + `libloading` (`0.9.0`), verifying ABI layout hashes and API versions at load time. Optional `.wasm` plugins exchange serialized `gopp_gir::Module` payloads via `rmp-serde` (`1.3.1`) inside a deterministic `wasmtime` (`50.0.1`) sandbox with zero host filesystem or network imports.
- **Mandatory safety firewall:**
  1. `MoveCheck`, `BorrowCheck`, and `ConcurrencyCheck` **always run before `PreDropElab` plugins**, so an external pass can never erase or hide an ownership, borrow, or concurrency violation.
  2. `gopp_gir::verify_module` (`V001`–`V085`) **runs immediately after every individual plugin invocation** (`PreDropElab` and `PostDropElab`). Any broken CFG, type, dominance, or cleanup invariant halts compilation with `E0440` (`err_gir_verify`), naming the offending plugin and rule.
- **Deterministic caching:** `gopp-plugin-host` hashes every loaded plugin binary and its sorted `-fplugin-arg-*` flags with `blake3` (`1.8.7`) into `.gopx` and object cache keys.

## 9. Verifier (`V001`–`V085`)

`gopp_gir::verify_module` runs after `lower_package`, after every `.gopx` decode, after every `-fpass-plugin` pass, after `elaborate_drops` / `inline_trivial`, and at the entry to `gopp-codegen-c`. It trusts no cached types, uses iterative worklists throughout, and runs in $O(|M|)$ time. Functions are checked in parallel; errors are reported in function order for any thread count. Errors format as `V027: fn main.f: bb3 #2: duplicate switch case 1`.

| Rule | What is checked |
|---|---|
| `V001` | Every function exists and has a unique symbol name. |
| `V002` | Type shapes: integers `8/16/32/64` bits, floats `32/64`, every referenced `Struct` or `Rec` has a body, no missing parts. |
| `V003` | No `Struct` or `Rec` contains itself by value (across fields, arrays, tuples, `Option` payloads, and `Rec` bodies; pointers, slices, and strings terminate the cycle check). |
| `V010` | Function signature is a `Func` type. |
| `V011` | Local `0` is `Return` of result type; `1..=N` are `Arg`s matching signature parameter types; remaining locals are `Var` or `Temp`; `Local::Source` appears only on `Arg` or `Var`. |
| `V012` | External declarations (empty `Blocks`) have zero `Locals`. |
| `V013` | Entry block `bb0` is not `Cleanup` and has no incoming CFG back-edges. |
| `V014` | `Local::Owned` is set if and only if `Local::Glue` is present; return place `_0` is never `Owned`. |
| `V020` | Every basic block ends with a `Terminator`. |
| `V021` | Every normal and unwind target names an existing `BlockId`. |
| `V022` | Every block is reachable from `bb0`. |
| `V023` | Normal edges do not cross the cleanup boundary except out of a `Catch` block, and only a `Catch` block may branch into a `Catch` block. |
| `V024` | An unwind edge targets a `Cleanup` block if and only if the action is `Cleanup(B)` (`Catch` implies `Cleanup`). |
| `V025` | Plain `Cleanup` blocks never `Return` and only use `Terminate` or `Unreachable` unwind actions; `Resume` appears only in `Cleanup`/`Catch` blocks. |
| `V026` | `NoUnwind` functions contain no `Resume` or `Continue` actions; direct calls to `NoUnwind` callees have no `Cleanup` edge, and `Unreachable` unwind is used only on direct `NoUnwind` calls (`-panic=abort` marks functions `NoUnwind`). |
| `V027` | `Switch` case values fit the discriminant width and are distinct. |
| `V028` | Calls with `Target == None` call a direct `NoReturn` function; `Drop` and `Assert` always have a normal target. |
| `V030` | Every `Place` names a valid `LocalId`. |
| `V031` | Place's cached type equals the type derived from its local and projection steps. |
| `V032` | Projections apply to valid base types (`String` fields `0..2`, `Slice` fields `0..3`; `Index` on `Array`/`Slice`/`String`; `ConstIndex` on `Array`). |
| `V033` | Each projection step records the exact resulting `TypeId`. |
| `V034` | `Index` projection local has integer type. |
| `V035` | `Deref` projection applies only to `Ptr(T)`. |
| `V036` | `StorageLive` and `StorageDead` reference valid locals. |
| `V040` | Constants match their `TypeId` (integer bit-width, valid `Strings` index, matching `Func` signature, valid `Globals` index with `Ptr` type). |
| `V050` | `Assign` rvalue type equals destination `Place` type; `Use` yields operand type. |
| `V051` | `Unary`: `Neg` on int/float, `Not` on bool, `BitNot` on int; result matches operand. |
| `V052` | `Binary`: shifts take two ints and yield LHS type; arithmetic/bitwise/comparison operands match and comparisons yield `Bool`. |
| `V053` | `Cast` source and target types match `CastKind` (including `PtrToPtr` between `*u8` and `Func` pointers for itab slots). |
| `V054` | `AddrOf(place, mut)` has type `Ptr(place.ty)`. |
| `V055` | `Aggregate` operands match target fields/elements (`String` = 3 fields, `Slice` = 4 fields). |
| `V056` | `Len` applies to `String`, `Slice`, or `Array` and yields `u64` (`usize`). |
| `V057` | `Aggregate` destination has no `Deref` and no operand reads the destination local. |
| `V060` | `Switch` discriminant is int or `Bool`; `Assert` condition is `Bool`. |
| `V061` | `Call` callee has `Func` type. |
| `V062` | `Call` argument count and types match `Func` parameter types. |
| `V063` | `Call` destination place matches `Func` return type. |
| `V064` | `Drop` place does not have type `Unit`. |
| `V065` | `Assert` message `Args` match `AssertKind` (`Bounds`, `SliceAlen`..`Slice3C`, `SliceConvert` carry two integer operands; others carry zero). |
| `V066` | Aggregate-returning `Call` writes into a non-dereferenced, non-address-taken `Temp` local. |
| `V067` | `CallConv::C` declarations, thunks, and `fn "C"` types pass and return valid C types (`CTypeRules`, [c-abi.md](c-abi.md)). |
| `V070` | Every non-empty `Span` references a valid `Module::Files` index. |
| `V071` | Every `Global` has a unique symbol, non-`Unit` type, and scalar/string/`Zero` initializer. |
| `V072` | `Global::Parts` static data leaves cover the global's layout depth-first without gaps or extras, while scalar initializer is `Zero`. |
| `V073` | `Function::Scopes` forms a valid parent-before-child tree (`Inlined` names a defined function; `Origin` blocks lie under `Inlined` and reference callee scopes). |
| `V074` | Statements, terminators, and locals reference valid scopes; `Local::Origin` and `Local::ArgNo` satisfy inline-copy invariants. |
| `V075` | `Function::CC` equals `Function::Sig`'s `CallConv`. |
| `V076` | Indirect calls through `fn "C"` pointers pass and return scalars only. |
| `V077` | `Statement::Prologue` statements appear contiguously at the start of `bb0`. |
| `V078` | `Func` types with `has_env` carry `CallConv::Go` and `*u8` as parameter `0`. |
| `V079` | `@Inline` / `@NoInline` hints appear only on defined functions (`Blocks` non-empty). |
| `V081` | Drop glue (`Local::Glue` and `Drop` glue) has signature `fn(*u8) -> ()` with `CallConv::Go`. |
| `V085` | Every non-address-taken `Temp` local is initialized along all CFG paths reaching each read (verified via Lengauer-Tarjan dominator intervals + a backward search bounded to 32 CFG edges per read; a chain of single-predecessor blocks costs no edge, and a call initializes its destination only on its return edge). |

## 10. Monomorphization and hierarchy tables

- **On-demand `InstanceWalk` before GIR lowering:** Generic bodies are type-checked once in `gopp-sema`; `InstanceWalk` discovers all reachable function, type, and method instantiations in deterministic FIFO declaration order, applying `sema` type substitutions (`subst`) and re-resolving method selections on concrete types so `lower_package` never sees an uninstantiated type parameter.
- **Deduplication by mangled symbol:** `Box[byte]` and `Box[uint8]` produce the same mangled symbol (`gopp-mangle`) and share one GIR struct, one type descriptor, and one weak/ODR function definition (`__attribute__((weak))` / `static inline` / COMDAT).
- **Per-instance checks & complexity limits (`GEN-5`):**
  - Checks `Option[X]` where substituted `X` is already nilable (`E0446`), `@Required` fields substituted with `Option` (`E0363`), and unguarded dynamic `error` returns (`E0448`), identically in `gopp check` and `gopp build`.
  - Enforces hard expansion caps (`E0449`): constructed type-argument height `<= 48`, tree size `<= 4096`, `<= max(100_000, source_bytes)` instances per package, and `<= max(4_000_000, 16 * source_bytes)` walked AST nodes.
- **Hierarchy vtables (`P04.22`):** Types with parents (`@Extends`) and `@Virtual`/`@Abstract` methods (or tagged secondary parents) emit static vtable globals (`Struct("pkg.T.vtable")` at offset `0` — sharing a polymorphic C++ primary base's vptr slot when extending C++, `TYP-28` — plus secondary `(Child, Pi)` tables). Each vtable carries `offset_to_top` (`isize`), complete-object descriptor/drop/clone hooks, parent downcast tables, and virtual method slots (with `recv += offset_to_top` thunks for secondary overrides), keeping `Unique[P]`, `Shared[P]`, and `Weak[P]` single 1-word pointers (`TYP-19`).

## 11. Checker records consumed by GIR lowering

The tables are kept for each top-level declaration (`gopp_sema::Body`, in `Package::files[i].bodies[j]`), keyed by the expression. `gopp_sema::info_text` prints them one entry per line in source order (`gopp check -emit=info`); the text is the same for any number of threads.

| `gopp_sema::Info` table | How `gopp-gir` uses it |
|---|---|
| `types`, `values` | Expression `TypeId`s and compile-time `Constant`s |
| `defs`, `uses` | `LocalId` resolution, direct `FuncId` callees, builtins |
| `selections` (`FieldVal`, `MethodVal`, `MethodExpr`, `Base`) | `Field`/`Deref` chains (including the `ParentSteps` of inherited members), direct/itab method calls, method-value thunks |
| `instances`, `iface_conversions` | Monomorphization worklist roots and dynamic type descriptor method sets |
| `bangs` | `!` early-return vs `panic_must` lowering (section 6) |
| `operators`, `op_assigns`, `index_stores`, `case_operators` | `@Operator` and `@Convert` call lowering: `operators` for `+`, `-a`, `==`, `<`, an `Index` read, `T(a)`, the operand of `range` and a call by name that picks one of an overload set; `op_assigns` for `x op= y` (the `*Assign` method or the binary one); `index_stores` for `a[i] = v` (`SetIndex`); `case_operators` for `switch` cases (`Eq`) |
| `hier_parents`, `method_marks` | parent subobject layouts and virtual dispatch tables |
| `var_modes`, `swaps`, `self_moves` | Category-aware `OWN-C1`/`OWN-C2`/`OWN-C3` copies, skipped copies, moves (`move(x)`), borrows, and aliasing-safe swaps |

## 12. Text printer (`-emit=gir`)

`gopp build -emit=gir` and `gopp gir dump file.gopx` emit identical, deterministic human-readable GIR text:

```text
module "command-line-arguments"
file 0 "hello.gopp"

declare fn write(_1: i32, _2: *u8, _3: u64) -> i64 [c, external, nounwind]

fn main.main() -> () [go, external] {
  let _0: ()
  let _1: string
  let _2: i64
  bb0:
    _1 = const str#0 "hello\0A"  ; hello.gopp:7:2
    _2 = call const fn @write(const 1: i32, copy _1.0, copy _1.1) -> [return: bb1, unwind: unreachable]  ; hello.gopp:7:2
  bb1:
    return  ; hello.gopp:8:1
}
```

## 13. Binary `.gopx` serialization

`crates/gopp-gir/src/gopx.rs` serializes GIR modules into `.gopx` export/cache containers using **`postcard` (`1.1.3`) + `zstd` (`0.14.3`) + `blake3` (`1.8.7`)** ([ADR-0001](../adr/0001-implementation-language.md)):

```text
file = magic "GOPX" (4B) | format_version u32 LE | compiler_version str
     | blake3_digest [u8; 32] | uncompressed_len u64 LE
     | zstd_frame(postcard(ModuleEnvelope { tables, types, globals, func_decls, func_bodies, index }))
```

- **Selective per-function decoding:** `index` stores byte offsets for each `func_bodies[i]` entry so cross-package generic instantiation and inlining (`P06`) decode module tables plus a single requested function body without deserializing the rest of the package.
- **Deterministic & corruption-safe:** Type and symbol IDs are assigned in canonical traversal order (zero hash-iteration order dependence); every decoded index, enum discriminant, and slice bound is validated and passed through `verify_module` (`V001`–`V085`) so truncated or bit-flipped `.gopx` inputs always return a clean error and never panic.

## 14. Debug scopes (`Function::Scopes`)

Every statement and terminator carries a `Span` and a `ScopeId` into `Function::Scopes` (`0` = function root; `N > 0` = `Scopes[N - 1]`, ordered parent-before-child). `DebugScope::Block` represents lexical `{ ... }` blocks; `DebugScope::Inlined` represents trivial-inlined call sites (`V073`, `V074`). Struct field names live in a side table (`field_names`, persisted in `.gopx`) so structural layout identity stays fast while `gopp-codegen-c` emits exact C23 member names for DWARF ([debug-info.md](debug-info.md)).

## 15. Language lowering details

- **Strings, slices, maps, and custom allocators ([ADR-0026](../adr/0026-no-small-string-optimization.md), [ADR-0027](../adr/0027-mimalloc-global-allocator.md), [ADR-0029](../adr/0029-slice-and-map-allocator-word.md), [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md), [ADR-0041](../adr/0041-values-are-copied.md)):**
  - `string` is `{ptr, len, backing}` (`24` bytes on 64-bit); `[]T` is `{ptr, len, cap, backing}` (`32` bytes on 64-bit), where the heap/arena `backing` of a slice points to the header `{alloc_cap, alloc}` at the start of the buffer (`alloc == null` selects global `mimalloc`; non-null points to a C-ABI `{alloc, resize, free}` allocator vtable). A slice buffer has one owner and no reference count: the owner frees the buffer and drops exactly the elements `ptr` to `ptr + len`.
  - Slicing a `string` (`s[i:j]`) shares `backing` in $O(1)$ (`OWN-S1`). Slicing a `[]T` or an array (`s[i:j]`, `arr[i:j]`) is a value: a new slice with copies of those elements, and the copy is skipped when the program cannot notice it (`OWN-S2`). So `s = s[i:j]` works in place, and the elements it cuts off are dropped at that statement (`SLC-9`). `&s[i:j]` and `&arr[i:j]` are `*[]T` views of the same elements; a view and `unsafe.Slice` set `backing = null` and `cap = len` (a view is borrow-checked: it cannot outlive `s`, and `s` cannot be assigned, grown or moved while it is in use, `SLC-8`). The four-word record of a view is a hidden local of the block that holds the statement; a `defer` extends it like any local. A view written directly in a `return` is built in the caller's frame, in the hidden place that every `*[]T` result has ([calling-convention.md](calling-convention.md)). Dropping it frees nothing while `backing == null`; if code assigned or appended through the pointer, the record owns the new buffer and the drop frees it. `s` is never changed by that. `append` grows in place when `backing != null && len < cap` and otherwise allocates a fresh buffer in `alloc` (`SLC-2`).
  - `map[K]V` is a 1-word `gopp_map*` pointer to a header `{alloc, count, ...}` with no reference count; `m[k] = v` on an addressable slot initializes `nil` maps in place via `&slot` (`OWN-P9`, `PAR-1`), and `range` tracks active iterators to panic deterministically on concurrent `insert` or `clear` (`RNG-4`).
- **Strings today:** `s[i]` is a bounds `Assert` against `Len(s)` and an `Index` projection; `s[i:j]` copies the string value, asserts `j <= len` (`SliceAlen`) and then `i <= j` (`SliceB`), and builds `{AddrOf(s[i]), j - i, s.backing}` as an `Aggregate`, so no byte is copied; `+`, `==`, `!=` and the four orders are calls of the runtime ([runtime.md](runtime.md)), and a `switch` over strings is a chain of such calls; `string(r)` is a call too.
- **Slices today** (elements without a slice inside; the rest needs the copy glue of `P04.09`): a place whose value the checker marks as copied (`Info::var_modes`, in Go files too) gets `_gopp_slice_copy` where it is given to its new owner: an assignment, an argument, a receiver by value, a result. `s = append(s, ...)` and `s = s[i:j]` are found by their shape (the same place on both sides) and work on `s` where it is. `x[i:j]` builds the header `{AddrOf(x[i]), j - i, ...}` after its checks, the high bound against the length (`SLC-13`): as a value it is then copied, in place it keeps the backing and the room, and `&x[i:j]` is the address of a record with no backing and no room. An element that an assignment stores to is one of the slice as it was when the place was worked out. `make`, `append`, `copy`, `clear`, a literal and the conversions between `string` and `[]byte` or `[]rune` are calls of the runtime, which takes a slice by the address of its record ([runtime.md](runtime.md)).
- **Maps today** (keys that the runtime compares itself, values without a slice or a map inside; the rest needs the glue of `P04.09`): every operation is a call of the runtime ([runtime.md](runtime.md)) with the record of the map type, which the lowering builds from the checked type of the map, since every map type is the one type `*gopp.map` in GIR. A read copies the value into a slot; an entry is no place. `m[k] = v` works out the place that holds the map and the key with the other operands of the assignment, and at its store calls `_gopp_map_set` with the address of that place and writes through the pointer that comes back; `m[k] op= v` reads, works out the value, and then stores the same way. A place whose value the checker marks as copied gets `_gopp_map_copy` where it is given to its new owner, in Go files too.
- **`range`:** the range expression is worked out once, into a slot of its own; a hidden counter of the loop is compared with the count in the head block; each pass has a scope of its own for its variables, which get copies of the counter and of the element, so a body that changes them does not change the loop; `continue` goes to the block that steps the counter. Over an integer the counter has its type. Over an array the count is its length, and without a variable for the elements the array is not worked out at all; an array whose elements are copied by their bytes is copied before the loop, as in Go, and any other is walked where it is (`RNG-6`), as is an array behind a pointer. Over a string each pass calls `_gopp_string_decode`, which gives the code point and its width, and the counter steps by the width. Over a slice the loop walks the elements of the slice as it is when the loop starts. Over a map the head block calls `_gopp_map_iter_next`, which copies a key into a slot of the loop and gives the address of its value, which the pass copies; the scope of the loop holds the walk, and every way out of that scope (the end of the loop, `break`, `return`, a `goto`, a `continue` of an outer loop) calls `_gopp_map_iter_end` where it marks the variables of the scope dead. With `&v` (`RNG-1a`) the variable of a pass holds the address of the element (`AddrOf` of the indexed place, or the pointer that the runtime gave for a map), and every use of `v` is what that pointer leads to: nothing is copied, so the loop also walks elements that hold a slice, and an array is walked where it is. Channels and functions are not lowered yet.
- **Evaluation & assignment order (`EXP-3`):** Two-phase evaluation evaluates LHS index/pointer operands and RHS expressions left-to-right into temporaries before executing bounds-checked stores left-to-right, using replay scopes (`derive_dest`, `resolve_dest`) so container growth on an earlier tuple element never leaves a dangling slice/map pointer. An operand that is read before a later operand of the same expression calls a function is copied into a temporary first; an aggregate that is stored behind a pointer, or that reads the variable it is stored in, is built in a temporary (`V057`).
- **Pointers (`OWN-P5`, `EXP-7`):** in a `.gopp` file a `*T` where only a `T` fits is read through the pointer, an assignment that `Info::writes_through` lists stores through it, and no nil check is made (a pointer is never nil). In a `.go` file every dereference, written or implied by a field, an index or a method call, is preceded by `Assert(p != null, Nil)`.
- **Control flow:** a `switch` whose cases are integer constants is one `Switch` terminator; any other is a chain of comparisons in source order. Leaving a scope by any way emits `StorageDead` for its variables, a backward `goto` also for the variables of the label's own scope that were declared after the label. Before a function is finished, unreachable blocks are dropped, blocks that only jump on are bypassed, and the rest are laid out in the order in which they were first written.
- **Closures & `func` values (`CLO-1`–`CLO-8`):** `{code, env}` (`16` bytes); iterative escape analysis (`KeepTable` + `may_hold`) keeps non-escaping closure environments on the stack (`Immortal` tag) and allocates heap environments + refcounted capture cells for escaping closures (per-iteration cells on `for` loops, `CLO-8`), rejecting escaping stack/arena borrows (`CLO-3a`, `OWN-B5`).
- **Interfaces & itabs (`TYP-5`–`TYP-9`):** `gopp.iface` = `{itab, data}` (`16` bytes); per-type `_GPTi<type>` descriptors and `_GPTa<type><iface>` static itabs (with runtime cached `get_itab` for interface-to-interface conversions); inline storage for `<= 1`-word `OWN-C1` values and single-owner boxes with no refcount for everything else; a copy of an interface value copies the value it holds (`TYP-5`).
- **Stack discipline:** Recursive AST walks use `stacker::maybe_grow` (`0.1.25`), while every GIR pass, type walk, verifier check, `.gopx` codec, and C23 emitter walk is strictly iterative.

## 16. Verification suite

- **Unit, snapshot & property tests (`cargo test --workspace`):**
  - `insta` (`1.49.0`) snapshots for GIR text (`-emit=gir`) and emitted C23 (`-emit=c`) across ownership, generics, closures, interfaces, `@Operator` methods, and C-ABI thunks.
  - Positive and negative tests for every verifier rule `V001`–`V085`, `.gopx` deterministic round-tripping and bit-flip fuzzing, and `proptest` (`1.12.0`) pass-equivalence suites (including `-fpass-plugin` hooks).
  - Step-counting linearity stress tests (`4x` input `<= 4.5x` steps against a quadratic control) across `MoveCheck`, `BorrowCheck`, `InstanceWalk`, `verify_module`, FMA matching, and type emission.
- **Differential & sanitizer execution (`gopp test-run`, `tests/run/`):**
  - Scalar, string/slice/map collection, generic, closure, interface, panic, and C-ABI differential suites verified against Go reference goldens across `-opt=0..z` on `linux/amd64` and `windows/amd64` ([ADR-0037](../adr/0037-v1-targets.md)), plus AddressSanitizer (`-sanitize=address`) across all ownership categories, with LeakSanitizer zero-leak verification on Linux (it does not exist on Windows).
- **Fuzzing ([fuzzing.md](fuzzing.md)):** generated valid programs through every oracle, injected rule violations that `MoveCheck`/`BorrowCheck`/`ConcurrencyCheck` must reject, random GIR modules for pass equivalence, and mutated modules that test `verify_module` for missing rules.
