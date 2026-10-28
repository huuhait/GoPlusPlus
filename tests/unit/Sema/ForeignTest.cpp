//===- ForeignTest.cpp - Foreign scope interface tests (P03.09) -----------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The checker resolves `import "zlib.h"` and `import "<vector>"` through
// ForeignScope without Clang (D3.7). The tests declare fake C and C++ headers
// in a MockForeignScope and check Go++ programs against them: names, calls
// with overloads, explicit template arguments (ADR-0010), const metadata
// (ADR-0008) and ambiguous short names. Each test has the good program next
// to the bad one.
//
//===----------------------------------------------------------------------===//

#include "gopp/Sema/Loader.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace gopp;
using namespace gopp::sema;

namespace {

class Fixture {
public:
  Fixture() {
    llvm::SmallString<128> P;
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-foreign", P));
    Root = std::string(P);
  }
  ~Fixture() { llvm::sys::fs::remove_directories(Root); }

  BasicType *t(BasicKind K) { return Ctx.typ(K); }

  /// The header package (also the Pkg of the objects declared in it).
  Package *header(llvm::StringRef Path) {
    std::string Err;
    return Prov.header(Path, Mem, Err);
  }

  Func *func(Package *P, llvm::StringRef Name, std::vector<Type *> Params,
             std::vector<Type *> Results, bool Variadic = false) {
    auto *F = Mem.make<Func>();
    F->Name = Mem.copy(Name);
    F->Pkg = P;
    F->Ty = sig(Params, Results, Variadic);
    return F;
  }
  SignatureType *sig(std::vector<Type *> Params, std::vector<Type *> Results,
                     bool Variadic = false) {
    return Ctx.signature(Ctx.tuple(Params), Ctx.tuple(Results), Variadic, false,
                         0);
  }
  Const *constant(Package *P, llvm::StringRef Name, Type *Ty, int64_t V) {
    auto *C = Mem.make<Const>();
    C->Name = Mem.copy(Name);
    C->Pkg = P;
    C->Ty = Ty;
    C->Val = Mem.constant(ConstValue::makeInt64(V));
    return C;
  }
  TypeName *alias(Package *P, llvm::StringRef Name, Type *Ty) {
    auto *TN = Mem.make<TypeName>();
    TN->Name = Mem.copy(Name);
    TN->Pkg = P;
    TN->Ty = Ty;
    TN->IsAlias = true;
    return TN;
  }
  /// A C++ class: an opaque named type with methods.
  NamedType *klass(Package *P, llvm::StringRef Name) {
    auto *TN = Mem.make<TypeName>();
    TN->Name = Mem.copy(Name);
    TN->Pkg = P;
    NamedType *N = Ctx.newNamed(TN);
    TN->Ty = N;
    N->Underlying = Ctx.structOf({});
    return N;
  }
  Func *method(NamedType *N, Package *P, llvm::StringRef Name,
               std::vector<Type *> Params, std::vector<Type *> Results) {
    Func *F = func(P, Name, Params, Results);
    auto *Recv = Mem.make<Var>();
    Recv->Name = "self";
    Recv->Ty = N;
    Recv->Pkg = P;
    F->Recv = Recv;
    if (!N->Methods)
      N->Methods = Ctx.newMethodList();
    N->Methods->push_back(F);
    return F;
  }

  std::vector<std::string> check(const std::string &Main) {
    {
      std::ofstream Out(Root + "/main.gopp");
      Out << Main;
    }
    SourceManager SM;
    CollectingDiagnosticConsumer Collect;
    DiagnosticsEngine Diags(SM, Collect, 0);
    LoadOptions LO;
    LO.Root = Root;
    Loader L(SM, Diags, Ctx, LO, {});
    L.setForeignProvider(&Prov);
    L.loadDir(Root, "");
    std::vector<std::string> Errors;
    for (const Diagnostic &D : Collect.Diags) {
      if (D.Sev != Severity::Error)
        continue;
      std::string M = D.Message;
      for (const DiagNote &N : D.Notes)
        M += "\n  note: " + N.Message;
      Errors.push_back(M);
    }
    return Errors;
  }

