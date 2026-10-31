//===- CodeGenTest.cpp - Targets, GIR to LLVM IR, link lines --------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.02, P04.03. What can be checked without running a program: the target
// table and its pinned data layouts, the LLVM IR and assembly of the same GIR
// for both targets, the link command line for a glibc system in the FHS
// layout (Debian) and for a Nix one, and the GIR the code generator refuses.
//
//===----------------------------------------------------------------------===//

#include "../Support/GIRTestSupport.h"

#include "gopp/CodeGen/CodeGen.h"
#include "gopp/GIR/Builder.h"
#include "gopp/GIR/Lower.h"
#include "gopp/Sema/Loader.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Target/TargetMachine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>

using namespace gopp;

namespace {

std::unique_ptr<gir::Module> lowerSource(gir::Context &C,
                                         const std::string &Source) {
  llvm::SmallString<128> Dir;
  EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-cg", Dir));
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
  sema::Loader L(SM, Diags, TCtx, LO, {});
  sema::CheckedPackage *CP = L.loadDir(std::string(Dir), "");
  std::unique_ptr<gir::Module> M;
  if (CP && !Diags.hasErrors())
    M = gir::lowerPackage(C, SM, *CP, L.filesOf(CP), Diags);
  llvm::sys::fs::remove_directories(Dir);
  return M;
}

const char *Hello =
    "package main\n"
    "@Extern(\"C\")\nfunc write(fd int32, p *byte, n uint) int\n"
    "func f(a int, b int) int { return a / b }\n"
    "func main() { println(\"hi\", f(7, 2)) }\n";

TEST(Targets, TheTableAndItsNames) {
  auto A = parseTarget("linux/amd64");
  ASSERT_TRUE(static_cast<bool>(A));
  EXPECT_EQ(A->Triple, "x86_64-unknown-linux-gnu");
  EXPECT_EQ(A->name(), "linux/amd64");
  EXPECT_EQ(A->Emulation, "elf_x86_64");
  auto R = parseTarget("linux/arm64");
  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R->Triple, "aarch64-unknown-linux-gnu");
  EXPECT_EQ(R->Emulation, "aarch64linux");
  EXPECT_EQ(R->DefaultDynamicLinker, "/lib/ld-linux-aarch64.so.1");
  // Anything else lists what is supported.
  auto Bad = parseTarget("windows/amd64");
  ASSERT_FALSE(static_cast<bool>(Bad));
  std::string Msg = toString(Bad.takeError());
  EXPECT_NE(Msg.find("linux/amd64, linux/arm64"), std::string::npos) << Msg;
  // The host is one of them on the machines that run the tests.
  auto H = parseTarget("host");
  if (H)
    EXPECT_TRUE(H->Arch == "amd64" || H->Arch == "arm64");
  else
    llvm::consumeError(H.takeError());
}

TEST(Targets, DataLayoutsArePinnedAndChecked) {
  for (const char *Name : {"linux/amd64", "linux/arm64"}) {
    auto T = parseTarget(Name);
    ASSERT_TRUE(static_cast<bool>(T));
    auto TM = createTargetMachine(*T);
    ASSERT_TRUE(static_cast<bool>(TM)) << toString(TM.takeError());
    llvm::DataLayout DL = (*TM)->createDataLayout();
    // What D4.5 relies on: little endian, 64-bit pointers, 64-bit alignment.
    EXPECT_TRUE(DL.isLittleEndian()) << Name;
    EXPECT_EQ(DL.getPointerSizeInBits(), 64u) << Name;
    llvm::LLVMContext LC;
    EXPECT_EQ(DL.getABITypeAlign(llvm::Type::getInt64Ty(LC)).value(), 8u);
    // A different pinned layout is refused with a message that says why.
    TargetSpec Wrong = *T;
    Wrong.DataLayout = "e-m:e-p:32:32";
    auto Err = createTargetMachine(Wrong);
    ASSERT_FALSE(static_cast<bool>(Err)) << Name;
    EXPECT_NE(toString(Err.takeError()).find("pinned"), std::string::npos);
  }
}

