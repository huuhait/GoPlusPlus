//===- Parser.cpp - The Go++ parser ---------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The structure, the error recovery and the handling of Go's grammar
// ambiguities follow go/parser (parser.go):
//   Copyright 2009 The Go Authors. All rights reserved.
//   Use of this source code is governed by a BSD-style license (see NOTICE).
//
//===----------------------------------------------------------------------===//

#include "gopp/Parse/Parser.h"
#include "gopp/AST/ASTDumper.h"
#include "gopp/Lex/Lexer.h"

#include "llvm/ADT/SmallVector.h"

#include <deque>

using namespace gopp;
using namespace gopp::ast;
using llvm::ArrayRef;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::SmallVector;

namespace {

/// A set of token kinds, for error recovery.
class TokenSet {
  uint64_t Bits[(tok::NUM_TOKENS + 63) / 64] = {};

public:
  TokenSet(std::initializer_list<tok::Kind> Ks) {
    for (tok::Kind K : Ks)
      Bits[K / 64] |= uint64_t(1) << (K % 64);
  }
  bool contains(tok::Kind K) const { return Bits[K / 64] >> (K % 64) & 1; }
};

// Where to resynchronize after an error (as in go/parser).
const TokenSet StmtStart = {
    tok::kw_break,       tok::kw_const,  tok::kw_continue, tok::kw_defer,
    tok::kw_fallthrough, tok::kw_for,    tok::kw_go,       tok::kw_goto,
    tok::kw_if,          tok::kw_return, tok::kw_select,   tok::kw_switch,
    tok::kw_type,        tok::kw_var,    tok::hash};
// Inside a function body also stop at ';' and '}' (the end of the statement or
// of the block), so one bad statement does not swallow what follows. A bad
// top-level initializer stops at the next 'func' or 'macro' declaration.
const TokenSet StmtSync = {
    tok::kw_break,       tok::kw_const,  tok::kw_continue, tok::kw_defer,
    tok::kw_fallthrough, tok::kw_for,    tok::kw_go,       tok::kw_goto,
    tok::kw_if,          tok::kw_return, tok::kw_select,   tok::kw_switch,
    tok::kw_type,        tok::kw_var,    tok::hash,        tok::semi,
    tok::r_brace,        tok::kw_func,   tok::kw_macro};
const TokenSet DeclStart = {tok::kw_const, tok::kw_import, tok::kw_type,
                            tok::kw_var,   tok::kw_func,   tok::kw_macro,
                            tok::at,       tok::hash};
const TokenSet ExprEnd = {tok::comma,   tok::colon,   tok::semi,
                          tok::r_paren, tok::r_brack, tok::r_brace};

enum SimpleMode { Basic, LabelOk, RangeOk };

constexpr int LowestPrec = 0;

int precedence(tok::Kind K) {
  switch (K) {
  case tok::lor:
    return 1;
  case tok::land:
    return 2;
  case tok::eql:
  case tok::neq:
  case tok::lss:
  case tok::leq:
  case tok::gtr:
  case tok::geq:
    return 3;
  case tok::add:
  case tok::sub:
  case tok::or_:
  case tok::xor_:
    return 4;
  case tok::mul:
  case tok::quo:
  case tok::rem:
  case tok::shl:
  case tok::shr:
  case tok::and_:
  case tok::and_not:
    return 5;
  default:
    return LowestPrec;
  }
}

Expr *unparen(Expr *X) {
  while (auto *P = dyn_cast<ParenExpr>(X))
    X = P->X;
  return X;
}

/// A parameter entry before names and types are matched up.
struct ParamEntry {
  Ident *Name = nullptr;
  Expr *Type = nullptr;
  SourceLoc MoveLoc;
};

class Parser {
public:
  Parser(ASTContext &Ctx, const SourceManager &SM, FileID File,
         DiagnosticsEngine &Diags, ParseOptions Opts)
      : Ctx(Ctx), SM(SM), File(File), Diags(Diags), Opts(Opts),
        LexDiags(SM, LexQueue, /*ErrorLimit=*/0),
        Lex(SM, File, LexDiags,
            [] {
              LexerOptions LO;
              LO.KeepComments = true; // collected for the comment map (D2.3)
              return LO;
            }()),
        Src(SM.getBuffer(File)) {
    PrevEnd = SM.getStartLoc(File);
    next();
  }

  ast::File *parseFile();

private:
  //===--------------------------------------------------------------------===//
  // Tokens
  //===--------------------------------------------------------------------===//

  Token eofToken() const {
    Token T;
    T.Kind = tok::eof;
    T.Loc = SM.getEndLoc(File);
    return T;
  }

  Token lexToken() {
    for (;;) {
      Token T = Lex.lex();
      if (T.is(tok::comment)) {
        Comment C;
        C.Loc = T.Loc;
        C.Length = T.Length;
        Comments.push_back(C);
        continue;
      }
      return T;
    }
  }

  bool stopped() const { return Bailed || Diags.limitReached(); }

  void next() {
    if (!Tok.isAutoSemi())
      PrevEnd = Tok.getEndLoc();
    if (stopped()) {
      Tok = eofToken();
      Ahead.clear();
      return;
    }
    if (!Ahead.empty()) {
      Tok = Ahead.front();
      Ahead.pop_front();
    } else {
      Tok = lexToken();
    }
    // As in go/parser, a token's lexical errors are reported once the parser
    // reaches it, not when a lookahead first scans it.
    flushLexDiags(Tok.getEndLoc());
  }

  /// The N-th token after the current one (N >= 1).
  const Token &peek(unsigned N = 1) {
    if (stopped()) {
      PeekEOF = eofToken();
      return PeekEOF;
    }
    while (Ahead.size() < N)
      Ahead.push_back(lexToken());
    return Ahead[N - 1];
  }

  bool is(tok::Kind K) const { return Tok.Kind == K; }
  llvm::StringRef text(const Token &T) const { return Lex.getText(T); }
  bool isWord(const Token &T, llvm::StringRef W) const {
    return T.is(tok::ident) && text(T) == W;
  }
  Symbol symbol(const Token &T) { return Ctx.intern(text(T)); }

  /// Go's token strings, used in "expected X" messages.
  static std::string tokString(tok::Kind K) {
    if (const char *S = tok::getSpelling(K))
      return S;
    switch (K) {
    case tok::ident:
      return "IDENT";
    case tok::int_lit:
      return "INT";
    case tok::float_lit:
      return "FLOAT";
    case tok::imag_lit:
      return "IMAG";
    case tok::char_lit:
      return "CHAR";
    case tok::string_lit:
      return "STRING";
    case tok::eof:
      return "EOF";
    default:
      return tok::getName(K);
    }
  }

  //===--------------------------------------------------------------------===//
  // Errors
  //===--------------------------------------------------------------------===//

  /// Reports the queued lexer diagnostics at or before UpTo (all of them if
  /// UpTo is invalid), so that errors come out in source order. They count for
  /// the one-error-per-line rule: in go/parser the scanner's and the parser's
  /// errors share one list.
  void flushLexDiags(SourceLoc UpTo) {
    std::vector<Diagnostic> &Queue = LexQueue.Diags;
    while (NextLexDiag < Queue.size() &&
           (!UpTo.isValid() || Queue[NextLexDiag].Loc <= UpTo)) {
      Diagnostic &D = Queue[NextLexDiag++];
      if (D.Loc.isValid())
        LastErrorLine = SM.getLineNumber(D.Loc);
      Diags.emit(std::move(D));
    }
  }

  /// Go reports only the first error of a line: later ones are usually noise.
  bool shouldReport(SourceLoc L) {
    if (!L.isValid())
      return true;
    flushLexDiags(L);
    unsigned Line = SM.getLineNumber(L);
    if (Line == LastErrorLine)
      return false;
    LastErrorLine = Line;
    return true;
  }

  void errorRange(SourceLoc L, SourceRange R, diag::ID ID,
                  ArrayRef<llvm::StringRef> Args = {}) {
    if (!shouldReport(L))
      return;
    DiagnosticBuilder B = Diags.report(L, ID);
    B.range(R);
    for (llvm::StringRef A : Args)
      B << A;
  }

  void error(SourceLoc L, diag::ID ID, ArrayRef<llvm::StringRef> Args = {}) {
    errorRange(L, SourceRange(L), ID, Args);
  }

  void errorExpected(SourceLoc L, llvm::StringRef What) {
    if (L != Tok.Loc) {
      error(L, diag::err_expected, {What});
      return;
    }
    // the error is at the current token: say what is there
    std::string Found;
    if (Tok.isAutoSemi())
      Found = "newline";
    else if (tok::isLiteral(Tok.Kind))
      Found = text(Tok).str();
    else
      Found = "'" + tokString(Tok.Kind) + "'";
    SourceRange R =
        !Tok.isAutoSemi() && Tok.Length ? Tok.getRange() : SourceRange(L);
    errorRange(L, R, diag::err_expected_found, {What, Found});
  }

  SourceLoc expect(tok::Kind K) {
    SourceLoc L = Tok.Loc;
    if (!is(K)) {
      errorExpected(L, "'" + tokString(K) + "'");
      // A '}' closes some enclosing block: keep it for that block.
      if (is(tok::r_brace))
        return L;
    }
    next(); // make progress
    return L;
  }

  /// Like expect, but does not consume the token if it is not K.
  SourceLoc expect2(tok::Kind K) {
    SourceLoc L = Tok.Loc;
    if (!is(K))
      errorExpected(L, "'" + tokString(K) + "'");
    else
      next();
    return L;
  }

  /// expect for a closing token. Reports a missing trailing comma before a
  /// newline with a fix-it.
  SourceLoc expectClosing(tok::Kind K, llvm::StringRef Context) {
    if (!is(K) && is(tok::semi) && Tok.isAutoSemi()) {
      if (shouldReport(Tok.Loc))
        Diags.report(Tok.Loc, diag::err_missing_comma_newline)
                .insert(PrevEnd, ",")
            << Context;
      next();
    }
    return expect(K);
  }

  void expectSemi() {
    // semicolon is optional before a closing ')' or '}'
    if (is(tok::r_paren) || is(tok::r_brace))
      return;
    switch (Tok.Kind) {
    case tok::comma:
      // permit a ',' instead of a ';' but complain
      errorExpected(Tok.Loc, "';'");
      next();
      break;
    case tok::semi:
      next();
      break;
    default:
      errorExpected(Tok.Loc, "';'");
      advance(StmtSync);
      break;
    }
  }

  bool atComma(llvm::StringRef Context, tok::Kind Follow) {
    if (is(tok::comma))
      return true;
    if (!is(Follow)) {
      if (is(tok::semi) && Tok.isAutoSemi()) {
        if (shouldReport(Tok.Loc))
          Diags.report(Tok.Loc, diag::err_missing_comma_newline)
                  .insert(PrevEnd, ",")
              << Context;
      } else {
        error(Tok.Loc, diag::err_missing_comma, {Context});
      }
      return true; // "insert" comma and continue
    }
    return false;
  }

