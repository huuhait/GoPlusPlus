//===- GIRTest.cpp - GIR types, verifier, bitstream and builder -----------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.01. A hand-built module uses every kind of statement, rvalue and
// terminator, including the unwind and cleanup edges the hello subset never
// makes, so the verifier, the printer and the bitstream are tested on all of
// GIR and not only on what the builder makes today. The verifier is tested on
// both sides: the good module passes, and each mutation that breaks one rule
// is reported by that rule.
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/Builder.h"
#include "gopp/GIR/Lower.h"
#include "gopp/GIR/Print.h"
#include "gopp/GIR/Serialize.h"
#include "gopp/GIR/Verify.h"
#include "gopp/Sema/Loader.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <gtest/gtest.h>

#include <fstream>
#include <functional>

using namespace gopp;
using namespace gopp::gir;

namespace {

std::string text(const Module &M) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  print(OS, M);
  return S;
}

std::string errorsOf(const Module &M) {
  std::string S;
  for (const VerifyError &E : verify(M))
    S += E.str() + "\n";
  return S;
}

/// A module with one of everything.
std::unique_ptr<Module> sample(Context &C) {
  auto M = std::make_unique<Module>(C, "sample");
  uint32_t File = M->addFile("sample.gopp");
  Type *I64 = C.i64(), *U8 = C.u8(), *Bool = C.boolean();
  Type *Pair = C.structTy("sample.Pair");
  C.setStructBody(Pair, {I64, C.floatTy(32)});

  // declare fn callee(i64) -> i64
  auto Callee = std::make_unique<Function>();
  Callee->Name = "callee";
  Callee->Sig = C.func({I64}, I64);
  Callee->NoUnwind = false;
  FuncId CalleeId = M->addFunction(std::move(Callee));

  auto F = std::make_unique<Function>();
  F->Name = "sample.f";
  F->Sig = C.func({I64, C.ptr(I64)}, I64);
  F->Link = Linkage::Internal;
  F->Loc = {File + 1, 3, 1};
  Function &Fn = *F;
  M->addFunction(std::move(F));

  FuncBuilder B(*M, Fn);
  B.setSpan({File + 1, 4, 2});
  LocalId Arr = B.newLocal(C.array(U8, 4), Local::Var, "arr");
  LocalId Str = B.newLocal(C.string(), Local::Var, "s");
  LocalId Pr = B.newLocal(Pair, Local::Var, "pair");
  LocalId Idx = B.newLocal(C.u64(), Local::Var, "i");
  LocalId Res = B.newLocal(I64, Local::Temp);
  B.storageLive(Arr);
  B.assign(B.local(Idx), B.use(constInt(C.u64(), 2)));
  B.assign(B.index(B.local(Arr), Idx), B.use(constInt(U8, 7)));
  B.assign(B.constIndex(B.local(Arr), 0), B.use(constInt(U8, 1)));
  B.assign(B.local(Str), B.use(constStr(*M, "hi")));
  Place Len = B.temp(B.len(B.local(Str)));
  Place Len2 = B.temp(B.len(B.local(Arr)));
  (void)Len2;
  B.assign(B.local(Pr),
           B.aggregate(AggKind::Struct, Pair,
                       {constInt(I64, 5), constFloat(C.floatTy(32), 1.5)}));
  Place Tup = B.temp(B.aggregate(AggKind::Tuple, C.tuple({I64, Bool}),
                                 {constInt(I64, 1), constBool(C, true)}));
  Place Ptr = B.temp(B.addrOf(B.field(B.local(Pr), 0), true));
  Place Loaded = B.temp(B.use(copyOf(B.deref(Ptr))));
  Place Casted =
      B.temp(B.cast(CastKind::IntToFloat, copyOf(Loaded), C.floatTy(64)));
  Place Back = B.temp(B.cast(CastKind::FloatToInt, copyOf(Casted), I64));
  Place Neg = B.temp(B.unary(UnOp::Neg, copyOf(Back)));
  Place Shift = B.temp(B.binary(BinOp::Shl, copyOf(Neg), constInt(C.u64(), 1)));
  Place Cmp = B.temp(B.binary(BinOp::Lt, copyOf(Shift), constInt(I64, 9)));
  B.assign(B.deref(B.local(B.param(1))), B.use(copyOf(Shift)));
  B.assign(B.local(B.returnPlace()), B.use(copyOf(Back)));
  (void)Len;
  (void)Tup;

  BlockId Cleanup = B.newBlock(/*Cleanup=*/true);
  BlockId Cleanup2 = B.newBlock(/*Cleanup=*/true);
  BlockId Then = B.newBlock(), Else = B.newBlock(), Join = B.newBlock();
  B.branch(copyOf(Cmp), Then, Else);

  B.setBlock(Then);
  B.call(constFunc(*M, CalleeId), {copyOf(B.local(B.param(0)))}, B.local(Res),
         {UnwindAction::Cleanup, Cleanup}, Join);
  B.setBlock(Else);
  BlockId After = B.assertThat(copyOf(Cmp), false, AssertKind::Overflow,
                               {UnwindAction::Cleanup, Cleanup});
  (void)After;
  B.drop(B.local(Str), {UnwindAction::Terminate, NoId});
  B.switchInt(copyOf(B.local(B.param(0))), {{1, Join}, {2, Join}}, Join);

  B.setBlock(Join);
  B.storageDead(Arr);
  B.ret();

  // A cleanup chain: drop (a panic in it terminates), then the next cleanup
  // block resumes.
  B.setBlock(Cleanup);
  B.drop(B.local(Str), {UnwindAction::Terminate, NoId}, Cleanup2);
  B.setBlock(Cleanup2);
  B.resume();
  return M;
}