TEST(CodeGen, BothTargetsGetTheSameGIR) {
  for (const char *Name : {"linux/amd64", "linux/arm64"}) {
    gir::Context GC;
    auto GM = lowerSource(GC, Hello);
    ASSERT_NE(GM, nullptr);
    auto T = parseTarget(Name);
    auto TM = createTargetMachine(*T);
    ASSERT_TRUE(static_cast<bool>(TM));
    llvm::LLVMContext LC;
    auto LM = lowerToLLVM(LC, *GM, **TM);
    ASSERT_TRUE(static_cast<bool>(LM)) << toString(LM.takeError());
    EXPECT_EQ((*LM)->getTargetTriple().str(), T->Triple);
    EXPECT_EQ((*LM)->getDataLayout().getStringRepresentation(), T->DataLayout);
    std::string Asm;
    llvm::raw_string_ostream OS(Asm);
    llvm::Error E = emitAssembly(**LM, **TM, OS);
    ASSERT_FALSE(static_cast<bool>(E)) << toString(std::move(E));
    // The call to the C function and the entry point are in the assembly.
    EXPECT_NE(Asm.find("write"), std::string::npos) << Name;
    EXPECT_NE(Asm.find("_GPF4mainE4main"), std::string::npos) << Name;
    EXPECT_NE(Asm.find(std::string(Name) == "linux/amd64" ? "callq" : "bl\t"),
              std::string::npos)
        << Name << "\n"
        << Asm;
  }
}

const llvm::TargetMachine &targetMachine(const char *Name) {
  static std::map<std::string, std::unique_ptr<llvm::TargetMachine>> TMs;
  std::unique_ptr<llvm::TargetMachine> &TM = TMs[Name];
  if (!TM)
    TM = std::move(*createTargetMachine(*parseTarget(Name)));
  return *TM;
}

/// The LLVM IR of M for the target, or the error's text.
std::string irOf(const gir::Module &M, const char *Target = "linux/amd64") {
  llvm::LLVMContext LC;
  auto LM = lowerToLLVM(LC, M, targetMachine(Target));
  if (!LM)
    return "error: " + toString(LM.takeError());
  std::string S;
  llvm::raw_string_ostream OS(S);
  (*LM)->print(OS, nullptr);
  return S;
}

/// A function `f() -> ()` whose body B fills.
gir::Function &newFunction(gir::Module &M, const char *Name, gir::Type *Sig) {
  return M.func(test::declare(M, Name, Sig));
}

TEST(CodeGen, RefusesGIRItCannotLowerYet) {
  // A drop (P04.09), an unwind edge into cleanup and an unwind action that
  // terminates (both P04.26) are refused with the construct named: lowering
  // them as plain calls would change what a panic does.
  for (int Case = 0; Case < 3; ++Case) {
    gir::Context GC;
    gir::Module M(GC, "m");
    gir::FuncId Ext = test::declare(M, "ext", GC.func({}, GC.unit()));
    gir::Function &Fn = newFunction(M, "f", GC.func({}, GC.unit()));
    gir::FuncBuilder B(M, Fn);
    gir::LocalId S = B.newLocal(GC.string(), gir::Local::Var, "s");
    const char *Want;
    if (Case == 0) {
      B.drop(B.local(S));
      B.ret();
      Want = "a drop without drop glue in f";
    } else if (Case == 1) {
      gir::BlockId Pad = B.newBlock(/*Cleanup=*/true);
      B.call(gir::constFunc(M, Ext), {}, B.local(B.newTemp(GC.unit())),
             {gir::UnwindAction::Cleanup, Pad});
      B.ret();
      B.setBlock(Pad);
      B.resume();
      Want = "an unwind edge to a cleanup block (P04.26) in f";
    } else {
      B.call(gir::constFunc(M, Ext), {}, B.local(B.newTemp(GC.unit())),
             {gir::UnwindAction::Terminate, gir::NoId});
      B.ret();
      Want = "an unwind action that terminates (P04.26) in f";
    }
    ASSERT_EQ(test::errorsOf(M), "") << Case;
    std::string IR = irOf(M);
    EXPECT_NE(IR.find(Want), std::string::npos) << IR;
  }
}

