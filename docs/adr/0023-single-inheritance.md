# ADR-0023: Single inheritance with `extends`; Go struct embedding removed

- **Status:** accepted; amended 2026-10-28 (a Go++ type may extend a C++ class, decision 14; generic constraint `T extends Parent`, decision 15; see Amendment); amended 2026-10-29 (`Clone` always copies the whole real value, decision 16; see Amendment 2)
- **Date:** 2026-10-28
- **Plan reference:** replaces the non-goal "no class inheritance" (spec 1.7, plan README non-goals); spec new section 3.5 (TYP-16 to TYP-26), changes TYP-10, TYP-15, TYP-15a, 6.9 (new EXP-13 to EXP-15), OWN-R2, OWN-R3, OWN-R6, GEN-2, MAC-8, LEX-2 and the grammar in chapters 2, 3 and 13; builds on ADR-0016, ADR-0019, ADR-0020, ADR-0021 and ADR-0022; new tasks P03.22 and P04.22; P14 note. Amendment: new TYP-28, GEN-12 rewritten, new task P08.16, P03.22 extended.

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
   - `extends` on a non-struct type, an interface or a pointer is an error. `extends` on a C++ class is allowed (decision 14, TYP-28).
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
6. **Upcast and downcast (TYP-21).** A `*Child` converts implicitly to a `*Parent`, or to any ancestor pointer, and to the pointer kinds: `Unique[*Child]` to `Unique[*Parent]`, and the same for `Shared` and `Weak`. (Changed by decision 16: the upcast exists only when the parent has a type table.)
   - A call through the parent pointer to a `@Virtual` or `@Abstract` method runs the child's override (dynamic dispatch). Calls to final methods are direct.
   - **Limits without a type table (see decision 7).** If the parent has no type table, an upcast gives only a borrowed `*Parent`. (Changed by decision 16: without a type table there is no upcast at all, borrowed or owning.) `Unique[*Child]` to `Unique[*Parent]`, and the same for `Shared` and `Weak`, are errors, because a parent owner could not drop the right type. A downcast `p.(*Child)` is an error, because there is no dynamic type. With a type table, all three are allowed.
   - Downcast is a checked type assertion: `p.(*Card)` and `x, ok := p.(*Card)`. It panics, or returns `ok = false`, on a mismatch.
   - Value slicing is an error: `var a Parent = child` (copying only the parent part), and passing a `Child` value where a `Parent` value is expected. Use a pointer.
7. **Cost (TYP-22).** A type gets a hidden type-table pointer only if it (or an ancestor) has `@Virtual` or `@Abstract` methods. It is one word, stored first.
   - Calls through a parent pointer to virtual methods are indirect. Every other call is direct.
   - Final-only hierarchies (no `@Virtual` or `@Abstract` method anywhere in the hierarchy) have zero overhead. They can still be extended, with the limits of decision 6: no owning-pointer upcast and no downcast. These are compile errors. (Changed by decision 16: no borrowed or `View` upcast either.)
   - The parent part is the first part of the child after the table word, if any, so an upcast, where it exists (only to a parent with a type table, decision 16), keeps the same address. Drop order is the child's `drop`, the child's fields in reverse order, then the parent part. An abstract type is not allowed as a field or element. A cycle in `extends` is an error.
8. **Interfaces (TYP-23).** A child satisfies every interface its method set satisfies, and that method set includes the inherited methods. Calls through an interface use the overrides.
9. **Safety interplay (TYP-24).**
   - *Ownership:* drop order is the child's fields first, then the parent part (OWN-R2). Moves and `move` work on the whole value as before (OWN-R3).
   - *Clone:* auto-`Clone` covers the parent's and the child's fields. An abstract type is not cloneable by value (OWN-R6). A `Clone` through a pointer copies the whole real value (decision 16).
   - *Borrow checking (ADR-0021):* an upcast pointer is the same borrow, not a new one.
   - *Views (ADR-0020):* `View[Child]` converts to `View[Parent]`, read-only. (Changed by decision 16: only when the parent has a type table.)
   - *Send / Sync (ADR-0022):* a child is labeled from all its fields, the parent's included.
   - *`@Strict`:* the required bits of the parent's fields apply in the child (TYP-15a).
   - *Generics:* the constraint `T extends Parent` is allowed (decision 15, GEN-12).
   - *C++ parents:* TYP-28 (decision 14).
