//===- LowerBang.cpp - Lowering of the unwrapping call f!(...) ------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.10. `f!(args)` (ERR-5 to ERR-9) is the plain call, a test of the error
// word (the last value of the call's result), and two edges:
//
//   ok    the value is the results without the error;
//   fail  (a cold block, ERR-8) what Info::Bangs says (the checker decided it
//         from the innermost function, ERR-6):
//           Return  the enclosing function returns the error, zero values for
//                   the other results (ERR-3, ERR-7);
//           Panic   panic with errors.MustError{Err, Expr, Pos} (ERR-9).
//
// `go f!()` and `defer f!()` (BangKind::DeferSet) are statements the lowering
// refuses as a whole (P12, P04.26), so they never get here.
//
// The decision is in this file only: every form of call (a function, a method,
// an interface method) reaches it through lowerCall, because it wraps
// lowerPlainCall; the operand positions need nothing of their own, since the
// failing edge leaves the function (or the program) before any later operand
// is evaluated, which is Go's order.
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"

#include "gopp/Lex/Lexer.h"

#include "llvm/Support/Path.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;
using llvm::dyn_cast_or_null;

namespace {
/// The longest text of the call a panic message shows; a longer call ends in
/// "..." (the text of a nest of n `!` calls would be quadratic otherwise).
constexpr size_t MaxExprText = 120;
/// The most bytes of source read for it (comments and blanks take no output,
/// so a call that hides its text in them is cut too).
constexpr uint32_t MaxScan = 2 * MaxExprText;
} // namespace

// The text is made from the tokens of the call (the Go++ lexer on the source
// of the call, `!` left out), not from its bytes: comments are dropped,
// literals are copied whole and unchanged, and the source's own spacing is
// kept: one blank where the source had white space or a comment between two
// tokens (a run is one blank), nothing where it had nothing, and no blank
// inside a bracket, after a dot or before a comma, whatever the line breaks.
// Only the outer
// `!` is left out (inner ones stay as written). A trailing comma before `)` is
// layout and is dropped. A raw string with a newline in it shows each newline
// as `\n`, so the message stays on one line.
std::string Body::bangExprText(const ast::CallExpr *C) {
  FileID F = L.SM.getFileID(C->getBegin());
  std::string Out;
  if (!F.isValid() || !C->getEnd().isValid())
    return Out;
  StringRef Buf = L.SM.getBuffer(F);
  uint32_t B = L.SM.getFileOffset(C->getBegin());
  uint32_t E = std::min<uint32_t>(L.SM.getFileOffset(C->getEnd()), Buf.size());
  if (E <= B)
    return Out;
  uint32_t Bang = L.SM.getFileOffset(C->BangLoc) - B;
  bool Truncated = false;
  if (E - B > MaxScan) {
    E = B + MaxScan;
    Truncated = true;
  }
  // The lexer reads a whole buffer, so the call is lexed as a buffer of its
  // own (at most MaxScan bytes).
  SourceManager SM;
  llvm::Expected<FileID> Slice = SM.addMemoryBuffer("bang", Buf.slice(B, E));
  if (!Slice) {
    llvm::consumeError(Slice.takeError());
    return Out;
  }
  CollectingDiagnosticConsumer Ignore; // a cut literal is no error here
  DiagnosticsEngine Diags(SM, Ignore, 0);
  LexerOptions LO;
  LO.InsertSemis = false;
  Lexer Lx(SM, *Slice, Diags, LO);
  uint32_t Base = SM.getFileOffset(SM.getStartLoc(*Slice));
  tok::Kind Prev = tok::eof;
  uint32_t Read = 0;
  for (;;) {
    Token T = Lx.lex();
    if (T.is(tok::eof))
      break;
    uint32_t At = SM.getFileOffset(T.getLoc()) - Base;
    bool Gap = At > Read; // white space or a comment since the last token
    Read = At + T.Length;
    if (At == Bang) {
      Prev = tok::eof; // no blank around the outer `!`
      continue;
    }
    tok::Kind K = T.Kind;
    if (K == tok::r_paren && Prev == tok::comma) {
      Out.pop_back(); // `f(a,\n b,\n)`: the last comma is layout
      Gap = false;
    }
    // A gap right inside a bracket, after a dot or before a comma, a dot or a
    // closing bracket is layout (a line break, a comment), never text.
    if (Prev == tok::l_paren || Prev == tok::l_brack || Prev == tok::period ||
        K == tok::r_paren || K == tok::r_brack || K == tok::comma ||
        K == tok::period)
      Gap = false;
    if (Gap && Prev != tok::eof)
      Out += ' ';
    StringRef Text = Lx.getText(T);
    if (K == tok::string_lit && Text.starts_with("`")) {
      for (char Ch : Text) {
        if (Ch == '\n')
          Out += "\\n";
        else if (Ch != '\r')
          Out += Ch;
      }
    } else {
      Out += Text;
    }
    Prev = K;
    if (Out.size() > MaxExprText) {
      Truncated = true;
      break;
    }
  }
  LowerSteps += Read; // the scan builds no GIR, so it counts itself
  if (Out.size() > MaxExprText) {
    // Cut at a character boundary: back off while the byte after the cut is
    // inside a character (a cut right after a whole character keeps it).
    size_t Cut = MaxExprText;
    while (Cut > 0 && (static_cast<unsigned char>(Out[Cut]) & 0xC0) == 0x80)
      --Cut;
    Out.resize(Cut);
  }
  if (Truncated)
    Out += "...";
  return Out;
}