  /// Skips tokens up to one in To (or EOF). It always makes progress over a
  /// sequence of calls, so a parse loop cannot spin forever.
  void advance(const TokenSet &To) {
    for (; !is(tok::eof); next()) {
      if (!To.contains(Tok.Kind))
        continue;
      // Return only if the parser made some progress since the last sync, or
      // if it has not reached 10 advance calls without progress. Otherwise
      // consume at least one token to avoid an endless parser loop.
      if (Tok.Loc == SyncPos && SyncCnt < 10) {
        ++SyncCnt;
        return;
      }
      if (SyncPos < Tok.Loc) {
        SyncPos = Tok.Loc;
        SyncCnt = 0;
        return;
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // Nesting
  //===--------------------------------------------------------------------===//

  /// Counts recursion depth; past the limit it reports once and stops the
  /// parse (the token stream turns into EOF, so every loop unwinds).
  class NestGuard {
    Parser &P;

  public:
    explicit NestGuard(Parser &P) : P(P) {
      if (++P.Depth > P.Opts.MaxNesting)
        P.tooDeep();
    }
    ~NestGuard() { --P.Depth; }
  };

  void tooDeep() {
    if (!Bailed) {
      Bailed = true;
      Diags.report(Tok.Loc, diag::err_nesting_too_deep);
    }
    Tok = eofToken();
    Ahead.clear();
  }

  /// Checks the length of an iterative chain. A chain of N operators is a tree
  /// at least N tall, so this stops early; the exact check is limitHeight.
  bool chainTooLong(unsigned N) {
    if (N <= Opts.MaxHeight)
      return false;
    if (!Bailed) {
      Bailed = true;
      Diags.report(Tok.Loc, diag::err_expression_too_deeply_nested)
          << Opts.MaxHeight;
    }
    Tok = eofToken();
    Ahead.clear();
    return true;
  }

  /// True if the tree under D is taller than MaxHeight (D itself counts 1).
  /// Iterative, and stops as soon as it passes the limit.
  bool exceedsMaxHeight(const Node *D) {
    SmallVector<std::pair<const Node *, unsigned>, 64> Work;
    Work.emplace_back(D, 1);
    while (!Work.empty()) {
      auto [N, H] = Work.pop_back_val();
      if (H > Opts.MaxHeight)
        return true;
      forEachChild(
          N, [&](const char *, const Node *C) { Work.emplace_back(C, H + 1); });
    }
    return false;
  }

  /// Every later pass walks the tree recursively, so a declaration taller
  /// than MaxHeight (nesting and chains together) becomes a BadDecl.
  Decl *limitHeight(Decl *D) {
    if (!exceedsMaxHeight(D))
      return D;
    // The parse goes on: the next declaration is not affected.
    Diags.report(D->Begin, diag::err_expression_too_deeply_nested)
        << Opts.MaxHeight;
    return mk<BadDecl>(D->Begin, D->End);
  }

  //===--------------------------------------------------------------------===//
  // Node construction
  //===--------------------------------------------------------------------===//

  template <class T> T *mk(SourceLoc B, SourceLoc E) {
    T *N = Ctx.create<T>();
    N->Begin = B;
    N->End = E;
    return N;
  }
  template <class T> T *mk(SourceLoc B) { return mk<T>(B, PrevEnd); }

  BadExpr *badExpr(SourceLoc From, SourceLoc To) {
    return mk<BadExpr>(From, To < From ? From : To);
  }
  BadExpr *badExprHere(SourceLoc From) { return badExpr(From, Tok.Loc); }

  Ident *makeIdent(llvm::StringRef Name, SourceLoc B, SourceLoc E) {
    Ident *I = mk<Ident>(B, E);
    I->Name = Ctx.intern(Name);
    return I;
  }

  //===--------------------------------------------------------------------===//
  // Lookahead predicates for the contextual words (LEX-12)
  //===--------------------------------------------------------------------===//

  /// At `ref` followed by `string` or by `[` `]`: a RefType starts here.
  bool isRefTypeStart() {
    if (!isWord(Tok, "ref"))
      return false;
    const Token &T1 = peek(1);
    if (isWord(T1, "string"))
      return true;
    return T1.is(tok::l_brack) && peek(2).is(tok::r_brack);
  }

  /// At `move` in a parameter declaration: is it the mark (`move v T`) or a
  /// parameter named `move` (`move T`, `move pkg.T`, `move G[K]`)? Decided by
  /// scanning to the end of the type that would follow the name.
  bool isMoveMark() {
    if (!isWord(Tok, "move"))
      return false;
    Token T1 = peek(1);
    if (!T1.is(tok::ident))
      return false; // move []int, move *T, move ...T, move func(): a name
    // `move ref string`: the parameter `move` of type `ref string`
    if (isWord(T1, "ref")) {
      const Token &T2 = peek(2);
      if (isWord(T2, "string") ||
          (T2.is(tok::l_brack) && peek(3).is(tok::r_brack)))
        return false;
    }
    Token T2 = peek(2);
    switch (T2.Kind) {
    case tok::comma:
    case tok::r_paren:
    case tok::period:
    case tok::eof:
      return false; // move T / move pkg.T: T is the type
    case tok::l_brack: {
      // `move s [K]E` or `move s []E` (mark) versus `move G[K]` (a name and an
      // instantiated type): look at what follows the matching `]`.
      unsigned Depth = 0;
      for (unsigned I = 2; I < 2 + 256; ++I) {
        const Token &T = peek(I);
        if (T.is(tok::eof))
          return false;
        if (T.is(tok::l_brack))
          ++Depth;
        else if (T.is(tok::r_brack) && --Depth == 0) {
          const Token &After = peek(I + 1);
          return !(After.is(tok::comma) || After.is(tok::r_paren) ||
                   After.is(tok::eof));
        }
      }
      return true; // very long: assume the mark
    }
    default:
      return true;
    }
  }

  //===--------------------------------------------------------------------===//
  // Identifiers and lists
  //===--------------------------------------------------------------------===//

  Ident *parseIdent() {
    SourceLoc L = Tok.Loc;
    if (is(tok::ident)) {
      Ident *I = mk<Ident>(L, Tok.getEndLoc());
      I->Name = symbol(Tok);
      next();
      return I;
    }
    expect(tok::ident); // use expect() error handling
    return makeIdent("_", L, L);
  }

  void parseIdentList(SmallVector<Ident *, 8> &List) {
    List.push_back(parseIdent());
    while (is(tok::comma)) {
      next();
      List.push_back(parseIdent());
    }
  }

  void parseExprList(SmallVector<Expr *, 8> &List) {
    List.push_back(parseExpr());
    while (is(tok::comma)) {
      next();
      List.push_back(parseExpr());
    }
  }

  void parseList(SmallVector<Expr *, 8> &List, bool InRhs) {
    bool Old = this->InRhs;
    this->InRhs = InRhs;
    parseExprList(List);
    this->InRhs = Old;
  }

  //===--------------------------------------------------------------------===//
  // Types
  //===--------------------------------------------------------------------===//

  Expr *parseType() {
    Expr *T = tryIdentOrType();
    if (!T) {
      SourceLoc L = Tok.Loc;
      errorExpected(L, "type");
      advance(ExprEnd);
      return badExprHere(L);
    }
    return T;
  }

  Expr *parseQualifiedIdent(Ident *I) {
    Expr *T = parseTypeName(I);
    if (is(tok::l_brack))
      T = parseTypeInstance(T);
    return T;
  }

  Expr *parseTypeName(Ident *I) {
    if (!I)
      I = parseIdent();
    if (is(tok::period)) {
      // I is a package name
      next();
      Ident *Sel = parseIdent();
      SelectorExpr *S = mk<SelectorExpr>(I->Begin);
      S->X = I;
      S->Sel = Sel;
      return S;
    }
    return I;
  }

  Expr *makeArrayOrSlice(SourceLoc Lbrack, Expr *Len, Expr *Elt) {
    if (!Len) {
      SliceType *S = mk<SliceType>(Lbrack, Elt ? Elt->End : PrevEnd);
      S->Lbrack = Lbrack;
      S->Elt = Elt;
      return S;
    }
    ArrayType *A = mk<ArrayType>(Lbrack, Elt ? Elt->End : PrevEnd);
    A->Lbrack = Lbrack;
    A->Len = Len;
    A->Elt = Elt;
    return A;
  }

  /// '[' has been consumed. Len is the already parsed length, or null.
  Expr *parseArrayType(SourceLoc Lbrack, Expr *Len) {
    if (!Len) {
      ++ExprLev;
      // always permit an ellipsis for more fault-tolerant parsing
      if (is(tok::ellipsis)) {
        Ellipsis *E = mk<Ellipsis>(Tok.Loc, Tok.getEndLoc());
        next();
        Len = E;
      } else if (!is(tok::r_brack)) {
        Len = parseRhs();
      }
      --ExprLev;
    }
    if (is(tok::comma)) {
      // Trailing commas are accepted in type parameter lists but not in array
      // type declarations. Accept for better error handling but complain.
      error(Tok.Loc, diag::err_unexpected_comma_rbrack);
      next();
    }
    expect(tok::r_brack);
    Expr *Elt = parseType();
    return makeArrayOrSlice(Lbrack, Len, Elt);
  }

  Expr *packIndexExpr(Expr *X, SourceLoc Lbrack, ArrayRef<Expr *> Exprs,
                      SourceLoc Rbrack) {
    IndexExpr *I = mk<IndexExpr>(X->Begin, Rbrack.getLocWithOffset(1));
    I->X = X;
    I->Lbrack = Lbrack;
    I->Rbrack = Rbrack;
    I->Indices = Ctx.copy(Exprs);
    I->End = PrevEnd < Rbrack.getLocWithOffset(1) ? Rbrack.getLocWithOffset(1)
                                                  : PrevEnd;
    return I;
  }

  /// `x [P]E` (a field or parameter named x of array type) versus `x[P]` (an
  /// embedded or unnamed instantiated type). Returns the name (null for the
  /// instance) and the type.
  std::pair<Ident *, Expr *> parseArrayFieldOrTypeInstance(Ident *X) {
    SourceLoc Lbrack = expect(tok::l_brack);
    SourceLoc TrailingComma;
    SmallVector<Expr *, 8> Args;
    if (!is(tok::r_brack)) {
      ++ExprLev;
      Args.push_back(parseRhs());
      while (is(tok::comma)) {
        SourceLoc Comma = Tok.Loc;
        next();
        if (is(tok::r_brack)) {
          TrailingComma = Comma;
          break;
        }
        Args.push_back(parseRhs());
      }
      --ExprLev;
    }
    SourceLoc Rbrack = expect(tok::r_brack);

    if (Args.empty()) {
      // x []E
      Expr *Elt = parseType();
      return {X, makeArrayOrSlice(Lbrack, nullptr, Elt)};
    }

    // x [P]E or x[P]
    if (Args.size() == 1) {
      Expr *Elt = tryIdentOrType();
      if (Elt) {
        // x [P]E
        if (TrailingComma.isValid()) // invalid in array type fields
          error(TrailingComma, diag::err_unexpected_comma_rbrack);
        return {X, makeArrayOrSlice(Lbrack, Args[0], Elt)};
      }
    }
    // x[P], x[P1, P2], ...
    return {nullptr, packIndexExpr(X, Lbrack, Args, Rbrack)};
  }

  /// Attributes after a field's type: `@Name` and `@Name(...)`.
  void parseFieldAttributes(SmallVector<Attribute *, 8> &Attrs) {
    while (is(tok::at))
      Attrs.push_back(parseAttribute());
  }

  Field *parseFieldDecl() {
    SourceLoc Begin = Tok.Loc;
    SmallVector<Ident *, 8> Names;
    Expr *Type = nullptr;
    switch (Tok.Kind) {
    case tok::ident: {
      Ident *Name = parseIdent();
      if (is(tok::period) || is(tok::string_lit) || is(tok::semi) ||
          is(tok::r_brace) || is(tok::at)) {
        // embedded type
        Type = Name;
        if (is(tok::period))
          Type = parseQualifiedIdent(Name);
      } else {
        // name1, name2, ... T
        Names.push_back(Name);
        while (is(tok::comma)) {
          next();
          Names.push_back(parseIdent());
        }
        // We don't know if we have an embedded instantiated type T[P1, P2, ...]
        // or a field T of array type []E or [P]E.
        if (Names.size() == 1 && is(tok::l_brack)) {
          auto [N, T] = parseArrayFieldOrTypeInstance(Name);
          Type = T;
          if (!N)
            Names.clear();
        } else {
          // T P
          Type = parseType();
        }
      }
      break;
    }
    case tok::mul: {
      SourceLoc Star = Tok.Loc;
      next();
      if (is(tok::l_paren)) { // *(T)
        error(Tok.Loc, diag::err_paren_embedded_type);
        next();
        Type = parseQualifiedIdent(nullptr);
        // expect closing ')' but no need to complain if missing
        if (is(tok::r_paren))
          next();
      } else { // *T
        Type = parseQualifiedIdent(nullptr);
      }
      StarExpr *S = mk<StarExpr>(Star);
      S->X = Type;
      Type = S;
      break;
    }
    case tok::l_paren:
      error(Tok.Loc, diag::err_paren_embedded_type);
      next();
      if (is(tok::mul)) { // (*T)
        SourceLoc Star = Tok.Loc;
        next();
        StarExpr *S = mk<StarExpr>(Star);
        S->X = parseQualifiedIdent(nullptr);
        S->End = PrevEnd;
        Type = S;
      } else { // (T)
        Type = parseQualifiedIdent(nullptr);
      }
      if (is(tok::r_paren))
        next();
      break;
    default: {
      SourceLoc L = Tok.Loc;
      errorExpected(L, "field name or embedded type");
      advance(ExprEnd);
      Type = badExprHere(L);
      break;
    }
    }

    SmallVector<Attribute *, 8> Attrs;
    parseFieldAttributes(Attrs);
    BasicLit *Tag = nullptr;
    if (is(tok::string_lit)) {
      Tag = parseBasicLit();
    }
    SourceLoc End = PrevEnd;
    expectSemi();

    Field *F = mk<Field>(Begin, End);
    F->Names = Ctx.copy(ArrayRef<Ident *>(Names));
    F->Type = Type;
    F->Attrs = Ctx.copy(ArrayRef<Attribute *>(Attrs));
    F->Tag = Tag;
    return F;
  }

  Expr *parseStructType() {
    NestGuard G(*this);
    SourceLoc Pos = expect(tok::kw_struct);
    SourceLoc Lbrace = expect(tok::l_brace);
    SmallVector<Field *, 8> List;
    while (is(tok::ident) || is(tok::mul) || is(tok::l_paren)) {
      // a field declaration cannot start with a '(' but we accept it here for
      // more robust parsing and better error messages
      List.push_back(parseFieldDecl());
    }
    SourceLoc Rbrace = expect(tok::r_brace);

    FieldList *FL = mk<FieldList>(Lbrace);
    FL->Opening = Lbrace;
    FL->Closing = Rbrace;
    FL->List = Ctx.copy(ArrayRef<Field *>(List));
    StructType *S = mk<StructType>(Pos);
    S->StructLoc = Pos;
    S->Fields = FL;
    return S;
  }

  Expr *parsePointerType() {
    SourceLoc Star = expect(tok::mul);
    Expr *Base = parseType();
    StarExpr *S = mk<StarExpr>(Star);
    S->X = Base;
    return S;
  }

  Ellipsis *parseDotsType() {
    SourceLoc Pos = expect(tok::ellipsis);
    Expr *Elt = parseType();
    Ellipsis *E = mk<Ellipsis>(Pos);
    E->Elt = Elt;
    return E;
  }

  /// `ref string` or `ref []T`; the `ref` word is current.
  Expr *parseRefType() {
    SourceLoc Pos = Tok.Loc;
    next(); // ref
    Expr *Elem;
    if (is(tok::ident)) {
      Elem = parseIdent(); // string
    } else {
      SourceLoc Lbrack = expect(tok::l_brack);
      Elem = parseArrayType(Lbrack, nullptr);
    }
    RefType *R = mk<RefType>(Pos);
    R->RefLoc = Pos;
    R->Elem = Elem;
    return R;
  }

  //===--------------------------------------------------------------------===//
  // Parameters
  //===--------------------------------------------------------------------===//

  ParamEntry parseParamDecl(Ident *Name0, bool TypeSetsOK, bool AllowMove) {
    ParamEntry F;
    tok::Kind PTok = Tok.Kind;
    bool Forced = Name0 != nullptr;
    if (Forced) {
      PTok = tok::ident; // force the ident case below
    } else if (TypeSetsOK && is(tok::tilde)) {
      // "~" ...
      F.Type = embeddedElem(nullptr);
      return F;
    }

    // `ref string` / `ref []T` at the start of an entry is a type (LEX-12).
    if (!Forced && PTok == tok::ident && isRefTypeStart()) {
      F.Type = parseType();
      if (TypeSetsOK && is(tok::or_))
        F.Type = embeddedElem(F.Type);
      return F;
    }

    // `move name Type`
    if (!Forced && AllowMove && PTok == tok::ident && isMoveMark()) {
      F.MoveLoc = Tok.Loc;
      next(); // move
      F.Name = parseIdent();
      if (is(tok::ellipsis))
        F.Type = parseDotsType();
      else
        F.Type = parseType();
      return F;
    }

    switch (PTok) {
    case tok::ident:
      // name
      F.Name = Forced ? Name0 : parseIdent();
      switch (Tok.Kind) {
      case tok::ident:
      case tok::mul:
      case tok::arrow:
      case tok::kw_func:
      case tok::kw_chan:
      case tok::kw_map:
      case tok::kw_struct:
      case tok::kw_interface:
      case tok::l_paren:
        // name type
        F.Type = parseType();
        break;
      case tok::l_brack: {
        // name "[" type1, ..., typeN "]" or name "[" n "]" type
        auto [N, T] = parseArrayFieldOrTypeInstance(F.Name);
        F.Name = N;
        F.Type = T;
        break;
      }
      case tok::ellipsis:
        // name "..." type
        F.Type = parseDotsType();
        return F; // don't allow ...type "|" ...
      case tok::period:
        // name "." ...
        F.Type = parseQualifiedIdent(F.Name);
        F.Name = nullptr;
        break;
      case tok::tilde:
        if (TypeSetsOK) {
          F.Type = embeddedElem(nullptr);
          return F;
        }
        break;
      case tok::or_:
        if (TypeSetsOK) {
          // name "|" typeset
          F.Type = embeddedElem(F.Name);
          F.Name = nullptr;
          return F;
        }
        break;
      default:
        break;
      }
      break;
    case tok::mul:
    case tok::arrow:
    case tok::kw_func:
    case tok::l_brack:
    case tok::kw_chan:
    case tok::kw_map:
    case tok::kw_struct:
    case tok::kw_interface:
    case tok::l_paren:
      // type
      F.Type = parseType();
      break;
    case tok::ellipsis:
      // "..." type (always accepted)
      F.Type = parseDotsType();
      return F; // don't allow ...type "|" ...
    default:
      errorExpected(Tok.Loc, "')'");
      advance(ExprEnd);
      break;
    }

    // [name] type "|"
    if (TypeSetsOK && is(tok::or_) && F.Type)
      F.Type = embeddedElem(F.Type);
    return F;
  }

  /// Parses a parameter list up to (not including) Closing. Name0/Typ0 are an
  /// already parsed first entry (type declarations). Closing == ']' means a
  /// type parameter list. DotsOK allows `...T` for the last entry.
  void parseParameterList(Ident *Name0, Expr *Typ0, tok::Kind Closing,
                          bool DotsOK, bool AllowMove,
                          SmallVector<Field *, 8> &Params) {
    // Type parameters are the only parameter list closed by ']'.
    bool TParams = Closing == tok::r_brack;

    SourceLoc Pos0 = Tok.Loc;
    if (Name0)
      Pos0 = Name0->Begin;
    else if (Typ0)
      Pos0 = Typ0->Begin;

    SmallVector<ParamEntry, 8> List;
    unsigned Named = 0; // number of entries that have an explicit name and type
    unsigned Typed = 0; // number of entries that have an explicit type

    while (Name0 || (!is(Closing) && !is(tok::eof))) {
      ParamEntry Par;
      if (Typ0) {
        if (TParams)
          Typ0 = embeddedElem(Typ0);
        Par.Name = Name0;
        Par.Type = Typ0;
      } else {
        Par = parseParamDecl(Name0, TParams, AllowMove);
      }
      Name0 = nullptr; // 1st name was consumed if present
      Typ0 = nullptr;  // 1st typ was consumed if present
      if (Par.Name || Par.Type) {
        List.push_back(Par);
        if (Par.Name && Par.Type)
          ++Named;
        if (Par.Type)
          ++Typed;
      }
      if (!atComma("parameter list", Closing))
        break;
      next();
    }

    if (List.empty())
      return; // not uncommon

    // distribute parameter types (List is not empty)
    if (Named == 0) {
      // all unnamed => found names are type names
      for (ParamEntry &Par : List) {
        if (Par.Name) {
          Par.Type = Par.Name;
          Par.Name = nullptr;
        }
      }
      if (TParams) {
        if (Named == Typed) // same as Typed == 0
          error(Tok.Loc, diag::err_missing_type_constraint); // at closing ]
        else // at opening [ or first name
          error(Pos0, List.size() == 1
                          ? diag::err_missing_type_param_name_or_len
                          : diag::err_missing_type_param_name);
      }
    } else if (Named != List.size()) {
      // some named or we're in a type parameter list => all must be named
      SourceLoc ErrPos;    // left-most error position (or invalid)
      Expr *Typ = nullptr; // current type (from right to left)
      for (size_t I = List.size(); I-- > 0;) {
        ParamEntry &Par = List[I];
        if (Par.Type) {
          // A `move` entry has its own explicit type and gives it to no one.
          Typ = Par.MoveLoc.isValid() ? nullptr : Par.Type;
          if (!Par.Name) {
            ErrPos = Par.Type->Begin;
            Par.Name = makeIdent("_", ErrPos, ErrPos);
          }
        } else if (Typ) {
          Par.Type = Typ;
        } else {
          // Par.Type == null && Typ == null => we only have a name
          ErrPos = Par.Name->Begin;
          Par.Type = badExpr(ErrPos, Tok.Loc);
        }
      }
      if (ErrPos.isValid()) {
        // Not all parameters are named because Named != List.size(). If
        // Named == Typed, there must be entries that have no types.
        diag::ID ID;
        if (Named == Typed) {
          ErrPos = Tok.Loc; // position error at closing token ) or ]
          ID = TParams ? diag::err_missing_type_constraint
                       : diag::err_missing_param_type;
        } else if (TParams) {
          ID = List.size() == 1 ? diag::err_missing_type_param_name_or_len
                                : diag::err_missing_type_param_name;
        } else {
          ID = diag::err_missing_param_name;
        }
        error(ErrPos, ID);
      }
    }

    // check use of ...
    bool First = true; // only report the first occurrence
    for (size_t I = 0; I < List.size(); ++I) {
      ParamEntry &Par = List[I];
      if (auto *E = dyn_cast<ast::Ellipsis>(Par.Type);
          E && (!DotsOK || I + 1 < List.size())) {
        if (First) {
          First = false;
          error(E->Begin,
                DotsOK ? diag::err_dots_not_final : diag::err_dots_invalid);
        }
        // use a BadExpr instead of the invalid ...T
        Par.Type = badExpr(E->Begin, E->End);
      }
    }

    // Convert List to Fields.
    if (Named == 0) {
      // the parameter list consists of types only
      for (ParamEntry &Par : List) {
        Field *F = mk<Field>(Par.Type->Begin, Par.Type->End);
        F->Type = Par.Type;
        Params.push_back(F);
      }
      return;
    }

    // Collect all names with the same type into a single Field. A `move`
    // entry always stands alone.
    SmallVector<Ident *, 8> Names;
    Expr *Typ = nullptr;
    SourceLoc MoveLoc;
    auto AddParams = [&] {
      Field *F = mk<Field>(Names.front()->Begin, Typ->End);
      if (MoveLoc.isValid())
        F->Begin = MoveLoc;
      F->Names = Ctx.copy(ArrayRef<Ident *>(Names));
      F->Type = Typ;
      F->MoveLoc = MoveLoc;
      Params.push_back(F);
      Names.clear();
    };
    for (ParamEntry &Par : List) {
      if (Par.Type != Typ || Par.MoveLoc.isValid() || MoveLoc.isValid()) {
        if (!Names.empty())
          AddParams();
        Typ = Par.Type;
        MoveLoc = Par.MoveLoc;
      }
      Names.push_back(Par.Name);
    }
    if (!Names.empty())
      AddParams();
  }

  FieldList *finishList(SourceLoc Open, SourceLoc Close,
                        const SmallVector<Field *, 8> &List) {
    FieldList *FL = mk<FieldList>(Open, Close.getLocWithOffset(1));
    FL->Opening = Open;
    FL->Closing = Close;
    FL->List = Ctx.copy(ArrayRef<Field *>(List));
    return FL;
  }

  FieldList *parseTypeParameters() {
    SourceLoc Lbrack = expect(tok::l_brack);
    SmallVector<Field *, 8> List;
    if (!is(tok::r_brack))
      parseParameterList(nullptr, nullptr, tok::r_brack, false, false, List);
    SourceLoc Rbrack = expect(tok::r_brack);
    if (List.empty()) {
      error(Rbrack, diag::err_empty_type_params);
      return nullptr; // avoid follow-on errors
    }
    return finishList(Lbrack, Rbrack, List);
  }

  /// Parameters, or (Result) the result list: either a parenthesized list or a
  /// single unparenthesized type. Returns null for no result.
  FieldList *parseParameters(bool Result) {
    if (!Result || is(tok::l_paren)) {
      SourceLoc Lparen = expect(tok::l_paren);
      SmallVector<Field *, 8> List;
      if (!is(tok::r_paren))
        parseParameterList(nullptr, nullptr, tok::r_paren, !Result,
                           /*AllowMove=*/!Result, List);
      SourceLoc Rparen = expect(tok::r_paren);
      return finishList(Lparen, Rparen, List);
    }
    if (Expr *T = tryIdentOrType()) {
      Field *F = mk<Field>(T->Begin, T->End);
      F->Type = T;
      FieldList *FL = mk<FieldList>(T->Begin, T->End);
      FL->List = Ctx.copy(ArrayRef<Field *>(&F, 1));
      return FL;
    }
    return nullptr;
  }

  /// `[!] Signature` after the name (or after `func`). Fills in Fails and the
  /// parameter and result lists.
  void parseSignature(FuncType *FT) {
    if (is(tok::not_)) {
      FT->Fails = true;
      FT->BangLoc = Tok.Loc;
      next();
    }
    FT->Params = parseParameters(false);
    FT->Results = parseParameters(true);
    FT->End = PrevEnd;
  }

  FuncType *parseFuncType() {
    SourceLoc Pos = expect(tok::kw_func);
    FuncType *FT = mk<FuncType>(Pos);
    FT->FuncLoc = Pos;
    // accept type parameters for more tolerant parsing but complain
    if (is(tok::l_brack)) {
      if (FieldList *TP = parseTypeParameters())
        error(TP->Opening, diag::err_func_type_type_params);
    }
    parseSignature(FT);
    return FT;
  }

  Field *parseMethodSpec() {
    SourceLoc Begin = Tok.Loc;
    SmallVector<Ident *, 8> Idents;
    Expr *Type = nullptr;
    Expr *X = parseTypeName(nullptr);
    if (auto *I = dyn_cast<Ident>(X)) {
      if (is(tok::l_brack)) {
        // generic method or embedded instantiated type
        SourceLoc Lbrack = Tok.Loc;
        next();
        ++ExprLev;
        Expr *Arg = parseExpr();
        --ExprLev;
        auto *Name0 = dyn_cast<Ident>(Arg);
        if (Name0 && !is(tok::comma) && !is(tok::r_brack)) {
          // generic method m[T any]
          // Interface methods do not have type parameters. We parse them for a
          // better error message and improved error recovery.
          SmallVector<Field *, 8> Discard;
          parseParameterList(Name0, nullptr, tok::r_brack, false, false,
                             Discard);
          expect(tok::r_brack);
          error(Lbrack, diag::err_interface_method_type_params);
          FuncType *FT = mk<FuncType>(Tok.Loc);
          parseSignature(FT);
          FT->Begin = I->End;
          Idents.push_back(I);
          Type = FT;
        } else {
          // embedded instantiated type
          SmallVector<Expr *, 8> List;
          List.push_back(Arg);
          if (atComma("type argument list", tok::r_brack)) {
            ++ExprLev;
            next();
            while (!is(tok::r_brack) && !is(tok::eof)) {
              List.push_back(parseType());
              if (!atComma("type argument list", tok::r_brack))
                break;
              next();
            }
            --ExprLev;
          }
          SourceLoc Rbrack = expectClosing(tok::r_brack, "type argument list");
          Type = packIndexExpr(I, Lbrack, List, Rbrack);
        }
      } else if (is(tok::l_paren) || is(tok::not_)) {
        // ordinary method
        FuncType *FT = mk<FuncType>(I->End);
        parseSignature(FT);
        Idents.push_back(I);
        Type = FT;
      } else {
        // embedded type
        Type = X;
      }
    } else {
      // embedded, possibly instantiated type
      Type = X;
      if (is(tok::l_brack)) // embedded instantiated interface
        Type = parseTypeInstance(Type);
    }
    Field *F = mk<Field>(Begin, PrevEnd);
    F->Names = Ctx.copy(ArrayRef<Ident *>(Idents));
    F->Type = Type;
    return F;
  }

  Expr *embeddedElem(Expr *X) {
    if (!X)
      X = embeddedTerm();
    unsigned N = 0;
    while (is(tok::or_)) {
      if (chainTooLong(++N))
        break;
      BinaryExpr *B = mk<BinaryExpr>(X->Begin);
      B->OpLoc = Tok.Loc;
      B->Op = tok::or_;
      next();
      B->X = X;
      B->Y = embeddedTerm();
      B->End = PrevEnd;
      X = B;
    }
    return X;
  }

  Expr *embeddedTerm() {
    if (is(tok::tilde)) {
      UnaryExpr *U = mk<UnaryExpr>(Tok.Loc);
      U->OpLoc = Tok.Loc;
      U->Op = tok::tilde;
      next();
      U->X = parseType();
      U->End = PrevEnd;
      return U;
    }
    Expr *T = tryIdentOrType();
    if (!T) {
      SourceLoc L = Tok.Loc;
      errorExpected(L, "~ term or type");
      advance(ExprEnd);
      return badExprHere(L);
    }
    return T;
  }

  Expr *parseInterfaceType() {
    NestGuard G(*this);
    SourceLoc Pos = expect(tok::kw_interface);
    SourceLoc Lbrace = expect(tok::l_brace);
    SmallVector<Field *, 8> List;
    for (;;) {
      if (is(tok::ident)) {
        Field *F = parseMethodSpec();
        if (F->Names.empty()) {
          F->Type = embeddedElem(F->Type);
          F->End = PrevEnd;
        }
        List.push_back(F);
      } else if (is(tok::tilde)) {
        SourceLoc B = Tok.Loc;
        Expr *T = embeddedElem(nullptr);
        Field *F = mk<Field>(B, T->End);
        F->Type = T;
        List.push_back(F);
      } else if (Expr *T = tryIdentOrType()) {
        Expr *E = embeddedElem(T);
        Field *F = mk<Field>(T->Begin, E->End);
        F->Type = E;
        List.push_back(F);
      } else {
        break;
      }
      expectSemi();
    }
    SourceLoc Rbrace = expect(tok::r_brace);
    InterfaceType *IT = mk<InterfaceType>(Pos);
    IT->InterfaceLoc = Pos;
    IT->Methods = finishList(Lbrace, Rbrace, List);
    return IT;
  }

  Expr *parseMapType() {
    NestGuard G(*this);
    SourceLoc Pos = expect(tok::kw_map);
    expect(tok::l_brack);
    Expr *Key = parseType();
    expect(tok::r_brack);
    Expr *Value = parseType();
    MapType *M = mk<MapType>(Pos);
    M->MapLoc = Pos;
    M->Key = Key;
    M->Value = Value;
    return M;
  }

  ChanType *parseChanType() {
    SourceLoc Pos = Tok.Loc;
    ChanType::Dir Dir = ChanType::Both;
    SourceLoc Arrow;
    if (is(tok::kw_chan)) {
      next();
      if (is(tok::arrow)) {
        Arrow = Tok.Loc;
        next();
        Dir = ChanType::Send;
      }
    } else {
      Arrow = expect(tok::arrow);
      expect(tok::kw_chan);
      Dir = ChanType::Recv;
    }
    Expr *Value = parseType();
    ChanType *C = mk<ChanType>(Pos);
    C->ArrowLoc = Arrow;
    C->Direction = Dir;
    C->Value = Value;
    return C;
  }

  Expr *parseTypeInstance(Expr *Type) {
    SourceLoc Opening = expect(tok::l_brack);
    ++ExprLev;
    SmallVector<Expr *, 8> List;
    while (!is(tok::r_brack) && !is(tok::eof)) {
      List.push_back(parseType());
      if (!atComma("type argument list", tok::r_brack))
        break;
      next();
    }
    --ExprLev;
    SourceLoc Closing = expectClosing(tok::r_brack, "type argument list");
    if (List.empty()) {
      errorExpected(Closing, "type argument list");
      Expr *Bad = badExpr(Opening.getLocWithOffset(1), Closing);
      List.push_back(Bad);
    }
    return packIndexExpr(Type, Opening, List, Closing);
  }

  /// Returns null if no type starts here.
  Expr *tryIdentOrType() {
    NestGuard G(*this);
    switch (Tok.Kind) {
    case tok::ident: {
      if (isRefTypeStart())
        return parseRefType();
      Expr *T = parseTypeName(nullptr);
      if (is(tok::l_brack))
        T = parseTypeInstance(T);
      return T;
    }
    case tok::l_brack: {
      SourceLoc Lbrack = expect(tok::l_brack);
      return parseArrayType(Lbrack, nullptr);
    }
    case tok::kw_struct:
      return parseStructType();
    case tok::mul:
      return parsePointerType();
    case tok::kw_func:
      return parseFuncType();
    case tok::kw_interface:
      return parseInterfaceType();
    case tok::kw_map:
      return parseMapType();
    case tok::kw_chan:
    case tok::arrow:
      return parseChanType();
    case tok::l_paren: {
      SourceLoc Lparen = Tok.Loc;
      next();
      Expr *T = parseType();
      SourceLoc Rparen = expect(tok::r_paren);
      ParenExpr *P = mk<ParenExpr>(Lparen);
      P->Lparen = Lparen;
      P->Rparen = Rparen;
      P->X = T;
      return P;
    }
    default:
      return nullptr; // no type found
    }
  }

  //===--------------------------------------------------------------------===//
  // Macros and attributes (P02.06)
  //===--------------------------------------------------------------------===//

  /// The current token is '('. Collects the tokens up to the matching ')' as a
  /// flat, bracket-balanced list. Automatic semicolons are kept, except a
  /// newline right before the closing ')'. Returns false after a syntax error
  /// (an unterminated or mismatched bracket).
  bool parseTokenTree(ArrayRef<Token> &Args, SourceLoc &Lparen,
                      SourceLoc &Rparen) {
    Lparen = Tok.Loc;
    next();
    SmallVector<Token, 16> Toks;
    SmallVector<Token, 16> Open; // brackets opened inside the arguments
    auto CloserOf = [](tok::Kind K) {
      return K == tok::l_paren   ? tok::r_paren
             : K == tok::l_brack ? tok::r_brack
                                 : tok::r_brace;
    };
    bool Ok = true;
    for (;;) {
      if (is(tok::eof)) {
        error(Lparen, diag::err_macro_args_unterminated);
        Rparen = Tok.Loc;
        Ok = false;
        break;
      }
      if (is(tok::r_paren) || is(tok::r_brack) || is(tok::r_brace)) {
        if (!Open.empty() && CloserOf(Open.back().Kind) == Tok.Kind) {
          Open.pop_back();
        } else if (Open.empty() && is(tok::r_paren)) {
          Rparen = Tok.Loc;
          next();
          break;
        } else {
          // A note points at the bracket left open: the innermost one, or
          // the '(' of the arguments.
          tok::Kind OpenKind = Open.empty() ? tok::l_paren : Open.back().Kind;
          SourceLoc OpenLoc = Open.empty() ? Lparen : Open.back().Loc;
          if (shouldReport(Tok.Loc)) {
            DiagnosticBuilder B = Diags.report(
                Tok.Loc, Open.empty()
                             ? diag::err_macro_args_mismatched
                             : diag::err_macro_args_mismatched_expected);
            B << tokString(Tok.Kind);
            if (!Open.empty())
              B << tokString(CloserOf(OpenKind));
            B.note(OpenLoc, diag::note_to_match_this, {tokString(OpenKind)});
          }
          Rparen = Tok.Loc;
          Ok = false;
          break;
        }
      } else if (is(tok::l_paren) || is(tok::l_brack) || is(tok::l_brace)) {
        Open.push_back(Tok);
      } else if (Tok.isAutoSemi() && Open.empty() && peek(1).is(tok::r_paren)) {
        next(); // a newline before the closing ')' is not part of the arguments
        continue;
      }
      Toks.push_back(Tok);
      next();
    }
    Args = Ctx.copy(ArrayRef<Token>(Toks));
    return Ok;
  }

  /// MacroRef = [ PackageName "." ] MacroName. Names start with a capital
  /// letter (MAC-4); non-ASCII first letters are not checked here.
  void parseMacroRef(Ident *&Pkg, Ident *&Name) {
    Pkg = nullptr;
    Name = parseIdent();
    if (is(tok::period)) {
      next();
      Pkg = Name;
      Name = parseIdent();
    }
    checkMacroName(Name);
  }

  void checkMacroName(const Ident *Name) {
    llvm::StringRef S = Name->Name.str();
    if (!S.empty() && S[0] >= 'a' && S[0] <= 'z')
      error(Name->Begin, diag::err_macro_name_lowercase, {S});
  }

  /// '#' MacroRef '(' tokens ')'
  MacroInvocation *parseMacroInvocation() {
    SourceLoc Hash = expect(tok::hash);
    MacroInvocation *M = mk<MacroInvocation>(Hash);
    M->HashLoc = Hash;
    parseMacroRef(M->Pkg, M->Name);
    if (is(tok::l_paren)) {
      parseTokenTree(M->Args, M->Lparen, M->Rparen);
    } else {
      errorExpected(Tok.Loc, "'('");
      M->Lparen = M->Rparen = Tok.Loc;
    }
    M->End = PrevEnd;
    return M;
  }

  /// '@' MacroRef [ '(' tokens ')' ]
  Attribute *parseAttribute() {
    SourceLoc At = expect(tok::at);
    Attribute *A = mk<Attribute>(At);
    A->AtLoc = At;
    parseMacroRef(A->Pkg, A->Name);
    if (is(tok::l_paren)) {
      A->HasArgs = true;
      parseTokenTree(A->Args, A->Lparen, A->Rparen);
    }
    A->End = PrevEnd;
    return A;
  }

  //===--------------------------------------------------------------------===//
  // Expressions
  //===--------------------------------------------------------------------===//

  BasicLit *parseBasicLit() {
    BasicLit *L = mk<BasicLit>(Tok.Loc, Tok.getEndLoc());
    L->LitKind = Tok.Kind;
    L->Text = text(Tok);
    next();
    return L;
  }

  Expr *parseFuncTypeOrLit() {
    FuncType *FT = parseFuncType();
    if (!is(tok::l_brace))
      return FT; // function type only
    ++ExprLev;
    BlockStmt *Body = parseBody();
    --ExprLev;
    FuncLit *F = mk<FuncLit>(FT->Begin);
    F->Type = FT;
    F->Body = Body;
    return F;
  }

  /// May return an expression or a raw type (including `[...]T`).
  Expr *parseOperand() {
    switch (Tok.Kind) {
    case tok::ident:
      if (isRefTypeStart())
        return parseRefType();
      return parseIdent();
    case tok::int_lit:
    case tok::float_lit:
    case tok::imag_lit:
    case tok::char_lit:
    case tok::string_lit:
      return parseBasicLit();
    case tok::l_paren: {
      SourceLoc Lparen = Tok.Loc;
      next();
      ++ExprLev;
      Expr *X = parseRhs(); // types may be parenthesized: (some type)
      --ExprLev;
      SourceLoc Rparen = expect(tok::r_paren);
      ParenExpr *P = mk<ParenExpr>(Lparen);
      P->Lparen = Lparen;
      P->Rparen = Rparen;
      P->X = X;
      return P;
    }
    case tok::kw_func:
      return parseFuncTypeOrLit();
    case tok::hash:
      return parseMacroInvocation();
    default:
      break;
    }

    if (Expr *T = tryIdentOrType())
      return T; // could be a type for a composite literal or a conversion

    // we have an error
    SourceLoc L = Tok.Loc;
    errorExpected(L, "operand");
    advance(StmtSync);
    return badExprHere(L);
  }

  Expr *parseSelector(Expr *X) {
    Ident *Sel = parseIdent();
    SelectorExpr *S = mk<SelectorExpr>(X->Begin);
    S->X = X;
    S->Sel = Sel;
    return S;
  }

  Expr *parseTypeAssertion(Expr *X) {
    SourceLoc Lparen = expect(tok::l_paren);
    (void)Lparen;
    Expr *Type = nullptr;
    if (is(tok::kw_type))
      next(); // type switch: Type == null
    else
      Type = parseType();
    expect(tok::r_paren);
    TypeAssertExpr *T = mk<TypeAssertExpr>(X->Begin);
    T->X = X;
    T->Type = Type;
    return T;
  }

  Expr *parseIndexOrSliceOrInstance(Expr *X) {
    SourceLoc Lbrack = expect(tok::l_brack);
    if (is(tok::r_brack)) {
      // empty index, slice or index expressions are not permitted; accept them
      // for parsing tolerance, but complain
      errorExpected(Tok.Loc, "operand");
      SourceLoc Rbrack = Tok.Loc;
      next();
      Expr *Bad = badExpr(Rbrack, Rbrack);
      return packIndexExpr(X, Lbrack, ArrayRef<Expr *>(&Bad, 1), Rbrack);
    }
    ++ExprLev;

    constexpr unsigned N = 3; // change the 3 to 2 to disable 3-index slices
    SmallVector<Expr *, 8> Args;
    Expr *Index[N] = {};
    SourceLoc Colons[N - 1];
    if (!is(tok::colon))
      Index[0] = parseRhs();
    unsigned NColons = 0;
    switch (Tok.Kind) {
    case tok::colon:
      // slice expression
      while (is(tok::colon) && NColons < N - 1) {
        Colons[NColons] = Tok.Loc;
        ++NColons;
        next();
        if (!is(tok::colon) && !is(tok::r_brack) && !is(tok::eof))
          Index[NColons] = parseRhs();
      }
      break;
    case tok::comma:
      // instance expression
      Args.push_back(Index[0]);
      while (is(tok::comma)) {
        next();
        if (!is(tok::r_brack) && !is(tok::eof))
          Args.push_back(parseType());
      }
      break;
    default:
      break;
    }

    --ExprLev;
    SourceLoc Rbrack = expect(tok::r_brack);

    if (NColons > 0) {
      // slice expression
      bool Slice3 = false;
      if (NColons == 2) {
        Slice3 = true;
        // Check presence of the middle and final index here rather than during
        // type-checking to prevent erroneous programs from passing through.
        if (!Index[1]) {
          error(Colons[0], diag::err_slice3_middle_index);
          Index[1] = badExpr(Colons[0].getLocWithOffset(1), Colons[1]);
        }
        if (!Index[2]) {
          error(Colons[1], diag::err_slice3_final_index);
          Index[2] = badExpr(Colons[1].getLocWithOffset(1), Rbrack);
        }
      }
      SliceExpr *S = mk<SliceExpr>(X->Begin);
      S->X = X;
      S->Lbrack = Lbrack;
      S->Rbrack = Rbrack;
      S->Lo = Index[0];
      S->Hi = Index[1];
      S->Max = Index[2];
      S->Slice3 = Slice3;
      return S;
    }

    if (Args.empty()) {
      // index expression
      return packIndexExpr(X, Lbrack, ArrayRef<Expr *>(&Index[0], 1), Rbrack);
    }
    // instance expression
    return packIndexExpr(X, Lbrack, Args, Rbrack);
  }

  /// The '(' is current. BangLoc is valid for the unwrapping call `f!(...)`.
  CallExpr *parseCallOrConversion(Expr *Fun, SourceLoc BangLoc) {
    SourceLoc Lparen = expect(tok::l_paren);
    ++ExprLev;
    SmallVector<Expr *, 8> List;
    SourceLoc EllipsisLoc;
    while (!is(tok::r_paren) && !is(tok::eof) && !EllipsisLoc.isValid()) {
      List.push_back(
          parseRhs()); // builtins may expect a type: make(some type, ...)
      if (is(tok::ellipsis)) {
        EllipsisLoc = Tok.Loc;
        next();
      }
      if (!atComma("argument list", tok::r_paren))
        break;
      next();
    }
    --ExprLev;
    SourceLoc Rparen = expectClosing(tok::r_paren, "argument list");

    CallExpr *C = mk<CallExpr>(Fun->Begin);
    C->Fun = Fun;
    C->Lparen = Lparen;
    C->Rparen = Rparen;
    C->EllipsisLoc = EllipsisLoc;
    C->BangLoc = BangLoc;
    C->Args = Ctx.copy(ArrayRef<Expr *>(List));
    return C;
  }

  Expr *parseValue() {
    if (is(tok::l_brace))
      return parseLiteralValue(nullptr);
    return parseExpr();
  }

  Expr *parseElement() {
    Expr *X = parseValue();
    if (is(tok::colon)) {
      SourceLoc Colon = Tok.Loc;
      next();
      Expr *V = parseValue();
      KeyValueExpr *KV = mk<KeyValueExpr>(X->Begin);
      KV->Key = X;
      KV->Colon = Colon;
      KV->Value = V;
      X = KV;
    }
    return X;
  }

  void parseElementList(SmallVector<Expr *, 8> &List) {
    while (!is(tok::r_brace) && !is(tok::eof)) {
      List.push_back(parseElement());
      if (!atComma("composite literal", tok::r_brace))
        break;
      next();
    }
  }

  Expr *parseLiteralValue(Expr *Type) {
    NestGuard G(*this);
    SourceLoc Lbrace = expect(tok::l_brace);
    SmallVector<Expr *, 8> Elts;
    ++ExprLev;
    if (!is(tok::r_brace))
      parseElementList(Elts);
    --ExprLev;
    SourceLoc Rbrace = expectClosing(tok::r_brace, "composite literal");
    CompositeLit *C = mk<CompositeLit>(Type ? Type->Begin : Lbrace);
    C->Type = Type;
    C->Lbrace = Lbrace;
    C->Rbrace = Rbrace;
    C->Elts = Ctx.copy(ArrayRef<Expr *>(Elts));
    return C;
  }

  Expr *parsePrimaryExpr(Expr *X) {
    if (!X)
      X = parseOperand();
    for (unsigned N = 1;; ++N) {
      if (chainTooLong(N))
        return X;
      switch (Tok.Kind) {
      case tok::period:
        next();
        switch (Tok.Kind) {
        case tok::ident:
          X = parseSelector(X);
          break;
        case tok::l_paren:
          X = parseTypeAssertion(X);
          break;
        default: {
          SourceLoc L = Tok.Loc;
          errorExpected(L, "selector or type assertion");
          if (!is(tok::r_brace))
            next(); // make progress
          SelectorExpr *S = mk<SelectorExpr>(X->Begin);
          S->X = X;
          S->Sel = makeIdent("_", L, L);
          X = S;
          break;
        }
        }
        break;
      case tok::l_brack:
        X = parseIndexOrSliceOrInstance(X);
        break;
      case tok::l_paren:
        X = parseCallOrConversion(X, SourceLoc());
        break;
      case tok::not_:
        // `f!(x)`: the unwrapping call. A '!' after an operand can only be
        // this (LEX-5): != is a token of its own.
        if (!peek(1).is(tok::l_paren))
          return X;
        {
          SourceLoc Bang = Tok.Loc;
          next();
          X = parseCallOrConversion(X, Bang);
        }
        break;
      case tok::l_brace: {
        // operand may have returned a parenthesized complit type; accept it
        // but complain if we have a complit
        Expr *T = unparen(X);
        // determine if '{' belongs to a composite literal or a block statement
        switch (T->getKind()) {
        case Node::Kind::BadExpr:
        case Node::Kind::Ident:
        case Node::Kind::SelectorExpr:
        case Node::Kind::IndexExpr:
          if (ExprLev < 0)
            return X;
          // X is possibly a composite literal type
          break;
        case Node::Kind::ArrayType:
        case Node::Kind::SliceType:
        case Node::Kind::StructType:
        case Node::Kind::MapType:
          // X is a composite literal type
          break;
        default:
          return X;
        }
        if (T != X)
          error(T->Begin, diag::err_paren_complit_type);
        X = parseLiteralValue(X);
        break;
      }
      default:
        return X;
      }
    }
  }

  Expr *parseUnaryExpr() {
    NestGuard G(*this);
    switch (Tok.Kind) {
    case tok::add:
    case tok::sub:
    case tok::not_:
    case tok::xor_:
    case tok::and_:
    case tok::tilde: {
      SourceLoc Pos = Tok.Loc;
      tok::Kind Op = Tok.Kind;
      next();
      Expr *X = parseUnaryExpr();
      UnaryExpr *U = mk<UnaryExpr>(Pos, X->End);
      U->OpLoc = Pos;
      U->Op = Op;
      U->X = X;
      return U;
    }
    case tok::arrow: {
      // channel type or receive expression
      SourceLoc Arrow = Tok.Loc;
      next();

      // If the next token is `chan` we still don't know if it is a channel
      // type or a receive operation - we only know once we have found the end
      // of the unary expression. There are two cases:
      //   <- type  => (<-type) must be a channel type
      //   <- expr  => <-(expr) is a receive from an expression
      // In the first case, the arrow must be re-associated with the channel
      // type parsed already:
      //   <- (chan type)    =>  (<-chan type)
      //   <- (chan<- type)  =>  (<-chan (<-type))
      Expr *X = parseUnaryExpr();

      if (auto *Typ = dyn_cast<ChanType>(X)) {
        // (<-type): re-associate position info and <-
        ChanType::Dir Dir = ChanType::Send;
        bool Ok = true;
        while (Ok && Dir == ChanType::Send) {
          if (Typ->Direction == ChanType::Recv) {
            // error: (<-type) is (<-(<-chan T))
            errorExpected(Typ->ArrowLoc, "'chan'");
          }
          SourceLoc OldArrow = Typ->ArrowLoc;
          Typ->Begin = Arrow;
          Typ->ArrowLoc = Arrow;
          Arrow = OldArrow;
          Dir = Typ->Direction;
          Typ->Direction = ChanType::Recv;
          Typ = dyn_cast<ChanType>(Typ->Value);
          Ok = Typ != nullptr;
        }
        if (Dir == ChanType::Send)
          errorExpected(Arrow, "channel type");
        return X;
      }

      // <-(expr)
      UnaryExpr *U = mk<UnaryExpr>(Arrow, X->End);
      U->OpLoc = Arrow;
      U->Op = tok::arrow;
      U->X = X;
      return U;
    }
    case tok::mul: {
      // pointer type or unary "*" expression
      SourceLoc Pos = Tok.Loc;
      next();
      Expr *X = parseUnaryExpr();
      StarExpr *S = mk<StarExpr>(Pos, X->End);
      S->X = X;
      return S;
    }
    default:
      return parsePrimaryExpr(nullptr);
    }
  }

  tok::Kind tokPrecKind() const {
    // in a right-hand side "=" is almost certainly a mistyped "=="
    return InRhs && is(tok::assign) ? tok::eql : Tok.Kind;
  }

  Expr *parseBinaryExpr(Expr *X, int Prec1) {
    if (!X)
      X = parseUnaryExpr();
    for (unsigned N = 1;; ++N) {
      if (chainTooLong(N))
        return X;
      tok::Kind Op = tokPrecKind();
      int OPrec = precedence(Op);
      if (OPrec < Prec1)
        return X;
      SourceLoc Pos = expect(Op);
      Expr *Y = parseBinaryExpr(nullptr, OPrec + 1);
      BinaryExpr *B = mk<BinaryExpr>(X->Begin, Y->End);
      B->X = X;
      B->OpLoc = Pos;
      B->Op = Op;
      B->Y = Y;
      X = B;
    }
  }

  /// The result may be a type or even a raw type (`[...]int`).
  Expr *parseExpr() { return parseBinaryExpr(nullptr, LowestPrec + 1); }

  Expr *parseRhs() {
    bool Old = InRhs;
    InRhs = true;
    Expr *X = parseExpr();
    InRhs = Old;
    return X;
  }

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  struct SimpleResult {
    Stmt *S = nullptr;
    bool IsRange = false;
    SourceLoc RangeLoc;
    Expr *RangeX = nullptr;
  };

  SimpleResult parseSimpleStmt(SimpleMode Mode) {
    SimpleResult R;
    SmallVector<Expr *, 8> X;
    parseList(X, false);

    switch (Tok.Kind) {
    case tok::define:
    case tok::assign:
    case tok::add_assign:
    case tok::sub_assign:
    case tok::mul_assign:
    case tok::quo_assign:
    case tok::rem_assign:
    case tok::and_assign:
    case tok::or_assign:
    case tok::xor_assign:
    case tok::shl_assign:
    case tok::shr_assign:
    case tok::and_not_assign: {
      // assignment statement, possibly part of a range clause
      SourceLoc Pos = Tok.Loc;
      tok::Kind Op = Tok.Kind;
      next();
      SmallVector<Expr *, 8> Y;
      if (Mode == RangeOk && is(tok::kw_range) &&
          (Op == tok::define || Op == tok::assign)) {
        R.RangeLoc = Tok.Loc;
        next();
        R.RangeX = parseRhs();
        R.IsRange = true;
      } else {
        parseList(Y, true);
      }
      AssignStmt *A = mk<AssignStmt>(X[0]->Begin);
      A->Lhs = Ctx.copy(ArrayRef<Expr *>(X));
      A->OpLoc = Pos;
      A->Op = Op;
      A->Rhs = Ctx.copy(ArrayRef<Expr *>(Y));
      R.S = A;
      return R;
    }
    default:
      break;
    }

    if (X.size() > 1) {
      errorExpected(X[0]->Begin, "1 expression");
      // continue with the first expression
    }

    switch (Tok.Kind) {
    case tok::colon: {
      // labeled statement
      SourceLoc Colon = Tok.Loc;
      next();
      if (auto *Label = dyn_cast<Ident>(X[0]); Mode == LabelOk && Label) {
        // The scope of a label is the body of the function in which it is
        // declared and excludes the body of any nested function.
        LabeledStmt *L = mk<LabeledStmt>(Label->Begin);
        L->Label = Label;
        L->Colon = Colon;
        L->Body = parseStmt();
        L->End = L->Body->End;
        R.S = L;
        return R;
      }
      error(Colon, diag::err_illegal_label);
      R.S = mk<BadStmt>(X[0]->Begin, Colon.getLocWithOffset(1));
      return R;
    }
    case tok::arrow: {
      // send statement
      SourceLoc Arrow = Tok.Loc;
      next();
      Expr *Y = parseRhs();
      SendStmt *S = mk<SendStmt>(X[0]->Begin, Y->End);
      S->Chan = X[0];
      S->ArrowLoc = Arrow;
      S->Value = Y;
      R.S = S;
      return R;
    }
    case tok::inc:
    case tok::dec: {
      // increment or decrement
      IncDecStmt *S = mk<IncDecStmt>(X[0]->Begin, Tok.getEndLoc());
      S->X = X[0];
      S->OpLoc = Tok.Loc;
      S->Op = Tok.Kind;
      next();
      R.S = S;
      return R;
    }
    default:
      break;
    }

    // expression
    ExprStmt *S = mk<ExprStmt>(X[0]->Begin, X[0]->End);
    S->X = X[0];
    R.S = S;
    return R;
  }

  /// The call of a go or defer statement, or null after an error.
  Expr *parseCallExpr(llvm::StringRef CallType) {
    Expr *X = parseRhs(); // could be a conversion: (some type)(x)
    if (Expr *T = unparen(X); T != X) {
      error(X->Begin, diag::err_call_parenthesized, {CallType});
      X = T;
    }
    if (isa<CallExpr>(X))
      return X;
    if (!isa<BadExpr>(X)) {
      // only report error if it's a new one
      error(X->End, diag::err_call_expected, {CallType});
    }
    return nullptr;
  }

  Stmt *parseGoStmt() {
    SourceLoc Pos = expect(tok::kw_go);
    Expr *Call = parseCallExpr("go");
    SourceLoc End = PrevEnd;
    expectSemi();
    if (!Call)
      return mk<BadStmt>(Pos, Pos.getLocWithOffset(2)); // len("go")
    GoStmt *G = mk<GoStmt>(Pos, End);
    G->GoLoc = Pos;
    G->Call = Call;
    return G;
  }

  Stmt *parseDeferStmt() {
    SourceLoc Pos = expect(tok::kw_defer);
    Expr *Call = parseCallExpr("defer");
    SourceLoc End = PrevEnd;
    expectSemi();
    if (!Call)
      return mk<BadStmt>(Pos, Pos.getLocWithOffset(5)); // len("defer")
    DeferStmt *D = mk<DeferStmt>(Pos, End);
    D->DeferLoc = Pos;
    D->Call = Call;
    return D;
  }

  Stmt *parseReturnStmt() {
    SourceLoc Pos = Tok.Loc;
    expect(tok::kw_return);
    SmallVector<Expr *, 8> X;
    if (!is(tok::semi) && !is(tok::r_brace))
      parseList(X, true);
    SourceLoc End = PrevEnd;
    expectSemi();
    ReturnStmt *R = mk<ReturnStmt>(Pos, End);
    R->Results = Ctx.copy(ArrayRef<Expr *>(X));
    return R;
  }

  Stmt *parseBranchStmt(tok::Kind Keyword) {
    SourceLoc Pos = expect(Keyword);
    Ident *Label = nullptr;
    if (Keyword == tok::kw_goto ||
        ((Keyword == tok::kw_continue || Keyword == tok::kw_break) &&
         is(tok::ident)))
      Label = parseIdent();
    SourceLoc End = PrevEnd;
    expectSemi();
    BranchStmt *B = mk<BranchStmt>(Pos, End);
    B->Keyword = Keyword;
    B->Label = Label;
    return B;
  }

  Expr *makeExpr(Stmt *S, llvm::StringRef Want) {
    if (!S)
      return nullptr;
    if (auto *ES = dyn_cast<ExprStmt>(S))
      return ES->X;
    llvm::StringRef Found =
        isa<AssignStmt>(S) ? "assignment" : "simple statement";
    error(S->Begin, diag::err_expected_found_complit, {Want, Found});
    return badExpr(S->Begin, S->End);
  }

  /// An adjusted version of parser.header in cmd/compile/internal/syntax,
  /// tuned for better error handling (as in go/parser).
  void parseIfHeader(Stmt *&Init, Expr *&Cond) {
    Init = nullptr;
    Cond = nullptr;
    if (is(tok::l_brace)) {
      error(Tok.Loc, diag::err_missing_if_condition);
      Cond = badExpr(Tok.Loc, Tok.Loc);
      return;
    }

    int PrevLev = ExprLev;
    ExprLev = -1;

    if (!is(tok::semi)) {
      // accept potential variable declaration but complain
      if (is(tok::kw_var)) {
        next();
        error(Tok.Loc, diag::err_var_in_if_init);
      }
      Init = parseSimpleStmt(Basic).S;
    }

    Stmt *CondStmt = nullptr;
    SourceLoc SemiLoc;
    bool SemiIsNewline = false;
    if (!is(tok::l_brace)) {
      if (is(tok::semi)) {
        SemiLoc = Tok.Loc;
        SemiIsNewline = Tok.isAutoSemi();
        next();
      } else {
        expect(tok::semi);
      }
      if (!is(tok::l_brace))
        CondStmt = parseSimpleStmt(Basic).S;
    } else {
      CondStmt = Init;
      Init = nullptr;
    }

    if (CondStmt) {
      Cond = makeExpr(CondStmt, "boolean expression");
    } else if (SemiLoc.isValid()) {
      if (SemiIsNewline)
        error(SemiLoc, diag::err_newline_after_if_clause);
      else
        error(SemiLoc, diag::err_missing_if_condition);
    }

    // make sure we have a valid AST
    if (!Cond)
      Cond = badExpr(Tok.Loc, Tok.Loc);

    ExprLev = PrevLev;
  }

  Stmt *parseIfStmt() {
    NestGuard G(*this);
    SourceLoc Pos = expect(tok::kw_if);

    Stmt *Init;
    Expr *Cond;
    parseIfHeader(Init, Cond);
    BlockStmt *Body = parseBlockStmt();

    Stmt *Else = nullptr;
    if (is(tok::kw_else)) {
      next();
      switch (Tok.Kind) {
      case tok::kw_if:
        Else = parseIfStmt();
        break;
      case tok::l_brace:
        Else = parseBlockStmt();
        expectSemi();
        break;
      default:
        errorExpected(Tok.Loc, "if statement or block");
        Else = mk<BadStmt>(Tok.Loc, Tok.Loc);
        break;
      }
    } else {
      expectSemi();
    }

    IfStmt *S = mk<IfStmt>(Pos, Else ? Else->End : Body->End);
    S->Init = Init;
    S->Cond = Cond;
    S->Then = Body;
    S->Else = Else;
    return S;
  }

  void parseStmtList(SmallVector<Stmt *, 8> &List) {
    while (!is(tok::kw_case) && !is(tok::kw_default) && !is(tok::r_brace) &&
           !is(tok::eof)) {
      SourceLoc Before = Tok.Loc;
      List.push_back(parseStmt());
      if (Tok.Loc == Before && !is(tok::eof))
        next(); // no progress: never loop forever on bad input
    }
  }

  CaseClause *parseCaseClause() {
    SourceLoc Pos = Tok.Loc;
    CaseClause *C = mk<CaseClause>(Pos);
    SmallVector<Expr *, 8> List;
    if (is(tok::kw_case)) {
      next();
      parseList(List, true);
    } else {
      expect(tok::kw_default);
      C->IsDefault = true;
    }
    SourceLoc Colon = expect(tok::colon);
    SmallVector<Stmt *, 8> Body;
    parseStmtList(Body);
    C->CaseLoc = Pos;
    C->List = Ctx.copy(ArrayRef<Expr *>(List));
    C->Colon = Colon;
    C->Body = Ctx.copy(ArrayRef<Stmt *>(Body));
    C->End = PrevEnd;
    return C;
  }

  static bool isTypeSwitchAssert(const Expr *X) {
    auto *A = dyn_cast<TypeAssertExpr>(X);
    return A && !A->Type;
  }

  bool isTypeSwitchGuard(Stmt *S) {
    if (!S)
      return false;
    if (auto *ES = dyn_cast<ExprStmt>(S))
      return isTypeSwitchAssert(ES->X); // x.(type)
    if (auto *A = dyn_cast<AssignStmt>(S)) {
      // v := x.(type)
      if (A->Lhs.size() == 1 && A->Rhs.size() == 1 &&
          isTypeSwitchAssert(A->Rhs[0])) {
        if (A->Op == tok::assign) {
          // permit v = x.(type) but complain
          error(A->OpLoc, diag::err_expected_found, {"':='", "'='"});
          return true;
        }
        return A->Op == tok::define;
      }
    }
    return false;
  }

  Stmt *parseSwitchStmt() {
    SourceLoc Pos = expect(tok::kw_switch);

    Stmt *S1 = nullptr, *S2 = nullptr;
    if (!is(tok::l_brace)) {
      int PrevLev = ExprLev;
      ExprLev = -1;
      if (!is(tok::semi))
        S2 = parseSimpleStmt(Basic).S;
      if (is(tok::semi)) {
        next();
        S1 = S2;
        S2 = nullptr;
        if (!is(tok::l_brace)) {
          // A TypeSwitchGuard may declare a variable in addition to the
          // variable declared in the initial SimpleStmt. If we don't have a
          // type switch, S2 must be an expression.
          S2 = parseSimpleStmt(Basic).S;
        }
      }
      ExprLev = PrevLev;
    }

    bool TypeSwitch = isTypeSwitchGuard(S2);
    SourceLoc Lbrace = expect(tok::l_brace);
    SmallVector<Stmt *, 8> List;
    while (is(tok::kw_case) || is(tok::kw_default))
      List.push_back(parseCaseClause());
    SourceLoc Rbrace = expect(tok::r_brace);
    SourceLoc End = PrevEnd;
    expectSemi();
    BlockStmt *Body = mk<BlockStmt>(Lbrace, End);
    Body->Lbrace = Lbrace;
    Body->Rbrace = Rbrace;
    Body->List = Ctx.copy(ArrayRef<Stmt *>(List));

    if (TypeSwitch) {
      TypeSwitchStmt *T = mk<TypeSwitchStmt>(Pos, End);
      T->Init = S1;
      T->Assign = S2;
      T->Body = Body;
      return T;
    }
    SwitchStmt *S = mk<SwitchStmt>(Pos, End);
    S->Init = S1;
    S->Tag = makeExpr(S2, "switch expression");
    S->Body = Body;
    return S;
  }

  CommClause *parseCommClause() {
    SourceLoc Pos = Tok.Loc;
    Stmt *Comm = nullptr;
    if (is(tok::kw_case)) {
      next();
      SmallVector<Expr *, 8> Lhs;
      parseList(Lhs, false);
      if (is(tok::arrow)) {
        // SendStmt
        if (Lhs.size() > 1) {
          errorExpected(Lhs[0]->Begin, "1 expression");
          // continue with the first expression
        }
        SourceLoc Arrow = Tok.Loc;
        next();
        Expr *Rhs = parseRhs();
        SendStmt *S = mk<SendStmt>(Lhs[0]->Begin, Rhs->End);
        S->Chan = Lhs[0];
        S->ArrowLoc = Arrow;
        S->Value = Rhs;
        Comm = S;
      } else if (is(tok::assign) || is(tok::define)) {
        // RecvStmt with assignment
        tok::Kind Op = Tok.Kind;
        if (Lhs.size() > 2) {
          errorExpected(Lhs[0]->Begin, "1 or 2 expressions");
          // continue with the first two expressions
          Lhs.resize(2);
        }
        SourceLoc OpLoc = Tok.Loc;
        next();
        Expr *Rhs = parseRhs();
        AssignStmt *A = mk<AssignStmt>(Lhs[0]->Begin, Rhs->End);
        A->Lhs = Ctx.copy(ArrayRef<Expr *>(Lhs));
        A->OpLoc = OpLoc;
        A->Op = Op;
        A->Rhs = Ctx.copy(ArrayRef<Expr *>(&Rhs, 1));
        Comm = A;
      } else {
        // lhs must be a single receive operation
        if (Lhs.size() > 1) {
          errorExpected(Lhs[0]->Begin, "1 expression");
          // continue with the first expression
        }
        ExprStmt *E = mk<ExprStmt>(Lhs[0]->Begin, Lhs[0]->End);
        E->X = Lhs[0];
        Comm = E;
      }
    } else {
      expect(tok::kw_default);
    }

    SourceLoc Colon = expect(tok::colon);
    SmallVector<Stmt *, 8> Body;
    parseStmtList(Body);
    CommClause *C = mk<CommClause>(Pos);
    C->CaseLoc = Pos;
    C->Comm = Comm;
    C->Colon = Colon;
    C->Body = Ctx.copy(ArrayRef<Stmt *>(Body));
    return C;
  }

  Stmt *parseSelectStmt() {
    SourceLoc Pos = expect(tok::kw_select);
    SourceLoc Lbrace = expect(tok::l_brace);
    SmallVector<Stmt *, 8> List;
    while (is(tok::kw_case) || is(tok::kw_default))
      List.push_back(parseCommClause());
    SourceLoc Rbrace = expect(tok::r_brace);
    SourceLoc End = PrevEnd;
    expectSemi();
    BlockStmt *Body = mk<BlockStmt>(Lbrace, End);
    Body->Lbrace = Lbrace;
    Body->Rbrace = Rbrace;
    Body->List = Ctx.copy(ArrayRef<Stmt *>(List));
    SelectStmt *S = mk<SelectStmt>(Pos, End);
    S->Body = Body;
    return S;
  }

  /// `for k, &v := range x`: the `&` mark of D1.21 (RNG). Unwraps `&v` into an
  /// alias flag on the statement and validates where it may appear.
  Expr *takeRangeAlias(Expr *E, bool IsValue, bool IsOnly, tok::Kind Op,
                       RangeStmt *R) {
    auto *U = dyn_cast<UnaryExpr>(E);
    if (!U || U->Op != tok::and_ || !isa<Ident>(U->X))
      return E;
    if (Op != tok::define)
      error(U->OpLoc, diag::err_range_alias_assign);
    else if (!IsValue && !IsOnly)
      error(U->OpLoc, diag::err_range_alias_key);
    R->Alias =
        IsValue ? RangeStmt::AliasKind::Value : RangeStmt::AliasKind::Key;
    R->AliasLoc = U->OpLoc;
    return U->X;
  }

  Stmt *parseForStmt() {
    SourceLoc Pos = expect(tok::kw_for);

    Stmt *S1 = nullptr, *S2 = nullptr, *S3 = nullptr;
    SimpleResult Range;
    bool IsRange = false;
    if (!is(tok::l_brace)) {
      int PrevLev = ExprLev;
      ExprLev = -1;
      if (!is(tok::semi)) {
        if (is(tok::kw_range)) {
          // "for range x" (no variables)
          Range.RangeLoc = Tok.Loc;
          next();
          Range.RangeX = parseRhs();
          Range.IsRange = true;
          AssignStmt *A = mk<AssignStmt>(Range.RangeLoc, PrevEnd);
          Range.S = A;
          S2 = A;
        } else {
          Range = parseSimpleStmt(RangeOk);
          S2 = Range.S;
        }
        IsRange = Range.IsRange;
      }
      if (!IsRange && is(tok::semi)) {
        next();
        S1 = S2;
        S2 = nullptr;
        if (!is(tok::semi))
          S2 = parseSimpleStmt(Basic).S;
        expectSemi();
        if (!is(tok::l_brace))
          S3 = parseSimpleStmt(Basic).S;
      }
      ExprLev = PrevLev;
    }

    BlockStmt *Body = parseBlockStmt();
    SourceLoc End = PrevEnd;
    expectSemi();

    if (IsRange) {
      auto *As = cast<AssignStmt>(S2);
      // check lhs
      if (As->Lhs.size() > 2) {
        errorExpected(As->Lhs.back()->Begin, "at most 2 expressions");
        return mk<BadStmt>(Pos, Body->End);
      }
      RangeStmt *R = mk<RangeStmt>(Pos, Body->End);
      R->Op = As->Lhs.empty() ? tok::eof : As->Op;
      R->OpLoc = As->OpLoc;
      if (As->Lhs.size() == 1) {
        R->Key = takeRangeAlias(As->Lhs[0], false, true, As->Op, R);
      } else if (As->Lhs.size() == 2) {
        R->Key = takeRangeAlias(As->Lhs[0], false, false, As->Op, R);
        R->Value = takeRangeAlias(As->Lhs[1], true, false, As->Op, R);
      }
      R->RangeLoc = Range.RangeLoc;
      R->X = Range.RangeX;
      R->Body = Body;
      (void)End;
      return R;
    }

    // regular for statement
    ForStmt *F = mk<ForStmt>(Pos, Body->End);
    F->Init = S1;
    F->Cond = makeExpr(S2, "boolean or range expression");
    F->Post = S3;
    F->Body = Body;
    return F;
  }

  Stmt *parseStmt() {
    NestGuard G(*this);
    switch (Tok.Kind) {
    case tok::kw_const:
    case tok::kw_type:
    case tok::kw_var: {
      DeclStmt *S = mk<DeclStmt>(Tok.Loc);
      S->D = parseGenDecl({});
      S->End = S->D->End;
      return S;
    }
    // tokens that may start an expression
    case tok::ident:
    case tok::int_lit:
    case tok::float_lit:
    case tok::imag_lit:
    case tok::char_lit:
    case tok::string_lit:
    case tok::kw_func:
    case tok::l_paren: // operands
    case tok::l_brack:
    case tok::kw_struct:
    case tok::kw_map:
    case tok::kw_chan:
    case tok::kw_interface: // composite types
    case tok::add:
    case tok::sub:
    case tok::mul:
    case tok::and_:
    case tok::xor_:
    case tok::arrow:
    case tok::not_:   // unary operators
    case tok::hash: { // macro invocation
      Stmt *S = parseSimpleStmt(LabelOk).S;
      // because of the required look-ahead, labeled statements are parsed by
      // parseSimpleStmt - don't expect a semicolon after them
      if (!isa<LabeledStmt>(S))
        expectSemi();
      return S;
    }
    case tok::kw_go:
      return parseGoStmt();
    case tok::kw_defer:
      return parseDeferStmt();
    case tok::kw_return:
      return parseReturnStmt();
    case tok::kw_break:
    case tok::kw_continue:
    case tok::kw_goto:
    case tok::kw_fallthrough:
      return parseBranchStmt(Tok.Kind);
    case tok::l_brace: {
      Stmt *S = parseBlockStmt();
      expectSemi();
      return S;
    }
    case tok::kw_if:
      return parseIfStmt();
    case tok::kw_switch:
      return parseSwitchStmt();
    case tok::kw_select:
      return parseSelectStmt();
    case tok::kw_for:
      return parseForStmt();
    case tok::semi: {
      // Is it ever possible to have an implicit semicolon producing an empty
      // statement in a valid program? (handle correctly anyway)
      EmptyStmt *S = mk<EmptyStmt>(Tok.Loc, Tok.getEndLoc());
      S->Implicit = Tok.isAutoSemi();
      next();
      return S;
    }
    case tok::r_brace: {
      // a semicolon may be omitted before a closing "}"
      EmptyStmt *S = mk<EmptyStmt>(Tok.Loc, Tok.Loc);
      S->Implicit = true;
      return S;
    }
    case tok::at: {
      SourceLoc L = Tok.Loc;
      error(L, diag::err_attr_on_stmt);
      parseAttribute();
      return mk<BadStmt>(L, PrevEnd);
    }
    default: {
      // no statement found
      SourceLoc L = Tok.Loc;
      errorExpected(L, "statement");
      advance(StmtSync);
      return badExprStmt(L);
    }
    }
  }

  Stmt *badExprStmt(SourceLoc From) {
    return mk<BadStmt>(From, Tok.Loc < From ? From : Tok.Loc);
  }

  BlockStmt *parseBody() {
    NestGuard G(*this);
    SourceLoc Lbrace = expect(tok::l_brace);
    SmallVector<Stmt *, 8> List;
    parseStmtList(List);
    bool Closed = is(tok::r_brace);
    SourceLoc Rbrace = expect2(tok::r_brace);
    BlockStmt *B =
        mk<BlockStmt>(Lbrace, Closed ? Rbrace.getLocWithOffset(1) : PrevEnd);
    B->Lbrace = Lbrace;
    B->Rbrace = Rbrace;
    B->List = Ctx.copy(ArrayRef<Stmt *>(List));
    return B;
  }

  BlockStmt *parseBlockStmt() { return parseBody(); }

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  Spec *parseImportSpec() {
    SourceLoc Begin = Tok.Loc;
    Ident *Name = nullptr;
    switch (Tok.Kind) {
    case tok::ident:
      Name = parseIdent();
      break;
    case tok::period:
      Name = makeIdent(".", Tok.Loc, Tok.getEndLoc());
      next();
      break;
    default:
      break;
    }

    SourceLoc Pos = Tok.Loc;
    BasicLit *Path;
    if (is(tok::string_lit)) {
      // Any string: "fmt", "github.com/x/y", "<vector>" (D2.7).
      Path = parseBasicLit();
    } else {
      if (tok::isLiteral(Tok.Kind) && !is(tok::ident)) {
        error(Pos, diag::err_import_path_not_string);
        next();
      } else {
        error(Pos, diag::err_missing_import_path);
        advance(ExprEnd);
      }
      Path = mk<BasicLit>(Pos, Pos);
      Path->LitKind = tok::string_lit;
    }
    SourceLoc End = PrevEnd;
    expectSemi();

    ImportSpec *S = mk<ImportSpec>(Begin < Pos ? Begin : Pos, End);
    S->Name = Name;
    S->Path = Path;
    Imports.push_back(S);
    return S;
  }

  Spec *parseValueSpec(tok::Kind Keyword) {
    SourceLoc Begin = Tok.Loc;
    SmallVector<Ident *, 8> Idents;
    parseIdentList(Idents);
    Expr *Type = nullptr;
    SmallVector<Expr *, 8> Values;
    if (Keyword == tok::kw_const) {
      // always permit an optional type and initialization for more tolerant
      // parsing
      if (!is(tok::eof) && !is(tok::semi) && !is(tok::r_paren)) {
        Type = tryIdentOrType();
        if (is(tok::assign)) {
          next();
          parseList(Values, true);
        }
      }
    } else {
      if (!is(tok::assign))
        Type = parseType();
      if (is(tok::assign)) {
        next();
        parseList(Values, true);
      }
    }
    SourceLoc End = PrevEnd;
    expectSemi();
    ValueSpec *S = mk<ValueSpec>(Begin, End);
    S->Names = Ctx.copy(ArrayRef<Ident *>(Idents));
    S->Type = Type;
    S->Values = Ctx.copy(ArrayRef<Expr *>(Values));
    return S;
  }

  void parseGenericType(TypeSpec *Spec, SourceLoc OpenPos, Ident *Name0,
                        Expr *Typ0) {
    SmallVector<Field *, 8> List;
    parseParameterList(Name0, Typ0, tok::r_brack, false, false, List);
    SourceLoc ClosePos = expect(tok::r_brack);
    Spec->TypeParams = finishList(OpenPos, ClosePos, List);
    if (is(tok::assign)) {
      // type alias
      Spec->AssignLoc = Tok.Loc;
      next();
    }
    Spec->Type = parseType();
  }

  /// Splits X into (name, expr) if syntactically X can be written as
  /// `name expr`. The split only happens if expr is a type element or Force is
  /// set. If X is just a name the result is (name, null). If the split fails,
  /// the result is (null, X). (extractName of go/parser.)
  std::pair<Ident *, Expr *> extractName(Expr *X, bool Force) {
    if (auto *I = dyn_cast<Ident>(X))
      return {I, nullptr};
    if (auto *B = dyn_cast<BinaryExpr>(X)) {
      if (B->Op == tok::mul) {
        if (auto *Name = dyn_cast<Ident>(B->X);
            Name && (Force || isTypeElem(B->Y))) {
          // X = name *B->Y
          StarExpr *S = mk<StarExpr>(B->OpLoc, B->Y->End);
          S->X = B->Y;
          return {Name, S};
        }
      } else if (B->Op == tok::or_) {
        auto [Name, Lhs] = extractName(B->X, Force || isTypeElem(B->Y));
        if (Name && Lhs) {
          // X = name Lhs|B->Y
          BinaryExpr *Op = mk<BinaryExpr>(Lhs->Begin, B->End);
          Op->X = Lhs;
          Op->OpLoc = B->OpLoc;
          Op->Op = B->Op;
          Op->Y = B->Y;
          return {Name, Op};
        }
      }
    } else if (auto *C = dyn_cast<CallExpr>(X)) {
      if (auto *Name = dyn_cast<Ident>(C->Fun)) {
        if (C->Args.size() == 1 && !C->EllipsisLoc.isValid() && !C->isMust() &&
            (Force || isTypeElem(C->Args[0]))) {
          // X = name "(" C->Args[0] ")"
          return {Name, C->Args[0]};
        }
      }
    }
    return {nullptr, X};
  }

  static bool isTypeElem(const Expr *X) {
    switch (X->getKind()) {
    case Node::Kind::ArrayType:
    case Node::Kind::SliceType:
    case Node::Kind::StructType:
    case Node::Kind::FuncType:
    case Node::Kind::InterfaceType:
    case Node::Kind::MapType:
    case Node::Kind::ChanType:
    case Node::Kind::RefType:
      return true;
    case Node::Kind::BinaryExpr: {
      auto *B = cast<BinaryExpr>(X);
      return isTypeElem(B->X) || isTypeElem(B->Y);
    }
    case Node::Kind::UnaryExpr:
      return cast<UnaryExpr>(X)->Op == tok::tilde;
    case Node::Kind::ParenExpr:
      return isTypeElem(cast<ParenExpr>(X)->X);
    default:
      return false;
    }
  }

  Spec *parseTypeSpec() {
    SourceLoc Begin = Tok.Loc;
    Ident *Name = parseIdent();
    TypeSpec *Spec = mk<TypeSpec>(Begin);
    Spec->Name = Name;

    if (is(tok::l_brack)) {
      // Spec.Name "[" ...: array/slice type or type parameter list
      SourceLoc Lbrack = Tok.Loc;
      next();
      if (is(tok::ident)) {
        // We may have an array type or a type parameter list. In either case
        // we expect an expression X (which may just be a name, or a more
        // complex expression) which we can analyze further.
        //
        // A type parameter list may have a type bound starting with a "[" as
        // in: P []E. In that case, simply parsing an expression would lead to
        // an error: P[] is invalid. But since index or slice expressions are
        // never constant and thus invalid array length expressions, if the name
        // is followed by "[" it must be the start of an array or slice
        // constraint. Only if we don't see a "[" do we need to parse a full
        // expression.
        Expr *X = parseIdent();
        if (!is(tok::l_brack)) {
          // To parse the expression starting with name, expand the call
          // sequence we would get by passing in name to parseExpr, and pass in
          // name to parsePrimaryExpr.
          ++ExprLev;
          Expr *Lhs = parsePrimaryExpr(X);
          X = parseBinaryExpr(Lhs, LowestPrec + 1);
          --ExprLev;
        }
        // Analyze expression X. If we can split X into a type parameter name,
        // possibly followed by a type parameter type, we consider this the
        // start of a type parameter list, with some caveats: a single name
        // followed by "]" tilts the decision towards an array declaration; a
        // type parameter type that could also be an ordinary expression but
        // which is followed by a comma tilts the decision towards a type
        // parameter list.
        auto [PName, PType] = extractName(X, is(tok::comma));
        if (PName && (PType || !is(tok::r_brack))) {
          // Spec.Name "[" pname ...
          parseGenericType(Spec, Lbrack, PName, PType); // PType may be null
        } else {
          // Spec.Name "[" pname "]" ...
          // Spec.Name "[" X ...
          Spec->Type = parseArrayType(Lbrack, X);
        }
      } else {
        // array type
        Spec->Type = parseArrayType(Lbrack, nullptr);
      }
    } else {
      // no type parameters
      if (is(tok::assign)) {
        // type alias
        Spec->AssignLoc = Tok.Loc;
        next();
      }
      Spec->Type = parseType();
    }

    Spec->End = PrevEnd;
    expectSemi();
    return Spec;
  }

  GenDecl *parseGenDecl(ArrayRef<Attribute *> Attrs) {
    tok::Kind Keyword = Tok.Kind;
    SourceLoc Pos = expect(Keyword);
    GenDecl *D = mk<GenDecl>(Attrs.empty() ? Pos : Attrs.front()->Begin);
    D->Attrs = Attrs;
    D->KeywordLoc = Pos;
    D->Keyword = Keyword;
    SmallVector<Spec *, 8> List;
    auto ParseSpec = [&]() -> Spec * {
      switch (Keyword) {
      case tok::kw_import:
        return parseImportSpec();
      case tok::kw_type:
        return parseTypeSpec();
      default:
        return parseValueSpec(Keyword);
      }
    };
    if (is(tok::l_paren)) {
      D->Lparen = Tok.Loc;
      next();
      while (!is(tok::r_paren) && !is(tok::eof)) {
        SourceLoc Before = Tok.Loc;
        List.push_back(ParseSpec());
        if (Tok.Loc == Before && !is(tok::eof))
          next(); // no progress: never loop forever on bad input
      }
      D->Rparen = expect(tok::r_paren);
      D->End = PrevEnd;
      expectSemi();
    } else {
      List.push_back(ParseSpec());
      D->End = List.back()->End;
    }
    D->Specs = Ctx.copy(ArrayRef<Spec *>(List));
    return D;
  }

  FuncDecl *parseFuncDecl(ArrayRef<Attribute *> Attrs) {
    SourceLoc Pos = expect(tok::kw_func);

    FieldList *Recv = nullptr;
    if (is(tok::l_paren))
      Recv = parseParameters(false);

    Ident *Name = parseIdent();

    FuncType *FT = mk<FuncType>(Pos);
    FT->FuncLoc = Pos;
    if (is(tok::l_brack)) {
      FieldList *TParams = parseTypeParameters();
      if (Recv && TParams) {
        // Method declarations do not have type parameters. We parse them for a
        // better error message and improved error recovery.
        error(TParams->Opening, diag::err_method_type_params);
        TParams = nullptr;
      }
      FT->TypeParams = TParams;
    }
    parseSignature(FT);

    BlockStmt *Body = nullptr;
    switch (Tok.Kind) {
    case tok::l_brace:
      Body = parseBody();
      expectSemi();
      break;
    case tok::semi:
      next();
      if (is(tok::l_brace)) {
        // opening { of the function declaration on the next line
        error(Tok.Loc, diag::err_newline_before_lbrace);
        Body = parseBody();
        expectSemi();
      }
      break;
    default:
      expectSemi();
      break;
    }

    FuncDecl *D = mk<FuncDecl>(Attrs.empty() ? Pos : Attrs.front()->Begin,
                               Body ? Body->End : FT->End);
    D->Attrs = Attrs;
    D->Recv = Recv;
    D->Name = Name;
    D->Type = FT;
    D->Body = Body;
    return D;
  }

  /// `macro Name(p kind, ...) { ... }` or `macro func Name[!](...) ... { ... }`
  Decl *parseMacroDecl() {
    SourceLoc Pos = expect(tok::kw_macro);
    if (is(tok::kw_func)) {
      SourceLoc FuncLoc = Tok.Loc;
      next();
      Ident *Name = parseIdent();
      checkMacroName(Name);
      FuncType *FT = mk<FuncType>(FuncLoc);
      FT->FuncLoc = FuncLoc;
      parseSignature(FT);
      BlockStmt *Body = nullptr;
      if (is(tok::l_brace)) {
        Body = parseBody();
      } else {
        errorExpected(Tok.Loc, "'{'");
      }
      ProcMacroDecl *D = mk<ProcMacroDecl>(Pos);
      D->MacroLoc = Pos;
      D->FuncLoc = FuncLoc;
      D->Name = Name;
      D->Type = FT;
      D->Body = Body;
      D->End = PrevEnd;
      expectSemi();
      return D;
    }

    Ident *Name = parseIdent();
    checkMacroName(Name);
    SourceLoc Lparen = expect(tok::l_paren);
    SmallVector<MacroParam *, 8> Params;
    while (!is(tok::r_paren) && !is(tok::eof)) {
      SourceLoc Before = Tok.Loc;
      MacroParam *P = mk<MacroParam>(Tok.Loc);
      P->Name = parseIdent();
      if (is(tok::ellipsis)) {
        P->Variadic = true;
        next();
      }
      P->KindLoc = Tok.Loc;
      P->ParamKind = parseMacroKind();
      P->End = PrevEnd;
      Params.push_back(P);
      if (!atComma("macro parameter list", tok::r_paren))
        break;
      next();
      if (Tok.Loc == Before && !is(tok::eof))
        next();
    }
    SourceLoc Rparen = expect(tok::r_paren);
    BlockStmt *Body = nullptr;
    if (is(tok::l_brace)) {
      Body = parseBody();
    } else {
      errorExpected(Tok.Loc, "'{'");
    }
    MacroDecl *D = mk<MacroDecl>(Pos);
    D->MacroLoc = Pos;
    D->Name = Name;
    D->Lparen = Lparen;
    D->Rparen = Rparen;
    D->Params = Ctx.copy(ArrayRef<MacroParam *>(Params));
    D->Body = Body;
    D->End = PrevEnd;
    expectSemi();
    return D;
  }

  MacroKind parseMacroKind() {
    MacroKind K = MacroKind::Invalid;
    if (is(tok::kw_type)) {
      K = MacroKind::Type;
      next();
    } else if (is(tok::ident)) {
      llvm::StringRef S = text(Tok);
      if (S == "expr")
        K = MacroKind::Expr;
      else if (S == "stmt")
        K = MacroKind::Stmt;
      else if (S == "block")
        K = MacroKind::Block;
      else if (S == "ident")
        K = MacroKind::Ident;
      else if (S == "lit")
        K = MacroKind::Lit;
      else if (S == "decl")
        K = MacroKind::Decl;
      else if (S == "tokens")
        K = MacroKind::Tokens;
      else
        error(Tok.Loc, diag::err_unknown_macro_kind, {S});
      next();
    } else {
      errorExpected(Tok.Loc, "macro parameter kind");
    }
    return K;
  }

  /// A declaration at the top level, with the leading attributes if any.
  Decl *parseDecl() {
    switch (Tok.Kind) {
    case tok::kw_import:
    case tok::kw_const:
    case tok::kw_var:
    case tok::kw_type:
      return parseGenDecl({});
    case tok::kw_func:
      return parseFuncDecl({});
    case tok::kw_macro:
      return parseMacroDecl();
    case tok::hash: {
      MacroCallDecl *D = mk<MacroCallDecl>(Tok.Loc);
      D->Call = parseMacroInvocation();
      D->End = PrevEnd;
      expectSemi();
      return D;
    }
    case tok::at:
      return parseAttributedDecl();
    default: {
      SourceLoc L = Tok.Loc;
      errorExpected(L, "declaration");
      advance(DeclStart);
      BadDecl *D = mk<BadDecl>(L, Tok.Loc < L ? L : Tok.Loc);
      return D;
    }
    }
  }

  /// Attribute { [ ";" ] Attribute } [ ";" ] ( Declaration | FunctionDecl )
  Decl *parseAttributedDecl() {
    SmallVector<Attribute *, 8> Attrs;
    while (is(tok::at)) {
      Attrs.push_back(parseAttribute());
      while (is(tok::semi))
        next();
    }
    ArrayRef<Attribute *> A = Ctx.copy(ArrayRef<Attribute *>(Attrs));
    switch (Tok.Kind) {
    case tok::kw_const:
    case tok::kw_var:
    case tok::kw_type:
      return parseGenDecl(A);
    case tok::kw_func:
      return parseFuncDecl(A);
    default: {
      SourceLoc L = Tok.Loc;
      if (is(tok::kw_import))
        error(L, diag::err_attr_on_import);
      else
        errorExpected(L, "declaration after attributes");
      advance(DeclStart);
      BadDecl *D = mk<BadDecl>(Attrs.front()->Begin, Tok.Loc < L ? L : Tok.Loc);
      return D;
    }
    }
  }

  //===--------------------------------------------------------------------===//
  // Files
  //===--------------------------------------------------------------------===//

  ASTContext &Ctx;
  const SourceManager &SM;
  FileID File;
  DiagnosticsEngine &Diags;
  ParseOptions Opts;
  // The lexer reports into a queue; flushLexDiags hands its diagnostics to
  // Diags in source order. Declared before Lex, which keeps a reference.
  CollectingDiagnosticConsumer LexQueue;
  DiagnosticsEngine LexDiags;
  size_t NextLexDiag = 0;
  Lexer Lex;
  llvm::StringRef Src;

  Token Tok;
  Token PeekEOF;
  std::deque<Token> Ahead;
  SourceLoc PrevEnd;

  int ExprLev = 0; // < 0: in control clause, composite literals are not allowed
  bool InRhs = false;
  unsigned Depth = 0;
  bool Bailed = false;
  unsigned LastErrorLine = 0;
  SourceLoc SyncPos;
  unsigned SyncCnt = 0;

  SmallVector<Comment, 16> Comments;
  SmallVector<ImportSpec *, 8> Imports;
};

ast::File *Parser::parseFile() {
  ast::File *F = mk<ast::File>(SM.getStartLoc(File), SM.getEndLoc(File));
  SmallVector<Decl *, 16> Decls;

  // package clause
  F->PackageLoc = Tok.Loc;
  bool HasPackage = is(tok::kw_package);
  expect(tok::kw_package);
  if (HasPackage) {
    F->Name = parseIdent();
    expectSemi();
  } else {
    // Not a Go++ source file (or a very broken one): one error is enough.
    F->Name = makeIdent("_", Tok.Loc, Tok.Loc);
    advance(TokenSet{tok::eof});
  }

  if (HasPackage) {
    bool SeenNonImport = false;
    while (!is(tok::eof)) {
      // Continue to accept import declarations for error tolerance, but
      // complain.
      if (is(tok::kw_import) && SeenNonImport)
        error(Tok.Loc, diag::err_import_after_decl);
      if (!is(tok::kw_import))
        SeenNonImport = true;
      SourceLoc Before = Tok.Loc;
      Decls.push_back(limitHeight(parseDecl()));
      if (Tok.Loc == Before && !is(tok::eof))
        next(); // no progress: never loop forever on bad input
    }
  }

  flushLexDiags(SourceLoc()); // the rest, e.g. from a lookahead past a stop
  F->Decls = Ctx.copy(ArrayRef<Decl *>(Decls));
  F->Imports = Ctx.copy(ArrayRef<ImportSpec *>(Imports));
  F->Comments = Ctx.copy(ArrayRef<Comment>(Comments));
  return F;
}

} // namespace

ast::File *gopp::parseFile(ASTContext &Ctx, const SourceManager &SM,
                           FileID File, DiagnosticsEngine &Diags,
                           ParseOptions Opts) {
  Parser P(Ctx, SM, File, Diags, Opts);
  return P.parseFile();
}
