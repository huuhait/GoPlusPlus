# ADR-0026: No small-string optimization in `string`

- **Status:** accepted
- **Date:** 2026-10-29
- **Plan reference:** decides P15 Q1 / P01 Q8 (task P15.14); spec SLC-5 and D4.5 (string layout); P05 runtime note.

## Context

Short strings are common. A C++ `std::string` keeps short text inside the string value (small-string optimization, SSO), so building a short string does not allocate. The layout of `string` is part of the ABI, so this had to be decided before v1.0 (P15 Q1, from P01 Q8).

In Go++ a `string` is immutable (`{ptr, len, backing}`, [ADR-0032](0032-shared-buffers-and-borrow-engine.md)), slicing `s[i:j]` shares the reference-counted backing buffer in $O(1)$, and a full NUL-terminated string or literal passes to a C `const char*` without copying (FOR-6). String literals already cost nothing: they point into read-only data with `backing == nil`.

On 2026-10-29 the owner decided: **no SSO**.

## Decision

1. `string` has **no small-string optimization**, in v1.0 and in its ABI. The 3-word string header `{ptr, len, backing}` points to text that lives elsewhere: in read-only data for a literal (`backing == nil`), or in a reference-counted heap buffer `{refcount, byte_cap}` (plus a trailing NUL byte) for a dynamically built string (SLC-5).
2. No later version may change this layout without a new ADR, because it is ABI.

```text
A (chosen): the text lives elsewhere
  string  ptr     ──► "Hi Bob\0" (heap buffer, or read-only data for a literal)
          len     = 6
          backing ──► {refcount, byte_cap} (nil for static literals / "")

B (rejected): the text lives inside the value
  string  [ 'H' 'i' ' ' 'B' 'o' 'b' ... ]   (a short string, no pointer)
```

## Consequences

- Every string read is the same code: load `ptr` and `len`. There is no "inline or heap?" branch.
- `s[i:j]` always shares the memory of `s` in $O(1)$, and moving or copying the 3-word string header never moves the text bytes.
- A NUL-terminated heap string or literal passes to C with no copy (FOR-6); a non-NUL-terminated substring or `ffi.String` makes a temporary NUL-terminated copy at the C call boundary.
- A short built string (for example `"Hi " + name`) costs one heap allocation. If benchmarks show it matters in a hot path, use a `SmallString` library type, a pooled buffer, or a `mem.Arena`.

## Alternatives

- **B: SSO inside `string`, like C++. Rejected.**
  - Every string read pays an "inline or heap?" branch.
  - `s[i:j]` could no longer always share memory in $O(1)$: an inline string has no stable address when the value moves.
  - Inline strings complicate C interop pointer rules.
- **C: A, plus a separate opt-in `SmallString` library type. Not now.** Can be added later as an ordinary library type without changing the language.
- **Go and Rust** both use option A.
