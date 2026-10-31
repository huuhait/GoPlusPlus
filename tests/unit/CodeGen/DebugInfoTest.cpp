//===- DebugInfoTest.cpp - DWARF metadata of the code generator -----------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.12, docs/design/debug-info.md. The metadata the code generator makes,
// read back from the LLVM module, on both targets: readable function and
// variable names (an operator method's too), the linkage name only on
// request, the Go++ spelling and layout of every kind of type (self-referring
// ones included, closed by declarations), lexical blocks and inlined code in
// the callee's scope, the variables kept in memory at `opt 0`, and the limits
// that keep the work linear and LLVM's own recursion bounded (types at most
// MaxTypeHeight high, blocks at most MaxBlockDepth deep), with controls. The
// debugger sessions on real binaries are tests/debug/*.test.
//
//===----------------------------------------------------------------------===//

#include "../Support/GIRTestSupport.h"

#include "gopp/Basic/NewStack.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/CodeGen/DebugInfo.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/GIR/Inline.h"
#include "gopp/GIR/Lower.h"
#include "gopp/Sema/Loader.h"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <tuple>

using namespace gopp;

namespace {

/// A source package lowered to GIR (optionally inlined) and then to LLVM.
struct Built {
  gir::Context GC;
  std::unique_ptr<gir::Module> GM;
  llvm::LLVMContext LC;
  std::unique_ptr<llvm::Module> LM;
  LowerWorkCounts Counts;
  std::string Error;

  explicit Built(const std::string &Source, bool Inline = false,
                 LowerToLLVMOptions Opts = {}) {
    llvm::SmallString<128> Dir;
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-di", Dir));
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
    if (CP && !Diags.hasErrors())
      GM = gir::lowerPackage(GC, SM, *CP, L.filesOf(CP), Diags);
    llvm::sys::fs::remove_directories(Dir);
    if (!GM) {
      Error = "the source does not lower";
      return;
    }
    if (Inline)
      gir::inlineTrivial(*GM);
    auto TM = createTargetMachine(*parseTarget("linux/amd64"));
    EXPECT_TRUE(static_cast<bool>(TM));
    Opts.Counts = &Counts;
    auto R = lowerToLLVM(LC, *GM, **TM, Opts);
    if (!R) {
      Error = llvm::toString(R.takeError());
      return;
    }
    LM = std::move(*R);
  }

  llvm::Function *fn(llvm::StringRef Name) {
    for (llvm::Function &F : *LM)
      if (F.getSubprogram() && F.getSubprogram()->getName() == Name)
        return &F;
    return nullptr;
  }
};

/// The declared variables of F by name.
std::map<std::string, llvm::DILocalVariable *> variablesOf(llvm::Function &F) {
  std::map<std::string, llvm::DILocalVariable *> Out;
  for (llvm::Instruction &I : llvm::instructions(F))
    for (llvm::DbgRecord &R : I.getDbgRecordRange())
      if (auto *V = llvm::dyn_cast<llvm::DbgVariableRecord>(&R))
        Out[V->getVariable()->getName().str()] = V->getVariable();
  return Out;
}

const llvm::DIDerivedType *member(const llvm::DICompositeType *T,
                                  llvm::StringRef Name) {
  for (const llvm::Metadata *E : T->getElements()->operands())
    if (auto *M = llvm::dyn_cast<llvm::DIDerivedType>(E))
      if (M->getName() == Name)
        return M;
  return nullptr;
}

/// Is T the declaration of the type called Name (no members: a debugger
/// finds the definition by the name)?
bool isDeclarationOf(const llvm::DIType *T, llvm::StringRef Name) {
  auto *C = llvm::dyn_cast_or_null<llvm::DICompositeType>(T);
  return C && C->isForwardDecl() && C->getName() == Name;
}

const char *Program = R"(package main

type Doc struct {
	Name  string
	Pages int
	Tags  []string
	Next  Option[*Doc]
	Opt   Option[int]
	Pair  [2]float64
}

var counter int = 7

func add(x, y int) int {
	z := x + y
	return z
}

func main() {
	d := Doc{Name: "hello", Pages: 3, Tags: []string{"a"}, Next: nil, Opt: nil}
	m := map[string]int{"x": 1}
	arr := [3]int{1, 2, 3}
	ok := true
	n := add(counter, d.Pages)
	println(n, d.Name, len(d.Tags), len(m), arr[1], ok)
}
)";

TEST(DebugInfo, NamesAreReadableAndTheLinkageNameIsOptIn) {
  Built B(Program);
  ASSERT_TRUE(B.LM) << B.Error;
  llvm::Function *Add = B.fn("main.add");
  ASSERT_NE(Add, nullptr);
  EXPECT_EQ(Add->getName(), "_GPF4mainE3add");
  EXPECT_EQ(Add->getSubprogram()->getLinkageName(),
            ""); // gdb/lldb would show it
  ASSERT_NE(B.fn("main.main"), nullptr);
  ASSERT_NE(B.fn("gopp.print_i64"), nullptr);
  // The package variable has its readable name and a type.
  bool Found = false;
  for (llvm::GlobalVariable &G : B.LM->globals()) {
    llvm::SmallVector<llvm::DIGlobalVariableExpression *, 1> GVs;
    G.getDebugInfo(GVs);
    for (auto *E : GVs)
      if (E->getVariable()->getName() == "main.counter") {
        Found = true;
        EXPECT_EQ(E->getVariable()->getType()->getName(), "int64");
      }
  }
  EXPECT_TRUE(Found);

  LowerToLLVMOptions O;
  O.DwarfLinkageNames = true;
  Built L(Program, false, O);
  ASSERT_TRUE(L.LM) << L.Error;
  EXPECT_EQ(L.fn("main.add")->getSubprogram()->getLinkageName(),
            "_GPF4mainE3add");
  // A name with no _GP form (the C entry point) has none to add.
  EXPECT_EQ(L.fn("main")->getSubprogram()->getLinkageName(), "");
}

TEST(DebugInfo, NoDebugInfoMakesNoMetadata) {
  LowerToLLVMOptions O;
  O.DebugInfo = false;
  Built B(Program, false, O);
  ASSERT_TRUE(B.LM) << B.Error;
  EXPECT_EQ(B.LM->getNamedMetadata("llvm.dbg.cu"), nullptr);
  for (llvm::Function &F : *B.LM)
    EXPECT_EQ(F.getSubprogram(), nullptr);
}

TEST(DebugInfo, ParametersAndVariablesButNoTemporaries) {
  Built B(Program);
  ASSERT_TRUE(B.LM) << B.Error;
  auto Add = variablesOf(*B.fn("main.add"));
  ASSERT_EQ(Add.size(), 3u);
  EXPECT_EQ(Add["x"]->getArg(), 1u);
  EXPECT_EQ(Add["y"]->getArg(), 2u);
  EXPECT_EQ(Add["z"]->getArg(), 0u);
  EXPECT_EQ(Add["x"]->getType()->getName(), "int64");
  // The parameters sit on the function's line, a variable on its own.
  EXPECT_EQ(Add["x"]->getLine(), 14u);
  EXPECT_EQ(Add["z"]->getLine(), 15u);
  auto Main = variablesOf(*B.fn("main.main"));
  for (const char *N : {"d", "m", "arr", "ok", "n"})
    EXPECT_EQ(Main.count(N), 1u) << N;
  EXPECT_EQ(Main.size(), 5u); // no `_3`, no `tmp`
  // The compiler's helpers have no source: their locals are no variables.
  EXPECT_TRUE(variablesOf(*B.fn("gopp.print_i64")).empty());
}

TEST(DebugInfo, TypesHaveGoSpellingAndTheRealLayout) {
  Built B(Program);
  ASSERT_TRUE(B.LM) << B.Error;
  auto Main = variablesOf(*B.fn("main.main"));
  auto *Doc = llvm::dyn_cast<llvm::DICompositeType>(Main["d"]->getType());
  ASSERT_NE(Doc, nullptr);
  EXPECT_EQ(Doc->getName(), "main.Doc");
  EXPECT_EQ(Doc->getSizeInBits(), 104u * 8);
  struct Want {
    const char *Name, *Type;
    unsigned Offset, Size;
  } Fields[] = {
      {"Name", "string", 0, 24},        {"Pages", "int64", 24, 8},
      {"Tags", "[]string", 32, 32},     {"Next", "*main.Doc", 64, 8},
      {"Opt", "Option[int64]", 72, 16}, {"Pair", "[2]float64", 88, 16}};
  for (const Want &W : Fields) {
    const llvm::DIDerivedType *M = member(Doc, W.Name);
    ASSERT_NE(M, nullptr) << W.Name;
    EXPECT_EQ(M->getBaseType()->getName(), W.Type) << W.Name;
    EXPECT_EQ(M->getOffsetInBits(), W.Offset * 8u) << W.Name;
    EXPECT_EQ(M->getSizeInBits(), W.Size * 8u) << W.Name;
  }
  // The pointer to Doc in the field is made while Doc is: it points to a
  // declaration of main.Doc, which a debugger finds by its name, so the
  // types the definitions refer to form no cycle.
  auto *Next =
      llvm::cast<llvm::DIDerivedType>(member(Doc, "Next")->getBaseType());
  EXPECT_EQ(Next->getTag(), llvm::dwarf::DW_TAG_pointer_type);
  EXPECT_TRUE(isDeclarationOf(Next->getBaseType(), "main.Doc"));
  // A string is {ptr, len, cap}, a slice {ptr, len, cap, alloc} (ADR-0029):
  // the GIR type's own fields, at the data layout's offsets.
  auto *Str =
      llvm::cast<llvm::DICompositeType>(member(Doc, "Name")->getBaseType());
  for (const char *N : {"ptr", "len", "cap"})
    EXPECT_NE(member(Str, N), nullptr) << N;
  EXPECT_EQ(member(Str, "alloc"), nullptr);
  EXPECT_EQ(member(Str, "ptr")->getBaseType()->getName(), "*uint8");
  auto *Tags =
      llvm::cast<llvm::DICompositeType>(member(Doc, "Tags")->getBaseType());
  EXPECT_EQ(Tags->getSizeInBits(), 32u * 8);
  unsigned Off = 0;
  for (const char *N : {"ptr", "len", "cap", "alloc"}) {
    ASSERT_NE(member(Tags, N), nullptr) << N;
    EXPECT_EQ(member(Tags, N)->getOffsetInBits(), Off) << N;
    Off += 64;
  }
  EXPECT_EQ(member(Tags, "ptr")->getBaseType()->getName(), "*string");
  EXPECT_EQ(member(Tags, "alloc")->getBaseType()->getName(), "*uint8");
  auto *Opt =
      llvm::cast<llvm::DICompositeType>(member(Doc, "Opt")->getBaseType());
  EXPECT_EQ(member(Opt, "value")->getBaseType()->getName(), "int64");
  EXPECT_EQ(member(Opt, "set")->getBaseType()->getName(), "bool");
  EXPECT_EQ(member(Opt, "set")->getOffsetInBits(), 8u * 8);
  // Scalars, arrays and the map (opaque: a pointer to the runtime object).
  EXPECT_EQ(Main["ok"]->getType()->getName(), "bool");
  auto *Arr = llvm::cast<llvm::DICompositeType>(Main["arr"]->getType());
  EXPECT_EQ(Arr->getTag(), llvm::dwarf::DW_TAG_array_type);
  EXPECT_EQ(Arr->getName(), "[3]int64");
  EXPECT_EQ(Arr->getSizeInBits(), 24u * 8);
  EXPECT_EQ(Main["m"]->getType()->getName(), "*gopp.map");
}

