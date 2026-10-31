//===- AllocatorTest.cpp - A C++ allocator object behind a slice's word ---===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.25 (ADR-0029, P05 D5.3). The allocator word of an owned slice or a map
// header points to an allocator object that starts with its function table
// (alloc, resize, free), inline, then the allocator's own state (gir.md
// section 15). The run tests (tests/run/core-allocword*.test) prove that the
// slice and map runtime goes through the table, with counting allocators the
// compiler writes itself. This test proves the SHAPE P05.03 relies on: an
// allocator written OUTSIDE the compiler, here a C++ struct with three plain
// function pointers, works behind a slice and a map without a change to the
// slice or map runtime. A Go++ program is checked, lowered with the test
// intrinsics (`gopp build -test-intrinsics`), run in LLJIT, and makes its
// slice and map in the object a C function of the test returns.
//
// Both sides: the call counts the C++ allocator sees are exact (growth
// allocates there, the free routines free there), every block comes back
// with the size it was made with and no size is 0 (the contract of
// gopp.mem_resize_in), a block it hands out is the block the program uses
// (the program reads back what it wrote), and a program that never names the
// object does not touch it.
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/SourceManager.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/GIR/Lower.h"
#include "gopp/Sema/Loader.h"

#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"

#include <gtest/gtest.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#define GOPP_LSAN 1
#endif
#endif

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>

using namespace gopp;

