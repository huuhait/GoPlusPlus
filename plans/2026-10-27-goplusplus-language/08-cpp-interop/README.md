# P08 — C++ interop

**Goal:** use real C++ libraries directly from Go++ (classes, RAII, templates, overloads, operators, exceptions), with zero-overhead calls that inline.
**Depends on:** P05, P07 · **Unblocks:** P11 (C++ cdeps), P15 · **Size:** XL, the largest risk in this plan · **Milestone:** M5

**Principle: Clang decides C++ semantics.** Go++ never re-implements overload resolution, template instantiation, name mangling, class layout or vtable layout. It asks Clang Sema and Clang CodeGen.

## Staging

The work ships in five stages. Each stage is tested and usable before the next one starts:
1. Free functions, POD structs, enums, overloads.
2. Classes: methods, constructors, destructors, copies, moves.
3. Templates.
4. Operators and iterators.
5. Exceptions ↔ panics.

## Decisions

**D8.1 — Import surface: each header is a package.**
- `import "<vector>"` (standard library) or a Go-style package path such as `import "github.com/fmtlib/fmt/include/fmt/format.h"` adds the header to the package's C++ import unit. The default standard is C++20. The standard is set once per module with `cxxstd c++20` in gopp.mod (P10), and headers from cdeps use the standard of their CMake target. Different standards from two cdep targets in one package are an error (P07 D7.1).
- The header becomes a package named after the file, as for C (P07 D7.3): `<vector>` → `vector`, `<algorithm>` → `algorithm`, `format.h` → `format`, `imgui.h` → `imgui`. A header whose name is a Go++ keyword or predeclared name must be renamed: `import stdmap "<map>"`, `import cppstr "<string>"`, `import stdany "<any>"`.
- **Namespaces are flattened into the package.** Every name the header declares, in any namespace, is reachable directly:
  - `std::vector` → `vector.vector[int32]`, `std::sort` → `algorithm.sort(…)`, `ImGui::Begin` → `imgui.Begin(…)`, `fmt::vformat` → `format.vformat(…)`;
  - depth is measured from the library's **root namespace**: the namespace that holds the header's own declarations (`std`, `fmt`, `ImGui`);
  - when a name exists at several depths, the shallowest wins and the others keep their namespace path: `std::pmr::vector` → `vector.pmr.vector[int32]`;
  - global C names never hide names in the root namespace: `cmath.abs(-2.5)` is `std::abs`, not C's `abs(int)`;
  - inline namespaces (libc++'s `std::__1`, fmt's `v11`) are invisible;
  - a name found in two unrelated places, such as two namespaces at the same depth, is an error that lists the candidates. Qualify it to choose: `pkg.ns.name`. The error is reported only where the short name is used;
  - **deep names** (Q4, ADR-0010): only the first name after `pkg.` is flattened, and the rest is normal member lookup. `boost::asio::ip::tcp::socket` is `asio.tcp.socket` while `tcp` is unique, and the full path `asio.ip.tcp.socket` always works;
  - the root of an umbrella header such as `<boost/asio.hpp>` is the deepest namespace that holds all of the declarations from the library's own files (`boost::asio`);
  - names in `detail`, `impl` or `internal` namespaces, and reserved names (`__x`, `_Upper`), get no short form; they are reached only by their full path.
- **Stable resolution.** Export data records the entity each name resolved to. A dependency upgrade that changes it is an error.

**D8.2 — Classes become Go++ named types.**
- Size and alignment are exactly what Clang computes.
- Fields of standard-layout types are accessible directly; other fields go through accessors.
- Methods use Go syntax: `v.push_back(1)`. `const` methods are callable on borrowed values.
- Static members are reached through the type selector: `pkg.Class.fn(...)`.

**D8.3 — Object lifetime maps onto Go++ ownership.**
- **Default construction.** `var v vector.vector[int32]` runs the default constructor. A type without one has no zero value (P01 D1.4 rule 9), so it must be initialized at its declaration.
- **Other constructors.** Use conversion-call syntax: `cppstr.string("hi")` (with `import cppstr "<string>"`), `foo.Foo(1, 2)`. Sema picks the constructor.
- **Destruction.** At scope end and during unwinding. Go++ drop glue calls the C++ destructor.
- **Copy.** `x.Clone()` (and its short form `clone(x)`) calls the copy constructor. A type whose copy constructor is deleted does not satisfy `Clone`.
- **Move.** Every Go++ move of a C++ value, implicit or `move(x)`, runs the move constructor. The source keeps the C++ "moved-from" state (not the zero value) and is destroyed at the end of its scope (C++ semantics).
- **Relocation.** Containers (slice growth, map rehash) relocate elements with the type's relocate operation (P05 D5.5):
  - `memcpy` only for types Clang reports as trivially relocatable (e.g. `[[clang::trivial_abi]]`);
  - the move constructor plus the destructor for all other types.