TEST(DebugInfo, SelfNamingAndMutuallyRecursiveTypes) {
  Built B(R"(package main

type S []S
type Node struct {
	next Option[*Node]
	kids []Node
	val  int
}
type A [2]Option[*A]

func main() {
	var s S
	var n Node
	var a A
	println(len(s), n.val, len(a))
}
)");
  ASSERT_TRUE(B.LM) << B.Error;
  auto V = variablesOf(*B.fn("main.main"));
  // `type S []S`: the slice struct named S, whose pointer is a pointer to S
  // (its declaration: S is being made).
  auto *S = llvm::cast<llvm::DICompositeType>(V["s"]->getType());
  EXPECT_EQ(S->getName(), "main.S");
  EXPECT_EQ(S->getSizeInBits(), 32u * 8);
  auto *SP = llvm::cast<llvm::DIDerivedType>(member(S, "ptr")->getBaseType());
  EXPECT_TRUE(isDeclarationOf(SP->getBaseType(), "main.S"));
  auto *N = llvm::cast<llvm::DICompositeType>(V["n"]->getType());
  EXPECT_EQ(N->getName(), "main.Node");
  auto *Kids =
      llvm::cast<llvm::DICompositeType>(member(N, "kids")->getBaseType());
  EXPECT_EQ(Kids->getName(), "[]main.Node");
  EXPECT_TRUE(isDeclarationOf(
      llvm::cast<llvm::DIDerivedType>(member(Kids, "ptr")->getBaseType())
          ->getBaseType(),
      "main.Node"));
  // `type A [2]Option[*A]`: a cycle of an array and a pointer, which C
  // debuggers cannot follow: a struct named A holds the array in `value`.
  auto *A = llvm::cast<llvm::DICompositeType>(V["a"]->getType());
  EXPECT_EQ(A->getName(), "main.A");
  EXPECT_EQ(A->getTag(), llvm::dwarf::DW_TAG_structure_type);
  EXPECT_EQ(A->getSizeInBits(), 16u * 8);
  auto *Arr =
      llvm::cast<llvm::DICompositeType>(member(A, "value")->getBaseType());
  EXPECT_EQ(Arr->getTag(), llvm::dwarf::DW_TAG_array_type);
  EXPECT_EQ(Arr->getName(), "[2]*main.A");
  EXPECT_TRUE(isDeclarationOf(
      llvm::cast<llvm::DIDerivedType>(Arr->getBaseType())->getBaseType(),
      "main.A"));
}

TEST(DebugInfo, InlinedCodeIsInTheCalleesScopeInlinedAtTheCall) {
  Built B(Program, /*Inline=*/true);
  ASSERT_TRUE(B.LM) << B.Error;
  llvm::Function *Main = B.fn("main.main");
  llvm::DISubprogram *AddSP = B.fn("main.add")->getSubprogram();
  bool InAddAtMain = false, OwnLine = false;
  for (llvm::Instruction &I : llvm::instructions(Main)) {
    const llvm::DILocation *L = I.getDebugLoc().get();
    if (!L)
      continue;
    if (L->getScope()->getSubprogram() == AddSP && L->getInlinedAt()) {
      InAddAtMain = true;
      // The callee's own line, and the call's line in main.
      if (L->getLine() == 15 || L->getLine() == 16) {
        EXPECT_EQ(L->getInlinedAt()->getScope()->getSubprogram(),
                  Main->getSubprogram());
        EXPECT_EQ(L->getInlinedAt()->getLine(), 24u);
      }
    }
    OwnLine |= L->getScope()->getSubprogram() == Main->getSubprogram() &&
               !L->getInlinedAt() && L->getLine() == 24;
  }
  EXPECT_TRUE(InAddAtMain);
  EXPECT_TRUE(OwnLine); // the call's line stays main's own
  // The callee's variables are declared in main in the callee's scope.
  auto V = variablesOf(*Main);
  ASSERT_EQ(V.count("z"), 1u);
  EXPECT_EQ(V["z"]->getScope()->getSubprogram(), AddSP);
  ASSERT_EQ(V.count("x"), 1u);
  EXPECT_EQ(V["x"]->getArg(), 1u);
  // The variable is the callee's own (one for every inlined copy).
  EXPECT_EQ(V["z"], variablesOf(*B.fn("main.add"))["z"]);
  // Not inlined: no such location.
  Built P(Program, /*Inline=*/false);
  ASSERT_TRUE(P.LM);
  for (llvm::Instruction &I : llvm::instructions(P.fn("main.main")))
    if (const llvm::DILocation *L = I.getDebugLoc().get())
      EXPECT_EQ(L->getInlinedAt(), nullptr);
}

/// The names of the slots (allocas) that a volatile load, store, memset or
/// memcpy into them of F reaches, and those such an access reaches that is
/// not volatile. (A memcpy out of a slot only reads it: not counted.)
std::pair<std::set<std::string>, std::set<std::string>>
volatileSlots(llvm::Function &F) {
  std::set<std::string> Vol, Plain;
  auto Note = [&](llvm::Value *P, bool V) {
    if (auto *A = llvm::dyn_cast<llvm::AllocaInst>(P->stripInBoundsOffsets()))
      (V ? Vol : Plain).insert(A->getName().str());
  };
  for (llvm::Instruction &I : llvm::instructions(F)) {
    if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I))
      Note(L->getPointerOperand(), L->isVolatile());
    else if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I))
      Note(S->getPointerOperand(), S->isVolatile());
    else if (auto *M = llvm::dyn_cast<llvm::MemIntrinsic>(&I))
      Note(M->getRawDest(), M->isVolatile());
  }
  return {Vol, Plain};
}

TEST(DebugInfo, KeepVariablesKeepsTheVariablesInMemoryAndNothingElse) {
  // M2 of the team build: at `opt 0` every access to a variable's slot is
  // volatile, so SROA and instcombine leave it as written; the temporaries
  // stay theirs.
  LowerToLLVMOptions O;
  O.KeepVariables = true;
  Built On(Program, /*Inline=*/true, O);
  ASSERT_TRUE(On.LM) << On.Error;
  auto [Vol, Plain] = volatileSlots(*On.fn("main.main"));
  // The variables of main and the inlined copies of add's x, y, z (small:
  // kept).
  for (const char *N : {"d", "m", "arr", "ok", "n", "x", "y", "z"})
    EXPECT_EQ(Vol.count(N), 1u) << N;
  for (const std::string &N : Vol)
    EXPECT_FALSE(N.empty()) << "a temporary is volatile";
  for (const std::string &N : Plain)
    EXPECT_EQ(Vol.count(N), 0u) << N << " is volatile only in part";
  // Without the option, or without debug information, nothing is volatile.
  Built Off(Program, /*Inline=*/true);
  ASSERT_TRUE(Off.LM) << Off.Error;
  EXPECT_TRUE(volatileSlots(*Off.fn("main.main")).first.empty());
  LowerToLLVMOptions N;
  N.KeepVariables = true;
  N.DebugInfo = false;
  Built None(Program, true, N);
  ASSERT_TRUE(None.LM);
  for (llvm::Function &F : *None.LM)
    EXPECT_TRUE(volatileSlots(F).first.empty()) << F.getName().str();
}

TEST(DebugInfo, ABigVariableOfAnInlinedCalleeIsNotKept) {
  // The other side of the 4 KB limit: a copy of a callee's 8 KB parameter
  // stays the optimizer's (run/core-argslots.test: 40 inlined calls with a
  // 512 KB parameter); the callee's own frame keeps it.
  LowerToLLVMOptions O;
  O.KeepVariables = true;
  Built B(R"(package main

func first(a [1024]int64) int64 {
	return a[0]
}

func main() {
	var big [1024]int64
	small := [4]int64{1, 2, 3, 4}
	println(first(big), small[1])
}
)",
          /*Inline=*/true, O);
  ASSERT_TRUE(B.LM) << B.Error;
  auto [Vol, Plain] = volatileSlots(*B.fn("main.main"));
  EXPECT_EQ(Vol.count("big"), 1u);
  EXPECT_EQ(Vol.count("small"), 1u);
  EXPECT_EQ(Vol.count("a"), 0u); // the inlined copy of the parameter
  EXPECT_EQ(Plain.count("a"), 1u);
  // The copy out of `big` into it reads `big` only: no volatile copy pins
  // the 8 KB slot of `a`.
  for (llvm::Instruction &I : llvm::instructions(*B.fn("main.main")))
    if (auto *M = llvm::dyn_cast<llvm::MemTransferInst>(&I))
      EXPECT_FALSE(M->isVolatile() && M->getRawDest()->getName() == "a");
  auto Main = variablesOf(*B.fn("main.main"));
  EXPECT_EQ(Main.count("a"), 1u); // still a variable: <optimized out> at worst
}