TEST(GIRTypes, AreUniquedAndPrinted) {
  Context C;
  EXPECT_EQ(C.intTy(64, true), C.i64());
  EXPECT_NE(C.intTy(64, false), C.i64());
  EXPECT_EQ(C.ptr(C.i64()), C.ptr(C.i64()));
  EXPECT_EQ(C.tuple({}), C.unit());
  EXPECT_NE(C.tuple({C.i64()}), C.i64());
  Type *F = C.func({C.i64(), C.string()}, C.unit(), /*Fails=*/true);
  EXPECT_EQ(F, C.func({C.i64(), C.string()}, C.unit(), true));
  EXPECT_NE(F, C.func({C.i64(), C.string()}, C.unit(), false));
  EXPECT_EQ(F->str(), "fn(i64, string)! -> ()");
  EXPECT_EQ(C.array(C.slice(C.u8()), 3)->str(), "[3][]u8");
  // Structs are nominal and get a body once.
  Type *S = C.structTy("p.S");
  EXPECT_EQ(S, C.structTy("p.S"));
  EXPECT_FALSE(S->hasBody());
  EXPECT_TRUE(C.setStructBody(S, {C.i64(), C.ptr(S)}));
  EXPECT_FALSE(C.setStructBody(S, {}));
  EXPECT_EQ(S->fieldType(1, C), C.ptr(S));
  // The fields of string and slice are (ptr, len, cap).
  EXPECT_EQ(C.string()->fieldType(0, C), C.ptr(C.u8()));
  EXPECT_EQ(C.slice(C.i64())->fieldType(2, C), C.u64());
  EXPECT_EQ(C.string()->fieldType(3, C), nullptr);
}

TEST(GIRVerify, AcceptsTheSample) {
  Context C;
  auto M = sample(C);
  EXPECT_EQ(errorsOf(*M), "");
  std::string T = text(*M);
  EXPECT_NE(T.find("bb1 (cleanup):"), std::string::npos) << T;
  EXPECT_NE(T.find("unwind: cleanup bb1"), std::string::npos) << T;
  EXPECT_NE(T.find("drop(_4) -> [return: bb"), std::string::npos) << T;
  EXPECT_NE(T.find("resume"), std::string::npos) << T;
}

/// One broken rule: how to break the sample, and the rule the verifier must
/// name (tests/unit/GIR/VerifierTest.cpp has both sides of every rule).
struct Mutation {
  const char *Name;
  std::function<void(Module &, Context &)> Break;
  const char *Rule;
};

Function &sampleFn(Module &M) { return *M.Funcs[1]; }

