//===- ParserTest.cpp - Parser unit tests ---------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../Support/FrontendTestSupport.h"

#include "llvm/Support/MemoryBuffer.h"
#include "gtest/gtest.h"

#include <functional>
#include <set>

using namespace gopp;
using namespace gopp::test;
using namespace gopp::ast;

namespace {

std::string wrap(const std::string &Body) {
  return "package p\n" + Body + "\n";
}

/// Parses "package p" + Body and returns the dump.
std::string dumpOf(const std::string &Body) {
  Parsed P(wrap(Body));
  EXPECT_EQ(P.numErrors(), 0u) << Body << "\n"
                               << ::testing::PrintToString(P.errors());
  return P.dump();
}

bool contains(const std::string &Hay, llvm::StringRef Needle) {
  return Hay.find(Needle.str()) != std::string::npos;
}

//===----------------------------------------------------------------------===//
// Valid Go++ programs: every production of the P01 grammar
//===----------------------------------------------------------------------===//

const char *const GoppValids[] = {
    // ref types (LEX-12)
    "func f(s ref string) ref string { return s }",
    "func f(s ref []int, t ref []ref string) {}",
    "var x ref string\nvar y ref []byte\nvar z []ref string",
    "type T struct{ a ref string; b ref []T }",
    "func f() { ref := \"main\"; _ = ref; ref.Name = 1 }",
    "func ref() {}",
    "func f() { x := Dup[ref string](w); y := (ref string)(x); _, _ = x, y }",
    "func f(ref [3]int) {}",
    "type T struct{ ref string }",
    "var m map[string]ref string",
    "var c chan ref []int",
    "type U Unique[T]\ntype S Shared[ref string]\ntype W Weak[T]",
    "func f(u Unique[T], s Shared[T], w Weak[T]) {}",

    // move (LEX-12, PAR-5)
    "func (l *List) Push(move s string) {}",
    "func (move b Builder) Finish() string { return \"\" }",
    "func Join(move parts ...string) string { return \"\" }",
    "func f(move a T, move b T) {}",
    "func f(move _ string) {}",
    "var g func(move _ string, move _ int)",
    "type F func(move string)", // a parameter named move
    "func f(move T) {}",
    "func f(move pkg.T) {}",
    "func f(move G[K]) {}",
    "func f(move s [K]E) {}",
    "func f(move s []int) {}",
    "func f(move []int) {}",
    "func f(move *T, move2 int) {}",
    "func f(move ...T) {}",
    "func f(move func()) {}",
    "func f(move map[string]int) {}",
    "func f(move, x int) {}",
    "func f(a, b int, move c string) {}",
    "func f(move ref string) {}",
    "func f(move ref []int) {}",
    "func (m *OrderedMap[K, V]) Put(move k K, move v V) {}",
    "func f() { move(x); y := move(z); _ = y }",
    "func f() { move := 1; move++ }",

    // range &
    "func f() { for _, &d := range xs { _ = d } }",
    "func f() { for k, &v := range m { _, _ = k, v } }",
    "func f() { for &d := range seq { _ = d } }",
    "func f() { for i, x := range xs { _, _ = i, x } }",
    "func f() { for range xs {} }",
    "func f() { for i := range 10 { _ = i } }",

    // ! forms
    "func f!(x int) int { return x }",
    "func f!() {}",
    "func parse[T any]!(s string) T { var z T; return z }",
    "func (r *R) Read!(p []byte) int { return 0 }",
    "func load!(path string) []byte { f := os.Open!(path); return "
    "io.ReadAll!(f) }",
    "var g = func!(x int) int { return x }",
    "var h func!(int) int",
    "var h2 func!() ",
    "type I interface { Load!(path string) []byte; Close!() }",
    "func f() { x := parse[int]!(s); y := f!(1, 2)(3); z := a.b.c!(); _, _, _ "
    "= x, y, z }",
    "func f() { x := g!(h!(1)); _ = x }",
    "func f() { if x != (y) && !(z) { } }",
    "func f() { f != (x); a := !b; _ = a }",
    "func f() { defer f!(); go g!(1) }",
    "func f() { f!(x).y!(z) }",
    "func f() { c := make(chan int); v := <-c; _ = v; _ = !<-b }",

    // header imports (D2.7)
    "import \"<vector>\"\nimport \"<stdio.h>\"",
    "import (\n\t\"fmt\"\n\t\"<vector>\" // package "
    "vector\n\t\"github.com/madler/zlib/zlib.h\"\n\tz \"<zlib.h>\"\n\t_ "
    "\"os\"\n\t. \"strings\"\n)",

    // macros: invocation in operand, statement and top-level position
    "func f() { #Assert(x > 0, \"msg\") }",
    "func f() { n := #Line(); _ = n }",
    "func f() { x := #pkg.Name(a, b) + 1; _ = x }",
    "func f() { #pkg.Name(a, b) }",
    "func f() { #Repeat(3, func() { fmt.Println(\"hi\") }) }",
    "func f() { #Stringify(a.Equal(b)) }",
    "func f() { #A(#B(1), [3]int{1, 2, 3}, map[string]int{\"a\": 1}) }",
    "func f() { #A(\n\t1,\n\t2,\n) }",
    "func f() { #A(x\n) }",
    "func f() { #File().Len() }",
    "#Setup()\nvar x int",
    "#pkg.Gen(a, b)",
    "var _ = #Line()",
    "func f() { fmt.Println(#Stringify(a.Equal(b)), \"=\", a.Equal(b)) }",
    "const c = #Line()",

    // attributes
    "@Pinned\ntype Gate struct{ n int }",
    "@Derive(JSON, Equal)\ntype Config struct{ Name string }",
    "@Pinned\n@Packed\ntype Header struct{ A uint8; B uint32 }",
    "@Pinned; @Packed\ntype Header struct{ A uint8 }",
    "@Pinned @Packed\ntype H struct{}",
    "@mymacros.Table(\"users\")\ntype Account struct{ ID int }",
    "@Retry(3)\nfunc fetch!() []byte { return nil }",
    "@If(#Profile() == \"debug\")\nfunc dump(s string) { println(s) }",
    "@Inline\nfunc (r R) M() {}",
    "@Extern(\"C\")\nfunc c_puts(s *byte) int",
    "@X\nvar v int",
    "@X\nconst c = 1",
    "@X\n@Y(1)\nvar (\n\ta int\n\tb int\n)",
    "@Derive(JSON)\ntype User struct {\n\tID   int\n\tName string "
    "@JSON(\"name\") `db:\"name\"`\n}",
    "type S struct {\n\tName string @A @B\n\tAge int @C(1, 2)\n\tT *T @D "
    "`tag`\n\t*Embedded @E\n\tpkg.Emb @F `t`\n}",
    "type S struct{ a, b int @X }",
    "type S struct{ Name string @JSON(\"n\") }",

    // macro declarations
    "macro Swap(a expr, b expr) {\n\ttmp := a\n\ta = b\n\tb = tmp\n}",
    "macro Assert(cond expr, msg lit) {\n\tif !(cond) {\n\t\tpanic(\"assertion "
    "failed: \" + #Stringify(cond) + \": \" + msg)\n\t}\n}",
    "macro Kinds(a expr, b stmt, c block, d type, e ident, f lit, g decl, h "
    "tokens) {}",
    "macro Var(xs ...expr) {}",
    "macro Empty() {}",
    "macro func Equal(t reflect.Type) tokens { return nil }",
    "macro func MustEqual!(t reflect.Type) tokens { return nil }",
    "macro func NoResult() {}",
    "macro func Multi(a, b int) (int, string) { return 0, \"\" }",

    // generics
    "type Stack[T any] struct { items []T }",
    "func Map[K comparable, V any](m map[K]V) []K { return nil }",
    "type Number interface { ~int | ~int64 | ~float64 }",
    "func Sum[T Number](xs ...T) T { var s T; return s }",
    "var _ = Sum[int](1, 2)",
    "type OrderedMap[K cmp.Ordered, V any] struct { keys []K; vals []V }",
    "func (m *OrderedMap[K, V]) Len() int { return len(m.keys) }",
    "func F[T interface{ ~[]E }, E any](x T) {}",

    // statements and expressions
    "func f() { var a, b int = 1, 2; const c = 3; type T int; _, _, _ = a, b, "
    "c }",
    "func f() { x := []int{1, 2, 3}; x[0]++; x[1:2]; x[:]; x[1:2:3]; _ = x }",
    "func f() { var i interface{}; switch v := i.(type) { case int, string: _ "
    "= v; default: } }",
    "func f() { switch { case a: fallthrough; case b: } }",
    "func f() { switch x := f(); x { case 1, 2: default: } }",
    "func f() { select { case v := <-c: _ = v; case c <- 1: default: } }",
    "func f() { L: for { break L; continue L }; goto L }",
    "func f() { for i := 0; i < 10; i++ { } }",
    "func f() { for ; ; { } }",
    "func f() { for x < 3 { } }",
    "func f() { if x := f(); x > 0 { } else if y { } else { } }",
    "func f() { go func() {}(); defer func() { recover() }() }",
    "func f() { ch <- 1; <-ch; x, ok := <-ch; _, _ = x, ok }",
    "func f() { a, b = b, a; a += 1; a -= 1; a *= 2; a /= 2; a %= 2; a &= 1; a "
    "|= 1; a ^= 1; a <<= 1; a >>= 1; a &^= 1 }",
    "func f() { x := struct{ a int }{1}; _ = x; y := &T{a: 1}; _ = y }",
    "func f() { var _ = [...]int{1, 2}; var _ = map[string][]int{\"a\": {1}} }",
    "func f() { _ = 'a' + 0x1F + 0b101 + 0o17 + 1_000 + 1e3 + 0x1p-2 + 2i }",
    "func f() { _ = `raw` + \"str\\n\" }",
    "func f() { _ = x.(T); _ = x.(*T); _ = x.(interface{ M() }) }",
    "func f() { _ = -x + +y - ^z; _ = &x; _ = *p; _ = !b; _ = a &^ b }",
    "func f() { _ = a || b && c == d }",
    "func f(a, b int, c string, d ...float64) (x, y int, err error) { return }",
    "func f(int, string) (int, error)",
    "func f() func(int) func(string) bool { return nil }",
    "type T struct { a, b int; c string `tag`; T2; *T3; pkg.T4; _ int }",
    "type I interface { M(); N(int) string; Embedded; pkg.E; ~int | string }",
    "type A = B",
    "type (\n\tA int\n\tB struct{}\n)",
    "var (\n\ta = 1\n\tb, c int\n)",
    "const (\n\ta = iota\n\tb\n\tc\n)",
    "func (T) m() {}\nfunc (*T) n() {}\nfunc (t T) o() {}\nfunc (_ T) p() {}",
    "var f = func(a int) int { return a }",
    "var _ = []func(){ func() {}, func() {} }",
    "func f() { if (T{}) == x {}; for _, v := range []T{{1}, {2}} { _ = v } }",
    "func f() { if v, ok := m[k]; ok {} }",
    "func f() { switch x := y.(type) {} }",
    "func f() { x := T[int]{}; _ = x }",
    "func f() { _ = map[string]interface{}{} }",
    "func f() { var _ chan<- int; var _ <-chan int; var _ chan (<-chan int) }",
    "func f() { var _ [10]int; var _ [N * 2]int; var _ [...]int }",
    "func f() {;;; }",
};

TEST(Parser, GoppValidPrograms) {
  for (const char *Body : GoppValids) {
    SCOPED_TRACE(Body);
    Parsed P(wrap(Body));
    EXPECT_EQ(P.numErrors(), 0u) << ::testing::PrintToString(P.errors());
  }
}

//===----------------------------------------------------------------------===//
// Structure
//===----------------------------------------------------------------------===//

TEST(Parser, MoveMarkVersusParameterNamedMove) {
  struct Case {
    const char *Params;
    const char *Want; // a line of the dump that must be present
  };
  const Case Cases[] = {
      {"move v T", "Field <2:8, 2:16> move"},
      {"move T", "Names: Ident <2:8, 2:12> 'move'"},
      {"move pkg.T", "Names: Ident <2:8, 2:12> 'move'"},
      {"move G[K]", "Names: Ident <2:8, 2:12> 'move'"},
      {"move s [K]E", "Field <2:8, 2:19> move"},
      {"move s []E", "Field <2:8, 2:18> move"},
      {"move []E", "Names: Ident <2:8, 2:12> 'move'"},
      {"move *T", "Names: Ident <2:8, 2:12> 'move'"},
      {"move ...T", "Names: Ident <2:8, 2:12> 'move'"},
      {"move s ...T", "Field <2:8, 2:19> move"},
      {"move _ string", "Field <2:8, 2:21> move"},
      {"move string", "Names: Ident <2:8, 2:12> 'move'"},
      {"move ref string", "Names: Ident <2:8, 2:12> 'move'"},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Params);
    std::string D = dumpOf(std::string("func f(") + C.Params + ") {}");
    EXPECT_TRUE(contains(D, C.Want)) << D;
    bool IsMark = contains(C.Want, " move");
    // a mark never produces a parameter named move, and vice versa
    EXPECT_EQ(contains(D, "Names: Ident <2:8, 2:12> 'move'"), !IsMark) << D;
  }
}

