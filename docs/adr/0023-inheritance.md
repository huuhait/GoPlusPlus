# ADR-0023: Inheritance with `@Extends`; Go struct embedding removed

- **Status:** accepted (integrated with [ADR-0016](0016-required-fields.md), [ADR-0021](0021-borrow-checking.md), [ADR-0022](0022-race-free-concurrency.md) and [ADR-0032](0032-shared-buffers-and-borrow-engine.md))
- **Date:** 2026-10-28 (updated for several parents without diamonds; syntax updated 2026-11-11)
- **Status note (2026-11-11, [ADR-0034](0034-extends-attribute.md)):** parents are named with the attribute `@Extends(Parent)`, one per parent. This text is written in that syntax. The earlier keyword forms and why they were dropped are in ADR-0034.
- **Plan reference:** replaces the non-goal "no class inheritance" (spec 1.7); spec section 3.5 (TYP-16 to TYP-26, TYP-28), TYP-10, TYP-15, 6.9 (EXP-13 to EXP-15), OWN-R2, OWN-R3, OWN-R6, GEN-2, GEN-12, MAC-8, LEX-2 and the grammar in chapters 2, 3, 10 and 13; tasks P03.22, P04.22, P08.16.

## Context

Go has no inheritance. It has struct embedding, which copies the names of the inner type into the outer type ("promotion"). Embedding is not inheritance: a method of the inner type that calls another method of the inner type never sees the outer type's version.

```go
type Payment struct{ Amount int64 }
func (p *Payment) Fee() int64   { return 0 }
func (p *Payment) Total() int64 { return p.Amount + p.Fee() }   // always Payment.Fee

type Card struct{ Payment }
func (c *Card) Fee() int64 { return c.Amount * 3 / 100 }
// Card{...}.Total() calls Payment.Fee, not Card.Fee
```

Real types also frequently combine an OOP base with reusable stateful struct capabilities (a `Card` with the parents `Payment`, `Auditable` and `Timestamps`). Interfaces or traits cannot hold fields, while C++'s unrestricted multiple inheritance suffers from diamond inheritance (`virtual public` base classes) and silent name collisions.

Go++ therefore provides explicit struct inheritance with `@Extends(P)`, one attribute per parent, explicit method marks (`@Virtual`, `@Abstract`, `@Override`), disambiguated `base` / `base[Parent]` calls, **no diamond inheritance**, and `offset_to_top` type tables that keep `Unique[T]`, `Shared[T]` and `Weak[T]` at a single pointer word (8 bytes). Go struct embedding is removed from `.gopp` files so there is one clear way to inherit struct fields and methods.

## Decision

1. **Syntax (TYP-16, [ADR-0034](0034-extends-attribute.md)).** A type names each parent with the attribute `@Extends(Parent)` before `type Child struct { fields }`, one attribute per parent. The order of the attributes is the order of the parents `P1`, `P2`, ….
   - Each parent must be a struct type (possibly generic: `@Extends(A[T]) @Extends(Log[T]) type B[T any] struct { … }`), except that at most **one** C++ class may be a parent, as the **first** `@Extends` (`TYP-28`, decision 14). A child of a child is fine.
   - **No cycles, no duplicate parents, no diamonds.** Naming the same parent twice, two instantiations of the same generic parent (`Box[int]` and `Box[string]`), a parent that is already an ancestor of another parent (`Payment` and `Card`), or two parents that share any common ancestor (`B` and `C` with the parent `Entity`, as parents of `D`) is a compile error. Every ancestor appears at most once in a type's hierarchy.
   - `extends` is not a keyword; it is an ordinary identifier.
   - `type X Y` where `Y` has parents, is a tagged type or is an abstract type is an error: write `@Extends(Y)` before `type X struct{}`.
   - A parent that is a non-struct type, an interface or a pointer is an error.

