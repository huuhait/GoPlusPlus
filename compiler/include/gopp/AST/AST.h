//===- gopp/AST/AST.h - AST nodes -------------------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The syntax tree of Go++ v0, shaped like Go's go/ast (D2.2).
//   - Nodes live in an ASTContext arena (one per file) and are trivially
//     destructible. The parser fills them in; everything else sees only
//     `const` nodes, so the tree is immutable after parsing.
//   - Every node has a half-open source range [getBegin(), getEnd()).
//   - Types are expressions (as in go/ast); `Unique[T]` is an IndexExpr.
//   - Semantics (types, resolutions) live in side tables owned by Sema.
//   - A node with an error in it is a Bad* node, or has null children where
//     the grammar needs one; consumers must tolerate that.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_AST_AST_H
#define GOPP_AST_AST_H

#include "gopp/Basic/SourceManager.h"
#include "gopp/Basic/Symbol.h"
#include "gopp/Lex/Token.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Casting.h"

namespace gopp {
namespace ast {

class Node {
public:
  enum class Kind : uint8_t {
#define EXPR(X) X,
#define STMT(X) X,
#define DECL(X) X,
#define SPEC(X) X,
#define MISC(X) X,
#include "gopp/AST/NodeKinds.def"
    NumKinds,
    // Group boundaries (inclusive), for classof.
    FirstExpr = BadExpr,
    LastExpr = RefType,
    FirstStmt = BadStmt,
    LastStmt = RangeStmt,
    FirstDecl = BadDecl,
    LastDecl = MacroCallDecl,
    FirstSpec = ImportSpec,
    LastSpec = TypeSpec,
  };

  Kind getKind() const { return K; }
  const char *getKindName() const;
  SourceLoc getBegin() const { return Begin; }
  SourceLoc getEnd() const { return End; }
  SourceRange getRange() const { return SourceRange(Begin, End); }