TEST(DebugInfo, AnAggregateParameterIsReadThroughASlotOfItsAddress) {
  // An aggregate the convention passes by address (P04.18: more than four
  // scalars) is the address the caller passed: it lives in a slot of its
  // own and the variable is read through it (DW_OP_deref), as Clang does for
  // an indirect argument. The other side: one passed as its scalars is the
  // body's own copy, read in place (no deref).
  Built C(R"(package main

type Big struct {
	a, b, c, d, e int
}

type Pair struct {
	a, b int
}

func sum(p Big) int {
	return p.a + p.e
}

func add(q Pair) int {
	return q.a + q.b
}

func main() {
	println(sum(Big{1, 2, 3, 4, 5}), add(Pair{1, 2}))
}
)");
  ASSERT_TRUE(C.LM) << C.Error;
  for (auto [Fn, Name, Deref] : {std::tuple{"main.sum", "p", true},
                                 std::tuple{"main.add", "q", false}}) {
    SCOPED_TRACE(Fn);
    bool Found = false;
    for (llvm::Instruction &I : llvm::instructions(*C.fn(Fn)))
      for (llvm::DbgRecord &R : I.getDbgRecordRange())
        if (auto *V = llvm::dyn_cast<llvm::DbgVariableRecord>(&R))
          if (V->getVariable()->getName() == Name) {
            Found = true;
            EXPECT_EQ(V->getVariable()->getArg(), 1u);
            auto *Slot =
                llvm::dyn_cast<llvm::AllocaInst>(V->getVariableLocationOp(0));
            ASSERT_NE(Slot, nullptr);
            EXPECT_EQ(Slot->getName() == std::string(Name) + ".addr", Deref);
            EXPECT_EQ(V->getExpression()->isDeref() ||
                          V->getExpression()->startsWithDeref(),
                      Deref);
          }
    EXPECT_TRUE(Found);
  }
}

TEST(DebugInfo, ABorrowedSliceParameterIsTheSliceTheBodyMakes) {
  // P04.25's hand-off: a borrowed []T arrives as a (ptr, len) pair, which the
  // body makes into a slice of its own. That slice is the debugger's
  // parameter `xs` (ArgNo 1); the pair is no variable. A `move []T` is the
  // whole slice, its four words passed as scalars (P04.18) into the body's
  // own slot. Both are {ptr, len, cap, alloc}, 32 bytes, read in place.
  Built C(R"(package main

func sum(xs []int) int {
	t := 0
	for _, x := range xs {
		t += x
	}
	return t
}

func grow(move xs []int, n int) []int {
	for i := 0; i < n; i++ {
		xs = append(xs, i)
	}
	return xs
}

func main() {
	println(sum([]int{1, 2}), len(grow([]int{3}, 2)))
}
)");
  ASSERT_TRUE(C.LM) << C.Error;
  for (const char *Fn : {"main.sum", "main.grow"}) {
    SCOPED_TRACE(Fn);
    unsigned Params = 0, Named = 0;
    for (llvm::Instruction &I : llvm::instructions(*C.fn(Fn)))
      for (llvm::DbgRecord &R : I.getDbgRecordRange())
        if (auto *V = llvm::dyn_cast<llvm::DbgVariableRecord>(&R)) {
          Params += V->getVariable()->getArg() != 0;
          if (V->getVariable()->getName() != "xs")
            continue;
          ++Named;
          EXPECT_EQ(V->getVariable()->getArg(), 1u);
          auto *T =
              llvm::cast<llvm::DICompositeType>(V->getVariable()->getType());
          EXPECT_EQ(T->getName(), "[]int64");
          EXPECT_EQ(T->getSizeInBits(), 32u * 8);
          EXPECT_EQ(T->getElements().size(), 4u);
          auto *Slot =
              llvm::dyn_cast<llvm::AllocaInst>(V->getVariableLocationOp(0));
          ASSERT_NE(Slot, nullptr);
          // sum: the slice the body made; grow: the body's copy of the
          // four words. Neither is read through an address.
          EXPECT_FALSE(V->getExpression()->startsWithDeref());
        }
    EXPECT_EQ(Named, 1u); // one `xs`, not the pair as well
    EXPECT_EQ(Params, std::string(Fn) == "main.sum" ? 1u : 2u);
  }
}

//===----------------------------------------------------------------------===//
// Work: near-linear (M8), with a control; and deep types
//===----------------------------------------------------------------------===//

::testing::AssertionResult stepsAreLinear(uint64_t S1, uint64_t S4,
                                          uint64_t N) {
  const uint64_t C = 24;
  if (S1 == 0 || S1 > C * N || S4 > C * 4 * N || 2 * S4 > 9 * S1)
    return ::testing::AssertionFailure()
           << "steps(N) = " << S1 << ", steps(4N) = " << S4 << ", N = " << N;
  return ::testing::AssertionSuccess();
}

/// N variables in one function.
std::string manyVariables(unsigned N) {
  std::string S = "package main\nfunc main() {\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tv" + std::to_string(I) + " := " + std::to_string(I) + "\n\tt += v" +
         std::to_string(I) + "\n";
  return S + "\tprintln(t)\n}\n";
}

/// N functions with a parameter and a variable each.
std::string manyFunctions(unsigned N) {
  std::string S = "package main\n";
  for (unsigned I = 0; I < N; ++I)
    S += "func f" + std::to_string(I) + "(a int) int {\n\tb := a + " +
         std::to_string(I) + "\n\treturn b\n}\n";
  return S + "func main() { println(f0(1)) }\n";
}

/// A chain of N structs, each holding the one before.
std::string deepChain(unsigned N) {
  std::string S = "package main\ntype T0 struct{ a int }\n";
  for (unsigned I = 1; I < N; ++I)
    S += "type T" + std::to_string(I) + " struct{ a T" + std::to_string(I - 1) +
         " }\n";
  return S + "func main() {\n\tvar x T" + std::to_string(N - 1) +
         "\n\t_ = x\n}\n";
}

/// N calls of one trivial function, all inlined.
std::string manyInlinedCalls(unsigned N) {
  std::string S =
      "package main\nfunc add(x, y int) int {\n\tz := x + y\n\treturn z\n}\n"
      "func main() {\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tt = add(t, " + std::to_string(I) + ")\n";
  return S + "\tprintln(t)\n}\n";
}

struct Steps {
  uint64_t Types, Vars, Locs;
};
Steps stepsOf(const std::string &Src, bool Inline = false) {
  Built B(Src, Inline);
  EXPECT_TRUE(B.LM) << B.Error;
  return {B.Counts.DITypeSteps, B.Counts.DIVariableSteps,
          B.Counts.DILocationSteps};
}

TEST(DebugInfoStress, ManyVariablesInOneFunction) {
  Steps A = stepsOf(manyVariables(500)), B = stepsOf(manyVariables(2000));
  EXPECT_TRUE(stepsAreLinear(A.Vars, B.Vars, 500));
  EXPECT_TRUE(stepsAreLinear(A.Locs, B.Locs, 500));
  EXPECT_TRUE(stepsAreLinear(A.Types, B.Types, 500) || A.Types == B.Types)
      << "one int type, whatever the number of variables";
}

TEST(DebugInfoStress, ManyFunctions) {
  Steps A = stepsOf(manyFunctions(500)), B = stepsOf(manyFunctions(2000));
  EXPECT_TRUE(stepsAreLinear(A.Vars, B.Vars, 500));
  EXPECT_TRUE(stepsAreLinear(A.Locs, B.Locs, 500));
}

TEST(DebugInfoStress, ADeepChainOfNamedTypes) {
  Steps A = stepsOf(deepChain(250)), B = stepsOf(deepChain(1000));
  EXPECT_TRUE(stepsAreLinear(A.Types, B.Types, 250));
}

TEST(DebugInfoStress, ManyInlinedCalls) {
  Steps A = stepsOf(manyInlinedCalls(300), true),
        B = stepsOf(manyInlinedCalls(1200), true);
  EXPECT_TRUE(stepsAreLinear(A.Locs, B.Locs, 300));
  EXPECT_TRUE(stepsAreLinear(A.Vars, B.Vars, 300));
}

TEST(DebugInfoStress, ControlAQuadraticCountFailsTheCheck) {
  EXPECT_TRUE(stepsAreLinear(5 * 1000, 5 * 4000, 1000));
  EXPECT_FALSE(stepsAreLinear(1000ull * 1000, 16ull * 1000 * 1000, 1000));
}

/// The definitions of the struct types of a module by name (a debugger finds
/// the definition of a declaration this way).
std::map<std::string, const llvm::DICompositeType *>
definitions(const llvm::Module &M) {
  std::map<std::string, const llvm::DICompositeType *> Out;
  llvm::DebugInfoFinder Finder;
  Finder.processModule(M);
  for (const llvm::DIType *T : Finder.types())
    if (auto *C = llvm::dyn_cast<llvm::DICompositeType>(T))
      if (!C->isForwardDecl() && !C->getName().empty())
        Out[C->getName().str()] = C;
  for (const llvm::DICompileUnit *CU : M.debug_compile_units())
    for (const llvm::DIScope *S : CU->getRetainedTypes())
      if (auto *C = llvm::dyn_cast<llvm::DICompositeType>(S))
        Out[C->getName().str()] = C;
  return Out;
}