TEST(Parser, MoveParamsAreSeparateFields) {
  std::string D = dumpOf("func f(a, b int, move c string, d string) {}");
  // a, b share int; c is marked alone; d stands alone.
  EXPECT_TRUE(contains(D, "Field <2:8, 2:16>\n")) << D;
  EXPECT_TRUE(contains(D, "Field <2:18, 2:31> move")) << D;
  EXPECT_TRUE(contains(D, "Field <2:33, 2:41>\n")) << D;
}

TEST(Parser, MoveDoesNotShareTypeWithPrecedingNames) {
  // `a` has no type of its own: it cannot take the type of a `move` parameter.
  Parsed P(wrap("func f(a, move b T) {}"));
  EXPECT_GE(P.numErrors(), 1u);
}

TEST(Parser, RefTypeDecision) {
  // `ref string` and `ref []T` are types; otherwise `ref` is a name.
  EXPECT_TRUE(contains(dumpOf("var x ref string"), "RefType"));
  EXPECT_TRUE(contains(dumpOf("var x ref []int"), "RefType"));
  EXPECT_FALSE(contains(dumpOf("func f() { ref := 1 }"), "RefType"));
  EXPECT_FALSE(contains(dumpOf("type T struct{ ref string }"), "RefType"));
  EXPECT_FALSE(contains(dumpOf("func f(ref [3]int) {}"), "RefType"));
  EXPECT_TRUE(contains(dumpOf("func f(ref string) {}"), "RefType"));
  EXPECT_TRUE(contains(dumpOf("func f() { _ = (ref string)(x) }"), "RefType"));
  EXPECT_TRUE(
      contains(dumpOf("func f() { _ = Dup[ref string](x) }"), "RefType"));
}