Statement &firstAssignTo(Function &F, LocalId L) {
  for (BasicBlock &B : F.Blocks)
    for (Statement &S : B.Stmts)
      if (S.K == Statement::Assign && S.Dest.Local == L && S.Dest.Proj.empty())
        return S;
  ADD_FAILURE() << "no assignment";
  return F.Blocks[0].Stmts[0];
}

TEST(GIRVerify, ReportsEachBrokenRule) {
  const Mutation Table[] = {
      {"wrong assigned type",
       [](Module &M, Context &C) {
         Function &F = sampleFn(M);
         firstAssignTo(F, 6).RV.A = constInt(C.i32(), 2);
       },
       "V050"},
      {"missing terminator",
       [](Module &M, Context &) { sampleFn(M).Blocks[3].Term.Set = false; },
       "V020"},
      {"normal edge into a cleanup block",
       [](Module &M, Context &) {
         sampleFn(M).Blocks[7].Term.Cases.clear();
         sampleFn(M).Blocks[7].Term.Otherwise = 1;
       },
       "V023"},
      {"unwind to a normal block",
       [](Module &M, Context &) {
         sampleFn(M).Blocks[3].Term.Unwind.Target = 5;
       },
       "V024"},
      {"resume in a normal block",
       [](Module &M, Context &) {
         sampleFn(M).Blocks[5].Term = Terminator();
         sampleFn(M).Blocks[5].Term.K = Terminator::Resume;
         sampleFn(M).Blocks[5].Term.Set = true;
       },
       "V025"},
      {"cleanup block that continues unwinding",
       [](Module &M, Context &) {
         sampleFn(M).Blocks[1].Term.Unwind = {UnwindAction::Continue, NoId};
       },
       "V025"},
      {"cleanup block that returns",
       [](Module &M, Context &) {
         sampleFn(M).Blocks[2].Term = Terminator();
         sampleFn(M).Blocks[2].Term.K = Terminator::Return;
         sampleFn(M).Blocks[2].Term.Set = true;
       },
       "V025"},
      {"call with too few arguments",
       [](Module &M, Context &) { sampleFn(M).Blocks[3].Term.Args.clear(); },
       "V062"},
      {"call argument of the wrong type",
       [](Module &M, Context &C) {
         sampleFn(M).Blocks[3].Term.Args[0] = constInt(C.u8(), 1);
       },
       "V062"},
      {"call result into the wrong type",
       [](Module &M, Context &C) {
         Function &F = sampleFn(M);
         F.Locals[7].Ty = C.u8();
         F.Blocks[3].Term.Dest.Ty = C.u8();
       },
       "V063"},
      {"local out of range",
       [](Module &M, Context &) {
         sampleFn(M).Blocks[0].Stmts[1].Dest.Local = 99;
       },
       "V030"},
      {"deref of a non-pointer",
       [](Module &M, Context &C) {
         PlaceElem E;
         E.K = PlaceElem::Deref;
         E.Ty = C.i64();
         Place &P = sampleFn(M).Blocks[0].Stmts[1].Dest;
         P.Local = 7;
         P.Proj = {E};
         P.Ty = C.i64();
       },
       "V035"},
      {"field that does not exist",
       [](Module &M, Context &C) {
         Function &F = sampleFn(M);
         PlaceElem E;
         E.K = PlaceElem::Field;
         E.Val = 7;
         E.Ty = C.i64();
         Place &P = F.Blocks[0].Stmts[1].Dest;
         P.Local = 5;
         P.Proj = {E};
         P.Ty = C.i64();
       },
       "V032"},
      {"switch on a float",
       [](Module &M, Context &C) {
         sampleFn(M).Blocks[7].Term.Discr = constFloat(C.floatTy(64), 1.0);
       },
       "V060"},
      {"integer constant too wide",
       [](Module &M, Context &C) {
         Operand O = constInt(C.u8(), 1);
         O.C.Bits = 300;
         firstAssignTo(sampleFn(M), 6).RV.A = O;
       },
       "V040"},
      {"return place of the wrong type",
       [](Module &M, Context &C) { sampleFn(M).Locals[0].Ty = C.u8(); },
       "V011"},
      {"shift of a float",
       [](Module &M, Context &C) {
         Function &F = sampleFn(M);
         for (BasicBlock &B : F.Blocks)
           for (Statement &S : B.Stmts)
             if (S.K == Statement::Assign && S.RV.K == Rvalue::Binary &&
                 S.RV.Bin == BinOp::Shl)
               S.RV.A = constFloat(C.floatTy(64), 1.0);
       },
       "V052"},
  };
  for (const Mutation &Mu : Table) {
    Context C;
    auto M = sample(C);
    ASSERT_EQ(errorsOf(*M), "") << Mu.Name;
    Mu.Break(*M, C);
    bool Found = false;
    for (const VerifyError &E : verify(*M, 100))
      Found |= E.Rule == Mu.Rule;
    EXPECT_TRUE(Found) << Mu.Name << ": wanted " << Mu.Rule << ", got:\n"
                       << errorsOf(*M);
  }
}

