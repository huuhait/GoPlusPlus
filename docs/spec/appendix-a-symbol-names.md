# Appendix A. Symbol names

Plan decision D4.13, task P04.17. Code: `crates/gopp-mangle` (above `gopp-sema`, below `gopp-gir` and `gopp-codegen-c`: the nodes of a name in a `Context`, the mangler `Context::symbol`, the parser `parse`, the readable printer `Context::readable`, and `symbol_of` for what the checker knows); `gopp demangle` is in `crates/gopp-cli`. This appendix is not part of the language: a program cannot see a symbol name. It is the contract between the code generator, the linker, the build cache and the tools that read object files, traces and profiles.

## A.1 What a name guarantees

**SYM-1.** *From content.* A name is a function of the *content* of the thing it names: package paths, declared names, type arguments. It never depends on creation order, on a pointer, on a thread, on the order the packages were checked in, or on how many threads checked them. The same entity has the same name in every package, every build and every machine.

**SYM-2.** *One type, one name.* Two types have the same name exactly when they are identical types (A.6): a spelling the identity ignores (`byte` for `uint8`, `func!(int) T` for `func(int) (T, error)` ([ERR-2](09-errors-and-panics.md#91-functions-that-can-fail-d18))) gives the same name, and every part the identity compares gives another one. Every declared function and method has its own name, operator overloads too (A.5.1). A generic instance (`Map[int, string]`, `Box[float64]`) has one name wherever it is used, so every package refers to the one home of the instance (D4.3), and weak copies fold at link time.

**SYM-3.** *Readable.* `gopp demangle` turns every name back into Go++ text (A.9); for a type it is the type printer's text, so a name in a trace reads like the type in a compile error. The name itself is also legible: `_GPF4mainE3Foo` is the function `Foo` of package `main`.

**SYM-4.** *No collision with C.* Every name starts with `_GP` (A.2), and a name that C code can see (`@Export`, a C-ABI name) must not (A.8).

**SYM-5.** *Reversible and canonical.* `demangle(mangle(x))` is `x`, and the demangler accepts exactly the text the mangler writes: `mangle(parse(S)) == S` for every accepted `S`, so two different entities never share a name and one entity never has two.

**SYM-6.** *Total.* The mangler has no length limit and no failure. The demangler refuses malformed text with a message and an offset, never with a crash, and its walks are iterative, so a name a million levels deep is fine.

## A.2 Characters and the prefix

A name is `_GP` followed by an *entity* (A.5). The characters are `A-Z a-z 0-9 _ $` only, which every object format and assembler accepts. `_GP` is the version: a future format change uses another prefix (`_GQ`, ...), so old and new names never mix.

`_` followed by a capital letter is in the namespace that C reserves for the implementation, so no C program declares `_GP...`. The other manglings in the same objects start differently: C++ `_Z`, Rust `_R` and `_ZN`, Swift `$s`. `gopp_mangle::is_reserved_name(name)` says whether a name has the prefix.

## A.3 Identifiers, paths and numbers

An **identifier** (a declared name, a package segment, a struct tag) is its length in decimal, then that many characters of the escaped text:

| Case | Written as |
|---|---|
| a letter or `_` | itself |
| a digit, not first | itself |
| any other code point (a digit first, a non-ASCII letter, `$`, a NUL, ...) | `$` + the code point in lowercase hex, no leading zeros + `$` |
| a byte that is not part of well-formed UTF-8 | `$x` + two lowercase hex digits + `$` |
| the empty identifier | `0_` |

The length counts the escaped characters, and the escaped text never starts with a digit, so `3Foo`, `5$30$x` (`0x`) and `18$65e5$$672c$$8a9e$` (日本語) read without ambiguity.

A **package path** is its segments (the text between `/`) as identifiers, then `E`: `std/strings` is `3std7stringsE`, `github.com/x/y` is `13github$2e$com1x1yE`, the empty path is `0_E`. A **number** is decimal digits, no leading zero (except `0`), then `_`: `12_`. A **signed number** has an `n` first for a negative one: `n3_`.

## A.4 Grammar

```
symbol  = "_GP" entity .
entity  = Func | Method | OpMethod | Var | Closure | PkgInit | InitFunc
        | TypeInfo | VTable | SecondaryVTable | ITab | EqGlue | HashGlue | DropGlue | CloneGlue
        | Data | CABIThunk | MethodValue | MethodExpr | Promoted | IfaceThunk
        | IfacePtrThunk | BaseAdjustThunk | MethodTable | IfaceDesc
        | Instance(entity base) .
type    = Basic | Pointer | Slice | Array | Map | Option | Chan
        | Tuple | Signature | Struct | Interface | Named | LocalNamed | Universe
        | Instance(type base) | TypeParam | BackRef .
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
| `X` | Chan | `b` both, `s` send, `r` receive | type |
| `L` | Tuple | | types, `E` |
| `G` | Signature | flags (number: 1 variadic), `0_` | parameter Tuple, result Tuple |
| `Z` | Struct | `0_` | Fields, `E` |
| `q` | Field | flags (number), [package path], name, [tag] | type |
| `Y` | Interface | flags (number: 1 `Sync`), when non-zero; omitted when `0` | IMethods sorted by package, name; `E` |
| `n` | IMethod | `0`, or `1` and a package path (unexported name); name | Signature |
| `N` | Named | package path, name | |
| `g` | LocalNamed | ordinal (number), name | the declaring entity |
| `u` | Universe | name (`error`, `Unique`, `Local`, ...) | |
| `I` | Instance | | the generic thing, type arguments (at least one), `E` |
| `Q` | TypeParam | index (number) | |
| `B` | BackRef | offset (number) | |
| `F` | Func | package path, name | |
| `M` | Method | name | receiver type: a named type, or an interface type |
| `To` | OpMethod | name (`op_`, or `opr_` for `@Right`, and a mnemonic: A.5.1) | receiver named type, parameter Tuple |
| `R` | Var | package path, name | |
| `C` | Closure | ordinal (number) | enclosing entity |
| `J` | PkgInit | package path | |
| `K` | InitFunc | package path, ordinal (number) | |
| `Ti` | TypeInfo | | type |
| `Tv` | VTable | | type |
| `Ts` | SecondaryVTable | | child type, parent type |
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
| `Tt` | BaseAdjustThunk | offset (signed number) | method |

Field flags, every mark of a field that type identity compares ([TYP-15](03-types.md#34-required-fields-d14-rule-11)): 1 embedded, 2 the name is unexported so its package follows, 4 a tag follows, 8 `@Required`, 16 reserved, 32 the hidden parent part of a type with parents (ADR-0023), 64 reserved, 128 required in the "must be set" sense only (the field of an instance declared with a type parameter, ADR-0016 point 8). Signature flags: 1 variadic; the second header number is always `0_`. There is no flag for `!`: `func!(P) R` is identical to `func(P) (R, error)` (ERR-2) and is written as that. A field is never both 8 and 128. The last parameter of a variadic Signature is a slice.

Which child a node may have is fixed, and `Context::add` and the parser reject any other:

- the receiver of a Method or an OpMethod is a named or local-named type, or an instance of one;
- the first child of an Instance is a named type, a local-named type, a `Universe` type, a function, a method or an operator method, and it has at least one type argument;
- the entity of a Closure or a LocalNamed is one that code is written in: a function, a method, an operator method, an instance of one of these, a variable, an `init` function, a closure, or the package's init entry point (its closure with the number *n*, from 1, is the function that gives the variables of the *n*th step of the start their first value);
- `TC`, `TV`, `TE`, `TP` and `TI` are made around a function, a method, an operator method, an instance of one of these, a closure or another thunk;
- the child of an IfacePtrThunk or a BaseAdjustThunk is a method or an operator method, or an instance of one;
- every other child is a type where the table says type, a Field in a Struct and an IMethod in an Interface.

## A.5 What each entity is

- **Func** `F`: a package-level function. **Var** `R`: a package-level variable (not a blank one, `var _ = f()`, which has no storage).
- **Method** `M`: a method of the named type (or instance) in its child. The receiver kind (value or pointer) is not part of the name: a method belongs to the type. A method of an interface type without a name has that type as its child: only its method value and its method expression have code, as `_GPTVM4AreaYn04AreaG0_0_LELiEE`, `method value interface{Area() int}.Area`. Promoted methods of types with parents and of embedded types are named by their **Promoted** wrapper (`TP`) around the method of the child type.
- **OpMethod** `To`: an `@Operator` method ([ADR-0024](../adr/0024-operator-overloading.md)). Its name is `op_` (or `opr_` for `@Right`) followed by the Rust-style operator method identifier (`op_Add`, `op_Mul`, `opr_Div`, …; A.5.1), and **the parameter types are part of the name**, because several `@Operator` methods with one name and different parameter lists form an overload set (`Vec2.Mul(float64)`, `Vec2.Mul(Vec2)`). **`@Right` is part of the name too** (`opr_` in place of `op_`): a non-commutative operator may have a `@Right` method and a plain one with the same parameters ([EXP-26](06-expressions.md#610-operator-methods-adr-0024)). The parameter types are the *declared* ones: in a method of a generic type a type parameter of the receiver is `Q` and its index, so `@Operator func (a W[T]) Add(b T)` and `@Operator func (a W[T]) Add(b float64)` keep two names in `W[float64]`, where both take a `float64`. An `@Operator` method is the node `OpMethod`, with its parameter tuple and whether it is `@Right`. An ordinary method without `@Operator` is a Method (`M`), a different tag, so the two never meet. A `@Convert` method (ADR-0024) is an ordinary method and has an ordinary Method name: `float64(b)` calls `BigInt.Float64`; it is unique per target type because its name is ([EXP-25](06-expressions.md#610-operator-methods-adr-0024)).
- **Instance** `I`: a generic function, method or type with its type arguments. A method of a generic type is `Method(Instance(Named, args), name)`; a generic method (with its own type parameters) of it is `Instance(Method(...), method args)`. Symbols name instances, never generic declarations.
- **Closure** `C`: a function literal is its ordinal (1, 2, ... in source order among the literals directly inside the enclosing function) and the enclosing entity, which is a function, method, instance, variable (for a package variable's initializer), `init` function or another closure. A closure in a generic method is a closure of the *instance*, so it has one name per instance. The ordinal is a property of the source text, never of the order the compiler met the literals. The body of a `range` loop over a function is a function too ([RNG-6a](08-slices-and-range.md)): the `range` statements that are written directly inside the same function have the ordinals after those of its literals, in source order, whatever they range over. The ordinal 0, which no literal has, names the function that the compiler makes for the deferred calls of the enclosing function or closure ([PAN-4](09-errors-and-panics.md)); `gopp demangle` reads it as `func0`.
- **PkgInit** `J` and **InitFunc** `K`: the package's init entry point, and its *n*th `init` function (n from 0 in the order of the sorted file names, then source position). A package may declare many `init` functions, none of them in its scope, so an `init` function never has a Func name; nor does a blank function or method (`func _()`), which is never emitted.
- **TypeInfo** `Ti`, **VTable** `Tv` (primary ADR-0023 type table), **SecondaryVTable** `Ts` (secondary `(Child, Pi)` type table carrying `-offset(Pi)` and `BaseAdjustThunk` `Tt` overrides, [TYP-22](03-types.md#35-inheritance-adr-0023)), **ITab** `Ta` (concrete type then interface type), **EqGlue**, **HashGlue**, **DropGlue**, **CloneGlue**, **MethodTable** `Tm` (the methods of a type's method set with their keys, which an assertion to an interface walks; P04.06), **IfaceDesc** `Tn` (an interface type's method keys, for those assertions; P04.06): one per type (or `(Child, Pi)` pair for `Ts`). Two types that are identical have the same type node, hence the same names (A.6). `gopp demangle` prints them as "typeinfo for main.T", "method table for main.T", "interface descriptor for main.I".
- **Data** `TD`: constant data, keyed by a content digest the caller computes (for example a hash of a string literal's bytes).
- **CABIThunk** `TC` (D4.4), **MethodValue** `TV`, **MethodExpr** `TE`, **Promoted** `TP`, **IfaceThunk** `TI`: generated wrappers, named by the entity they wrap. A C-ABI thunk of an `@Export` function is *not* named like this: its symbol is the declared C name (A.8); `TC` names the thunk of a function used as a callback. The code of a func value (P04.07) is one of these, emitted as a weak definition: `TV` of a method is the code of a method value `x.M` (the receiver is in its environment); `TE` of a method is the code of the method expression `T.M`, and `TE` of a package-level function (an `@Extern` one too) is the code of the function used as a value (`f := add`), the same shape (the entity with the environment in front, which it ignores); `(*T).M` of a value method is `TE` of the method's `Tp` (it reads the value through the pointer). `TV` of the `TE` of a method is the code of `base.M` as a value, for a method of a parent that a child may replace: the method value that calls the method of the parent itself and does not look into the type table of the value ([TYP-20](03-types.md)). `TV` of the method `Clone` of a type with a type table that declares no `Clone` is the code of `p.Clone` as a value, which copies the whole value that `p` points into. `gopp demangle` prints all `TE` as "method expression ...". The code of an interface method value (`i.M`) wraps no entity (an interface method has no symbol): it calls the function the value holds, so it is one internal function per code type in each module, never named after the interface.
- **IfaceThunk** `TI` and **IfacePtrThunk** `Tp` (P04.06): the adapter that a table of methods of an interface holds for a method, named by the method as the method's own symbol names it (a method belongs to its type, whatever its receiver). `TI` is the adapter in the table of the method's type, for an interface value that holds a value of the type (in Go++ such a value has the methods with a pointer receiver too, [TYP-5](03-types.md)); `Tp` is the adapter in the table of the pointer type, for an interface value that holds a pointer. The two differ in where they find the value, so the two adapters of one method (`var s Summer = a` and `= &a`) have two names. `gopp demangle` prints them as `interface thunk for main.T.M` and `interface thunk for (*main.T).M`.

### A.5.1 Operator method names

The name of an `@Operator` method in `To` is `op_` (or `opr_` for `@Right`) followed by its declared Rust-style method name (`Add`, `Sub`, `Mul`, `Div`, `Rem`, `BitAnd`, `BitOr`, `BitXor`, `BitClear`, `Shl`, `Shr`, `Neg`, `Not`, `BitNot`, `AddAssign` .. `ShrAssign`, `Eq`, `Cmp`, `Index`, `SetIndex`, `Iter`). The readable form prints the method name, the parameter types and, for `@Right`, ` @Right` after them: `main.Vec2.Div(float64) @Right`.

## A.6 Types

- **Identity.** The type node is the Go++ type identity, the one the checker decides (`gopp_sema::identical`): two types have one node exactly when they are identical. `Option[T]` is its own node; a struct includes the names, every mark of its fields (embedded, parent, `@Required`, "must be set" only), tags and (for unexported names) the package of its fields; an interface includes its `Sync` mark (`Y1_` when embedding `Sync`, plain `Y` otherwise) and its method set, with embedded interfaces flattened, in one order: the exported names first, then by the package path of an unexported name, then by the name, each compared byte by byte (so `interface{ B(); A() }` and `interface{ I; B() }` with `I` declaring `A()` are one node); a function type has its parameters, results and variadic flag (`0_` in the reserved second header slot), without the `!` mark (ERR-2). `byte` and `rune` are `uint8` and `int32`. Defined types are `Named` (package path and name), or `LocalNamed` for a type declared inside a function (the declaring entity, an ordinal, and the name; the ordinal says which of the types of that name in the function it is, from 0 in source order, since two blocks may declare one name; inside a generic function the declaring entity is the instance, so a local type has one name per instance). `error` is `Universe`, and `Unique[T]`, `Shared[T]`, `Weak[T]` and `Local[T]` are instances of a `Universe` node; `Option[T]` has its own node. `unsafe.Pointer` is the basic type `p`. A declared constraint has no node either. Constraint interfaces and unions (type sets, operator elements) are never the type of a value and have no node.
- **Type parameters** `Q` name a type parameter of the receiver in the declared parameters of an `@Operator` method (A.5). Nowhere else are they part of an emitted symbol: a symbol names an instance.
- **Back references.** A type that occurs again in the same name and is at least 8 characters long when written out is written as `B` and the offset in the name (counted from the `_` of the prefix) of its first occurrence: `main.Vec2.Add(main.Vec2)` is `_GPTo6op_AddN4mainE4Vec2LB12_E` (the second `main.Vec2` is the type that starts at offset 12, the `N`). Two rules make this canonical: the *only* subtrees that become references are type nodes whose written-out size is at least 8, and the reference is to the *first* occurrence. A name in which a type is written out again, or where a reference points anywhere else, is refused. This is what keeps `Pair[Pair[Pair[...]]]`, where each level repeats the one below, linear in its depth instead of exponential.

## A.7 Examples

These rows are checked against the code by the `gopp-mangle` tests `symbol_name::goldens` and `symbol_name::the_appendix_is_the_code`.

| Name | Readable |
|---|---|
| `_GPF4mainE3Foo` | `main.Foo` |
| `_GPF3std7stringsE5Index` | `std/strings.Index` |
| `_GPF13github$2e$com1x1yE3Run` | `github.com/x/y.Run` |
| `_GPF4mainE18$65e5$$672c$$8a9e$` | `main.日本語` |
| `_GPF1pE5$30$x` | `p.0x` |
| `_GPM3LenN4mainE4Vec2` | `main.Vec2.Len` |
| `_GPTo6op_AddN4mainE4Vec2LB12_E` | `main.Vec2.Add(main.Vec2)` |
| `_GPTo6op_NegN4mainE4Vec2LE` | `main.Vec2.Neg()` |
| `_GPTo6op_DivN4mainE4Vec2LdE` | `main.Vec2.Div(float64)` |
| `_GPTo7opr_DivN4mainE4Vec2LdE` | `main.Vec2.Div(float64) @Right` |
| `_GPTo6op_AddIN4mainE1WdELQ0_E` | `main.W[float64].Add($0)` |
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
| `_GPTiIN4mainE4PairIB6_iiEB18_E` | `typeinfo for main.Pair[main.Pair[int, int], main.Pair[int, int]]` |
| `_GPTiG0_0_LiELru5errorE` | `typeinfo for func(int) (string, error)` |
| `_GPTiZ0_q8_1aiE` | `typeinfo for struct{a int @Required}` |
| `_GPTiYE` | `typeinfo for any` |

## A.8 Names that are not mangled

- **`@Export`** (spec chapter 13): the C symbol is the declared name, exactly. An `@Export` name (and any other name that is visible to C) that starts with `_GP` is an error (`E0445`; `E0442` for an `@Extern` name), so a C name can never be one of ours; `gopp_mangle::is_reserved_name` is the one test for it. A name that starts with `_gopp_`, as the helpers of the runtime do, is refused the same way. The check is made where the lowering takes the declared name (D4.4). The Go++ function itself keeps its mangled name; the exported C symbol is the thunk.
- **What the checker does not record:** a type declared inside a function needs its function and its ordinal, and an `init` function its number. The lowering knows both and gives them to `gopp_mangle::symbol_of` (`Namer::with_locals`, `Entity::InitFunc`); without them `symbol_of` refuses the type with a reason instead of guessing. A function literal gets its `Closure` name when function literals are lowered (P04.07); until then a type declared inside one is named with the function around it.
- **What has no symbol:** generic code (only its instances), a constraint, a constant, a type or function in error, and what is named `_`. `symbol_of` says which (`NoSymbol`).
- **Foreign (header) symbols** keep the name the C or C++ compiler gives them. Go++ never defines a symbol with a C++ or C name for its own entities.
- **`@Extern("C")`** (MAC-8): the function is defined in C, so its symbol is its C name. The checker marks such a function, and the lowering calls it by its name and never asks `gopp_mangle::symbol_of` for it.
- **Compiler helper and linker names** (`_gopp_*`, `.L*`, section names) are not Go++ symbols.

## A.9 The demangler

`gopp demangle NAME...` prints one readable name per argument and exits 1, with a message and the offset, for a name that is not a Go++ symbol. With no argument it filters standard input like `c++filt`: every word that starts with `_GP` and is a name is replaced; a word that is not is left alone. Use it on the output of `perf report`, `objdump`, `nm` and sanitizer reports. `-max-output=N` cuts a readable name at N bytes, 1 MiB unless it is given (a repeated type is exponentially long once written out); the symbol itself is never affected.

What the demangler refuses (SYM-5, SYM-6): everything the mangler does not write — bad syntax, a number with a leading zero, an unescaped or wrongly escaped character, a child of the wrong kind, a back reference that is not where the mangler writes one, and the children of a set (interface methods) out of order.

These rules keep the demangler linear: the parser and the printer walk with a stack of their own, never by recursion, and every type that a back reference names prints at least one byte, so the work for a name is the size of the text plus the size of the output, and the output is cut at `-max-output`. `symbol_name::long_names_take_linear_steps` counts the steps of eight shapes (deep, wide, shared halves, a long identifier, a long path, a tuple of back references, nested closures, a wide struct), and `long::hostile_text_costs_linear_time` (long suite) times them at two sizes.

Readable forms: a type is written as the type printer of the checker writes it (TypeString), with package paths in full in place of package names (`std/strings.Index`); the test `sema_symbols::readable_name_is_the_type_printers_text` compares the two on a table of types of a checked package. Where one name stands for several spellings of one type, the demangler writes one of them: `uint8` and `int32` (for `byte` and `rune`), a fallible function type with its `error` result (`func(int) (string, error)`), an interface as its sorted method set, and the empty one as `any` (`sema_symbols::spelling_differs_only_for_one_type_written_two_ways`). A generic instance is `Name[args]`, a method `Type.Name`, an `@Operator` method has its parameters (`main.Vec2.Mul(float64)`, `main.Vec2.Div(float64) @Right`), a closure is `parent.funcN`, a function type `func(a, b) (r, s)` (`...T` for a variadic last parameter), and the glue and tables are `typeinfo for T`, `vtable for T`, `vtable for T as P` (the secondary one), `itab for T as I`, `eq glue for T`, `hash glue for T`, `drop glue for T`, `clone glue for T`, `interface thunk for T.M` and, for the pointer type's table, `interface thunk for (*T).M`. The other forms: a local type is `f.T` (`f.T.1` for the second `T` of `f`), an `init` function `p.init.N`, a type parameter `$N`, an interface that embeds `Sync` `interface{M(); Sync}`, the marks of a field `@Required`, `@MustSet` and `@Parent` after its type, and `method value X`, `method expression X`, `promoted method X`, `C-ABI thunk for X`, `thunk at offset N for X`, `data KEY`.

The debug information (P04.12) carries the *readable* name as `DW_AT_name`. The mangled one is `DW_AT_linkage_name` only with the hidden option `-dwarf-linkage-names` (a deviation from the plan, P04 D4.7 and D4.13): gdb and lldb show a linkage name in place of `DW_AT_name` when it is not an Itanium one and cannot demangle `_GP`, so a default backtrace would read `_GPF4mainE4main`. The symbol table has the `_GP` name either way, and `gopp demangle` covers tools that only see it. The linkage name comes back when the debuggers demangle `_GP` (P14.07).

## A.10 How the guarantees are tested

All tests are in `crates/gopp-mangle`: `symbol_name` for the names themselves, `sema_symbols` for the names of what the checker knows, and `long` for the long tests (`#[ignore]`).

- `symbol_name::corpus_round_trips`, `every_identifier_round_trips`, `all_code_points_round_trip`: a generated corpus over every kind (6000 entities, depth 4, with repeated subtrees) and the nastiest names (Unicode, punctuation, invalid UTF-8, NUL, empty names and paths, every byte, every code point), checked against an independent oracle for the readable text.
- `symbol_name::same_name_whatever_the_build_order`: the same entities built in 12 shuffled orders, in contexts with different amounts of unrelated nodes, give the same bytes. `sema_symbols::names_do_not_depend_on_checking_order_or_threads`: packages (generic types and methods, `@Operator` overloads, `@Extends`) checked in 8 shuffled orders, on 1 and 16 threads, give the same set of names, byte for byte.
- `sema_symbols::one_name_exactly_when_the_checker_says_identical` (SYM-2): every pair of a table of type spellings, with the checker's `identical` as the oracle; `struct_identity_has_every_field_mark` for the marks of a field; `every_method_has_its_own_name` for `@Operator` overloads, `@Right` and the methods of instances; `what_has_no_symbol_says_why` for generic code, local types, constraints, constants and what is named `_` (refused with a reason, never one shared name); `package_paths_with_dots_slashes_and_unicode`, `symbol_name::boundaries_between_paths_and_names_never_blur`.
- `sema_symbols::readable_name_is_the_type_printers_text` and `spelling_differs_only_for_one_type_written_two_ways` (SYM-3).
- `symbol_name::never_collides_with_a_c_name`, `tags_are_prefix_free_and_in_the_alphabet` (SYM-4).
- `symbol_name::only_what_the_mangler_writes_is_a_name` (a table of texts that are no names, each with the offset where it is wrong, beside the names that they are near), `a_type_is_referred_to_from_eight_characters_on`, `every_prefix_of_a_valid_name_is_rejected`, `mutated_names_never_crash`, `random_text_never_crashes`, `hostile_shapes`, the fuzz target `symbol_name` with the names of A.7 as its seeds ([fuzzing.md](../design/fuzzing.md)) and `long::a_million_and_a_half_mutated_names` (SYM-5, SYM-6).
- `symbol_name::exponential_sharing_stays_small`, `deep_and_wide_names_work`, `long_names_take_linear_steps` and `sema_symbols::symbol_of_takes_linear_steps` (five shapes of checker type: a deep chain of every constructor, func types, a wide struct, wide tuples, shared halves) count steps. The long ones measure time: `long::hostile_text_costs_linear_time` at 16000 and 256000 in size and `long::symbol_of_costs_linear_time` at 10000 and 160000 must not take more than 6 times the linear time, `long::a_quadratic_walk_is_told_from_a_linear_one` shows that the bound tells the two apart, and `long::a_name_a_million_levels_deep_round_trips`.
