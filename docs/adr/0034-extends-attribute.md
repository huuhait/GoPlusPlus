# ADR-0034: Parents are named with the attribute `@Extends`, not with an `extends` keyword

- **Status:** accepted (owner decision)
- **Date:** 2026-11-11
- **Plan reference:** P01.16 point 18, P02.05, P02.08, P03.22; spec TYP-16, GEN-12, LEX-2, LEX-12, MAC-8 and appendix B. Replaces the syntax of [ADR-0023](0023-inheritance.md) decisions 1 and 15 and the "contextual `extends`" line of [ADR-0006](0006-p01-15-owner-decisions.md); both are rewritten. Everything else in ADR-0023 stands.

## Context

Inheritance used to be written with a contextual keyword in three places:

```go
type Card extends Payment, Auditable { Number string }       // a type declaration
func Feed[T extends Animal](a *T)                            // a type parameter list
type Pet interface { extends Animal; Clone }                 // a constraint interface
```

`extends` was not reserved, so each place needed a rule that tells the keyword from a name. Writing the parser showed where that breaks down: `interface{ extends (Animal) }` reads both as the constraint with a parenthesized type and as a method named `extends`, and the spec did not say which. The type declaration form also made a struct with parents look unlike every other struct: no `struct` keyword, and the parents sat between the name and the body.

Go++ already marks everything else about a hierarchy with attributes (`@Virtual`, `@Abstract`, `@Override`).

## Decision

1. **A parent is named by the built-in attribute `@Extends(Parent)` before the type declaration.**
   ```go
   @Extends(Payment)
   @Extends(Auditable)
   @Extends(Timestamps)
   type Card struct {
       Number string
   }
   ```
2. **One parent per attribute.** Several parents are several `@Extends` attributes. Their order, top to bottom, is the parent order `P1`, `P2`, … of ADR-0023 (layout, drop order, the C++ parent first). `@Extends(A, B)` and `@Extends()` are errors.
3. **On a struct type declaration** (`type Name struct { … }`) the type inherits from the parent, with all the rules of ADR-0023.
4. **On an interface type declaration** (`type Name interface { … }`) the interface is a constraint: a type satisfies it when it is the parent or has it as an ancestor. Several `@Extends` require all the parents. Such an interface is not the type of a value.
   ```go
   @Extends(Animal)
   type AnimalKind interface{}

   func Feed[T AnimalKind](a *T) { a.Eat(); a.Speak() }
   ```
5. **The three keyword forms are removed:** `type C extends P { … }`, `[T extends P]` and the interface element `extends P`. `extends` is an ordinary identifier everywhere. `base` stays contextual (ADR-0023 decision 5).
6. **Where it is allowed.** Before a top-level declaration of one type whose type is a struct or interface type literal. It is an error on an alias, on a type defined from another type, on a parenthesized `type ( … )` group and on anything that is not a type. A type declared inside a function has no parents, because attributes do not go on statements.
7. **The argument is one type.** It may be qualified and generic, and the type parameters of the declaration are in scope in it although it is written before them: `@Extends(A[T]) type B[T any] struct { … }`.
8. **No grammar.** `@Extends` is parsed like every attribute; the checker reads its argument as a type. The productions `ExtendsDecl`, the `extends` form of `TypeParamDecl` and the `extends` form of `InterfaceElem` are gone.
9. **Error codes** keep their rows in `codes.lock` (`err_extends_*`); `err_extends_one_parent` (`E0374`) is the error for an `@Extends` that does not have exactly one argument. Codes for a misplaced `@Extends` are assigned when the checker implements them.

## Consequences

- A struct with parents is declared like any struct, and its parents are in the same place as its other marks.
- The language has no contextual keyword left for inheritance, and the `extends (` ambiguity is gone. The parser loses three special cases.
- **A constraint that asks for a parent must be a named interface.** `func Feed[T extends Animal](a *T)` becomes two declarations. This is the cost of the decision.
- **A local type cannot have parents** until attributes are allowed on statements.
- An attribute argument can name a type parameter declared after it; the checker resolves `@Extends` in the scope of the declaration's type parameters.
- ADR-0023 and the spec (TYP-16, GEN-12) are rewritten in the new syntax. This ADR is the only place that still shows the old forms, as the record of what was removed.
- To do: the parser drops the three forms (P02.05, P02.08); the checker reads `@Extends` (P03.22).

## Alternatives

- **Keep the `extends` keyword** (ADR-0023): rejected by the owner as less clear; it also needs a disambiguation rule per place.
- **`@Extends(A, B)` with several parents in one attribute:** rejected by the owner. One parent per line makes the order, which decides layout and drop order, plain to see, and keeps diffs to one line per parent.
- **Keep `[T extends P]` as a short form next to the attribute:** it would keep a contextual keyword for one place and give one idea two spellings.
