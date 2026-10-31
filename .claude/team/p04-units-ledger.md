# p04-units ledger (P04.24 units lowering)

Decisions, with the reason. The first three are language-visible or change the
shape of the design and need the manager.

## Needs the manager
1. **The dictionary is built as ADR-0025 Amendment says** (the first version of
   this branch, one instance per unit combination, was rejected). The unit
   arguments are no part of an instance (`requestFunc` takes them as the
   empty unit, in `gopp check` and the build alike); a generic with a collapse
   site takes one `*u8` per unit parameter that has `Use::UnitDict`. Limits,
   each recorded in TODO P04.24 with a both-sided Done-when:
   - **Factor width (R4, waits for the owner).** A descriptor's factor is
     `num/den`, two u64, reduced. The ADR says 4096 bits. A factor past 64
     bits is marked (`den = 0`) and only a site that needs the factor of a
     unit that is empty at the instance panics ("unit out of range"). Not
     changed in round 1.
   - **Closures, func values, methods, boxing.** E0439 each, decided from the
     generic-level type and `Use::UnitDict`; see TODO P04.24 (2) and (3).
   - **Slots.** A frame slot for a derived descriptor has room for every
     base unit the program declares (TODO P04.24 (5)).
2. **"unit out of range" message.** `panic: unit out of range: <unit as the
   generic code wrote it>` (the unit of the site that uses the factor, e.g.
   `U/V`, `U^3*V`), exit 2; the substituted unit is not shown (past the range
   it has no text).
3. **Type names.** `rtypeName` of a unit type is the best display form
   (`float64[KM/H]`); units of package main are bare, others `pkg.KM`. An
   instance of a generic type is `main.Vec2[KM]` in the type descriptor; the
   GIR struct and its DWARF name are `main.Vec2[unit]`.

## Review round 1 (R1 to R11)
- R1: the checker kept the raw value of a typed constant operand (`LT`,
  `int[KM](4)`) under the operand's node and never scaled it; `unitScale` now
  records the scaled value (an untyped operand already got it through
  `convertUntyped`). The lowering's `constOperand` is the one place that
  applies any further record, with one rounding (`scaleConstant`).
- R2: a body shared across unit arguments takes no unit fact from the
  instance's types: `physicalIntQuo` is off in it; the method refusal reads
  the generic-level receiver (`mentionsErasedUnit`) and `Use::UnitDict`.
- R3: boxing, asserting, type-switching a type with a unit parameter in such
  a body: one E0439 (P05.08), decided in `InstanceWalk::described`.
- R5: `unit_scale_int` (128-bit second product); plain code uses it for
  `p*q` past 62 bits (so the E0439 for that case is gone).
- R6: `Body::Hoist`; measured: the loop over a generic site is the plain loop
  at `-O2` (same time, 0.24 s for 3e8 divisions).
- R7: `bodyHasUnitSite` is gone; the method refusal is `Use::UnitDict` alone
  (so every method of `Vec` is refused when one uses the unit: the type's
  parameter carries the use, not the method).
- R8: every E0439 of the units names P04.24 (follow-up) or P05.08; the TODO
  items are in P04.24, P05.08, P06.06, P14.07, P04.26, P12.04.
- R9: `unit_ratio` rounds the exact quotient once to 24 or 53 bits.
- R10: descriptors are `{num, den, count, pairs}`: linear
  (`UnitDescriptorsFollowTheirUnitsNotTheProgram`, control
  `LowerOptions::DenseUnitDescriptors`). Self-describing, with no program-wide
  count in the helper names, so objects of different programs link.
- R11: `units-scale.test` compares `Twice` with `TwicePlain` line by line;
  `AddConstR` is checked; the generated table has negatives, int64, compound
  units, typed constants and methods.
- `r2.sh` and `r3.sh` expect a built program; the round-1 decision is an
  E0439 for both forms, so they fail on the build step by design (their
  first lines, the integer quotient, are right).