TEST(GIRSerialize, RoundTripsAndIsDeterministic) {
  Context C1;
  auto M = sample(C1);
  std::string Bytes = writeModule(*M);
  EXPECT_EQ(Bytes, writeModule(*M));
  EXPECT_EQ(Bytes.size() % 4, 0u);
  EXPECT_EQ(Bytes.substr(0, 3), "GIR");

  // Read into another context: same text, same bytes again.
  Context C2;
  auto R = readModule(Bytes, C2);
  ASSERT_TRUE(static_cast<bool>(R)) << toString(R.takeError());
  EXPECT_EQ(text(**R), text(*M));
  EXPECT_EQ(writeModule(**R), Bytes);
  EXPECT_EQ(errorsOf(**R), "");
  // The structs came back with their bodies.
  EXPECT_TRUE(C2.structTy("sample.Pair")->hasBody());
}

TEST(GIRSerialize, CatchBlocksRoundTrip) {
  Context C1;
  auto M = std::make_unique<Module>(C1, "catchy");
  auto Ext = std::make_unique<Function>();
  Ext->Name = "catchy.ext";
  Ext->Sig = C1.func({}, C1.unit());
  FuncId E = M->addFunction(std::move(Ext));
  auto H = std::make_unique<Function>();
  H->Name = "catchy.h";
  H->Sig = C1.func({}, C1.unit());
  Function &Hn = *H;
  M->addFunction(std::move(H));
  FuncBuilder B(*M, Hn);
  BlockId Catch = B.newCatchBlock(), Plain = B.newBlock(/*Cleanup=*/true);
  B.call(constFunc(*M, E), {}, B.local(B.newTemp(C1.unit())),
         {UnwindAction::Cleanup, Catch});
  B.ret();
  B.setBlock(Catch);
  B.call(constFunc(*M, E), {}, B.local(B.newTemp(C1.unit())),
         {UnwindAction::Cleanup, Plain}); // stays a catch block
  B.ret();                                // recovered
  B.setBlock(Plain);
  B.resume();
  ASSERT_EQ(errorsOf(*M), "");
  EXPECT_TRUE(Hn.Blocks[Catch].Catch && Hn.Blocks[Catch].Cleanup);
  EXPECT_TRUE(Hn.Blocks[4].Catch); // the block after the call in it
  EXPECT_FALSE(Hn.Blocks[Plain].Catch);
  EXPECT_NE(text(*M).find("(catch)"), std::string::npos);

  std::string Bytes = writeModule(*M);
  Context C2;
  auto R = readModule(Bytes, C2);
  ASSERT_TRUE(static_cast<bool>(R)) << toString(R.takeError());
  EXPECT_EQ(text(**R), text(*M));
  EXPECT_EQ(writeModule(**R), Bytes);
  EXPECT_TRUE((*R)->Funcs[1]->Blocks[Catch].Catch);
  EXPECT_FALSE((*R)->Funcs[1]->Blocks[Plain].Catch);
}

