# ADR-0023: Single inheritance with `extends`; Go struct embedding removed

- **Status:** accepted
- **Date:** 2026-10-28
- **Plan reference:** replaces the non-goal "no class inheritance" (spec 1.7, plan README non-goals); spec new section 3.5 (TYP-16 to TYP-26), changes TYP-10, TYP-15, TYP-15a, 6.9 (new EXP-13 to EXP-15), OWN-R2, OWN-R3, OWN-R6, GEN-2, MAC-8, LEX-2 and the grammar in chapters 2, 3 and 13; builds on ADR-0016, ADR-0019, ADR-0020, ADR-0021 and ADR-0022; new tasks P03.22 and P04.22; P14 note.

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

Programmers who want "a common part with its own methods, and children that change some of the behavior" must write interfaces and forward calls by hand. On 2026-10-28 the owner decided that Go++ gets **single inheritance** with a new explicit syntax, `extends`. The owner also decided to **remove struct embedding** from `.gopp` files, so there is one way to reuse a struct, not two that look alike.

## Decision

1. **Syntax (TYP-16).** `type Child extends Parent { fields }`.
   - The parent must be a struct type, possibly generic: `type B[T any] extends A[T] { … }`. A child of a child is fine.
   - There is **one** parent. `extends A, B` is an error.
   - `extends` is a contextual keyword: it is special only in that position (after the type name and optional type parameters, before the `{`). It is not reserved, so `extends` stays usable as a name.
   - `extends` on a non-struct type, an interface, a pointer, or a C++ type is an error. C++ class inheritance at the boundary is open point 1.
2. **What the child gets (TYP-17).** The child has the parent's fields and methods.
   - A composite literal may set parent fields directly (`Card{Amount: 100, Number: "4242"}`) or through the parent (`Card{Payment: Payment{…}}`). Setting the same field both ways is an error.
   - A composite literal of an `extends` type must use field names. A positional literal is an error.
   - Field and method names may not clash with the parent's, except through `@Override`.
   - `@Required`, `@Strict` and never-nil rules of the parent's fields apply in the child.
3. **Method marks (TYP-18; attributes, MAC-8).** On parent methods: no mark = **final**; `@Virtual` = may be replaced; `@Abstract` = no body, must be replaced. On child methods, `@Override` replaces a `@Virtual` or `@Abstract` method of an ancestor and is **required** for that. These are compile errors:
   - a same-named method without `@Override`;
   - `@Override` with nothing to override (for example a typo);
   - overriding a final method;
   - a signature that does not match exactly;
   - `@Virtual`, `@Abstract` or `@Override` on a value receiver;
   - these marks on functions that are not methods.

   An `@Abstract` method has no body; a body is an error. An `@Override` method stays replaceable: a further child may override it again (`PremiumCard` below overrides the `Fee` of `Card`). There is no `@Final` for now.
4. **Abstract types (TYP-19).** A type with an `@Abstract` method that is not overridden on its path is **abstract**.
   - It cannot be created directly: a literal, `var x T`, `make([]T, n)`, `new_unique(T{…})` and so on are errors (as a variable, field, element or argument too). The one exception is the parent part inside a child's composite literal: `Card{Payment: Payment{…}}` is allowed even when `Payment` is abstract, because that part is not a standalone value.
   - It is usable only through pointers to a concrete child.
   - A child that leaves an `@Abstract` method unimplemented is an error, unless the child is itself marked `@Abstract` (a type attribute: `@Abstract type Bird extends Animal {}`).
5. **`base.M(…)` (TYP-20).** Inside a method of a child type, `base.M()` calls the **direct parent's** version of `M`, statically. It works for any parent method. `base.M()` on a method that is `@Abstract` in the parent is an error (there is no body).
   - `base` is contextual: it has this meaning only inside methods of an `extends` type. Declaring a local named `base` there is an error with a rename hint. Elsewhere `base` is an ordinary identifier. `base.M()` inside a function literal within a child method is allowed: it captures the receiver and means the same as in the method body.
6. **Upcast and downcast (TYP-21).** A `*Child` converts implicitly to a `*Parent`, or to any ancestor pointer, and to the pointer kinds: `Unique[*Child]` to `Unique[*Parent]`, and the same for `Shared` and `Weak`.
   - A call through the parent pointer to a `@Virtual` or `@Abstract` method runs the child's override (dynamic dispatch). Calls to final methods are direct.
   - **Limits without a type table (see decision 7).** If the parent has no type table, an upcast gives only a borrowed `*Parent`. `Unique[*Child]` to `Unique[*Parent]`, and the same for `Shared` and `Weak`, are errors, because a parent owner could not drop the right type. A downcast `p.(*Child)` is an error, because there is no dynamic type. With a type table, all three are allowed.
   - Downcast is a checked type assertion: `p.(*Card)` and `x, ok := p.(*Card)`. It panics, or returns `ok = false`, on a mismatch.
   - Value slicing is an error: `var a Parent = child` (copying only the parent part), and passing a `Child` value where a `Parent` value is expected. Use a pointer.