TEST(Parser, RangeAliasStructure) {
  EXPECT_TRUE(
      contains(dumpOf("func f() { for _, &d := range xs {} }"), "alias=value"));
  EXPECT_TRUE(
      contains(dumpOf("func f() { for &d := range s {} }"), "alias=key"));
  std::string D = dumpOf("func f() { for k, &v := range m {} }");
  EXPECT_TRUE(contains(D, "Value: Ident <2:20, 2:21> 'v'")) << D;
  EXPECT_FALSE(contains(D, "UnaryExpr")) << D;
  // `&` stays an address-of everywhere else
  EXPECT_TRUE(contains(dumpOf("func f() { _ = &x }"), "UnaryExpr"));
  EXPECT_FALSE(
      contains(dumpOf("func f() { for _, d := range xs {} }"), "alias"));
}

TEST(Parser, RangeAliasErrors) {
  for (const char *Body : {
           "func f() { for &k, v := range m {} }", // never on the key
           "func f() { for _, &v = range m {} }",  // only with :=
       }) {
    SCOPED_TRACE(Body);
    Parsed P(wrap(Body));
    EXPECT_EQ(P.numErrors(), 1u) << ::testing::PrintToString(P.errors());
  }
}

TEST(Parser, BangForms) {
  std::string D = dumpOf("func f!(x int) int { return 0 }\n"
                         "func g() { a := parse[int]!(s); b := h(1) }");
  EXPECT_TRUE(contains(D, "FuncDecl <2:1, 2:32> fails")) << D;
  EXPECT_TRUE(contains(D, "FuncType <2:1, 2:19> fails")) << D;
  EXPECT_TRUE(contains(D, "CallExpr <3:17, 3:31> must")) << D;
  EXPECT_FALSE(contains(D, "CallExpr <3:38, 3:42> must")) << D;
}

TEST(Parser, BangIsUnaryNotOtherwise) {
  // `f != (x)`, `!x` and `a !b`-like spacing never become unwrapping calls
  std::string D = dumpOf("func f() { _ = a != (b); _ = !(b); _ = c && !d }");
  EXPECT_FALSE(contains(D, "must")) << D;
  EXPECT_TRUE(contains(D, "op='!='"));
  EXPECT_TRUE(contains(D, "op='!'"));
}

TEST(Parser, MacroInvocationTokens) {
  std::string D = dumpOf("func f() { #Assert(x > 0, \"msg\") }");
  EXPECT_TRUE(contains(D, "name=Assert args=[x > 0 , \"msg\"]")) << D;
  D = dumpOf("func f() { #pkg.Name(a, (b, [c]), {d}) }");
  EXPECT_TRUE(contains(D, "name=pkg.Name args=[a , ( b , [ c ] ) , { d }]"))
      << D;
  Parsed P(wrap("func f() { #Repeat(3, func() { x() }) }"));
  EXPECT_EQ(P.numErrors(), 0u);
  auto *FD = llvm::cast<FuncDecl>(P.File->Decls[0]);
  auto *ES = llvm::cast<ExprStmt>(FD->Body->List[0]);
  auto *M = llvm::cast<MacroInvocation>(ES->X);
  ASSERT_GE(M->Args.size(), 4u);
  // a flat list in which every bracket is matched
  int Depth = 0;
  for (const Token &T : M->Args) {
    if (T.is(tok::l_paren) || T.is(tok::l_brace) || T.is(tok::l_brack))
      ++Depth;
    if (T.is(tok::r_paren) || T.is(tok::r_brace) || T.is(tok::r_brack))
      --Depth;
    EXPECT_GE(Depth, 0);
  }
  EXPECT_EQ(Depth, 0);
}

TEST(Parser, MacroInvocationIsAnExpressionStatement) {
  Parsed P(wrap("func f() { #A(1) }"));
  auto *FD = llvm::cast<FuncDecl>(P.File->Decls[0]);
  ASSERT_EQ(FD->Body->List.size(), 1u);
  EXPECT_TRUE(llvm::isa<ExprStmt>(FD->Body->List[0]));
  Parsed Q("package p\n#A(1)\n");
  EXPECT_TRUE(llvm::isa<MacroCallDecl>(Q.File->Decls[0]));
}