// P04.05: the self-naming types (`type S []S`, `type O Option[*O]`, `type A
// [2]Option[*A]`) and the Owned flag survive the bitstream, and Context makes
// one type of every spelling of the same body.
TEST(GIRSerialize, SelfNamingTypesAndOwnedTemporariesRoundTrip) {
  Context C1;
  auto M = std::make_unique<Module>(C1, "rec");
  Type *S = C1.recTy("rec.S"), *O = C1.recTy("rec.O"), *A = C1.recTy("rec.A");
  ASSERT_TRUE(C1.setRecBody(S, C1.slice(S)));
  ASSERT_TRUE(C1.setRecBody(O, C1.ptr(O)));
  ASSERT_TRUE(C1.setRecBody(A, C1.array(C1.option(C1.ptr(A)), 2)));
  // The same body is the same type, however it is spelled.
  EXPECT_EQ(C1.slice(S), S);
  EXPECT_EQ(C1.ptr(O), O);
  EXPECT_EQ(C1.array(C1.option(C1.ptr(A)), 2), A);
  EXPECT_EQ(C1.canonical(C1.slice(S)), S);
  // Two names, one body: the second is refused.
  Type *Twin = C1.recTy("rec.Twin");
  EXPECT_FALSE(C1.setRecBody(Twin, C1.slice(S)));
  EXPECT_TRUE(S->isRec() && S->isNominal() && S->hasBody());
  EXPECT_EQ(S->str(), "rec.S");

  auto F = std::make_unique<Function>();
  F->Name = "rec.f";
  F->Sig = C1.func({S, A}, C1.unit());
  F->CC = CallConv::Go; // (a C function takes only C types, V067)
  Function &Fn = *F;
  M->addFunction(std::move(F));
  FuncBuilder B(*M, Fn);
  LocalId T = B.newTemp(S);
  Fn.Locals[T].Owned = true;
  B.assign(B.local(T), B.use(copyOf(B.local(B.param(0)))));
  B.ret();
  ASSERT_EQ(errorsOf(*M), "");
  EXPECT_NE(text(*M).find("[owned]"), std::string::npos) << text(*M);

  std::string Bytes = writeModule(*M);
  EXPECT_EQ(Bytes, writeModule(*M));
  Context C2;
  auto R = readModule(Bytes, C2);
  ASSERT_TRUE(static_cast<bool>(R)) << toString(R.takeError());
  EXPECT_EQ(text(**R), text(*M));
  EXPECT_EQ(writeModule(**R), Bytes);
  EXPECT_EQ(errorsOf(**R), "");
  EXPECT_TRUE((*R)->Funcs[0]->Locals[T].Owned);
  Type *S2 = (*R)->Funcs[0]->Sig->elems()[0];
  ASSERT_TRUE(S2->isRec());
  EXPECT_EQ(S2->name(), "rec.S");
  EXPECT_EQ(S2->getKind(), Type::Kind::Slice);
  EXPECT_EQ(S2->elem(), S2); // the cycle came back as a cycle
  Type *A2 = (*R)->Funcs[0]->Sig->elems()[1];
  EXPECT_EQ(A2->getKind(), Type::Kind::Array);
  EXPECT_EQ(A2->elem()->getKind(), Type::Kind::Option);
}

TEST(GIRSerialize, ReadsOneBodyThroughTheIndex) {
  Context C1;
  auto M = sample(C1);
  // A second function, so a body can be left out.
  auto G = std::make_unique<Function>();
  G->Name = "sample.g";
  G->Sig = C1.func({}, C1.unit());
  Function &Gn = *G;
  M->addFunction(std::move(G));
  FuncBuilder B(*M, Gn);
  B.ret();
  std::string Bytes = writeModule(*M);

  Context C2;
  ReadOptions Opts;
  Opts.OnlyFunction = "sample.g";
  auto R = readModule(Bytes, C2, Opts);
  ASSERT_TRUE(static_cast<bool>(R)) << toString(R.takeError());
  EXPECT_TRUE((*R)->Funcs[1]->isDeclaration()); // sample.f: not read
  EXPECT_FALSE((*R)->Funcs[2]->isDeclaration());
  EXPECT_EQ((*R)->Funcs.size(), 3u);
}

TEST(GIRSerialize, RefusesAnotherCompilerVersion) {
  Context C1, C2;
  auto M = sample(C1);
  ReadOptions Opts;
  Opts.Version = "9.9.9";
  auto R = readModule(writeModule(*M), C2, Opts);
  ASSERT_FALSE(static_cast<bool>(R));
  EXPECT_NE(toString(R.takeError()).find("rebuild"), std::string::npos);
}