**D8.4 — Clang Sema resolves overloads and default arguments.**
- Go++ argument types are mapped to C++ types (P07 D7.4) and handed to Sema.
- Only these implicit conversions are allowed: exact match, qualification changes (such as adding `const`), derived → base, and the documented string and slice rules (P07 D7.7).
- Anything else is an error that shows Clang's candidates. Ambiguities are reported with Clang's candidate list as notes.
- *Why:* other C++ conversions, such as `double` → `int`, can silently pick a different overload.

**D8.5 — Templates.**
- Instantiation syntax: `vector.vector[int32]`.
- Function templates such as `algorithm.sort(...)` are deduced by Sema from the argument types.
- **Explicit template arguments** use the same brackets (Q3, ADR-0010): `memory.make_unique[Foo](1, 2)`, `tuple.get[0](t)`, `j.get[int32]()`, `type_traits.is_same_v[A, B]`. They give the first arguments, and Sema deduces the rest. With brackets only templates are candidates. In v0 a function template instance is only used in a call (`fn := algorithm.max[int32]` is an error).
- Instantiation is requested through Sema, and Clang CodeGen emits the member functions that are needed.
- Template arguments allowed in v0: C-compatible Go++ types, C++ types, and integer, `bool` and enum constants.
- In v0, a Go++ generic function may not instantiate a C++ template over its own type parameters: `vector.vector[T]` inside `func f[T any]()` is an error.
- Arbitrary owning Go++ types as template arguments come in v1.x. They need synthesized C++ shims for Go++ drop and clone.

**D8.6 — Operators and iterators.**
- Imported C++ operator overloads work with Go++ operator syntax on C++ types: `a + b`, `v[i]`, `a == b`, `*it`.
- `for _, x := range c` works on any type with `begin()`/`end()`; it is lowered to C++ iterator calls. It works like a slice loop (P01 D1.21): `x` is a read-only borrow of each element (no C++ copy constructor runs; trivial element types are copied), and `for _, &x := range c { x += 1 }` changes the real element, like C++ `auto&`.
- C++ containers work like a slice for the two-variable and `&` forms. The one-variable form `for x := range c` gives the index for indexable containers and the key for associative ones (`std::map`); for containers with neither (`std::list`) it is a compile error, so use `for _, x := range c`. For associative containers the two-variable form gives the key and the mapped value, like a Go map (`for k, v := range cppmap`; `&v` aliases the mapped value).
- Native Go++ types still have no user-defined operators (P01).

**D8.7 — References.**
- A `T&` parameter takes a Go++ `*T` (never nil, like a C++ reference; ADR-0018), or an addressable value (the `&` is inserted automatically). A C++ `T*` is `Option[*T]` (P07 D7.4).
- A `const T&` parameter takes a borrowed value; temporaries are materialized when needed.
- A `T&&` parameter takes `move(x)`.
- A returned `T&` becomes a raw `*T` (never nil); a returned `T*` becomes `Option[*T]`.

**D8.8 — Inheritance and virtual functions.**
- Derived → base pointer conversion is implicit.
- Virtual calls use normal method-call syntax; Clang emits the dispatch.
- `ffi.DynamicCast[T](p)` performs a dynamic cast.
- **Go++ types may extend one C++ class** (owner decision 2026-10-28, ADR-0023 decision 14, spec TYP-28; task P08.16): `type MyButton extends QtWidgets.QPushButton { clicks int }`. `@Override` replaces a C++ `virtual` method and `base.M()` calls the C++ implementation. A pure virtual acts as `@Abstract`; a non-virtual method cannot be overridden. The layout is the C++ object first (with its vtable), then the Go++ fields. The compiler generates a C++ subclass shim through Clang (the vtable synthesis), so C++ code calling the virtual reaches the Go++ override. Construction calls a C++ constructor (syntax decided in P08.16); destruction drops the Go++ fields, then runs the C++ destructor. C++ code holding raw pointers to the object is UB-9 / UB-10 territory.
- Not in v1: several C++ bases, virtual inheritance, a C++ class template as the parent. The conflicts with earlier rules (relocation, RTTI for downcasts, failable overrides, derived C++ classes and `T extends P`) are listed in ADR-0023 open point 5 and decided in P08.16.

