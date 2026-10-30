# ADR-0026: No small-string optimization in `string`

- **Status:** accepted
- **Date:** 2026-10-29
- **Plan reference:** decides P15 Q1 / P01 Q8 (task P15.14); spec SLC-5 and D4.5 (string layout); P05 runtime note.

## Context

Short strings are common. A C++ `std::string` keeps short text inside the string value (small-string optimization, SSO), so building a short string does not allocate. The layout of `string` is part of the ABI, so this had to be decided before v1.0 (P15 Q1, from P01 Q8).

In Go++ a string is read-only, slicing shares memory (`s[i:j]`), `View[string]` is `(ptr, len)`, and an owned string or a literal passes to a C `const char*` as it is, because it is kept NUL-terminated (FOR-5). String literals already cost nothing: they point into read-only data.

On 2026-10-29 the owner decided: **no SSO**.

## Decision

1. `string` has **no small-string optimization**, in v1.0 and in its ABI. The string header points to the text, which lives elsewhere: in read-only data for a literal, on the heap for a built string. The header holds the pointer and the length (an owned string also keeps its capacity word, SLC-5; a view or a slice `s[i:j]` is exactly `(ptr, len)`).
2. No later version may change this layout without a new ADR, because it is ABI.

```text
A (chosen): the text lives elsewhere
  string  ptr ──► "Hi Bob"      (heap, or read-only data for a literal)
          len = 6

B (rejected): the text lives inside the value
  string  [ 'H' 'i' ' ' 'B' 'o' 'b' ... ]   (a short string, no pointer)
```

## Consequences

- Every string use is the same code: load the pointer and the length. There is no "inline or heap?" branch.
- `s[i:j]` always shares the memory of `s`. The pointer stays valid while `s` is alive, and a move of the string value does not move the text.
- An owned string and a literal pass to C with no copy (FOR-5). A `View[string]` or a substring still needs the NUL-terminated copy; SSO would not change that.
- A short built string (for example `"Hi " + name`) costs one heap allocation. This is the price. If benchmarks show it matters, use the library path (alternative C), a pooled allocator, or a `mem.Arena`.

## Alternatives

- **B: SSO inside `string`, like C++. Rejected.**
  - Every string use pays an "inline or heap?" branch.
  - `s[i:j]` would no longer always share memory: an inline string has no stable address, because the text moves with the value.
  - `View[string]` gets more complex, and an inline string would need its own pointer rule for C.
  - It would slow every string use just to save allocations for short built strings.
- **C: A, plus a separate opt-in `SmallString` library type. Not now.** It can be added later as a library type if benchmarks show that short-string allocations matter. No language change is needed for that, so nothing is decided against it.
- **Go and Rust** both use option A.
