//===- ASTTest.cpp - AST arena, interning, traversal and dump ------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../Support/FrontendTestSupport.h"

#include "gtest/gtest.h"

#include <functional>

using namespace gopp;
using namespace gopp::ast;
using namespace gopp::test;

namespace {

TEST(AST, InterningGivesPointerEquality) {
  ASTContext Ctx;
  Symbol A = Ctx.intern("name");
  Symbol B = Ctx.intern(std::string("na") + "me");
  Symbol C = Ctx.intern("other");
  EXPECT_EQ(A, B);
  EXPECT_NE(A, C);
  EXPECT_EQ(A.str(), "name");
  EXPECT_EQ(A.str().data(), B.str().data());
  EXPECT_TRUE(Symbol().empty());
}

TEST(AST, InternedIdentifiersShareStorage) {
  Parsed P("package p\nvar a, a, b int\nfunc a() {}\n");
  auto *Names0 = llvm::cast<GenDecl>(P.File->Decls[0]);
  auto *VS = llvm::cast<ValueSpec>(Names0->Specs[0]);
  ASSERT_EQ(VS->Names.size(), 3u);
  EXPECT_EQ(VS->Names[0]->Name, VS->Names[1]->Name);
  EXPECT_NE(VS->Names[0]->Name, VS->Names[2]->Name);
  auto *FD = llvm::cast<FuncDecl>(P.File->Decls[1]);
  EXPECT_EQ(FD->Name->Name, VS->Names[0]->Name);
}

TEST(AST, NodesAreTriviallyDestructible) {
  static_assert(std::is_trivially_destructible_v<Ident>);
  static_assert(std::is_trivially_destructible_v<File>);
  static_assert(std::is_trivially_destructible_v<FuncDecl>);
  static_assert(std::is_trivially_destructible_v<MacroDecl>);
  SUCCEED();
}

TEST(AST, ArenaGrowsWithTheTree) {
  ASTContext Ctx;
  size_t Before = Ctx.getTotalMemory();
  for (int I = 0; I < 10000; ++I)
    Ctx.create<Ident>();
  EXPECT_GT(Ctx.getTotalMemory(), Before + 10000 * sizeof(Ident) / 2);
}

TEST(AST, CreatedNodesHaveTheirKind) {
  ASTContext Ctx;
  Ident *I = Ctx.create<Ident>();
  EXPECT_EQ(I->getKind(), Node::Kind::Ident);
  EXPECT_TRUE(llvm::isa<Expr>(I));
  EXPECT_FALSE(llvm::isa<Stmt>(I));
  EXPECT_TRUE(llvm::dyn_cast<Node>(I) != nullptr);
  BlockStmt *B = Ctx.create<BlockStmt>();
  EXPECT_TRUE(llvm::isa<Stmt>(B));
  EXPECT_FALSE(llvm::isa<Expr>(B));
  FuncDecl *F = Ctx.create<FuncDecl>();
  EXPECT_TRUE(llvm::isa<Decl>(F));
}

TEST(AST, EveryNodeKindHasAName) {
  ASTContext Ctx;
  EXPECT_STREQ(Ctx.create<Ident>()->getKindName(), "Ident");
  EXPECT_STREQ(Ctx.create<FuncDecl>()->getKindName(), "FuncDecl");
  EXPECT_STREQ(Ctx.create<BadExpr>()->getKindName(), "BadExpr");
}

TEST(AST, ForEachChildStaysInsideTheParent) {
  Parsed P("package p\nfunc f() { x := a + b*c }\n");
  ASSERT_EQ(P.numErrors(), 0u);
  unsigned Count = 0;
  std::function<void(const Node *)> Walk = [&](const Node *N) {
    ++Count;
    // Children lie inside the parent.
    forEachChild(N, [&](llvm::StringRef, const Node *C) {
      EXPECT_LE(N->getBegin(), C->getBegin());
      EXPECT_LE(C->getEnd(), N->getEnd());
      Walk(C);
    });
  };
  Walk(P.File);
  EXPECT_GT(Count, 10u);
}

TEST(AST, DumpGolden) {
  Parsed P("package p\n\nimport \"fmt\"\n\nfunc f(a, b int) int {\n"
           "\treturn a + b\n}\n");
  ASSERT_EQ(P.numErrors(), 0u);
  EXPECT_EQ(P.dump(), "File <1:1, 8:1>\n"
                      "  Name: Ident <1:9, 1:10> 'p'\n"
                      "  Decl: GenDecl <3:1, 3:13> import\n"
                      "    Spec: ImportSpec <3:8, 3:13>\n"
                      "      Path: BasicLit <3:8, 3:13> string_lit \"fmt\"\n"
                      "  Decl: FuncDecl <5:1, 7:2>\n"
                      "    Name: Ident <5:6, 5:7> 'f'\n"
                      "    Type: FuncType <5:1, 5:21>\n"
                      "      Params: FieldList <5:7, 5:17>\n"
                      "        Field: Field <5:8, 5:16>\n"
                      "          Names: Ident <5:8, 5:9> 'a'\n"
                      "          Names: Ident <5:11, 5:12> 'b'\n"
                      "          Type: Ident <5:13, 5:16> 'int'\n"
                      "      Results: FieldList <5:18, 5:21>\n"
                      "        Field: Field <5:18, 5:21>\n"
                      "          Type: Ident <5:18, 5:21> 'int'\n"
                      "    Body: BlockStmt <5:22, 7:2>\n"
                      "      Stmt: ReturnStmt <6:2, 6:14>\n"
                      "        Result: BinaryExpr <6:9, 6:14> op='+'\n"
                      "          X: Ident <6:9, 6:10> 'a'\n"
                      "          Y: Ident <6:13, 6:14> 'b'\n");
}

TEST(AST, DumpIsDeterministicAcrossRuns) {
  const char *Src = "package p\nfunc f[T any](x T) T { return x }\n"
                    "type S struct { a int `t` }\n#M(1, 2)\n";
  Parsed A(Src), B(Src);
  EXPECT_EQ(A.dump(), B.dump());
  EXPECT_FALSE(A.dump().empty());
}

TEST(AST, DumpOfPartialTreeDoesNotCrash) {
  // Error recovery produces BadExpr/BadStmt/BadDecl nodes and null children.
  for (const char *Src :
       {"package p\nvar x = \n", "package p\nfunc (", "package p\n)",
        "package p\nfunc f() { if { } }", "package"}) {
    Parsed P(Src);
    EXPECT_FALSE(P.dump().empty()) << Src;
  }
}

TEST(AST, LocationsAreWithinTheFile) {
  Parsed P("package p\nvar x = []int{1, 2, 3}\nfunc f() { for i := range x { _ "
           "= i } }\n");
  ASSERT_EQ(P.numErrors(), 0u);
  uint32_t Size = P.Src.SM.getBuffer(P.Src.File).size();
  std::function<void(const Node *)> Walk = [&](const Node *N) {
    ASSERT_TRUE(N->getBegin().isValid());
    EXPECT_LE(P.Src.offsetOf(N->getEnd()), Size);
    EXPECT_LE(N->getBegin(), N->getEnd());
    forEachChild(N, [&](llvm::StringRef, const Node *C) { Walk(C); });
  };
  Walk(P.File);
}

TEST(AST, OutlivesNothingItNeedsNot) {
  // The tree stays valid and complete after the lexer and parser are gone.
  Parsed P("package p\nconst c = 1\n");
  ASSERT_EQ(P.File->Decls.size(), 1u);
  EXPECT_EQ(P.File->Name->Name.str(), "p");
}

} // namespace
