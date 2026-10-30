# GIR: the Go++ mid-level IR

Task P04.01 · decisions D4.1 to D4.5, D4.15 to D4.20 of `plans/2026-10-27-goplusplus-language/04-gir-and-llvm-codegen/README.md`.

GIR sits between the checked AST and LLVM IR. It is shaped like Rust's MIR and Swift's SIL: typed, a control-flow graph of basic blocks, values in *places*, computed by *rvalues*, and **not SSA** (LLVM's `mem2reg` and SROA build SSA). This document is the contract for P04.04 to P04.24: every later task adds to a part named here and says which. The code is `compiler/lib/GIR` and `compiler/include/gopp/GIR`.

## 1. Where GIR sits

```text
 sema::CheckedPackage (AST + sema::Info)
        |  gir::lowerPackage        Lower.cpp   reads Info, never re-derives
        v
   gir::Module  --verify-->  per-function passes (sections 7, 8)  --> CodeGen
        |                                                          (LLVM IR)
        +-- gir::writeModule / readModule   (bitstream, section 13; .gopx)
        +-- gir::print                      (text, `gopp build -emit=gir`)
```

Rules that keep the layers apart:

- **GIR does not include LLVM IR, Clang or LLD headers.** `tools/check-frontend-includes.sh` checks it (GIR may include `llvm/ADT`, `llvm/Support` and `llvm/Bitstream`). Only `CodeGen` knows LLVM. The macro interpreter (P09) runs GIR, so GIR must stand alone.
- **The builder reads the checker, it does not decide again.** The type of an expression is `Info::typeOf`, a constant is `TypeAndValue::Val`, what a name denotes is `Info::Uses` / `Defs`. Nothing in `Lower.cpp` looks a name up or works out a type. If a later task needs a fact the checker does not record, the fact is added to `Info` (P03 style), not guessed in GIR.
- **One way to make a body:** `gir::FuncBuilder` (`Builder.h`). It computes the type of every rvalue, so types are made in one place; the verifier checks them again.

## 2. Data structures

All in `GIR.h`. Everything inside a body refers to its parts by **index** (`LocalId`, `BlockId`, `FuncId`, string id), never by pointer, so a body can be copied, substituted, serialized and compared. The only pointers are `gir::Type*`, uniqued in a `gir::Context`.

```text
Module      Ctx, Name (package path), Files[], Strings[], Funcs[]
Function    Name (symbol), Sig (a Func type), Link, CC, NoUnwind, Loc
            Locals[] (0 = return place, 1..N = parameters, then the rest)
            Blocks[] (0 = entry; empty = a declaration)
Local       Kind (Return, Arg, Var, Temp), Ty, Name, Mutable, Loc
BasicBlock  Stmts[], Term, Cleanup
Statement   Assign(Dest place, Rvalue) | StorageLive(L) | StorageDead(L) | Nop
Terminator  Goto | Switch | Return | Unreachable | Call | Drop | Assert | Resume
Span        {File+1, Line, Col}: self-contained, needs no SourceManager (D4.7)
```

Design points:

- **Locals are stack slots, not values.** A `Local` may be assigned many times. LLVM turns the slots into SSA (`alloca` + `mem2reg`). GIR never needs phi nodes.
- **Temporaries** (`Local::Temp`) hold intermediate results. M0 gives them no `StorageLive` / `StorageDead`; P04.04 adds both where lifetimes matter (LLVM lifetime markers), and drop elaboration needs them for temporaries that own something.
- **`StorageLive` / `StorageDead`** mark the extent of a source variable. The builder keeps a scope stack; leaving a scope by falling out, `return`, `break` or `continue` emits `StorageDead` for exactly the locals of the scopes it crosses, innermost first (`Body::exitScopes`). This is also the shape drop elaboration needs: the *same* exits are where drops go (section 7).
- **Functions are separate objects with symbols as names.** A call names its callee as an operand (`Constant::Func`, or a pointer in a place for an indirect call). `Function::Name` is the final symbol; P04.17 owns how it is made. One function decides every name, `gir::symbolOf(SymbolKind, Package, Name)` (`Lower.h`): M0 makes `main.f` for package main, `<path>.f` for another package (`command-line-arguments` for files on the command line, as in Go), the bare C name for `@Extern("C")`, `main` for the C entry point and `gopp.<name>` for the compiler's helpers. A Go++ symbol always has a dot, so it never collides with a C name. P04.17 plugs its mangling into that one function.
- **`NoUnwind` and `NoReturn`** are on the `Function`. A call of a `NoReturn` function may have no return target (`Target == NoId`); the verifier checks that only such calls have none (V028).
- **Calling convention** (`CallConv`): `Go` is the internal convention (P04.18), `C` the exact C ABI (D4.4). In M0 both lower to LLVM's default C convention for the scalar and pointer arguments M0 has. P04.18 gives `Go` its register rules; P04.11 gives `C` the `ABIArgInfo` lowering. The callee's `CC` is on the `Function`, so a call needs nothing extra.

## 3. Types

`Type.h`. GIR has its own types: fixed width (`int` is already `i64`), no names except structs, cheap to serialize and to substitute.

| Kind | Meaning | Language types it stands for (D4.5) |
|---|---|---|
| `Unit` | empty tuple, size 0 | no result; `struct{}` |
| `Bool` | one byte in memory, `i1` in registers | `bool` |
| `Int(bits, signed)` | 8, 16, 32, 64 | all integer kinds; `int`/`uint`/`uintptr` take the target's pointer width (`LowerOptions::PointerBits`; both targets are 64-bit) |
| `Float(bits)` | 32, 64 | `float32`, `float64` |
| `String` | `{ptr *u8, len u64, cap u64}`; fields 0, 1, 2 (ADR-0026: no SSO; a literal points to rodata with cap 0) | `string` |
| `Ptr(T)` | one pointer | `*T`, `Unique[*T]`; `unsafe.Pointer` is `*u8` |
| `Slice(T)` | `{ptr *T, len, cap}`; fields 0, 1, 2 | `[]T` |
| `Array(T, N)` | `N` elements | `[N]T` |
| `Tuple(T...)` | laid out like a struct | multi-value results, `(T, error)` |
| `Struct(name)` | nominal; body set once (recursive types work) | struct types; instances have their mangled key in the name |
| `Func(P..., R, fails)` | a function pointer type (no environment) | the code part of a func value |

**One decision point for the checker's types:** `Lowering::lowerType` (`Lower.cpp`). It is the only function that maps a `sema::Type` to a `gir::Type`; it caches by `sema::Type*`. Other kinds are added there and nowhere else. Its rules today and for the tasks that extend it:

| Checker type | GIR | Task |
|---|---|---|
| `bool`, integers, floats, `string`; untyped kinds take their default (`int`, `rune` = `i32`, `float64`) | as the table | M0 |
| `*T` | `Ptr` | M0 |
| defined type over a basic type | the basic type (types are erased; methods are functions) | M0 |
| defined struct type | `Struct("pkg.Name")` with the field types, declaration order, C alignment | P04.04 |
| `[N]T`, `[]T` | `Array`, `Slice` | P04.04, P04.05 |
| `View[T]` for a string or slice `T` | `{ptr, len}` as `Tuple(*u8, u64)` | P04.05 |
| other `View[T]` | `Ptr` | P04.05 |
| `Shared[*T]`, `Weak[*T]` | `Ptr` to the control block | P04.05 |
| `map[K]V`, `chan T` | `Ptr` to the runtime object | P04.05 |
| interface | `Tuple(*itab, *data)`; `Option[I]` has the same type, `nil` is a null `itab` | P04.06 |
| func value | `Tuple(Func-pointer, *env)` | P04.07 |
| `Option[P]` for a pointer kind, `Option[I]`, `Option[F]` | the type of `P` / `I` / `F` (niche); the null pattern is `nil` | P04.04, 06, 07 |
| `Option[T]` otherwise | `Tuple(T, bool)` (flag after `T`, padding not compared) | P04.04 |
| type parameter | `Param(index, bound)`: a new kind added by P04.08; a *generic* body is built once with `Param` types and substituted at instantiation (section 10) | P04.08 |
| unit types | the base kind (units are erased, D4.20) | M0 for basic, P04.24 for the rest |

A type the lowering cannot map yet is reported with `err_gir_unsupported` (E0439), naming the construct, at its position. Nothing is silently dropped.

`Context` uniques structural types in a `FoldingSet` behind one lock. Types are the same type exactly when the pointers are equal. Nothing a type prints or serializes as depends on the order types were created in, so a context shared by threads that build different functions is deterministic in what it writes.

## 4. Places

A `Place` is a local plus projections; every projection records the type after it, so a place has a type without walking.

| Projection | Meaning |
|---|---|
| `Deref` | `*p`; the operand is a `Ptr` |
| `Field N` | field `N` of a `String`, `Slice`, `Tuple` or `Struct` |
| `Index _L` | element of an `Array` or a `Slice`; `_L` is an integer local. **No bounds check here:** the check is an `Assert` before it (P04.04) |
| `ConstIndex N` | element `N` of an `Array` |
| `Downcast N` | the payload of variant `N` (enum-like `Option[T]`, interface data, `extends` parts); verifier rejects it until P04.04/05 define the types |

A **move path** (section 7) is a place with only `Field` / `Deref` / `ConstIndex` steps. Places with `Index` of a non-constant local are never move paths: moving out of an indexed element is not allowed for owning types (the checker already rejects it).

## 5. Operands, constants, rvalues

`Operand = Copy(place) | Move(place) | Const`.

- `Copy` reads and leaves the value valid. `Move` reads and leaves the source moved-from: elaboration zeroes it (Go++ values, D4.2 step 3) or marks it moved-from (C++ values: a move constructor call, P01 D1.4 rule 9). The builder emits `Copy` for trivial types and for borrows; `Move` for `move(x)` and for the implicit moves the checker decided. M0 has only trivially copyable values, so it emits `Copy`.
- `Constant`: `Int` (bits truncated to the type's width), `Float` (the bits of a double; an `f32` is rounded first), `Bool`, `Str` (an id in `Module::Strings`), `Func` (a `FuncId`), `Null`, `Zero` (all-zero value of any type).

`Rvalue` kinds: `Use`, `AddrOf(place, mut)`, `Binary(op, a, b)`, `Unary(op, a)`, `Cast(kind, a)`, `Aggregate(kind, ops)` (tuple, array, string, slice, struct), `Len(place)`.

Operations carry **Go semantics, not LLVM semantics** (D4.12). The codegen of the operation, not the builder, implements the Go result:

- `Div` / `Rem` of signed integers: `MinInt / -1` is `MinInt`, `% -1` is 0. `Div` and `Rem` assume the divisor is not zero: the builder puts an `Assert(divisor != 0, DivByZero)` before them unless the divisor is a constant.
- `Shl` / `Shr`: a count `>=` the width gives 0 (or the sign fill for `Shr` of a signed value). A signed count is checked `>= 0` by an `Assert(NegativeShift)`.
- `Cast(FloatToInt)` saturates (`llvm.fptosi.sat`).
- `Add`, `Sub`, `Mul`, `Neg` wrap (two's complement); no `nsw` or `nuw`.

Comparisons produce `bool`. `Eq`/`Ne` are defined for scalars; strings, structs, `Option` and interface equality are calls or expansions made by P04.05, 06, 04 (they are not rvalues).

## 6. Terminators, unwind and cleanup

```text
Goto(T)                                 Switch(op, [(v, T)...], otherwise)
Return                                  Unreachable
Call(callee, args, dest, T, unwind)     Drop(place, T, unwind)
Assert(cond, expected, kind, T, unwind) Resume
```

- `if` is a `Switch` on a bool (`case 0` is the else edge). `switch` statements and type switches are `Switch` on an integer or on a tag read from an interface.
- **Calls and unwinding (D4.6).** Every terminator that can run user code (`Call`, `Drop`, `Assert`) has an `UnwindAction`:
  - `Continue`: no cleanup in this function, keep unwinding. LLVM `call`.
  - `Cleanup(B)`: run cleanup block `B`, which ends in `Resume`. LLVM `invoke` with `B` as the landing pad.
  - `Terminate`: a panic here aborts (a drop that panics while unwinding).
  - `Unreachable`: the callee cannot unwind (`NoUnwind`; every C function that is not `@Unwind`). LLVM `call nounwind`.
- **Cleanup blocks** (`BasicBlock::Cleanup`) run only on the unwind path. The verifier enforces: normal edges stay among normal blocks or among cleanup blocks (V023); a cleanup block is entered only by `Cleanup` unwind edges (V024); it ends in `Resume`, `Unreachable`, or an edge to another cleanup block, never `Return` (V025); a call, drop or assert in a cleanup block has the action `Terminate` or `Unreachable`, never `Continue` or `Cleanup` (V025: a panic during unwinding terminates, as in C++; the cleanup region cannot re-enter itself through an unwind edge, as in Rust's MIR). A `NoUnwind` function has no `Resume` and no `Continue` action; a direct call of a `NoUnwind` callee has no cleanup edge, and `Unreachable` (lowered as a `nounwind` call) is allowed only on a direct call of a `NoUnwind` callee (V026).
- **Catch blocks** (`BasicBlock::Catch`, which implies `Cleanup`) are the one cleanup kind that may leave the unwind path. They are the catch-all handler of a function with `defer` (D4.6): the deferred calls run inside it, and `recover` is a runtime call whose result a `Switch` tests. The verifier lets a catch block (a) `Return`, or branch to a normal block, after a recovered panic; and (b) make calls, drops and asserts that unwind onward (`Continue`, or `Cleanup` into a drop pad), because a deferred call that panics again replaces the panic (P01 D1.9). It still ends in `Resume` when the panic was not recovered. Only a catch block may branch into a catch block (V023), so normal code cannot fall into a handler; a catch block is also marked `Cleanup` (V024), so unwind edges name it like any pad. Plain drop pads keep the strict rules (V023, V025). The text dump prints `bbN (catch)`, the bitstream stores `BLOCK[cleanup, catch]` (magic byte `0xB2`).
- **How P04.07 uses it with ADR-0028's pad.** A function with `defer` keeps ADR-0028's single state-indexed landing pad: that pad is the catch block (`Catch`), entered by every `Cleanup` unwind edge. Each unwinding site stores its cleanup-state index into a slot before the call, and the catch block dispatches on it. GIR writes the dispatch as a `Switch`; the LLVM lowering of P04.07 turns it into compares (FastISel cannot select a `switch`, ADR-0028 Table 6). The chain below the dispatch runs the block-scoped drops of the state, innermost first; then the deferred calls LIFO, each a `Call` whose unwind is `Continue` or a `Cleanup` edge to a plain drop pad of the function-scope locals; then `recover`; then a `Switch` on "recovered": the recovered side drops the function-scope and defer-extended locals and `Return`s (or branches to the normal return block), the other side `Resume`s. Functions without `defer` use plain cleanup blocks only; the split pass and the `nounwind` rule (V026) of ADR-0028 decision 3 are unchanged by this.
- **Code generation (M0).** `Continue` lowers to a plain `call`, `Unreachable` to a `nounwind` call. A `Cleanup` edge (an `invoke` with a landing pad), a catch block, `Terminate`, `Drop` and `Resume` are refused with the task named (P04.07, P04.09): lowering them as plain calls would change what a panic does.
- **Defer and panics (P04.07).** A function with `defer` has a catch-all landing pad (a cleanup block that ends in a call into the runtime and a `Switch` on "recovered"): GIR needs no new terminator, only the `Catch` block kind above. `recover` is a runtime call. Under `-panic=abort` drop elaboration makes no cleanup blocks, and calls get `Unreachable` unwind.
- **`!` (P04.10).** `f!(...)` lowers to a `Call` followed by a `Switch` on the error word; the failure edge returns the error (`BangKind::Return`), calls the panic helper (`Panic`), or sets the deferred error (`DeferSet`). `Info::Bangs` says which, per call.
- **Bounds, nil, overflow checks** are `Assert` terminators with an `AssertKind`; the failing edge calls the runtime panic helper from codegen (M0 has no runtime: the failing edge is `llvm.trap`; P04.04 replaces it with the panic call and its message, selected by the `AssertKind`).
- **`go` / `select` / channel operations** are calls into the runtime (P12); `select` is a call plus a `Switch` on the chosen case.

## 7. Moves, drops and drop elaboration (P04.09)

The builder (P04.04) emits **no drops**. It emits moves (`Move` operands), assignments, and the scope structure of section 2. A separate pass, *drop elaboration*, adds the drops. This keeps the builder simple and gives the ownership checks (section 8) one CFG to read.

**Input facts.** For every local whose type has drop glue (`Info`/type query: an owning pointer kind, a type with a `drop` method, an owning container, a struct containing one) the pass tracks **move paths**: the local and its fields reached by `Field` steps. A path has a *state* at each point: initialized, moved-out, maybe-initialized. The state is computed by a forward dataflow over the CFG (bit-vector per move path; linear in blocks × paths, paths per function are few).

**Output.** At each place a value goes out of scope the pass inserts, in order:

1. at normal scope exit (`StorageDead` points, including those inserted for `return`/`break`/`continue`): a `Drop(place)` for each path that is *initialized*, innermost scope first, locals in reverse declaration order;
2. for a path that is *maybe* initialized: a **drop flag**, a `bool` local set to true at initialization and false at a move; the drop is guarded by a `Switch` on the flag;
3. on every `Call`/`Drop`/`Assert` that can unwind: a chain of cleanup blocks that drops the paths initialized at that point, in the same order, then `Resume`. Calls whose live-droppable set is empty get `Continue`. Chains are shared between calls with the same set (the cleanup block for a set is made once).

**Rules it implements (D4.2 step 3).**

- Moving a Go++ value copies the bytes and **zeroes the source** (`Move` operand elaborated to a copy plus `Assign(zero)`); drop glue skips `drop` on an all-zero value.
- Moving a C++ value runs its move constructor (`Call`), leaves it moved-from, and destroys it at scope end (the Go++ and C++ distinction is a flag on the type, from `Info`).
- Defer-extended locals (D4.6): the local lives in the defer record; the scope-end drop is *not* emitted for it; the function-scope cleanup drops it after the deferred calls.
- A swap of places evaluates every index, key and pointer operand (with its bounds and nil checks) before it moves anything (OWN-R4): the builder lowers a parallel assignment to "evaluate all operands into temporaries, then store". M0 already does this for `a, b = b, a+b`.
- Drops of temporaries happen at the end of the statement that made them (Go++ has no extended temporaries).

## 8. The ownership, borrow and concurrency checks (P04.09, P04.20, P04.21)

They run per function, after the builder and before drop elaboration, over **the GIR CFG**. A *point* is `(block, statement index)`; the terminator is the last point of the block. They read, besides GIR:

- `Info::Types`, `Uses` for borrow modes (`Var::Mode`: `Borrowed`, `Alias`, `MoveParam`), already decided by P03;
- the per-function summaries from export data (borrow summary, "writes this parameter", "callee keeps this parameter").

**What GIR must provide so they stay linear:**

- Every read and write of a variable is an operand or a destination **place**: a use of a move path is a point. No hidden uses.
- `AddrOf(place, mut)` is the only way to create a borrow, so each borrow has one creation point and a `mut` flag (D4.16 modes). `Call` operands that are references carry the borrow forward to the destination by the callee's borrow summary.
- Liveness of a borrow is the backward liveness of its destination local; a "change" of the borrowed place (an `Assign` to the place or a prefix, a `Move`, a `Drop`, a call that writes the parameter) at a point where the borrow is live is the error. Overlap is a path-prefix test with constant indexes and distinct fields known disjoint.
- `go` and `s.Go` are `Call` terminators to a runtime function whose closure operand records captures; the concurrency pass reads capture modes (moved, copied, shared) from the closure aggregate.
- Spans on every statement and terminator give the diagnostics their positions.

Each pass is one forward and one backward walk, linear in function size; each gets a generated stress test (the lessons in CLAUDE.md).

## 9. Verifier

`Verify.h`. It runs at the end of `lowerPackage` (an error is `err_gir_verify`, E0440, "internal error"), after every read of the bitstream (`readModule`), and at the start of `lowerToLLVM` (a module built by hand is checked too), so each test that makes GIR verifies it. It trusts nothing it is given and recomputes every type. Each rule is decided in one function of `Verify.cpp`, under its id. Every walk is iterative and the time is linear in the module: a place is a loop over its projections, the CFG a work list, the types a work list that looks at each type once, the struct-cycle check a three-colour DFS with its own stack (`tests/unit/GIR/StressTest.cpp`: 80000-block ladders, wide cleanup regions, thousands of functions; four times the input takes at most eight times as long).

An error is `VerifyError{Rule, Func, Block, Index, Message}` and prints as `V027: fn main.f: bb3 #2: duplicate switch case 1` (`#` is the statement index; the terminator has index `Stmts.size()`). `verify(M, MaxErrors)` stops at MaxErrors.

| Rule | What is checked |
|---|---|
| V001 | every function exists and has a name no other function has (the name is the symbol) |
| V002 | type shapes: integers 8/16/32/64 bits, floats 32/64, a struct the module uses has its body, no part is missing |
| V003 | a struct does not contain itself by value (through fields, arrays and tuples; a pointer, slice or string ends the walk) |
| V010 | the signature is a function type |
| V011 | local 0 is the return place of the result type, locals 1..N the parameters of the signature's types in order, every other local a variable or a temporary; every local has a type |
| V012 | a declaration (no blocks) has no locals |
| V013 | the entry block is not a cleanup block and no edge goes back to it |
| V020 | every block has a terminator |
| V021 | every edge, normal or unwind, names an existing block |
| V022 | every block is reachable from the entry (the builder removes the blocks after `return`, `break`, `continue`) |
| V023 | a normal edge stays on its side of the cleanup boundary; only a catch block leaves it (to a normal block), and only a catch block branches into a catch block |
| V024 | an unwind action names a cleanup block exactly when it is `Cleanup`; a catch block is also a cleanup block |
| V025 | a plain cleanup block does not return; `Resume` only in a cleanup or catch block; code in a plain cleanup block cannot unwind again (`Terminate` or `Unreachable`); a catch block is exempt from the first and last |
| V026 | `NoUnwind`: no `Resume` and no `Continue` in such a function; no cleanup edge on a direct call of one; `Unreachable` only on a direct call of one |
| V027 | switch case values fit the discriminant's type and are distinct |
| V028 | a call without a return target has a direct `NoReturn` callee; a drop and an assert have a target |
| V030 | a place names an existing local |
| V031 | the place's type is the type its local and projections give |
| V032 | a field, index or constant index applies to a type that has it (no downcast yet: GIR has no enum types) |
| V033 | every projection records the type its step gives |
| V034 | an index is an integer local |
| V035 | a deref is of a pointer |
| V036 | `StorageLive` / `StorageDead` name an existing local |
| V040 | constants: integer bits fit the width, floats/bools/nulls of their types, a string id in the table of type `string`, a function constant of its function's signature |
| V050 | `Dest = rvalue`: the rvalue's type is the destination's; `Use` gives its operand's type |
| V051 | unary: `neg` of an integer or float, `not` of a bool, `compl` of an integer; result type = operand type |
| V052 | binary: a shift takes two integers and gives the first one's type; otherwise one type: arithmetic on integers or floats, `rem` and the bit operators on integers, `eq`/`ne` on scalars and ordering on integers or floats, giving `bool` |
| V053 | a cast goes between the kinds of types its kind names |
| V054 | `AddrOf` is a pointer to the place's type |
| V055 | an aggregate builds a value of the type its kind names (a struct with its body), one operand of the right type per field or element |
| V056 | `Len` is of a string, slice or array and gives `u64` |
| V060 | a switch is on an integer or a bool; an assert on a bool |
| V061 | the callee is a value of a function type |
| V062 | the arguments match the parameters in number and type |
| V063 | the call's destination has the result type |
| V064 | a drop is of a place that holds a value (not unit) |
| V070 | a source position names a file of the module (or none); the printer and the debug info index the file table with it |

`tests/unit/GIR/VerifierTest.cpp` has both sides of every rule: the module that breaks it is rejected under that rule's id, and the nearby module that keeps it passes.

Not checked yet, added with the pass that creates the thing: that every `Move` source is initialized (P04.09 owns the dataflow) and `StorageLive`/`Dead` pairing (P04.09).

## 10. Monomorphization (P04.08, D4.3)

- **Generic bodies are built once, with `Param` types**, in the package that declares them, and stored in export data (section 13). An instance is made by *substitution* on the GIR body: every `Type*` in locals, places, rvalues and constants is mapped through the type arguments, and every `Call` of a function with the generic's own `FuncId` and type arguments is redirected to the instance. Method calls on a type parameter are the `Info::Operators`/`Selections` entries marked `ViaConstraint`; after substitution they name the concrete method (the same function the plain code would call), so plain and generic code share one resolver.
- **Instance key** = origin function + the type arguments as *canonical GIR type strings* + target. Two instances with the same key are the same function. The key's text is the mangled suffix P04.17 appends to the origin's symbol.
- **Fingerprint** (debug builds, one home per instance across packages, D4.3) = hash of the instance's substituted GIR bytes (`writeModule` of just that function, deterministic) + the fingerprints of every type and function it references + the profile settings that affect codegen + target.
- **Per-instance checks** (`Option[X]` of a nilable `X`, `call[error]`) run at substitution, with the checker's own predicates, because the checker saw only the generic body (D4.3).
- **Unit dictionaries (GEN-14, P04.24).** A generic whose `TypeParamType::uses()` contains `UnitDict` takes one hidden parameter per unit parameter: a pointer to a descriptor `{factor: f64/rational bits, dimensionless: bool}`. The `Info::UnitSites` of the body (kinds `Mul`, `Quo`, `Conv`, `Const`) become `Binary` operations whose factor operand is loaded from the dictionary; the "factor where the unit is dimensionless at the instance, 1 elsewhere" rule is evaluated at substitution when the instance's units are known, so an instance with static units has no dictionary parameter left and the factor is a constant.
- **Hierarchy type tables (P04.22).** `extends` types with `@Virtual` / `@Abstract` methods get a table global: `Struct("pkg.T.vtable")` of function pointers in method order, plus the parent-table pointer and a depth for downcast checks. A virtual call is `Call` with an indirect callee (a `Field` of a loaded table). The table is emitted per type (`linkonce_odr` in release). `base.M()` and every non-virtual call are direct (`Selection::Base`, `ParentSteps` give the field path to the parent part: `Field 0` repeated).

## 11. The checker's records and what lowers them

| `sema::Info` record | Lowering | Task |
|---|---|---|
| `Types`, `Val` | operand types, `Constant`s | M0 |
| `Defs`, `Uses` | the local of a `Var`, the callee of a `Func`, builtins | M0 |
| `Selections` (`FieldVal`) | `Field` / `Deref` projection chain: `Index` is the field path, `ParentSteps` extra `Field 0` steps, `Indirect` a `Deref` before the first step | P04.04 |
| `Selections` (`MethodVal`, `MethodExpr`, `Base`) | a `Call` of the method's function with the receiver as first argument (`AddrOf` for a pointer receiver); a method value is a closure aggregate (P04.07) | P04.06 |
| `Instances` | the instance key (section 10) of a generic call or type | P04.08 |
| `Bangs` | the failure edge of section 6 | P04.10 |
| `Operators` (`OperatorUse`) | the entry's `Call` is the call to lower: `a + b` is `Call(Method, a, b)` (`Swapped`: arguments in source order, receiver is `b`); `Negate` adds a `Unary(Not)`; `Sign` a `Binary` compare of the `<=>` result with 0; `Lazy` passes the right side as a non-escaping closure; `InPlace` takes `AddrOf(place, mut)`; `Builtin` `<=>` is two compares and a select; `Index`/`IndexSet`/`SetCall` evaluate `a` and the index once into temporaries | P04.23 |
| `UnitConvs` | a `Binary(Mul)` (floats: `fmul` by the factor rounded once to the type) or an integer `Mul` by a constant; `Truncate` an explicit `Cast`; constants are already folded by the checker | P04.24 |
| `UnitSites` | factor operands from the unit dictionary (section 10) | P04.24 |
| `UnitDisplays` | the display string of a boxed unit value in its type descriptor | P04.24 |
| hierarchy (`NamedType::HierParent` ..., `Func::MethodMark`) | the type tables of section 10 | P04.22 |
| `Var::Mode`, `LoopVar`, `OptionBinding`, borrow flags | read by the passes of section 8 | P04.09 / 20 / 21 |

Because the builder only follows these records, a rule changed in the checker changes the lowering in the one place that reads its record.

## 12. Printer

`gopp build -emit=gir` (alias `-dump-gir`) and `gopp gir dump file.girb` print the same text. The text is for people and FileCheck, the bitstream is the data. Example (hello, shortened):

```text
module "command-line-arguments"
file 0 "hello.gopp"

declare fn write(_1: i32, _2: *u8, _3: u64) -> i64 [c, external, nounwind]

fn main.main() -> () [go, external] {
  let _0: ()
  let _1: string
  bb0:
    _1 = const str#0 "hello\0A"  ; hello.gopp:7:2
    _2 = call const fn @write(const 1: i32, copy _1.0, copy _1.1) -> [return: bb1, unwind: unreachable]
  bb1:
    return  ; hello.gopp:8:1
}
```

A function line ends with `[go|c, external|internal|linkonce_odr, nounwind, noreturn]` (the last two when set).

Places print as `_1`, `_1.0`, `(*_1).2`, `_2[_3]`, `_2[0]`; blocks `bbN`, `bbN (cleanup)` or `bbN (catch)`; positions after `;` (`-gir-no-locs` leaves them out). Local and block numbering is the order of creation; dead blocks are removed and the rest renumbered in old order, so the numbering is stable for one input.

## 13. Serialization (D4.15)

`Serialize.h`. The container is **LLVM's bitstream** (`llvm/Bitstream`), used only as a container; `llvm-bcanalyzer` shows the block layout but cannot read the records.

```text
file      = magic "GIR" 0xB1 | IDENT block | MODULE block | index offset (u64 LE, 2 x 32 bits)
IDENT     (id 8)  : VERSION [chars]                 the Go++ version string
MODULE    (id 9)  : NAME [chars]
                    FILE [chars]            *        Module::Files in order
                    STRING [chars]          *        Module::Strings in order
                    TYPES block (id 10)
                    FUNC_DECL               *        one per function, in FuncId order
                    FUNC blocks (id 11)     *        one per function with a body
                    INDEX block (id 12)
TYPES     : one record per type, post-order (components before what they make
            up); then STRUCT_BODY records. Codes: UNIT, BOOL, INT[bits,signed],
            FLOAT[bits], STRING, PTR[t], SLICE[t], ARRAY[t,len], TUPLE[t...],
            FUNC[ret,fails,params...], STRUCT[name chars], STRUCT_BODY[id,n,fields...]
FUNC_DECL : [sig, link, cc, nounwind, noreturn, file, line, col, name chars...]
FUNC      : BODY[funcId]; LOCAL[kind,ty,mutable,span(3),name chars]*; then per
            block BLOCK[cleanup,catch], STMT[kind,span(3),payload]*, TERM[kind,span(3),payload]
INDEX     : BODY[funcId, bit offset low32, bit offset high32] *
```

- **Operands inside records** are flat `uint64` runs: a place is `local, nproj, (kind, val, type)*`; an operand is `kind, then (constKind, type, bits)` or a place; an rvalue is `kind, type, payload`. The reader recomputes nothing: a place's type is the last projection's type, or the local's.
- **Type ids** are assigned in the order the writer first meets a type walking the module (declarations, then bodies in order), components first (iteratively, so deep types cannot overflow the stack). Struct *bodies* are written after all types, so recursive structs are fine.
- **Index and jumping.** Every body is a block whose length is in its header. The trailer holds the bit offset of the INDEX block; the INDEX records hold the bit offset of each FUNC block. `ReadOptions::OnlyFunction` reads the tables and the declarations of all functions and the body of one (the others stay declarations): this is how P06 instantiates one generic or inlines one function without reading the whole file.
- **Version.** The IDENT block holds the compiler version; `readModule` fails with "rebuild" on any mismatch (the same compiler only, as in Go). There is no compatibility across versions, so records may change freely between releases.
- **Deterministic.** No pointer or hash order is written: the same module gives the same bytes (`GIRSerialize.RoundTripsAndIsDeterministic`, `tests/gir/roundtrip.test`).
- **Damaged input is an error, never a crash:** every id and enum value is range-checked, every record read is bounds-checked, a second body for one function is refused, and the module is verified after reading, so a source position that names no file (V070) never reaches the printer (`GIRSerialize.DamagedInputIsAnErrorNeverACrash` truncates and flips every byte, and prints each module that still verifies).
- **Text round trip.** D4.15 asks for binary to text to binary with identical bytes. The binary to text half exists (`gopp gir dump`, and the test requires its output to equal `-emit=gir`'s). A reader of the *text* form is not written: it is needed only for hand-written GIR tests, and P04.04 adds it if those tests want it. Until then the round trip is binary to module to binary (identical bytes) and binary to text equal to builder text.
- P06.02 puts these blocks into the `.gopx` sections; it needs a way to write several modules or one module with a part of its functions. `writeModule` takes a whole module; P06.02 may add an overload that writes a subset (functions by id). The format already allows it: the function records are self-contained apart from the module-level tables.

## 14. Debug information (D4.7)

Each statement and terminator carries a `Span` (file index, line, column). Codegen makes one `DISubprogram` per function and sets the `DILocation` of each instruction from the span of the statement it came from (P04.02). Locals with names become `DILocalVariable`s when P04.12 adds variable and type info; the `Local::Name` and `Local::Loc` fields already carry what it needs.

## 15. The M0 subset and what each task adds

Built today (`Lower.cpp`): package-level functions with bool, integer and float parameters and one result; `@Extern("C")` declarations; defined types over basic types (`type Celsius int`; a `type` declaration lowers nothing, the type is lowered where it is used); locals (`var`, `:=`, `=`, `op=`, `++`/`--`, parallel assignment of identifiers), `if`/`else`, three-clause and condition-only `for`, `break`, `continue`, `return`, expression statements that are calls; integer and float arithmetic, comparisons, `&&`, `||`, `!`, unary `-`, `^`, `+`; conversions between number types; constants; `println` and `print` of strings, integers and bools. Strings exist only as constants (cap 0), so M0 needs no drop glue. Anything else is E0439 at its first use (an unsupported type is reported once); `init` functions are among them (they run before `main.main` in `InitOrder`: P04.04).

- **`println` / `print`** (`Body::lowerPrintln`, D4.11 e: the bootstrap writes to standard output). Go evaluates every operand before the call, so the operands that are not constants are evaluated first, left to right, a variable into its own temporary; then the text is written. A constant operand is formatted when the program is built (an integer in decimal, a bool, a string's bytes), and the text from one value to the next, println's spaces and newline included, is one call of C `write`. Values call GIR helpers written once per module (`gopp.print_i64`, `gopp.print_u64`; a bool is a branch between two writes). Floats are not printed before P04.04 (Go's format needs the runtime's float printing).
- **`@Extern("C")`** (`Lowering::declareFunction`): the attribute takes the one string `"C"`, the function has no body, and its parameter and result types are C scalars or pointers (bool, integers, floats, pointers; P04.11 adds aggregates); the C name `main` is the entry point's. Anything else is `err_gir_extern_bad` (E0442) at the attribute, and calls of the function are not reported again. A program that declares `write` itself shares it with println when the type is println's (`func write(fd int32, p *byte, n uint) int`), and gets E0442 at the println otherwise.
- **The program.** `LowerOptions::Entry` (a program): package main must have `func main` (else `err_gir_no_main`, E0443, Go's wording, at the package clause), and the module gets the C entry point `main` that calls `main.main` and returns 0. A package that is not main is lowered without an entry (an object); `gopp build` refuses to link it into an executable.
- **Stack discipline (P03.26).** The builder recurses once per level of the checked AST; the parser bounds the height of a declaration (`ParseOptions::MaxHeight`, 1024). Measured in the sanitizer build, a level costs up to 7.1 KB (a left-deep `x + x + ...` or `b && b && ...` chain at the limit needs 7.1 MB, near the 8 MB of a main thread), so `lowerPackage` runs the builder on a thread with a 32 MB stack (only the touched pages use memory; without the thread it runs on the caller). Everything after the builder (the verifier, dead-block removal, the printer, the bitstream, the LLVM lowering) is iterative. `DeepChain.*ThroughGIRAndLLVMAtTheParserLimit` (`tests/unit/GIR/DeepTest.cpp`) nests every form the subset lowers as deep as the parser allows and lowers it through GIR and LLVM IR, also under ASan.

| Task | Extends |
|---|---|
| P04.04 | struct, array and pointer types and places (`Field`, `Index`, `Deref`); `&x`, `*p`, `unsafe`; `switch`; labelled `break`/`continue`/`goto`; package variables and init order (`Info`: `InitOrder`); `Option[T]`; bounds/nil/overflow `Assert`s with the runtime panic call; the text reader if wanted |
| P04.05 | `String`/`Slice` aggregates and places, `append`, `copy`, slicing, `View`, maps (runtime calls), `range` |
| P04.06 | methods (`Call` of the method function), interfaces (`Tuple(itab, data)`), itabs as globals, type switches |
| P04.07 | closures (aggregate of function pointer + environment), `defer` (catch-all cleanup block), panics (`Call` + unwind edges), `recover` |
| P04.08 | `Param` types, substitution, instance keys, `linkonce_odr` |
| P04.09 | drop elaboration, drop flags, move dataflow, the use-after-move and `defer`/`go` borrow checks |
| P04.10 | `!` lowering |
| P04.11 | C ABI lowering per `ABIArgInfo` at `C` calls and `@Export` thunks |
| P04.12 | DWARF variables and types |
| P04.17 | symbol names (`Function::Name`) |
| P04.18 | the `Go` calling convention, `(T, error)` in registers |
| P04.20/21 | borrow and concurrency checks |
| P04.22 | `extends` type tables |
| P04.23 | operator calls |
| P04.24 | units |

## 16. Tests

- `tests/gir/*.test` (`ninja check-gir`): FileCheck on the text. `hello`, `subset` (scopes, loops, short-circuit, division check), `println` (folding, evaluation order), `extern` and `extern-bad` (E0442, both sides), `no-main` (E0443), `unsupported` (E0439, once per construct), `roundtrip` (determinism, binary to text equals builder text), `stress-limits` (the deepest shapes the parser lets through, 20000-statement functions), `linear.long` (near-linear time for N and 4N, `check-long`).
- `tests/codegen/*.test` (`check-codegen`): LLVM IR, objects for both targets, line tables, the arm64 link against a stand-in C library, missing link inputs, link errors, `gopp lld`. `tests/run/*.test` (`check-run`): programs built and run with exact stdout and exit status (hello, Go semantics of D4.12, empty, long and non-ASCII strings, every println operand, evaluation order, exit codes, byte-identical objects and executables, the link flags, the Debian runs of P04.02).
- `tests/unit/GIR`: `VerifierTest.cpp` (both sides of every rule), `StressTest.cpp` (the verifier and the printer stay near-linear), `DeepTest.cpp` (stack discipline), `GIRTest.cpp` (types are uniqued; a hand-built module with every statement, rvalue and terminator, unwind and cleanup edges included, and mutations of it that each break one rule; bitstream round trip, determinism, selective reading, version refusal, damaged input; `lowerPackage` verifies every program it builds and reports what it cannot lower).
- Every path that makes GIR verifies it (section 9). A test that builds GIR needs no extra call.