**D8.9 — Exceptions are unified with panics (P04 D4.6).**
- **Panics are native C++ exceptions.** Go++ panics passing through C++ frames run C++ destructors. A C++ `catch (...)` can catch them, and frees them correctly; this is documented behavior.
- A C++ exception unwinding through Go++ frames runs Go++ destructors and defers.
- `recover()` yields an `ffi.Exception`, which offers `What()`, the type name and rethrow.
- `ffi.Try(func() T) (T, error)` converts exceptions into Go++ errors at an API boundary. Like `recover()`, it never catches forced unwinding (`pthread_exit`, thread cancellation; P01 D1.9).
- Calls to `noexcept` functions get no landing pads.
- Under `-panic=abort`, a C++ exception aborts at the first Go++ frame it reaches.

**D8.10 — C++ code goes into the package's own LLVM module.**
- Clang CodeGen generates the C++ code a package needs (inline functions, template instances, thunks) into the *same* LLVM module as the package's Go++ code.
- The optimizer can then inline `vector::size()` and `operator[]` into Go++ loops without LTO. R5 depends on this.
- Duplicates across packages are `linkonce_odr` (C++ inline semantics) and fold at link time.
- The identity and flags check of P07 D7.2 also covers inline C++ bodies, so the copies that fold are the same code (ODR).

**D8.11 — C++ standard library.**
- Default: bundled libc++ + libc++abi + libunwind, built per target and cached. This is hermetic and cross-compiles.
- `cxxstdlib libstdc++` in gopp.mod links against system C++ libraries on Linux.
- Windows uses the MSVC STL (tier 3).
- Exactly one C++ standard library per binary, enforced.
- One unwinder per binary: libunwind with libc++, libgcc_s with libstdc++.

**D8.12 — Unsupported in v0.** Each case gets a precise diagnostic and a test:
- importing C++20 modules;
- C++ coroutines;
- consteval-only APIs, e.g. `fmt::format` with compile-time format strings (use `fmt::vformat` instead);
- user-defined literals;
- variadic templates beyond what Sema can deduce from a call;
- C++ templates instantiated over a Go++ generic function's own type parameters (D8.5);
- explicitly requested partial specializations;
- `std::function` wrapping Go++ closures (v1.x);
- overload sets that need C++ syntax to disambiguate, such as `f<>()` (Go++ has no `f[]()`).

## Deliverables

- `compiler/lib/Import` C++ mode, header packages and namespace flattening.
- Lifetime mapping, the overload/template bridge, and exception unification.
- libc++ build integration.
- The C++ ABI fuzz suite for classes (P08.14).
- Samples:
  - `std::vector`, `std::string`, `std::map`, `std::unordered_map`;
  - `std::unique_ptr`, `std::shared_ptr`, `std::optional`;
  - a header-only template library;
  - fmt's runtime API, built from package-local C++ files (P07 D7.9); the CMake-built fmt is tested in P11.

## Exit criteria

- M5: the samples above (std containers, smart pointers, a header-only template library, and fmt's runtime API from package-local files) work and are ASan-clean.
- Exceptions ↔ panics work in both directions, with destructors verified on both sides.
- A release-mode loop over `std::vector<int32_t>` using `operator[]` runs within 5% of the same loop compiled by clang++ `-O2`. IR inspection shows the accessor inlined.

## Risks

- **XL complexity** → the five stages above. Each one ships and is tested before the next begins.
- **Clang API churn** → one pinned LLVM major (P00 D0.2).
- **Compile time of heavy C++ headers** → PCH files shared between packages with the same headers and flags (P07 D7.1, P06).
- **Mixing the libstdc++ and libc++ ABIs** → one stdlib and one unwinder per binary, enforced (D8.11).

## Open questions

Task P08.15 decides Q3 and Q4. Task P08.16 decides Q5.

- **Q1** (decided 2026-10-27) C++ identifiers that are Go++ keywords get a trailing underscore (`type_`, `range_`, `map_`), as for C (P07 D7.3).
- **Q2** (moved to P07 Q2; decided 2026-10-27, ADR-0008) Should `const` on methods and parameters show up in Go++ signatures? No: it stays metadata and is enforced at the boundary (only `const` methods on borrowed values; no read-only value passed where C++ may change it).
- **Q3** (decided 2026-10-27, ADR-0010) Explicit template arguments use Go's generic brackets: `memory.make_unique[Foo](1, 2)` (D8.5).
- **Q4** (decided 2026-10-27, ADR-0010) Deep names: `asio.tcp` while it is unique, and the full path `asio.ip.tcp` always works; collisions are errors on use, listing each candidate with its C++ name and location (D8.1).
- **Q5** (open, decided in task P08.16) The construction syntax of a Go++ type that extends a C++ class (ADR-0023 decision 14, D8.8): a composite literal with the C++ constructor's arguments, or a constructor call, or both; how a C++ base with several constructors is chosen; and the conflicts in ADR-0023 open point 5.