namespace {

/// The allocator object: the table first (P05 D5.3), then the state.
struct CppAllocator {
  void *(*Alloc)(CppAllocator *Self, uint64_t Size, bool Zero);
  void *(*Resize)(CppAllocator *Self, void *P, uint64_t Old, uint64_t Size,
                  bool Zero);
  void (*Free)(CppAllocator *Self, void *P, uint64_t Size);
  // state
  unsigned Allocs, Resizes, Frees;
  /// Calls that broke the contract: a size of 0, or a free or a resize of a
  /// block this allocator did not make or with a size it was not made with.
  unsigned Bad;
};
static_assert(offsetof(CppAllocator, Alloc) == 0 &&
                  offsetof(CppAllocator, Resize) == 8 &&
                  offsetof(CppAllocator, Free) == 16 &&
                  offsetof(CppAllocator, Allocs) == 24,
              "the table is inline at offset 0, the state after it");

CppAllocator *TheAllocator = nullptr;
/// The blocks the allocator has handed out and not taken back, and their
/// sizes.
std::map<void *, uint64_t> Live;

void *cppAlloc(CppAllocator *Self, uint64_t Size, bool Zero) {
  ++Self->Allocs;
  Self->Bad += Size == 0;
  void *P = Zero ? std::calloc(1, Size) : std::malloc(Size);
  Live[P] = Size;
  return P;
}
void *cppResize(CppAllocator *Self, void *P, uint64_t Old, uint64_t Size,
                bool Zero) {
  ++Self->Resizes;
  auto It = Live.find(P);
  Self->Bad += Size == 0 || It == Live.end() || It->second != Old;
  if (It != Live.end())
    Live.erase(It);
  auto *Q = static_cast<char *>(std::realloc(P, Size));
  if (Zero && Size > Old)
    std::memset(Q + Old, 0, Size - Old);
  Live[Q] = Size;
  return Q;
}
void cppFree(CppAllocator *Self, void *P, uint64_t Size) {
  ++Self->Frees;
  auto It = Live.find(P);
  Self->Bad += It == Live.end() || It->second != Size;
  if (It != Live.end())
    Live.erase(It);
  std::free(P);
}

unsigned Failures = 0;

extern "C" void *gopp_unit_cpp_allocator() { return TheAllocator; }
extern "C" void gopp_unit_expect(bool Ok) { Failures += !Ok; }

struct Ran {
  bool Ok = false;
  std::string Errors;
};

/// Checks, lowers with the test intrinsics (host target) and runs the program
/// Source (its C `main`) in LLJIT, with `gopp_unit_cpp_allocator` bound to
/// the test's C++ allocator.
Ran runProgram(const std::string &Source) {
  Ran R;
  llvm::SmallString<128> D;
  EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-alloc", D));
  std::string Dir(D);
  {
    std::ofstream Out(Dir + "/main.gopp", std::ios::binary);
    Out << Source;
  }
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags(SM, Collect, 0);
  sema::TypeContext TCtx;
  sema::LoadOptions LO;
  LO.Root = Dir;
  sema::Loader L(SM, Diags, TCtx, LO, {});
  sema::CheckedPackage *CP = L.loadDir(Dir, "");
  gir::Context GC;
  std::unique_ptr<gir::Module> GM;
  if (CP && !Diags.hasErrors()) {
    gir::LowerOptions Opts;
    Opts.TestIntrinsics = true;
    GM = gir::lowerPackage(GC, SM, *CP, L.filesOf(CP), Diags, Opts);
  }
  for (const Diagnostic &Dg : Collect.Diags)
    R.Errors += Dg.Message + "\n";
  llvm::sys::fs::remove_directories(Dir);
  if (!GM)
    return R;
  initializeTargets();
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  auto Spec = hostTarget();
  if (!Spec) {
    R.Errors += llvm::toString(Spec.takeError());
    return R;
  }
  auto TM = createTargetMachine(*Spec);
  if (!TM) {
    R.Errors += llvm::toString(TM.takeError());
    return R;
  }
  auto Ctx = std::make_unique<llvm::LLVMContext>();
  auto LM = lowerToLLVM(*Ctx, *GM, **TM);
  if (!LM) {
    R.Errors += llvm::toString(LM.takeError());
    return R;
  }
  // The program is entered as C enters it: the C `main`, which runs the
  // package's init and main.main. The test calls it through a C function
  // pointer, so it must have the C convention (main.main itself is a Go++
  // function, P04.18, and cannot be called that way).
  const char *Entry = "main";
  llvm::Function *EntryFn = (*LM)->getFunction(Entry);
  if (!EntryFn || EntryFn->getCallingConv() != llvm::CallingConv::C) {
    R.Errors += std::string("no C entry point ") + Entry + "\n";
    return R;
  }
  auto Made = llvm::orc::LLJITBuilder().create();
  if (!Made) {
    R.Errors += llvm::toString(Made.takeError());
    return R;
  }
  auto &J = *Made;
  auto Gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
      J->getDataLayout().getGlobalPrefix());
  if (!Gen) {
    R.Errors += llvm::toString(Gen.takeError());
    return R;
  }
  llvm::orc::JITDylib &JD = J->getMainJITDylib();
  JD.addGenerator(std::move(*Gen));
  llvm::orc::SymbolMap Own;
  Own[J->mangleAndIntern("gopp_unit_cpp_allocator")] = {
      llvm::orc::ExecutorAddr::fromPtr(&gopp_unit_cpp_allocator),
      llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
  Own[J->mangleAndIntern("gopp_unit_expect")] = {
      llvm::orc::ExecutorAddr::fromPtr(&gopp_unit_expect),
      llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
  if (llvm::Error E = JD.define(llvm::orc::absoluteSymbols(std::move(Own)))) {
    R.Errors += llvm::toString(std::move(E));
    return R;
  }
  if (llvm::Error E = J->addIRModule(
          llvm::orc::ThreadSafeModule(std::move(*LM), std::move(Ctx)))) {
    R.Errors += llvm::toString(std::move(E));
    return R;
  }
  auto Main = J->lookup(Entry);
  if (!Main) {
    R.Errors += llvm::toString(Main.takeError());
    return R;
  }
  // The program's runtime leaks what it allocates until P04.09 drops values
  // (gir.md section 15): LeakSanitizer must not fail the test for it.
#ifdef GOPP_LSAN
  __lsan_disable();
#endif
  EXPECT_EQ(Main->toPtr<int (*)()>()(), 0);
#ifdef GOPP_LSAN
  __lsan_enable();
#endif
  R.Ok = true;
  return R;
}

// The intrinsics take the allocator object itself where the run tests pass
// `which` (LowerTest.cpp).
constexpr const char *Decls = R"(package main

@Extern("C") func gopp_unit_cpp_allocator() *byte
@Extern("C") func gopp_unit_expect(ok bool)
@Extern("C") func gopp_test_make_slice(a *byte, n int, c int) []int
@Extern("C") func gopp_test_make_slice_z(a *byte, n int, c int) []struct{}
@Extern("C") func gopp_test_clone_slice(s []int) []int
@Extern("C") func gopp_test_free_slice(s []int)
@Extern("C") func gopp_test_free_slice_z(s []struct{})
@Extern("C") func gopp_test_make_map(a *byte) map[int]int
@Extern("C") func gopp_test_free_map(m map[int]int)
)";

struct Fixture {
  CppAllocator A{};
  Fixture() {
    Failures = 0;
    Live.clear();
    A.Alloc = cppAlloc;
    A.Resize = cppResize;
    A.Free = cppFree;
    TheAllocator = &A;
  }
  ~Fixture() {
    // The old buffers append leaves behind until P04.09 drops them.
    for (auto &[P, Size] : Live)
      std::free(P);
    Live.clear();
    TheAllocator = nullptr;
  }
};

TEST(CppAllocator, ASliceGrowsClonesAndIsFreedThroughTheTable) {
  Fixture F;
  std::string Src = std::string(Decls) + R"(
func main() {
	a := gopp_unit_cpp_allocator()
	s := gopp_test_make_slice(a, 0, 1) // 1 allocation
	for i := 0; i < 100; i++ {
		s = append(s, i) // grows: 2, 4, 8, ... 128
	}
	t := gopp_test_clone_slice(s) // 1 allocation, same allocator
	gopp_unit_expect(len(t) == 100 && t[99] == 99 && cap(s) == 128)
	gopp_test_free_slice(t)
	gopp_test_free_slice(s)
	// Size 0 reaches the allocator as one byte, and so does its free.
	e := gopp_test_make_slice(a, 0, 0) // 1 allocation
	gopp_test_free_slice(e)
	z := gopp_test_make_slice_z(a, 0, 0) // 1 allocation
	for i := 0; i < 3; i++ {
		z = append(z, struct{}{}) // zero-size elements: 1 allocation each
	}
	gopp_unit_expect(len(z) == 3)
	gopp_test_free_slice_z(z)
}
)";
  Ran R = runProgram(Src);
  ASSERT_TRUE(R.Ok) << R.Errors;
  EXPECT_EQ(Failures, 0u) << "the program's own checks";
  // cap 1 -> 2 -> 4 -> 8 -> 16 -> 32 -> 64 -> 128: seven growths. (gc's size
  // classes round the capacities the same way for 8-byte elements.)
  EXPECT_EQ(F.A.Allocs, 1u + 7u + 1u + 1u + 1u + 3u);
  EXPECT_EQ(F.A.Frees, 4u);
  EXPECT_EQ(F.A.Resizes, 0u); // append allocates and copies; it never resizes
  EXPECT_EQ(F.A.Bad, 0u) << "a size of 0, or a free of the wrong size";
}