// R4: a failed emit leaves the output as it was, and no temporary file.
TEST(CodeGen, FailedEmitKeepsTheOldOutputAndLeavesNoTemporary) {
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-emit", Dir));
  auto Entries = [&] {
    std::vector<std::string> Names;
    std::error_code EC;
    for (llvm::sys::fs::directory_iterator I(Dir, EC), E; !EC && I != E;
         I.increment(EC))
      Names.push_back(llvm::sys::path::filename(I->path()).str());
    std::sort(Names.begin(), Names.end());
    return Names;
  };
  auto Read = [](const std::string &P) {
    std::ifstream In(P, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(In), {});
  };
  std::string Out = (Dir + "/out.o").str();
  {
    std::ofstream(Out, std::ios::binary) << "OLD CONTENTS";
  }

  auto &TM = const_cast<llvm::TargetMachine &>(targetMachine("linux/amd64"));
  llvm::LLVMContext LC;
  {
    // An invalid module: a block without a terminator.
    llvm::Module Bad("bad", LC);
    Bad.setTargetTriple(TM.getTargetTriple());
    Bad.setDataLayout(TM.createDataLayout());
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(LC), false),
        llvm::GlobalValue::ExternalLinkage, "f", Bad);
    llvm::BasicBlock::Create(LC, "entry", Fn);

    llvm::Error E = emitObject(Bad, TM, Out);
    ASSERT_TRUE(static_cast<bool>(E));
    EXPECT_NE(toString(std::move(E)).find("invalid"), std::string::npos);
    EXPECT_EQ(Read(Out), "OLD CONTENTS");
    EXPECT_EQ(Entries(), std::vector<std::string>{"out.o"});

    // The same with no old file: none appears.
    std::string Fresh = (Dir + "/fresh.o").str();
    EXPECT_TRUE(static_cast<bool>(emitObject(Bad, TM, Fresh)));
    EXPECT_EQ(Entries(), std::vector<std::string>{"out.o"});

    // The assembly path verifies too.
    std::string Asm;
    llvm::raw_string_ostream AS(Asm);
    EXPECT_TRUE(static_cast<bool>(emitAssembly(Bad, TM, AS)));
    EXPECT_EQ(Asm, "");

    // A failing writer leaves the old file and no temporary.
    llvm::Error W = writeFileAtomic(Out, [](llvm::raw_pwrite_stream &OS) {
      OS << "half";
      return llvm::createStringError(std::errc::io_error, "boom");
    });
    EXPECT_TRUE(static_cast<bool>(W));
    llvm::consumeError(std::move(W));
    EXPECT_EQ(Read(Out), "OLD CONTENTS");
    EXPECT_EQ(Entries(), std::vector<std::string>{"out.o"});

    // Fix the module: now the old file is replaced.
    llvm::IRBuilder<> IB(&Fn->getEntryBlock());
    IB.CreateRetVoid();
    ASSERT_FALSE(static_cast<bool>(emitObject(Bad, TM, Out)));
    EXPECT_NE(Read(Out), "OLD CONTENTS");
    EXPECT_EQ(Read(Out).substr(1, 3), "ELF");
    EXPECT_EQ(Entries(), std::vector<std::string>{"out.o"});
  }
  llvm::sys::fs::remove_directories(Dir);
}

// R5: /dev/null stays a device, a symlink stays a link and its target is
// written, a regular file is replaced.
TEST(CodeGen, WriteFileAtomicHandlesDevicesAndSymlinks) {
  auto Put = [](llvm::raw_pwrite_stream &OS) {
    OS << "NEW";
    return llvm::Error::success();
  };
  EXPECT_FALSE(static_cast<bool>(writeFileAtomic("/dev/null", Put)));
  {
    bool Other = false;
    EXPECT_FALSE(llvm::sys::fs::is_other("/dev/null", Other));
    EXPECT_TRUE(Other);
  }

  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-lnk", Dir));
  std::string Real = (Dir + "/real").str(), Lnk = (Dir + "/lnk").str();
  {
    std::ofstream(Real) << "OLD";
  }
  ASSERT_FALSE(llvm::sys::fs::create_link("real", Lnk));
  EXPECT_FALSE(static_cast<bool>(writeFileAtomic(Lnk, Put)));
  llvm::sys::fs::file_status St;
  ASSERT_FALSE(llvm::sys::fs::status(Lnk, St, /*follow=*/false));
  EXPECT_EQ(St.type(), llvm::sys::fs::file_type::symlink_file);
  std::ifstream In(Real);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(In), {}), "NEW");
  llvm::sys::fs::remove_directories(Dir);
}

