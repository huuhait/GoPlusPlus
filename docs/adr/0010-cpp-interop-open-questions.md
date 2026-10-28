# ADR-0010: C++ interop open questions (explicit template arguments, deep namespaces)

- **Status:** accepted
- **Date:** 2026-10-27
- **Plan reference:** P08 Q3, Q4 (task P08.15); P08 D8.1, D8.5, D8.12.

## Context

P08 left two questions open about how C++ names are written in Go++:

1. Sema deduces most function template arguments from the call (`algorithm.sort(v.begin(), v.end())`), but some can't be deduced because the template parameter doesn't appear in the arguments: `std::make_unique<Foo>(1, 2)`, `std::get<0>(t)`. Go++ needs a way to write them.
2. D8.1 flattens namespaces into the header's package, but it didn't say how deep names such as `boost::asio::ip::tcp` are written, which namespace is the root of an umbrella header such as `<boost/asio.hpp>`, or what the collision error says.

The owner decided both on 2026-10-27: Q3 option A (Go generic brackets) and Q4 option A (the short form when it is unique, and the full path always works). The details below (the root rule, internal namespaces, the error wording, `bool`/enum arguments, call-only instances) are the defaults that come with those choices.

## Decision

1. **Explicit template arguments use Go's generic syntax `f[args](…)`.**
   - `memory.make_unique[Foo](1, 2)`, `tuple.get[0](t)`, `memory.static_pointer_cast[Derived](p)`.
   - The brackets give the first template arguments, and Sema deduces the rest, as in C++ (and Go's partial type argument lists). You can't skip one in the middle.
   - Allowed arguments: the D8.5 set (C-compatible Go++ types, C++ types, integer constants) plus `bool` constants and C++ enum constants. An argument must be a constant expression; an untyped constant takes the template parameter's type and must fit it (`tuple.get[-1](t)` is an error).
   - With brackets, only templates are candidates (the C++ rule): given `int parse(const char*)` and `template<class T> T parse(const char*)`, `mylib.parse(s)` calls the first and `mylib.parse[float64](s)` the second. Clang picks among the candidates (D8.4).
   - The same syntax works for method templates (`j.get[int32]()`), static member templates (`widget.Widget.create[widget.Button]()`), and variable templates (`type_traits.is_same_v[int32, int32]`, `numbers.pi_v[float32]`).
   - The parser reads `pkg.f[x](y)` as Go does. The checker decides from what `f` is whether `[x]` is an index or template arguments.
   - In v0 a function template instance is only used in a call. `fn := algorithm.max[int32]` is an error, and the hint suggests a closure.
   - `f[]()` (C++ `f<>()`, which forces the template overload) is a parse error. It counts as an overload set that needs C++ syntax to disambiguate (D8.12).
2. **Deep namespaces: the short form when it is unique, and the full path always works.**
   - The first name after `pkg.` is looked up with flattening (D8.1: the shallowest match wins). Everything after it is normal member lookup. So `asio.tcp.socket` and `asio.ip.tcp.socket` both name `boost::asio::ip::tcp::socket`, as long as `tcp` is unique at its depth.
   - The full path from the root always works, because its first name sits directly in the root namespace, and names there are never hidden.
   - **Root of an umbrella header:** the deepest namespace that holds all of the declarations coming from the library's own files (the files under the header's include root). For `<boost/asio.hpp>` this is `boost::asio`. There is no override in v0.
   - **Internal names get no short form:** names inside namespaces called `detail`, `impl` or `internal`, and names reserved by the C++ standard (`__x`, `_Upper`), are reached only by their full path.
   - **Collisions are lazy:** an ambiguous short name is an error only where it is used, so a library with internal collisions still imports.
   - **Error wording** (the code number comes from the P03 table, D3.11):

     ```
     main.gopp:5:11: error[E0xxx]: "tcp" is ambiguous in package asio
         var s asio.tcp.socket
                    ^~~
     note: candidate asio.ip.tcp (boost::asio::ip::tcp, boost/asio/ip/tcp.hpp:38)
     note: candidate asio.local.tcp (boost::asio::local::tcp, boost/asio/local/tcp.hpp:20)
     help: write the full path, e.g. asio.ip.tcp
     ```

     The stable-resolution error (D8.1: a dependency upgrade changed what a name resolves to) uses the same note style: `note: was boost::asio::ip::tcp (…)`, `note: now boost::asio::tcp (…)`.

## Consequences

- No new grammar: explicit template arguments reuse Go's index/instantiation syntax, so the parser, `gopp fmt` and the LSP need nothing new for Q3.
- The checker (P03 foreign scope, P08) must hand bracketed arguments to Sema as explicit template arguments, convert constants to the template parameter's type, and reject function template instances used as values.
- D8.5's argument set grows by `bool` and enum constants.
- The C++ importer must compute the root namespace for umbrella headers, skip internal namespaces when it builds the flat name table, and report ambiguities only on use.
- Every C++ name has a spelling that always works (the full path), so ambiguity errors always have a fix-it.

## Alternatives

- **Q3: angle brackets `make_unique<Foo>(1, 2)`.** Rejected: `a < b > (c)` already parses as comparisons, so the grammar becomes ambiguous.
- **Q3: a helper such as `ffi.Template[Foo](memory.make_unique)(1, 2)`.** Rejected: long, and a second syntax next to Go generics.
- **Q3: the type as a normal first argument, `make_unique(Foo, 1, 2)`.** Rejected: it mixes types and values in one argument list, and you can't tell which argument is the type.
- **Q4: always the full path (`asio.ip.tcp`).** Rejected: long, and it goes against D8.1's flattening.
- **Q4: always the short form only.** Rejected: there is no way out when two names collide.
