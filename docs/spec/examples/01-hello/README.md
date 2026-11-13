# 01 hello

The smallest program.

## Spec rules exercised

- [LEX-1](../../02-lexical.md): the file is `.gopp`, UTF-8.
- [TYP-7](../../03-types.md): the string literal passes to the `...any` parameter of `fmt.Println` as a borrow. Nothing is boxed or allocated.
