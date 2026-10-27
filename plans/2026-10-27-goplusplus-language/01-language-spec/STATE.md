# P01 Language specification v0: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[~] in_progress`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P01) · Design: [README.md](README.md)

## Tasks

- [x] P01.01 Spec skeleton
- [x] P01.02 Lexical deltas
- [x] P01.03 Ownership chapter
- [x] P01.04 Parameter passing
- [x] P01.05 `!` and errors chapter
- [x] P01.06 panic/defer/recover
- [x] P01.07 Generics deltas
- [x] P01.08 Concurrency semantics
- [x] P01.09 Closures chapter
- [x] P01.10 Macro & attribute surface
- [x] P01.11 Foreign import surface
- [x] P01.12 Close open questions Q4, Q5, Q6
- [x] P01.13 Ten sample programs
- [x] P01.14 UB & implementation-defined list
- [~] P01.15 Spec review (owner review of the `Open (P01 review)` notes; ADR-0003/0004 update). Decisions so far: see "P01.15 owner decisions" below.
- [x] P01.16 Slices and range chapter

## Open questions

- [x] Q1 Closure capture rule (decided: D1.11)
- [x] Q2 Map element access with owning values (decided: D1.4 rule 3a)
- [x] Q3 Owning pointers: `Unique[T]`/`Shared[T]`/`Weak[T]` (decided: D1.7)
- [x] Q4 Allocators: every owned slice and map stores its allocator (decided: D1.22)
- [x] Q5 Generic methods: allowed (decided: D1.10)
- [x] Q6 Interface inline storage: one word (decided: D1.12)
- [x] Q7 Range over owning elements / consuming range (decided: D1.21)
- [x] Q8 Small-string optimization (deferred to P15, P15 Q1)

## Exit criteria

- [x] Every delta D1.2–D1.21 has grammar, semantics, an example and a rationale in `docs/spec/`
- [x] Ten samples written with expected output and compared against Go versions
- [x] Q1–Q8 decided or explicitly deferred
- [x] UB / implementation-defined list written

## Decision changes

- None. Plan-silent points are written as the reading most consistent with the plan and marked `> **Open (P01 review):**` in `docs/spec/`, for the owner to confirm or change in P01.15.

## P01.15 owner decisions

Decided by the owner on 2026-10-27. **Not yet applied** to `docs/spec/`, this phase's README or the ADRs. They are applied together when the review of all Open notes is finished. "Note #N" refers to the numbered list in the review page (the Open notes in spec order).

| # | Decision | Example | Changes |
|---|---|---|---|
| D1 | Variadic parameters of an owning type must be written `own ...T`; a plain `...T` with an owning `T` is a compile error. The caller writes `x.Clone()` to keep its value. | `func Join(parts own ...string) string` · `filepath.Join(dir.Clone(), "x")` | confirms `05-parameters.md:20` |
| D2 | Inserting a new key into a map while a `range` over that map runs **always panics**, not only when the table must grow. Visible inserts stay a compile error. | `for k := range m { addKey(m) }` → panic every time | P05 D5.4, D1.21 wording; needs an ADR |
| D3 | Growing a slice while a `range` over it runs, where the compiler can't see it, stays undefined behavior; `gopp -asan` finds it. | `for _, v := range w.queue { w.push(10) }` | confirms `08-slices-and-range.md:109` (UB-2e) |
| D4 | Every clonable type has an `x.Clone()` method, built-in types too (`string`, `[]T`, `map`, `Unique[T]`, …). `clone(x)` stays as a short form that calls it. `move(x)` stays a built-in. A type whose `Clone` method has another signature is not clonable (compile error). | `s2 := s.Clone()` · `u := User{Name: name.Clone()}` | D1.4 rule 6, D1.10 `clonable`; ADR-0003 |
| D5 | The keyword `view` is renamed **`ref`**, and the attribute `@View` is renamed **`@Ref`**. `ref` is special only where a type is expected, so `ref := "main"` still works as a variable name. | `func firstWord(s string) ref string` · `type Token struct { Text ref string }` | D1.2, D1.6, every chapter; ADR-0003 |
| D6 | In a type switch or assertion, `case string:` also matches a `ref string` (and `case []T:` a `ref []T`). The bound value is a borrow either way. `case ref string:` is still allowed when the difference matters. | `switch x := v.(type) { case string: use(x) }` | D1.12 ("`case string:` doesn't match a `view string`") changes; ADR-0003 |
| D7 | New form `for _, &d := range xs`: `d` is the element itself, so writes change the collection. It works for slices, arrays and map values (`for k, &v := range m`). The key can't use `&`. Growing the collection inside the loop stays a compile error. The plain form `for _, d := range xs` stays read-only for owning elements, and its error message suggests `&d` first. `m[k].f = x` outside a loop stays illegal, as in Go. | `for _, &d := range docs { d.Title = "x" }` · `for k, &v := range m { v.Count++ }` | D1.21, RNG-1/OWN-R3b, EBNF for `RangeClause`; ADR-0003 |
| D8 | Notes #1–#5 accepted as written (with D5/D6 applied): view/string comparison compares contents; type assertions borrow; type-switch rule per D6; re-pointing a borrowed `string`/`[]T` range variable; range variables over owning elements are read-only (like Rust `for d in &docs`). | | `03-types.md:37`, `:101`, `:140`; `04-ownership.md:141`, `:166` |