TEST(Parser, MacroNewlineBeforeCloseIsNotAnArgument) {
  Parsed P(wrap("func f() { #A(1,\n 2\n) }"));
  auto *FD = llvm::cast<FuncDecl>(P.File->Decls[0]);
  auto *M =
      llvm::cast<MacroInvocation>(llvm::cast<ExprStmt>(FD->Body->List[0])->X);
  ASSERT_EQ(M->Args.size(), 3u);
  EXPECT_FALSE(M->Args.back().isAutoSemi());
}

TEST(Parser, AttributesAttachToDeclarations) {
  Parsed P(wrap("@A\n@B(1)\ntype T int\n@C\nfunc f() {}\n@D\nvar v int"));
  ASSERT_EQ(P.numErrors(), 0u) << ::testing::PrintToString(P.errors());
  auto *G = llvm::cast<GenDecl>(P.File->Decls[0]);
  ASSERT_EQ(G->Attrs.size(), 2u);
  EXPECT_EQ(G->Attrs[0]->Name->Name.str(), "A");
  EXPECT_TRUE(G->Attrs[1]->HasArgs);
  // the declaration's range starts at its first attribute
  EXPECT_EQ(G->Begin, G->Attrs[0]->Begin);
  auto *F = llvm::cast<FuncDecl>(P.File->Decls[1]);
  ASSERT_EQ(F->Attrs.size(), 1u);
  auto *V = llvm::cast<GenDecl>(P.File->Decls[2]);
  ASSERT_EQ(V->Attrs.size(), 1u);
}

TEST(Parser, FieldAttributesComeBeforeTheTag) {
  Parsed P(wrap(
      "type U struct {\n\tName string @JSON(\"name\") @X `db:\"name\"`\n}"));
  ASSERT_EQ(P.numErrors(), 0u) << ::testing::PrintToString(P.errors());
  auto *TS =
      llvm::cast<TypeSpec>(llvm::cast<GenDecl>(P.File->Decls[0])->Specs[0]);
  auto *ST = llvm::cast<StructType>(TS->Type);
  Field *F = ST->Fields->List[0];
  ASSERT_EQ(F->Attrs.size(), 2u);
  EXPECT_EQ(F->Attrs[0]->Name->Name.str(), "JSON");
  EXPECT_EQ(F->Attrs[1]->Name->Name.str(), "X");
  ASSERT_NE(F->Tag, nullptr);
  EXPECT_EQ(F->Tag->Text, "`db:\"name\"`");
}

TEST(Parser, MacroDeclarations) {
  Parsed P(wrap("macro Swap(a expr, xs ...tokens, t type) { a = a }\n"
                "macro func Eq!(t reflect.Type) { }"));
  ASSERT_EQ(P.numErrors(), 0u) << ::testing::PrintToString(P.errors());
  auto *M = llvm::cast<MacroDecl>(P.File->Decls[0]);
  ASSERT_EQ(M->Params.size(), 3u);
  EXPECT_EQ(M->Params[0]->ParamKind, MacroKind::Expr);
  EXPECT_EQ(M->Params[1]->ParamKind, MacroKind::Tokens);
  EXPECT_TRUE(M->Params[1]->Variadic);
  EXPECT_EQ(M->Params[2]->ParamKind, MacroKind::Type);
  EXPECT_NE(M->Body, nullptr);
  auto *PM = llvm::cast<ProcMacroDecl>(P.File->Decls[1]);
  EXPECT_TRUE(PM->Type->Fails);
  EXPECT_EQ(PM->Name->Name.str(), "Eq");
}

TEST(Parser, ImportsAreCollected) {
  Parsed P(
      "package p\nimport (\n\"fmt\"\n\"<vector>\"\n)\nimport z \"<zlib.h>\"\n");
  ASSERT_EQ(P.numErrors(), 0u);
  ASSERT_EQ(P.File->Imports.size(), 3u);
  EXPECT_EQ(P.File->Imports[1]->Path->Text, "\"<vector>\"");
  EXPECT_EQ(P.File->Imports[2]->Name->Name.str(), "z");
}

TEST(Parser, CommentsAreCollected) {
  Parsed P("// one\npackage p /* two */\n// three\nfunc f() {} // four\n");
  ASSERT_EQ(P.numErrors(), 0u);
  ASSERT_EQ(P.File->Comments.size(), 4u);
  EXPECT_EQ(P.File->Comments[0].Length, 6u);
}

TEST(Parser, CompositeLiteralAmbiguity) {
  // In control clauses a `{` after a type name starts the block, but not after
  // an explicit literal type or in parentheses.
  EXPECT_TRUE(contains(dumpOf("func f() { if x == y { } }"), "IfStmt"));
  std::string D = dumpOf("func f() { if x == (T{}) { } }");
  EXPECT_TRUE(contains(D, "CompositeLit")) << D;
  D = dumpOf("func f() { for _, v := range []T{{1}} { _ = v } }");
  EXPECT_TRUE(contains(D, "CompositeLit")) << D;
  D = dumpOf("func f() { switch x := y; x { case T{}: } }");
  EXPECT_TRUE(contains(D, "CompositeLit")) << D;
  D = dumpOf("func f() { if x := (T{1}); x.a == 1 { } }");
  EXPECT_TRUE(contains(D, "CompositeLit")) << D;
  D = dumpOf("func f() { if f(T{1}) { } }");
  EXPECT_TRUE(contains(D, "CompositeLit")) << D;
  D = dumpOf("func f() { if a[T{1}] { } }");
  EXPECT_TRUE(contains(D, "CompositeLit")) << D;
  // `x == T{}` without parentheses is an error in Go
  Parsed P(wrap("func f() { if x == T{} { } }"));
  EXPECT_GE(P.numErrors(), 1u);
}

TEST(Parser, GenericDeclarationAmbiguity) {
  // array length versus type parameter list
  EXPECT_TRUE(contains(dumpOf("type A [N]int"), "ArrayType"));
  EXPECT_TRUE(contains(dumpOf("type A [N * 2]int"), "ArrayType"));
  EXPECT_TRUE(contains(dumpOf("type A []int"), "SliceType"));
  EXPECT_TRUE(contains(dumpOf("type A [...]int"), "ArrayType"));
  EXPECT_TRUE(contains(dumpOf("type A[T any] []T"), "TypeParams"));
  EXPECT_TRUE(contains(dumpOf("type A[T *int,] []T"), "TypeParams"));
  EXPECT_TRUE(contains(dumpOf("type A[T interface{}] []T"), "TypeParams"));
}

