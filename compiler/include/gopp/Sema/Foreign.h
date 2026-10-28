//===- gopp/Sema/Foreign.h - Foreign (C/C++ header) scopes ------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// D3.7. `import "zlib.h"` and `import "<vector>"` make a package whose names
// come from a C/C++ header. The checker does not know Clang: everything it
// needs from such a package goes through ForeignScope, which the Clang
// importer implements in P07/P08. P03 ships MockForeignScope, so the checker
// can be tested with fake declarations.
//
// What the checker asks, and where:
//   lookup      `pkg.Name` and `pkg.ns.Name` (Call.cpp selector): flattened
//               namespaces, ambiguity on use only (ADR-0010).
//   checkCall   every call of a function or method whose package is foreign:
//               overload resolution, default arguments, explicit template
//               arguments `f[T](x)` (ADR-0010 Q3), and the const metadata that
//               tells which arguments the callee may change (ADR-0008 Q2).
//   instantiate `pkg.vec[int32]` in a type: C++ class template instances.
//   describe    how a foreign type is spelled in messages.
//   hasDefaultConstructor
//               whether a C++ type has a zero value (D1.4 rule 9): a type
//               without a default constructor is a type with no zero value,
//               like a struct with a `@Required` field (ADR-0016).
// Pointers (ADR-0018 point 8): a C `T*` is imported as Option[*T], a pointer
// the header marks non-null (`_Nonnull`, `nonnull`, `returns_nonnull`) as *T,
// and a C++ `T&` as *T; a C++ `T*` is Option[*T]. The importer builds these
// types with cPointer() and cxxReference() below, so the mapping is in one
// place.
// The objects a scope returns are owned by it; their types are made in the
// TypeContext of the checker (types are uniqued, so identity works across
// packages). A scope that needs more (a template instance made by Clang)
// creates the Type and returns it through the same TypeContext.
// Function bodies are checked on several threads (P03.10), so the checker may
// call one scope from several threads at once: an implementation must allow
// that (the mock answers from tables filled before the check).
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_SEMA_FOREIGN_H
#define GOPP_SEMA_FOREIGN_H

#include "gopp/Sema/Types.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gopp {
namespace sema {

/// One of several declarations a short name may mean (ADR-0010 Q4).
struct ForeignCandidate {
  Object *Obj = nullptr;
  /// The spelling that always works: `asio.ip.tcp`.
  std::string Spelling;
  /// Where it comes from: `boost::asio::ip::tcp (boost/asio/ip/tcp.hpp:38)`.
  std::string Origin;
};

/// The answer to a name lookup: one object, none, or several candidates (the
/// short form is ambiguous; the error is reported only where it is used).
struct ForeignLookup {
  Object *Obj = nullptr;
  std::vector<ForeignCandidate> Ambiguous;
  explicit operator bool() const { return Obj || !Ambiguous.empty(); }
};

/// An argument of a foreign call, as far as overload resolution needs it.
struct ForeignArg {
  /// Null for the untyped nil.
  Type *Ty = nullptr;
  SourceLoc Pos;
  bool Constant = false;
  bool Nil = false;
  const ConstValue *Val = nullptr;
  /// A read-only Go++ value: a borrowed parameter, a range variable, a `ref`
  /// value (ADR-0008 Q2). Filled in by the checker.
  bool ReadOnly = false;
};

/// An explicit template argument `f[T, 3](x)` (ADR-0010 Q3): a type, or a
/// constant (integer, bool, enum) that the importer converts to the template
/// parameter's type.
struct ForeignTemplateArg {
  Type *Ty = nullptr;
  const ConstValue *Val = nullptr;
  bool isType() const { return Ty && !Val; }
};

/// The result of checking a call.
struct ForeignCall {
  /// The signature of the chosen overload (with `!` / error results as the
  /// importer maps them); null if the call is invalid, with Error set.
  SignatureType *Sig = nullptr;
  std::string Error;
  /// One note per rejected candidate.
  std::vector<std::string> Notes;
  /// Bit i: the callee may change what parameter i points to (a non-`const`
  /// pointer or reference parameter). A read-only value cannot be passed there.
  uint64_t MutableParams = 0;
  /// A non-`const` C++ method: the receiver may change.
  bool MutableRecv = false;
};

struct ForeignInstance {
  /// A TypeName (class template instance) or Func; null with Error set.
  Object *Obj = nullptr;
  std::string Error;
};

class ForeignScope {
public:
  virtual ~ForeignScope() = default;

  /// Looks Name up in the header package. A foreign name is accessible
  /// whatever its case. A nested namespace is a PkgName (so `asio.ip.tcp`
  /// resolves one name at a time).
  virtual ForeignLookup lookup(llvm::StringRef Name) = 0;

  /// Checks a call of Callee (a Func of this scope, or a method of one of its
  /// types). Recv is the receiver of a method call. The default accepts the
  /// declared signature, without overloads and without const metadata.
  virtual ForeignCall checkCall(Object *Callee,
                                llvm::ArrayRef<ForeignTemplateArg> Explicit,
                                const ForeignArg *Recv,
                                llvm::ArrayRef<ForeignArg> Args);

  /// Instantiates a template object (IsTemplate) with the given arguments.
  virtual ForeignInstance instantiate(Object *Tmpl,
                                      llvm::ArrayRef<ForeignTemplateArg> Args);

  /// How a type of this scope is spelled in messages (`std::vector<int>`);
  /// empty for the default Go++ spelling.
  virtual std::string describe(const Type *T) const;

