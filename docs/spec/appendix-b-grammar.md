# Appendix B. Grammar

The whole syntax of Go++ in one place: the grammar of the [Go specification](https://go.dev/ref/spec) with the Go++ changes written in. A production marked `/* Go++ */` is new or changed; the chapter named beside it defines its meaning and stays the place where it is defined ([2.3](02-lexical.md#23-where-the-grammar-is-defined)). Everything unmarked is Go's. Code: `crates/gopp-syntax`.

Notation is Go's EBNF: `|` alternatives, `[ ]` optional, `{ }` repeated, `( )` grouping, `"x"` a token.

## B.1 Tokens

Lexical elements are Go's ([chapter 2](02-lexical.md)): identifiers, the literals `int_lit`, `float_lit`, `imaginary_lit`, `rune_lit` and `string_lit`, Go's operators and punctuation, and Go's semicolon insertion.

```
keyword           = /* the 25 Go keywords */ | "macro" .        /* Go++ LEX-2 */
operator_or_punct = /* Go tokens */ | "#" | "@" .               /* Go++ LEX-3 */
```

`base` is an identifier. It appears in no production: it gets its meaning from the checker ([LEX-12](02-lexical.md#lex-12)). Inheritance has no production either: a parent is named by the attribute `@Extends` ([TYP-16](03-types.md#35-inheritance-adr-0023)).

## B.2 Source files

```
SourceFile    = { Attribute [ ";" ] } PackageClause ";" { ImportDecl ";" } { TopLevelDecl ";" } .   /* Go++ FOR-11: `@Build` only */
PackageClause = "package" PackageName .
PackageName   = identifier .

ImportDecl    = "import" ( ImportSpec | "(" { ImportSpec ";" } ")" ) .
ImportSpec    = [ "." | PackageName ] ImportPath .
ImportPath    = string_lit .          /* a package path, a header in a module, or "<header>": chapter 14 */

TopLevelDecl   = Declaration | FunctionDecl | MethodDecl
               | MacroDecl | ProcMacroDecl | MacroInvocation | AttributedDecl .     /* Go++ 13.1 */
AttributedDecl = Attribute { [ ";" ] Attribute } [ ";" ] ( Declaration | FunctionDecl | MethodDecl | ImportDecl ) .   /* Go++ 13.1; on an import: `@Build` only */
Declaration    = ConstDecl | TypeDecl | VarDecl .
```

## B.3 Declarations

```
ConstDecl      = "const" ( ConstSpec | "(" { ConstSpec ";" } ")" ) .
ConstSpec      = IdentifierList [ [ Type ] "=" ExpressionList ] .
VarDecl        = "var" ( VarSpec | "(" { VarSpec ";" } ")" ) .
VarSpec        = IdentifierList ( Type [ "=" ExpressionList ] | "=" ExpressionList ) .
IdentifierList = identifier { "," identifier } .
ExpressionList = Expression { "," Expression } .

TypeDecl    = "type" ( TypeSpec | "(" { TypeSpec ";" } ")" ) .
TypeSpec    = AliasDecl | TypeDef .
AliasDecl   = identifier [ TypeParameters ] "=" Type .
TypeDef     = identifier [ TypeParameters ] Type .
TypeList    = Type { "," Type } .

TypeParameters = "[" TypeParamList [ "," ] "]" .
TypeParamList  = TypeParamDecl { "," TypeParamDecl } .
TypeParamDecl  = IdentifierList TypeConstraint .
TypeConstraint = TypeElem .

FunctionDecl = "func" FunctionName [ TypeParameters ] [ "!" ] Signature [ FunctionBody ] .          /* Go++ ERR-1 */
MethodDecl   = "func" Receiver MethodName [ TypeParameters ] [ "!" ] Signature [ FunctionBody ] .   /* Go++ ERR-2, GEN-6 */
FunctionName = identifier .
MethodName   = identifier .
Receiver     = Parameters .
FunctionBody = Block .

Signature      = Parameters [ Result ] .
Result         = Parameters | Type .
Parameters     = "(" [ ParameterList [ "," ] ] ")" .
ParameterList  = ParameterDecl { "," ParameterDecl } .
ParameterDecl  = [ IdentifierList ] [ "..." ] Type .
```

## B.4 Types

```
Type      = TypeName [ TypeArgs ] | TypeLit | "(" Type ")" .
TypeName  = identifier | QualifiedIdent .
TypeArgs  = "[" TypeList [ "," ] "]" .
TypeLit   = ArrayType | StructType | PointerType | FunctionType | InterfaceType
          | SliceType | MapType | ChannelType .

ArrayType   = "[" ArrayLength "]" ElementType .
ArrayLength = Expression .
ElementType = Type .
SliceType   = "[" "]" ElementType .
PointerType = "*" BaseType .
BaseType    = Type .
MapType     = "map" "[" KeyType "]" ElementType .
KeyType     = Type .
ChannelType = ( "chan" | "chan" "<-" | "<-" "chan" ) ElementType .

StructType      = "struct" "{" { FieldDecl ";" } "}" .
FieldDecl       = IdentifierList Type [ FieldAttributes ] [ Tag ] .                  /* Go++ 3.1: no EmbeddedField, TYP-25 */
FieldAttributes = Attribute { Attribute } .                                         /* Go++ MAC-12 */
Tag             = string_lit .

FunctionType = "func" [ "!" ] Signature .                                           /* Go++ ERR-2 */

InterfaceType  = "interface" "{" { InterfaceElem ";" } "}" .
InterfaceElem  = MethodElem | TypeElem .
MethodElem     = MethodName [ "!" ] Signature .                                     /* Go++ ERR-2 */
TypeElem       = TypeTerm { "|" TypeTerm } .
TypeTerm       = Type | UnderlyingType .
UnderlyingType = "~" Type .
```

## B.5 Expressions

```
Expression = UnaryExpr | Expression binary_op Expression .
UnaryExpr  = PrimaryExpr | unary_op UnaryExpr .

binary_op  = "||" | "&&" | rel_op | add_op | mul_op .
rel_op     = "==" | "!=" | "<" | "<=" | ">" | ">=" .
add_op     = "+" | "-" | "|" | "^" .
mul_op     = "*" | "/" | "%" | "<<" | ">>" | "&" | "&^" .
unary_op   = "+" | "-" | "!" | "^" | "*" | "&" | "<-" .

PrimaryExpr = Operand
            | Conversion
            | MethodExpr
            | PrimaryExpr Selector
            | PrimaryExpr Index
            | PrimaryExpr Slice
            | PrimaryExpr TypeAssertion
            | PrimaryExpr Arguments
            | PrimaryExpr "!" Arguments .                                           /* Go++ ERR-5: the unwrapping call */

Selector      = "." identifier .
Index         = "[" Expression [ "," ] "]" .
Slice         = "[" [ Expression ] ":" [ Expression ] "]"
              | "[" [ Expression ] ":" Expression ":" Expression "]" .
TypeAssertion = "." "(" Type ")" .
Arguments     = "(" [ ( ExpressionList | Type [ "," ExpressionList ] ) [ "..." ] [ "," ] ] ")" .

Operand     = Literal | OperandName [ TypeArgs ] | "(" Expression ")"
            | MacroInvocation .                                                     /* Go++ MAC-2 */
OperandName = identifier | QualifiedIdent .
QualifiedIdent = PackageName "." identifier .
Conversion  = Type "(" Expression [ "," ] ")" .
MethodExpr  = ReceiverType "." MethodName .
ReceiverType = Type .

Literal     = BasicLit | CompositeLit | FunctionLit .
BasicLit    = int_lit | float_lit | imaginary_lit | rune_lit | string_lit .
FunctionLit = "func" [ "!" ] Signature FunctionBody .                               /* Go++ ERR-2 */

CompositeLit = LiteralType LiteralValue .
LiteralType  = StructType | ArrayType | "[" "..." "]" ElementType | SliceType | MapType
             | TypeName [ TypeArgs ] .
LiteralValue = "{" [ ElementList [ "," ] ] "}" .
ElementList  = KeyedElement { "," KeyedElement } .
KeyedElement = [ Key ":" ] Element .
Key          = FieldName | Expression | LiteralValue .
FieldName    = identifier .
Element      = Expression | LiteralValue .
```

Operator precedence is Go's ([EXP-28](06-expressions.md)): 5 `*  /  %  <<  >>  &  &^`, 4 `+  -  |  ^`, 3 `==  !=  <  <=  >  >=`, 2 `&&`, 1 `||`. Binary operators of one precedence associate left to right.

## B.6 Statements

```
Block         = "{" StatementList "}" .
StatementList = { Statement ";" } .

Statement = Declaration | LabeledStmt | SimpleStmt
          | GoStmt | ReturnStmt | BreakStmt | ContinueStmt | GotoStmt
          | FallthroughStmt | Block | IfStmt | SwitchStmt | SelectStmt | ForStmt
          | DeferStmt .

SimpleStmt     = EmptyStmt | ExpressionStmt | SendStmt | IncDecStmt | Assignment | ShortVarDecl .
EmptyStmt      = .
ExpressionStmt = Expression .                 /* a macro invocation in statement position is one of these, 13.1 */
SendStmt       = Channel "<-" Expression .
Channel        = Expression .
IncDecStmt     = Expression ( "++" | "--" ) .
Assignment     = ExpressionList assign_op ExpressionList .
assign_op      = [ add_op | mul_op ] "=" .
ShortVarDecl   = IdentifierList ":=" ExpressionList .

LabeledStmt     = Label ":" Statement .
Label           = identifier .
GoStmt          = "go" Expression .
DeferStmt       = "defer" Expression .
ReturnStmt      = "return" [ ExpressionList ] .
BreakStmt       = "break" [ Label ] .
ContinueStmt    = "continue" [ Label ] .
GotoStmt        = "goto" Label .
FallthroughStmt = "fallthrough" .

IfStmt = "if" [ SimpleStmt ";" ] Expression Block [ "else" ( IfStmt | Block ) ] .

SwitchStmt     = ExprSwitchStmt | TypeSwitchStmt .
ExprSwitchStmt = "switch" [ SimpleStmt ";" ] [ Expression ] "{" { ExprCaseClause } "}" .
ExprCaseClause = ExprSwitchCase ":" StatementList .
ExprSwitchCase = "case" ExpressionList | "default" .
TypeSwitchStmt  = "switch" [ SimpleStmt ";" ] TypeSwitchGuard "{" { TypeCaseClause } "}" .
TypeSwitchGuard = [ identifier ":=" ] PrimaryExpr "." "(" "type" ")" .
TypeCaseClause  = TypeSwitchCase ":" StatementList .
TypeSwitchCase  = "case" TypeList | "default" .

SelectStmt = "select" "{" { CommClause } "}" .
CommClause = CommCase ":" StatementList .
CommCase   = "case" ( SendStmt | RecvStmt ) | "default" .
RecvStmt   = [ ExpressionList "=" | IdentifierList ":=" ] RecvExpr .
RecvExpr   = Expression .

ForStmt     = "for" [ Condition | ForClause | RangeClause ] Block .
Condition   = Expression .
ForClause   = [ InitStmt ] ";" [ Condition ] ";" [ PostStmt ] .
InitStmt    = SimpleStmt .
PostStmt    = SimpleStmt .
RangeClause = [ ExpressionList "=" | RangeVars ":=" ] "range" Expression .           /* Go++ 8.3 */
RangeVars   = RangeVar [ "," RangeVar ] .                                           /* Go++ 8.3 */
RangeVar    = [ "&" ] identifier .                                                  /* Go++ RNG-1a */
```

## B.7 Macros and attributes

```
MacroDecl      = "macro" MacroName "(" [ MacroParamList ] ")" Block .               /* Go++ MAC-1: declarative */
ProcMacroDecl  = "macro" "func" MacroName [ "!" ] Signature FunctionBody .          /* Go++ MAC-1: procedural */
MacroParamList = MacroParam { "," MacroParam } .
MacroParam     = identifier ( Kind | "..." Kind ) .
Kind           = "expr" | "stmt" | "block" | "type" | "ident" | "lit" | "decl" | "tokens" .

MacroName      = identifier .                 /* first letter upper case, LEX-10 */
MacroRef       = [ PackageName "." ] MacroName .

MacroInvocation = "#" MacroRef "(" [ MacroArgs ] ")" .                              /* Go++ MAC-2 */
Attribute       = "@" MacroRef [ "(" [ MacroArgs ] ")" ] .                          /* Go++ MAC-3, MAC-8 */
MacroArgs       = ExpressionList .            /* a `block` argument is a function literal, MAC-13 */
```

The fragment kinds are identifiers to the lexer; they are names only in a `MacroParam`.

## B.8 What the parser decides beyond the productions

1. **`!` after an operand** ([LEX-5](02-lexical.md)). After a `PrimaryExpr`, a `!` followed by `(` is the unwrapping mark of `PrimaryExpr "!" Arguments`. A unary `!` can only start an operand, so the two never compete. `!=` is one token.
2. **Inheritance.** A parent is named by the attribute `@Extends(Parent)` before a type declaration ([TYP-16](03-types.md#35-inheritance-adr-0023)). The parser reads it like any attribute: the argument is an expression, and the checker reads it as a type.
3. **Semicolons after attributes** ([LEX-6](02-lexical.md)). An attribute line ends in an identifier or `)`, so the lexer inserts a `;` after it. `AttributedDecl` accepts that one `;` after each attribute.
4. **Where attributes go** ([13.1](13-macros-surface.md)). Before a top-level declaration, and in a struct field after the field's type and before its tag. An attribute before a statement or before an import is an error.
5. **Macro invocations** ([MAC-2](13-macros-surface.md#132-rules)). `#Name(args)` is an operand, so in statement position it is an `ExpressionStmt`, and at top level it is a declaration. Where a type is expected, `#` is a parse error.
6. **Range variables** ([8.3](08-slices-and-range.md#83-range-loops-d121)). `&` is accepted only before an identifier of a `:=` range clause. Which of the variables may carry it is a rule of the checker.
7. **Type argument lists and index lists.** `a[i, j]` is parsed as one node with a list; the checker decides whether it is an instantiation or an `Index` operator call ([ADR-0010](../adr/0010-cpp-interop-open-questions.md)). `f[]()` is a parse error.
8. **Composite literals and blocks.** As in Go: in the header of an `if`, `for` or `switch`, a `{` after a type name starts the block, not a composite literal, unless the literal is inside parentheses.
9. **Function bodies.** A `FunctionDecl` or `MethodDecl` may have no body; whether that is allowed (`@Extern`, `@Abstract`) is a rule of the checker.
10. **`base`** ([TYP-20](03-types.md#35-inheritance-adr-0023)). `base.M(…)` and `base[Parent].M(…)` are ordinary selector and index expressions here; their meaning is the checker's.