TEST(Parser, ChannelDirections) {
  std::string D = dumpOf("var a chan int\nvar b <-chan int\nvar c chan<- int\n"
                         "var d chan<- chan int\nvar e chan (<-chan int)");
  EXPECT_TRUE(contains(D, "ChanType <2:7, 2:15> both")) << D;
  EXPECT_TRUE(contains(D, "ChanType <3:7, 3:17> recv")) << D;
  EXPECT_TRUE(contains(D, "ChanType <4:7, 4:17> send")) << D;
}

TEST(Parser, NodeRangesAreNested) {
  // For error-free input every child lies inside its parent's range, and
  // every range is inside the file.
  std::string Src = "package p\nimport \"fmt\"\n";
  for (const char *B : GoppValids)
    Src += std::string(B) + "\n";
  // each snippet individually, so a clash between snippets cannot matter
  for (const char *B : GoppValids) {
    SCOPED_TRACE(B);
    Parsed P(wrap(B));
    ASSERT_EQ(P.numErrors(), 0u);
    std::function<void(const Node *)> Walk = [&](const Node *N) {
      EXPECT_TRUE(N->getBegin().isValid());
      EXPECT_LE(N->getBegin(), N->getEnd()) << N->getKindName();
      EXPECT_LE(N->getEnd(), P.Src.SM.getEndLoc(P.Src.File));
      forEachChild(N, [&](const char *Label, const Node *C) {
        EXPECT_LE(N->getBegin(), C->getBegin())
            << N->getKindName() << "/" << Label << ":" << C->getKindName();
        EXPECT_LE(C->getEnd(), N->getEnd())
            << N->getKindName() << "/" << Label << ":" << C->getKindName();
        Walk(C);
      });
    };
    Walk(P.File);
  }
}

//===----------------------------------------------------------------------===//
// Errors and recovery
//===----------------------------------------------------------------------===//

TEST(Parser, InvalidGoppPrograms) {
  const char *const Invalids[] = {
      "var macro = 1", // `macro` is a keyword
      "func macro() {}",
      "func f() { @Pinned\n x := 1 }", // no attributes on statements
      "@Pinned\nimport \"fmt\"",       // not on imports
      "@Pinned\nmacro M() {}",         // not on macros
      "@Pinned",                       // nothing to attach to
      "@Pinned\n42",
      "#Foo(1, 2", // unterminated token tree
      "#Foo(1, ]", // mismatched bracket
      "func f() { #Foo(1, (2] ) }",
      "#foo(1)", // lower-case macro name
      "@foo\ntype T int",
      "macro lower() {}",
      "macro M(a badkind) {}",
      "macro M(a) {}",
      "macro M(a, b expr) {}", // each parameter has its own kind
      "macro M(a expr",
      "macro func () {}",
      "func f() { for &k, v := range m {} }",
      "func f() { for _, &v = range m {} }",
      "func f(a, move b T) {}",
      "type T struct { a string @ }",
      "type T struct { a string @A( }",
      "func f!",
      "func f() { x := f!(1 }",
      "func f() { #Macro }", // missing arguments
  };
  for (const char *Body : Invalids) {
    SCOPED_TRACE(Body);
    Parsed P(wrap(Body));
    EXPECT_GE(P.numErrors(), 1u);
  }
}

TEST(Parser, ErrorLimit) {
  std::string Src = "package p\n";
  for (int I = 0; I < 50; ++I)
    Src += "var x" + std::to_string(I) + " = = \n";
  Parsed P(Src, {}, /*ErrorLimit=*/10);
  EXPECT_EQ(P.numErrors(), 10u);
  const auto &D = P.Src.Consumer.Diags;
  ASSERT_EQ(D.size(), 11u);
  EXPECT_EQ(D.back().Message, "too many errors");
  EXPECT_TRUE(P.Src.Diags.limitReached());
}

TEST(Parser, OneErrorPerLine) {
  Parsed P(wrap("var x = = = = =\nvar y = = ="));
  std::set<unsigned> Lines;
  for (const Diagnostic &D : P.Src.Consumer.Diags)
    EXPECT_TRUE(Lines.insert(P.Src.SM.getLineNumber(D.Loc)).second)
        << "two errors on line " << P.Src.SM.getLineNumber(D.Loc);
  EXPECT_GE(Lines.size(), 2u);
}

TEST(Parser, MissingCommaFixIt) {
  Parsed P(wrap("var x = []int{\n\t1,\n\t2\n}"));
  ASSERT_EQ(P.numErrors(), 1u);
  const Diagnostic &D = P.Src.Consumer.Diags[0];
  EXPECT_EQ(D.Message, "missing ',' before newline in composite literal");
  ASSERT_EQ(D.FixIts.size(), 1u);
  EXPECT_EQ(D.FixIts[0].Text, ",");
  // inserted right after the `2`
  EXPECT_EQ(P.Src.SM.getLineNumber(D.FixIts[0].Range.getBegin()), 4u);
  EXPECT_EQ(P.Src.SM.getColumnNumber(D.FixIts[0].Range.getBegin()), 3u);
}

TEST(Parser, NotAGoppFile) {
  Parsed P("hello world\n");
  EXPECT_EQ(P.numErrors(), 1u);
  EXPECT_TRUE(P.File->Decls.empty());
  Parsed Q("");
  EXPECT_EQ(Q.numErrors(), 1u);
}

TEST(Parser, ErrorsStillProduceATree) {
  Parsed P(wrap("func f() { x := ; y := 2 }\nfunc g() {}"));
  EXPECT_GE(P.numErrors(), 1u);
  EXPECT_EQ(P.File->Decls.size(), 2u);
  EXPECT_TRUE(contains(P.dump(), "FuncDecl <3:1"));
}

TEST(Parser, BadTopLevelInitializerDoesNotSwallowTheNextFunction) {
  // '?' is illegal and does not end the line (no semicolon is inserted).
  Parsed P("package p\nvar b = ?\nfunc f() {\n\tx := 1\n}\nfunc g() {}\n");
  EXPECT_GE(P.numErrors(), 2u);
  ASSERT_EQ(P.File->Decls.size(), 3u);
  EXPECT_TRUE(llvm::isa<GenDecl>(P.File->Decls[0]));
  auto *F = llvm::dyn_cast<FuncDecl>(P.File->Decls[1]);
  ASSERT_TRUE(F);
  EXPECT_EQ(F->Name->Name.str(), "f");
  ASSERT_TRUE(F->Body);
  EXPECT_EQ(F->Body->List.size(), 1u);
}

