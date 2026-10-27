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
- `import "<vector>"` (standard library) or a Go-style package path such as `import "github.com/fmtlib/fmt/include/fmt/format.h"` adds the header to the package's C++ import unit. The default standard is C++20. A package can override it with `//gopp:cxxstd`, and headers from cdeps use the standard of their CMake target. Different standards from two cdep targets in one package are an error (P07 D7.1).
- The header becomes a package named after the file, as for C (P07 D7.3): `<vector>` → `vector`, `<algorithm>` → `algorithm`, `format.h` → `format`, `imgui.h` → `imgui`. A header whose name is a Go++ keyword or predeclared name must be renamed: `import stdmap "<map>"`, `import cppstr "<string>"`, `import stdany "<any>"`.
- **Namespaces are flattened into the package.** Every name the header declares, in any namespace, is reachable directly:
  - `std::vector` → `vector.vector[int32]`, `std::sort` → `algorithm.sort(…)`, `ImGui::Begin` → `imgui.Begin(…)`, `fmt::vformat` → `format.vformat(…)`;
  - depth is measured from the library's **root namespace**: the namespace that holds the header's own declarations (`std`, `fmt`, `ImGui`);
  - when a name exists at several depths, the shallowest wins and the others keep their namespace path: `std::pmr::vector` → `vector.pmr.vector[int32]`;
  - global C names never hide names in the root namespace: `cmath.abs(-2.5)` is `std::abs`, not C's `abs(int)`;
  - inline namespaces (libc++'s `std::__1`, fmt's `v11`) are invisible;
  - a name found in two unrelated places, such as two namespaces at the same depth, is an error that lists the candidates. Qualify it to choose: `pkg.ns.name`.
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
- **Copy.** `clone(x)` calls the copy constructor. A type whose copy constructor is deleted is not `clonable`.
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
- Instantiation is requested through Sema, and Clang CodeGen emits the member functions that are needed.
- Template arguments allowed in v0: C-compatible Go++ types, C++ types, and integer constants.
- In v0, a Go++ generic function may not instantiate a C++ template over its own type parameters: `vector.vector[T]` inside `func f[T any]()` is an error.
- Arbitrary owning Go++ types as template arguments come in v1.x. They need synthesized C++ shims for Go++ drop and clone.

**D8.6 — Operators and iterators.**
- Imported C++ operator overloads work with Go++ operator syntax on C++ types: `a + b`, `v[i]`, `a == b`, `*it`.
- `for x := range c` works on any type with `begin()`/`end()`; it is lowered to C++ iterator calls.
- Native Go++ types still have no user-defined operators (P01).

**D8.7 — References.**
- A `T&` parameter takes a Go++ `*T`, or an addressable value (the `&` is inserted automatically).
- A `const T&` parameter takes a borrowed value; temporaries are materialized when needed.
- A `T&&` parameter takes `move(x)`.
- A returned `T&` becomes a raw `*T`.

**D8.8 — Inheritance and virtual functions.**
- Derived → base pointer conversion is implicit.
- Virtual calls use normal method-call syntax; Clang emits the dispatch.
- `ffi.DynamicCast[T](p)` performs a dynamic cast.
- Go++ types that subclass C++ classes or override virtuals come in v1.x, because they need vtable synthesis.

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
- overload sets that need C++ syntax to disambiguate.

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

Task P08.15 decides Q3 and Q4.

- **Q1** (decided 2026-10-27) C++ identifiers that are Go++ keywords get a trailing underscore (`type_`, `range_`, `map_`), as for C (P07 D7.3).
- **Q2** (moved to P07 Q2) Should `const` on methods and parameters show up in Go++ signatures? P07 Q2 now owns all constness questions.
- **Q3** How to spell explicit template arguments for function templates, e.g. `memory.make_unique[Foo](args)`.
- **Q4** Namespace flattening for deep hierarchies (e.g. should `boost::asio::ip::tcp` be `asio.tcp` or `asio.ip.tcp`?) and the wording of collision errors.
