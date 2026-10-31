# Appendix A. Symbol names

Plan decision D4.13, task P04.17. Code: the library `compiler/lib/Mangle` (above Sema, below GIR and CodeGen): `gopp/Mangle/SymbolName.h` (the mangler, the parser and the printer), `gopp/Mangle/SemaSymbols.h` (the names of the checker's types and objects), and `gopp demangle` (the command). This appendix is not part of the language: a program cannot see a symbol name. It is the contract between the code generator, the linker, the build cache and the tools that read object files, traces and profiles.

## A.1 What a name guarantees

**SYM-1.** *From content.* A name is a function of the *content* of the thing it names: package paths, declared names, type arguments, canonical units. It never depends on creation order, on a pointer, on a thread, on the order the packages were checked in, or on how many threads checked them. The same entity has the same name in every package, every build and every machine.

**SYM-2.** *One type, one name.* Two types have the same name exactly when they are identical types (A.6): a spelling the identity ignores (`byte` for `uint8`, `func!(int) T` for `func(int) (T, error)` ([ERR-2](09-errors-and-panics.md#91-functions-that-can-fail-d18)), `KM*H^-1` for `KM/H`) gives the same name, and every part the identity compares gives another one. Every declared function and method has its own name, operator overloads too (A.5.1). A generic instance (`Map[int, string]`, `Box[float64[KM/H]]`) has one name wherever it is used, so every package refers to the one home of the instance (D4.3), and `linkonce_odr` copies fold at link time.

**SYM-3.** *Readable.* `gopp demangle` turns every name back into Go++ text (A.9); for a type it is the type printer's text, so a name in a trace reads like the type in a compile error. The name itself is also legible: `_GPF4mainE3Foo` is the function `Foo` of package `main`.

**SYM-4.** *No collision with C.* Every name starts with `_GP` (A.2), and a name that C code can see (`@Export`, a C-ABI name) must not (A.8).

**SYM-5.** *Reversible and canonical.* `demangle(mangle(x))` is `x`, and the demangler accepts exactly the text the mangler writes: `mangle(parse(S)) == S` for every accepted `S`, so two different entities never share a name and one entity never has two.

**SYM-6.** *Total.* The mangler has no length limit and no failure. The demangler refuses malformed text with a message and an offset, never with a crash, and its walks are iterative, so a name a million levels deep is fine.

## A.2 Characters and the prefix

A name is `_GP` followed by an *entity* (A.5). The characters are `A-Z a-z 0-9 _ $` only, which every object format and assembler accepts. `_GP` is the version: a future format change uses another prefix (`_GQ`, ...), so old and new names never mix.

`_` followed by a capital letter is in the namespace that C reserves for the implementation, so no C program declares `_GP...`. The other manglings in the same objects start differently: C++ `_Z`, Rust `_R` and `_ZN`, Swift `$s`. `sym::isReservedName(name)` says whether a name has the prefix.

## A.3 Identifiers, paths and numbers

An **identifier** (a declared name, a package segment, a struct tag) is its length in decimal, then that many characters of the escaped text:

| Case | Written as |
|---|---|
| a letter or `_` | itself |
| a digit, not first | itself |
| any other code point (a digit first, a non-ASCII letter, an operator spelling, `$`, a NUL, ...) | `$` + the code point in lowercase hex, no leading zeros + `$` |
| a byte that is not part of well-formed UTF-8 | `$x` + two lowercase hex digits + `$` |
| the empty identifier | `0_` |

The length counts the escaped characters, and the escaped text never starts with a digit, so `3Foo`, `4$2b$` (the operator `+`) and `18$65e5$$672c$$8a9e$` (日本語) read without ambiguity.

A **package path** is its segments (the text between `/`) as identifiers, then `E`: `std/strings` is `3std7stringsE`, `github.com/x/y` is `13github$2e$com1x1yE`, the empty path is `0_E`. A **number** is decimal digits, no leading zero (except `0`), then `_`: `12_`. A **signed number** has an `n` first for a negative one: `n3_`.

## A.4 Grammar

```
symbol  = "_GP" entity .
entity  = Func | Method | OpMethod | Var | Closure | PkgInit | InitFunc
        | TypeInfo | VTable | ITab | EqGlue | HashGlue | DropGlue | CloneGlue
        | Data | CABIThunk | MethodValue | MethodExpr | Promoted | IfaceThunk
        | IfacePtrThunk | MethodTable | IfaceDesc | Instance(entity base) .
type    = Basic | Pointer | Slice | Array | Map | Option | View | Borrowed | Chan
        | Tuple | Signature | Struct | Interface | Named | LocalNamed | Universe
        | Instance(type base) | TypeParam | Unit | UnitType | BackRef .
```

Every node is a **tag** (one letter, or `T` and a letter), a header, and children. The tag table is the whole grammar; it is *prefix-free* (no tag begins another), so a name is read left to right with no look-ahead. A node with a fixed number of children has no terminator; a node with a list of children ends with `E`. The letters `E` (end), `B` (back reference) and `I` (instance) are used by no other node.

| Tag | Kind | Header | Children |
|---|---|---|---|
| `b` | Bool | | |
| `i` | Int | | |
| `a` | Int8 | | |
| `s` | Int16 | | |
| `l` | Int32 | | |
| `x` | Int64 | | |
| `j` | Uint | | |
| `h` | Uint8 | | |
| `t` | Uint16 | | |
| `m` | Uint32 | | |
| `y` | Uint64 | | |
| `z` | Uintptr | | |
| `f` | Float32 | | |
| `d` | Float64 | | |
| `c` | Complex64 | | |
| `e` | Complex128 | | |
| `r` | String | | |
| `p` | UnsafePointer | | |
| `P` | Pointer | | type |
| `S` | Slice | | type |
| `A` | Array | length (number) | type |
| `H` | Map | | key, element |
| `O` | Option | | type |
| `V` | View | | type |
| `W` | Borrowed | | type |
| `X` | Chan | `b` both, `s` send, `r` receive | type |
| `L` | Tuple | | types, `E` |
| `G` | Signature | flags (number: 1 variadic), move mask (number) | parameter Tuple, result Tuple |
| `Z` | Struct | 1 if `@Strict` else 0 (number) | Fields, `E` |
| `q` | Field | flags (number), [package path], name, [tag] | type |
| `Y` | Interface | | IMethods sorted by package, name; `E` |
| `n` | IMethod | `0`, or `1` and a package path (unexported name); name | Signature |
| `N` | Named | package path, name | |
| `g` | LocalNamed | ordinal (number), name | the declaring entity |
| `u` | Universe | name (`error`, `Unique`, ...) | |
| `I` | Instance | | the generic thing, type arguments (at least one), `E` |
| `Q` | TypeParam | index (number) | |
| `k` | Unit | numerator `_` denominator `_` (decimal; `n` before a negative numerator) | Dims sorted, `E` |
| `U` | UnitType | the numeric type letter | Unit |
| `o` | Dim | package path, unit name, exponent (signed number) | |
| `w` | ParamDim | index (number), exponent (signed number) | |
| `B` | BackRef | offset (number) | |
| `F` | Func | package path, name | |
| `M` | Method | name | receiver named type |
| `To` | OpMethod | name (`op_`, or `opr_` for `@Right`, and a mnemonic: A.5.1) | receiver named type, parameter Tuple |
| `R` | Var | package path, name | |
| `C` | Closure | ordinal (number) | enclosing entity |
| `J` | PkgInit | package path | |
| `K` | InitFunc | package path, ordinal (number) | |
| `Ti` | TypeInfo | | type |
| `Tv` | VTable | | type |
| `Ta` | ITab | | concrete type, interface type |
| `Te` | EqGlue | | type |
| `Th` | HashGlue | | type |
| `Td` | DropGlue | | type |
| `Tc` | CloneGlue | | type |
| `TD` | Data | key (identifier) | |
| `TC` | CABIThunk | | entity |
| `TV` | MethodValue | | entity |
| `TE` | MethodExpr | | entity |
| `TP` | Promoted | | entity |
| `TI` | IfaceThunk | | entity |
| `Tm` | MethodTable | | type |
| `Tn` | IfaceDesc | | interface type |
| `Tp` | IfacePtrThunk | | method |

Field flags, every mark of a field that type identity compares ([TYP-15](03-types.md#34-required-fields-d14-rule-11), [TYP-15a](03-types.md#34-required-fields-d14-rule-11)): 1 embedded, 2 the name is unexported so its package follows, 4 a tag follows, 8 the effective `@Required` bit, 16 `@Option`, 32 the hidden parent part of an `extends` type (ADR-0023), 64 `@Required` as written, 128 required in the "must be set" sense only (the field of an instance declared with a type parameter, ADR-0016 point 8). Signature flags: 1 variadic; the move mask has bit *i* set for a `move` parameter. There is no flag for `!`: `func!(P) R` is identical to `func(P) (R, error)` (ERR-2) and is written as that. Which child a node may have is fixed (a Method's child is a named type, an Instance's first child is a named type, function, method or operator method, an IfacePtrThunk's child is a method or operator method, or an instance of one, ...); the parser rejects any other.

## A.5 What each entity is

- **Func** `F`: a package-level function. **Var** `R`: a package-level variable (not a blank one, `var _ = f()`, which has no storage).
- **Method** `M`: a method of the named type (or instance) in its child. The receiver kind (value or pointer) is not part of the name: a method belongs to the type. Promoted methods of `extends` and embedded types are named by their **Promoted** wrapper (`TP`) around the method of the child type.
- **OpMethod** `To`: an operator method (ADR-0024). Its name is `op_` and a mnemonic from the fixed table in A.5.1, and **the parameter types are part of the name**, because several methods with one operator name and different parameters form an overload set (`Vec2 * float64`, `Vec2 * Vec2`); unary and binary `-` differ by the number of parameters. **`@Right` is part of the name too** (`opr_` in place of `op_`): a non-commutative operator may have a `@Right` method and a plain one with the same parameters ([EXP-26](06-expressions.md#610-operator-methods-adr-0024)). The parameter types are the *declared* ones: in a method of a generic type a type parameter of the receiver is `Q` and its index (a unit type parameter `w`), so `func (a W[T]) + (b T)` and `func (a W[T]) + (b float64)` keep two names in `W[float64]`, where both take a `float64`. A method whose name is an operator spelling must be built with its parameter tuple (`Context::method(Recv, Name, Params, Right)`). An ordinary method that happens to be called `op_add` is a Method (`M`), a different tag, so the two never meet. A `@Convert` method (ADR-0024) is an ordinary method and has an ordinary Method name: `float64(b)` calls `BigInt.Float64`; it is unique per target type because its name is ([EXP-25](06-expressions.md#610-operator-methods-adr-0024)).
- **Instance** `I`: a generic function, method or type with its type arguments. A method of a generic type is `Method(Instance(Named, args), name)`; a generic method (with its own type parameters) of it is `Instance(Method(...), method args)`. Symbols name instances, never generic declarations.
- **Closure** `C`: a function literal is its ordinal (1, 2, ... in source order among the literals directly inside the enclosing function) and the enclosing entity, which is a function, method, instance, variable (for a package variable's initializer) or another closure. A closure in a generic method is a closure of the *instance*, so it has one name per instance. The ordinal is a property of the source text, never of the order the compiler met the literals.
- **PkgInit** `J` and **InitFunc** `K`: the package's init entry point, and its *n*th `init` function (n from 0 in the order of the sorted file names, then source position). A package may declare many `init` functions, none of them in its scope, so an `init` function never has a Func name; nor does a blank function or method (`func _()`), which is never emitted.
- **TypeInfo** `Ti`, **VTable** `Tv` (ADR-0023 type table), **ITab** `Ta` (concrete type then interface type), **EqGlue**, **HashGlue**, **DropGlue**, **CloneGlue**, **MethodTable** `Tm` (the methods of a type's method set with their keys, which an assertion to an interface walks; P04.06), **IfaceDesc** `Tn` (an interface type's method keys, for those assertions; P04.06): one per type. Two types that are identical have the same type node, hence the same names (A.6). `gopp demangle` prints them as "typeinfo for main.T", "method table for main.T", "interface descriptor for main.I".
- **Data** `TD`: constant data, keyed by a content digest the caller computes (for example a hash of a string literal's bytes).
- **CABIThunk** `TC` (D4.4), **MethodValue** `TV`, **MethodExpr** `TE`, **Promoted** `TP`, **IfaceThunk** `TI`: generated wrappers, named by the entity they wrap. A C-ABI thunk of an `@Export` function is *not* named like this: its symbol is the declared C name (A.8); `TC` names the thunk of a function used as a callback.
- **IfaceThunk** `TI` and **IfacePtrThunk** `Tp` (P04.06): the adapter an itab holds for a method, named by the method as the method's own symbol names it (a method belongs to its type, whatever its receiver). `TI` is the adapter for the method table of the method's type, and for that of the pointer type when the method has a pointer receiver; `Tp` is the adapter of a value method for the method table of the pointer type, which reads the value through the pointer, so the two adapters of one value method (`var s Summer = a` and `= &a`) have two names. `gopp demangle` prints them as `interface thunk for main.T.M` and `interface thunk for (*main.T).M`.

### A.5.1 Operator method names

The name of an operator method is `op_` and the mnemonic of its row; the row is chosen by the spelling and the number of parameters. A `@Right` method has `opr_` in place of `op_` (`opr_quo` is the `@Right` `/`). A spelling without a row at that count (a future operator, a malformed declaration) is written `opx_` (`oprx_`) and the spelling, escaped like any identifier, so the mangler never fails and a new operator needs no change here. The readable form is the spelling, the parameter types and, for `@Right`, ` @Right` after them, as the checker's messages write it: `main.Vec2./(float64) @Right`.

| Spelling | Parameters | Mnemonic |
|---|---|---|
| `+ - * / % & \| ^ << >> &^` | 1 | `add sub mul quo rem and or xor shl shr andnot` |
| `+= -= *= /= %= &= \|= ^= <<= >>= &^=` | 1 | `addassign subassign mulassign quoassign remassign andassign orassign xorassign shlassign shrassign andnotassign` |
| `- + ^ ! * ++ --` | 0 | `neg pos compl not deref inc dec` |
| `==` `<=>` `&&` `\|\|` | 1 | `eq cmp land lor` |
| `[]` | 1 or more | `index` |
| `[]=` | 2 or more | `setindex` |
| `()` | any | `call` |
| `<-` | 1, 0 | `send`, `recv` |
| `range` | any | `range` |

## A.6 Types

- **Identity.** The type node is the Go++ type identity, the one the checker decides (`Checker::identical`): two types have one node exactly when they are identical. `Option[T]`, `View[T]` and the borrowed slice are different nodes; a struct includes `@Strict`, the names, every mark of its fields (embedded, parent, the effective and the written `@Required`, "must be set" only, `@Option`), tags and (for unexported names) the package of its fields; an interface is its method set, sorted by package and name, with embedded interfaces flattened (so `interface{ B(); A() }` and `interface{ I; B() }` with `I` declaring `A()` are one node); a function type has its parameters, results, variadic flag and move mask, but not the `!` mark (ERR-2). `byte` and `rune` are `uint8` and `int32`. Defined types are `Named` (package path and name), or `LocalNamed` for a type declared inside a function (the declaring entity, an ordinal for the blocks that declare one name twice, and the name; inside a generic function the declaring entity is the instance, so a local type has one name per instance). `error` and the other predeclared generics are `Universe`. Constraint interfaces and unions (type sets, operator elements) are never the type of a value and have no node.
- **Units** (ADR-0025). A unit type is `float64[U]` with `U` in its **canonical form** (UNI-5): the factor as an exact reduced fraction and the base units sorted by package path and name with their exponents. `KM/H`, `KM*H^-1` and `1000 M / (3600 S)` are one type and so one name. The *display form* a program uses to print a unit is not part of any name. A unit given as a type argument (`Vec2[KM]`) is the Unit node itself. A canonical form that mentions a unit type parameter is for debugging generic code only.
- **Type parameters** `Q` (and unit type parameters `w`) name a type parameter of the receiver in the declared parameters of an operator method (A.5). Nowhere else are they part of an emitted symbol: a symbol names an instance.
- **Back references.** A type that occurs again in the same name and is at least 8 characters long when written out is written as `B` and the offset in the name (counted from the `_` of the prefix) of its first occurrence: `main.Vec2.+(main.Vec2)` is `_GPTo6op_addN4mainE4Vec2LB12_E` (the second `main.Vec2` is the type that starts at offset 12, the `N`). Two rules make this canonical: the *only* subtrees that become references are type nodes whose written-out size is at least 8, and the reference is to the *first* occurrence. A name in which a type is written out again, or where a reference points anywhere else, is refused. This is what keeps `Pair[Pair[Pair[...]]]`, where each level repeats the one below, linear in its depth instead of exponential.

## A.7 Examples

These rows are checked against the code by the unit tests `SymbolName.Goldens` and `SymbolName.TheAppendixIsTheCode`.

| Name | Readable |
|---|---|
| `_GPF4mainE3Foo` | `main.Foo` |
| `_GPF3std7stringsE5Index` | `std/strings.Index` |
| `_GPF13github$2e$com1x1yE3Run` | `github.com/x/y.Run` |
| `_GPF4mainE18$65e5$$672c$$8a9e$` | `main.日本語` |
| `_GPF1pE5$30$x` | `p.0x` |
| `_GPM3LenN4mainE4Vec2` | `main.Vec2.Len` |
| `_GPTo6op_addN4mainE4Vec2LB12_E` | `main.Vec2.+(main.Vec2)` |
| `_GPTo6op_negN4mainE4Vec2LE` | `main.Vec2.-()` |
| `_GPTo6op_quoN4mainE4Vec2LdE` | `main.Vec2./(float64)` |
| `_GPTo7opr_quoN4mainE4Vec2LdE` | `main.Vec2./(float64) @Right` |
| `_GPTo6op_addIN4mainE1WdELQ0_E` | `main.W[float64].+($0)` |
| `_GPIF3std6slicesE3MapirE` | `std/slices.Map[int, string]` |
| `_GPC2_M3GetIN4mainE3BoxiE` | `main.Box[int].Get.func2` |
| `_GPTiHrSi` | `typeinfo for map[string][]int` |
| `_GPTvN4mainE3Dog` | `vtable for main.Dog` |
| `_GPTmN4mainE3Dog` | `method table for main.Dog` |
| `_GPTnN4mainE6Shaper` | `interface descriptor for main.Shaper` |
| `_GPJ4mainE` | `main.init` |
| `_GPK4mainE1_` | `main.init.1` |
| `_GPR4mainE7counter` | `main.counter` |
| `_GPTCF4mainE8Callback` | `C-ABI thunk for main.Callback` |
| `_GPTIM4BumpN4mainE3Box` | `interface thunk for main.Box.Bump` |
| `_GPTpM3SumN4mainE5Adder` | `interface thunk for (*main.Adder).Sum` |
| `_GPTiUdk5_18_o5unitsE1M1_o5unitsE1Sn1_E` | `typeinfo for float64[(5/18) units.M/units.S]` |
| `_GPTiIN4mainE4PairIB6_iiEB18_E` | `typeinfo for main.Pair[main.Pair[int, int], main.Pair[int, int]]` |
| `_GPTiG0_0_LiELru5errorE` | `typeinfo for func(int) (string, error)` |
| `_GPTiZ1_q8_1aiE` | `typeinfo for @Strict struct{a int}` |
| `_GPTiYE` | `typeinfo for any` |

## A.8 Names that are not mangled

- **`@Export`** (spec chapter 14): the C symbol is the declared name, exactly. An `@Export` name (and any other name that is visible to C) that starts with `_GP` must be an error, so a C name can never be one of ours; `sym::isReservedName` is the one test for it. The check is made where the code generator takes the declared name (P04.18, D4.4); until then nothing emits a C-visible name. The Go++ function itself keeps its mangled name; the exported C symbol is the thunk.
- **Not named yet** (the checker does not record what the name needs): a type declared inside a function and a function literal need their enclosing function and ordinal, and an `init` function its ordinal, which the lowering knows (`LocalNamed`, `Closure` and `InitFunc` are in the grammar; P04.04 and P04.07 provide them). `SemaNames` refuses such a type or function with a reason instead of guessing.
- **Foreign (header) symbols** keep the name the C or C++ compiler gives them. Go++ never defines a symbol with a C++ or C name for its own entities.
- **`@Extern("C")`** (MAC-8): the function is defined in C, so its symbol is its C name. The checker does not mark such a function yet (P07), so the code generator takes the name from the attribute and never asks `SemaNames` for it.
- **LLVM and linker names** (`llvm.*`, `.L*`, section names) are not Go++ symbols.

## A.9 The demangler

`gopp demangle NAME...` prints one readable name per argument and exits 1, with a message and the offset, for a name that is not a Go++ symbol. With no argument it filters standard input like `c++filt`: every word that starts with `_GP` and is a name is replaced; a word that is not is left alone. Use it on the output of `perf report`, `objdump`, `nm` and sanitizer reports. `-max-output=N` cuts a readable name at N bytes (a repeated type is exponentially long once written out); the symbol itself is never affected.

What the demangler refuses (SYM-5, SYM-6): everything the mangler does not write. Besides bad syntax, a number with a leading zero, an unescaped or wrongly escaped character, a child of the wrong kind, a back reference that is not where the mangler writes one, and the children of a set (interface methods, unit dimensions) out of order, it refuses the units the checker never makes (UNI-5, UNI-6):

- a dimension (`o` or `w`) with **exponent 0**, which is not part of a canonical unit;
- the **same dimension twice** (the dimensions are sorted *strictly* by package path and name, parameters by index);
- an exponent whose **magnitude is above 2^20**, the checker's limit (`MaxUnitExp`, kept equal to `units::MaxExp` by a `static_assert`); this also covers the most negative 64-bit number, which has no positive twin.

These rules are also what keeps the demangler linear: every node the readable form expands prints at least one byte of its own (the one exception, an empty package path with an empty name, is too short to be a back reference), so the work for a name is the size of the text plus the size of the output, and the output is cut at `-max-output`. `SymbolNameLong.HostileTextCostsLinearTime` (long suite) times five hostile shapes (dimensions to the power 0, one dimension repeated, many dimensions under many back references, an empty struct under many back references, a wide tuple) at two sizes.

Readable forms: a type is written as the type printer of the checker writes it (TypeString), with package paths in full in place of package names (`std/strings.Index`); the test `SemaSymbols.ReadableNameIsTheTypePrintersText` compares the two on every type of a checked package. Where one name stands for several spellings of one type, the demangler writes one of them: `uint8` and `int32` (for `byte` and `rune`), a fallible function type with its `error` result (`func(int) (string, error)`), an interface as its sorted method set, and the empty one as `any` (`SemaSymbols.SpellingDiffersOnlyForOneTypeWrittenTwoWays`). A generic instance is `Name[args]`, a method `Type.Name`, an operator method has its parameters (`main.Vec2.*(float64)`, `main.Vec2./(float64) @Right`), a closure is `parent.funcN`, a function type `func(a, b) (r, s)` (`...T` for a variadic last parameter), a unit is its canonical text (`(5/18) units.M/units.S`, `1000 units.M`), and the glue and tables are `typeinfo for T`, `vtable for T`, `itab for T as I`, `eq glue for T`, `hash glue for T`, `drop glue for T`, `clone glue for T`, `interface thunk for T.M` and, for the pointer type's table, `interface thunk for (*T).M`.

The debug information (P04.12) carries the *readable* name as `DW_AT_name`. The mangled one is `DW_AT_linkage_name` only with the hidden option `-dwarf-linkage-names` (a deviation from the plan, P04 D4.7 and D4.13): gdb and lldb show a linkage name in place of `DW_AT_name` when it is not an Itanium one and cannot demangle `_GP`, so a default backtrace would read `_GPF4mainE4main`. The symbol table has the `_GP` name either way, and `gopp demangle` covers tools that only see it. The linkage name comes back when the debuggers demangle `_GP` (P14.07).

## A.10 How the guarantees are tested

- `SymbolName.CorpusRoundTrips`, `EveryIdentifierRoundTrips`, `AllCodePointsRoundTrip`: a generated corpus over every kind (6000 entities, depth 4, with repeated subtrees) and the nastiest names (Unicode, operator spellings, invalid UTF-8, NUL, empty names and paths, every byte, every code point), checked against an independent oracle for the readable text.
- `SymbolName.SameNameWhateverTheBuildOrder`: the same entities built in 12 shuffled orders, in Contexts with different amounts of unrelated nodes, give the same bytes. `SemaSymbols.NamesDoNotDependOnCheckingOrderOrThreads`: four packages (generic types and methods, operator overloads, units, `extends`) checked in 8 shuffled orders, with 1 and 16 body threads, give the same set of names, byte for byte.
- `SemaSymbols.OneNameExactlyWhenTheCheckerSaysIdentical` (SYM-2): every pair of 56 type spellings, with the checker as the oracle (`func(x *A) *B { return x }` type-checks exactly when `A` and `B` are identical); `StructIdentityHasEveryFieldMark` for the `@Strict` and "must be set" marks; `EveryMethodHasItsOwnName` for operator overloads, `@Right` and the methods of instances; `WhatHasNoSymbolSaysWhy` for generic code, local types, constraints, `init` and blank functions and blank variables (refused with a reason, never one shared name); `UnitsAreNamedByTheirCanonicalForm`, `PackagePathsWithDotsSlashesAndUnicode`, `SymbolName.BoundariesBetweenPathsAndNamesNeverBlur`.
- `SemaSymbols.ReadableNameIsTheTypePrintersText` and `SpellingDiffersOnlyForOneTypeWrittenTwoWays` (SYM-3).
- `SymbolName.NeverCollidesWithACName`, `TagsArePrefixFreeAndInTheAlphabet`.
- `SymbolName.EveryPrefixOfAValidNameIsRejected`, `MutatedNamesNeverCrash`, `RandomTextNeverCrashes`, `HostileShapes`, the libFuzzer target `gopp-fuzz-symbol` (`tests/fuzz/README.md`) and the long suite `SymbolNameLong.*` (1.5 million mutated names, run under ASan too).
- `SymbolName.ExponentialSharingStaysSmall`, `DeepAndWideNamesWork` and `SymbolNameLong.*`: eight shapes of long name (deep, wide, a long identifier, a long path, many unit dimensions, shared halves) at 16000 and 256000 in size must not take more than 6 times the linear time; a name a million levels deep round-trips. `SemaSymbolsLong.*`: the same for `SemaNames` on five shapes of checker type (deep chains of every constructor, func types, a wide struct and tuple, shared halves) at 10000 and 160000.