7. **Cost (TYP-22).** A type gets a hidden type-table pointer only if it (or an ancestor) has `@Virtual` or `@Abstract` methods. It is one word, stored first.
   - Calls through a parent pointer to virtual methods are indirect. Every other call is direct.
   - Final-only hierarchies (no `@Virtual` or `@Abstract` method anywhere in the hierarchy) have zero overhead. They can still be extended, with the limits of decision 6: no owning-pointer upcast and no downcast. These are compile errors.
   - The parent part is the first part of the child after the table word, if any, so an upcast is free (a static address adjustment at most). Drop order is the child's `drop`, the child's fields in reverse order, then the parent part. An abstract type is not allowed as a field or element. A cycle in `extends` is an error.
8. **Interfaces (TYP-23).** A child satisfies every interface its method set satisfies, and that method set includes the inherited methods. Calls through an interface use the overrides.
9. **Safety interplay (TYP-24).**
   - *Ownership:* drop order is the child's fields first, then the parent part (OWN-R2). Moves and `move` work on the whole value as before (OWN-R3).
   - *Clone:* auto-`Clone` covers the parent's and the child's fields. An abstract type is not cloneable by value (OWN-R6).
   - *Borrow checking (ADR-0021):* an upcast pointer is the same borrow, not a new one.
   - *Views (ADR-0020):* `View[Child]` converts to `View[Parent]`, read-only.
   - *Send / Sync (ADR-0022):* a child is labeled from all its fields, the parent's included.
   - *`@Strict`:* the required bits of the parent's fields apply in the child (TYP-15a).
   - *Generics:* a constraint `T extends Parent` is **not** added (open point 2).
10. **Go mode (TYP-26).** `.go` files are unchanged. `extends` is not Go syntax.
11. **Reflection and field tables (TYP-10).** They include the parent chain: the field table of a child lists the parent's fields first, then its own.
12. **Go struct embedding is removed in `.gopp` files (TYP-25).** An embedded field (an anonymous field: `struct{ Animal; … }`, `struct{ *Animal }`, `struct{ sync.Mutex }`) is a compile error, with a fix-it: use `extends` for the one main parent, and a named field for everything else (`Lock sync.Mutex`, `Log Logger`).
    - Promoted fields and methods go away with it. The method set of a struct no longer includes methods from embedded fields.
    - **Interface embedding stays.** `interface{ io.Reader; io.Writer }` and embedding a constraint in a constraint interface are type-set composition, not inheritance.
    - `.go` files are unchanged: struct embedding still works there.
13. **New error codes** are named at implementation (no numbers here) and get `gopp explain` texts (P03.14).

## Example

```go
type Payment struct { Amount int64; ID string }

@Abstract
func (p *Payment) Pay() error                    // no body: every child must write it

@Virtual
func (p *Payment) Fee() int64 { return 0 }       // children may replace it

func (p *Payment) Total() int64 { return p.Amount + p.Fee() }   // final

type Card extends Payment { Number string }

@Override
func (c *Card) Pay() error { fmt.Println("card", c.Number, "pays", c.Total()); return nil }
@Override
func (c *Card) Fee() int64 { return c.Amount * 3 / 100 }

type PremiumCard extends Card {}

@Override
func (p *PremiumCard) Fee() int64 { return base.Fee() / 2 }   // the parent's (Card's) version

func main() {
    list := []Unique[*Payment]{                  // Unique[*Card] converts to Unique[*Payment]
        new_unique(Card{Amount: 1000, ID: "p1", Number: "4242"}),
        new_unique(PremiumCard{Amount: 2000, ID: "p2", Number: "1111"}),
    }
    for _, p := range list {
        p.Pay()                                  // dynamic: Card.Pay
        fmt.Println(p.ID, p.Fee())               // dynamic: Card.Fee / PremiumCard.Fee
    }
    if c, ok := list[1].(*Card); ok {            // checked downcast
        fmt.Println("is a card:", c.Number)
    }
}
```

Output:

```
card 4242 pays 1030
p1 30
card 1111 pays 2030
p2 30
is a card: 1111
```

`Total` is final and calls `p.Fee()` through a `*Payment`, so it sees the child's `Fee`. `PremiumCard.Fee` is `Card.Fee(2000) / 2 = 60 / 2 = 30`.

