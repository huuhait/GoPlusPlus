# Contributing to Go++

This is a stub; it grows with the project.

## Workflow

- Read [CLAUDE.md](CLAUDE.md) for the working rules and commands.
- Run everything through the dev shell: `nix develop -c <command>`
  (or `direnv allow`; `.envrc` has `use flake`).
- Before you send a change: `ninja -C build check format-check` must pass.
- Decisions that change the plan need an ADR in [docs/adr/](docs/adr/)
  (copy `0000-template.md`).
- Style: LLVM coding standards. `ninja -C build format` fixes formatting.

## License and file headers

The project is licensed under Apache-2.0 WITH LLVM-exception ([LICENSE](LICENSE)).
`LICENSE` is the Apache-2.0 text and the "LLVM Exceptions" section copied
verbatim from LLVM's `llvm/LICENSE.TXT` (LLVM 23.1.0; identical in 22.1.8). We left out the banner
and the "Software from third parties" part, which only describe LLVM itself.

Every C++ source and header file starts with this header (adjust the first
line to the file name; headers add `-*- C++ -*-` and the guard follows):

```cpp
//===- path/File.cpp - Short description ----------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
```

Go++ (`.gopp`) sources in `std/` and `runtime/` use the same four lines with
`//` comments. Lit tests, CMake files, and generated files do not need a header.