/// The longest chain of references from T that LLVM's DWARF writer follows
/// (members, bases, elements, the types of a subroutine); a declaration ends
/// it. Iterative; a cycle of references is a failure (-1).
int heightOf(const llvm::DIType *Root) {
  std::map<const llvm::Metadata *, int> H; // -2: on the stack
  std::vector<std::pair<const llvm::Metadata *, bool>> Work{{Root, false}};
  auto Parts = [](const llvm::Metadata *N) {
    std::vector<const llvm::Metadata *> Out;
    if (auto *C = llvm::dyn_cast<llvm::DICompositeType>(N)) {
      if (C->isForwardDecl())
        return Out;
      if (C->getBaseType())
        Out.push_back(C->getBaseType());
      if (C->getElements())
        for (const llvm::Metadata *E : C->getElements()->operands())
          if (llvm::isa<llvm::DIType>(E))
            Out.push_back(E);
    } else if (auto *D = llvm::dyn_cast<llvm::DIDerivedType>(N)) {
      if (D->getBaseType())
        Out.push_back(D->getBaseType());
    } else if (auto *S = llvm::dyn_cast<llvm::DISubroutineType>(N)) {
      for (const llvm::Metadata *E : S->getTypeArray()->operands())
        if (E)
          Out.push_back(E);
    }
    return Out;
  };
  while (!Work.empty()) {
    auto [N, Done] = Work.back();
    Work.pop_back();
    if (Done) {
      int Max = 0;
      for (const llvm::Metadata *P : Parts(N))
        Max = std::max(Max, H[P]);
      H[N] = Max + 1;
      continue;
    }
    auto It = H.find(N);
    if (It != H.end()) {
      if (It->second == -2)
        return -1;
      continue;
    }
    H[N] = -2;
    Work.push_back({N, true});
    for (const llvm::Metadata *P : Parts(N)) {
      auto Seen = H.find(P);
      if (Seen != H.end() && Seen->second == -2)
        return -1;
      if (Seen == H.end())
        Work.push_back({P, false});
    }
  }
  return H[Root];
}

TEST(DebugInfoDeep, AChainOfTypesHopsToNewStacksAndKeepsItsShape) {
  // A walk that hops every 8 levels: the metadata is the same as a plain walk.
  // The chain is 200 types deep, higher than MaxTypeHeight: every few dozen
  // levels a member is a declaration, whose definition the unit keeps, so a
  // debugger that follows the names finds every level.
  ScopedNewStackSegment Hop(8);
  Built B(deepChain(200));
  ASSERT_TRUE(B.LM) << B.Error;
  auto Defs = definitions(*B.LM);
  auto V = variablesOf(*B.fn("main.main"));
  const llvm::DIType *T = V["x"]->getType();
  EXPECT_GT(heightOf(T), 0);
  EXPECT_LE(heightOf(T), int(DebugInfoBuilder::MaxTypeHeight) + 1);
  unsigned Depth = 0, Cuts = 0;
  while (auto *C = llvm::dyn_cast<llvm::DICompositeType>(T)) {
    if (C->isForwardDecl()) {
      ++Cuts;
      ASSERT_EQ(Defs.count(C->getName().str()), 1u) << C->getName().str();
      C = Defs[C->getName().str()];
      EXPECT_LE(heightOf(C), int(DebugInfoBuilder::MaxTypeHeight) + 1);
    }
    EXPECT_EQ(C->getName(), "main.T" + std::to_string(199 - Depth));
    ++Depth;
    auto *M = llvm::dyn_cast<llvm::DIDerivedType>(
        *C->getElements()->operands().begin());
    ASSERT_NE(M, nullptr);
    T = M->getBaseType();
  }
  EXPECT_EQ(Depth, 200u);
  EXPECT_GE(Cuts, 3u);
  EXPECT_EQ(T->getName(), "int64");
}

//===----------------------------------------------------------------------===//
// Modules built in GIR: names, types and scopes on both targets
//===----------------------------------------------------------------------===//

/// A module that lowers (no cleanup blocks), with one file.
struct Mini {
  gir::Context C;
  gir::Module M{C, "mini"};
  gir::Type *I64 = C.i64(), *Bool = C.boolean(), *Str = C.string();
  uint32_t File = M.addFile("m.gopp") + 1;
};

struct Lowered {
  llvm::LLVMContext Ctx;
  std::unique_ptr<llvm::TargetMachine> TM;
  std::unique_ptr<llvm::Module> LM;
  LowerWorkCounts Counts;
  bool Ok = false;
  Lowered(const Mini &X, const char *Target = "linux/amd64",
          LowerToLLVMOptions O = {}) {
    initializeTargets();
    auto T = parseTarget(Target);
    EXPECT_TRUE(static_cast<bool>(T));
    if (!T) {
      llvm::consumeError(T.takeError());
      return;
    }
    auto Made = createTargetMachine(*T);
    EXPECT_TRUE(static_cast<bool>(Made));
    if (!Made) {
      llvm::consumeError(Made.takeError());
      return;
    }
    TM = std::move(*Made);
    O.Counts = &Counts;
    auto R = lowerToLLVM(Ctx, X.M, *TM, O);
    EXPECT_TRUE(static_cast<bool>(R)) << llvm::toString(R.takeError());
    if (!R)
      return;
    LM = std::move(*R);
    Ok = true;
  }
  const llvm::DISubprogram *sp(const char *Name) const {
    const llvm::Function *F = LM->getFunction(Name);
    return F ? F->getSubprogram() : nullptr;
  }
  /// The variables of the dbg.declare records of function Name, by name.
  std::map<std::string, const llvm::DILocalVariable *>
  vars(const char *Name) const {
    std::map<std::string, const llvm::DILocalVariable *> V;
    const llvm::Function *F = LM->getFunction(Name);
    if (!F)
      return V;
    for (const llvm::BasicBlock &BB : *F)
      for (const llvm::Instruction &I : BB)
        for (const llvm::DbgRecord &R : I.getDbgRecordRange())
          if (auto *DV = llvm::dyn_cast<llvm::DbgVariableRecord>(&R))
            V[DV->getVariable()->getName().str()] = DV->getVariable();
    return V;
  }
  std::string emit() {
    std::string S;
    llvm::raw_string_ostream OS(S);
    llvm::Error E = emitAssembly(*LM, *TM, OS);
    EXPECT_FALSE(static_cast<bool>(E)) << llvm::toString(std::move(E));
    return S;
  }
};

/// f(a Param) with a source parameter a and a source variable v of VarTy.
gir::FuncId withVar(Mini &X, const char *Name, gir::Type *Param,
                    gir::Type *VarTy) {
  using namespace gir;
  FuncId F = test::declare(X.M, Name, X.C.func({Param}, X.C.unit()));
  FuncBuilder B(X.M, X.M.func(F));
  Function &Fn = X.M.func(F);
  Fn.Loc = {X.File, 3, 1};
  Fn.CC = CallConv::Go;
  Fn.Locals[1].Name = "a";
  Fn.Locals[1].Source = true;
  Fn.Locals[1].Loc = {X.File, 3, 8};
  LocalId V = B.newLocal(VarTy, Local::Var, "v");
  Fn.Locals[V].Source = true;
  Fn.Locals[V].Loc = {X.File, 4, 2};
  B.ret();
  return F;
}

TEST(DebugInfo, ACopyIsAVariableByItsCalleeNotByTheFunctionItIsIn) {
  // An interface thunk has no source position, so its own locals are no
  // variables; the method GIR inlines into it keeps its receiver and its
  // locals, which are the method's, kept in memory at `opt 0` (review R5).
  const char *Src = R"(package main

type Shape interface {
	Area() int
}

type Rect struct {
	w, h int
}

func (r Rect) Area() int {
	a := r.w * r.h
	return a
}

func main() {
	var s Shape = Rect{2, 3}
	println(s.Area())
}
)";
  LowerToLLVMOptions O;
  O.KeepVariables = true;
  Built B(Src, /*Inline=*/true, O);
  ASSERT_TRUE(B.LM) << B.Error;
  llvm::Function *Thunk = B.fn("interface thunk for main.Rect.Area");
  ASSERT_NE(Thunk, nullptr);
  EXPECT_TRUE(Thunk->getSubprogram()->isArtificial());
  llvm::DISubprogram *Area = B.fn("main.Rect.Area")->getSubprogram();
  auto V = variablesOf(*Thunk);
  EXPECT_EQ(V.size(), 2u); // r and a, and none of the thunk's own
  ASSERT_EQ(V.count("r"), 1u);
  ASSERT_EQ(V.count("a"), 1u);
  EXPECT_EQ(V["r"]->getScope()->getSubprogram(), Area);
  EXPECT_EQ(V["r"]->getArg(), 1u);
  EXPECT_EQ(V["a"]->getScope()->getSubprogram(), Area);
  EXPECT_EQ(volatileSlots(*Thunk).first.count("a"), 1u);
  // The other side: a function with no position shows none of its locals,
  // even one marked as the program's.
  Mini X;
  gir::FuncId F = withVar(X, "helper", X.I64, X.I64);
  X.M.func(F).Loc = {};
  Lowered L(X);
  ASSERT_TRUE(L.Ok);
  EXPECT_TRUE(L.vars("helper").empty());
}

TEST(DebugInfo, FunctionsGetTheirReadableName) {
  // An operator method runs since P04.23 (tests/debug/ops.test); here its name
  // is checked on the symbol, on both targets, with and without the linkage
  // name.
  Mini X;
  const char *Op = "_GPTo6op_addN4mainE4Vec2LB12_E"; // main.Vec2.+(main.Vec2)
  const char *Fn = "_GPF4mainE3Foo";
  const char *Raw = "plain_c_name";
  for (const char *N : {Op, Fn, Raw}) {
    gir::FuncId F = test::declare(X.M, N, X.C.func({}, X.C.unit()));
    X.M.func(F).Loc = {X.File, 1, 1};
    gir::FuncBuilder B(X.M, X.M.func(F));
    B.ret();
  }
  ASSERT_EQ(test::errorsOf(X.M), "");
  for (const char *Target : {"linux/amd64", "linux/arm64"}) {
    SCOPED_TRACE(Target);
    Lowered L(X, Target);
    ASSERT_TRUE(L.Ok);
    // The backtrace shows `main.Vec2.+(main.Vec2)`, not the symbol.
    EXPECT_EQ(L.sp(Op)->getName(), "main.Vec2.+(main.Vec2)");
    EXPECT_EQ(L.sp(Fn)->getName(), "main.Foo");
    EXPECT_EQ(L.sp(Raw)->getName(), "plain_c_name"); // not a _GP symbol
    for (const char *N : {Op, Fn, Raw})
      EXPECT_EQ(L.sp(N)->getLinkageName(), "") << N;
  }
  LowerToLLVMOptions O;
  O.DwarfLinkageNames = true;
  Lowered L(X, "linux/amd64", O);
  ASSERT_TRUE(L.Ok);
  EXPECT_EQ(L.sp(Op)->getLinkageName(), Op);
  EXPECT_EQ(L.sp(Op)->getName(), "main.Vec2.+(main.Vec2)");
  // A name that is its own readable form has no separate linkage name.
  EXPECT_EQ(L.sp(Raw)->getLinkageName(), "");
}