  /// The element type of a C++ container T that has `begin()` / `end()`
  /// (RNG-6, P08 D8.6): the type of `x` in `for _, x := range c`. Key is set
  /// to the key type of an associative container (`std::map`) and left null
  /// for the index of an indexable one. Null when T is not a container.
  virtual Type *rangeElem(const Type *T, Type **Key) const;

  /// Whether the C++ type T can be default-constructed. A foreign type that
  /// cannot has no zero value: `var x T`, `make([]T, n)` and the other places
  /// that need one are errors (OWN-R9). The default says yes.
  virtual bool hasDefaultConstructor(const Type *T) const;

  /// The Go++ type of a C pointer `Pointee*`: Option[*Pointee], or *Pointee
  /// when the header says it is never null.
  static Type *cPointer(TypeContext &Ctx, Type *Pointee, bool NonNull);
  /// The Go++ type of a C function pointer, whose `ffi.Func[...]` type is
  /// Func: Option[Func], or Func when the header says it is never null
  /// (ADR-0018 point 8; a Go++ func is never nil, so a C function pointer that
  /// may be NULL is an Option, like every C pointer).
  static Type *cFuncPointer(TypeContext &Ctx, Type *Func, bool NonNull);
  /// The Go++ type of a C++ reference `Pointee&` (it cannot be null).
  static Type *cxxReference(TypeContext &Ctx, Type *Pointee);
};

/// A ForeignScope with declarations added by hand, for tests.
class MockForeignScope : public ForeignScope {
public:
  /// Adds an object under its own name. The mock does not own it.
  void add(Object *O) { Objs[O->Name] = O; }
  /// Adds one overload of a function: the mock resolves a call to the first
  /// overload of the name that accepts the arguments (a constant converts to
  /// a basic parameter type, nil to a pointer; everything else is identity).
  struct Overload {
    SignatureType *Sig = nullptr;
    uint64_t MutableParams = 0;
    bool MutableRecv = false;
    /// Template parameters (names); Explicit arguments must not exceed them.
    unsigned NumTemplateParams = 0;
  };
  void addOverload(Func *F, Overload O);
  /// A class template: instantiate() calls Make with the arguments.
  using TemplateFn = std::function<Object *(llvm::ArrayRef<ForeignTemplateArg>,
                                            std::string &Error)>;
  void addTemplate(Object *Tmpl, TemplateFn Make);
  /// Two objects of one name in different namespaces (short-name ambiguity).
  void addAmbiguous(llvm::StringRef Name, std::vector<ForeignCandidate> Cands);
  /// Marks a C++ type as having no default constructor.
  void setNoDefaultConstructor(const Type *T) { NoDefault.insert(T); }
  bool hasDefaultConstructor(const Type *T) const override {
    return !NoDefault.count(T);
  }
  /// Declares the C++ container T rangeable with elements of type Elem (and
  /// keys of type Key for an associative one).
  void setRangeElem(const Type *T, Type *Elem, Type *Key = nullptr) {
    RangeElems[T] = {Elem, Key};
  }
  Type *rangeElem(const Type *T, Type **Key) const override {
    auto It = RangeElems.find(T);
    if (It == RangeElems.end())
      return nullptr;
    if (Key)
      *Key = It->second.second;
    return It->second.first;
  }
  /// The spelling of a type in messages.
  void setSpelling(const Type *T, std::string Text) {
    Spellings[T] = std::move(Text);
  }

  ForeignLookup lookup(llvm::StringRef Name) override;
  ForeignCall checkCall(Object *Callee,
                        llvm::ArrayRef<ForeignTemplateArg> Explicit,
                        const ForeignArg *Recv,
                        llvm::ArrayRef<ForeignArg> Args) override;
  ForeignInstance instantiate(Object *Tmpl,
                              llvm::ArrayRef<ForeignTemplateArg> Args) override;
  std::string describe(const Type *T) const override;

private:
  llvm::StringMap<Object *> Objs;
  llvm::StringMap<std::vector<ForeignCandidate>> Ambiguous;
  llvm::DenseMap<const Object *, std::vector<Overload>> Overloads;
  llvm::DenseMap<const Object *, TemplateFn> Templates;
  llvm::DenseMap<const Type *, std::string> Spellings;
  llvm::DenseSet<const Type *> NoDefault;
  llvm::DenseMap<const Type *, std::pair<Type *, Type *>> RangeElems;
};

/// Provides the packages made from C/C++ headers (P03.09).
class ForeignProvider {
public:
  virtual ~ForeignProvider() = default;
  /// The package for the header path (e.g. `<vector>`), or null with Error.
  virtual Package *header(llvm::StringRef Path, Arena &Mem,
                          std::string &Error) = 0;
};

/// Makes foreign packages with a MockForeignScope per header.
class MockForeignProvider : public ForeignProvider {
public:
  Package *header(llvm::StringRef Path, Arena &Mem,
                  std::string &Error) override;
  /// The scope of a header (created on demand), to add objects to.
  MockForeignScope &scopeOf(llvm::StringRef Path);
  /// A namespace nested in Parent: a foreign package reached as
  /// `pkg.<Name>.x`. Created on demand, owned by the provider; Mem owns the
  /// package and the PkgName object that Parent gets.
  MockForeignScope &namespaceIn(MockForeignScope &Parent, llvm::StringRef Name,
                                Arena &Mem);

private:
  std::map<std::string, std::unique_ptr<MockForeignScope>> Scopes;
  std::vector<std::unique_ptr<MockForeignScope>> Nested;
};

} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_FOREIGN_H