std::string Body::bangPosText(const ast::CallExpr *C) {
  PresumedLoc P = L.SM.getPresumedLoc(C->getBegin());
  if (!P.isValid())
    return "?";
  return (llvm::sys::path::filename(P.Filename) + ":" + llvm::Twine(P.Line) +
          ":" + llvm::Twine(P.Column))
      .str();
}

void Body::bangReturn(Operand Err) {
  Function &F = FB.function();
  Type *Ret = F.Sig->ret();
  if (Ret->isIface()) {
    FB.assign(FB.local(FB.returnPlace()), FB.use(std::move(Err)));
  } else {
    // (T..., error): zero values, then the error. A result with no zero
    // value is left unset, which for GIR is its zero bits (ERR-3).
    SmallVector<Operand, 4> Vals;
    for (unsigned K = 0; K + 1 < Ret->numFields(); ++K)
      Vals.push_back(zeroOf(Ret->fieldType(K, ctx())));
    Vals.push_back(std::move(Err));
    FB.assign(FB.local(FB.returnPlace()),
              FB.aggregate(AggKind::Tuple, Ret, Vals));
  }
  exitScopes(0);
  FB.ret();
}

void Body::bangPanic(const ast::CallExpr *C, Operand Err) {
  if (BangPanicBB == NoId) {
    FuncId P = L.ensurePanicMust(C->getBegin());
    if (P == NoId) {
      FB.unreachable(); // reported; the module fails
      return;
    }
    // The shared block, made once (ADR-0028 rule 3.3): it calls the runtime
    // with what the failing edges stored in three locals.
    BangExprSlot = FB.newLocal(ctx().string(), Local::Var, "bang.expr");
    BangPosSlot = FB.newLocal(ctx().string(), Local::Var, "bang.pos");
    BangErrSlot = FB.newLocal(ctx().iface(), Local::Var, "bang.err");
    BlockId Here = FB.block();
    Span Sp = FB.span();
    BangPanicBB = FB.newBlock();
    FB.function().Blocks[BangPanicBB].Cold = true;
    FB.setBlock(BangPanicBB);
    FB.setSpan(Sp);
    FB.call(constFunc(M, P),
            {copyOf(FB.local(BangExprSlot)), copyOf(FB.local(BangPosSlot)),
             copyOf(FB.local(BangErrSlot))},
            FB.local(FB.newTemp(ctx().unit())), {UnwindAction::Continue, NoId});
    FB.unreachable();
    FB.setBlock(Here);
  }
  FB.assign(FB.local(BangErrSlot), FB.use(std::move(Err)));
  FB.assign(FB.local(BangExprSlot), FB.use(constStr(M, bangExprText(C))));
  FB.assign(FB.local(BangPosSlot), FB.use(constStr(M, bangPosText(C))));
  FB.gotoBlock(BangPanicBB);
}

void Body::bangFail(const ast::CallExpr *C, S::BangKind K, Operand Err) {
  at(C);
  if (K == S::BangKind::Return)
    return bangReturn(std::move(Err));
  if (K == S::BangKind::Panic)
    return bangPanic(C, std::move(Err));
  // DeferSet: only `defer f!()`, which no statement lowers yet (P04.26).
  L.internalError(C->getBegin(), "a deferred ! reached the lowering");
  FB.unreachable();
}

