//===- FlowTest.cpp - Option flow typing in the side table (P03.17) -------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Flow typing (ADR-0018 point 4) records the narrowed type of each use of an
// Option variable in Info::Types, while Info::Uses still names the variable
// (type Option[T]). Info::narrowedType reads that back; P04 codegen uses it
// to know where an Option is already a T. Both sides: the uses inside a check
// are narrowed, the operand of the check and an assignment target are not.
//
//===----------------------------------------------------------------------===//

#include "../Support/FrontendTestSupport.h"

#include "gopp/Sema/Sema.h"

#include <gtest/gtest.h>

using namespace gopp;
using namespace gopp::sema;

namespace {

class NoImports : public Importer {
public:
  Package *import(llvm::StringRef, std::string &) override { return nullptr; }
};

struct Checked {
  explicit Checked(llvm::StringRef Text) : Pr(Text) {
    const ast::File *Files[] = {Pr.File};
    CP = checkPackage(Ctx, Pr.Src.SM, "p", Files, Imp, Pr.Src.Diags, {});
  }
  /// The narrowed uses of the variable called Name: their types as text.
  std::vector<std::string> narrowed(llvm::StringRef Name) {
    std::vector<std::pair<unsigned, std::string>> Found;
    for (auto &[Id, Obj] : CP->I.Uses) {
      if (Obj->Name != Name)
        continue;
      if (Type *T = CP->I.narrowedType(Id))
        Found.push_back({Id->getBegin().getRaw(), typeString(T)});
    }
    std::sort(Found.begin(), Found.end());
    std::vector<std::string> R;
    for (auto &F : Found)
      R.push_back(F.second);
    return R;
  }
  /// The types of the variables called Name, in source order.
  std::vector<std::string> defTypes(llvm::StringRef Name) {
    std::vector<std::pair<unsigned, std::string>> Found;
    for (auto &[Id, Obj] : CP->I.Defs)
      if (Obj && Obj->Name == Name && Obj->Ty)
        Found.push_back({Id->getBegin().getRaw(), typeString(Obj->Ty)});
    std::sort(Found.begin(), Found.end());
    std::vector<std::string> R;
    for (auto &F : Found)
      R.push_back(F.second);
    return R;
  }
  unsigned uses(llvm::StringRef Name) {
    unsigned N = 0;
    for (auto &[Id, Obj] : CP->I.Uses)
      N += Obj->Name == Name;
    return N;
  }
  test::Parsed Pr;
  TypeContext Ctx;
  NoImports Imp;
  std::unique_ptr<CheckedPackage> CP;
};

TEST(Flow, TheSideTableHoldsTheNarrowedTypeOfEachUse) {
  Checked C(R"(package p

type N struct {
	V    int
	Next Option[*N]
}

func f(head Option[*N]) int {
	if head == nil {
		return 0
	}
	a := head.V
	b := head.V
	head = nil
	return a + b
}
)");
  ASSERT_TRUE(C.CP);
  EXPECT_EQ(C.Pr.numErrors(), 0u) << C.Pr.errors().size();
  // Uses of head: the comparison, two reads, the assignment target.
  EXPECT_EQ(C.uses("head"), 4u);
  EXPECT_EQ(C.narrowed("head"), (std::vector<std::string>{"*p.N", "*p.N"}));
}

TEST(Flow, AnUncheckedUseIsNotNarrowedAndIsAnError) {
  Checked C(R"(package p

type N struct{ V int }

func f(o Option[*N]) int {
	x := o.V
	if o != nil {
		return o.V + x
	}
	return 0
}
)");
  ASSERT_TRUE(C.CP);
  auto Errs = C.Pr.errors();
  ASSERT_EQ(Errs.size(), 1u);
  EXPECT_NE(Errs[0].find("o may be nil"), std::string::npos) << Errs[0];
  // Only the read inside the check is narrowed.
  EXPECT_EQ(C.narrowed("o").size(), 1u);
}

// Owner decision (OWN-P10): where `a` is known to be set it is a *N, and so
// is a copy made there; outside a check, and with a declared Option type, the
// copy is an Option.
TEST(Flow, ACopyMadeWhereTheOptionIsCheckedHasTheValueType) {
  Checked C(R"(package p

type N struct{ V int }

func f(a Option[*N]) int {
	p := a
	var q Option[*N] = a
	if a == nil {
		return 0
	}
	p = nil
	r := a
	var s = a
	var t Option[*N] = a
	_, _, _, _ = p, q, s, t
	return r.V
}
)");
  ASSERT_TRUE(C.CP);
  EXPECT_EQ(C.Pr.numErrors(), 0u) << C.Pr.errors().size();
  EXPECT_EQ(C.defTypes("p"), (std::vector<std::string>{"Option[*p.N]"}));
  EXPECT_EQ(C.defTypes("q"), (std::vector<std::string>{"Option[*p.N]"}));
  EXPECT_EQ(C.defTypes("r"), (std::vector<std::string>{"*p.N"}));
  EXPECT_EQ(C.defTypes("s"), (std::vector<std::string>{"*p.N"}));
  EXPECT_EQ(C.defTypes("t"), (std::vector<std::string>{"Option[*p.N]"}));
}

// A function literal starts with nothing known, and inside it a captured
// variable is not narrowed (CLO-9): it is copied into a local first.
TEST(Flow, ACapturedOptionIsNotNarrowedInsideAFunctionLiteral) {
  Checked C(R"(package p

type N struct{ V int }

func f(a Option[*N]) func() int {
	if a == nil {
		return func() int { return 0 }
	}
	return func() int {
		if a != nil {
			return a.V
		}
		if c := a; c != nil {
			return c.V
		}
		return 0
	}
}
)");
  ASSERT_TRUE(C.CP);
  auto Errs = C.Pr.errors();
  ASSERT_EQ(Errs.size(), 1u);
  EXPECT_NE(Errs[0].find("a may be nil; check a local copy"), std::string::npos)
      << Errs[0];
  EXPECT_TRUE(C.narrowed("a").empty());
  EXPECT_EQ(C.narrowed("c"), (std::vector<std::string>{"*p.N"}));
}

} // namespace