  SourceLoc Begin, End;

protected:
  explicit Node(Kind K) : K(K) {}

private:
  Kind K;
};

class Expr : public Node {
public:
  static bool classof(const Node *N) {
    return N->getKind() >= Kind::FirstExpr && N->getKind() <= Kind::LastExpr;
  }

protected:
  explicit Expr(Kind K) : Node(K) {}
};

class Stmt : public Node {
public:
  static bool classof(const Node *N) {
    return N->getKind() >= Kind::FirstStmt && N->getKind() <= Kind::LastStmt;
  }

protected:
  explicit Stmt(Kind K) : Node(K) {}
};

class Decl : public Node {
public:
  static bool classof(const Node *N) {
    return N->getKind() >= Kind::FirstDecl && N->getKind() <= Kind::LastDecl;
  }

protected:
  explicit Decl(Kind K) : Node(K) {}
};

class Spec : public Node {
public:
  static bool classof(const Node *N) {
    return N->getKind() >= Kind::FirstSpec && N->getKind() <= Kind::LastSpec;
  }

protected:
  explicit Spec(Kind K) : Node(K) {}
};

#define GOPP_NODE(Name, Base)                                                  \
  Name() : Base(Kind::Name) {}                                                 \
  static bool classof(const Node *N) { return N->getKind() == Kind::Name; }

/// Forward declarations of the classes the others refer to.
class Ident;
class BasicLit;
class FuncType;
class BlockStmt;
class FieldList;
class Attribute;
class MacroInvocation;

//===----------------------------------------------------------------------===//
// Expressions and types
//===----------------------------------------------------------------------===//

/// A syntax error in an expression; the range covers the skipped tokens.
class BadExpr : public Expr {
public:
  GOPP_NODE(BadExpr, Expr)
};

class Ident : public Expr {
public:
  GOPP_NODE(Ident, Expr)
  Symbol Name;
};

/// int, float, imaginary, rune and string literals. Text is the raw source
/// text (it points into the source buffer and still contains escapes).
class BasicLit : public Expr {
public:
  GOPP_NODE(BasicLit, Expr)
  tok::Kind LitKind = tok::int_lit;
  llvm::StringRef Text;
};

/// `Type{Elts...}`; Type is null for an elided type inside another literal.
class CompositeLit : public Expr {
public:
  GOPP_NODE(CompositeLit, Expr)
  Expr *Type = nullptr;
  SourceLoc Lbrace, Rbrace;
  llvm::ArrayRef<Expr *> Elts;
};

class KeyValueExpr : public Expr {
public:
  GOPP_NODE(KeyValueExpr, Expr)
  Expr *Key = nullptr;
  SourceLoc Colon;
  Expr *Value = nullptr;
};

class FuncLit : public Expr {
public:
  GOPP_NODE(FuncLit, Expr)
  FuncType *Type = nullptr;
  BlockStmt *Body = nullptr;
};

class ParenExpr : public Expr {
public:
  GOPP_NODE(ParenExpr, Expr)
  SourceLoc Lparen, Rparen;
  Expr *X = nullptr;
};

class SelectorExpr : public Expr {
public:
  GOPP_NODE(SelectorExpr, Expr)
  Expr *X = nullptr;
  Ident *Sel = nullptr;
};

/// `X[i]`, and generic instantiation `X[T1, T2]`.
class IndexExpr : public Expr {
public:
  GOPP_NODE(IndexExpr, Expr)
  Expr *X = nullptr;
  SourceLoc Lbrack, Rbrack;
  llvm::ArrayRef<Expr *> Indices;
};

class SliceExpr : public Expr {
public:
  GOPP_NODE(SliceExpr, Expr)
  Expr *X = nullptr;
  SourceLoc Lbrack, Rbrack;
  Expr *Lo = nullptr, *Hi = nullptr, *Max = nullptr;
  bool Slice3 = false;
};

/// `X.(T)`; Type is null for `X.(type)` in a type switch.
class TypeAssertExpr : public Expr {
public:
  GOPP_NODE(TypeAssertExpr, Expr)
  Expr *X = nullptr;
  Expr *Type = nullptr;
};

/// `Fun(Args)`, `Fun(Args...)`, and the unwrapping call `Fun!(Args)` (Must).
class CallExpr : public Expr {
public:
  GOPP_NODE(CallExpr, Expr)
  Expr *Fun = nullptr;
  SourceLoc Lparen, Rparen;
  SourceLoc EllipsisLoc; // valid if the last argument is followed by `...`
  SourceLoc BangLoc;     // valid for `f!(...)`
  llvm::ArrayRef<Expr *> Args;
  bool isMust() const { return BangLoc.isValid(); }
};

/// `*X`: a dereference or a pointer type.
class StarExpr : public Expr {
public:
  GOPP_NODE(StarExpr, Expr)
  Expr *X = nullptr;
};

/// `+x -x !x ^x &x <-x ~x`
class UnaryExpr : public Expr {
public:
  GOPP_NODE(UnaryExpr, Expr)
  SourceLoc OpLoc;
  tok::Kind Op = tok::add;
  Expr *X = nullptr;
};

class BinaryExpr : public Expr {
public:
  GOPP_NODE(BinaryExpr, Expr)
  Expr *X = nullptr;
  SourceLoc OpLoc;
  tok::Kind Op = tok::add;
  Expr *Y = nullptr;
};

/// `#Name(tokens)` or `#pkg.Name(tokens)`. The arguments are a balanced token
/// tree, kept flat: every token between the parentheses, including nested
/// brackets (which are balanced) and automatic semicolons. P09 interprets it.
class MacroInvocation : public Expr {
public:
  GOPP_NODE(MacroInvocation, Expr)
  SourceLoc HashLoc;
  Ident *Pkg = nullptr; // null if unqualified
  Ident *Name = nullptr;
  SourceLoc Lparen, Rparen;
  llvm::ArrayRef<Token> Args;
};

/// `...T` in a parameter list (Elt = T) or the `...` of `[...]T` (Elt = null).
class Ellipsis : public Expr {
public:
  GOPP_NODE(Ellipsis, Expr)
  Expr *Elt = nullptr;
};

/// `[Len]Elt`; Len may be an Ellipsis node for `[...]T`.
class ArrayType : public Expr {
public:
  GOPP_NODE(ArrayType, Expr)
  SourceLoc Lbrack;
  Expr *Len = nullptr;
  Expr *Elt = nullptr;
};

class SliceType : public Expr {
public:
  GOPP_NODE(SliceType, Expr)
  SourceLoc Lbrack;
  Expr *Elt = nullptr;
};

class StructType : public Expr {
public:
  GOPP_NODE(StructType, Expr)
  SourceLoc StructLoc;
  FieldList *Fields = nullptr;
};

/// A function type or the signature of a declaration. `Fails` is the `!` mark
/// (`func!(int) T`, `func f!()`): the function can fail.
class FuncType : public Expr {
public:
  GOPP_NODE(FuncType, Expr)
  SourceLoc
      FuncLoc; // the "func" keyword; invalid for an interface method signature
  SourceLoc BangLoc; // valid if Fails
  FieldList *TypeParams = nullptr;
  FieldList *Params = nullptr;
  FieldList *Results = nullptr;
  bool Fails = false;
};

/// Each Field of Methods is a method (one name, a FuncType), an embedded type
/// (no names) or a type-set element (`~int | string` as Unary/Binary exprs).
class InterfaceType : public Expr {
public:
  GOPP_NODE(InterfaceType, Expr)
  SourceLoc InterfaceLoc;
  FieldList *Methods = nullptr;
};

class MapType : public Expr {
public:
  GOPP_NODE(MapType, Expr)
  SourceLoc MapLoc;
  Expr *Key = nullptr;
  Expr *Value = nullptr;
};

class ChanType : public Expr {
public:
  enum Dir : uint8_t { Send = 1, Recv = 2, Both = 3 };
  GOPP_NODE(ChanType, Expr)
  SourceLoc ArrowLoc; // valid if the type has an arrow
  Dir Direction = Both;
  Expr *Value = nullptr;
};

/// `ref string` or `ref []T`. Elem is the Ident `string` or a SliceType.
class RefType : public Expr {
public:
  GOPP_NODE(RefType, Expr)
  SourceLoc RefLoc;
  Expr *Elem = nullptr;
};

//===----------------------------------------------------------------------===//
// Fields, attributes, macro parameters
//===----------------------------------------------------------------------===//

/// `@Name` / `@Name(tokens)`: an attribute or attached macro. Args has the
/// same shape as MacroInvocation::Args.
class Attribute : public Node {
public:
  GOPP_NODE(Attribute, Node)
  SourceLoc AtLoc;
  Ident *Pkg = nullptr;
  Ident *Name = nullptr;
  bool HasArgs = false;
  SourceLoc Lparen, Rparen;
  llvm::ArrayRef<Token> Args;
};

/// One entry of a parameter, result, type-parameter, struct-field or
/// interface-element list.
///   - parameter: Names (maybe empty), Type, MoveLoc valid for `move p T`;
///   - struct field: Names or an embedded Type, Attrs, Tag;
///   - interface element: see InterfaceType.
class Field : public Node {
public:
  GOPP_NODE(Field, Node)
  llvm::ArrayRef<Ident *> Names;
  Expr *Type = nullptr;
  llvm::ArrayRef<Attribute *> Attrs; // after the type, before the tag
  BasicLit *Tag = nullptr;
  SourceLoc MoveLoc;
  bool isMove() const { return MoveLoc.isValid(); }
};

/// A list in (), [] or {}. Opening/Closing are invalid for an unparenthesized
/// result type.
class FieldList : public Node {
public:
  GOPP_NODE(FieldList, Node)
  SourceLoc Opening, Closing;
  llvm::ArrayRef<Field *> List;
  /// Number of names, counting an anonymous Field as one.
  unsigned numFields() const {
    unsigned N = 0;
    for (const Field *F : List)
      N += F->Names.empty() ? 1 : F->Names.size();
    return N;
  }
};

enum class MacroKind : uint8_t {
  Expr,
  Stmt,
  Block,
  Type,
  Ident,
  Lit,
  Decl,
  Tokens,
  Invalid,
};
const char *getMacroKindName(MacroKind K);

/// `name kind` or `name ...kind` in a macro declaration.
class MacroParam : public Node {
public:
  GOPP_NODE(MacroParam, Node)
  Ident *Name = nullptr;
  MacroKind ParamKind = MacroKind::Invalid;
  SourceLoc KindLoc;
  bool Variadic = false;
};

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

class BadStmt : public Stmt {
public:
  GOPP_NODE(BadStmt, Stmt)
};

class DeclStmt : public Stmt {
public:
  GOPP_NODE(DeclStmt, Stmt)
  Decl *D = nullptr;
};

class EmptyStmt : public Stmt {
public:
  GOPP_NODE(EmptyStmt, Stmt)
  bool Implicit = false; // the `;` was omitted (before a closing brace)
};

class LabeledStmt : public Stmt {
public:
  GOPP_NODE(LabeledStmt, Stmt)
  Ident *Label = nullptr;
  SourceLoc Colon;
  Stmt *Body = nullptr;
};

class ExprStmt : public Stmt {
public:
  GOPP_NODE(ExprStmt, Stmt)
  Expr *X = nullptr;
};

class SendStmt : public Stmt {
public:
  GOPP_NODE(SendStmt, Stmt)
  Expr *Chan = nullptr;
  SourceLoc ArrowLoc;
  Expr *Value = nullptr;
};

class IncDecStmt : public Stmt {
public:
  GOPP_NODE(IncDecStmt, Stmt)
  Expr *X = nullptr;
  SourceLoc OpLoc;
  tok::Kind Op = tok::inc; // inc or dec
};

/// `a, b = x, y`, `a := x`, `a += x`. Op is the operator token.
class AssignStmt : public Stmt {
public:
  GOPP_NODE(AssignStmt, Stmt)
  llvm::ArrayRef<Expr *> Lhs;
  SourceLoc OpLoc;
  tok::Kind Op = tok::assign;
  llvm::ArrayRef<Expr *> Rhs;
};

class GoStmt : public Stmt {
public:
  GOPP_NODE(GoStmt, Stmt)
  SourceLoc GoLoc;
  Expr *Call = nullptr;
};

class DeferStmt : public Stmt {
public:
  GOPP_NODE(DeferStmt, Stmt)
  SourceLoc DeferLoc;
  Expr *Call = nullptr;
};

class ReturnStmt : public Stmt {
public:
  GOPP_NODE(ReturnStmt, Stmt)
  llvm::ArrayRef<Expr *> Results;
};

/// break, continue, goto, fallthrough
class BranchStmt : public Stmt {
public:
  GOPP_NODE(BranchStmt, Stmt)
  tok::Kind Keyword = tok::kw_break;
  Ident *Label = nullptr;
};

class BlockStmt : public Stmt {
public:
  GOPP_NODE(BlockStmt, Stmt)
  SourceLoc Lbrace, Rbrace;
  llvm::ArrayRef<Stmt *> List;
};

class IfStmt : public Stmt {
public:
  GOPP_NODE(IfStmt, Stmt)
  Stmt *Init = nullptr;
  Expr *Cond = nullptr;
  BlockStmt *Then = nullptr;
  Stmt *Else = nullptr; // IfStmt or BlockStmt
};

/// `case a, b:` or `default:` (List empty and IsDefault).
class CaseClause : public Stmt {
public:
  GOPP_NODE(CaseClause, Stmt)
  SourceLoc CaseLoc;
  bool IsDefault = false;
  llvm::ArrayRef<Expr *> List;
  SourceLoc Colon;
  llvm::ArrayRef<Stmt *> Body;
};

class SwitchStmt : public Stmt {
public:
  GOPP_NODE(SwitchStmt, Stmt)
  Stmt *Init = nullptr;
  Expr *Tag = nullptr;
  BlockStmt *Body = nullptr; // of CaseClauses
};

/// `switch [Init;] [x :=] y.(type) { ... }`. Assign is an ExprStmt or an
/// AssignStmt whose right side is the TypeAssertExpr.
class TypeSwitchStmt : public Stmt {
public:
  GOPP_NODE(TypeSwitchStmt, Stmt)
  Stmt *Init = nullptr;
  Stmt *Assign = nullptr;
  BlockStmt *Body = nullptr;
};

/// `case <send or receive>:` or `default:` (Comm null).
class CommClause : public Stmt {
public:
  GOPP_NODE(CommClause, Stmt)
  SourceLoc CaseLoc;
  Stmt *Comm = nullptr;
  SourceLoc Colon;
  llvm::ArrayRef<Stmt *> Body;
};

class SelectStmt : public Stmt {
public:
  GOPP_NODE(SelectStmt, Stmt)
  BlockStmt *Body = nullptr; // of CommClauses
};

class ForStmt : public Stmt {
public:
  GOPP_NODE(ForStmt, Stmt)
  Stmt *Init = nullptr;
  Expr *Cond = nullptr;
  Stmt *Post = nullptr;
  BlockStmt *Body = nullptr;
};

/// `for Key, Value := range X { ... }`. Op is define, assign, or tok::eof if
/// there are no variables. The `&` mark of D1.21 is in Alias: it applies to
/// the value variable, or to the only variable of `for &d := range seq`.
class RangeStmt : public Stmt {
public:
  enum class AliasKind : uint8_t { None, Key, Value };
  GOPP_NODE(RangeStmt, Stmt)
  Expr *Key = nullptr;
  Expr *Value = nullptr;
  AliasKind Alias = AliasKind::None;
  SourceLoc AliasLoc;
  SourceLoc OpLoc;
  tok::Kind Op = tok::eof;
  SourceLoc RangeLoc;
  Expr *X = nullptr;
  BlockStmt *Body = nullptr;
};

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

class BadDecl : public Decl {
public:
  GOPP_NODE(BadDecl, Decl)
};

class ImportSpec : public Spec {
public:
  GOPP_NODE(ImportSpec, Spec)
  Ident *Name = nullptr; // `.`, `_`, or a package name; may be null
  BasicLit *Path = nullptr;
};

/// const or var spec. For `const (x = 0; y; z)` the later specs have no Type
/// and no Values; the repetition is semantic.
class ValueSpec : public Spec {
public:
  GOPP_NODE(ValueSpec, Spec)
  llvm::ArrayRef<Ident *> Names;
  Expr *Type = nullptr;
  llvm::ArrayRef<Expr *> Values;
};

class TypeSpec : public Spec {
public:
  GOPP_NODE(TypeSpec, Spec)
  Ident *Name = nullptr;
  FieldList *TypeParams = nullptr;
  SourceLoc AssignLoc; // valid for an alias `type A = B`
  Expr *Type = nullptr;
};

/// import, const, type or var declaration, with attributes before it.
class GenDecl : public Decl {
public:
  GOPP_NODE(GenDecl, Decl)
  llvm::ArrayRef<Attribute *> Attrs;
  SourceLoc KeywordLoc;
  tok::Kind Keyword = tok::kw_var;
  SourceLoc Lparen, Rparen; // invalid if not grouped
  llvm::ArrayRef<Spec *> Specs;
};

/// A function or method. Type->Fails is the `!` mark; Type->TypeParams has the
/// type parameters. Body is null for a declaration without a body.
class FuncDecl : public Decl {
public:
  GOPP_NODE(FuncDecl, Decl)
  llvm::ArrayRef<Attribute *> Attrs;
  FieldList *Recv = nullptr;
  Ident *Name = nullptr;
  FuncType *Type = nullptr;
  BlockStmt *Body = nullptr;
};

/// `macro Name(p kind, ...) { body }`: a declarative macro.
class MacroDecl : public Decl {
public:
  GOPP_NODE(MacroDecl, Decl)
  SourceLoc MacroLoc;
  Ident *Name = nullptr;
  SourceLoc Lparen, Rparen;
  llvm::ArrayRef<MacroParam *> Params;
  BlockStmt *Body = nullptr;
};

/// `macro func Name[!](...) results { body }`: a procedural macro.
class ProcMacroDecl : public Decl {
public:
  GOPP_NODE(ProcMacroDecl, Decl)
  SourceLoc MacroLoc, FuncLoc;
  Ident *Name = nullptr;
  FuncType *Type = nullptr; // Type->Fails is the `!` mark
  BlockStmt *Body = nullptr;
};

/// `#Name(...)` at the top level.
class MacroCallDecl : public Decl {
public:
  GOPP_NODE(MacroCallDecl, Decl)
  MacroInvocation *Call = nullptr;
};

//===----------------------------------------------------------------------===//
// File
//===----------------------------------------------------------------------===//

struct Comment {
  SourceLoc Loc;
  uint32_t Length = 0;
};

/// A run of comments that belong together (the comment map of D2.3), built
/// like go/parser does: comments with no blank line between them. The comments
/// are File::Comments[First, First + Count).
///   - Lead: the group ends on the line just above the token that follows it
///     (a doc comment of the declaration, spec or field that starts there);
///     Next is that token.
///   - Line: the group starts on the line of the token before it and the next
///     token is on a later line (a trailing comment of that line).
struct CommentGroup {
  uint32_t First = 0;
  uint32_t Count = 0;
  bool Lead = false;
  bool Line = false;
  SourceLoc Next; // the token after the group
};

class File : public Node {
public:
  GOPP_NODE(File, Node)
  SourceLoc PackageLoc;
  Ident *Name = nullptr;
  llvm::ArrayRef<Decl *> Decls;
  llvm::ArrayRef<ImportSpec *> Imports; // all imports, in source order
  /// Every comment, in source order (the comment map of D2.3 is built from it).
  llvm::ArrayRef<Comment> Comments;
  /// The same comments in groups.
  llvm::ArrayRef<CommentGroup> CommentGroups;
};

#undef GOPP_NODE

} // namespace ast
} // namespace gopp

#endif // GOPP_AST_AST_H
