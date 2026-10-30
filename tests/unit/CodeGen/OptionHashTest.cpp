//===- OptionHashTest.cpp - The hash glue of Option[T] values -------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.04 (ADR-0018 amendment, point 1; D4.5). Equal Option values hash
// equal: the padding of a set value, and the payload of a nil one, are never
// read. A program declares `var g Option[...]`; the lowering gives g the
// D4.5 layout of its checked type, the hash glue of that GIR type
// (gir::addHashGlue, the one P05's maps will call) is lowered for the host,
// compiled in LLJIT and called on buffers whose padding bytes are filled
// with garbage. Both sides: the same value in different garbage hashes the
// same, and different values (and set against nil) hash differently.
// GlueTest.cpp tests the glue on hand-made GIR types; this test on the types
// the checker and lowerType make.
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/SourceManager.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/GIR/Glue.h"
#include "gopp/GIR/Lower.h"
#include "gopp/Sema/Loader.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <functional>

using namespace gopp;

namespace {

using HashFn = uint64_t (*)(const void *, uint64_t);

/// Checks and lowers a program that declares `var g Option[...]`, adds the
/// hash glue of g's GIR type as `hash_g`, and compiles the module in a JIT
/// that lives as long as this object. Fn is null (and Error says why) when
/// the type has no glue or something failed.
struct Jit {
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  std::unique_ptr<DiagnosticsEngine> Diags;
  sema::TypeContext TCtx;
  gir::Context GC;
  std::unique_ptr<llvm::orc::LLJIT> J;
  HashFn Fn = nullptr;
  std::string Error;

  explicit Jit(const std::string &Decl) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::SmallString<128> Dir;
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-hash", Dir));
    {
      std::ofstream Out(std::string(Dir) + "/main.gopp", std::ios::binary);
      Out << "package main\n\ntype Pt struct {\n\tx int8\n\ty int64\n}\n\n" +
                 Decl + "\n\nfunc main() {}\n";
    }
    Diags = std::make_unique<DiagnosticsEngine>(SM, Collect, 0);
    sema::LoadOptions LO;
    LO.Root = std::string(Dir);
    sema::Loader L(SM, *Diags, TCtx, LO, {});
    sema::CheckedPackage *CP = L.loadDir(std::string(Dir), "");
    llvm::sys::fs::remove_directories(Dir);
    if (!CP || Diags->hasErrors()) {
      for (const Diagnostic &D : Collect.Diags)
        Error += D.Message + "\n";
      return;
    }
    auto GM = gir::lowerPackage(GC, SM, *CP, L.filesOf(CP), *Diags);
    if (!GM || GM->Globals.empty()) {
      Error = "lowering failed";
      return;
    }
    gir::Type *T = GM->Globals[0].Ty;
    if (gir::addHashGlue(*GM, T, "hash_g") == gir::NoId) {
      Error = "no hash glue for " + T->str();
      return;
    }
    auto Spec = hostTarget();
    if (!Spec) {
      Error = toString(Spec.takeError());
      return;
    }
    auto TM = createTargetMachine(*Spec);
    if (!TM) {
      Error = toString(TM.takeError());
      return;
    }
    auto Ctx = std::make_unique<llvm::LLVMContext>();
    auto LM = lowerToLLVM(*Ctx, *GM, **TM);
    if (!LM) {
      Error = toString(LM.takeError());
      return;
    }
    // The program's own entry point and init function stay out of the JIT's
    // way: only the glue is looked up.
    auto Made = llvm::orc::LLJITBuilder().create();
    if (!Made) {
      Error = toString(Made.takeError());
      return;
    }
    J = std::move(*Made);
    if (llvm::Error E = J->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(*LM), std::move(Ctx)))) {
      Error = toString(std::move(E));
      return;
    }
    auto Addr = J->lookup("hash_g");
    if (!Addr) {
      Error = toString(Addr.takeError());
      return;
    }
    Fn = Addr->toPtr<HashFn>();
  }
};

/// A buffer filled with Fill, then written by Set.
struct Buf {
  alignas(16) unsigned char B[64];
  Buf(unsigned char Fill, const std::function<void(unsigned char *)> &Set) {
    std::memset(B, Fill, sizeof B);
    Set(B);
  }
};

template <typename T> void put(unsigned char *P, size_t Off, T V) {
  std::memcpy(P + Off, &V, sizeof V);
}

TEST(OptionHash, Int32PaddingAndNilPayloadAreIgnored) {
  // Option[int32] is {int32 at 0, flag at 4}, 8 bytes (D4.5).
  Jit J("var g Option[int32]");
  ASSERT_NE(J.Fn, nullptr) << J.Error;
  auto Set = [](int32_t V) {
    return [V](unsigned char *P) {
      put<int32_t>(P, 0, V);
      put<uint8_t>(P, 4, 1);
    };
  };
  auto Nil = [](int32_t Payload) {
    return [Payload](unsigned char *P) {
      put<int32_t>(P, 0, Payload);
      put<uint8_t>(P, 4, 0);
    };
  };
  // Same value, padding bytes 5..7 different.
  uint64_t H = J.Fn(Buf(0x00, Set(7)).B, 1);
  EXPECT_EQ(J.Fn(Buf(0xAA, Set(7)).B, 1), H);
  EXPECT_EQ(J.Fn(Buf(0x55, Set(7)).B, 1), H);
  // Nil: the payload is never read.
  uint64_t N = J.Fn(Buf(0x00, Nil(0)).B, 1);
  EXPECT_EQ(J.Fn(Buf(0xFF, Nil(12345)).B, 1), N);
  EXPECT_EQ(J.Fn(Buf(0x11, Nil(-1)).B, 1), N);
  // And the other side: different values, and nil against set, differ.
  EXPECT_NE(J.Fn(Buf(0, Set(8)).B, 1), H);
  EXPECT_NE(J.Fn(Buf(0, Set(0)).B, 1), N);
  EXPECT_NE(H, N);
}