TEST(CodeGen, VerifiesTheGIRItIsGiven) {
  // A module built by hand is verified before it is lowered (the lowering
  // indexes it without checking): an error, never a crash.
  gir::Context GC;
  gir::Module M(GC, "m");
  gir::Function &Fn = newFunction(M, "f", GC.func({}, GC.unit()));
  gir::FuncBuilder B(M, Fn);
  B.gotoBlock(42);
  std::string IR = irOf(M);
  EXPECT_NE(IR.find("internal error: the GIR is invalid"), std::string::npos)
      << IR;
  EXPECT_NE(IR.find("V021: fn f: bb0"), std::string::npos) << IR;
}

TEST(CodeGen, EveryRvalueAndTerminatorLowers) {
  using namespace gir;
  Context C;
  Module M(C, "all");
  Type *I64 = C.i64(), *U64 = C.u64(), *I32 = C.i32(), *U8 = C.u8(),
       *Bool = C.boolean(), *F64 = C.floatTy(64), *F32 = C.floatTy(32);
  Type *Pair = C.structTy("all.Pair");
  C.setStructBody(Pair, {I64, Bool});
  Type *Arr = C.array(I64, 4);
  FuncId Callee = test::declare(M, "callee", C.func({I64, Bool}, I64));
  M.func(Callee).NoUnwind = true;
  Function &Fn =
      newFunction(M, "all.f", C.func({I64, U64, F64, C.ptr(U8)}, I64));
  FuncBuilder B(M, Fn);
  auto Arg = [&](unsigned I) { return copyOf(B.local(B.param(I))); };
  Place Sd = B.temp(B.binary(BinOp::Div, Arg(0), constInt(I64, 3)));
  Place Ud = B.temp(B.binary(BinOp::Div, Arg(1), constInt(U64, 3)));
  Place Sr = B.temp(B.binary(BinOp::Rem, Arg(0), constInt(I64, 3)));
  Place Sh = B.temp(B.binary(BinOp::Shr, Arg(0), constInt(U8, 3)));
  Place Uh = B.temp(B.binary(BinOp::Shr, Arg(1), constInt(U8, 70)));
  Place Shl = B.temp(B.binary(BinOp::Shl, Arg(1), Arg(1)));
  Place Fc = B.temp(B.binary(BinOp::Ne, Arg(2), constFloat(F64, 0)));
  Place Lt = B.temp(B.binary(BinOp::Lt, Arg(1), constInt(U64, 9)));
  Place An = B.temp(B.binary(BinOp::AndNot, Arg(0), constInt(I64, 1)));
  B.temp(B.unary(UnOp::Neg, Arg(2)));
  Place Nt = B.temp(B.unary(UnOp::Not, copyOf(Fc)));
  Place Cp = B.temp(B.unary(UnOp::Compl, Arg(0)));
  B.temp(B.cast(CastKind::IntToInt, Arg(0), I32));
  Place Fi = B.temp(B.cast(CastKind::FloatToInt, Arg(2), I64));
  B.temp(B.cast(CastKind::FloatToInt, Arg(2), U8));
  B.temp(B.cast(CastKind::IntToFloat, copyOf(Ud), F64));
  B.temp(B.cast(CastKind::FloatToFloat, Arg(2), F32));
  Place Pi = B.temp(B.cast(CastKind::PtrToInt, Arg(3), U64));
  B.temp(B.cast(CastKind::IntToPtr, copyOf(Pi), C.ptr(U8)));
  B.temp(B.cast(CastKind::PtrToPtr, Arg(3), C.ptr(I64)));
  LocalId Pr = B.newLocal(Pair, Local::Var, "pr");
  B.assign(B.local(Pr),
           B.aggregate(AggKind::Struct, Pair, {copyOf(Sd), copyOf(Lt)}));
  B.temp(B.aggregate(AggKind::Tuple, C.tuple({I64, Bool}),
                     {copyOf(Sr), copyOf(Nt)}));
  LocalId Ar = B.newLocal(Arr, Local::Var, "ar");
  B.assign(B.local(Ar),
           B.aggregate(AggKind::Array, Arr,
                       {copyOf(Sh), copyOf(Sd), copyOf(Sr), copyOf(An)}));
  B.temp(B.binary(BinOp::Add, copyOf(Uh), copyOf(Shl)));
  LocalId Ix = B.newLocal(U64, Local::Var, "ix");
  B.assign(B.local(Ix), B.use(constInt(U64, 2)));
  B.assign(B.index(B.local(Ar), Ix), B.use(copyOf(Cp)));
  B.assign(B.constIndex(B.local(Ar), 1), B.use(copyOf(Fi)));
  Place Ad = B.temp(B.addrOf(B.field(B.local(Pr), 0), true));
  B.assign(B.deref(Ad), B.use(copyOf(B.constIndex(B.local(Ar), 3))));
  Place S = B.temp(B.use(constStr(M, "str")));
  B.temp(B.len(S));
  B.temp(B.len(B.local(Ar)));
  BlockId Two = B.newBlock(), Other = B.newBlock(), Dead = B.newBlock();
  B.assertThat(copyOf(Lt), true, AssertKind::Overflow);
  B.call(constFunc(M, Callee),
         {copyOf(B.field(B.local(Pr), 0)), copyOf(B.field(B.local(Pr), 1))},
         B.local(0), {UnwindAction::Unreachable, NoId});
  B.switchInt(Arg(0), {{2, Two}}, Other);
  B.setBlock(Two);
  B.ret();
  B.setBlock(Other);
  B.gotoBlock(Dead);
  B.setBlock(Dead);
  B.unreachable();
  ASSERT_EQ(test::errorsOf(M), "");
  std::string IR = irOf(M);
  for (const char *Want : {"sdiv",
                           "udiv",
                           "srem",
                           "ashr",
                           "lshr",
                           "shl",
                           "fcmp une",
                           "icmp ult",
                           "fneg",
                           "@llvm.fptosi.sat.i64.f64",
                           "@llvm.fptoui.sat.i8.f64",
                           "uitofp",
                           "fptrunc",
                           "ptrtoint",
                           "inttoptr",
                           "getelementptr inbounds nuw %all.Pair",
                           "getelementptr inbounds",
                           "switch i64",
                           "@llvm.trap",
                           "unreachable",
                           "call swiftcc i64 @callee(i64"})
    EXPECT_NE(IR.find(Want), std::string::npos) << Want << "\n" << IR;
  // Both back ends generate code for it.
  for (const char *Target : {"linux/amd64", "linux/arm64"}) {
    llvm::LLVMContext LC;
    auto LM = lowerToLLVM(LC, M, targetMachine(Target));
    ASSERT_TRUE(static_cast<bool>(LM)) << toString(LM.takeError());
    std::string Asm;
    llvm::raw_string_ostream OS(Asm);
    llvm::Error E = emitAssembly(
        **LM, const_cast<llvm::TargetMachine &>(targetMachine(Target)), OS);
    ASSERT_FALSE(static_cast<bool>(E)) << toString(std::move(E));
    EXPECT_NE(Asm.find("all.f:"), std::string::npos) << Target;
  }
}