| D9 | Every pointer kind has an `IsNil()` method: raw `*T`, `Unique[T]`, `Shared[T]` and `Weak[T]`. `p == nil` keeps working, as in Go. If `T` has its own `IsNil` method, the pointer's `IsNil` wins; call `T`'s as `(*p).IsNil()`. For `Weak[T]`, `IsNil()` is true when there is no target (never set, or already freed). | `if u.IsNil() { … }` · `if u == nil { … }` | `04-ownership.md:373` (note #9); D1.7 |
| D10 | Notes #6, #7, #8 and #10 accepted as written (with D4 applied): moving a variable a closure or pending `defer` still uses is an error; `m[k] = append(m[k], v)` works in place; `Clone` works on simple types; map keys are cloned only when a new entry is added. | | `04-ownership.md:195`, `:211`, `:241`, `:422` |

| D11 | The `own` parameter marker is renamed **`move`** and goes **before the parameter name**: `func (l *List) Push(move s string)`, `func (move b Builder) Finish() string`, `func Join(move parts ...string)` (updates D1). Parameters without it still borrow (D1.5 unchanged), so calls look like Go. Passing an owned local to a `move` parameter moves it automatically; a field, element or map value needs `move(x)` or `x.Clone()` (rule 3a unchanged). `&` and `*` keep their Go meanings. A Rust-style default (plain = take, `ref`/`&` = borrow) was discussed and rejected: it puts a mark on most functions and reuses Go's signs. | `list.Push(name)` · `list.Push(name.Clone())` · `list.Push(move(u.Name))` | D1.2 keywords (`own` → `move`), D1.4, D1.5, D1.7 example, every chapter; ADR-0003 |

| D12 | Notes #11–#15 accepted as written (with D11 applied): `move` only on parameters, receivers and function types; methods with a `move` receiver are not in interface method sets (you can't use up a value through a shared interface box); `move`/`clone` are not function values; assignment builds the new value, then frees the old one, and `_ = f()` frees the result at once; re-slicing past the length panics (the items were already freed). | `b.Finish()` ✅ · `var f Finisher = b` ❌ · `s = s[:0]; s = s[:2]` → panic | `05-parameters.md:18`, `:63`; `06-expressions.md:27`; `07-statements.md:11`; `08-slices-and-range.md:47` |

| D13 | Notes #16–#20 accepted (with D7 applied): a plain range over trivial elements gives copies (`&x` changes the real item); `range move(xs)` stopped early frees the unreached items in order; writing the current item and then reading the borrowed loop variable is a compile error; deleting the current map entry during `range` keeps `k`/`v` valid to the end of the turn. **Change to #19:** `move(v)` on a plain (read-only) map range variable stays an error (fix-its: `&v` or `v.Clone()`). Through `&v` it is allowed: it takes the value out and leaves the zero value in the map, and the key stays. Same for slices: `for _, &d := range xs { keep(move(d)) }` leaves the zero value in `xs[i]`. | `for k, &v := range m { keep(move(v)) }` → `m[k] == ""` | `08-slices-and-range.md:79`, `:83`, `:97`, `:101`, `:103` |

| D14 | #21 accepted: a range over an array of owning elements is not copied (cheaper; arrays of trivial elements are still copied, like Go). **#22 decided now instead of in P08:** a range over a C++ container works like a slice: `for _, x := range vec` is a read-only borrow of each element (no C++ copy constructor), and `for _, &x := range vec` changes the real element (like C++ `auto&`). | `for _, &x := range vec { x += 1 }` | `08-slices-and-range.md:121`, `:123`; P08 D8.6 |

| D15 | #24 accepted: `x!()` follows the innermost function; if that function can return an error, the error is returned, otherwise it panics. **#25 changed:** a `!` function already adds the `error` result, so it can't also declare `error` as its last result: `func check!() error` is a compile error ("`!` already adds the error; remove it"). `func check!()` returns only an error. To return an error as a normal value, use the Go form `func check() (error, error)`. This removes the `return e` ambiguity. | `func load!() []byte` → `([]byte, error)` · `func check!() error` ❌ | `09-errors-and-panics.md:35`, `:42`; D1.8 |

| D16 | #23 changed so range-over-func follows the slice-loop rules: `for x := range seq` borrows each value read-only (`x.Clone()` to keep it); `for &x := range seq` changes the real item, and is allowed only when the iterator yields the real items as pointers (`iter.Seq[*Doc]`), otherwise it's a compile error. Calling `yield` after the loop has ended panics, as in Go. | `for &d := range docs.All() { d.Title = "x" }` with `func (l *DocList) All() iter.Seq[*Doc]` | `08-slices-and-range.md:125` |

| D17 | **#26 changed to "like Go":** `defer x.M()` inside a loop is legal. The loop-body variable is kept alive (moved into the pending deferred call) until the function ends, so the deferred calls run at function exit exactly as in Go. Other `defer` argument rules stay as written. #29 and #30 accepted (captured variables live as long as the closure; a closure stored in an outer variable keeps its captures alive). | `for _, n := range files { f := os.Open!(n); defer f.Close() }` | `09-errors-and-panics.md:171`; `11-closures.md:15`, `:26`; PAN-3, CLO-3b |
| D18 | (Form replaced by D19.) The `clonable` constraint becomes a normal std interface, **`Clone[T]`** (like Rust's `Clone` trait): `type Clone[T any] interface { Clone() T }`, used as `func Dup[T Clone[T]](x T) T`. Every clonable type satisfies it, numbers included (D4). The name for `trivial` is still open (see #27/#28 discussion). | `Dup("hi")`, `Dup(5)` | D1.10, `10-generics.md:19`, `:21` |

| D19 | **Replaces D18's form.** `Clone` and `Copy` are built-in constraints, written like Go's `comparable`: `[T Clone]` means `T` has `Clone() T` (every clonable type, numbers included); `[T Copy]` means `T` copies for free, like numbers (the old `trivial`). No `[T Clone[T]]`; the short `[Clone[T]]` form was rejected because it isn't Go syntax (`go vet`: "missing type parameter name or invalid array length"). #27 accepted: they embed in constraint interfaces like any Go interface, and Go's own rule keeps them out of `\|` unions. #28 accepted: numbers satisfy `Clone`. | `func Dup[T Clone](x T) T` · `func Sum[T Copy](xs []T) T` | D1.2 (`trivial`, `clonable` names), D1.10, `10-generics.md:19`, `:21` |

| D20 | #31 accepted: a closure that runs later (`go`, stored, returned) can't capture a borrowed loop variable. Fix-its never reuse the name: `go fetch(u.Clone())`, `mine := u.Clone()`, or `range move(users)`. **New rule:** declaring a variable with the same name as a loop variable inside that loop's body (`u := u`, `u := u.Clone()`) is a compile error, not just a vet warning. Old Go code that used `u := u` (before Go 1.22) must rename. | `for _, u := range users { u := u.Clone() }` ❌ | `11-closures.md:30`; new rule in statements/range chapter |

Still to review: notes #32–#42 (next: #32–#35).

## Notes / blockers

- The spec is in `docs/spec/` (16 chapters + README, 216 numbered rules). The samples are in `docs/spec/examples/`. Each Go counterpart runs and matches `expected_output.txt` byte for byte.
- The `.gopp` samples can't be compiled until P02/P03 exist. They were checked by hand against the rules.
- P01.15 is waiting on the owner. The spec has 47 `Open (P01 review)` notes; the reviewers flagged the variadic `...T` note (`05-parameters.md:20`) as the most important. More owner notes are in the ledger.
- Team-build ledger: `.claude/team/p01-language-spec-ledger.md` (58 findings over 20 rounds, all closed; 5 fresh final reviewers).
