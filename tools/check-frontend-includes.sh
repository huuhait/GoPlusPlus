#!/usr/bin/env bash
# D0.13: the front-end libraries (Basic, Lex, Parse, AST, Format, Sema) and
# Mangle (symbol names, which CodeGen calls) may use LLVM ADT and Support
# headers, but never LLVM IR, Clang or LLD headers. That keeps their rebuilds
# fast: only CodeGen and Import include the heavy headers.
#
# Usage: tools/check-frontend-includes.sh [repo-root]
set -u
root=${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
status=0
for lib in Basic Lex Parse AST Format Sema Mangle; do
  for dir in "$root/compiler/lib/$lib" "$root/compiler/include/gopp/$lib"; do
    [ -d "$dir" ] || continue
    hits=$(grep -rnE --include='*.h' --include='*.cpp' --include='*.inc' \
        '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](clang|lld|llvm)/' "$dir" |
      grep -vE '#[[:space:]]*include[[:space:]]*[<"]llvm/(ADT|Support)/')
    if [ -n "$hits" ]; then
      echo "$hits"
      status=1
    fi
  done
done
if [ $status -ne 0 ]; then
  echo "error: front-end libraries may include only llvm/ADT and llvm/Support headers (D0.13)" >&2
fi
exit $status