TEST(CodeGen, FunctionAttributes) {
  // A function that never returns is `noreturn`, one that cannot unwind is
  // `nounwind`, and every function Go++ defines has unwind tables (D4.6).
  // A bool crosses the C ABI zero-extended; a Go++ function is swiftcc
  // (P04.18, ADR-0030).
  using namespace gir;
  Context C;
  Module M(C, "m");
  FuncId Die = test::declare(
      M, "die", C.func({C.boolean()}, C.unit(), false, CallConv::C));
  M.func(Die).CC = CallConv::C;
  M.func(Die).NoReturn = M.func(Die).NoUnwind = true;
  Function &Fn = newFunction(M, "m.f", C.func({}, C.unit()));
  FuncBuilder B(M, Fn);
  B.call(constFunc(M, Die), {constBool(C, true)}, B.local(B.newTemp(C.unit())),
         {UnwindAction::Unreachable, NoId});
  B.ret();
  Fn.Blocks[0].Term.Target = NoId; // never returns: no return block
  Fn.Blocks.pop_back();
  ASSERT_EQ(test::errorsOf(M), "");
  std::string IR = irOf(M);
  EXPECT_NE(IR.find("declare void @die(i1 zeroext)"), std::string::npos) << IR;
  EXPECT_NE(IR.find("noreturn nounwind"), std::string::npos) << IR;
  EXPECT_NE(IR.find("define swiftcc void @m.f()"), std::string::npos) << IR;
  EXPECT_NE(IR.find("uwtable"), std::string::npos) << IR;
  EXPECT_NE(IR.find("call void @die(i1 zeroext true)"), std::string::npos)
      << IR;
}

/// A scratch directory, removed at the end of the test.
struct TempDir {
  llvm::SmallString<128> Path;
  TempDir() {
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-link-test", Path));
  }
  ~TempDir() { llvm::sys::fs::remove_directories(Path); }
  std::string file(llvm::StringRef Name) const {
    llvm::SmallString<128> P(Path);
    llvm::sys::path::append(P, Name);
    return std::string(P);
  }
  void touch(llvm::StringRef Name) const {
    std::ofstream(file(Name), std::ios::binary) << "";
  }
};