## Lowering
- Scale in the source kind (the checker's `UnitConv`); float: `fmul` by the
  factor rounded once, then a same-type cast so LLVM cannot fuse it into an
  FMA (rounded once). Integer factor: wrapping `mul`; ratio p/q: `x*p/q`
  truncating (`p==1`: a plain `div`; p*q past 62 bits: `unit_scale_int`);
  p or q past 64 bits is E0439 "convert to float first" (TODO P04.24 (1)).
  Complex: both parts.
- Generic integer quotient of two scales (plain code, method instances): the
  instance scales the coarser operand first (`physicalIntQuo`), as the
  checker does in plain code; the site factor is dropped then. In a shared
  body the same is decided at run time (`lowerBinary`).
- `case` values of a unit switch scale with the Tag factor; the AllConst jump
  table is off when a case scales.
- `x.In[U]()` lowers its receiver (the checker keys the conversion there).

## Debug info and GIR
- `Local::UnitName` (bitstream record `F_LocalUnit`, printer `[unit NAME]`,
  verifier V080: a Source local of integer/float/pair-of-float type). DWARF:
  a typedef over the number type. A unit field of a struct or element of a
  slice shows the number type (GIR struct fields carry no unit): in
  debug-info.md and TODO P14.07.

## Known gaps (not mine)
- Float `min`/`max` is E0439 in the lowering (pre-existing); the min/max tests
  are integer.
- Operator-method generics (`Sum[T interface{+(T) T}]`) hit P04.23's E0439.
- `fmt` metadata: P05.08/P13 (TODO). Imported generics: P06.06.
- `Vec2[U]` method returning `Vec2[U/S]` is E0366 (instantiation cycle) in the
  checker; the tests use a generic function.

## Review round 2 (R12 to R14)

- **R12** a method's own unit need (`Func::UnitDictRecv`, a bit for each
  receiver type parameter) is recorded in the checker: the method's sites and
  what it passes on to a generic function or method (not the naming of a type
  such as `Vec[U]`). `InstanceWalk::requestMethod` refuses by it, so `Sum`
  calling `Add` works once `Vec` has `UnitX`. A method that does need the unit
  and is called on a receiver with a unit parameter is still one E0439, with
  the same (now true) text: passing descriptors to methods is the P04.24 (2)
  follow-up.
- **R13** nothing panics at the entry: `unit_mulpow` marks an exponent out of
  range (count ~0, kept by every product), `unit_site` returns 0 / 0, and
  `scaleDyn` tests the denominator (one compare) and calls `panic_unit` with
  the site's text. TODO P04.24 (5) (entry-time panic) is gone.
- **R14** min and max of the checker check validity with `assignment(X, any)`
  without recording an interface conversion (`Checker::NoIfaceNote`). Audit:
  `panic`, explicit conversions and `==` of an interface with a value are real
  boxing; nothing else is a validity-only check.

## Review round 3 (R15, R16)

- **R15** a method reaches another method's unit need through any edge
  (call, pointer call, literal then call, method value, copy, function
  literal, branch, loop): `Checker::recordSelection` records every
  method-to-method edge (`MethodEdges`), and `untaggedCheck` settles
  `Func::UnitDictRecv` in one worklist after the uses. Safety net:
  `InstanceWalk::mentionsErasedUnit` also treats a method instance whose
  receiver unit is the empty unit as shared, so a missed edge is an E0439 at
  the call inside it, never a wrong value. Test: `tests/gir/units-method-chain.test`
  (`Inputs/gen-method-chain.py`). Method expressions (`Vec[U].M`) and `defer`
  are E0439 in codegen for other reasons and are not in the chain.
- **R16** the site panic text is the display form of the site's expression
  (`U/KM`), units of main bare (`Body::siteText`).

## Review round 5 (R17, R18)

- **R18** `InstanceWalk::requestMethod` decides the refusal of a needy method
  called from a shared body on every request, before the `ByMethod` / symbol
  caches, so plain code that made the same instance first (`W[U*IKM]` at
  `U = KM`: the empty unit, factor 1, the same key and symbol as the erased
  `W[U]`) cannot bypass it. A method that needs no unit is one instance for
  both, which is correct. Deviation: the coordinator asked for a distinct
  key and symbol for erased instances. A distinct erased unit by value is not
  possible (a factor other than 1 gives powers past the 4096-bit range, E0435
  on `U^100`; a marker dimension is lost by `U/V`), and a distinct identity by
  provenance needs the lowering to key instances by (method, shared) instead
  of by the checker's `Func`; not done, recorded below.
  Test: `tests/gir/units-erased-alias.test`.


## Round 6 takeover (R19, R20: the erased-unit class at its root)