10. **Go mode (TYP-26).** `.go` files are unchanged. `extends` is not Go syntax.
11. **Reflection and field tables (TYP-10).** They include the parent chain: the field table of a child lists the parent's fields first, then its own.
12. **Go struct embedding is removed in `.gopp` files (TYP-25).** An embedded field (an anonymous field: `struct{ Animal; … }`, `struct{ *Animal }`, `struct{ sync.Mutex }`) is a compile error, with a fix-it: use `extends` for the one main parent, and a named field for everything else (`Lock sync.Mutex`, `Log Logger`).
    - Promoted fields and methods go away with it. The method set of a struct no longer includes methods from embedded fields.
    - **Interface embedding stays.** `interface{ io.Reader; io.Writer }` and embedding a constraint in a constraint interface are type-set composition, not inheritance.
    - `.go` files are unchanged: struct embedding still works there.
13. **New error codes** are named at implementation (no numbers here) and get `gopp explain` texts (P03.14).
14. **A Go++ type may extend a C++ class (TYP-28; owner decision 2026-10-28; amendment).**
    ```go
    import "<QtWidgets>"
    type MyButton extends QtWidgets.QPushButton { clicks int }

    @Override
    func (b *MyButton) mousePressEvent(e *QtWidgets.QMouseEvent) { b.clicks++; base.mousePressEvent(e) }
    ```
    - A Go++ struct may `extends` **one** C++ class. If the class is upcast (a `*MyButton` becomes a `*QtWidgets.QPushButton`), it must be polymorphic or have a virtual destructor for the owning-pointer upcast. A class that is not polymorphic follows the final-only rules of decision 6 and 7: a borrowed upcast only, no owning upcast, no downcast. (Changed by decision 16: "tagged" for a C++ parent means polymorphic, with a vtable. A Go++ child of a non-polymorphic C++ class gets no upcast to it, like an untagged Go++ parent; the base's fields and methods stay available on the child.)
    - `@Override` works on the C++ class's `virtual` methods. A non-virtual C++ method cannot be overridden (error). A C++ method marked `final` cannot be overridden either.
    - `base.M()` calls the C++ class's implementation of `M`, statically (`QPushButton::mousePressEvent`).
    - C++ pure virtual methods act like `@Abstract`: the Go++ type must override them, or be marked `@Abstract` itself.
    - *Layout.* The C++ object comes first (with its vtable pointer), then the Go++ fields. The compiler generates a C++ subclass shim through Clang, so C++ code that calls the virtual method reaches the Go++ override. There is no second type-table word: the C++ vtable is the type table (TYP-22).
    - *Construction.* The type is created through a composite literal or a constructor call that calls a C++ constructor. P08 decides the exact syntax (P08.16).
    - *Destruction.* The Go++ fields are dropped first (the child's `drop`, then its fields in reverse order), then the C++ destructor runs. This is decision 9's order with the C++ part as the parent part.
    - *Safety.* On the Go++ side the type follows the Go++ rules (ownership, borrows, views, Send / Sync). C++ code that holds raw pointers to such an object is UB-9 / UB-10 territory, as for any C++ interop.
    - *Not in v1:* more than one C++ base, virtual inheritance, and a C++ class template (or an instance of one) as the parent. They are open points 3 to 5.
15. **Generic constraint `T extends Parent` (GEN-12; owner decision 2026-10-28; amendment).**
    ```go
    func Feed[T extends Animal](a *T) { a.Eat(); a.Speak() }
    ```
    - `[T extends P]` means `T` is `P` or a type that extends `P`, directly or indirectly.
    - In the body, `*T` converts to `*P` (an upcast), the methods of `P` are available on `T` (with dispatch per instance), and the fields of `P` are accessible.
    - It can be combined with other constraint elements: `interface{ extends Animal; Clone }`.
    - Instantiating with a type that does not extend `P` is an error. If `P` is final-only, the upcast limits of decision 6 still apply: a borrowed upcast only. (Changed by decision 16: with a final-only `P` there is no upcast `*T` to `*P`; the fields and methods of `P` stay available on `T`.)

16. **`Clone` always copies the whole real value, 1:1 (owner decision 2026-10-29; Amendment 2).** A clone never loses the child part. "Tagged" means the type has a type table (decision 7). For a C++ parent it means polymorphic (it has a vtable).
    - *Untagged types have no upcast.* A type without a type table can still be extended, but a `*Child` does not convert to a `*Parent`, and a `View[Child]` does not convert to a `View[Parent]` (the owning kinds already could not, decision 6). The same holds in a generic body: with `T extends P` and a final-only `P`, `*T` does not convert to `*P`, but the fields and methods of `P` stay available on `T`. The error hint: add a `@Virtual` method to the parent to use children through a parent pointer, or use `*Child`.
    - *Inherited methods of an untagged parent are allowed.* `func (p *Config) Dup() Config` is called as `my.Dup()`. Inside a parent method the receiver is the parent part, and the signature says what the method returns (here `Config`). This is a documented limit, the same as Go embedding, and not a silent loss. Outside the parent's own methods, a pointer to an untagged type points at exactly that type.
    - *Explicit forms that build a `*Config` from a child are upcasts, and are errors for an untagged parent:* `&my.Config`, `View` of `my.Config`, and the method expression `(*Config).Dup(&my)`. A method value `my.Dup` is fine. Method expressions follow the upcast rule.
    - *Tagged types clone through the table.* `p.Clone()` on a `*P` or a `View[P]` of a tagged `P` gives a `Unique[*P]` that holds a full copy of the dynamic type (read from the table), so the overrides keep working on the copy. This replaces the OWN-R6 ambiguity error for these types. `x.Clone()` on a `P` value (a variable of type `P`) gives a `P`: a value always has its real type, because value slicing is an error (decision 6).
    - *Clone and `Unique`, `Shared`, `Weak`.* `Unique[*P].Clone()` for a tagged `P` holds a full dynamic copy; this replaces OWN-R6's `(*u).Clone()` for these types. `Shared[*P].Clone()` and `Weak[*P].Clone()` retain, so nothing is lost.
    - *Untagged types keep OWN-R6.* `p.Clone()` on a raw `*T` with a value-receiver `Clone` is still the ambiguity error. Write `(*p).Clone()`.
    - *`*p` has no value use for a tagged `P`* (any pointer kind). Only field access, a method call and `&*p` are allowed. These are errors:
      - every copy: `x := *p`, an argument, a literal element, `return *p`, `ch <- *p`, `m[k] = *p`, `append(s, *p)`, `Some(*p)`, `defer f(*p)`, and ranging by value over `P` elements reached through a pointer;
      - assignment through it: `*p = v` and `*q = *p`;
      - `(*p).Clone()`.
    - *A value receiver of a tagged type is the same.* In `func (a Animal) M()`, `a` may be the parent part of a child. A copy of `a` or `a.Clone()` inside it is an error. Field access and method calls are fine.
    - *Generics and `Clone`.* `*P` does not satisfy `[T Clone]`; `Unique[*P]` does. `clone(x)` is a predeclared function and the short form of `x.Clone()`, so it follows the same rules. In a generic body, `a.Clone()` on `*T` with a tagged `T extends P` gives `Unique[*T]`. Auto-`Clone` of a struct with a `Unique[*P]` field, or a slice or map of them, goes through the table.
    - *Other ways to reach the parent.* An `unsafe` cast to `*Parent` is UB (UB-7). An interface that holds a `*Child` and is asserted to `*Parent` is an error: the dynamic type is `*Child`.
    ```go
    type Animal struct{ Name string }
    @Virtual func (a *Animal) Speak() string { return "..." }
    type Dog extends Animal { Tricks []string }
    @Override func (d *Dog) Speak() string { return "woof" }

    type Config struct{ Port int }               // untagged
    func (p *Config) Dup() Config { return *p }  // the receiver is the parent part
    type MyConfig extends Config { Debug bool }

    var p *Animal = &dog
    a := p.Clone()          // Unique[*Animal] holding a full Dog; a.Speak() == "woof"
    x := *p                 // error: could copy only the Animal part; use p.Clone()
    d := dog.Clone()        // Dog

    var q *Config = &my     // error: Config has no type table, so no upcast
    c := my.Dup()           // Config (the parent part only; the signature says so)
    r := &my.Config         // error: an upcast
    f := (*Config).Dup      // ok as a value
    g := f(&my)             // error: an upcast
    var cfg Config          // a value
    cfg2 := cfg.Clone()     // Config (exact)
    var z *Config = &cfg
    cfg3 := (*z).Clone()    // Config; z.Clone() is the OWN-R6 ambiguity error
    ```

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
- The layout puts the parent part first in the child (after the table word, if any). An upcast is free (it exists only to a parent with a type table, decision 16). This is needed for TYP-21 and is part of the layout rule TYP-22.
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

1. *(Decided 2026-10-28: decision 14.)* C++ class inheritance at the boundary.
2. *(Decided 2026-10-28: decision 15.)* Constraint `T extends Parent`.
3. **Several C++ bases and virtual inheritance** are not in v1 (decision 14). A Go++ type with one C++ base only.
4. **A C++ class template as the parent** (`extends vector.vector[int32]`, a CRTP base, or any instance) is not in v1.
5. **Conflicts of decision 14 with earlier rules, not resolved here (P08.16 decides):**
   - TYP-22 says a type table is "one word, stored first". With a C++ parent the C++ vtable pointer is first, so no extra word is stored. A new `@Virtual` or `@Abstract` method added by a Go++ type directly below a non-polymorphic C++ class would need a vtable pointer that the C++ part does not have, and the C++ object could no longer be first. Proposed v1 default: an error.
   - TYP-18 requires `@Virtual` on a replaceable method, but C++ has no such mark. A C++ `virtual` method counts as `@Virtual`, a C++ `final` method as final, and a pure virtual as `@Abstract`.
   - OWN-R9: moves of C++ values use the type's relocate operation, and `memcpy` only for trivially relocatable types. A Go++ type with a C++ base is relocatable only if the C++ part is; many classes (Qt `QObject` and friends) are not safe to move, since others may hold their address. A move of such a type is an error or needs the C++ move constructor. Not decided.
   - OWN-R9 and TYP-17: a C++ base without a default constructor gives the child no zero value.
   - Downcast (EXP-15) from a C++ pointer to a Go++ child needs C++ RTTI (`dynamic_cast`); with `-fno-rtti` it is an error. Not decided.
   - Failable overrides: a `!` on an override cannot match a C++ signature (TYP-18 requires an exact match), so such overrides cannot be failable. A panic in an override unwinds as a C++ exception through the C++ caller (P08 D8.9).
   - Field tables (TYP-10): the C++ base's fields are not listed. Auto-`Clone` and `Send` / `Sync` of the C++ part follow the C++ class rules of ADR-0022 and OWN-R9.
   - GEN-12: whether a C++ class derived (in C++) from `P` satisfies `T extends P` is not decided; v1 default: no, only Go++ `extends`.
   - Decision 16 and C++ copy constructors: a C++ copy constructor copies only the base part when called through a base reference (slicing). That is C++ territory, in the class of UB-9 and UB-10, and Go++ cannot prevent it. P08.16 decides how `Clone` of a polymorphic C++ parent works (for example through a C++ `clone()` method) and records it.

## Amendment (2026-10-28, owner decisions)

- Open point 1 (C++ class inheritance) became decision 14 and open point 2 (constraint `T extends Parent`) became decision 15. Spec: TYP-16 allows a C++ class as the parent; new TYP-28; TYP-24 and GEN-12 rewritten. Plan: P08.08 and D8.8 point to the new task P08.16; P03.22 gets the `T extends Parent` checks.
- The text in decision 1 and decision 9 (Generics) that said "a C++ type is an error" or "no `T extends Parent`" is replaced above. The rest of the ADR is unchanged.
- ADR-0020 (views): `View[Child]` converts to `View[Parent]` also when `Parent` is a C++ class (read-only; only when the parent has a type table, decision 16). ADR-0021 (borrows): an upcast, also in a generic body (`*T` to `*P`), is the same borrow. ADR-0022 (Send / Sync): the label of a type that extends a C++ class includes the C++ part. ADR-0024 (operators): a child inherits the operator methods of a C++ parent, and a `T extends P` body sees the operator methods of `P` (GEN-13).

## Amendment 2 (2026-10-29, owner decision)

- New decision 16: `Clone` always copies the whole real value. The owner compared allowing a partial clone (as in C++), an error on every clone through a pointer, a second method (`CloneFull`), a smart `Clone` that copies only the parent part of an untagged type, and a hidden type word in every struct; the owner chose "always full, 1:1" with zero cost, which needs the untagged upcast removed.
- Decisions 6, 7, 9, 14 and 15 carry a "Changed by decision 16" note; the old text is kept.
- Spec items to update with P03.22: TYP-21 (including `View`), TYP-22, TYP-24, EXP-14, EXP-15, OWN-R6 and GEN-12. Plan: P03.22 (checker), P04.22 (the clone through the table, the layout) and P08.16 (C++ parents).
- Other ADRs: ADR-0020 (views: `View[Child]` to `View[Parent]` only with a type table); ADR-0021 (an upcast is the same borrow, when it exists); ADR-0022 (no change); ADR-0024 (inherited operator methods of an untagged `P` run on the parent part, like `Dup()` in decision 16). ADR-0020, ADR-0021 and ADR-0024 got a status note.
- Alternatives rejected: partial clone through a parent pointer (silent loss of the child part and of its overrides); `CloneFull` as a second name (two names for one idea); a hidden type word in every struct (one word per value of every struct, and a different layout from C).