TEST(Parser, RecoveryResynchronizesAtNextDeclaration) {
  Parsed P(wrap("func f() {\n\tx := = 1\n\ty := (\n}\nvar ok = 1\nfunc g() int "
                "{ return 1 }"));
  EXPECT_GE(P.numErrors(), 1u);
  EXPECT_EQ(P.File->Decls.size(), 3u) << P.dump();
  EXPECT_TRUE(contains(P.dump(), "Name: Ident <7:6, 7:7> 'g'")) << P.dump();
}

TEST(Parser, DiagnosticsComeInSourceOrder) {
  // Deciding `move G[` looks ahead for the `]` and lexes line 5 early. Its
  // lexical error must still follow the parser's errors on lines 3 and 4
  // (go/parser sorts its errors).
  Parsed P("package p\nfunc f(move G[K\nfunc g() {\n\tx := 'ab'\n}\n");
  const auto &D = P.Src.Consumer.Diags;
  ASSERT_GE(D.size(), 2u);
  bool SawRune = false;
  for (size_t I = 0; I < D.size(); ++I) {
    SawRune |= D[I].ID == diag::err_lex_rune_illegal;
    if (I > 0)
      EXPECT_LE(P.Src.offsetOf(D[I - 1].Loc), P.Src.offsetOf(D[I].Loc))
          << ::testing::PrintToString(P.errors());
  }
  EXPECT_TRUE(SawRune);
}

TEST(Parser, LexicalErrorHidesParserErrorsOnItsLine) {
  // One error per line counts the lexer's errors too, as in go/parser: no
  // "expected ';'" or "expected operand" after an illegal character.
  Parsed P(wrap("var a = 1 $ 2\nvar b = \xE2\x80\x9Chi\xE2\x80\x9D"));
  std::vector<diag::ID> IDs;
  for (const Diagnostic &D : P.Src.Consumer.Diags)
    IDs.push_back(D.ID);
  EXPECT_EQ(IDs, (std::vector<diag::ID>{diag::err_lex_illegal_char,
                                        diag::err_lex_curly_quote,
                                        diag::err_lex_curly_quote}))
      << ::testing::PrintToString(P.errors());
}

TEST(Parser, MacroBracketMismatchNotesTheOpenBracket) {
  Parsed P(wrap("func f() {\n\t#M(a, [b)\n}"));
  ASSERT_GE(P.numErrors(), 1u);
  const Diagnostic &D = P.Src.Consumer.Diags[0];
  EXPECT_EQ(D.ID, diag::err_macro_args_mismatched_expected);
  EXPECT_EQ(D.Message, "mismatched ')' in macro arguments, expected ']'");
  ASSERT_EQ(D.Notes.size(), 1u);
  EXPECT_EQ(D.Notes[0].ID, diag::note_to_match_this);
  EXPECT_EQ(D.Notes[0].Message, "to match this '['");
  EXPECT_EQ(P.Src.SM.getColumnNumber(D.Notes[0].Loc), 8u);

  // Nothing open inside the arguments: the note points at their '('.
  Parsed Q(wrap("func f() {\n\t#M(a, b]\n}"));
  ASSERT_GE(Q.numErrors(), 1u);
  const Diagnostic &E = Q.Src.Consumer.Diags[0];
  EXPECT_EQ(E.Message, "mismatched ']' in macro arguments");
  ASSERT_EQ(E.Notes.size(), 1u);
  EXPECT_EQ(E.Notes[0].Message, "to match this '('");
  EXPECT_EQ(Q.Src.SM.getColumnNumber(E.Notes[0].Loc), 4u);
}

TEST(Parser, EmbeddedFieldAttributes) {
  // FieldDecl = ( IdentifierList Type | EmbeddedField ) [ FieldAttributes ]
  //             [ Tag ]
  Parsed P(wrap("type U struct {\n\tBase @A\n\t*pkg.Ptr @B(1) `t`\n}"));
  ASSERT_EQ(P.numErrors(), 0u) << ::testing::PrintToString(P.errors());
  auto *TS =
      llvm::cast<TypeSpec>(llvm::cast<GenDecl>(P.File->Decls[0])->Specs[0]);
  auto *ST = llvm::cast<StructType>(TS->Type);
  ASSERT_EQ(ST->Fields->List.size(), 2u);
  Field *Base = ST->Fields->List[0];
  EXPECT_TRUE(Base->Names.empty());
  ASSERT_EQ(Base->Attrs.size(), 1u);
  EXPECT_EQ(Base->Attrs[0]->Name->Name.str(), "A");
  EXPECT_FALSE(Base->Attrs[0]->HasArgs);
  Field *Ptr = ST->Fields->List[1];
  EXPECT_TRUE(Ptr->Names.empty());
  EXPECT_TRUE(llvm::isa<StarExpr>(Ptr->Type));
  ASSERT_EQ(Ptr->Attrs.size(), 1u);
  EXPECT_EQ(Ptr->Attrs[0]->Name->Name.str(), "B");
  ASSERT_NE(Ptr->Tag, nullptr);
  EXPECT_EQ(Ptr->Tag->Text, "`t`");
}

//===----------------------------------------------------------------------===//
// Robustness: nothing may crash, hang or overflow the stack
//===----------------------------------------------------------------------===//

TEST(ParserRobustness, DeepNesting) {
  const unsigned N = 200000;
  struct Case {
    const char *Name;
    std::string Open, Close, Body;
  } Cases[] = {
      {"parens", "(", ")", "x"},
      {"brackets types", "[]", "", "int"},
      {"pointers", "*", "", "x"},
      {"unary minus", "-", "", "x"},
      {"receive", "<-", "", "x"},
      {"blocks", "{", "}", ""},
      {"func literals", "func() {", "}", ""},
      {"if chain", "if x {", "}", ""},
      {"composite literals", "T{", "}", ""},
      {"index", "a[", "]", "x"},
      {"call", "f(", ")", "x"},
      {"chan", "chan ", "", "int"},
      {"map", "map[int]", "", "int"},
      {"struct", "struct{ a ", "}", "int"},
      {"interface", "interface{ ", "}", ""},
      {"macro tokens", "#A(", ")", "x"},
      {"switch", "switch { case x: ", "}", ""},
      {"labels", "L: ", "", "x"},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    std::string Src = "package p\nfunc f() {\n_ = ";
    std::string Stmt;
    for (unsigned I = 0; I < N; ++I)
      Stmt += C.Open;
    Stmt += C.Body;
    for (unsigned I = 0; I < N; ++I)
      Stmt += C.Close;
    // statement-like openers are not expressions: put them as statements
    if (C.Open == "{" || C.Open.rfind("func", 0) == 0 || C.Open == "if x {" ||
        C.Open.rfind("switch", 0) == 0 || C.Open.rfind("L:", 0) == 0 ||
        C.Open.rfind("#", 0) == 0)
      Src = "package p\nfunc f() {\n";
    Src += Stmt + "\n}\n";
    Parsed P(Src);
    // A macro's token tree is collected iteratively: no nesting limit.
    if (C.Open.rfind("#", 0) == 0)
      EXPECT_EQ(P.numErrors(), 0u);
    else
      EXPECT_GE(P.numErrors(), 1u);
  }
}