R2, R3, R12, R15, R17 and R18 each guarded one route by which the empty unit
of a body shared across unit arguments (call it e) leaves it. R19 (a generic
over a type, `F[T Aer]`, calling `x.A()` with `T = W[U]`) and R20
(`Box[T Copy](x T) any` with `T = float64[U]`) were two more. The class, the
routes and the one mechanism are in `gir.md` section 10 ("Units a shared body
passes on").

- **Routes listed** (gir.md): unit type arguments; type arguments that mention
  a unit parameter (`float64[U]`, `W[U]`, `[]W[U]`, `map[K]float64[U]`,
  `func(float64[U])`, `Pair[W[U], int]`, a type parameter that carries one);
  method receivers (the instance, a pointer, a literal, a type parameter, a
  field or element, a method of a generic type with such an argument,
  promoted methods); method values and expressions; func values of generic
  functions; composite literals, conversions, assignments; instances of
  generic types and structs with such fields or elements; operator methods;
  function literals.
- **Mechanism: provenance.** An instance asked for from such a body with a
  type (or receiver) argument that mentions one of its unknown units is a
  shared instance: `GenericInst::Shared` (bit k for type parameter k; every
  bit for a receiver that is not the instance itself), `Erased` (those, and
  a function's unit parameters), `FromCaller`, `Origin`. `Shared` is in the
  key (`ByKey` for methods, the symbol for both) and in the symbol (new
  appendix A kind `Tu` SharedUnits, demangled `main.Id[main.W[1]] (shared
  units 1)`; `SymbolName.TheAppendixIsTheCode`, corpus, fuzz seed-36).
- **One predicate**, `InstanceWalk::mentionsErasedUnit` (on
  `MethodSets::mentionsTypeParamIf`, memoized per instance): `described`, the
  needy-method refusal (`UnitDictRecv & Shared`, so a type-parameter receiver
  is covered), every request (makes `Shared`), and in the lowering
  (`Body::erasedUnit`) the sites of a non-dict instance (constants too),
  `physicalIntQuo`, `unitDesc`, the debugger's unit name.
- **One E0439 per refusal**: own unit of the body at the decision; a unit a
  caller shared at `Origin`, the call in the outermost shared body, once
  (`Said` keyed by origin), with a note at the decision; the instance is
  not walked further.
- **Removed:** `GenericInst::SharedUnits`, `Rescan` (R17) and
  `requestMethodImpl` with its refusal-before-cache order (R18): the key
  makes them unnecessary. The recursive, depth-capped (64)
  `mentionsErasedUnit` (it answered "not erased" past depth 64).
- **Found and fixed on the way (pre-existing, V062 internal error from valid
  source):** a value of `Box[float64[KM]]`, `Pair[W[KM], int]` or
  `struct{ f float64[KM] }` passed to a shared body whose parameter is the
  erased type: two GIR structs. `Lowering::unitFree` keys GIR types by the
  symbol node with units erased (replaces `UnitErasedTypes`); such instances
  are named with units erased (`main.Box[float64]`), recorded in
  debug-info.md and TODO P14.07 with a both-sided Done-when.
- **Also:** `units-method-chain.long.test` lacked `REQUIRES: long` and ran in
  the normal suite; fixed. Function instances are found by symbol node
  (`ByNode`) before their text is written.
- **Not mine, seen:** the checker cannot infer `V` from `Box[float64[KM]]`
  against `Box[float64[V]]` (E0316; `Box[W[V]]` infers). The generator writes
  the unit argument there.
- **Tests:** `tests/gir/units-shared-routes.test` (48 cases, 24 programs) and
  `.long.test` (8844 cases, 8040 refused, 4316 programs), generator
  `Inputs/gen-shared-units.py`: chains of depth 0 to 3 of 8 type-argument
  routes plus 3 unit-generic layers (`unit`, `ubox`, `ustruct`), 7 leaves
  (needy method, its method value, a method that needs no unit, assertion,
  type switch, interface ==, map[any] key), carriers `W[U]` and `float64[U]`,
  roots plain (KM), a real instance with the empty unit of factor 1 (the
  twin), and shared; both orders of twin and shared, `-build-check-j` 1 and
  16; each case is plain code's values or exactly one E0439 at the computed
  line. Before the fix: 3288 of 3536 programs fail (built, no E0439).
  `GIRInstances.UnitsASharedBodyDoesNotKnowAreFoundOncePerInstance` (the
  predicate and `unitFree`, counted, with the `ForgetErasedUnits` control).
- TODO P04.24 (6) removed (done: own key, own symbol, both orders); (2) and
  (3) name the routes.

Round 7 (reviewer, Mode 2): R19, R20 CONFIRMED-FIXED (Opus takeover, provenance + `Tu`); R21 (inference gap, pre-existing) recorded by the manager as TODO P03.33. CLEAN apart from R4 (factor width), which waits for the owner and stays recorded as TODO P04.24 (1).