/// The DWARF type name of the variable v of a new function whose v has T.
std::string dwarfNameOf(Mini &X, gir::Type *T,
                        const char *Target = "linux/amd64") {
  static int N = 0;
  std::string Name = "f" + std::to_string(N++);
  withVar(X, Name.c_str(), X.I64, T);
  Lowered L(X, Target);
  if (!L.Ok)
    return "<lowering failed>";
  auto V = L.vars(Name.c_str());
  if (!V.count("v") || !V["v"]->getType())
    return "<no variable>";
  return V["v"]->getType()->getName().str();
}

TEST(DebugInfo, EveryKindOfTypeHasItsSpelling) {
  for (const char *Target : {"linux/amd64", "linux/arm64"}) {
    SCOPED_TRACE(Target);
    Mini X;
    gir::Context &C = X.C;
    EXPECT_EQ(dwarfNameOf(X, C.i64(), Target), "int64");
    EXPECT_EQ(dwarfNameOf(X, C.intTy(8, false), Target), "uint8");
    EXPECT_EQ(dwarfNameOf(X, C.intTy(16, true), Target), "int16");
    EXPECT_EQ(dwarfNameOf(X, C.floatTy(32), Target), "float32");
    EXPECT_EQ(dwarfNameOf(X, C.boolean(), Target), "bool");
    EXPECT_EQ(dwarfNameOf(X, C.string(), Target), "string");
    EXPECT_EQ(dwarfNameOf(X, C.slice(C.string()), Target), "[]string");
    EXPECT_EQ(dwarfNameOf(X, C.ptr(C.i64()), Target), "*int64");
    EXPECT_EQ(dwarfNameOf(X, C.option(C.i64()), Target), "Option[int64]");
    EXPECT_EQ(dwarfNameOf(X, C.tuple({C.i64(), C.boolean()}), Target),
              "(int64, bool)");
    EXPECT_EQ(dwarfNameOf(X, C.func({C.i64()}, C.boolean()), Target),
              "func(int64) bool");
  }
}

TEST(DebugInfo, StructsHaveTheirNamesFieldsAndOffsets) {
  Mini X;
  gir::Type *P = X.C.structTy("main.Pair");
  ASSERT_TRUE(X.C.setStructBody(P, {X.I64, X.C.boolean(), X.C.string()}));
  ASSERT_TRUE(X.C.setFieldNames(P, {"n", "ok", "name"}));
  // A struct without field names (a runtime struct): _N.
  gir::Type *Q = X.C.structTy("main.Bare");
  ASSERT_TRUE(X.C.setStructBody(Q, {X.I64, X.I64}));
  withVar(X, "f", P, Q);
  for (const char *Target : {"linux/amd64", "linux/arm64"}) {
    SCOPED_TRACE(Target);
    Lowered L(X, Target);
    ASSERT_TRUE(L.Ok);
    auto V = L.vars("f");
    ASSERT_TRUE(V.count("a") && V.count("v"));
    // The parameter is an aggregate: read through its address.
    auto *A = llvm::cast<llvm::DICompositeType>(V["a"]->getType());
    EXPECT_EQ(A->getName(), "main.Pair");
    EXPECT_EQ(A->getSizeInBits(), 8u * 40); // 8 + 1(+7) + 24
    std::vector<std::string> Names;
    std::vector<uint64_t> Offs;
    for (llvm::Metadata *E : A->getElements()->operands()) {
      auto *M = llvm::cast<llvm::DIDerivedType>(E);
      Names.push_back(M->getName().str());
      Offs.push_back(M->getOffsetInBits());
    }
    EXPECT_EQ(Names, (std::vector<std::string>{"n", "ok", "name"}));
    EXPECT_EQ(Offs, (std::vector<uint64_t>{0, 64, 128}));
    auto *B = llvm::cast<llvm::DICompositeType>(V["v"]->getType());
    std::vector<std::string> BN;
    for (llvm::Metadata *E : B->getElements()->operands())
      BN.push_back(llvm::cast<llvm::DIDerivedType>(E)->getName().str());
    EXPECT_EQ(BN, (std::vector<std::string>{"_0", "_1"}));
  }
}

TEST(DebugInfo, EveryNameNamesOneType) {
  // A declaration is found by its name, so a name never stands for two
  // layouts: an unnamed struct is spelled by its fields, and two of one
  // spelling (`struct { a int }` and `struct { a int64 }`) have one layout
  // and are one DWARF type; a name cut at MaxNameLength ends in a hash of the
  // whole type, so two types that differ only past the cut have two names.
  Mini X;
  gir::Type *S1 = X.C.structTy("struct.0"), *S2 = X.C.structTy("struct.1");
  ASSERT_TRUE(X.C.setStructBody(S1, {X.I64}));
  ASSERT_TRUE(X.C.setFieldNames(S1, {"a"}));
  ASSERT_TRUE(X.C.setStructBody(S2, {X.I64}));
  ASSERT_TRUE(X.C.setFieldNames(S2, {"a"}));
  std::vector<gir::Type *> Many(40, X.I64), Other(40, X.I64);
  Other.back() = X.C.boolean();
  gir::Type *Long1 = X.C.tuple(Many), *Long2 = X.C.tuple(Other);
  withVar(X, "f", S1, S2);
  withVar(X, "g", Long1, Long2);
  Lowered L(X);
  ASSERT_TRUE(L.Ok);
  auto F = L.vars("f"), G = L.vars("g");
  EXPECT_EQ(F["a"]->getType()->getName(), "struct { a int64 }");
  EXPECT_EQ(F["v"]->getType(), F["a"]->getType());
  std::string N1 = G["a"]->getType()->getName().str(),
              N2 = G["v"]->getType()->getName().str();
  EXPECT_NE(N1, N2);
  EXPECT_LE(N1.size(), DebugInfoBuilder::MaxNameLength + 20); // ...#, 16 hex
  EXPECT_NE(N1.find("...#"), std::string::npos);
  EXPECT_EQ(N1.substr(0, 10), N2.substr(0, 10));
}

TEST(DebugInfo, SelfNamingAndRecursiveTypesEndTheirCycles) {
  Mini X;
  gir::Context &C = X.C;
  // type Link struct { next *Link; n int }
  gir::Type *Link = C.structTy("main.Link");
  ASSERT_TRUE(C.setStructBody(Link, {C.ptr(Link), C.i64()}));
  // type S []S ; type O Option[*O] (a pointer to itself)
  gir::Type *S = C.recTy("main.S");
  ASSERT_TRUE(C.setRecBody(S, C.slice(S)));
  gir::Type *O = C.recTy("main.O");
  ASSERT_TRUE(C.setRecBody(O, C.ptr(O)));
  withVar(X, "f", Link, S);
  withVar(X, "g", O, C.array(O, 2));
  Lowered L(X);
  ASSERT_TRUE(L.Ok);
  auto V = L.vars("f");
  EXPECT_EQ(V["a"]->getType()->getName(), "main.Link");
  EXPECT_EQ(V["v"]->getType()->getName(), "main.S");
  // `type O Option[*O]` is a struct of its name holding the pointer, which
  // points to O's declaration: no type is its own base.
  auto G = L.vars("g");
  auto *SO = llvm::cast<llvm::DICompositeType>(G["a"]->getType());
  EXPECT_EQ(SO->getName(), "main.O");
  auto *PO =
      llvm::cast<llvm::DIDerivedType>(member(SO, "value")->getBaseType());
  EXPECT_TRUE(isDeclarationOf(PO->getBaseType(), "main.O"));
  for (auto &[Name, Var] : V)
    EXPECT_GT(heightOf(Var->getType()), 0) << Name; // no cycle
  for (auto &[Name, Var] : G)
    EXPECT_GT(heightOf(Var->getType()), 0) << Name;
  EXPECT_NE(L.emit(), ""); // the DWARF writer finishes
}