2. **What the child gets and clash rules (TYP-17).** The child has all parents' fields and methods.
   - A composite literal must use field names (a positional literal is an error). It may set ancestor fields directly (`Card{Amount: 100, By: "alice", CreatedAt: now, Number: "4242"}`) or through a parent part (`Card{Payment: Payment{Amount: 100}, Timestamps: Timestamps{CreatedAt: now}, Number: "4242"}`). Setting the same field both ways is an error.
   - **Field clashes:** a field name may not clash with any ancestor's field or method, and two parents may not declare a field (or field and method) of the same name.
   - **Method clashes:**
     - Between child and ancestor: a same-named method on the child requires `@Override` (`TYP-18`).
     - Across multiple parents: if two parents have a method of the same name and either is **final** or their signatures differ, it is a compile error. If all clashing parent versions are `@Virtual` or `@Abstract` with the **exact same signature**, the child **must** declare an `@Override` method of that signature to resolve the clash (unless all clashing versions are `@Abstract` and the child itself is marked `@Abstract`). One `@Override` on the child overrides every matching ancestor slot.
   - `@Required` and never-nil rules of all parents' fields apply in the child (`TYP-15`); if any parent has no zero value, the child has no zero value (`OWN-R9`).

3. **Method marks (TYP-18; attributes, MAC-8).** On parent methods: no mark = **final**; `@Virtual` = may be replaced; `@Abstract` = no body, must be replaced. On child methods, `@Override` replaces a `@Virtual` or `@Abstract` method of one or more ancestors and is **required** for that. Compile errors:
   - a same-named method without `@Override`;
   - `@Override` with nothing to override (for example a typo);
   - overriding a final method;
   - a signature that does not match exactly;
   - `@Virtual`, `@Abstract` or `@Override` on a value receiver;
   - these marks on functions that are not methods;
   - a body on an `@Abstract` method.

   An `@Override` method stays replaceable by a further child.