TEST(ParserRobustness, ModerateNestingIsAccepted) {
  std::string Src = "package p\nfunc f() { _ = ";
  for (int I = 0; I < 300; ++I)
    Src += "(";
  Src += "x";
  for (int I = 0; I < 300; ++I)
    Src += ")";
  Src += " }\n";
  Parsed P(Src);
  EXPECT_EQ(P.numErrors(), 0u) << ::testing::PrintToString(P.errors());
}

TEST(ParserRobustness, NestingLimitIsConfigurable) {
  ParseOptions O;
  O.MaxNesting = 8;
  Parsed P("package p\nvar _ = ((((((((((((((((x))))))))))))))))\n", O);
  EXPECT_GE(P.numErrors(), 1u);
  EXPECT_NE(P.errors()[0].find("exceeded max nesting depth"),
            std::string::npos);
}

TEST(ParserRobustness, LongChains) {
  std::string Src = "package p\nvar _ = 1";
  for (int I = 0; I < 100000; ++I)
    Src += " + 1";
  Src += "\n";
  Parsed P(Src);
  ASSERT_GE(P.numErrors(), 1u);
  EXPECT_NE(P.errors()[0].find("expression too deeply nested (limit 1024)"),
            std::string::npos);
  // and a long but reasonable chain is fine
  Src = "package p\nvar _ = 1";
  for (int I = 0; I < 1000; ++I)
    Src += " + 1";
  Parsed Q(Src + "\n");
  EXPECT_EQ(Q.numErrors(), 0u);
  // selectors and calls chain iteratively too
  Src = "package p\nvar _ = a";
  for (int I = 0; I < 100000; ++I)
    Src += ".b()";
  Parsed R(Src + "\n");
  EXPECT_GE(R.numErrors(), 1u);
}

// The tree of a declaration may be MaxHeight tall: GenDecl, ValueSpec, then a
// left-deep chain of binary nodes ending in one literal.
static std::string chainDecl(unsigned Binaries) {
  std::string Src = "package p\nvar _ = 1";
  for (unsigned I = 0; I < Binaries; ++I)
    Src += " + 1";
  return Src + "\n";
}

TEST(ParserRobustness, HeightLimitExactBoundary) {
  ParseOptions O;
  unsigned Max = O.MaxHeight;
  Parsed Ok(chainDecl(Max - 3), O);
  EXPECT_EQ(Ok.numErrors(), 0u) << ::testing::PrintToString(Ok.errors());
  EXPECT_TRUE(llvm::isa<GenDecl>(Ok.File->Decls[0]));
  Parsed Bad(chainDecl(Max - 2), O);
  ASSERT_EQ(Bad.numErrors(), 1u);
  EXPECT_NE(Bad.errors()[0].find("expression too deeply nested (limit 1024)"),
            std::string::npos);
  EXPECT_TRUE(llvm::isa<BadDecl>(Bad.File->Decls[0]));
}

TEST(ParserRobustness, HeightLimitCountsNestingTimesChains) {
  // 300 levels of parentheses, each with a chain of 400: no single limit is
  // reached, but the tree is about 120k tall. It must be rejected, and the
  // result must be safe to walk.
  std::string E = "a";
  for (int I = 0; I < 300; ++I) {
    std::string T = "(" + E;
    for (int J = 0; J < 400; ++J)
      T += "+a";
    E = T + ")";
  }
  Parsed P("package p\nvar x = " + E + "\nvar y = 1\n");
  ASSERT_GE(P.numErrors(), 1u);
  EXPECT_NE(P.errors()[0].find("expression too deeply nested"),
            std::string::npos);
  EXPECT_TRUE(llvm::isa<BadDecl>(P.File->Decls[0]));
}

TEST(ParserRobustness, ManyStatementsAndHugeFile) {
  std::string Src = "package p\nfunc f() {\n";
  for (int I = 0; I < 100000; ++I)
    Src += "\tx" + std::to_string(I % 10) + " := f(" + std::to_string(I) +
           ", \"s\") + 1\n";
  Src += "}\n";
  Parsed P(Src);
  EXPECT_EQ(P.numErrors(), 0u);
  EXPECT_EQ(llvm::cast<FuncDecl>(P.File->Decls[0])->Body->List.size(), 100000u);
}

TEST(ParserRobustness, HugeLiteralsAndIdentifiers) {
  std::string Big(1 << 20, '9');
  Parsed P("package p\nvar _ = " + Big + "\nvar _ = 0x" +
           std::string(100000, 'f') + "\nvar _ = 1e" + Big + "\nvar " +
           std::string(1 << 20, 'a') + " int\nvar _ = \"" +
           std::string(1 << 20, 'x') + "\"\n");
  EXPECT_EQ(P.numErrors(), 0u) << ::testing::PrintToString(P.errors());
  // overflowing values are not the parser's business: the text is kept
  auto *G = llvm::cast<GenDecl>(P.File->Decls[0]);
  auto *VS = llvm::cast<ValueSpec>(G->Specs[0]);
  EXPECT_EQ(llvm::cast<BasicLit>(VS->Values[0])->Text.size(), Big.size());
}

TEST(ParserRobustness, UnterminatedConstructs) {
  const char *const Cases[] = {
      "package p\nvar s = \"abc",
      "package p\nvar s = `abc",
      "package p\nvar s = 'a",
      "package p\n/* unterminated",
      "package p\nfunc f() {",
      "package p\nfunc f(",
      "package p\ntype T struct {",
      "package p\ntype T interface {",
      "package p\nvar x = []int{1, 2",
      "package p\nvar x = map[string]int{\"a\":",
      "package p\nfunc f() { if x {",
      "package p\nfunc f() { switch x { case",
      "package p\nfunc f() { select { case",
      "package p\nimport (",
      "package p\nimport \"fmt",
      "package p\nmacro M(",
      "package p\n@",
      "package p\n#",
      "package p\n@A(",
      "package p\ntype T[",
      "package p\nfunc f[T",
      "package p\nvar x = f!",
      "package p\nvar x = a.",
      "package p\nfunc (",
      "package p\nfunc (move",
      "package p\nfunc f(move x",
      "package p\nfunc f(move x [",
  };
  for (const char *Src : Cases) {
    SCOPED_TRACE(Src);
    Parsed P(Src);
    EXPECT_GE(P.numErrors(), 1u);
  }
}