TEST(Linker, LinkLineForAnFhsSystemLikeDebian) {
  auto T = parseTarget("linux/amd64");
  LinkInputs In;
  In.Crt1 = "/usr/lib/x86_64-linux-gnu/crt1.o";
  In.Crti = "/usr/lib/x86_64-linux-gnu/crti.o";
  In.Crtn = "/usr/lib/x86_64-linux-gnu/crtn.o";
  In.LibDirs = {"/usr/lib/x86_64-linux-gnu"};
  In.DynamicLinker = "/lib64/ld-linux-x86-64.so.2";
  std::vector<std::string> Args =
      lldArguments(*T, In, {"/tmp/a.o", "/tmp/b.o"}, "out");
  // crt1 crti objects -L -lc crtn, in that order; no build id (the same
  // input gives the same bytes); the unwinder's frame index.
  std::vector<std::string> Want = {
      "-o",
      "out",
      "-m",
      "elf_x86_64",
      "--eh-frame-hdr",
      "--hash-style=gnu",
      "--build-id=none",
      "-z",
      "relro",
      "--dynamic-linker=/lib64/ld-linux-x86-64.so.2",
      "/usr/lib/x86_64-linux-gnu/crt1.o",
      "/usr/lib/x86_64-linux-gnu/crti.o",
      "/tmp/a.o",
      "/tmp/b.o",
      "-L/usr/lib/x86_64-linux-gnu",
      "-lc",
      "/usr/lib/x86_64-linux-gnu/crtn.o"};
  EXPECT_EQ(Args, Want);
  // arm64: its emulation and its loader, the same shape.
  LinkInputs Arm = In;
  Arm.DynamicLinker = parseTarget("linux/arm64")->DefaultDynamicLinker;
  std::vector<std::string> A =
      lldArguments(*parseTarget("linux/arm64"), Arm, {"/tmp/a.o"}, "out");
  EXPECT_EQ(A[3], "aarch64linux");
  EXPECT_EQ(A[9], "--dynamic-linker=/lib/ld-linux-aarch64.so.1");
}

TEST(Linker, LinkLineIsAFunctionOfItsInput) {
  auto T = parseTarget("linux/amd64");
  LinkInputs In;
  In.Crt1 = "/l/crt1.o";
  In.Crti = "/l/crti.o";
  In.Crtn = "/l/crtn.o";
  In.LibDirs = {"/l"};
  In.DynamicLinker = "/lib64/ld.so";
  EXPECT_EQ(lldArguments(*T, In, {"/tmp/a.o"}, "out"),
            lldArguments(*T, In, {"/tmp/a.o"}, "out"));
  EXPECT_NE(lldArguments(*T, In, {"/tmp/a.o"}, "out"),
            lldArguments(*T, In, {"/tmp/a.o"}, "other"));
}

TEST(Linker, MissingPiecesGiveAClearError) {
  auto T = parseTarget("linux/arm64");
  TempDir D;
  auto Error = [&](const char *What) {
    auto In = findLinkInputs(*T, D.Path, "");
    if (In) {
      ADD_FAILURE() << What << ": no error";
      return std::string();
    }
    return toString(In.takeError());
  };
  // An empty directory: the first missing piece and where it was looked for.
  EXPECT_NE(Error("empty").find("crt1.o does not exist: " + D.file("crt1.o")),
            std::string::npos);
  D.touch("crt1.o");
  EXPECT_NE(Error("crt1.o only").find("crti.o does not exist"),
            std::string::npos);
  D.touch("crti.o");
  D.touch("crtn.o");
  std::string NoLibc = Error("no libc");
  EXPECT_NE(NoLibc.find("libc.so (or libc.a) is not in " + std::string(D.Path)),
            std::string::npos)
      << NoLibc;
  // Good: a static libc.a or a libc.so will do; the loader is the target's.
  D.touch("libc.a");
  auto In = findLinkInputs(*T, D.Path, "");
  ASSERT_TRUE(static_cast<bool>(In)) << toString(In.takeError());
  EXPECT_EQ(In->DynamicLinker, "/lib/ld-linux-aarch64.so.1");
  EXPECT_EQ(In->Crt1, D.file("crt1.o"));
  auto Other = findLinkInputs(*T, D.Path, "/opt/ld.so");
  ASSERT_TRUE(static_cast<bool>(Other));
  EXPECT_EQ(Other->DynamicLinker, "/opt/ld.so");
}

