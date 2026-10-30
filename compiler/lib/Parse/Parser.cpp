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

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Threading.h"

#include <algorithm>
#include <cctype>
#include <deque>

using namespace gopp;
using namespace gopp::ast;
using llvm::ArrayRef;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
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
  case tok::cmp3:
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
  SourceLoc ExtendsLoc; // `T extends P`: Type is P
};

class Parser {
public:
  Parser(ASTContext &Ctx, const SourceManager &SM, FileID File,
         DiagnosticsEngine &Diags, ParseOptions Opts)
      : Ctx(Ctx), SM(SM), File(File), Diags(Diags), Opts(Opts),
        LexDiags(SM, LexQueue, /*ErrorLimit=*/0),
        Lex(SM, File, LexDiags,
            [&Opts] {
              LexerOptions LO;
              LO.KeepComments = true; // collected for the comment map (D2.3)
              LO.Spaceship = Opts.AllowOperators;
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
    // The comments before the next real token form groups as in go/parser
    // (parser.next): comments on the line of the previous token start a group
    // that may be a line comment; later groups are separated by blank lines,
    // and the last one is a lead comment if it ends on the line above the
    // token.
    size_t FirstPending = Comments.size();
    Token T;
    for (;;) {
      T = Lex.lex();
      if (!T.is(tok::comment))
        break;
      checkDirective(T);
      Comment C;
      C.Loc = T.Loc;
      C.Length = T.Length;
      Comments.push_back(C);
    }
    groupComments(FirstPending, T);
    // Only the location: its line is looked up when comments follow. A line
    // lookup per token would cost a good part of the parse time (D2.9).
    PrevTokLoc = T.Loc;
    return T;
  }

  /// The line on which the comment ends.
  unsigned commentEndLine(const Comment &C) const {
    unsigned L = SM.getLineNumber(C.Loc);
    llvm::StringRef Text = Src.substr(SM.getFileOffset(C.Loc), C.Length);
    if (Text.starts_with("/*"))
      L += Text.count('\n');
    return L;
  }

  void groupComments(size_t First, const Token &Next) {
    size_t N = Comments.size();
    if (First == N)
      return;
    unsigned NextLine = SM.getLineNumber(Next.Loc);
    size_t I = First;
    // consumeCommentGroup(Gap): comments starting within Gap lines of the end
    // of the previous one
    auto Group = [&](unsigned Gap, unsigned &EndLine) {
      CommentGroup G;
      G.First = static_cast<uint32_t>(I);
      EndLine = SM.getLineNumber(Comments[I].Loc);
      while (I < N && SM.getLineNumber(Comments[I].Loc) <= EndLine + Gap) {
        EndLine = commentEndLine(Comments[I]);
        ++I;
      }
      G.Count = static_cast<uint32_t>(I - G.First);
      G.Next = Next.Loc;
      return G;
    };
    unsigned EndLine = 0;
    if (PrevTokLoc.isValid() &&
        SM.getLineNumber(Comments[I].Loc) == SM.getLineNumber(PrevTokLoc)) {
      CommentGroup G = Group(0, EndLine);
      G.Line = NextLine != EndLine || Next.is(tok::semi) || Next.is(tok::eof);
      Groups.push_back(G);
    }
    EndLine = 0;
    bool Any = false;
    CommentGroup Last;
    while (I < N) {
      Last = Group(1, EndLine);
      Groups.push_back(Last);
      Any = true;
    }
    if (Any && EndLine + 1 == NextLine)
      Groups.back().Lead = true;
  }

  /// LEX-11: `//gopp:cflags` and friends belong in gopp.mod. The error goes
  /// through the lexer's queue so that it comes out in source order. No
  /// fix-it: deleting the line would lose the flags, and the move to gopp.mod
  /// is an edit of another file.
  void checkDirective(const Token &T) {
    if (!Opts.CheckLanguageRules)
      return;
    llvm::StringRef C = text(T);
    if (!C.consume_front("//gopp:"))
      return;
    llvm::StringRef Name = C.take_while(
        [](char Ch) { return Ch != ' ' && Ch != '\t' && Ch != '\r'; });
    if (Name != "cflags" && Name != "cxxstd" && Name != "ldflags")
      return;
    LexDiags.report(T.Loc, diag::err_gopp_directive_in_source)
            .range(T.getRange())
            .note(T.Loc, diag::note_gopp_mod_line, {Name})
        << Name;
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

  /// "newline", the text of a literal or identifier, or "'tok'".
  std::string describeTok() const {
    if (Tok.isAutoSemi())
      return "newline";
    if (tok::isLiteral(Tok.Kind))
      return text(Tok).str();
    return "'" + tokString(Tok.Kind) + "'";
  }

  void errorExpected(SourceLoc L, llvm::StringRef What) {
    if (L != Tok.Loc) {
      error(L, diag::err_expected, {What});
      return;
    }
    // the error is at the current token: say what is there
    std::string Found = describeTok();
    SourceRange R =
        !Tok.isAutoSemi() && Tok.Length ? Tok.getRange() : SourceRange(L);
    errorRange(L, R, diag::err_expected_found, {What, Found});
  }

  SourceLoc expect(tok::Kind K) {
    SourceLoc L = Tok.Loc;
    if (!is(K)) {
      errorExpected(L, "'" + tokString(K) + "'");
      // A '}' closes some enclosing block: keep it for that block. A '{' or
      // a keyword after a missing ')' or ']' starts what follows.
      if (is(tok::r_brace))
        return L;
      if ((K == tok::r_paren || K == tok::r_brack) && !is(tok::eof) &&
          endsListAt(0, K))
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
    if (!is(K) && is(tok::semi) && Tok.isAutoSemi() && peek(1).is(K)) {
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
    case tok::inc:
    case tok::dec:
      // `x = i++`: the statement is complete, the operator is extra.
      error(Tok.Loc, diag::err_incdec_expression, {tokString(Tok.Kind)});
      next();
      break;
    default:
      errorExpected(Tok.Loc, "';'");
      advance(StmtSync);
      break;
    }
  }

  /// True if T cannot continue a list that ends with Follow: it ends a block
  /// or starts a statement or declaration, so the closing token is missing.
  /// The list stops there (one "expected ')'" error, and the parse goes on at
  /// T) instead of reading the rest of the file as list elements.
  bool endsListAt(unsigned N, tok::Kind Follow) {
    const Token &T = N == 0 ? Tok : peek(N);
    switch (T.Kind) {
    case tok::kw_func:
      // `func name`: a declaration, not a function literal
      return peek(N + 1).is(tok::ident);
    case tok::kw_type:
    case tok::eof:
    case tok::kw_return:
    case tok::kw_var:
    case tok::kw_const:
    case tok::kw_import:
    case tok::kw_package:
    case tok::kw_if:
    case tok::kw_else:
    case tok::kw_for:
    case tok::kw_switch:
    case tok::kw_select:
    case tok::kw_go:
    case tok::kw_defer:
    case tok::kw_break:
    case tok::kw_continue:
    case tok::kw_goto:
    case tok::kw_fallthrough:
    case tok::kw_macro:
      return true;
    case tok::r_brace:
      return Follow != tok::r_brace;
    case tok::l_brace:
      // `func f() (int {`: but `{` starts an element of a composite literal
      return Follow == tok::r_paren || Follow == tok::r_brack;
    default:
      return false;
    }
  }

  bool atComma(llvm::StringRef Context, tok::Kind Follow) {
    if (is(tok::comma))
      return true;
    if (!is(Follow)) {
      if (endsListAt(0, Follow))
        return false;
      if (is(tok::semi) && Tok.isAutoSemi() && endsListAt(1, Follow))
        return false;
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
      // The base of a selector adds nothing (see parsePrimaryExpr).
      const bool Sel = Opts.UncountedSelectors && isa<SelectorExpr>(N);
      forEachChild(N, [&](const char *, const Node *C) {
        Work.emplace_back(C, Sel && C == cast<SelectorExpr>(N)->X &&
                                     isa<SelectorExpr>(C)
                                 ? H
                                 : H + 1);
      });
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
    // A node that is missing (error recovery at EOF) is empty, not reversed.
    N->End = B.isValid() && E.isValid() && E < B ? B : E;
    return N;
  }
  template <class T> T *mk(SourceLoc B) { return mk<T>(B, PrevEnd); }
  /// Sets the end of N; a node that starts at a token the parser did not
  /// consume (error recovery) is empty, not reversed.
  void setEnd(Node *N, SourceLoc E) {
    N->End = N->Begin.isValid() && E.isValid() && E < N->Begin ? N->Begin : E;
  }

  /// The location after the one-byte token at L, but not past the end of the
  /// file: a closing token that is missing is reported at the EOF position.
  SourceLoc after(SourceLoc L) const {
    SourceLoc N = L.getLocWithOffset(1), EndLoc = SM.getEndLoc(File);
    return EndLoc < N ? EndLoc : N;
  }

  /// The source text of [B, E).
  llvm::StringRef srcText(SourceLoc B, SourceLoc E) const {
    if (!B.isValid() || !E.isValid() || E < B)
      return {};
    return Src.slice(SM.getFileOffset(B), SM.getFileOffset(E));
  }

  /// [B, E) if it holds only blanks and line breaks (at least one), else an
  /// invalid range. Replacing it by a blank joins two lines; a comment in
  /// between is never deleted.
  SourceRange blankGap(SourceLoc B, SourceLoc E) const {
    llvm::StringRef Gap = srcText(B, E);
    if (Gap.empty() || !Gap.contains('\n') ||
        Gap.find_first_not_of(" \t\r\n") != llvm::StringRef::npos)
      return {};
    return SourceRange(B, E);
  }

  /// True if a comment starts in [R.getBegin(), R.getEnd()).
  bool hasCommentIn(SourceRange R) const {
    auto It = std::lower_bound(
        Comments.begin(), Comments.end(), R.getBegin(),
        [](const Comment &C, SourceLoc L) { return C.Loc < L; });
    return It != Comments.end() && It->Loc < R.getEnd();
  }

  /// Adds the fix-it unless it would replace a comment: a fix-it must not
  /// lose text that is not part of the mistake.
  void fixitKeepingComments(DiagnosticBuilder &B, SourceRange R,
                            llvm::StringRef Text) const {
    if (!hasCommentIn(R))
      B.fixit(R, Text);
  }

  BadExpr *badExpr(SourceLoc From, SourceLoc To) {
    return mk<BadExpr>(From, To < From ? From : To);
  }
  BadExpr *badExprHere(SourceLoc From) { return badExpr(From, Tok.Loc); }

  Ident *makeIdent(llvm::StringRef Name, SourceLoc B, SourceLoc E) {
    Ident *I = mk<Ident>(B, E);
    I->Name = Ctx.intern(Name);
    return I;
  }

  /// `x: int` in a parameter, field or variable declaration: reports the colon
  /// (with a fix-it that removes it) and skips it. Nothing else can follow a
  /// name there, so the type is parsed next as if the colon were not there.
  void skipColonAfterName(const Ident *Name) {
    if (!is(tok::colon))
      return;
    // `x:int` keeps a blank between the name and the type.
    bool BlankAfter = Src.substr(SM.getFileOffset(Tok.getEndLoc()), 1)
                          .find_first_of(" \t\r\n") == 0;
    if (shouldReport(Tok.Loc))
      Diags.report(Tok.Loc, diag::err_colon_before_type)
              .range(Tok.getRange())
              .fixit(Tok.getRange(), BlankAfter ? "" : " ")
          << Name->Name.str();
    next();
  }

  /// RNG-8: `u := u` (or `var u`, or a parameter `u`) inside the body of a loop
  /// that declares `u`.
  void checkLoopVar(const Ident *I) {
    if (LoopVars.empty() || I->Name.str() == "_")
      return;
    for (const auto &[Name, Loc] : LoopVars) {
      if (Name != I->Name)
        continue;
      if (shouldReport(I->Begin))
        Diags.report(I->Begin, diag::err_loop_var_redeclared)
                .range(I->getRange())
                .note(Loc, diag::note_loop_var_declared_here, {Name.str()})
            << Name.str();
      return;
    }
  }

  //===--------------------------------------------------------------------===//
  // Lookahead predicates for the contextual words (LEX-12)
  //===--------------------------------------------------------------------===//

  /// At `move` in a parameter declaration: is it the mark (`move v T`) or a
  /// parameter named `move` (`move T`, `move pkg.T`, `move G[K]`)? Decided by
  /// scanning to the end of the type that would follow the name.
  bool isMoveMark(llvm::StringRef Word = "move") {
    if (!isWord(Tok, Word))
      return false;
    Token T1 = peek(1);
    if (!T1.is(tok::ident))
      return false; // move []int, move *T, move ...T, move func(): a name
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

  /// Can a type start with this token? (for the contextual `extends`)
  static bool startsType(const Token &T) {
    switch (T.Kind) {
    case tok::ident:
    case tok::mul:
    case tok::arrow:
    case tok::kw_func:
    case tok::kw_chan:
    case tok::kw_map:
    case tok::kw_struct:
    case tok::kw_interface:
    case tok::l_paren:
      return true;
    default:
      return false;
    }
  }

  /// At the contextual `extends` of a type parameter `T extends P` or of an
  /// interface element `extends P` (LEX-12): the word is followed by a type.
  /// `[` is not taken (it could be a generic type named `extends`).
  bool atExtendsWord() {
    return Opts.AllowExtends && isWord(Tok, "extends") && startsType(peek(1)) &&
           !peek(1).is(tok::l_paren);
  }

  /// At the contextual `extends` after the name (and type parameters) of a
  /// type declaration. `[` starts a type here (`extends []int`,
  /// `extends [4]int`, `extends A[T]` is the ident case); it is the type named
  /// `extends` only when nothing but the brackets follow, as in the generic
  /// alias `type T extends[int]`.
  bool atTypeDeclExtends() {
    if (!Opts.AllowExtends || !isWord(Tok, "extends"))
      return false;
    const Token &T1 = peek(1);
    // `type A extends {}` forgot the parent: `extends` followed by `{` is
    // never a type named `extends` (that is a syntax error), so it is read
    // as the keyword and the missing parent is the one error.
    if (startsType(T1) || T1.is(tok::l_brace))
      return true;
    if (!T1.is(tok::l_brack))
      return false;
    unsigned Depth = 0;
    for (unsigned I = 1; I < 1 + 256; ++I) {
      const Token &T = peek(I);
      if (T.is(tok::eof))
        return false;
      if (T.is(tok::l_brack))
        ++Depth;
      else if (T.is(tok::r_brack) && --Depth == 0) {
        const Token &After = peek(I + 1);
        return !(After.is(tok::semi) || After.is(tok::eof) ||
                 After.is(tok::r_paren));
      }
    }
    return false;
  }

  //===--------------------------------------------------------------------===//
  // Identifiers and lists
  //===--------------------------------------------------------------------===//

  //===--------------------------------------------------------------------===//
  // Operator method names (TYP-27, EXP-29)
  //===--------------------------------------------------------------------===//

  /// The tokens that start an operator method name after a receiver. `(`
  /// starts `()` and `[` starts `[]` or `[]=`: a method name is required
  /// there, so there is no clash.
  bool atOperatorNameAfterRecv() const {
    if (!Opts.AllowOperators)
      return false;
    switch (Tok.Kind) {
    case tok::l_paren:
    case tok::l_brack:
      return true;
    default:
      return isSimpleOperator(Tok.Kind);
    }
  }

  /// A token that is an operator name by itself (or a blocked one, which gets
  /// its own error): never the start of a type.
  static bool isSimpleOperator(tok::Kind K) {
    switch (K) {
    case tok::add:
    case tok::sub:
    case tok::quo:
    case tok::rem:
    case tok::and_:
    case tok::or_:
    case tok::xor_:
    case tok::shl:
    case tok::shr:
    case tok::and_not:
    case tok::not_:
    case tok::eql:
    case tok::cmp3:
    case tok::land:
    case tok::lor:
    case tok::inc:
    case tok::dec:
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
    case tok::and_not_assign:
    case tok::kw_range:
    case tok::mul:
    case tok::arrow:
    // blocked names (EXP-29): parsed to give the reason
    case tok::assign:
    case tok::period:
    case tok::comma:
    case tok::neq:
    case tok::lss:
    case tok::leq:
    case tok::gtr:
    case tok::geq:
    case tok::define:
      return true;
    default:
      return false;
    }
  }

  /// At an operator method element of an interface (TYP-27). An element that
  /// starts with `*`, `[]` or `<-` followed by `(` is a method element,
  /// except `*(T)` and `[](T)` with nothing after the `)`: those are type
  /// terms. `*T`, `[]T` and `<-chan T` stay type terms.
  bool atInterfaceOperator() {
    if (!Opts.AllowOperators)
      return false;
    // After the matching `)` of the `(` at peek(From): does a type term end?
    auto TermEndsAfterParens = [&](unsigned From) {
      unsigned Depth = 0;
      for (unsigned I = From; I < From + 512; ++I) {
        const Token &T = peek(I);
        if (T.is(tok::eof))
          return true;
        if (T.is(tok::l_paren))
          ++Depth;
        else if (T.is(tok::r_paren) && --Depth == 0) {
          const Token &After = peek(I + 1);
          return After.is(tok::semi) || After.is(tok::r_brace) ||
                 After.is(tok::or_) || After.is(tok::eof);
        }
      }
      return true;
    };
    switch (Tok.Kind) {
    case tok::mul:
      if (!peek(1).is(tok::l_paren))
        return false;
      if (peek(2).is(tok::r_paren))
        return true; // `*()` is no type
      return !TermEndsAfterParens(1);
    case tok::arrow:
      return peek(1).is(tok::l_paren);
    case tok::l_brack:
      if (!peek(1).is(tok::r_brack))
        return false;
      if (peek(2).is(tok::assign))
        return true;
      if (!peek(2).is(tok::l_paren))
        return false;
      if (peek(3).is(tok::r_paren))
        return true;
      return !TermEndsAfterParens(2);
    case tok::l_paren:
      return peek(1).is(tok::r_paren);
    default:
      return isSimpleOperator(Tok.Kind);
    }
  }

  /// The reason a blocked operator cannot be declared (EXP-29), or null.
  static const char *blockedReason(tok::Kind K) {
    switch (K) {
    case tok::assign:
      return "assignment cannot be overloaded: ownership and moves depend on "
             "it";
    case tok::period:
      return "member access `.` cannot be overloaded";
    case tok::comma:
      return "the comma cannot be overloaded";
    case tok::neq:
      return "write `==`: `!=` is made from it";
    case tok::lss:
    case tok::leq:
    case tok::gtr:
    case tok::geq:
      return "write `<=>`: `<`, `<=`, `>` and `>=` are made from it";
    case tok::define:
      return "`:=` is not an operator and cannot be a method name";
    default:
      return nullptr;
    }
  }

  /// Parses the operator method name at the current token (atOperatorName...
  /// said yes) as an Ident whose text is the operator: `+`, `[]`, `[]=`, `()`,
  /// `range`. A blocked name is reported with its reason and gets the name
  /// `_`, so nothing else is said about the method.
  Ident *parseOperatorName() {
    SourceLoc B = Tok.Loc;
    tok::Kind K = Tok.Kind;
    std::string Spell;
    if (K == tok::l_brack) {
      next();
      Spell = "[]";
      expect(tok::r_brack);
      if (is(tok::assign)) {
        next();
        Spell = "[]=";
      }
    } else if (K == tok::l_paren) {
      next();
      Spell = "()";
      expect(tok::r_paren);
    } else if (K == tok::kw_range) {
      next();
      Spell = "range";
    } else {
      Spell = tokString(K);
      if (const char *Why = blockedReason(K)) {
        SourceLoc At = Tok.Loc;
        next();
        error(At, diag::err_op_blocked, {Spell, Why});
        return makeIdent("_", B, PrevEnd);
      }
      next();
    }
    return makeIdent(Spell, B, PrevEnd);
  }

  /// Unary `&` has no operator form (EXP-29): `&` with no parameters.
  void checkUnaryAmp(const Ident *Name, const FuncType *FT) {
    if (Name->Name.str() != "&" || !FT->Params || FT->Params->numFields())
      return;
    error(Name->Begin, diag::err_op_blocked,
          {"&", "unary `&` cannot be overloaded: the borrow checker depends "
                "on it"});
  }

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
      T = isNumericIdent(T) ? parseUnitBracket(T) : parseTypeInstance(T);
    return T;
  }

  Expr *parseTypeName(Ident *I) {
    if (!I)
      I = parseIdent();
    // I is a package name. In Go++ more names may follow for the nested
    // namespaces of a C++ header: `asio.ip.tcp.socket` (ADR-0010 Q4). Go mode
    // reads only `pkg.Name`; a further `.X` is then a syntax error as in Go.
    Expr *T = I;
    while (is(tok::period)) {
      next();
      Ident *Sel = parseIdent();
      SelectorExpr *S = mk<SelectorExpr>(I->Begin);
      S->X = T;
      S->Sel = Sel;
      T = S;
      if (!Opts.AllowNamespaceTypes)
        break;
    }
    return T;
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
    IndexExpr *I = mk<IndexExpr>(X->Begin, after(Rbrack));
    I->X = X;
    I->Lbrack = Lbrack;
    I->Rbrack = Rbrack;
    I->Indices = Ctx.copy(Exprs);
    setEnd(I, PrevEnd < after(Rbrack) ? after(Rbrack) : PrevEnd);
    return I;
  }

  /// `x [P]E` (a field or parameter named x of array type) versus `x[P]` (an
  /// embedded or unnamed instantiated type). Returns the name (null for the
  /// instance) and the type.
  std::pair<Ident *, Expr *> parseArrayFieldOrTypeInstance(Ident *X) {
    if (isNumericIdent(X)) // `float64[M/S^2]`: a unit type (UNI-7)
      return {nullptr, parseUnitBracket(X)};
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
        skipColonAfterName(Names.back()); // `a: int`
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
        setEnd(S, PrevEnd);
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
    return parseStructBody(Pos);
  }

  /// The keyword is consumed; Pos is where the struct type starts.
  Expr *parseStructBody(SourceLoc Pos) {
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

    // `move name Type`; `own name Type` is the same after an error.
    bool Own = !Forced && AllowMove && PTok == tok::ident &&
               (isWord(Tok, "own") || isWord(Tok, "owned")) &&
               isMoveMark(text(Tok));
    if (Own && shouldReport(Tok.Loc))
      Diags.report(Tok.Loc, diag::err_own_for_move)
              .range(Tok.getRange())
              .fixit(Tok.getRange(), "move")
          << text(Tok);
    if (!Forced && AllowMove && PTok == tok::ident && (Own || isMoveMark())) {
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
      if (TypeSetsOK && atExtendsWord()) {
        // `T extends P` (GEN-12)
        F.ExtendsLoc = Tok.Loc;
        next();
        F.Type = parseType();
        return F;
      }
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
      case tok::colon:
        // `x: int`: the colon of TypeScript, Python and Rust
        skipColonAfterName(F.Name);
        F.Type = is(tok::ellipsis) ? parseDotsType() : parseType();
        return F;
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
    llvm::DenseMap<Expr *, SourceLoc> Extends; // type -> its `extends` word
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
        if (Par.ExtendsLoc.isValid() && Par.Type)
          Extends[Par.Type] = Par.ExtendsLoc;
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
      SourceLoc ErrPos;         // left-most error position (or invalid)
      Expr *Typ = nullptr;      // current type (from right to left)
      Expr *MoveType = nullptr; // type of the `move` entry to the right
      // Names that got the type of a later `move` parameter, with that type.
      SmallVector<std::pair<Ident *, Expr *>, 4> Unshared;
      for (size_t I = List.size(); I-- > 0;) {
        ParamEntry &Par = List[I];
        if (Par.Type) {
          // A `move` entry has its own explicit type and gives it to no one.
          Typ = Par.MoveLoc.isValid() ? nullptr : Par.Type;
          MoveType = Par.MoveLoc.isValid() ? Par.Type : nullptr;
          if (!Par.Name) {
            ErrPos = Par.Type->Begin;
            Par.Name = makeIdent("_", ErrPos, ErrPos);
          }
        } else if (Typ) {
          Par.Type = Typ;
        } else if (MoveType) {
          // `(a, move b int)`: a gets no type from b. Recover with b's type
          // (no BadExpr in the tree) and say so once, with a fix-it for every
          // such name.
          Par.Type = MoveType;
          Unshared.emplace_back(Par.Name, MoveType);
        } else {
          // Par.Type == null && Typ == null => we only have a name
          ErrPos = Par.Name->Begin;
          // The placeholder sits right after the name, not up to the closing
          // token: it must not overlap the entries that follow.
          Par.Type = badExpr(Par.Name->End, Par.Name->End);
        }
      }
      if (!Unshared.empty()) {
        Ident *First = Unshared.back().first;
        if (shouldReport(First->Begin)) {
          DiagnosticBuilder B =
              Diags.report(First->Begin, diag::err_move_type_not_shared);
          B.range(First->getRange());
          for (auto &[N, T] : Unshared) {
            llvm::StringRef TypeText = srcText(T->Begin, T->End);
            // `...T` is only valid on the last parameter: no fix-it for it.
            if (!TypeText.contains('\n') && !isa<Ellipsis>(T))
              B.insert(N->End, (" " + TypeText).str());
          }
          B << First->Name.str();
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
      if (auto It = Extends.find(Typ); It != Extends.end())
        F->ExtendsLoc = It->second;
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
    FieldList *FL = mk<FieldList>(Open, after(Close));
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
    if (is(tok::sub) && peek(1).is(tok::gtr) &&
        peek(1).Loc == Tok.getEndLoc()) {
      // `func f() -> int`: the result arrow of Rust and Swift. The fix-it
      // deletes the arrow and the blanks after it; the result follows.
      SourceLoc Arrow = Tok.Loc;
      next(); // -
      next(); // >
      if (shouldReport(Arrow)) {
        DiagnosticBuilder B =
            Diags.report(Arrow, diag::err_arrow_before_result);
        B.range(SourceRange(Arrow, PrevEnd));
        fixitKeepingComments(B, SourceRange(Arrow, Tok.Loc), "");
      }
    }
    FT->Results = parseParameters(true);
    setEnd(FT, PrevEnd);
    if (FT->Fails && Opts.CheckLanguageRules)
      checkFailsResult(FT);
  }

  /// ERR-15: a `!` function cannot declare `error` as its last result.
  void checkFailsResult(const FuncType *FT) {
    const FieldList *R = FT->Results;
    if (!R || R->List.empty())
      return;
    const Field *Last = R->List.back();
    const auto *Id = dyn_cast_or_null<Ident>(Last->Type);
    if (!Id || Id->Name.str() != "error")
      return;
    if (!shouldReport(Id->Begin))
      return;
    DiagnosticBuilder B =
        Diags.report(Id->Begin, diag::err_fails_returns_error);
    B.range(Last->getRange());
    SourceLoc From = FT->Params ? FT->Params->End : FT->BangLoc;
    if (Last->Names.size() > 1) {
      // `(a, b error)`: no exact fix (two results of type error).
    } else if (R->List.size() == 1) {
      // `f!() error` / `f!() (error)`
      fixitKeepingComments(B, SourceRange(From, R->End), "");
    } else {
      const Field *Prev = R->List[R->List.size() - 2];
      // `(int, error)`
      fixitKeepingComments(B, SourceRange(Prev->End, Last->End), "");
    }
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
      setEnd(B, PrevEnd);
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
      setEnd(U, PrevEnd);
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
      if (atExtendsWord()) {
        // `extends P` (GEN-12)
        SourceLoc B = Tok.Loc;
        next();
        Expr *T = parseType();
        Field *F = mk<Field>(B, T->End);
        F->ExtendsLoc = B;
        F->Type = T;
        List.push_back(F);
      } else if (atInterfaceOperator()) {
        SourceLoc B = Tok.Loc;
        Ident *Name = parseOperatorName();
        FuncType *FT = mk<FuncType>(Name->End);
        parseSignature(FT);
        checkUnaryAmp(Name, FT);
        Field *F = mk<Field>(B, PrevEnd);
        F->Names = Ctx.copy(ArrayRef<Ident *>(&Name, 1));
        F->Type = FT;
        List.push_back(F);
      } else if (is(tok::ident)) {
        Field *F = parseMethodSpec();
        if (F->Names.empty()) {
          F->Type = embeddedElem(F->Type);
          setEnd(F, PrevEnd);
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

  //===--------------------------------------------------------------------===//
  // Units of measure (ADR-0025, UNI-1, UNI-4, UNI-7, EXP-31)
  //===--------------------------------------------------------------------===//

  /// A predeclared numeric type name: the parser reads `[` after it with the
  /// unit grammar (UNI-7). uintptr is read too; the checker rejects it.
  static bool isNumericTypeName(llvm::StringRef S) {
    static const char *const Names[] = {
        "int",   "int8",    "int16",   "int32",     "int64",     "uint",
        "uint8", "uint16",  "uint32",  "uint64",    "uintptr",   "byte",
        "rune",  "float32", "float64", "complex64", "complex128"};
    for (const char *N : Names)
      if (S == N)
        return true;
    return false;
  }
  bool isNumericIdent(const Expr *X) const {
    const auto *I = dyn_cast_or_null<Ident>(X);
    return Opts.AllowUnits && I && isNumericTypeName(I->Name.str());
  }

  /// Can a unit factor start at the current token?
  bool atUnitFactorStart() const {
    return is(tok::ident) || is(tok::l_paren) ||
           (is(tok::int_lit) && text(Tok) == "1");
  }

  /// UnitFactor = UnitName | "1" | "(" UnitExpr ")".
  Expr *parseUnitFactor() {
    NestGuard G(*this);
    switch (Tok.Kind) {
    case tok::ident: {
      Ident *I = parseIdent();
      if (!is(tok::period))
        return I;
      next();
      Ident *Sel = parseIdent();
      SelectorExpr *S = mk<SelectorExpr>(I->Begin);
      S->X = I;
      S->Sel = Sel;
      return S;
    }
    case tok::int_lit:
      return parseBasicLit();
    case tok::l_paren: {
      SourceLoc Lparen = Tok.Loc;
      next();
      Expr *X = parseUnitExpr(true);
      SourceLoc Rparen = expect(tok::r_paren);
      ParenExpr *P = mk<ParenExpr>(Lparen);
      P->Lparen = Lparen;
      P->Rparen = Rparen;
      P->X = X;
      return P;
    }
    default: {
      SourceLoc L = Tok.Loc;
      errorExpected(L, "unit");
      return badExprHere(L);
    }
    }
  }

  /// UnitExpr = UnitTerm { ( "*" | "/" ) UnitTerm }. A single name comes back
  /// as it is; anything longer is a UnitExpr (always, if AlwaysWrap). First is
  /// an already parsed first factor, or null.
  Expr *parseUnitExpr(bool AlwaysWrap, Expr *First = nullptr) {
    SmallVector<ast::UnitTerm, 4> Terms;
    SourceLoc Begin = First ? First->Begin : Tok.Loc;
    bool Open = true;
    while (Open) {
      ast::UnitTerm T;
      if (!Terms.empty()) {
        T.OpLoc = Tok.Loc;
        T.Div = is(tok::quo);
        next();
      }
      T.Base = First ? First : parseUnitFactor();
      First = nullptr;
      if (is(tok::xor_)) {
        T.CaretLoc = Tok.Loc;
        next();
        if (is(tok::sub)) {
          T.NegPower = true;
          next();
        }
        if (is(tok::int_lit)) {
          T.Power = parseBasicLit();
        } else {
          errorExpected(Tok.Loc, "integer exponent");
        }
      }
      Terms.push_back(T);
      Open = is(tok::mul) || is(tok::quo);
    }
    if (!AlwaysWrap && Terms.size() == 1 && !Terms[0].Power &&
        !Terms[0].CaretLoc.isValid())
      return Terms[0].Base;
    ast::UnitExpr *U = mk<ast::UnitExpr>(Begin);
    U->Terms = Ctx.copy(ArrayRef<ast::UnitTerm>(Terms));
    return U;
  }

  /// `[ UnitExpr ]` after X (a numeric type name, `.In`, ...): the brackets
  /// hold unit expressions only (a list, so the checker can say "one unit").
  Expr *parseUnitBracket(Expr *X) {
    // `int[]` is the Java form: the type-argument path has its own message.
    if (peek(1).Kind == tok::r_brack && isa<Ident>(X))
      return parseTypeInstance(X);
    SourceLoc Lbrack = expect(tok::l_brack);
    ++ExprLev;
    SmallVector<Expr *, 4> List;
    while (!is(tok::r_brack) && !is(tok::eof)) {
      SourceLoc Before = Tok.Loc;
      List.push_back(parseUnitExpr(false));
      if (!is(tok::comma))
        break;
      next();
      if (Tok.Loc == Before)
        next();
    }
    --ExprLev;
    SourceLoc Rbrack = expectClosing(tok::r_brack, "unit");
    if (List.empty())
      List.push_back(badExpr(Lbrack.getLocWithOffset(1), Rbrack));
    return packIndexExpr(X, Lbrack, List, Rbrack);
  }

  /// At `[` after `.In`: does the bracket hold a unit expression with a power
  /// `^`? Only then the unit grammar reads it (`x.In[M/S^2]()`); without `^`
  /// an expression and a unit expression have the same tree. (A field or a
  /// method named `In` indexed with a xor is therefore read as a power; the
  /// checker says so, UNI-7.)
  bool inBracketHasPower() {
    unsigned Depth = 0;
    bool Power = false;
    for (unsigned I = 0; I < 512; ++I) {
      const Token &T = I == 0 ? Tok : peek(I);
      switch (T.Kind) {
      case tok::l_brack:
      case tok::l_paren:
        ++Depth;
        break;
      case tok::r_brack:
      case tok::r_paren:
        if (--Depth == 0)
          return Power && T.is(tok::r_brack);
        break;
      case tok::xor_:
        Power = true;
        break;
      case tok::ident:
      case tok::period:
      case tok::mul:
      case tok::quo:
      case tok::sub:
      case tok::int_lit:
        break;
      default:
        return false;
      }
    }
    return false;
  }

  /// A parenthesized element of a type argument list: a unit expression
  /// (`(M/S)^2`, `(KM)/(H)`) or a parenthesized type? A unit if an operator
  /// of the unit grammar is inside at depth one, or follows the `)`.
  bool parenStartsUnit() {
    unsigned Depth = 0;
    for (unsigned I = 0; I < 512; ++I) {
      const Token &T = I == 0 ? Tok : peek(I);
      switch (T.Kind) {
      case tok::eof:
        return false;
      case tok::l_paren:
      case tok::l_brack:
      case tok::l_brace:
        ++Depth;
        break;
      case tok::r_paren:
      case tok::r_brack:
      case tok::r_brace:
        if (--Depth == 0) {
          const Token &A = peek(I + 1);
          return A.is(tok::quo) || A.is(tok::xor_) || A.is(tok::mul);
        }
        break;
      case tok::quo:
      case tok::xor_:
        if (Depth == 1)
          return true;
        break;
      default:
        break;
      }
    }
    return false;
  }

  /// One element of a type argument list. `Vec2[KM/H]`, `Vec2[1/S]` and
  /// `Vec2[(M/S)^2]` hold unit expressions: a type element that is followed
  /// by `/`, `^` or `*` continues as one (a type is never followed by these).
  Expr *parseTypeArg() {
    if (Opts.AllowUnits) {
      if (is(tok::int_lit) && text(Tok) == "1")
        return parseUnitExpr(false);
      if (is(tok::l_paren) && parenStartsUnit())
        return parseUnitExpr(false);
    }
    Expr *T = parseType();
    if (Opts.AllowUnits && (is(tok::quo) || is(tok::xor_) || is(tok::mul)) &&
        (isa<Ident>(T) || isa<SelectorExpr>(T)))
      return parseUnitExpr(false, T);
    return T;
  }

  /// At the contextual word `unit` that starts a declaration (UNI-1): an
  /// identifier or `(` follows on the same line.
  bool atUnitDecl() {
    return Opts.AllowUnits && isWord(Tok, "unit") &&
           (peek(1).is(tok::ident) || peek(1).is(tok::l_paren));
  }

  /// UnitSpec = identifier [ "=" [ Factor ] UnitExpr ].
  Spec *parseUnitSpec() {
    ast::UnitSpec *S = mk<ast::UnitSpec>(Tok.Loc);
    S->Name = parseIdent();
    if (is(tok::assign)) {
      S->Eq = Tok.Loc;
      next();
      if (is(tok::int_lit) && text(Tok) == "1" && !peek(1).is(tok::ident) &&
          !peek(1).is(tok::l_paren)) {
        S->Unit = parseUnitExpr(false); // `unit X = 1`: the unit expression 1
      } else {
        if (is(tok::int_lit) || is(tok::float_lit) || is(tok::imag_lit) ||
            is(tok::string_lit) || is(tok::char_lit)) {
          // (Only a number is a factor; the checker says so for the others.)
          S->Factor = parseBasicLit();
        } else if (is(tok::sub) || is(tok::add)) {
          // A signed factor: the checker says it must be greater than zero.
          UnaryExpr *U = mk<UnaryExpr>(Tok.Loc);
          U->OpLoc = Tok.Loc;
          U->Op = Tok.Kind;
          next();
          U->X = is(tok::int_lit) || is(tok::float_lit) || is(tok::imag_lit)
                     ? static_cast<Expr *>(parseBasicLit())
                     : badExprHere(Tok.Loc);
          setEnd(U, PrevEnd);
          S->Factor = U;
        } else if (is(tok::l_paren) && parenIsFactor()) {
          SourceLoc Lparen = Tok.Loc;
          next();
          ++ExprLev;
          Expr *X = parseRhs();
          --ExprLev;
          SourceLoc Rparen = expect(tok::r_paren);
          ParenExpr *P = mk<ParenExpr>(Lparen);
          P->Lparen = Lparen;
          P->Rparen = Rparen;
          P->X = X;
          S->Factor = P;
        }
        if (atUnitFactorStart())
          S->Unit = parseUnitExpr(false);
        else if (!S->Factor)
          errorExpected(Tok.Loc, "unit expression");
      }
    }
    setEnd(S, PrevEnd);
    expectSemi();
    return S;
  }

  /// At `(` in `unit X = (...)`: a factor `(1609344 / 1000) M` or the start
  /// of a unit expression `(KM)/(H)`? A factor when a unit factor follows the
  /// matching `)` directly.
  bool parenIsFactor() {
    unsigned Depth = 0;
    for (unsigned I = 0; I < 512; ++I) {
      const Token &T = I == 0 ? Tok : peek(I);
      switch (T.Kind) {
      case tok::eof:
        return false;
      case tok::l_paren:
      case tok::l_brack:
      case tok::l_brace:
        ++Depth;
        break;
      case tok::r_paren:
      case tok::r_brack:
      case tok::r_brace:
        if (--Depth == 0) {
          const Token &A = peek(I + 1);
          return A.is(tok::ident) || A.is(tok::l_paren) ||
                 (A.is(tok::int_lit) && text(A) == "1");
        }
        break;
      default:
        break;
      }
    }
    return false;
  }

  /// `unit` at the top level, single or grouped; attributes (`@Symbol`) were
  /// parsed by the caller.
  GenDecl *parseUnitDecl(ArrayRef<Attribute *> Attrs) {
    SourceLoc Pos = Tok.Loc;
    next(); // `unit`
    GenDecl *D = mk<GenDecl>(Attrs.empty() ? Pos : Attrs.front()->Begin);
    D->Attrs = Attrs;
    D->KeywordLoc = Pos;
    D->IsUnit = true;
    D->Keyword = tok::ident;
    SmallVector<Spec *, 8> List;
    if (is(tok::l_paren)) {
      D->Lparen = Tok.Loc;
      next();
      while (!is(tok::r_paren) && !is(tok::eof)) {
        SourceLoc Before = Tok.Loc;
        if (is(tok::ident))
          List.push_back(parseUnitSpec());
        else
          errorExpected(Tok.Loc, "unit name");
        if (Tok.Loc == Before && !is(tok::eof))
          next(); // no progress: never loop forever on bad input
      }
      D->Rparen = expect(tok::r_paren);
      setEnd(D, PrevEnd);
      expectSemi();
    } else {
      List.push_back(parseUnitSpec());
      D->End = List.back()->End;
    }
    D->Specs = Ctx.copy(ArrayRef<Spec *>(List));
    return D;
  }

  Expr *parseTypeInstance(Expr *Type) {
    SourceLoc Opening = expect(tok::l_brack);
    ++ExprLev;
    SmallVector<Expr *, 8> List;
    while (!is(tok::r_brack) && !is(tok::eof)) {
      List.push_back(parseTypeArg());
      if (!atComma("type argument list", tok::r_brack))
        break;
      next();
    }
    --ExprLev;
    SourceLoc Closing = expectClosing(tok::r_brack, "type argument list");
    if (List.empty()) {
      if (Opening.getLocWithOffset(1) == Closing && isa<Ident>(Type)) {
        // `int[]` (Java, C#): the brackets go before the element type.
        if (shouldReport(Opening))
          Diags.report(Opening, diag::err_brackets_after_type)
                  .range(SourceRange(Type->Begin, after(Closing)))
                  .fixit(SourceRange(Opening, after(Closing)), "")
                  .insert(Type->Begin, "[]")
              << cast<Ident>(Type)->Name.str();
      } else {
        errorExpected(Closing, "type argument list");
      }
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
      // `view string` / `view []T`: there is no `view` type (recovery: the
      // type after the word is read).
      if (isWord(Tok, "view")) {
        const Token &T1 = peek(1);
        if (isWord(T1, "string") ||
            (T1.is(tok::l_brack) && peek(2).is(tok::r_brack))) {
          if (shouldReport(Tok.Loc))
            Diags.report(Tok.Loc, diag::err_view_for_ref).range(Tok.getRange());
          next();
          return parseType();
        }
      }
      Expr *T = parseTypeName(nullptr);
      if (is(tok::l_brack))
        T = isNumericIdent(T) ? parseUnitBracket(T) : parseTypeInstance(T);
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
    if (!S.empty() && S[0] >= 'a' && S[0] <= 'z') {
      if (shouldReport(Name->Begin)) {
        std::string Fixed = S.str();
        Fixed[0] = static_cast<char>(Fixed[0] - 'a' + 'A');
        Diags.report(Name->Begin, diag::err_macro_name_lowercase)
                .range(Name->getRange())
                .fixit(Name->getRange(), Fixed)
            << S;
      }
    }
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
    setEnd(M, PrevEnd);
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
    setEnd(A, PrevEnd);
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

  /// `100.0[KM]`: the number literal L and a `[` on its line (EXP-31).
  Expr *parseUnitLit(BasicLit *L) {
    SourceLoc Lbrack = expect(tok::l_brack);
    ++ExprLev;
    Expr *U = nullptr;
    if (is(tok::r_brack)) {
      errorExpected(Tok.Loc, "unit");
      U = badExprHere(Tok.Loc);
    } else {
      U = parseUnitExpr(false);
    }
    --ExprLev;
    SourceLoc Rbrack = expectClosing(tok::r_brack, "unit");
    ast::UnitLit *N = mk<ast::UnitLit>(L->Begin, after(Rbrack));
    N->Lit = L;
    N->Lbrack = Lbrack;
    N->Rbrack = Rbrack;
    N->Unit = U;
    setEnd(N, PrevEnd < after(Rbrack) ? after(Rbrack) : PrevEnd);
    return N;
  }

  Expr *parseFuncTypeOrLit() {
    FuncType *FT = parseFuncType();
    if (!is(tok::l_brace))
      return FT; // function type only
    if (!LoopVars.empty() && FT->Params)
      for (const Field *F : FT->Params->List)
        for (const Ident *I : F->Names)
          checkLoopVar(I);
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
      return parseIdent();
    case tok::int_lit:
    case tok::float_lit:
    case tok::imag_lit: {
      BasicLit *L = parseBasicLit();
      if (Opts.AllowUnits && is(tok::l_brack))
        return parseUnitLit(L);
      return L;
    }
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
    if (is(tok::inc) || is(tok::dec)) {
      // `++i`: Go has no prefix form
      error(L, diag::err_incdec_expression, {tokString(Tok.Kind)});
      next();
      return parseUnaryExpr();
    }
    errorExpected(L, "operand");
    // `for i := range {`: the '{' opens the body, keep it
    if (!(is(tok::l_brace) && ExprLev < 0))
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
        // ADR-0024: `a[i, j]` indexes with several values; an operand that
        // is not a type is an index.
        if (!is(tok::r_brack) && !is(tok::eof))
          Args.push_back(Opts.AllowOperators ? parseRhs() : parseType());
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
    // With UncountedSelectors, a selector on a selector (`a.b.c`) is not
    // counted (P03.26).
    for (unsigned N = 1;; ++N) {
      if (chainTooLong(N))
        return X;
      switch (Tok.Kind) {
      case tok::period:
        next();
        switch (Tok.Kind) {
        case tok::ident:
          if (Opts.UncountedSelectors && isa<SelectorExpr>(X))
            --N;
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
        if (isNumericIdent(X) ||
            (Opts.AllowUnits && isa<SelectorExpr>(X) &&
             cast<SelectorExpr>(X)->Sel->Name.str() == "In" &&
             inBracketHasPower()))
          X = parseUnitBracket(X);
        else
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
      if (Op == tok::and_ && Opts.CheckLanguageRules) {
        auto *CL = dyn_cast<CompositeLit>(unparen(X));
        if (CL && CL->Type && shouldReport(Pos)) {
          // OWN-P4: `&T{...}` -> `new_unique(T{...})`
          Diags.report(Pos, diag::err_addr_of_complit)
              .range(SourceRange(Pos, X->End))
              .fixit(SourceRange(Pos, Pos.getLocWithOffset(1)), "new_unique(")
              .insert(X->End, ")");
        }
      }
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
    if (is(tok::inc) || is(tok::dec)) {
      // `++i` (C habit): the statement is `i++`. The fix-it moves the
      // operator behind the operand.
      SourceLoc OpLoc = Tok.Loc;
      tok::Kind Op = Tok.Kind;
      std::string Spelling = tokString(Op);
      next();
      Expr *X = parseUnaryExpr();
      if (shouldReport(OpLoc))
        Diags.report(OpLoc, diag::err_prefix_incdec)
                .range(SourceRange(OpLoc, OpLoc.getLocWithOffset(2)))
                .fixit(SourceRange(OpLoc, OpLoc.getLocWithOffset(2)), "")
                .insert(X->End, Spelling)
            << Spelling;
      IncDecStmt *S = mk<IncDecStmt>(OpLoc, X->End);
      S->X = X;
      S->OpLoc = OpLoc;
      S->Op = Op;
      R.S = S;
      return R;
    }
    SmallVector<Expr *, 8> X;
    parseList(X, false);
    if (X.size() == 1 && is(tok::ident))
      checkDeclWithoutVar(X[0]); // `int x = 5`, `x int`

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
      if (Op == tok::define && !LoopVars.empty())
        for (Expr *E : X)
          if (auto *I = dyn_cast<Ident>(E))
            checkLoopVar(I);
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
      R.S = mk<BadStmt>(X[0]->Begin, after(Colon));
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

  /// Go's predeclared types: they tell the type from the name in a
  /// declaration written without `var`.
  static bool isPredeclaredType(llvm::StringRef S) {
    static const char *const Types[] = {
        "any",    "bool",    "byte",    "comparable", "complex64", "complex128",
        "error",  "float32", "float64", "int",        "int8",      "int16",
        "int32",  "int64",   "rune",    "string",     "uint",      "uint8",
        "uint16", "uint32",  "uint64",  "uintptr"};
    return llvm::is_contained(Types, S);
  }

  /// `int x = 5` (C, Java) or `x int` (Go without `var`) at the start of a
  /// statement: a name followed by another name and `=` or the end of the
  /// statement. Two names in a row never start a valid statement. The second
  /// name is skipped, so the rest of the line parses. A fix-it is given when a
  /// predeclared type says which word is the type.
  void checkDeclWithoutVar(Expr *First) {
    auto *A = dyn_cast<Ident>(First);
    const Token &T1 = peek(1);
    if (!A || !(T1.is(tok::assign) || T1.is(tok::semi) || T1.is(tok::eof)))
      return;
    llvm::StringRef W1 = A->Name.str(), W2 = text(Tok);
    SourceRange R(A->Begin, Tok.getEndLoc());
    if (shouldReport(A->Begin)) {
      DiagnosticBuilder B = Diags.report(A->Begin, diag::err_decl_without_var);
      B.range(R);
      if (isPredeclaredType(W1))
        fixitKeepingComments(B, R, ("var " + W2 + " " + W1).str());
      else if (isPredeclaredType(W2))
        B.insert(A->Begin, "var ");
      B << W1 << W2;
    }
    next(); // the second name
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

  Expr *makeExpr(Stmt *S, llvm::StringRef Want, llvm::StringRef As) {
    if (!S)
      return nullptr;
    if (auto *ES = dyn_cast<ExprStmt>(S))
      return ES->X;
    if (auto *A = dyn_cast<AssignStmt>(S);
        A && A->Lhs.size() == 1 && A->Rhs.size() == 1 &&
        (A->Op == tok::assign || A->Op == tok::define)) {
      // `if x = 1 {`: almost always a typo for ==
      if (shouldReport(S->Begin)) {
        if (A->Op == tok::assign)
          Diags.report(S->Begin, diag::err_assign_in_condition)
                  .range(S->getRange())
                  .fixit(SourceRange(A->OpLoc, A->OpLoc.getLocWithOffset(1)),
                         "==")
              << As;
        else
          Diags.report(S->Begin, diag::err_define_in_condition)
                  .range(S->getRange())
              << As;
      }
      return badExpr(S->Begin, S->End);
    }
    llvm::StringRef Found =
        isa<AssignStmt>(S) ? "assignment" : "simple statement";
    error(S->Begin, diag::err_expected_found_complit, {Want, Found});
    return badExpr(S->Begin, S->End);
  }

  /// A keyword that can only start a statement, never an expression.
  bool startsKeywordStmt() const {
    switch (Tok.Kind) {
    case tok::kw_go:
    case tok::kw_defer:
    case tok::kw_return:
    case tok::kw_if:
    case tok::kw_for:
    case tok::kw_switch:
    case tok::kw_select:
    case tok::kw_break:
    case tok::kw_continue:
    case tok::kw_goto:
    case tok::kw_fallthrough:
    case tok::kw_var:
    case tok::kw_const:
    case tok::kw_type:
      return true;
    default:
      return false;
    }
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
    if (!is(tok::l_brace) && !is(tok::semi) && Init) {
      // `if x == 1 return 0`: the condition is complete, the '{' is missing
      // (parseBody reports it and takes the next statement as the body).
      CondStmt = Init;
      Init = nullptr;
    } else if (!is(tok::l_brace)) {
      if (is(tok::semi)) {
        SemiLoc = Tok.Loc;
        SemiIsNewline = Tok.isAutoSemi();
        next();
      } else {
        expect(tok::semi);
      }
      if (SemiIsNewline && Init && startsKeywordStmt()) {
        // `if x > 0` newline `return`: the '{' is missing and Init is the
        // condition (parseBody reports it and takes the statement as the
        // body).
        CondStmt = Init;
        Init = nullptr;
      } else if (!is(tok::l_brace)) {
        CondStmt = parseSimpleStmt(Basic).S;
      }
    } else {
      CondStmt = Init;
      Init = nullptr;
    }

    if (CondStmt) {
      Cond = makeExpr(CondStmt, "boolean expression", "a condition");
    } else if (SemiLoc.isValid() && SemiIsNewline && Init && is(tok::l_brace)) {
      // `if x` newline `{` (C habit): Init is the condition. The fix-it joins
      // the lines.
      if (shouldReport(SemiLoc)) {
        DiagnosticBuilder B =
            Diags.report(SemiLoc, diag::err_newline_after_if_clause);
        if (SourceRange Gap = blankGap(PrevEnd, Tok.Loc); Gap.isValid())
          B.fixit(Gap, " ");
      }
      Cond = makeExpr(Init, "boolean expression", "a condition");
      Init = nullptr;
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
    bool Recovered = false;
    BlockStmt *Body = parseBlockStmt("if condition", &Recovered);

    Stmt *Else = nullptr;
    if (!Recovered && Tok.isAutoSemi() && is(tok::semi) &&
        peek(1).is(tok::kw_else)) {
      // `}` newline `else`: the newline ended the statement.
      SourceLoc ElseLoc = peek(1).Loc;
      if (shouldReport(ElseLoc)) {
        DiagnosticBuilder B = Diags.report(ElseLoc, diag::err_else_on_new_line);
        B.range(peek(1).getRange());
        if (SM.getBuffer(File)
                .substr(SM.getFileOffset(PrevEnd),
                        SM.getFileOffset(ElseLoc) - SM.getFileOffset(PrevEnd))
                .find_first_not_of(" \t\r\n") == llvm::StringRef::npos)
          B.fixit(SourceRange(PrevEnd, ElseLoc), " ");
      }
      next(); // the semicolon
    }
    if (Recovered) {
      // the body statement consumed its own ';'
    } else if (is(tok::ident) && (text(Tok) == "elif" || text(Tok) == "elsif" ||
                                  text(Tok) == "elseif")) {
      if (shouldReport(Tok.Loc))
        Diags.report(Tok.Loc, diag::err_elif)
                .range(Tok.getRange())
                .fixit(Tok.getRange(), "else if")
            << text(Tok);
      Tok.Kind = tok::kw_if; // go on as `else if`
      Else = parseIfStmt();
    } else if (is(tok::kw_else)) {
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
    setEnd(C, PrevEnd);
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
    S->Tag = makeExpr(S2, "switch expression", "a switch tag");
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
                       SourceLoc OpLoc, RangeStmt *R) {
    auto *U = dyn_cast<UnaryExpr>(E);
    if (!U || U->Op != tok::and_ || !isa<Ident>(U->X))
      return E;
    if (Op != tok::define) {
      if (shouldReport(U->OpLoc))
        Diags.report(U->OpLoc, diag::err_range_alias_assign)
            .range(SourceRange(U->OpLoc, U->OpLoc.getLocWithOffset(1)))
            .fixit(SourceRange(OpLoc, OpLoc.getLocWithOffset(1)), ":=");
    } else if (!IsValue && !IsOnly) {
      if (shouldReport(U->OpLoc))
        Diags.report(U->OpLoc, diag::err_range_alias_key)
            .range(SourceRange(U->OpLoc, U->OpLoc.getLocWithOffset(1)))
            .fixit(SourceRange(U->OpLoc, U->OpLoc.getLocWithOffset(1)), "");
    }
    R->Alias =
        IsValue ? RangeStmt::AliasKind::Value : RangeStmt::AliasKind::Key;
    R->AliasLoc = U->OpLoc;
    return U->X;
  }

  /// The '{' of a for body on the next line (C habit): reports it at the line
  /// break, with a fix-it that joins the lines, and skips the line break.
  void braceOnNextLine() {
    if (!(is(tok::semi) && Tok.isAutoSemi() && peek(1).is(tok::l_brace)))
      return;
    if (shouldReport(Tok.Loc)) {
      DiagnosticBuilder B =
          Diags.report(Tok.Loc, diag::err_newline_after_for_clause);
      if (SourceRange Gap = blankGap(PrevEnd, peek(1).Loc); Gap.isValid())
        B.fixit(Gap, " ");
    }
    next();
  }

  /// `for x in xs` / `for i, x in xs` (Python habit): how many names (1 or 2)
  /// precede the `in`, or 0.
  unsigned forInAhead() {
    if (!is(tok::ident))
      return 0;
    if (isWord(peek(1), "in") && !peek(2).is(tok::semi))
      return 1;
    if (peek(1).is(tok::comma) && peek(2).is(tok::ident) &&
        isWord(peek(3), "in"))
      return 2;
    return 0;
  }

  /// Parses `names in X` and returns it as the range clause `_, x := range X`.
  AssignStmt *parseForIn(unsigned NumNames, SimpleResult &Range) {
    SourceLoc Begin = Tok.Loc;
    SmallVector<Expr *, 2> Lhs;
    if (NumNames == 1)
      Lhs.push_back(makeIdent("_", Begin, Begin));
    Lhs.push_back(parseIdent());
    if (NumNames == 2) {
      next(); // ,
      Lhs.push_back(parseIdent());
    }
    Token In = Tok;
    next(); // in
    // `for i in range(10)`: the `range` is already there; `for i := range
    // (10)` is an integer range.
    Token Kw = Tok;
    bool HasRange = is(tok::kw_range);
    if (HasRange)
      next(); // range
    SourceLoc FixEnd = HasRange ? Kw.getEndLoc() : In.getEndLoc();
    if (shouldReport(Begin)) {
      std::string Vars = NumNames == 1 && !HasRange ? "_, " : "";
      Vars += srcText(Begin, In.Loc).rtrim().str();
      DiagnosticBuilder B = Diags.report(Begin, diag::err_for_in);
      B.range(SourceRange(Begin, In.getEndLoc()));
      fixitKeepingComments(B, SourceRange(Begin, FixEnd), Vars + " := range");
    }
    Range.RangeLoc = HasRange ? Kw.Loc : In.Loc;
    Range.RangeX = parseRhs();
    Range.IsRange = true;
    AssignStmt *A = mk<AssignStmt>(Begin, PrevEnd);
    A->Lhs = Ctx.copy(ArrayRef<Expr *>(Lhs));
    A->OpLoc = In.Loc;
    A->Op = tok::define;
    Range.S = A;
    return A;
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
        } else if (unsigned NumNames = forInAhead()) {
          S2 = parseForIn(NumNames, Range);
        } else {
          Range = parseSimpleStmt(RangeOk);
          S2 = Range.S;
        }
        IsRange = Range.IsRange;
      }
      if (!IsRange)
        braceOnNextLine(); // `for cond` newline `{`
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
      braceOnNextLine(); // `for i := 0; i < n; i++` newline `{`
    }

    size_t NumLoopVars = LoopVars.size();
    if (Opts.CheckLanguageRules) {
      // RNG-8: the variables of the loop header, for the body
      auto Add = [&](Expr *E) {
        if (auto *U = dyn_cast_or_null<UnaryExpr>(E); U && U->Op == tok::and_)
          E = U->X;
        if (auto *I = dyn_cast_or_null<Ident>(E); I && I->Name.str() != "_")
          LoopVars.push_back({I->Name, I->Begin});
      };
      if (IsRange) {
        if (auto *As = dyn_cast_or_null<AssignStmt>(S2);
            As && As->Op == tok::define)
          for (Expr *E : As->Lhs)
            Add(E);
      } else if (auto *As = dyn_cast_or_null<AssignStmt>(S1);
                 As && As->Op == tok::define) {
        for (Expr *E : As->Lhs)
          Add(E);
      }
    }
    bool Recovered = false;
    BlockStmt *Body = parseBlockStmt("for clause", &Recovered);
    LoopVars.resize(NumLoopVars);
    SourceLoc End = PrevEnd;
    if (!Recovered)
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
        R->Key = takeRangeAlias(As->Lhs[0], false, true, As->Op, As->OpLoc, R);
      } else if (As->Lhs.size() == 2) {
        R->Key = takeRangeAlias(As->Lhs[0], false, false, As->Op, As->OpLoc, R);
        R->Value =
            takeRangeAlias(As->Lhs[1], true, false, As->Op, As->OpLoc, R);
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
    F->Cond = makeExpr(S2, "boolean or range expression", "a condition");
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
      if (Opts.AllowUnits && isWord(Tok, "unit") && peek(1).is(tok::ident)) {
        // UNI-1: units are declared at package level
        SourceLoc L = Tok.Loc;
        error(L, diag::err_unit_in_func);
        next();
        parseUnitSpec();
        return mk<BadStmt>(L, PrevEnd);
      }
      if (tailoredStmtWord())
        return parseStmt();
      [[fallthrough]];
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
    case tok::not_: // unary operators
    case tok::inc:
    case tok::dec:    // `++i`: an error, reported by parseSimpleStmt
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

  /// P02.07: habits from other languages at the start of a statement. Reports
  /// the error, turns the word into the Go++ keyword and returns true; the
  /// caller parses the statement again.
  bool tailoredStmtWord() {
    llvm::StringRef W = text(Tok);
    const Token &T1 = peek(1);
    if (W == "while") {
      // `while cond {`, `while (cond) {`; not `while(x)`, a call of a function
      // named while, or `while := 1`.
      switch (T1.Kind) {
      case tok::ident:
      case tok::int_lit:
      case tok::float_lit:
      case tok::char_lit:
      case tok::string_lit:
        break;
      case tok::not_:
        // `while !x {`; but `while!(x)` and `while !(x)` call the function
        // `while` with an unwrapping call (D1.8), unless a `{` follows the
        // parentheses.
        if (peek(2).is(tok::l_paren) && !parenthesizedThenLbrace(2))
          return false;
        break;
      case tok::l_paren:
        if (!parenthesizedThenLbrace())
          return false;
        break;
      default:
        return false;
      }
      if (shouldReport(Tok.Loc))
        Diags.report(Tok.Loc, diag::err_while_loop)
            .range(Tok.getRange())
            .fixit(Tok.getRange(), "for");
      Tok.Kind = tok::kw_for;
      return true;
    }
    if ((W == "let" || W == "auto" || W == "val") && T1.is(tok::ident)) {
      // `let x = 1`
      if (shouldReport(Tok.Loc))
        Diags.report(Tok.Loc, diag::err_let_decl)
                .range(Tok.getRange())
                .fixit(Tok.getRange(), "var")
            << W;
      Tok.Kind = tok::kw_var;
      return true;
    }
    return false;
  }

  /// At a word followed by '(' (or, with From, by '!' then '('): is the
  /// matching ')' followed by '{' (as in `while (x < 3) {`), so that the word
  /// is not the name of a called function?
  bool parenthesizedThenLbrace(unsigned From = 1) {
    unsigned Depth = 0;
    for (unsigned I = From; I < 64; ++I) {
      const Token &T = peek(I);
      if (T.is(tok::eof))
        return false;
      if (T.is(tok::l_paren))
        ++Depth;
      else if (T.is(tok::r_paren) && --Depth == 0)
        return peek(I + 1).is(tok::l_brace);
    }
    return false;
  }

  Stmt *badExprStmt(SourceLoc From) {
    return mk<BadStmt>(From, Tok.Loc < From ? From : Tok.Loc);
  }

  /// After names the construct ("if condition") for the missing-brace error.
  /// If the '{' is missing and a statement follows on the same line, that one
  /// statement becomes the body and *Recovered is set (the statement has
  /// already consumed its ';').
  BlockStmt *parseBody(const char *After = nullptr, bool *Recovered = nullptr) {
    NestGuard G(*this);
    if (After && !is(tok::l_brace) && !is(tok::semi) && !is(tok::eof) &&
        !is(tok::r_brace) && !is(tok::kw_else) && !is(tok::kw_case) &&
        !is(tok::kw_default)) {
      SourceLoc L = Tok.Loc;
      if (shouldReport(L)) {
        DiagnosticBuilder B = Diags.report(L, diag::err_expected_lbrace_after);
        B.range(Tok.getRange());
        B << After << describeTok();
      }
      SmallVector<Stmt *, 8> One;
      One.push_back(parseStmt());
      if (Recovered)
        *Recovered = true;
      BlockStmt *B = mk<BlockStmt>(L, PrevEnd);
      B->Lbrace = L;
      B->Rbrace = L;
      B->List = Ctx.copy(ArrayRef<Stmt *>(One));
      return B;
    }
    SourceLoc Lbrace = expect(tok::l_brace);
    SmallVector<Stmt *, 8> List;
    parseStmtList(List);
    bool Closed = is(tok::r_brace);
    SourceLoc Rbrace = expect2(tok::r_brace);
    BlockStmt *B = mk<BlockStmt>(Lbrace, Closed ? after(Rbrace) : PrevEnd);
    B->Lbrace = Lbrace;
    B->Rbrace = Rbrace;
    B->List = Ctx.copy(ArrayRef<Stmt *>(List));
    return B;
  }

  BlockStmt *parseBlockStmt(const char *After = nullptr,
                            bool *Recovered = nullptr) {
    return parseBody(After, Recovered);
  }

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  /// Grouped: the spec is inside `import ( ... )`.
  Spec *parseImportSpec(bool Grouped) {
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
    if (Grouped && is(tok::comma)) {
      // `import ("a", "b")`: the list syntax of other languages
      if (shouldReport(Tok.Loc))
        Diags.report(Tok.Loc, diag::err_comma_between_imports)
            .range(Tok.getRange())
            .fixit(Tok.getRange(), ";");
      next();
    } else {
      expectSemi();
    }

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
    if (Keyword == tok::kw_var && !LoopVars.empty())
      for (Ident *I : Idents)
        checkLoopVar(I);
    if (is(tok::define)) {
      // `var x := 1`: the parse goes on as `var x = 1`
      if (shouldReport(Tok.Loc))
        Diags.report(Tok.Loc, diag::err_define_in_var_decl)
                .range(Tok.getRange())
                .fixit(Tok.getRange(), "=")
            << tokString(Keyword);
      Tok.Kind = tok::assign;
    } else {
      skipColonAfterName(Idents.back()); // `var x: int`
    }
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
    parseTypeSpecRhs(Spec);
  }

  /// The type after the name of a type declaration (and its type parameters):
  /// a type, or `extends Parent { fields }` (TYP-16).
  void parseTypeSpecRhs(TypeSpec *Spec) {
    if (Spec->AssignLoc.isValid() || !atTypeDeclExtends()) {
      Spec->Type = parseType();
      return;
    }
    Spec->ExtendsLoc = Tok.Loc;
    next();
    Spec->Extends = parseType();
    if (is(tok::comma)) {
      // `extends A, B`: one parent only (TYP-16); the rest is skipped
      SourceLoc Comma = Tok.Loc;
      while (is(tok::comma)) {
        next();
        parseType();
      }
      if (shouldReport(Comma))
        Diags.report(Comma, diag::err_extends_one_parent)
            .range(SourceRange(Comma, PrevEnd));
    }
    Spec->Type = parseStructBody(Tok.Loc);
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
      parseTypeSpecRhs(Spec);
    }

    setEnd(Spec, PrevEnd);
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
        return parseImportSpec(D->Lparen.isValid());
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
      setEnd(D, PrevEnd);
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

    Ident *Name =
        Recv && atOperatorNameAfterRecv() ? parseOperatorName() : parseIdent();
    if (is(tok::lss) && !isOperatorSpelling(Name->Name.str()))
      skipAngleTypeParams(Name); // `func Map<T any>(...)`

    FuncType *FT = mk<FuncType>(Pos);
    FT->FuncLoc = Pos;
    if (is(tok::l_brack)) {
      FieldList *TParams = parseTypeParameters();
      if (Recv && TParams && !Opts.AllowGenericMethods) {
        // Go: method declarations do not have type parameters. We parse them
        // for a better error message and improved error recovery.
        error(TParams->Opening, diag::err_method_type_params);
        TParams = nullptr;
      }
      FT->TypeParams = TParams;
    }
    parseSignature(FT);
    checkUnaryAmp(Name, FT);

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
        if (shouldReport(Tok.Loc)) {
          DiagnosticBuilder B =
              Diags.report(Tok.Loc, diag::err_newline_before_lbrace);
          if (SourceRange Gap = blankGap(PrevEnd, Tok.Loc); Gap.isValid())
            B.fixit(Gap, " ");
        }
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

  /// `func Map<T any>(...)`: type parameters in angle brackets (Java, C#,
  /// TypeScript, Rust). Reports them, with a fix-it that makes the brackets
  /// square, and skips them up to the parameter list.
  void skipAngleTypeParams(const Ident *Name) {
    SourceLoc Lt = Tok.Loc, Gt;
    int Depth = 0;
    while (!is(tok::eof) && !is(tok::l_paren) && !is(tok::l_brace) &&
           !is(tok::semi)) {
      if (is(tok::lss))
        ++Depth;
      else if (is(tok::gtr))
        --Depth;
      else if (is(tok::shr)) // `Map<K, List<V>>`
        Depth -= 2;
      bool Closed = Depth <= 0;
      if (Closed && is(tok::gtr) && Depth == 0)
        Gt = Tok.Loc;
      next();
      if (Closed)
        break;
    }
    if (!shouldReport(Lt))
      return;
    DiagnosticBuilder B = Diags.report(Lt, diag::err_angle_type_params);
    B.range(SourceRange(Lt, Gt.isValid() ? after(Gt) : PrevEnd));
    B << Name->Name.str();
    if (!Gt.isValid() || hasCommentIn(SourceRange(Lt, Gt)))
      return;
    bool Same = false;
    std::string Inner;
    if (!squareTypeParams(srcText(after(Lt), Gt), Inner, Same))
      return; // no exact fix: the error stays
    if (Same) {
      B.fixit(SourceRange(Lt, after(Lt)), "[");
      B.fixit(SourceRange(Gt, after(Gt)), "]");
    } else {
      B.fixit(SourceRange(Lt, after(Gt)), "[" + Inner + "]");
    }
  }

  /// The text inside `<...>` of `func Map<...>` as the inside of `[...]`.
  /// Every piece must be `Name`, `Name C`, `Name extends C` or `Name: C`, with
  /// Name one identifier and C one identifier or `pkg.Name`; each parameter
  /// gets its own constraint (`T` -> `T any`, so `<T, U extends C>` becomes
  /// `[T any, U C]`). Returns false for anything else, or a word that is a
  /// keyword, a foreign modifier (`in`, `const`, ...) or a predeclared type
  /// used as a name (`<T: int, string>` is a union, not a parameter); the
  /// error then has no fix-it. Same is set if the text is unchanged.
  static bool squareTypeParams(llvm::StringRef In, std::string &Out,
                               bool &Same) {
    auto isWordChar = [](char C) {
      return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
    };
    auto isIdent = [&](llvm::StringRef S) {
      if (S.empty() || std::isdigit(static_cast<unsigned char>(S[0])))
        return false;
      return llvm::all_of(S, isWordChar) &&
             tok::lookupKeyword(S) == tok::ident &&
             !llvm::is_contained(
                 {"in", "out", "const", "reified", "super", "where", "extends"},
                 S);
    };
    SmallVector<llvm::StringRef, 4> Pieces;
    In.split(Pieces, ',');
    Out.clear();
    for (size_t I = 0; I < Pieces.size(); ++I) {
      llvm::StringRef P = Pieces[I].trim(" \t");
      llvm::StringRef Name = P.take_while(isWordChar);
      llvm::StringRef C = P.drop_front(Name.size()).ltrim(" \t");
      if (!isIdent(Name) || isPredeclaredType(Name))
        return false;
      if (C.starts_with(":"))
        C = C.drop_front().ltrim(" \t");
      else if (C.starts_with("extends") && C.size() > 7 &&
               (C[7] == ' ' || C[7] == '\t'))
        C = C.drop_front(7).ltrim(" \t");
      if (C.empty()) {
        // a name alone, but not `Name:` / `Name extends`
        if (P.size() != Name.size())
          return false;
        C = "any";
      } else {
        auto [Pkg, Rest] = C.split('.');
        bool Qualified = C.contains('.');
        if (!isIdent(Qualified ? Pkg : C) || (Qualified && !isIdent(Rest)))
          return false;
      }
      if (I)
        Out += ", ";
      Out += Name.str() + " " + C.str();
    }
    Same = Out == In;
    return !Out.empty();
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
      setEnd(D, PrevEnd);
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
      setEnd(P, PrevEnd);
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
    setEnd(D, PrevEnd);
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
      setEnd(D, PrevEnd);
      expectSemi();
      return D;
    }
    case tok::at:
      return parseAttributedDecl();
    default:
      break;
    }
    if (atUnitDecl())
      return parseUnitDecl({});
    if (is(tok::ident) || is(tok::kw_struct))
      if (Decl *D = tailoredDecl())
        return D;
    SourceLoc L = Tok.Loc;
    if (startsMisplacedStmt()) {
      // `println("hi")`, `if x {...}` at the top level: Go's message. The
      // statement is parsed and dropped, so that it causes no follow-on errors.
      error(L, diag::err_stmt_outside_func);
      parseStmt();
      return mk<BadDecl>(L, PrevEnd);
    }
    errorExpected(L, "declaration");
    advance(DeclStart);
    return mk<BadDecl>(L, Tok.Loc < L ? L : Tok.Loc);
  }

  /// At the top level: does a statement (not a declaration) start here? Only
  /// forms that cannot be a misspelled declaration count: a keyword that
  /// starts a statement, or a name followed by what continues an expression
  /// statement or an assignment (`f(x)`, `x.y = 1`, `x++`). `fucn main()` is
  /// not one of them.
  bool startsMisplacedStmt() {
    switch (Tok.Kind) {
    case tok::kw_if:
    case tok::kw_for:
    case tok::kw_switch:
    case tok::kw_select:
    case tok::kw_return:
    case tok::kw_go:
    case tok::kw_defer:
    case tok::kw_break:
    case tok::kw_continue:
    case tok::kw_goto:
    case tok::kw_fallthrough:
      return true;
    case tok::ident:
      switch (peek(1).Kind) {
      case tok::l_paren:
      case tok::period:
      case tok::l_brack:
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
      case tok::and_not_assign:
      case tok::inc:
      case tok::dec:
      case tok::arrow:
        return true;
      default:
        return false;
      }
    default:
      return false;
    }
  }

  /// P02.07 at the top level (the current token is an identifier or
  /// `struct`): `x := 1`, `function f() {}`, `struct S {...}` and
  /// `class C {...}`. Returns null if none of them starts here.
  Decl *tailoredDecl() {
    if ((is(tok::kw_struct) || isWord(Tok, "class")) &&
        peek(1).is(tok::ident) && peek(2).is(tok::l_brace))
      return tailoredStructDecl();
    if (!is(tok::ident))
      return nullptr;
    llvm::StringRef W = text(Tok);
    if (W == "function" || W == "fn" || W == "def" || W == "fun") {
      if (!peek(1).is(tok::ident))
        return nullptr;
      if (shouldReport(Tok.Loc))
        Diags.report(Tok.Loc, diag::err_fn_keyword)
                .range(Tok.getRange())
                .fixit(Tok.getRange(), "func")
            << W;
      Tok.Kind = tok::kw_func;
      return parseFuncDecl({});
    }
    // ident { "," ident } ":="
    unsigned I = 1;
    while (peek(I).is(tok::comma) && peek(I + 1).is(tok::ident))
      I += 2;
    if (!peek(I).is(tok::define))
      return nullptr;
    SourceLoc Begin = Tok.Loc;
    SmallVector<Ident *, 8> Idents;
    parseIdentList(Idents);
    SourceLoc DefineLoc = Tok.Loc;
    if (shouldReport(Begin))
      Diags.report(DefineLoc, diag::err_define_at_top_level)
              .range(SourceRange(Begin, Tok.getEndLoc()))
              .insert(Begin, "var ")
              .fixit(Tok.getRange(), "=")
          << Idents.front()->Name.str();
    next(); // :=
    SmallVector<Expr *, 8> Values;
    parseList(Values, true);
    SourceLoc End = PrevEnd;
    expectSemi();
    ValueSpec *VS = mk<ValueSpec>(Begin, End);
    VS->Names = Ctx.copy(ArrayRef<Ident *>(Idents));
    VS->Values = Ctx.copy(ArrayRef<Expr *>(Values));
    GenDecl *D = mk<GenDecl>(Begin, End);
    D->KeywordLoc = Begin;
    D->Keyword = tok::kw_var;
    Spec *Specs[] = {VS};
    D->Specs = Ctx.copy(ArrayRef<Spec *>(Specs));
    return D;
  }

  /// `struct Name { ... }` (C) or `class Name { ... }` at the top level; the
  /// body is read as the fields of a struct type.
  Decl *tailoredStructDecl() {
    SourceLoc Begin = Tok.Loc;
    Token NameTok = peek(1);
    if (shouldReport(Begin)) {
      SourceRange R(Begin, NameTok.getEndLoc());
      DiagnosticBuilder B = Diags.report(Begin, diag::err_struct_decl);
      B.range(R);
      fixitKeepingComments(B, R, "type " + text(NameTok).str() + " struct");
      B << text(Tok) << text(NameTok);
    }
    next(); // struct or class
    Ident *Name = parseIdent();
    // parseStructType wants the keyword: the current token is '{'.
    Expr *T = parseStructBody(Begin);
    SourceLoc End = PrevEnd;
    expectSemi();
    TypeSpec *TS = mk<TypeSpec>(Begin, End);
    TS->Name = Name;
    TS->Type = T;
    GenDecl *D = mk<GenDecl>(Begin, End);
    D->KeywordLoc = Begin;
    D->Keyword = tok::kw_type;
    Spec *Specs[] = {TS};
    D->Specs = Ctx.copy(ArrayRef<Spec *>(Specs));
    return D;
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
      if (atUnitDecl())
        return parseUnitDecl(A);
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

  /// Variables of the enclosing loops' headers while their bodies are parsed
  /// (RNG-8); only filled if Opts.CheckLanguageRules.
  SmallVector<std::pair<Symbol, SourceLoc>, 8> LoopVars;

  SmallVector<Comment, 16> Comments;
  SmallVector<CommentGroup, 16> Groups;
  SourceLoc PrevTokLoc; // the previous token, for groupComments
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
  F->CommentGroups = Ctx.copy(ArrayRef<CommentGroup>(Groups));
  return F;
}

} // namespace

ast::File *gopp::parseFile(ASTContext &Ctx, const SourceManager &SM,
                           FileID File, DiagnosticsEngine &Diags,
                           ParseOptions Opts) {
  Parser P(Ctx, SM, File, Diags, Opts);
  return P.parseFile();
}

namespace {

/// Parses Files[I] for every I into Out[I] on Pool; the diagnostics of every
/// file go to a private queue and are replayed into Diags in file order.
void parseOnPool(const SourceManager &SM, ArrayRef<FileID> Files,
                 DiagnosticsEngine &Diags, const ParseOptions &Opts,
                 llvm::ThreadPoolInterface &Pool,
                 std::vector<ParsedFile> &Out) {
  // Each file reports into its own engine, with the package's error limit.
  std::vector<CollectingDiagnosticConsumer> Queues(Files.size());
  llvm::ThreadPoolTaskGroup Group(Pool);
  for (size_t I = 0; I < Files.size(); ++I) {
    Group.async([&, I] {
      DiagnosticsEngine Local(SM, Queues[I], Diags.getErrorLimit());
      Out[I].AST = parseFile(*Out[I].Ctx, SM, Files[I], Local, Opts);
    });
  }
  Group.wait();
  for (CollectingDiagnosticConsumer &Q : Queues)
    for (Diagnostic &D : Q.Diags)
      Diags.emit(std::move(D));
}

std::vector<ParsedFile> makeSlots(size_t N) {
  std::vector<ParsedFile> Out(N);
  for (ParsedFile &P : Out)
    P.Ctx = std::make_unique<ASTContext>();
  return Out;
}

} // namespace

std::vector<ParsedFile> gopp::parseFiles(const SourceManager &SM,
                                         ArrayRef<FileID> Files,
                                         DiagnosticsEngine &Diags,
                                         ParseOptions Opts,
                                         unsigned NumThreads) {
  std::vector<ParsedFile> Out = makeSlots(Files.size());
  unsigned Workers = NumThreads
                         ? NumThreads
                         : llvm::hardware_concurrency().compute_thread_count();
  Workers = std::min<unsigned>(Workers, Files.size());
  if (Workers <= 1) {
    for (size_t I = 0; I < Files.size(); ++I)
      Out[I].AST = parseFile(*Out[I].Ctx, SM, Files[I], Diags, Opts);
    return Out;
  }
  llvm::DefaultThreadPool Pool(llvm::hardware_concurrency(Workers));
  parseOnPool(SM, Files, Diags, Opts, Pool, Out);
  return Out;
}

std::vector<ParsedFile> gopp::parseFiles(const SourceManager &SM,
                                         ArrayRef<FileID> Files,
                                         DiagnosticsEngine &Diags,
                                         ParseOptions Opts,
                                         llvm::ThreadPoolInterface &Pool) {
  std::vector<ParsedFile> Out = makeSlots(Files.size());
  parseOnPool(SM, Files, Diags, Opts, Pool, Out);
  return Out;
}