TEST(GIRSerialize, DamagedInputIsAnErrorNeverACrash) {
  Context C1;
  auto M = sample(C1);
  std::string Bytes = writeModule(*M);
  // Every truncation.
  for (size_t N = 0; N < Bytes.size(); N += 7) {
    Context C;
    auto R = readModule(Bytes.substr(0, N), C);
    if (!R)
      llvm::consumeError(R.takeError());
  }
  // Every flipped byte: an error, or a module that still verifies.
  for (size_t I = 0; I < Bytes.size(); ++I) {
    std::string Bad = Bytes;
    Bad[I] = static_cast<char>(Bad[I] ^ 0x5a);
    Context C;
    auto R = readModule(Bad, C);
    if (!R) {
      llvm::consumeError(R.takeError());
    } else {
      EXPECT_EQ(errorsOf(**R), "") << "byte " << I;
      text(**R); // what `gopp gir dump` does with it
    }
  }
}

//===----------------------------------------------------------------------===//
// The builder from source
//===----------------------------------------------------------------------===//

/// Checks and lowers Source (one package file); null if it reports errors.
std::unique_ptr<Module> lower(Context &C, const std::string &Source,
                              std::string *Diag = nullptr,
                              unsigned Threads = 1) {
  llvm::SmallString<128> Dir;
  EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-gir", Dir));
  {
    std::ofstream Out(std::string(Dir) + "/main.gopp", std::ios::binary);
    Out << Source;
  }
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags(SM, Collect, 0);
  sema::LoadOptions LO;
  LO.Root = std::string(Dir);
  sema::TypeContext TCtx;
  sema::CheckOptions CO;
  CO.BodyThreads = Threads;
  sema::Loader L(SM, Diags, TCtx, LO, CO);
  sema::CheckedPackage *CP = L.loadDir(std::string(Dir), "");
  std::unique_ptr<Module> M;
  if (CP && !Diags.hasErrors())
    M = lowerPackage(C, SM, *CP, L.filesOf(CP), Diags);
  if (Diag)
    for (const Diagnostic &D : Collect.Diags)
      *Diag += getDiagName(D.ID).str() + ": " + D.Message + "\n";
  llvm::sys::fs::remove_directories(Dir);
  return M;
}

TEST(GIRLower, EveryBuiltModuleVerifies) {
  const char *Programs[] = {
      "package main\nfunc main() {}\n",
      "package main\nfunc main() { println(1, \"a\", true) }\n",
      ("package main\nfunc f(a, b int) int { return a*b - a/b + a%b }\n"
       "func main() { println(f(6, 3)) }\n"),
      "package main\nfunc main() {\n for i := 0; i < 3; i++ { println(i) "
      "}\n}\n",
      "package main\nfunc main() {\n var x uint8 = 200\n x += 100\n println(x "
      "<< 1)\n}\n",
      "package main\nfunc main() { x := 2.5; y := float32(x); println(int(y)) "
      "}\n",
      // P04.04: switch in every form, labels, goto.
      ("package main\nfunc f(n int) int {\n switch {\n case n < 0:\n return "
       "-1\n"
       " case n == 0:\n fallthrough\n default:\n return 1\n case n > 9:\n "
       "return 2\n }\n"
       " return 0\n}\nfunc g(n int) int {\nouter:\n for i := 0; i < n; i++ {\n"
       "  for j := 0; ; j++ {\n   if j > i {\n    continue outer\n   }\n   if "
       "i > 5 {\n"
       "    break outer\n   }\n  }\n }\n i := 0\nloop:\n if i < 3 {\n  i++\n  "
       "goto loop\n }\n return i\n}\n"
       "func main() { println(f(3), g(4)) }\n"),
      // Structs, arrays, pointers, comparison, globals and init.
      ("package main\ntype P struct{ X, Y int }\ntype L struct{ A, B P; N "
       "[3]int8 }\n"
       "var g = P{1, 2}\nvar arr [4]float32\nfunc init() { arr[1] = 2 }\n"
       "func main() {\n l := L{A: g}\n l.N[2] = 3\n p := &l.B\n p.Y = 5\n"
       " m := l\n println(l == m, l.B.Y, arr[1], len(l.N))\n}\n"),
      // Complex numbers, Option, shifts and float conversions.
      ("package main\nfunc main() {\n c := complex(1, 2)\n d := c * c / c\n"
       " println(real(d), imag(d), c == d)\n var o Option[int]\n o = 3\n"
       " if o != nil {\n  println(o + 1)\n }\n v, ok := o.Get()\n println(v, "
       "ok)\n"
       " var s uint = 70\n var m int8 = -1\n println(1<<s, int8(-128)/m)\n}\n"),
      // Multiple and named results.
      ("package main\nfunc f() (a int, b bool) {\n a = 3\n return\n}\n"
       "func g(x int) (int, int) { return x, x + 1 }\n"
       "func main() {\n a, b := f()\n c, d := g(a)\n println(a, b, c, d)\n}\n"),
  };
  for (const char *Src : Programs) {
    Context C;
    std::string Diag;
    auto M = lower(C, Src, &Diag);
    // println of a float is not in the M0 subset: the last program is
    // reported, not lowered into something wrong.
    if (!M) {
      EXPECT_NE(Diag.find("err_gir_unsupported"), std::string::npos) << Diag;
      continue;
    }
    EXPECT_EQ(errorsOf(*M), "") << Src;
    // The bitstream of what the builder makes round-trips too.
    Context C2;
    auto R = readModule(writeModule(*M), C2);
    ASSERT_TRUE(static_cast<bool>(R)) << toString(R.takeError());
    EXPECT_EQ(text(**R), text(*M));
  }
}