TEST(OptionHash, StructPayloadIgnoresItsOwnPadding) {
  // Option[Pt]: Pt is {int8 at 0, 7 bytes of padding, int64 at 8}, the flag
  // is at 16, and bytes 17..23 are padding.
  Jit J("var g Option[Pt]");
  ASSERT_NE(J.Fn, nullptr) << J.Error;
  auto Set = [](int8_t X, int64_t Y) {
    return [X, Y](unsigned char *P) {
      put<int8_t>(P, 0, X);
      put<int64_t>(P, 8, Y);
      put<uint8_t>(P, 16, 1);
    };
  };
  uint64_t H = J.Fn(Buf(0x00, Set(1, 2)).B, 1);
  EXPECT_EQ(J.Fn(Buf(0xEE, Set(1, 2)).B, 1), H);
  EXPECT_EQ(J.Fn(Buf(0x37, Set(1, 2)).B, 1), H);
  EXPECT_NE(J.Fn(Buf(0, Set(2, 2)).B, 1), H);
  EXPECT_NE(J.Fn(Buf(0, Set(1, 3)).B, 1), H);
  auto Nil = [](unsigned char *P) { put<uint8_t>(P, 16, 0); };
  uint64_t N = J.Fn(Buf(0x00, Nil).B, 1);
  EXPECT_EQ(J.Fn(Buf(0xCC, Nil).B, 1), N);
  EXPECT_NE(H, N);
}

TEST(OptionHash, PointerUsesItsNilBitPattern) {
  // Option[*int32] is a pointer: nil is 0 (the size of a pointer, D4.5).
  Jit J("var g Option[*int32]");
  ASSERT_NE(J.Fn, nullptr) << J.Error;
  auto At = [](uint64_t V) {
    return [V](unsigned char *P) { put<uint64_t>(P, 0, V); };
  };
  uint64_t H = J.Fn(Buf(0xAA, At(0x1000)).B, 1);
  EXPECT_EQ(J.Fn(Buf(0x00, At(0x1000)).B, 1), H);
  EXPECT_NE(J.Fn(Buf(0, At(0x1008)).B, 1), H);
  EXPECT_NE(J.Fn(Buf(0, At(0)).B, 1), H);
  EXPECT_EQ(J.Fn(Buf(0xFF, At(0)).B, 1), J.Fn(Buf(0x00, At(0)).B, 1));
}

TEST(OptionHash, BoolAndArrayPayloads) {
  Jit B("var g Option[bool]");
  ASSERT_NE(B.Fn, nullptr) << B.Error;
  // {bool at 0, flag at 1}: a payload bool is hashed as 0 or 1.
  auto Bv = [](uint8_t V, uint8_t Flag) {
    return [V, Flag](unsigned char *P) {
      put<uint8_t>(P, 0, V);
      put<uint8_t>(P, 1, Flag);
    };
  };
  EXPECT_EQ(B.Fn(Buf(0x00, Bv(1, 1)).B, 1), B.Fn(Buf(0xFF, Bv(1, 1)).B, 1));
  EXPECT_NE(B.Fn(Buf(0, Bv(1, 1)).B, 1), B.Fn(Buf(0, Bv(0, 1)).B, 1));
  EXPECT_NE(B.Fn(Buf(0, Bv(0, 1)).B, 1), B.Fn(Buf(0, Bv(0, 0)).B, 1));
  EXPECT_EQ(B.Fn(Buf(0, Bv(0, 0)).B, 1), B.Fn(Buf(0, Bv(1, 0)).B, 1));

  Jit A("var g Option[[3]int16]");
  ASSERT_NE(A.Fn, nullptr) << A.Error;
  // {[3]int16 at 0..5, flag at 6}, 8 bytes.
  auto Av = [](int16_t X, int16_t Y, int16_t Z) {
    return [X, Y, Z](unsigned char *P) {
      put<int16_t>(P, 0, X);
      put<int16_t>(P, 2, Y);
      put<int16_t>(P, 4, Z);
      put<uint8_t>(P, 6, 1);
    };
  };
  uint64_t H = A.Fn(Buf(0, Av(1, 2, 3)).B, 1);
  EXPECT_EQ(A.Fn(Buf(0x99, Av(1, 2, 3)).B, 1), H);
  EXPECT_NE(A.Fn(Buf(0, Av(1, 2, 4)).B, 1), H);
  EXPECT_NE(A.Fn(Buf(0, Av(3, 2, 1)).B, 1), H);
}

TEST(OptionHash, RefusesAPayloadItCannotHashYet) {
  // A string needs P04.05: no glue is made, nothing is hashed wrongly.
  Jit J("var g Option[string]");
  EXPECT_EQ(J.Fn, nullptr);
  EXPECT_NE(J.Error.find("no hash glue"), std::string::npos) << J.Error;
}

} // namespace