TEST(Linker, ArmNeedsACrtDirOnAnotherHost) {
  auto T = parseTarget("linux/arm64");
  auto H = hostTarget();
  if (!H || H->Arch == "arm64") {
    if (!H)
      llvm::consumeError(H.takeError());
    GTEST_SKIP() << "the host is the target";
  }
  auto In = findLinkInputs(*T, "", "");
  ASSERT_FALSE(static_cast<bool>(In));
  std::string Msg = toString(In.takeError());
  EXPECT_NE(Msg.find("-crt-dir"), std::string::npos) << Msg;
  // A directory that lacks the files is named in the message.
  auto Missing = findLinkInputs(*T, "/nonexistent-gopp-dir", "");
  ASSERT_FALSE(static_cast<bool>(Missing));
  EXPECT_NE(toString(Missing.takeError()).find("/nonexistent-gopp-dir/crt1.o"),
            std::string::npos);
}

// P04.04 (ADR-0028 rule 3.3): every check of one kind in a function jumps to
// ONE panic block of that kind, which calls the runtime once; a second kind
// gets a second block, and so does an index of the other signedness (its
// message prints the number as it is); another function has its own. Both
// sides: the blocks exist, each makes one call, and none is duplicated.
TEST(CodeGen, OnePanicBlockPerKindPerFunction) {
  gir::Context GC;
  auto GM = lowerSource(
      GC, "package main\n"
          "var a [4]int\nvar z, i, j, k = 3, 1, 2, 3\nvar u uint = 1\n"
          "func two(x, y int) int {\n"
          "\treturn x/y + x%y + x/z + a[i] + a[j] + a[k]\n}\n"
          "func signs(x int) int { return a[i] + a[u] + a[x] + a[u+1] }\n"
          "func div(x, y int) int { return x / y }\n"
          "func plain(x, y int) int { return x + y }\n"
          "func main() {\n"
          "\tprintln(two(1, 2), signs(0), div(4, 2), plain(1, 2))\n}\n");
  ASSERT_NE(GM, nullptr);
  auto T = parseTarget("linux/amd64");
  auto TM = createTargetMachine(*T);
  ASSERT_TRUE(static_cast<bool>(TM));
  llvm::LLVMContext LC;
  auto LM = lowerToLLVM(LC, *GM, **TM);
  ASSERT_TRUE(static_cast<bool>(LM)) << toString(LM.takeError());
  // The panic blocks of the function whose symbol contains Fn: the runtime
  // function each calls, and how many calls it makes.
  auto panics = [&](const char *Fn) {
    std::map<std::string, unsigned> Callees;
    llvm::Function *F = nullptr;
    for (llvm::Function &G : **LM)
      if (G.getName().contains(Fn))
        F = &G;
    EXPECT_NE(F, nullptr) << Fn;
    if (!F)
      return Callees;
    for (llvm::BasicBlock &BB : *F) {
      if (!BB.getName().starts_with("panic"))
        continue;
      unsigned Calls = 0;
      for (llvm::Instruction &In : BB)
        if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&In)) {
          ++Calls;
          ++Callees[CI->getCalledFunction()->getName().str()];
        }
      EXPECT_EQ(Calls, 1u) << Fn << " " << BB.getName().str();
    }
    return Callees;
  };
  // two: the divide kind (three checks: /, %, /) has one block; the index
  // kind (three signed indexes) one more.
  auto Two = panics("F4mainE3two");
  EXPECT_EQ(Two.size(), 2u);
  for (auto &KV : Two)
    EXPECT_EQ(KV.second, 1u) << KV.first;
  // signs: signed and unsigned indexes print differently: two blocks, each
  // reached by two checks.
  auto Signs = panics("F4mainE5signs");
  EXPECT_EQ(Signs.size(), 2u);
  for (auto &KV : Signs)
    EXPECT_EQ(KV.second, 1u) << KV.first;
  // div: one kind, one block. plain: none.
  auto Div = panics("F4mainE3div");
  EXPECT_EQ(Div.size(), 1u);
  EXPECT_TRUE(panics("F4mainE5plain").empty());
}

} // namespace