Operand Body::lowerBang(const ast::CallExpr *C) {
  auto It = I.Bangs.find(C);
  if (It == I.Bangs.end()) {
    // The checker records every `!` it accepts; a call it rejected never
    // gets here.
    L.internalError(C->getBegin(), "no record of what this ! does");
    return zeroOfExpr(C);
  }
  const S::BangKind K = It->second;
  Operand Whole = lowerPlainCall(C);
  // The values the call keeps: the result type is a tuple (none, or two or
  // more) or the one value.
  const S::TypeAndValue *CTV = I.lookup(C);
  auto *KT = dyn_cast_or_null<S::TupleType>(sTyOf(C));
  const size_t NK = CTV && CTV->M == S::Mode::NoValue ? 0
                    : KT                              ? KT->Elems.size()
                                                      : 1;
  Type *WT = Whole.type();
  const bool Shaped = WT && (NK == 0 ? WT->isIface()
                                     : WT->getKind() == Type::Kind::Tuple &&
                                           WT->numFields() == NK + 1);
  if (!Shaped) {
    // The call was refused (and said so), or the shape is not the one the
    // checker's types give: never a silent wrong value.
    if (!L.Failed)
      L.internalError(C->getBegin(), "the result of this ! call");
    return zeroOfExpr(C);
  }
  at(C);
  Place Res;
  Place ErrP;
  if (NK == 0) {
    ErrP = placeOf(std::move(Whole));
  } else {
    Res = placeOf(std::move(Whole));
    ErrP = FB.field(Res, NK);
  }
  // The error word: a nil error has a null itab. A non-nil interface that
  // holds a nil pointer is an error (Go's rule), so only the itab is tested.
  Rt E(M, FB);
  Val Itab = E.snap(E.val(FB.field(ErrP, 0)));
  Val IsNil = Itab == E.null(ctx().ptr(ctx().u8()));
  Operand Err = copyOf(ErrP);
  BlockId Ok = FB.newBlock(), Fail = FB.newBlock();
  FB.function().Blocks[Fail].Cold = true;
  FB.branch(IsNil.Op, Ok, Fail);
  FB.setBlock(Fail);
  bangFail(C, K, std::move(Err));
  FB.setBlock(Ok);
  at(C);
  if (NK == 0)
    return unitValue();
  if (NK == 1)
    return copyOf(FB.field(Res, 0));
  SmallVector<Type *, 4> Ts;
  SmallVector<Operand, 4> Vs;
  for (size_t K2 = 0; K2 < NK; ++K2) {
    Ts.push_back(WT->fieldType(K2, ctx()));
    Vs.push_back(copyOf(FB.field(Res, K2)));
  }
  return copyOf(FB.temp(FB.aggregate(AggKind::Tuple, ctx().tuple(Ts), Vs)));
}

bool Body::lowerFailsReturn(const ast::ReturnStmt *R,
                            SmallVector<Operand, 2> &Vals) {
  // The signature of the function or literal being lowered (P04.07: a
  // `func!` literal returns the same way): the declared results, then the
  // error.
  S::SignatureType *Sig = FnSig;
  ArrayRef<S::Type *> Rs = ArrayRef<S::Type *>(Sig->Results->Elems).drop_back();
  const size_t N = Rs.size();
  S::Type *ErrTy = Sig->Results->Elems.back();
  Type *ErrG = L.lowerType(ErrTy, R->getBegin());
  if (!ErrG)
    return false;
  if (R->Results.empty()) {
    // A bare return: the named results and a nil error.
    for (const Place &Nm : NamedResults)
      Vals.push_back(copyOf(Nm));
    Vals.push_back(zeroOf(ErrG));
    return true;
  }
  // `return g()` where g gives N or N+1 values (the operand count decides,
  // ERR-3: never ambiguous, as the error cannot be declared last, ERR-15).
  const size_t Have = R->Results.size();
  auto *TT = Have == 1 ? dyn_cast_or_null<S::TupleType>(sTyOf(R->Results[0]))
                       : nullptr;
  if (TT && (TT->Elems.size() == N || TT->Elems.size() == N + 1)) {
    const size_t Want = TT->Elems.size();
    SmallVector<S::Type *, 4> To(Rs.begin(), Rs.end());
    if (Want == N + 1)
      To.push_back(ErrTy);
    if (!multiValues(R->Results[0], Want, Vals, To))
      return false;
    if (Want == N)
      Vals.push_back(zeroOf(ErrG));
    return true;
  }
  if (Have == N || Have == N + 1) {
    for (size_t K = 0; K < Have; ++K)
      Vals.push_back(valueFor(R->Results[K], K < N ? Rs[K] : ErrTy));
    if (Have == N)
      Vals.push_back(zeroOf(ErrG));
    return true;
  }
  unsupported(R, "this return");
  return false;
}
