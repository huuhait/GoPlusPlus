# 14. Foreign import surface

Base: Go spec [Import declarations](https://go.dev/ref/spec#Import_declarations), [Package clause](https://go.dev/ref/spec#Package_clause), [Blank identifier](https://go.dev/ref/spec#Blank_identifier), [Qualified identifiers](https://go.dev/ref/spec#Qualified_identifiers), [Exported identifiers](https://go.dev/ref/spec#Exported_identifiers), [Comments](https://go.dev/ref/spec#Comments). Go's cgo is **not** used.

Delta D1.15. This chapter fixes only the **surface**: how you write an import of a C or C++ header, and what name you get. The semantics are in **P07** (C), **P08** (C++), **P10** (modules) and **P11** (CMake and cdeps).

## 14.1 Syntax

The grammar of `ImportSpec` does not change. The `ImportPath` string gets two new forms.

```
ImportSpec  = [ "." | PackageName ] ImportPath .          /* Go */
ImportPath  = string_lit .                                  /* Go */

/* the contents of the string decide what it is: */
GoPackagePath    = /* a Go++ package path, as in Go */ .
HeaderPackPath   = PackagePath "/" HeaderFile .             /* e.g. "github.com/madler/zlib/zlib.h" */
SystemHeaderPath = "<" HeaderFile ">" .                     /* e.g. "<stdio.h>", "<vector>" */
HeaderFile       = FilePath [ HeaderExt ] .
HeaderExt        = ".h" | ".hh" | ".hpp" | ".hxx" .
```

`<…>` headers have no path prefix. A `<header>` such as `<vector>` has no extension.

## 14.2 Rules

**FOR-1.** *Two kinds of foreign import.*
- A **package header**: the package path plus the header's path inside the package: `import "github.com/madler/zlib/zlib.h"`.
- A **system or standard-library header**: angle brackets inside the string, as in C: `import "<stdio.h>"`, `import "<vector>"`.

**FOR-2.** *Classification.* The **longest prefix** over both `require` and `cdep` paths in `gopp.mod` decides whether an import is a Go++ package or a header. The same path in both lists is an error (P10 D10.1, P11 D11.4).

**FOR-3.** *Each header is a package named after the file.* It is used like any Go package. There is no `C.` or `cpp.` prefix:

| Import | Use |
|---|---|
| `"github.com/madler/zlib/zlib.h"` | `zlib.crc32(…)` |
| `"<stdio.h>"` | `stdio.printf(…)` |
| `"<vector>"` | `vector.vector[int32]` |
| `"<algorithm>"` | `algorithm.sort(…)` |

- C and C++ names keep their spelling and are always accessible, whatever their case (`zlib.crc32` is lower case yet usable).
- A C/C++ name that is a Go++ keyword gets a **trailing underscore**: `type_`, `range_`, `map_` (P07 D7.3, P08 Q1). Example: `stdmap.map_[int32, int32]` is `std::map<int32_t, int32_t>`.
- C++ namespaces are **flattened** into the package: `std::sort` from `<algorithm>` is `algorithm.sort` (P08 D8.1 has the depth and clash rules).

**FOR-4.** *Naming an import.* If the file name is a Go++ keyword or predeclared name (`<map>`, `<string>`, `<any>`), the import **must** be named: `import stdmap "<map>"`. A file name that is not an identifier (`foo-bar.h`) must also be imported with a name. Members whose names are keywords get a trailing underscore (see FOR-3): after `import stdmap "<map>"`, the class is `stdmap.map_`.

**FOR-5.** *Order.* Header imports keep their source order. `gopp fmt` never reorders them, because C header order can matter. `import _ "<…>"` is allowed for order-only or side-effect imports.

**FOR-6.** *No conversions in human code.*
- C types are plain Go++ types (`int32`, `uint64`, `byte`, …; P07 D7.4). There is no `C.int`.
- String literals and heap-owned strings whose byte at `s.ptr + len(s)` inside `s.backing` is NUL pass to `const char*` parameters directly with zero copy; a sub-sliced `string` whose byte at `len(s)` inside `s.backing` is not NUL, or a non-owning `ffi.String` (`backing == nil` and not a static literal), passes as a **temporary NUL-terminated copy** automatically without reading past foreign memory.
- Slices pass to pointer parameters (P07 D7.7).

**FOR-7.** *Header extensions* are `.h`, `.hh`, `.hpp` and `.hxx`. This is the one list every phase uses.

**FOR-8.** *Language.*
- For package headers, C or C++ comes from the package's CMake target (P11).
- For `<…>` headers, `.h` is C and anything else (`<vector>`, `.hpp`) is C++.

**FOR-9.** *Headers inside a Go++ module* (yours or a dependency's) are header imports too. An import path inside a Go++ module that ends in a header extension names a header file, not a package (P10 D10.1). `.h` is C, the other extensions are C++. Flags come from the `cflags` and `cxxstd` directives of the gopp.mod of the module of the **importing** package, because each package has one Clang instance (P07, P08). A dependency's own `cxxstd` is a **minimum**: if the importer's is lower, the build fails with "module D needs c++23" ([FOR-10](#142-rules)).

**FOR-10.** *Directives and settings.* C and C++ settings are **not** written in source files. They are module-wide directives of `gopp.mod`, set once per project, like Go's `go.mod` (P10):
- `cxxstd c++20` (the C++ standard);
- `cflags …` (extra C/C++ compiler flags);
- `ldflags …` (extra linker flags). The `ldflags` of all modules in the build are combined for the final link, in module order (the main module last).

C/C++ dependencies (`cdep`) keep taking their flags from their own CMake target (P11). A `cdep` is always a CMake project: a library that has none is used through a `CMakeLists.txt` its user writes, in a local directory or in a repository of their own, and `replace` may point the `cdep` path there ([ADR-0038](../adr/0038-cdeps-cmake-only.md)). A library already installed on the machine is not a `cdep`: import its header with `<…>` and name it in `ldflags`. No directive comment is left in source files ([LEX-11](02-lexical.md)); which files and declarations a build takes is said with the attribute `@Build` ([FOR-11](#142-rules)).

**FOR-11.** *Build conditions* ([ADR-0046](../adr/0046-build-attribute.md)). `@Build(condition)` says when a file or a declaration is part of the build.
- **The condition** is made of words, `&&`, `||`, `!` and parentheses: `@Build(linux && !arm64)`.
- **Above the `package` clause** it is for the whole file. **On a top-level declaration** (a function, a method, a type, a variable, a constant or an import) it is for that declaration; on a group (`var ( … )`, `import ( … )`) it is for every declaration of the group. It does not stand on a struct field, a statement or a macro declaration, and a file or a declaration has at most one.
- **What a condition leaves out is parsed and nothing more.** It must be correct syntax. Its names are not looked up, it is not type-checked, and an import that is left out is not loaded. Two declarations may have the same name when their conditions never hold together.
- **A word** is the name of an operating system (`linux`, `windows`, …) or of an architecture (`amd64`, `arm64`, …), `unix`, the name of a build profile of the main module (`@Build(debug)` holds only in the profile `debug`, P06 D6.12), or a tag that the module of the file declares in its `gopp.mod` (`tags fast`; `-tags=fast` turns it on). Any other word is a compile error. A profile name may not equal an OS, architecture or tag word.
- **The file name counts too, as in Go.** A file whose name ends in `_linux`, `_amd64` or `_linux_amd64` before the extension is for that system only. `_test` marks a test file.
- **Go's `//go:build` comment above the `package` clause is a compile error** that shows the attribute to write.
- `@If` ([MAC-15](13-macros-surface.md)) is different: its declaration is still type-checked. Code that cannot compile on another system needs `@Build`.

## 14.3 Examples

`gopp.mod` of the project (the C++ standard is set once for the module):

```
module example.com/demo

gopp 0.1

cxxstd c++20
```

The source file:

```go
@Build(linux)

package main

import (
    "fmt"

    "<algorithm>"                        // C++ std header -> package `algorithm`
    "<vector>"                           // -> package `vector`
    stdmap "<map>"                       // `map` is a keyword: the import must be named
    "<deque>"                            // -> package `deque`
    _ "<locale.h>"                       // order-only / side effect
    "github.com/madler/zlib/zlib.h"      // header from a cdep -> package `zlib`
)

func main() {
    var v vector.vector[int32]           // a real std::vector<int32_t>
    v.push_back(3); v.push_back(1)
    algorithm.sort(v.begin(), v.end())   // std::sort, namespace flattened
    var d deque.deque[int32]
    d.push_back(9)
    var m stdmap.map_[int32, int32]      // keyword member name gets a trailing underscore
    _ = m
    crc := zlib.crc32(0, []byte("abc"), 3)   // slice -> pointer, no cast, no C. prefix
    fmt.Println(crc)
}
```

Compared with Go (cgo): there is no comment preamble, no `import "C"`, no `C.CString` and no `C.free`. Calls have no per-call cost.

One function that differs per system, with the header it needs ([FOR-11](#142-rules)):

```go
package mem

@Build(windows)
import "<windows.h>"

@Build(windows)
func pageSize() int {
    var info windows.SYSTEM_INFO
    windows.GetSystemInfo(&info)
    return int(info.dwPageSize)
}

@Build(!windows)
func pageSize() int { return 4096 }
```

`@Build(linx)` is a compile error: `linx` is not a known word.

## 14.4 Rationale

- Header imports look like Go imports, so tools, `go doc`-like lookup and `gopp fmt` work on them.
- The longest-prefix rule gives one answer per import with no extra syntax.
- Flattened namespaces make `algorithm.sort` read like Go. A clash is an error, never a silent pick.
- Keeping header order protects C code where includes depend on each other.