### What the compiler stops

```go
type A extends B, C {}                            // error: one parent only
type X extends int {}                             // error: parent must be a struct type
type X extends *Payment {}                        // error: a pointer cannot be a parent
type X extends io.Reader {}                       // error: an interface cannot be a parent
var extends = 1                                   // ok: contextual keyword

func (c *Card) Fee() int64 { return 1 }           // error: Fee is @Virtual in Payment; write @Override
func (c *Card) Total() int64 { return 1 }         // error: Total is final in Payment
@Override
func (c *Card) Fe() int64 { return 1 }            // error: @Override but nothing to override (typo)
@Override
func (c *Card) Fee() int { return 1 }             // error: signature does not match
@Virtual
func (p Payment) Name() string { return "" }      // error: marks need a pointer receiver
@Virtual
func helper() {}                                  // error: not a method
@Abstract
func (p *Payment) Void() error { return nil }     // error: an @Abstract method has no body

type Bad extends Payment {}                       // error: Pay is not implemented (Bad is not @Abstract)
@Abstract type Bird extends Payment {}            // ok: still abstract
Payment{Amount: 1}                                // error: Payment is abstract (a standalone literal)
Card{Payment: Payment{Amount: 2}, Number: "1"}     // ok: the parent part of a child literal is not a standalone value
var p Payment                                     // error: Payment is abstract
make([]Payment, 3)                                // error: Payment is abstract
new_unique(Payment{})                             // error: Payment is abstract
Card{Amount: 1, Payment: Payment{Amount: 2}}      // error: Amount set twice
type C2 extends Payment { Amount int }            // error: field name clashes with the parent
func (p *PremiumCard) Pay() error { base := 1; _ = base; return nil }   // error: `base` is special here; rename the local
@Override
func (c *Card) Pay() error { return base.Pay() }  // error: Payment.Pay is @Abstract, no body to call
var a Payment = card                              // error: value slicing; use a pointer
use(card)  // use(p Payment)                      // error: a Card value is not a Payment value
type Z struct { Payment }                         // error: struct embedding is removed; use extends or a named field
```

## Consequences

- Go++ has one inheritance feature, with explicit marks for every override. A forgotten or misspelled override is a compile error, not a silent new method.
- The layout puts the parent part first in the child (after the table word, if any). An upcast is free. This is needed for TYP-21 and is part of the layout rule TYP-22.
- Struct embedding is gone from `.gopp` files. Go code that embeds must be rewritten (`extends`, or a named field plus forwarding methods). The owner accepts this, as with `sync.Mutex` in ADR-0022. This also removes the rules that existed only for embedding: `@Option` on an embedded field (ADR-0019), "embedded fields count" in `@Strict` and `@Required` (ADR-0016, ADR-0019), and a `Clone` promoted from an embedded field (OWN-R6, GEN-2). They are rewritten for `extends`.
- Field tables, export data, `gopp doc` and the LSP must know the parent chain.
- The checker needs: the hierarchy in export data (P03.11), override checks, `base`, upcast conversions, abstract checks. The code generator needs type tables, dynamic dispatch, `base` calls, up- and downcasts and the drop order.
- ADR-0016, ADR-0019 and ADR-0022 keep their text; each has a status note.

## Alternatives

- **Keep Go embedding only (rejected).** No change to the language. But `base` would not call the child's override (the problem shown in Context), and "common part plus changed behavior" stays a hand-written forwarding job.
- **Go embedding with automatic override by copying methods (rejected).** The outer type's method of the same name would replace the inner one in calls from the inner type. The owner wanted a new, explicit design and not a second meaning for embedding.
- **Interfaces with default methods (Rust and Swift traits) (rejected).** It shares behavior but not fields, so the "common data plus behavior" case still needs a struct and forwarding.
- **`open` / `override` keywords (Kotlin, Swift) (rejected).** The owner said they look weird in Go. Attributes (`@Virtual`, `@Abstract`, `@Override`) match the existing MAC-8 surface and add no reserved word.
- **`super`, `parent`, or naming the parent type for the parent call (rejected).** The owner chose `base`.
- **Full multiple inheritance (rejected).** The diamond problem: two parents with the same ancestor, or the same method name. One parent is simple to lay out (parent part first) and to check.

## Open points

1. **C++ class inheritance** at the boundary (P08 D8.8): whether an imported C++ class can be a parent, or a Go++ type a child of a C++ class. Today `extends` on a C++ type is an error (TYP-16). P08 decides.
2. **Constraint `T extends Parent`** in generics: not added. A generic function that needs "any `T` derived from `Parent`" takes `*Parent` or an interface.