4. **Abstract types (TYP-19).** A type with an `@Abstract` method that is not overridden on its path (or a type marked `@Abstract`) is **abstract**.
   - It has no standalone values: `var x T`, `T{…}`, `make([]T, n)`, `NewUnique(T{…})`, `Option[T]`, or holding `T` by value in a variable, field, slice/map/array/channel element, parameter or result is a compile error. The one exception is a parent part inside a child's composite literal (`Card{Payment: Payment{…}}`).
   - It is usable through pointers (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`) to a concrete child.
   - A child that leaves an `@Abstract` method unimplemented is an error unless the child is itself marked `@Abstract` (`@Abstract @Extends(Animal) type Bird struct{}`).

5. **`base.M(…)` and `base[Parent].M(…)` (TYP-20).**
   - Inside a method of a child type, `base.M(…)` calls the direct parent's `M` statically (and `base.f` accesses its field `f`) when **exactly one** direct parent has `M` (or `f`). If more than one direct parent defines `M`, unqualified `base.M(…)` is an ambiguity compile error naming the parents.
   - `base[Parent].M(…)` (and `base[Parent].f`), where `Parent` names one of the child's direct parents, as written in its `@Extends` attributes (for example `base[Payment].Validate()`, `base[Box[T]].Reset()`), selects that direct parent statically.
   - Calling `base.M(…)` or `base[Parent].M(…)` when `M` is `@Abstract` in that parent is an error (no body).
   - Neither `base` nor `base[Parent]` is a standalone value (`x := base` and `&base[Payment]` are errors).
   - `base` is contextual: special only inside methods (and closures within methods) of a type with parents. Declaring a local, parameter, result or receiver named `base` there is an error with a rename hint; elsewhere `base` is an ordinary identifier.

6. **Upcast and downcast (TYP-21, EXP-13 to EXP-15).**
   - When an ancestor `P` has a type table (decisions 7 and 16), `*Child` converts implicitly to `*P` (a constant compile-time byte-offset adjustment: `0` for `P1`, `+offset(P)` for secondary parents), and the 8-byte owning pointer kinds convert too: `Unique[Child]` to `Unique[P]`, `Shared[Child]` to `Shared[P]`, and `Weak[Child]` to `Weak[P]`.
   - If `P` has **no** type table (for example a plain data/helper struct like `Timestamps`), there is no upcast of any kind (`*Child` to `*P`, `&c.P`, method expression `(*P).M(&c)`, `Unique`, `Shared`, `Weak`) and no downcast `p.(*Child)`, while all of `P`'s fields, methods and interface satisfactions still work on `Child`.
   - Downcast is a checked type assertion on a pointer (`p.(*Card)` or `c, ok := p.(*Card)`) or a pointer type switch, allowed whenever `P` has a type table and the target extends `P`. At run time it recovers the complete object pointer via `offset_to_top`, checks the dynamic type table, and adjusts to the target subobject offset.
   - Value slicing is a compile error (`var a Parent = child`, copying `*p` of a tagged type, or copying `c.Parent`).

7. **Cost and layout (TYP-22).**
   - Parent subobjects are laid out first, in `@Extends` order (`P1`, `P2`, …), followed by the child's own fields, with standard C alignment (`TYP-12`).
   - **Primary table word:** if `P1` already has a type table, `P1` sits at offset `0` and its first word is shared as the child's primary type-table pointer (no extra word added). If the child (or a secondary parent) needs a primary type table and `P1` has none, one primary type-table word is stored at offset `0` before `P1`.
   - **Secondary parents:** an untagged parent (`Timestamps`, no `@Virtual` or `@Abstract` in its own hierarchy) adds **zero** table words—only its fields. A tagged secondary parent (`Auditable`, `i >= 2`) already begins with its own type-table pointer word inside its subobject; the child initializes that word to the `(Child, Pi)` secondary type table.
   - **`offset_to_top` keeps smart pointers 8 bytes:** every type table carries a signed byte offset `offset_to_top` (`0` for standalone objects and primary bases, `-offset(Pi)` for secondary base subobjects), matching the C++ Itanium ABI:
     - Virtual calls on secondary bases use compiler-generated receiver-adjusting thunks (`recv += offset_to_top`) when overridden by the child.
     - `Unique[P]` drop, `Shared[P]` / `Weak[P]` retain and release (locating the refcount header before `top = (byte*)p + p.vptr.offset_to_top`), dynamic `Clone()`, and downcasts all recover the complete object start `top` in one addition, so `Unique[T]`, `Shared[T]` and `Weak[T]` remain single 8-byte pointers. When a `Shared[T]` strong count reaches `0` while `weak > 0`, the payload fields are dropped and zeroed while static type-table pointer words remain intact until `weak == 0` frees the block.

8. **Interfaces (TYP-23).** A child satisfies every interface its combined method set satisfies. Calls through an interface dispatch to the child's overrides (and adjust the receiver offset for inherited parent methods).

9. **Safety interplay (TYP-24).**
   - *Ownership and drop order (OWN-R2):* destruction runs child first: the child's `drop` method (if any), then the child's own fields in reverse declaration order, then the parent subobjects in **reverse** `@Extends` order (`Pn` down to `P1`, each running its `drop`, its fields in reverse, and its own parents in reverse).
   - *Clone (OWN-R6, decision 16):* auto-`Clone` covers all parent and child fields. Through `*P` or `Unique[P]` of any tagged ancestor `P`, `p.Clone()` recovers `top` via `offset_to_top`, allocates a `Unique` copy of the **entire** dynamic object, and returns `Unique[P]` pointing at its `P` subobject.
   - *Borrow checking (ADR-0021):* any upcast pointer (`*Child` to `*P1` or `*P2`) is the same borrow of the same owner.
   - *Send / Sync (ADR-0022):* a child is labeled from all its fields, all parents' fields included.
   - *Generics (GEN-12):* an interface with `@Extends(P)` is a constraint, and several `@Extends` on it require all the parents.

10. **Go mode (TYP-26).** `.go` files are unchanged (they have no attributes, so no `@Extends` and no marks; `base` is not special there, and Go struct embedding works as in Go).

11. **Field tables (TYP-10).** The field table of a type with parents lists each parent chain's fields in `@Extends` order (`P1`, then `P2`, …), followed by the child's own fields.

12. **Go struct embedding is removed in `.gopp` files (TYP-25).** An anonymous struct field (`struct{ Animal; … }`, `struct{ *Animal }`, `struct{ sync.Mutex }`) is a compile error, with a fix-it suggesting `@Extends` for inherited structs or a named field for plain composition (`Lock Mutex[State]`, `Log Logger`). Interface embedding (`interface{ io.Reader; io.Writer }`) stays.

13. **Error codes** are pinned in `codes.lock` as implemented (`E0375`–`E0400`, plus new codes assigned at implementation for diamond hierarchies and ambiguous `base.M()`).

14. **Extending a C++ class (TYP-28; task P08.16).**
    ```go
    import "<QtWidgets>"
    @Extends(QtWidgets.QPushButton)
    @Extends(Timestamps)
    type MyButton struct { clicks int }

    @Override
    func (b *MyButton) mousePressEvent(e *QtWidgets.QMouseEvent) {
        b.clicks++
        b.Touch()
        base[QtWidgets.QPushButton].mousePressEvent(e)
    }
    ```
    - A Go++ struct may have at most **one** C++ class as a parent, and it must be the **first** `@Extends` (`P1`), followed by any number of Go++ struct parents (`P2, P3, …`).
    - Keeping the C++ base first places the C++ subobject (and its C++ vtable) at offset `0`; Go++ secondary parents and the child's own fields follow after the C++ part.
    - `@Override` replaces C++ `virtual` methods (pure virtual counts as `@Abstract`; C++ `final` or non-virtual cannot be overridden). `base.M()` or `base[CppBase].M()` calls the C++ base implementation statically.
    - Drop order drops the child's `drop`, the child's fields in reverse, Go++ parents `Pn .. P2` in reverse, and finally runs `P1`'s C++ destructor.

15. **Generic constraint with a parent (GEN-12).**
    ```go
    @Extends(Animal)
    type AnimalKind interface{}

    func Feed[T AnimalKind](a *T) { a.Eat(); a.Speak() }

    @Extends(Payment)
    @Extends(Auditable)
    type AuditedPayment interface{}

    func AuditPay[T AuditedPayment](x *T) {
        x.Audit()
        _ = x.Total()
    }
    ```
    - An interface declared with `@Extends(P)` is a constraint: a type satisfies it when it is `P` or has `P` as an ancestor. Several `@Extends` on the interface require all the parents. The constraint has a name; there is no short form inside a type parameter list.
    - Because duplicate generic parent origins (`Box[int]` and `Box[string]`) and diamonds are disallowed, type argument inference in `func Feed[T BoxKind[U], U any](a *T)`, with `@Extends(Box[U]) type BoxKind[U any] interface{}`, always finds at most one `Box[…]` ancestor on `T`.

16. **`Clone` always copies the whole real value, 1:1.**
    - Untagged parents have no upcast (`*Child` to `*Config` is an error); inherited methods of an untagged parent borrow or copy only that parent part as declared by their signature.
    - Tagged parents (`@Virtual` or `@Abstract`) disallow value-slicing reads through `*p`, value receivers and `c.Parent` (`E0395`), while `p.Clone()` on `*P` or `Unique[P]` clones the full dynamic object via the type table.

## Example

```go
type Payment struct { Amount int64; ID string }

@Abstract
func (p *Payment) Pay() error

@Virtual
func (p *Payment) Fee() int64 { return 0 }

func (p *Payment) Total() int64 { return p.Amount + p.Fee() }   // final

type Auditable struct { Actor string }

@Virtual
func (a *Auditable) Audit() string { return "by " + a.Actor }

type Timestamps struct { CreatedAt int64 }                      // untagged: 0 table overhead
func (t *Timestamps) Age(now int64) int64 { return now - t.CreatedAt }

@Extends(Payment)
@Extends(Auditable)
@Extends(Timestamps)
type Card struct {
    Number string
}

@Override
func (c *Card) Pay() error {
    fmt.Println("card", c.Number, "pays", c.Total(), c.Audit())
    return nil
}

@Override
func (c *Card) Fee() int64 { return c.Amount * 3 / 100 }

@Override
func (c *Card) Audit() string {
    return base[Auditable].Audit() + " (card " + c.Number + ")"
}

func main() {
    card := NewShared(Card{
        Amount:    1000,
        ID:        "p1",
        Actor:     "alice",
        CreatedAt: 100,
        Number:    "4242",
    })
    var p Shared[Payment] = card                 // 8-byte Shared upcast to 1st parent
    var a Shared[Auditable] = card               // 8-byte Shared upcast to 2nd parent
    p.Pay()                                      // dynamic: Card.Pay -> Total() uses Card.Fee()
    fmt.Println(a.Audit())                       // dynamic: Card.Audit
    if c, ok := a.(*Card); ok {                  // checked downcast from 2nd parent
        fmt.Println("age:", c.Age(160))
    }
}
```

### What the compiler stops

```go
type Entity struct { ID string }

@Extends(Entity)
type B1 struct{}

@Extends(Entity)
type B2 struct{}

@Extends(B1)
@Extends(B2)
type Diamond struct{}             // error: diamond inheritance (Entity reached twice)

@Extends(Timestamps)
@Extends(Timestamps)
type Dup struct{}                 // error: duplicate parent Timestamps

@Extends(Box[int])
@Extends(Box[string])
type TwoGen struct{}              // error: multiple instances of generic parent Box

@Extends(Payment)
@Extends(structWithAmount)
type Clash1 struct{}              // error: field Amount clashes across parents

@Extends(int)
type X1 struct{}                  // error: parent must be a struct type

@Extends(*Payment)
type X2 struct{}                  // error: a pointer cannot be a parent

@Extends(io.Reader)
type X3 struct{}                  // error: an interface cannot be a parent

@Extends(Timestamps)
@Extends(QPushButton)
type BadCpp struct{}              // error: C++ parent must come first

@Extends(Payment, Auditable)
type Two struct{}                 // error: one parent per @Extends

@Extends(Payment)
type Alias = Card                 // error: @Extends goes on a struct or interface type declaration

// If both P1 and P2 declare @Virtual Validate() error:
@Extends(P1)
@Extends(P2)
type BadCard struct{}             // error: must @Override Validate() to resolve clash
// Inside @Override Validate() on a child of P1, P2:
// base.Validate()                                // error: ambiguous base call; write base[P1].Validate() or base[P2].Validate()
```

## Alternatives Rejected

- **Single parent only:** too restrictive in practice when combining an OOP parent (`Payment` or C++ `QPushButton`) with reusable stateful capabilities (`Auditable`, `Timestamps`).
- **Unrestricted C++ diamond inheritance (`virtual public` bases):** adds hidden virtual-base offset tables, constructor initialization anomalies, and duplicated base subobjects when `virtual` is forgotten. Banning diamonds at compile time gives several struct parents with none of C++'s traps.
- **16-byte fat smart pointers for secondary bases:** unnecessary because `offset_to_top` in every type table lets 8-byte `Unique[P]`, `Shared[P]` and `Weak[P]` locate the allocation start and refcount header in a single addition.
- **Interfaces with default methods only (Java / C# / Rust traits):** cannot carry fields (`CreatedAt`, `Actor`), forcing boilerplate field duplication across structs.