TEST(ParserRobustness, BadBytes) {
  const std::string Cases[] = {
      std::string("\xEF\xBB\xBFpackage p\n"),            // BOM: ignored
      std::string("package p\n\xEF\xBB\xBFvar x int\n"), // BOM in the middle
      std::string("package p\nvar x\x00 int\n", 22),     // NUL
      std::string("package p\nvar \xFF\xFE x int\n"),    // invalid UTF-8
      std::string("package p\nvar s = \"\x80\xC0\xE0\"\n"),
      std::string("\xFF\xFEp\x00"
                  "a\x00"),                         // UTF-16 BOM
      std::string("package p\n\xED\xA0\x80\n"),     // surrogate
      std::string("package p\n\xF4\x90\x80\x80\n"), // > U+10FFFF
      std::string("package p\n\xC0\xAF\n"),         // overlong
      std::string(5000, '\0'),
      std::string("package p\r\nvar x = 1\r\n"), // CRLF
      std::string("package p\rvar x = 1\r"),     // lone CR
  };
  for (const std::string &Src : Cases) {
    Parsed P(Src);
    ASSERT_NE(P.File, nullptr);
    (void)P.dump();
  }
  // BOM at the start and CRLF line ends are fine
  EXPECT_EQ(Parsed(Cases[0]).numErrors(), 0u);
  EXPECT_EQ(Parsed(Cases[10]).numErrors(), 0u);
  EXPECT_GE(Parsed(Cases[1]).numErrors(), 1u);
  EXPECT_GE(Parsed(Cases[2]).numErrors(), 1u);
}

std::string readExample(const char *Name) {
  std::string Path = std::string(GOPP_EXAMPLES_DIR) + "/" + Name + "/main.gopp";
  auto Buf = llvm::MemoryBuffer::getFile(Path);
  EXPECT_TRUE(bool(Buf)) << Path;
  return Buf ? (*Buf)->getBuffer().str() : std::string();
}

const char *const Examples[] = {
    "01-hello", "02-wc",      "03-kvparse", "04-binarytrees", "05-workers",
    "06-zcrc",  "07-vecsort", "08-derive",  "09-orderedmap",  "10-minicalc"};

TEST(ParserRobustness, SamplesParseWithoutErrors) {
  for (const char *Name : Examples) {
    SCOPED_TRACE(Name);
    std::string Src = readExample(Name);
    ASSERT_FALSE(Src.empty());
    Parsed P(Src);
    EXPECT_EQ(P.numErrors(), 0u) << ::testing::PrintToString(P.errors());
  }
}

TEST(ParserRobustness, EveryPrefixOfTheSamples) {
  for (const char *Name : Examples) {
    SCOPED_TRACE(Name);
    std::string Src = readExample(Name);
    // Parse every prefix, in steps; a truncated program must give errors
    // (or be complete) but never crash.
    for (size_t N = 0; N < Src.size(); N += 7) {
      Parsed P(Src.substr(0, N), {}, 10);
      ASSERT_NE(P.File, nullptr);
    }
  }
}

/// A small deterministic generator (xorshift64*).
struct Rng {
  uint64_t S;
  uint64_t next() {
    S ^= S >> 12;
    S ^= S << 25;
    S ^= S >> 27;
    return S * 2685821657736338717ull;
  }
  size_t below(size_t N) { return N ? next() % N : 0; }
};

TEST(ParserRobustness, MutatedSamples) {
  Rng R{0x9E3779B97F4A7C15ull};
  const char Interesting[] = "(){}[];,.:#@!&*<>=\"'`/\\ \n\t~_";
  for (const char *Name : Examples) {
    std::string Orig = readExample(Name);
    for (int Iter = 0; Iter < 60; ++Iter) {
      std::string Src = Orig;
      for (int M = 0, E = 1 + R.below(4); M < E; ++M) {
        size_t Pos = R.below(Src.size());
        switch (R.below(5)) {
        case 0: // flip a byte
          Src[Pos] = static_cast<char>(R.next());
          break;
        case 1: // insert an interesting character
          Src.insert(Pos, 1, Interesting[R.below(sizeof(Interesting) - 1)]);
          break;
        case 2: // delete a range
          Src.erase(Pos, R.below(40));
          break;
        case 3: // duplicate a range
          Src.insert(Pos, Src.substr(R.below(Src.size()), R.below(60)));
          break;
        default: // insert a random byte string
          Src.insert(Pos,
                     std::string(1 + R.below(4), static_cast<char>(R.next())));
          break;
        }
      }
      Parsed P(Src, {}, 10);
      ASSERT_NE(P.File, nullptr);
      (void)P.dump();
    }
  }
}

TEST(ParserRobustness, RandomTokenSoup) {
  // Random sequences of real tokens reach the parser's error paths far more
  // often than random bytes do.
  const char *Toks[] = {
      "package",     "import",    "func",   "type",  "var",      "const",
      "struct",      "interface", "map",    "chan",  "if",       "else",
      "for",         "range",     "switch", "case",  "default",  "select",
      "go",          "defer",     "return", "break", "continue", "goto",
      "fallthrough", "macro",     "ref",    "move",  "string",   "x",
      "y",           "T",         "Foo",    "1",     "2.5",      "'a'",
      "\"s\"",       "`r`",       "(",      ")",     "[",        "]",
      "{",           "}",         ",",      ";",     ":",        ".",
      "...",         "+",         "-",      "*",     "/",        "&",
      "|",           "^",         "<<",     ">>",    "&^",       "&&",
      "||",          "<-",        "++",     "--",    "==",       "!=",
      "<",           ">",         "<=",     ">=",    "=",        ":=",
      "+=",          "!",         "~",      "#",     "@",        "\n",
  };
  Rng R{0x1234567ull};
  for (int Iter = 0; Iter < 3000; ++Iter) {
    std::string Src = "package p\n";
    for (int I = 0, N = 1 + R.below(60); I < N; ++I) {
      Src += Toks[R.below(sizeof(Toks) / sizeof(Toks[0]))];
      Src += ' ';
    }
    Parsed P(Src, {}, 10);
    ASSERT_NE(P.File, nullptr);
  }
}

} // namespace