  TypeContext Ctx;
  Arena Mem;
  MockForeignProvider Prov;
  std::string Root;
};

bool mentions(const std::vector<std::string> &Errs, const std::string &Sub) {
  for (const std::string &E : Errs)
    if (E.find(Sub) != std::string::npos)
      return true;
  return false;
}

//===----------------------------------------------------------------------===//
// C headers
//===----------------------------------------------------------------------===//

/// A fake zlib.h.
struct Zlib {
  explicit Zlib(Fixture &F) {
    Package *P = F.header("zlib.h");
    MockForeignScope &S = F.Prov.scopeOf("zlib.h");
    Type *U32 = F.t(BasicKind::Uint32);
    Type *Byte = F.Ctx.byteType();
    S.add(F.func(P, "crc32", {U32, F.Ctx.pointer(Byte), U32}, {U32}));
    S.add(F.constant(P, "Z_OK", F.t(BasicKind::Int32), 0));
    S.add(F.alias(P, "uLong", F.t(BasicKind::Uint64)));
    // `void fill(char *buf)`: the callee writes through the pointer. The
    // slice stands for it here. `sum` only reads (const char *).
    Type *Bytes = F.Ctx.slice(Byte);
    Func *Fill = F.func(P, "fill", {Bytes}, {});
    S.addOverload(Fill, {llvm::cast<SignatureType>(Fill->Ty), 1, false, 0});
    S.add(F.func(P, "sum", {Bytes}, {U32}));
  }
};

TEST(Foreign, CNamesCallsAndConstantsResolveThroughTheScope) {
  Fixture F;
  Zlib Z(F);
  EXPECT_TRUE(F.check(R"(package main
import "zlib.h"
func main() {
	var p *byte
	var crc uint32 = zlib.crc32(0, p, 3)
	var ok int32 = zlib.Z_OK
	var n zlib.uLong = 7
	var m uint64 = n
	_, _, _ = crc, ok, m
}
)")
                  .empty());
}

TEST(Foreign, CCallsAreCheckedAgainstTheDeclaredSignature) {
  Fixture F;
  Zlib Z(F);
  auto Errs = F.check(R"(package main
import "zlib.h"
func main() {
	var p *byte
	_ = zlib.crc32(0, p)
	var s string = zlib.crc32(0, p, 3)
	_ = s
	_ = zlib.nothing
}
)");
  EXPECT_TRUE(mentions(Errs, "cannot call zlib.crc32: no matching function"));
  EXPECT_TRUE(mentions(Errs, "as string value"));
  EXPECT_TRUE(mentions(Errs, "undefined: zlib.nothing"));
  EXPECT_EQ(Errs.size(), 3u);
}

TEST(Foreign, ConstMetadataStopsReadOnlyValuesReachingMutableParameters) {
  Fixture F;
  Zlib Z(F);
  // A borrowed parameter is read-only: passing it to a parameter the callee
  // changes is an error; passing it where C only reads is fine, and so is an
  // owned local (ADR-0008 Q2).
  auto Errs = F.check(R"(package main
import "zlib.h"
func readOnly(buf []byte) uint32 {
	zlib.fill(buf)
	return zlib.sum(buf)
}
func owned() uint32 {
	local := make([]byte, 4)
	zlib.fill(local)
	return zlib.sum(local)
}
func main() {}
)");
  ASSERT_EQ(Errs.size(), 1u) << Errs[0];
  EXPECT_TRUE(mentions(Errs, "cannot pass buf to zlib.fill: it is read-only"));
}

//===----------------------------------------------------------------------===//
// C++ headers
//===----------------------------------------------------------------------===//

/// A fake <vector> and <algorithm>.
struct Cpp {
  explicit Cpp(Fixture &Fx) : F(Fx) {
    VecPkg = F.header("<vector>");
    Vec = &F.Prov.scopeOf("<vector>");
    Type *I32 = F.t(BasicKind::Int32);
    auto *TN = F.Mem.make<TypeName>();
    TN->Name = "vector";
    TN->Pkg = VecPkg;
    TN->Ty = F.klass(VecPkg, "vector"); // placeholder: the template itself
    Vec->addTemplate(
        TN,
        [this, I32](llvm::ArrayRef<ForeignTemplateArg> A,
                    std::string &Err) -> Object * {
          if (A.size() != 1 || !A[0].isType()) {
            Err = "vector takes one type argument";
            return nullptr;
          }
          auto &Inst = Instances[A[0].Ty];
          if (!Inst) {
            NamedType *N = F.klass(VecPkg, "vector");
            F.method(N, VecPkg, "size", {}, {I32});
            PushBack = F.method(N, VecPkg, "push_back", {A[0].Ty}, {});
            Vec->addOverload(PushBack, {nullptr, 0, true, 0});
            Overloaded = N;
            F.Prov.scopeOf("<vector>")
                .setSpelling(N, "std::vector<" + typeString(A[0].Ty) + ">");
            Inst = N->Obj;
          }
          return Inst;
        });

    AlgPkg = F.header("<algorithm>");
    Alg = &F.Prov.scopeOf("<algorithm>");
    // template<class T> T max(T, T): usable only with brackets or by
    // deduction in the real importer; the mock takes int32 for T.
    Func *Max = F.func(AlgPkg, "max", {I32, I32}, {I32});
    Max->IsTemplate = true;
    Alg->addOverload(Max, {llvm::cast<SignatureType>(Max->Ty), 0, false, 1});
    // int parse(const char*) and template<class T> T parse(const char*).
    Type *Str = F.t(BasicKind::String);
    Func *Parse = F.func(AlgPkg, "parse", {Str}, {I32});
    Parse->IsTemplate = true; // an overload set with a template
    Alg->addOverload(Parse,
                     {llvm::cast<SignatureType>(Parse->Ty), 0, false, 0});
    Alg->addOverload(Parse,
                     {F.sig({Str}, {F.t(BasicKind::Float64)}), 0, false, 1});
    // Only a template (usable with brackets only).
  }
  Fixture &F;
  Package *VecPkg, *AlgPkg;
  MockForeignScope *Vec, *Alg;
  std::map<Type *, Object *> Instances;
  Func *PushBack = nullptr;
  NamedType *Overloaded = nullptr;
};

TEST(Foreign, CppClassTemplateInstanceAndMethods) {
  Fixture F;
  Cpp C(F);
  EXPECT_TRUE(F.check(R"(package main
import "<vector>"
func main() {
	var v vector.vector[int32]
	var n int32 = v.size()
	v.push_back(3)
	_ = n
}
)")
                  .empty());
}

TEST(Foreign, CppInstantiationAndCallErrorsAreReported) {
  Fixture F;
  Cpp C(F);
  auto Errs = F.check(R"(package main
import "<vector>"
func main() {
	var bad vector.vector[int32, int32]
	var v vector.vector[int32]
	v.push_back("x", 2)
	_ = bad
}
)");
  EXPECT_TRUE(mentions(Errs, "cannot instantiate vector.vector[int32, int32]: "
                             "vector takes one type argument"));
  EXPECT_TRUE(mentions(Errs, "cannot call v.push_back: no matching function"));
  EXPECT_EQ(Errs.size(), 2u);
}

TEST(Foreign, CppTypesAreSpelledByTheScopeInMessages) {
  Fixture F;
  Cpp C(F);
  auto Errs = F.check(R"(package main
import "<vector>"
func main() {
	var v vector.vector[int32]
	var s string = v
	_ = s
}
)");
  ASSERT_EQ(Errs.size(), 1u);
  EXPECT_TRUE(mentions(Errs, "std::vector<int32>")) << Errs[0];
}

TEST(Foreign, NonConstMethodsNeedAnOwnedReceiver) {
  Fixture F;
  Cpp C(F);
  // `push_back` is not const: a borrowed vector cannot call it; `size` can be
  // called on it (ADR-0008 Q2).
  auto Errs = F.check(R"(package main
import "<vector>"
type Holder struct {
	v     vector.vector[int32]
	names []string
}
func readOnly(h Holder) int32 {
	h.v.push_back(1)
	return h.v.size()
}
func owned() {
	var h Holder
	h.v.push_back(1)
}
func main() {}
)");
  ASSERT_EQ(Errs.size(), 1u) << (Errs.empty() ? "" : Errs[0]);
  EXPECT_TRUE(mentions(Errs, "cannot call a non-const method on h.v"));
}

TEST(Foreign, ExplicitTemplateArgumentsPickTheTemplateOverload) {
  Fixture F;
  Cpp C(F);
  // ADR-0010 Q3: with brackets only templates are candidates; without them
  // the plain overload is chosen.
  EXPECT_TRUE(F.check(R"(package main
import "<algorithm>"
func main() {
	var a int32 = algorithm.parse("1")
	var b float64 = algorithm.parse[float64]("1")
	var c int32 = algorithm.max[int32](1, 2)
	_, _, _ = a, b, c
}
)")
                  .empty());
  auto Errs = F.check(R"(package main
import "<algorithm>"
func main() {
	var a float64 = algorithm.parse("1")
	var c = algorithm.max[int32, int32](1, 2)
	_, _ = a, c
}
)");
  EXPECT_TRUE(mentions(Errs, "as float64 value"));
  EXPECT_TRUE(mentions(Errs, "cannot call algorithm.max[int32, int32]"));
}

TEST(Foreign, FunctionTemplateInstanceIsNotAValue) {
  Fixture F;
  Cpp C(F);
  auto Errs = F.check(R"(package main
import "<algorithm>"
func main() {
	fn := algorithm.max[int32]
	_ = fn
}
)");
  ASSERT_EQ(Errs.size(), 1u);
  EXPECT_TRUE(mentions(Errs, "cannot use algorithm.max[int32] as a value"));
}

TEST(Foreign, ShortNamespaceNamesAndTheFullPathBothWork) {
  Fixture F;
  Package *P = F.header("<asio>");
  MockForeignScope &Root = F.Prov.scopeOf("<asio>");
  MockForeignScope &Ip = F.Prov.namespaceIn(Root, "ip", F.Mem);
  MockForeignScope &Tcp = F.Prov.namespaceIn(Ip, "tcp", F.Mem);
  Tcp.add(F.alias(P, "port", F.t(BasicKind::Uint16)));
  // The short form `asio.tcp` is unique: the importer adds the nested
  // namespace to the root too (ADR-0010 Q4).
  Root.add(Ip.lookup("tcp").Obj);
  EXPECT_TRUE(F.check(R"(package main
import "<asio>"
var a asio.tcp.port = 1
var b asio.ip.tcp.port = a
func main() { _ = b }
)")
                  .empty());
  auto Errs = F.check(R"(package main
import "<asio>"
var a asio.tcp
var b asio.ip.tcp.nothing
func main() {}
)");
  EXPECT_TRUE(mentions(Errs, "use of package asio.tcp without selector") ||
              mentions(Errs, "without selector"))
      << (Errs.empty() ? "" : Errs[0]);
  EXPECT_TRUE(mentions(Errs, "undefined: asio.ip.tcp.nothing"));
}

TEST(Foreign, AmbiguousShortNamesAreReportedOnUseOnly) {
  Fixture F;
  Package *P = F.header("<asio>");
  MockForeignScope &Root = F.Prov.scopeOf("<asio>");
  MockForeignScope &Ip = F.Prov.namespaceIn(Root, "ip", F.Mem);
  MockForeignScope &Local = F.Prov.namespaceIn(Root, "local", F.Mem);
  Ip.add(F.alias(P, "endpoint", F.t(BasicKind::Int)));
  Local.add(F.alias(P, "endpoint", F.t(BasicKind::Int)));
  Root.addAmbiguous(
      "endpoint",
      {{nullptr, "asio.ip.endpoint",
        "boost::asio::ip::endpoint, boost/asio/ip/endpoint.hpp:38"},
       {nullptr, "asio.local.endpoint",
        "boost::asio::local::endpoint, boost/asio/local/endpoint.hpp:20"}});
  // Unused ambiguity: the import is fine. The full path always works.
  EXPECT_TRUE(F.check(R"(package main
import "<asio>"
var a asio.ip.endpoint = 1
func main() { _ = a }
)")
                  .empty());
  auto Errs = F.check(R"(package main
import "<asio>"
var a asio.endpoint
func main() { _ = a }
)");
  ASSERT_EQ(Errs.size(), 1u);
  EXPECT_TRUE(mentions(Errs, "\"endpoint\" is ambiguous in package asio"));
  EXPECT_TRUE(
      mentions(Errs, "candidate asio.ip.endpoint (boost::asio::ip::endpoint"));
  EXPECT_TRUE(
      mentions(Errs, "help: write the full path, e.g. asio.ip.endpoint"));
}

} // namespace