TEST(GIRLower, SymbolsDoNotDependOnTheThreadCount) {
  // Local types, init functions and many functions: the module is the same
  // text whatever the number of checker threads (the file bytes of the
  // bitstream hold the temporary directory's name, so the lit test
  // gir/symbols.test compares those, in one directory).
  std::string Src = "package main\n";
  for (int I = 0; I < 12; ++I)
    Src += "func f" + std::to_string(I) +
           "() bool {\n type L struct{ a int }\n"
           " x, y := L{" +
           std::to_string(I) + "}, L{1}\n return x == y\n}\n";
  Src += "func init() { println(1) }\nfunc init() { println(2) }\n"
         "func main() { println(f0(), f11()) }\n";
  std::string Want;
  for (unsigned Threads : {1u, 2u, 8u, 0u}) {
    Context C;
    std::string Diag;
    auto M = lower(C, Src, &Diag, Threads);
    ASSERT_NE(M, nullptr) << Diag;
    EXPECT_EQ(errorsOf(*M), "");
    std::string T = text(*M);
    if (Want.empty()) {
      Want = T;
      EXPECT_NE(T.find("fn _GPK4mainE1_"), std::string::npos) << T;
      EXPECT_NE(T.find("L#0"), std::string::npos) << T;
      EXPECT_NE(T.find("_GPTeg0_1LF4mainE3f11"), std::string::npos) << T;
    }
    EXPECT_EQ(T, Want) << Threads << " threads";
  }
}

TEST(GIRLower, ReportsWhatItCannotLower) {
  Context C;
  std::string Diag;
  EXPECT_EQ(lower(C, "package main\nfunc main() { defer println(1) }\n", &Diag),
            nullptr);
  EXPECT_NE(Diag.find("err_gir_unsupported"), std::string::npos) << Diag;
  Diag.clear();
  EXPECT_EQ(
      lower(C, "package main\nfunc main() { f := func() {}; f() }\n", &Diag),
      nullptr);
  EXPECT_NE(Diag.find("err_gir_unsupported"), std::string::npos) << Diag;
  // And the good side: a program in the subset is lowered, a map, a slice
  // and a range loop (P04.05) included.
  EXPECT_NE(lower(C, "package main\nfunc main() { x := 1; _ = x }\n"), nullptr);
  EXPECT_NE(lower(C,
                  "package main\nfunc main() {\n\tm := map[string]int{\"a\": "
                  "1}\n\tfor k, v := range m { println(k, v) }\n\tx := "
                  "[]int{1}\n\t_ = x\n}\n"),
            nullptr);
}

} // namespace