/// The types of the cuts (R1 of the review), each the type of the variable
/// `v` of a function of its own, the functions made in the order Order gives
/// (0: as listed, 1: reversed, 2: every third one first), so the first type
/// asked for differs: a self-referring struct and a pointer to it, a pair of
/// structs that point to each other, a chain of structs through pointers, a
/// struct 47 levels high in an array, a struct that points to an array of
/// itself, a struct whose function takes it, chains of 60 pointers and 60
/// arrays with no struct in them, two unnamed structs of one spelling and
/// two long names that differ past the cut.
std::vector<std::string> cutTypes(Mini &X, int Order = 0) {
  gir::Context &C = X.C;
  gir::Type *I64 = X.I64;
  std::vector<std::pair<std::string, gir::Type *>> Asks;
  auto Struct = [&](const std::string &Name, std::vector<gir::Type *> Fields,
                    std::vector<llvm::StringRef> Names) {
    gir::Type *T = C.structTy(Name);
    EXPECT_TRUE(C.setStructBody(T, Fields));
    if (!Names.empty())
      EXPECT_TRUE(C.setFieldNames(T, Names));
    return T;
  };
  gir::Type *Node = C.structTy("main.Node");
  EXPECT_TRUE(C.setStructBody(Node, {I64, C.ptr(Node)}));
  EXPECT_TRUE(C.setFieldNames(Node, {"val", "next"}));
  Asks.push_back({"node", Node});
  Asks.push_back({"pnode", C.ptr(Node)});
  gir::Type *A = C.structTy("main.A"), *B = C.structTy("main.B");
  EXPECT_TRUE(C.setStructBody(A, {C.ptr(B), I64}));
  EXPECT_TRUE(C.setFieldNames(A, {"b", "v"}));
  EXPECT_TRUE(C.setStructBody(B, {C.ptr(A), I64}));
  EXPECT_TRUE(C.setFieldNames(B, {"a", "w"}));
  Asks.push_back({"pb", C.ptr(B)});
  Asks.push_back({"a", A});
  gir::Type *P = Struct("main.P0", {C.array(C.array(I64, 1), 1)}, {"v"});
  for (unsigned I = 1; I < 20; ++I)
    P = Struct("main.P" + std::to_string(I), {I64, C.ptr(P)}, {"v", "p"});
  Asks.push_back({"p19", P});
  gir::Type *T = Struct("main.T0", {I64}, {"v"});
  for (unsigned I = 1; I <= 22; ++I)
    T = Struct("main.T" + std::to_string(I), {I64, T}, {"v", "in"});
  gir::Type *Holder = Struct("main.Holder", {C.array(T, 2), I64}, {"arr", "k"});
  Asks.push_back({"holder", Holder});
  Asks.push_back({"arr", C.array(T, 2)});
  gir::Type *S = C.structTy("main.S");
  EXPECT_TRUE(C.setStructBody(S, {C.ptr(C.array(S, 2)), I64}));
  EXPECT_TRUE(C.setFieldNames(S, {"p", "v"}));
  Asks.push_back({"s", S});
  gir::Type *F = C.structTy("main.F");
  EXPECT_TRUE(C.setStructBody(F, {C.func({F}, I64)}));
  EXPECT_TRUE(C.setFieldNames(F, {"f"}));
  Asks.push_back({"f", F});
  gir::Type *Q = Node, *R = I64;
  for (unsigned I = 0; I < 60; ++I) {
    Q = C.ptr(Q);
    R = C.array(R, 1);
  }
  Asks.push_back({"q", Q});
  Asks.push_back({"r", R});
  Asks.push_back({"anon1", Struct("struct.0", {I64}, {"a"})});
  Asks.push_back({"anon2", Struct("struct.1", {I64}, {"a"})});
  std::vector<gir::Type *> Many(40, I64), Other(40, I64);
  Other.back() = C.boolean();
  Asks.push_back({"long1", C.tuple(Many)});
  Asks.push_back({"long2", C.tuple(Other)});
  std::vector<std::string> Names;
  for (auto &[Name, Ty] : Asks)
    Names.push_back(Name);
  if (Order == 1)
    std::reverse(Asks.begin(), Asks.end());
  if (Order == 2) {
    std::vector<std::pair<std::string, gir::Type *>> Third, Rest;
    for (size_t I = 0; I < Asks.size(); ++I)
      (I % 3 == 2 ? Third : Rest).push_back(Asks[I]);
    Asks = Third;
    Asks.insert(Asks.end(), Rest.begin(), Rest.end());
  }
  for (auto &[Name, Ty] : Asks)
    withVar(X, Name.c_str(), I64, Ty);
  EXPECT_EQ(test::errorsOf(X.M), "");
  return Names;
}

/// A DWARF type in text, part by part (a declaration by its name, each
/// definition's parts once), so the types of two modules can be compared
/// whatever numbers their metadata has.
void printShape(const llvm::Metadata *N, llvm::raw_ostream &OS,
                std::set<const llvm::Metadata *> &Seen) {
  if (!N) {
    OS << "-";
    return;
  }
  if (auto *C = llvm::dyn_cast<llvm::DICompositeType>(N)) {
    OS << llvm::dwarf::TagString(C->getTag()) << " '" << C->getName() << "' "
       << C->getSizeInBits();
    if (C->isForwardDecl()) {
      OS << " declaration";
      return;
    }
    if (!Seen.insert(C).second)
      return;
    OS << " (";
    printShape(C->getBaseType(), OS, Seen);
    for (const llvm::Metadata *E : C->getElements()->operands()) {
      OS << ", ";
      if (auto *R = llvm::dyn_cast<llvm::DISubrange>(E)) {
        if (auto *Count =
                llvm::dyn_cast_if_present<llvm::ConstantInt *>(R->getCount()))
          OS << "[" << Count->getSExtValue() << "]";
        continue;
      }
      printShape(E, OS, Seen);
    }
    OS << ")";
  } else if (auto *D = llvm::dyn_cast<llvm::DIDerivedType>(N)) {
    OS << llvm::dwarf::TagString(D->getTag()) << " '" << D->getName() << "' "
       << D->getOffsetInBits() << "/" << D->getSizeInBits() << " ";
    printShape(D->getBaseType(), OS, Seen);
  } else if (auto *Basic = llvm::dyn_cast<llvm::DIBasicType>(N)) {
    OS << "'" << Basic->getName() << "'";
  } else if (auto *Sub = llvm::dyn_cast<llvm::DISubroutineType>(N)) {
    OS << "func(";
    for (const llvm::Metadata *E : Sub->getTypeArray()->operands()) {
      printShape(E, OS, Seen);
      OS << "; ";
    }
    OS << ")";
  } else {
    OS << "?";
  }
}

std::string shapeOf(const llvm::Metadata *N) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  std::set<const llvm::Metadata *> Seen;
  printShape(N, OS, Seen);
  return S;
}

/// The definitions a module keeps for its declarations, in its order.
std::string keptShapes(const llvm::Module &M) {
  std::string S;
  for (const llvm::DICompileUnit *CU : M.debug_compile_units())
    for (const llvm::DIScope *T : CU->getRetainedTypes())
      S += shapeOf(T) + "\n";
  return S;
}

TEST(DebugInfo, TypesDoNotDependOnTheOrderTheyAreAskedFor) {
  // Where a type is cut, and what names it has, depend on the type alone:
  // asked for in another order, every type is the same DWARF, part by part,
  // and the unit keeps the same definitions in the same order.
  Mini X0, X1, X2;
  std::vector<std::string> Names = cutTypes(X0, 0);
  cutTypes(X1, 1);
  cutTypes(X2, 2);
  Lowered L0(X0), L1(X1), L2(X2);
  ASSERT_TRUE(L0.Ok && L1.Ok && L2.Ok);
  for (const std::string &F : Names) {
    std::string Want = shapeOf(L0.vars(F.c_str())["v"]->getType());
    EXPECT_EQ(shapeOf(L1.vars(F.c_str())["v"]->getType()), Want) << F;
    EXPECT_EQ(shapeOf(L2.vars(F.c_str())["v"]->getType()), Want) << F;
  }
  std::string Kept = keptShapes(*L0.LM);
  EXPECT_NE(Kept, "");
  EXPECT_EQ(keptShapes(*L1.LM), Kept);
  EXPECT_EQ(keptShapes(*L2.LM), Kept);
}

TEST(DebugInfo, PointersAndArraysAreNeverDeclarations) {
  // A debugger dereferences a pointer and indexes an array, so neither is a
  // declaration: a pointer refers to a struct by its declaration, which the
  // unit keeps the definition of; an array of a struct too high to be held
  // is an array of the struct's declaration; a chain of pointers or arrays
  // with no struct in it is held, every few dozen levels, by a struct of its
  // name. Every declaration finds its definition by its name.
  Mini X;
  cutTypes(X);
  Lowered L(X);
  ASSERT_TRUE(L.Ok);
  auto Defs = definitions(*L.LM);
  llvm::DebugInfoFinder Finder;
  Finder.processModule(*L.LM);
  unsigned Declarations = 0;
  for (const llvm::DIType *T : Finder.types()) {
    if (auto *D = llvm::dyn_cast<llvm::DIDerivedType>(T))
      if (D->getTag() == llvm::dwarf::DW_TAG_pointer_type)
        EXPECT_NE(D->getBaseType(), nullptr) << D->getName().str();
    auto *C = llvm::dyn_cast<llvm::DICompositeType>(T);
    if (!C)
      continue;
    if (C->isForwardDecl()) {
      ++Declarations;
      ASSERT_EQ(Defs.count(C->getName().str()), 1u) << C->getName().str();
      EXPECT_EQ(Defs[C->getName().str()]->getTag(),
                llvm::dwarf::DW_TAG_structure_type);
      continue;
    }
    EXPECT_LE(heightOf(C), int(DebugInfoBuilder::MaxTypeHeight) + 2)
        << C->getName().str();
  }
  EXPECT_GT(Declarations, 0u);
  auto V = [&](const char *F) { return L.vars(F)["v"]->getType(); };
  auto Base = [](const llvm::DIType *T) {
    return llvm::cast<llvm::DIDerivedType>(T)->getBaseType();
  };
  // A pointer asked for first refers to the declaration, and the struct's own
  // pointer to itself too.
  EXPECT_TRUE(isDeclarationOf(Base(V("pnode")), "main.Node"));
  EXPECT_TRUE(isDeclarationOf(
      Base(member(Defs["main.Node"], "next")->getBaseType()), "main.Node"));
  EXPECT_TRUE(isDeclarationOf(Base(V("pb")), "main.B"));
  EXPECT_TRUE(isDeclarationOf(Base(member(Defs["main.B"], "a")->getBaseType()),
                              "main.A"));
  EXPECT_TRUE(isDeclarationOf(
      Base(member(llvm::cast<llvm::DICompositeType>(V("p19")), "p")
               ->getBaseType()),
      "main.P18"));
  // [2]main.T22 is 48 levels high: in Holder it is the array of T22's
  // declaration; a variable of it holds the definition.
  auto *Arr = llvm::cast<llvm::DICompositeType>(
      member(llvm::cast<llvm::DICompositeType>(V("holder")), "arr")
          ->getBaseType());
  EXPECT_EQ(Arr->getTag(), llvm::dwarf::DW_TAG_array_type);
  EXPECT_TRUE(isDeclarationOf(Arr->getBaseType(), "main.T22"));
  auto *Whole = llvm::cast<llvm::DICompositeType>(V("arr"));
  EXPECT_EQ(Whole->getTag(), llvm::dwarf::DW_TAG_array_type);
  EXPECT_FALSE(
      llvm::cast<llvm::DICompositeType>(Whole->getBaseType())->isForwardDecl());
  // S{ p *[2]S }: the pointer refers to the array of S's declaration.
  auto *SP = llvm::cast<llvm::DICompositeType>(Base(
      member(llvm::cast<llvm::DICompositeType>(V("s")), "p")->getBaseType()));
  EXPECT_EQ(SP->getTag(), llvm::dwarf::DW_TAG_array_type);
  EXPECT_TRUE(isDeclarationOf(SP->getBaseType(), "main.S"));
  // 60 pointers and 60 arrays: followed through the structs that hold them
  // by name, every level is there, down to main.Node and int64.
  for (const char *F : {"q", "r"}) {
    SCOPED_TRACE(F);
    const llvm::DIType *T = V(F);
    unsigned Levels = 0, Held = 0;
    for (;;) {
      if (auto *C = llvm::dyn_cast<llvm::DICompositeType>(T);
          C && C->isForwardDecl() && C->getName() != "main.Node") {
        ++Held;
        ASSERT_EQ(Defs.count(C->getName().str()), 1u);
        T = member(Defs[C->getName().str()], "value")->getBaseType();
        continue;
      }
      if (auto *D = llvm::dyn_cast<llvm::DIDerivedType>(T)) {
        EXPECT_EQ(D->getTag(), llvm::dwarf::DW_TAG_pointer_type);
        ++Levels;
        T = D->getBaseType();
        continue;
      }
      auto *C = llvm::dyn_cast<llvm::DICompositeType>(T);
      if (!C || C->getTag() != llvm::dwarf::DW_TAG_array_type)
        break;
      ++Levels;
      T = C->getBaseType();
    }
    EXPECT_EQ(Levels, 60u);
    EXPECT_GE(Held, 1u);
    EXPECT_EQ(T->getName(), *F == 'q' ? "main.Node" : "int64");
  }
  EXPECT_FALSE(llvm::verifyModule(*L.LM, &llvm::errs()));
  EXPECT_NE(L.emit(), "");
}

