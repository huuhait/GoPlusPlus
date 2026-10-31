# The internal calling convention (P04.18, D4.4)

Go++ calling Go++ uses its own convention, decided in one place: `goabi::GoAbi` (`CodeGen/GoABI.h`), next to `CABILower` (the C ABI, `docs/design/c-abi.md`). The C ABI is Clang's decision and Go++ lowers it; here Go++ decides, so the whole decision is one class and a call, a callee, a thunk and a test all ask it.

## Three decisions

1. **How a parameter is handed over** (`Lowering::passOf`, GIR level, from the signature alone). It changes the GIR type of the parameter (`Lowering::paramType`, used by declarations, method signatures and itab slots), so a call and a declaration cannot disagree, and a `Func` type (a call through a pointer, P04.06's itab slots, P04.07's func values) carries it in its parameter types.
2. **Which convention a call uses** (GIR level, from the type). The convention is part of a `Func` type (`CallConv`, printed `fn "C"(...)` for C), and a function's own convention is its signature's (V075). A direct call takes it from the callee, a call through a pointer from the pointer's type.
3. **How the values travel** (`GoAbi::classify`, LLVM level, from the LLVM memory types). Registers or memory.

### 1. Pass modes (GIR, PAR-10, D4.4)

| Parameter | Mode | GIR type of the parameter |
|---|---|---|
| a `move` parameter; a type that is not owning (a copy: scalars, pointers, views, structs and arrays of them; a shared handle, passed without a retain) | Value | the type itself; the callee owns its copy (`p.X = 1` leaves the caller's value) |
| a borrowed string or `[]T` | View | P04.25's pair `(ptr, len)` (`Lowering::sliceBorrowed` for `[]T`, ADR-0029 point 7); the callee builds its variable from it with capacity = length and a null allocator word (SLC-12), capacity 0 for a string |
| a borrowed map, or a borrowed struct, array or Option that holds a string, slice or map by value | Place | a pointer to the caller's own place; the callee's `Body::Aliases` maps the name to it, so `d.Index["x"] = 1` on a nil `Index` creates the map in the caller's `Doc` (PAR-1) |

A method's receiver (P04.06) is parameter 0 and follows the same rule (`Lowering::passOfRecv`, review R9; PAR-1 says "a parameter or receiver"): a pointer receiver and a `move` receiver are values, a borrowed value receiver is what a borrowed parameter of its type is, in the declaration, the body, a static call and an itab thunk. A static call on a pointer (`p.Put()`, `x.(*Doc).Put()` with a value method) passes the pointer; otherwise the table below decides. Through an interface, both sides of TYP-5: a pointer the interface holds (`&e`) is passed as it is, so a map the method makes is made in the pointed-to value (PAR-1); a value the interface holds, in its box or its data word, is immutable all the way down (TYP-5), so the thunk passes the address of its own copy, the map the method makes is dropped with it and the box stays as it was, as with Go's value receiver (`gir/recv-thunk.test`). So methods of map types lower too. The results of a call passed on to a variadic function, `vf(g())`, bind the fixed parameters and fill the slice with the rest. The other owning types, a `Unique` pointer (P05) and a type with a `drop` method, cannot be lowered yet; TODO P04.09 item (7) makes a borrowed one a Place too. A C signature has no pass modes: an `@Extern` function and the Go++ function of an `@Export` one take every parameter by value (C types hold no string, slice or map, so `passOf` gives Value for them today; item (7) must keep it so).

`holdsOwned(type)` is memoized per type and iterative (it counts `LowerSteps`; `GIRStress.ThePassMode*` on chains, diamonds, wide structs and many parameters).

**Where a Place receiver or argument points** (reviews R9 to R11). One function decides, for receivers (`lowerMethodCall`) and arguments (`callWith`) alike: `Body::placePath` walks the expression from itself toward its root (a field or an array element held by value is no step: the walk goes on into what holds it), the first step that leaves the value's storage decides, and `Body::placeHow` turns that into how it is passed. An address is taken after the other arguments (the P04.05 "late reads" pattern: it may point into a container another argument grows). `tests/run/cc-place-paths.test` runs every row as a receiver and as an argument, both sides, on both targets (`Inputs/gen-place-paths.py`).

| The value is in | Examples | Passed as |
|---|---|---|
| a variable: local, parameter, package variable (a borrowed one is its owner's place); a checked Option, which "simply is a `T`" (OWN-P10, review R12): its payload, also through a parameter | `d`, `w.D`, `a[1]`, `gd`, `o` in `if o != nil { put(o) }` | its address: PAR-1 reaches it |
| a pointer's target | `*p`, `p.D` with `p *W`, `x.(*Doc).F`, `*x.(*Doc)` | its address |
| a slice's buffer, also of a slice read from a map entry (R10) | `s[1]`, `ms["k"][1]`, `mw["k"].Ds[0]`, `ss[0][0]` | its address |
| an interface's box (TYP-5, TYP-9, R11): the value an assertion gives, an element of a slice or array held in it, a map held in it | `x.(Doc)`, `q.([2]Doc)[1]`, `b.([]Doc)[0]`, `y.(WS).Ds[0]`, `y.(Tab)` | the address of a copy: the box stays as it was, as in the itab thunk |
| a view (OWN-V1) | a field or element of a `View` | the address of a copy |
| a map entry of a map type, the index itself | `mt["k"]` (a `Tab`) | copied in and out around the call (the P04.05 map-entry record) |
| a map entry, or a field or array element of one, that holds a map | `md["k"]`, `mh["k"].D`, `ma["k"][1]`, `mh["k"].T` | refused: one E0439 naming P04.09 (TODO P04.09 item 6 copies it in and out); a copy would drop the map the callee makes |
| a map entry that holds no map | `mn["k"]` (a struct of a string) | the address of a copy: nothing the callee may change is lost |
| no place: a call result, a literal, a conversion, a constant; an argument that an implicit conversion makes a new value of: an Option made from a value (`Doc` to `Option[Doc]`), a view, an interface | `mk()`, `Doc{}`, `d` in `toOpt(d)` | the address of a copy, its own temporary: what the callee makes is dropped with it |

An argument the checker assigns to its parameter as it is although the types differ (`Checker::assignableAsIs`, recorded in `Info::AsIsArgs`: identical underlying types where one of them is unnamed, as `struct{...}` into a struct type or `[2]Doc` into `type Pair [2]Doc`; an alias is the type itself) is no new value: the row of where it is decides, and its address is cast to the parameter's type (review R14). A type-switch or assertion binding of an owning type is a `View` (TYP-9), never a Place receiver or argument.

### 2. The convention of a call (GIR)

| Callee | Convention |
|---|---|
| a Go++ function, or a pointer of a Go++ `Func` type (an itab slot: the method's signature with the data word first, `methodSig`; a type descriptor's and a map's hash and `==` glue) | Go++ (`swiftcc`, below) |
| a C function (`@Extern("C")`, libc, a thunk, the entry point `main`) | the exact C ABI (`cabi::lowerCArgs`, `docs/design/c-abi.md`) |
| a pointer of a C `Func` type (an allocator's table entry, ADR-0029: an allocator written in C or C++ fits) | the C ABI of its scalars (`cabi::classifyScalars`; V076: scalars only, aggregates are P04.07's) |

The counting allocator of the test intrinsics is a C function with a GIR body, so its table entries are C functions like any other allocator's. Methods, itab thunks (`_GPTI...`) and the glue in type descriptors are Go++ functions. After lowering, `gopp::checkCalls` checks the module: a direct call uses its callee's LLVM convention (an internal error otherwise: swiftcc and ccc put a result in other registers), and every function in static data (itabs, method tables, descriptors: called through pointers of Go++ function types) has the Go++ convention of the target (`GoAbiCalls.*`, both sides).

### 3. Registers (LLVM)

Every Go++ function is `swiftcc` on both targets (`goabi::Policy::CC`, per target so that a later target can differ; ADR-0030), because the C convention returns at most two values in registers:

- **amd64:** 4 integer values (rax, rdx, rcx, r8) and 4 floats (xmm0-3). A fifth integer makes the result a hidden pointer; a fifth double lands in an x87 register.
- **arm64:** 8 + 8 (x0-x7, v0-v7).

`GoABITest.ReturnRegistersAreWhatLLVMHas` measures both budgets from LLVM (assembly with and without a hidden result pointer, one value past the budget as the control); a newer LLVM that changes them fails it. A closure's environment (P04.07) is `swiftself`, a register that is no argument's, on both targets.

**The amd64 limit (ADR-0030).** A result of more than four integer-class or four float-class scalars goes through a hidden first pointer on amd64: `(string, error)` and `([]T, error)` do. Measured against `x86_regcallcc`, which returns them in registers: no runtime difference (307 vs 303 ms over 400M calls), while regcall made Go++ calls into C 16% slower, `.text` 6-11% larger and FastISel fall back on every call, and needed a workaround on every unwinding call. A P15 benchmark decides whether a custom convention (option C) is worth it.

swiftcc keeps across a call the registers C keeps (amd64: rbx, rbp, r12-r15, no XMM register; arm64: x19-x28, fp, lr, d8-d15), and the unwinder restores them as it does for C, so an `invoke` (P04.07) needs nothing more. `Lowering::emitCall` is the only place that makes a call (P04.07 makes it an `invoke` there).

The rules read the LLVM memory type of the GIR type, so they are layout-blind: a four-word slice (ADR-0029) changes nothing here.

| Value | Kind | IR |
|---|---|---|
| unit, or an aggregate with no scalar | Ignore | nothing |
| a scalar (bool, integer, float, pointer, func) | Scalar | one argument; narrow integers get `signext` / `zeroext` |
| an aggregate of at most 4 scalars (an array of length 1 flattens, longer does not) | Direct | one argument per scalar, in order |
| any other aggregate | Indirect | a pointer to the callee's own copy |
| a result whose scalars fit the budget | Direct | the scalar, or a literal struct of them |
| any other result | Indirect | a hidden first pointer, written by the callee |

So `(T, error)` (a tuple; the interface is two pointers, D4.5) comes back in registers while T has at most two integer-class scalars on amd64 and six on arm64: `(int, error)` in rax, rdx, rcx and x0, x1, x2 (`tests/codegen/go-abi.test`, also at `-opt=1` with no store), while `(string, error)` (five words: a Go++ string is three) and `([]T, error)` (six) go through memory on amd64 and stay in registers on arm64 (`go-abi-error.test` with the real `error`). Five integers stay in registers on arm64 and not on amd64, nine and twelve on neither; a `move []T` (four words) travels as four scalars both ways. The flatten walk is iterative, memoized per type, and counts its work (`GoAbiWork`).

The four lowering functions mirror the C ones and share their `Env` and `Value`: `lowerArgs` / `lowerResult` (caller), `unpackParams` / `packReturn` (callee).

## C-ABI thunks

A thunk is a GIR function with the `C` convention whose body calls a Go++ function. LowerToLLVM classifies C functions with bodies too; their prologue is `cabi::unpackCParams` and their return is `cabi::packCReturn`, and the call to the Go++ function is `lowerArgs` / `lowerResult`.

- `@Export("C") func f(...)`: the symbol `f` is the thunk (external); the Go++ function keeps its mangled name and calls from Go++ go to it. The C name must not start with `_GP` (`sym::isReservedName` alone, appendix A.8, SYM-4): E0445 names the reserved prefix; `GPx`, `_G`, `_Gx` are fine (`tests/run/export-link.test` links them). Also E0445: another ABI than "C", no argument, no body, `@Extern` and `@Export` together, a type C cannot take, the name `main` (the entry point) or one another function has. The reserved prefix on an `@Extern` name is E0442.
- A callback: a func-typed parameter of an `@Extern` function is a C function pointer (`fn "C"`). A top-level Go++ function passed there is wrapped in a thunk `sym::wrapper(CABIThunk, ...)` (`_GPTC...`), made once per function and linkonce_odr like the glue, so every package gives C the same pointer; a C function is passed as itself. Passing a closure, a method value or a variable is E0439 naming P04.07, as is a func result or field, and a func parameter or result of an `@Export` function.
- Verifier: V067 (C functions take C types, bodies too; a callback type has the C convention), V075 (a function's convention is its signature's), V076 (a call through a C function pointer passes scalars).

## How later tasks plug in

| Task | Where |
|---|---|
| P04.06 itab calls | the itab's entries are Go++ `Func` types made with `passOf` for each parameter; `classify` the method's signature; the receiver is the first parameter (the itab's `data`; the thunk passes it as `passOfRecv` says); an interface value is `(ptr, ptr)`, Direct |
| P04.07 closures, unwinding | `classify(Sig, HasEnv = true)` puts the environment in `swiftself`; `lowerArgs(..., EnvPtr)`. Whether a call unwinds is the call's business (invoke), not the convention's. A C function value (`fn "C"`) with aggregates is called through `CABILower` (V076 refuses it until then) |
| P04.08 generic instances | `classify` the instantiated signature; `passOf` runs on it |
| P04.10 `(T, error)` | done: the tuple GIR makes; Direct while it fits; `f!()` tests the itab scalar of the result with no store first (`tests/codegen/bang-cold.test`, both returns are shapes `tests/run/bang-*.test` run on both targets) |
| P12 goroutine entry | a thunk whose signature is `func(ptr)` built with the same four functions |
| P04.09 drops | decided by the checker's borrow of the parameter, not by its mode: the callee drops its `move` parameters only (a Place or View is the caller's; until item (7) a borrowed `drop` type is a bitwise copy the callee must not drop) |

## Known limits

A map entry may move when the callee grows the map, so no pointer into it is held across the call (the P04.05 pattern): a value that holds a map passed from a map entry (other than a map entry of a map type, which is copied in and out) is refused with one E0439 naming P04.09 (the table above), and TODO P04.09 item (6) gives it the copy-in and copy-out of a map-entry argument. A slice element passed as a Place is its address, and a callee that moves that slice's buffer invalidates it: the borrow checker must reject that call (TODO P04.20 item 6, the same as for a borrowed map).

A Place parameter is the caller's variable itself, so a callee that writes that variable another way during the call sees the write in its parameter, where Go's copy would not: `viaPtr(x, &x)` (the callee writes through `p`) and `viaGlobal(g)` (the callee writes the global `g`). Both calls break OWN-B4 / OWN-B9, and the borrow checker must reject them (TODO P04.20 item 8); until it does, the checker accepts them and the program sees the write (`tests/run/cc-params.test`, `Inputs/cc/alias.gopp`, pins it). An argument that a conversion makes (`Doc` passed to an `Option[Doc]` parameter, also a result of `f(g())`) is a new value, a temporary, never the variable.

## Tests

- `tests/codegen/go-abi.test` (IR and assembly, both targets, `(T, error)` shapes), `go-abi-error.test` (the real `error`, a method, an itab call and its thunk, both targets), `tests/gir/params.test` (the three pass modes), `tests/gir/callback.test`, `callback-bad.test`, `export.test`, `export-bad.test`, `export-main.test`, `extern-unsupported.test`, `allocword-view-fold.test` (the table call stays the C ABI).
- `tests/run/cc-params.test` (PAR-1 through fields, calls, array and slice elements, pointers and temporaries, the trivial copy, borrowed `cap`, PAR-2; results of many shapes and `(T, error)` with the real `error`, directly and through an itab, against `go run`), on arm64 in `cc-calls-arm64.test`; `run/core-variadic.test` (results spread over a variadic parameter); `Inputs/cc/recv.gopp` in both (receivers: PAR-1 directly, through a pointer and through an interface holding a pointer; the copy through an interface holding a value, TYP-5; trivial and view receivers, map receivers), `gir/recv-map-entry.test` (the E0439 of a map-holding receiver in a map entry, both sides), `gir/recv-thunk.test` (the thunk copies a boxed value or data word, passes a held pointer as it is), `run/cc-place-paths.test` (the table of where a Place receiver or argument points, generated, both sides, both targets).
- `tests/run/cc-calls.test`, `cc-calls-arm64.test`, `cc.long.test`, `cc-arm64.long.test`: programs of Go++ to Go++ calls made by `Inputs/gen-cc.py` (up to 24 parameters of scalars and structs, results of one to five values, forwarding, recursion, trivial, borrowed and `move` parameters), whose output Go agrees with (`Inputs/check-cc-go.py`, needs Go; the tests never run it); `run-cc.py` builds the long seeds at every `opt` level; `cc/calls0` is in `core-opt-matrix.test` and every program of `Inputs/cc` in the long opt matrix.
- `cabi-reverse-*.test` (C calls Go++ through `@Export` and callbacks, generated, byte for byte, both targets, every edge signature in the long test), `core-opt-matrix-cabi.test` (a reverse program at every `opt` level), `export-link.test` (`GPx`, `_G` link, both targets).
- `tests/unit/CodeGen/GoABITest.cpp` (kinds, the register budget measured, one decision per signature, linear work), `StressTest.cpp` (`ThePassMode*`), `VerifierTest.cpp` (V067, V075, V076), `CodeGenTest.cpp`, `GoABITest.cpp` `GoAbiCalls.*` (checkCalls, both sides).