TEST(CppAllocator, AMapAllocatesItsHeaderAndTablesThereAndFreesThem) {
  Fixture F;
  std::string Src = std::string(Decls) + R"(
func main() {
	a := gopp_unit_cpp_allocator()
	m := gopp_test_make_map(a) // the header
	for i := 0; i < 100; i++ {
		m[i] = i // grows the entries and the index
	}
	gopp_unit_expect(len(m) == 100 && m[57] == 57)
	gopp_test_free_map(m)
}
)";
  Ran R = runProgram(Src);
  ASSERT_TRUE(R.Ok) << R.Errors;
  EXPECT_EQ(Failures, 0u) << "the program's own checks";
  // The header, then two tables at each of the growths 8, 16, 32, 64, 128.
  EXPECT_EQ(F.A.Allocs, 1u + 2u * 5u);
  // The tables dropped by the four growths after the first, then the last
  // two tables and the header.
  EXPECT_EQ(F.A.Frees, 2u * 4u + 3u);
  EXPECT_EQ(F.A.Bad, 0u) << "a size of 0, or a free of the wrong size";
  EXPECT_TRUE(Live.empty()) << Live.size() << " blocks never came back";
}

TEST(CppAllocator, AProgramThatNeverNamesItLeavesItAlone) {
  Fixture F;
  std::string Src = std::string(Decls) + R"(
func main() {
	var s []int
	for i := 0; i < 100; i++ {
		s = append(s, i)
	}
	m := make(map[int]int)
	m[1] = 2
	t := gopp_test_clone_slice(s)
	gopp_test_free_slice(t)
	gopp_test_free_map(m)
	_ = gopp_unit_cpp_allocator() // the object exists, no slice uses it
}
)";
  Ran R = runProgram(Src);
  ASSERT_TRUE(R.Ok) << R.Errors;
  EXPECT_EQ(F.A.Allocs, 0u);
  EXPECT_EQ(F.A.Frees, 0u);
  EXPECT_EQ(F.A.Resizes, 0u);
}

} // namespace
