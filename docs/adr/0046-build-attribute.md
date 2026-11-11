# ADR-0046: A build condition is the attribute `@Build`

- **Status:** accepted (owner decision)
- **Date:** 2026-11-11
- **Plan reference:** P02.14, P06 (package loader), P10 (`tags` in `gopp.mod`); replaces the build comment of D24 in [ADR-0006](0006-p01-15-owner-decisions.md) and of point 6 in [ADR-0007](0007-build-profiles.md); changes LEX-11, FOR-10, MAC-3, MAC-11; adds FOR-11; related [ADR-0037](0037-v1-targets.md)

## Context

Code that talks to the operating system exists once per system, and a build must take one version. Go says which with a comment above the package clause, `//go:build linux`, and Go++ had copied that form.

A comment is not checked. `//go:biuld linux` and `// go:build linux` are ordinary comments, so the file is built for every system and the error shows up somewhere else. `//go:build linx` is a valid line that never holds, so the file is silently built for no system. A comment also works on a whole file only: one function that differs needs two more files.

Go++ already has attributes (`@Extends`, `@Inline`), and the compiler checks their names and arguments. Rust uses an attribute for the same job (`#[cfg(...)]`), on a file or on one item.

## Decision

1. **A build condition is written `@Build(condition)`.** The condition is made of words, `&&`, `||`, `!` and parentheses: `@Build(linux && !arm64)`.
2. **Where it stands:**
   - above the `package` clause: the condition is for the whole file;
   - on a top-level declaration: a function, a method, a type, a variable, a constant or an import. On a group (`var ( … )`, `import ( … )`) it is for every declaration of the group.

   It does not stand on a struct field, on a statement or on a macro declaration. A file or a declaration has at most one `@Build`.
3. **What a condition leaves out is parsed and nothing more.** It must be correct syntax, and `gopp fmt` formats it. Its names are not looked up, it is not type-checked, and an import that is left out is not loaded. So two declarations may have the same name when their conditions never hold together.
4. **The words are known words.** A word is the name of an operating system or of an architecture, `unix`, the name of a build profile of the main module, or a tag that the module of the file declares in its `gopp.mod` (`tags fast notzdata`; `-tags=fast` turns one on). **Any other word is a compile error.**
5. **The file name rule of Go stays.** A file whose name ends in `_linux`, `_amd64` or `_linux_amd64` before the extension is for that system only, and `_test` marks a test file. The names are those of Go, and `wasm32`.
6. **A `.go` file keeps Go's form:** a `//go:build` line (or `// +build` lines) in its header, read as Go 1.25 reads them. An unknown word in such a line does not hold and is no error, as in Go.
7. **In a `.gopp` file, Go's `//go:build` comment above the package clause is a compile error** that shows the attribute to write. Without it, a Go programmer's habit would give a file that is built for every system.
8. **`@If(cond)` stays as it is** (MAC-15). `@If` takes any constant `bool` and its declaration is still type-checked; `@Build` takes words only and what it leaves out is not checked. Code that cannot compile on another system needs `@Build`.

## Consequences

- A wrong attribute name or a wrong word is found by the compiler, on every system.
- A function that differs per system can stay next to the code that uses it, with its import:

  ```go
  @Build(windows)
  import "<windows.h>"

  @Build(windows)
  func pageSize() int { return 65536 }

  @Build(!windows)
  func pageSize() int { return 4096 }
  ```
- **Code under a condition that does not hold can rot.** A build for Linux does not check the Windows version of a function. Both targets of version 1 are built and tested for every change (ADR-0037), which finds it.
- Every file of a package is parsed for every build, also the ones the build leaves out. Only a `.go` file is decided from its header alone.
- When a name is not found and a declaration of that name was left out by `@Build`, the error says so and shows the condition. To do in the checker (P03).
- `gopp.mod` gains the `tags` line (P10). The tag `notzdata` of [ADR-0015](0015-time-zone-data.md) is declared in the standard library's `gopp.mod`.
- The formatter writes the `@Build` of a file above the package clause and its comment, with one blank line after it.

## Alternatives

- **The comment, as in Go:** known to every Go programmer, but silent on a mistake and for whole files only.
- **Rust's spelling `all(…)`, `any(…)`, `not(…)`:** longer, and Go's `//go:build` already uses `&&`, `||` and `!`.
- **Also on struct fields and statements,** as Rust allows: a type whose layout differs per system and code that disappears inside a function are harder to read and to format. They can be added later without breaking a program.
- **Leaving a file out without parsing it:** an excluded file could then hold anything, and the formatter and the editor could not work on it.