//===----------------------------------------------------------------------===//
// Scopes, deep and wide types: bounded depth, linear work
//===----------------------------------------------------------------------===//

std::set<const llvm::MDNode *> blocksOf(const llvm::Module &M) {
  std::set<const llvm::MDNode *> B;
  for (const llvm::Function &F : M)
    for (const llvm::BasicBlock &BB : F)
      for (const llvm::Instruction &I : BB)
        for (const llvm::DbgRecord &R : I.getDbgRecordRange())
          for (const llvm::DIScope *S = R.getDebugLoc()->getScope();
               llvm::isa_and_nonnull<llvm::DILexicalBlock>(S);
               S = llvm::cast<llvm::DILexicalBlock>(S)->getScope())
            B.insert(S);
  return B;
}

/// The longest chain of lexical blocks from a location up to its subprogram:
/// the length that makes LLVM's walks (and so the work) grow.
unsigned deepestChain(const llvm::Module &M) {
  unsigned Max = 0;
  for (const llvm::Function &F : M)
    for (const llvm::BasicBlock &BB : F)
      for (const llvm::Instruction &I : BB)
        for (const llvm::DbgRecord &R : I.getDbgRecordRange()) {
          unsigned D = 0;
          const llvm::DIScope *S = R.getDebugLoc()->getScope();
          while (auto *LB = llvm::dyn_cast_or_null<llvm::DILexicalBlock>(S)) {
            ++D;
            S = LB->getScope();
          }
          Max = std::max(Max, D);
        }
  return Max;
}

/// A function with blocks, each with a source variable. Mode 0: N nested and
/// N siblings; 1: 2N nested; 2: N siblings.
void manyScopes(Mini &X, unsigned N, int Mode = 0) {
  using namespace gir;
  FuncId F = test::declare(X.M, "scopes", X.C.func({}, X.C.unit()));
  FuncBuilder B(X.M, X.M.func(F));
  Function &Fn = X.M.func(F);
  Fn.Loc = {X.File, 1, 1};
  Fn.CC = CallConv::Go;
  ScopeId Cur = 0;
  for (unsigned I = 0; I < 2 * N; ++I) {
    bool Nest = Mode == 1 || (Mode == 0 && I < N);
    if (Mode == 2 && I >= N)
      break;
    B.setScope(Nest ? Cur : 0);
    ScopeId S = B.newScope();
    Fn.Scopes[S - 1].Loc = {X.File, 2 + I, 1};
    B.setScope(S);
    if (Nest)
      Cur = S;
    LocalId V = B.newLocal(X.I64, Local::Var, "v" + std::to_string(I));
    Fn.Locals[V].Source = true;
    Fn.Locals[V].Loc = {X.File, 2 + I, 2};
    B.assign(B.local(V), B.use(constInt(X.I64, I)));
  }
  B.setScope(0);
  B.ret();
}

TEST(DebugInfoStress, ThousandsOfNestedAndSiblingScopesAreOneBlockEach) {
  for (const char *Target : {"linux/amd64", "linux/arm64"}) {
    SCOPED_TRACE(Target);
    Mini X;
    manyScopes(X, 3000);
    ASSERT_EQ(test::errorsOf(X.M), "");
    Lowered L(X, Target);
    ASSERT_TRUE(L.Ok);
    // Every sibling has a variable: a lexical block each. The chain of 3000
    // nested ones stops at MaxBlockDepth (LLVM walks the chain of scopes for
    // each location, so a deeper one would be quadratic); every variable is
    // still there, the deep ones in the deepest block.
    EXPECT_EQ(blocksOf(*L.LM).size(), 3000u + DebugInfoBuilder::MaxBlockDepth);
    EXPECT_EQ(deepestChain(*L.LM), DebugInfoBuilder::MaxBlockDepth);
    EXPECT_EQ(L.vars("scopes").size(), 6000u);
    EXPECT_FALSE(llvm::verifyModule(*L.LM, &llvm::errs()));
  }
}

TEST(DebugInfoStress, NestingBelowTheLimitKeepsEveryBlock) {
  // The other side of the cap: 40 nested blocks are 40 lexical blocks.
  Mini X;
  manyScopes(X, 20, /*Mode=*/1);
  Lowered L(X);
  ASSERT_TRUE(L.Ok);
  EXPECT_EQ(blocksOf(*L.LM).size(), 40u);
  EXPECT_EQ(deepestChain(*L.LM), 40u);
}

/// A function "f" whose blocks are given as (parent, variable name) pairs:
/// block I + 1 lies in block Parent (0: the function) and holds a source
/// variable of that name.
void blocksWithNames(Mini &X,
                     llvm::ArrayRef<std::pair<unsigned, std::string>> Blocks) {
  using namespace gir;
  FuncId F = test::declare(X.M, "f", X.C.func({}, X.C.unit()));
  FuncBuilder B(X.M, X.M.func(F));
  Function &Fn = X.M.func(F);
  Fn.Loc = {X.File, 1, 1};
  Fn.CC = CallConv::Go;
  for (unsigned I = 0; I < Blocks.size(); ++I) {
    B.setScope(Blocks[I].first);
    ScopeId S = B.newScope();
    EXPECT_EQ(S, I + 1);
    Fn.Scopes[S - 1].Loc = {X.File, 2 + I, 1};
    B.setScope(S);
    LocalId V = B.newLocal(X.I64, Local::Var, Blocks[I].second);
    Fn.Locals[V].Source = true;
    Fn.Locals[V].Loc = {X.File, 2 + I, 2};
    B.assign(B.local(V), B.use(constInt(X.I64, I)));
  }
  B.setScope(0);
  B.ret();
  EXPECT_EQ(test::errorsOf(X.M), "");
}

/// The variables of F by name: how many there are, and how many of them have
/// a location (a dbg.declare).
std::map<std::string, std::pair<unsigned, unsigned>>
locatedVariables(const llvm::Function &F) {
  std::map<std::string, std::pair<unsigned, unsigned>> Out;
  for (const llvm::MDNode *N : F.getSubprogram()->getRetainedNodes())
    if (auto *V = llvm::dyn_cast<llvm::DILocalVariable>(N))
      ++Out[V->getName().str()].first;
  std::set<const llvm::DILocalVariable *> Located;
  for (const llvm::BasicBlock &BB : F)
    for (const llvm::Instruction &I : BB)
      for (const llvm::DbgRecord &R : I.getDbgRecordRange())
        if (auto *DV = llvm::dyn_cast<llvm::DbgVariableRecord>(&R))
          Located.insert(DV->getVariable());
  for (const llvm::DILocalVariable *V : Located)
    ++Out[V->getName().str()].second;
  return Out;
}

TEST(DebugInfoStress, PastTheBlockLimitASharedNameHasNoLocation) {
  // 70 nested blocks that each declare `x` (`{ x := x + 1`): past the 64th
  // they share its lexical block, where a debugger would take any of the
  // seven `x` for the innermost: those have no location (`<optimized out>`,
  // never a wrong value), the 63 above them keep theirs. A name of its own
  // in the shared block (`w`, `u`) is still shown.
  std::vector<std::pair<unsigned, std::string>> Nested;
  for (unsigned I = 0; I < 70; ++I)
    Nested.push_back({I, "x"});
  Nested.push_back({70, "w"});
  Nested.push_back({66, "u"});
  // Two sibling blocks below the 64th that both declare `y`, and `z` in one.
  Nested.push_back({64, "y"});
  Nested.push_back({64, "y"});
  Nested.push_back({74, "z"});
  Mini X;
  blocksWithNames(X, Nested);
  Lowered L(X);
  ASSERT_TRUE(L.Ok);
  auto V = locatedVariables(*L.LM->getFunction("f"));
  EXPECT_EQ(V["x"], std::make_pair(70u, 63u));
  EXPECT_EQ(V["w"], std::make_pair(1u, 1u));
  EXPECT_EQ(V["u"], std::make_pair(1u, 1u));
  EXPECT_EQ(V["y"], std::make_pair(2u, 0u));
  EXPECT_EQ(V["z"], std::make_pair(1u, 1u));
  EXPECT_EQ(deepestChain(*L.LM), DebugInfoBuilder::MaxBlockDepth);
  EXPECT_FALSE(llvm::verifyModule(*L.LM, &llvm::errs()));
  // The other side: below the limit every `x` has its block and location.
  Mini Y;
  blocksWithNames(Y, std::vector<std::pair<unsigned, std::string>>(
                         Nested.begin(), Nested.begin() + 40));
  Lowered LY(Y);
  ASSERT_TRUE(LY.Ok);
  EXPECT_EQ(locatedVariables(*LY.LM->getFunction("f"))["x"],
            std::make_pair(40u, 40u));
}

