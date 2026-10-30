//===- GIRTestSupport.h - Hand-built GIR for unit tests --------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A valid module with the shapes most verifier rules look at, so a test can
// break exactly one rule, plus the expectations that name the rule. Every
// test that builds GIR runs the verifier (P04.01).
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_TESTS_GIRTESTSUPPORT_H
#define GOPP_TESTS_GIRTESTSUPPORT_H

#include "gopp/GIR/Builder.h"
#include "gopp/GIR/Print.h"
#include "gopp/GIR/Verify.h"

#include "gtest/gtest.h"

#include <string>
#include <vector>

namespace gopp::test {

/// The verifier's errors of M, one per line ("" if M is valid).
inline std::string errorsOf(const gir::Module &M, unsigned Max = 50) {
  std::string S;
  for (const gir::VerifyError &E : gir::verify(M, Max))
    S += E.str() + "\n";
  return S;
}

inline bool hasRule(const gir::Module &M, const char *Rule) {
  for (const gir::VerifyError &E : gir::verify(M, 1000))
    if (E.Rule == Rule)
      return true;
  return false;
}

inline std::string textOf(const gir::Module &M) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  gir::print(OS, M);
  return S;
}

/// The module passes the verifier.
#define EXPECT_GIR_VALID(M) EXPECT_EQ(::gopp::test::errorsOf(M), "")

/// The module breaks Rule, and the verifier says so under that id.
#define EXPECT_GIR_RULE(M, Rule)                                               \
  EXPECT_TRUE(::gopp::test::hasRule(M, Rule)) << "expected " Rule ", got:\n"   \
                                              << ::gopp::test::errorsOf(M)

/// A function without a body (a declaration until something builds it).
inline gir::FuncId declare(gir::Module &M, llvm::StringRef Name,
                           gir::Type *Sig) {
  auto F = std::make_unique<gir::Function>();
  F->Name = Name.str();
  F->Sig = Sig;
  return M.addFunction(std::move(F));
}

/// A terminator made by hand (FuncBuilder makes only valid ones).
inline gir::Terminator term(gir::Terminator::Kind K,
                            gir::BlockId Target = gir::NoId) {
  gir::Terminator T;
  T.K = K;
  T.Target = Target;
  T.Set = true;
  return T;
}

/// A place made by hand, with the given projections; its type is Ty.
inline gir::Place placeOf(gir::LocalId L, gir::Type *Ty,
                          std::initializer_list<gir::PlaceElem> Proj = {}) {
  gir::Place P = gir::localPlace(L, Ty);
  P.Proj.assign(Proj.begin(), Proj.end());
  return P;
}

inline gir::PlaceElem elem(gir::PlaceElem::Kind K, uint32_t Val,
                           gir::Type *Ty) {
  gir::PlaceElem E;
  E.K = K;
  E.Val = Val;
  E.Ty = Ty;
  return E;
}

/// A valid module:
///   declare ext(i64) -> i64                (can unwind)
///   f(a i64, b bool) -> i64                bb0: _3 = add(a, 1); switch b
///                                          [0: bb2, otherwise: bb1]
///                                          bb1: _0 = _3; return
///                                          bb2: _0 = 7; return
///   g(x i64) -> ()                         bb0: _2 = call ext(x) ->
///                                          [return: bb2, unwind: cleanup bb1]
///                                          bb1 (cleanup): resume
///                                          bb2: return
struct GoodModule {
  gir::Context C;
  gir::Module M{C, "test"};
  gir::Type *I64 = C.i64(), *U64 = C.u64(), *I32 = C.i32(), *U8 = C.u8(),
            *Bool = C.boolean(), *Unit = C.unit(), *F64 = C.floatTy(64);
  gir::FuncId Ext, F, G;

  GoodModule() {
    using namespace gir;
    Ext = declare(M, "ext", C.func({I64}, I64));
    F = declare(M, "f", C.func({I64, Bool}, I64));
    {
      FuncBuilder B(M, M.func(F));
      Place T =
          B.temp(B.binary(BinOp::Add, copyOf(B.local(1)), constInt(I64, 1)));
      BlockId Then = B.newBlock(), Else = B.newBlock();
      B.branch(copyOf(B.local(2)), Then, Else);
      B.setBlock(Then);
      B.assign(B.local(0), B.use(copyOf(T)));
      B.ret();
      B.setBlock(Else);
      B.assign(B.local(0), B.use(constInt(I64, 7)));
      B.ret();
    }
    G = declare(M, "g", C.func({I64}, Unit));
    {
      FuncBuilder B(M, M.func(G));
      LocalId R = B.newTemp(I64);
      BlockId Cl = B.newBlock(/*Cleanup=*/true);
      B.call(constFunc(M, Ext), {copyOf(B.local(1))}, B.local(R),
             {UnwindAction::Cleanup, Cl});
      B.ret();
      B.setBlock(Cl);
      B.resume();
    }
  }

  gir::Function &fn(gir::FuncId Id) { return M.func(Id); }
  gir::Terminator &termOf(gir::FuncId Fn, gir::BlockId B) {
    return M.func(Fn).Blocks[B].Term;
  }
  /// Appends `Dest = RV` to the block (FuncBuilder would refuse a block
  /// that has its terminator).
  void append(gir::FuncId Fn, gir::BlockId B, gir::Place Dest, gir::Rvalue RV) {
    gir::Statement S;
    S.K = gir::Statement::Assign;
    S.Dest = std::move(Dest);
    S.RV = std::move(RV);
    M.func(Fn).Blocks[B].Stmts.push_back(std::move(S));
  }
};

} // namespace gopp::test

#endif // GOPP_TESTS_GIRTESTSUPPORT_H