TEST(DebugInfoStress, ScopesWithoutVariablesCollapseIntoTheirParent) {
  // A scope with no source variable makes no block (a debugger has nothing to
  // show in it), however many there are.
  using namespace gir;
  Mini X;
  FuncId F = test::declare(X.M, "empty", X.C.func({}, X.C.unit()));
  FuncBuilder B(X.M, X.M.func(F));
  X.M.func(F).Loc = {X.File, 1, 1};
  X.M.func(F).CC = CallConv::Go;
  for (unsigned I = 0; I < 100; ++I) {
    B.setScope(B.newScope());
    LocalId T = B.newTemp(X.I64);
    B.assign(B.local(T), B.use(constInt(X.I64, I)));
  }
  B.setScope(0);
  B.ret();
  Lowered L(X);
  ASSERT_TRUE(L.Ok);
  EXPECT_TRUE(blocksOf(*L.LM).empty());
}

TEST(DebugInfoStress, ScopesAreLinear) {
  auto Steps = [](unsigned N) {
    Mini X;
    manyScopes(X, N);
    Lowered L(X);
    EXPECT_TRUE(L.Ok);
    return L.Counts.DIScopeSteps + L.Counts.DILocationSteps +
           L.Counts.DIVariableSteps;
  };
  EXPECT_TRUE(stepsAreLinear(Steps(500), Steps(2000), 500));
}

/// A chain of N types: T0 holds a pointer to T1, and so on (Arrays: N arrays
/// nested in each other instead).
gir::Type *chainOfTypes(Mini &X, unsigned N, bool Arrays) {
  if (Arrays) {
    gir::Type *Cur = X.I64;
    for (unsigned I = 0; I < N; ++I)
      Cur = X.C.array(Cur, 1);
    return Cur;
  }
  std::vector<gir::Type *> T(N);
  for (unsigned I = 0; I < N; ++I)
    T[I] = X.C.structTy("main.T" + std::to_string(I));
  for (unsigned I = 0; I < N; ++I) {
    gir::Type *Next = I + 1 < N ? X.C.ptr(T[I + 1]) : X.I64;
    EXPECT_TRUE(X.C.setStructBody(T[I], {Next, X.I64}));
  }
  return T[0];
}

TEST(DebugInfoStress, AChainOfNestedTypesIsCutAndTheWriterFinishes) {
  // LLVM's DWARF writer recurses once per level of a type: a chain of 4000
  // is made of types at most MaxTypeHeight high, whose members refer to the
  // rest by declarations, and the whole module emits.
  for (bool Arrays : {false, true}) {
    SCOPED_TRACE(Arrays ? "arrays" : "structs through pointers");
    Mini X;
    withVar(X, "f", X.I64, chainOfTypes(X, 4000, Arrays));
    ASSERT_EQ(test::errorsOf(X.M), "");
    Lowered L(X);
    ASSERT_TRUE(L.Ok);
    int H = heightOf(L.vars("f")["v"]->getType());
    EXPECT_GT(H, 0);
    EXPECT_LE(H, int(DebugInfoBuilder::MaxTypeHeight) + 1);
    // A struct that holds a chain of arrays where a part would be too high
    // adds two levels to it: its member and itself.
    for (const llvm::DICompileUnit *CU : L.LM->debug_compile_units())
      for (const llvm::DIScope *S : CU->getRetainedTypes())
        EXPECT_LE(heightOf(llvm::cast<llvm::DIType>(S)),
                  int(DebugInfoBuilder::MaxTypeHeight) + 2);
    EXPECT_NE(L.emit(), "");
    EXPECT_FALSE(llvm::verifyModule(*L.LM, &llvm::errs()));
  }
}

/// N types in a ladder of diamonds: Ti holds two pointers to Ti+1, so there
/// are 2^N paths from T0 to TN (a cut that depends on the path would make the
/// work exponential).
gir::Type *diamondLadder(Mini &X, unsigned N) {
  std::vector<gir::Type *> T(N + 1);
  for (unsigned I = 0; I <= N; ++I)
    T[I] = X.C.structTy("main.D" + std::to_string(I));
  for (unsigned I = 0; I < N; ++I)
    EXPECT_TRUE(
        X.C.setStructBody(T[I], {X.C.ptr(T[I + 1]), X.C.ptr(T[I + 1]), X.I64}));
  EXPECT_TRUE(X.C.setStructBody(T[N], {X.I64}));
  return T[0];
}

/// N types in one cycle: Ti points to Ti+1, and the last one to T0.
gir::Type *cycleOfTypes(Mini &X, unsigned N) {
  std::vector<gir::Type *> T(N);
  for (unsigned I = 0; I < N; ++I)
    T[I] = X.C.structTy("main.C" + std::to_string(I));
  for (unsigned I = 0; I < N; ++I)
    EXPECT_TRUE(X.C.setStructBody(T[I], {X.C.ptr(T[(I + 1) % N]), X.I64}));
  return T[0];
}

uint64_t typeSteps(gir::Type *(*Make)(Mini &, unsigned), unsigned N,
                   int *Height = nullptr) {
  Mini X;
  withVar(X, "f", X.I64, Make(X, N));
  Lowered L(X);
  EXPECT_TRUE(L.Ok);
  if (Height)
    *Height = heightOf(L.vars("f")["v"]->getType());
  return L.Counts.DITypeSteps;
}

TEST(DebugInfoStress, ADiamondLadderOfTypesIsLinear) {
  int H = 0;
  uint64_t S1 = typeSteps(diamondLadder, 100, &H),
           S4 = typeSteps(diamondLadder, 400);
  EXPECT_TRUE(stepsAreLinear(S1, S4, 100));
  EXPECT_GT(H, 0);
  EXPECT_LE(H, int(DebugInfoBuilder::MaxTypeHeight) + 1);
}

TEST(DebugInfoStress, ACycleOfThousandsOfTypesIsCutAndLinear) {
  int H = 0;
  uint64_t S1 = typeSteps(cycleOfTypes, 1000, &H),
           S4 = typeSteps(cycleOfTypes, 4000);
  EXPECT_TRUE(stepsAreLinear(S1, S4, 1000));
  EXPECT_GT(H, 0); // the references form no cycle
  EXPECT_LE(H, int(DebugInfoBuilder::MaxTypeHeight) + 1);
  Mini X;
  withVar(X, "f", X.I64, cycleOfTypes(X, 3000));
  Lowered L(X);
  ASSERT_TRUE(L.Ok);
  EXPECT_NE(L.emit(), "");
}

/// The N types of a chain of one shape: 0: structs through pointers, 1:
/// pointers to pointers (no struct to name), 2: arrays of arrays, 3: structs
/// that hold an array of the one before by value, 4: pointers and arrays in
/// turn.
std::vector<gir::Type *> chainOf(Mini &X, unsigned N, int Shape) {
  gir::Context &C = X.C;
  std::vector<gir::Type *> T;
  gir::Type *Cur = C.structTy("main.Base");
  EXPECT_TRUE(C.setStructBody(Cur, {X.I64}));
  for (unsigned I = 0; I < N; ++I) {
    switch (Shape) {
    case 0:
    case 3: {
      gir::Type *S = C.structTy("main.C" + std::to_string(I));
      EXPECT_TRUE(C.setStructBody(
          S, {Shape == 0 ? C.ptr(Cur) : C.array(Cur, 2), X.I64}));
      Cur = S;
      break;
    }
    case 1:
      Cur = C.ptr(Cur);
      break;
    case 2:
      Cur = C.array(Cur, 1);
      break;
    default:
      Cur = I % 2 ? C.ptr(Cur) : C.array(Cur, 1);
      break;
    }
    T.push_back(Cur);
  }
  return T;
}

/// The type steps to make a variable of every type of a chain, asked for from
/// the top or from the bottom.
uint64_t chainSteps(unsigned N, int Shape, bool TopFirst) {
  using namespace gir;
  Mini X;
  std::vector<Type *> T = chainOf(X, N, Shape);
  if (TopFirst)
    std::reverse(T.begin(), T.end());
  FuncId F = test::declare(X.M, "f", X.C.func({}, X.C.unit()));
  FuncBuilder B(X.M, X.M.func(F));
  X.M.func(F).Loc = {X.File, 1, 1};
  X.M.func(F).CC = CallConv::Go;
  for (unsigned I = 0; I < T.size(); ++I) {
    LocalId V = B.newLocal(T[I], Local::Var, "v" + std::to_string(I));
    X.M.func(F).Locals[V].Source = true;
    X.M.func(F).Locals[V].Loc = {X.File, 2, 2};
  }
  B.ret();
  Lowered L(X);
  EXPECT_TRUE(L.Ok);
  return L.Counts.DITypeSteps;
}

TEST(DebugInfoStress, ChainsOfPointersAndArraysAreLinear) {
  // Every type of the chain is a variable (so each is asked for), from the
  // top or the bottom: the steps grow with N, never with N times the height.
  for (int Shape = 0; Shape < 5; ++Shape)
    for (bool TopFirst : {false, true}) {
      SCOPED_TRACE(std::to_string(Shape) + (TopFirst ? " top" : " bottom"));
      EXPECT_TRUE(stepsAreLinear(chainSteps(500, Shape, TopFirst),
                                 chainSteps(2000, Shape, TopFirst), 500));
    }
}

TEST(DebugInfoStress, ManySiblingTypesAreMadeOnceEach) {
  // 5000 variables of 50 struct types: 50 composite types.
  using namespace gir;
  Mini X;
  std::vector<Type *> T;
  for (unsigned I = 0; I < 50; ++I) {
    Type *S = X.C.structTy("main.S" + std::to_string(I));
    ASSERT_TRUE(X.C.setStructBody(S, {X.I64, X.C.string()}));
    T.push_back(S);
  }
  FuncId F = test::declare(X.M, "many", X.C.func({}, X.C.unit()));
  FuncBuilder B(X.M, X.M.func(F));
  X.M.func(F).Loc = {X.File, 1, 1};
  X.M.func(F).CC = CallConv::Go;
  for (unsigned I = 0; I < 5000; ++I) {
    LocalId V = B.newLocal(T[I % 50], Local::Var, "v" + std::to_string(I));
    X.M.func(F).Locals[V].Source = true;
    X.M.func(F).Locals[V].Loc = {X.File, 2, 2};
  }
  B.ret();
  Lowered L(X);
  ASSERT_TRUE(L.Ok);
  std::set<const llvm::DIType *> Types;
  for (auto &[Name, V] : L.vars("many"))
    Types.insert(V->getType());
  EXPECT_EQ(Types.size(), 50u);
}

} // namespace
