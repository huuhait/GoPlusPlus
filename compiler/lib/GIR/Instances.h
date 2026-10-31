//===- Instances.h - The instances of generic code (P04.08, D4.3) ---------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Private to lib/GIR. The instance walk is the ONE place that decides which
// instances of generic functions and methods a package needs, in which order,
// under which symbol, and whether each is valid. Both users run it:
//
//   - gir::lowerPackage declares and lowers exactly the instances it lists;
//   - `gopp check` (gir::checkInstances) runs it for the per-instance errors
//     (D4.3), without any GIR.
//
// It works on the checker's records, never on GIR: from every non-generic
// function body (the roots, in declaration order) and then from every
// instance found, in the order found (a queue), it walks the body once and
// takes
//
//   - each generic function named in the body (Info::Instances), with its type
//     arguments substituted by the instance being walked (the checker's own
//     substImpl, sema::MethodSets::subst);
//   - each method a selector names whose receiver is an instance of a generic
//     type (sema::MethodSets::resolveSelection: the lookup plain code does,
//     run on the instance's receiver type, so a method of a type parameter's
//     constraint is the concrete type's method);
//   - the whole method set of an instance of a generic type (or a pointer to
//     one) whose values become interface values (Info::IfaceConversions: the
//     checker's record of every assignment, conversion and comparison that
//     makes one) or that a type assertion or a type switch case tests: the
//     lowering makes its type descriptor there, which lists the method set
//     (Lowering::typeInfo). So `gopp check` lists the same methods as the
//     build. An instance of a generic type that is only named, like `*W[S]`
//     in a signature, brings no method with it, so n methods of n instances
//     are not n squared. (The lowering still asks for a method it lacks,
//     needMethod, but the walk has listed it.)
//
// The result is deterministic at any `-check-j`: the order is the order of the
// source and of the checker's declaration order, nothing depends on pointers
// or threads, and an instance is keyed by its symbol (P04.17), which is a
// function of the content of its type arguments (`Box[byte]` and `Box[uint8]`
// are one instance).
//
// Units (GEN-14, P04.24): a generic function is one body for every unit
// argument (its unit parameters are the empty unit at the instance). An
// instance it asks for with a type argument that carries one of those units
// is a shared instance (GenericInst::Shared), its own instance with its own
// symbol, and every decision that needs such a unit asks one predicate,
// mentionsErasedUnit, and is refused (E0439) where it cannot be made: the
// list of routes and decisions is in gir.md section 10.
//
// Per-instance checks (ADR-0018 point 11, ADR-0020, OWN-P10) run on the
// substituted types of an instance with the checker's own predicates:
// `Option[X]` with a nilable or Option X (E0446), `View[X]` with a pointer
// kind, unsafe.Pointer or View X (E0447), and a call whose last result is a
// type parameter that is `error` at the instance while another result has no
// zero value (E0448: the checker's guard rule and what `!` reads, on the
// instance's types). One error per instance, at the first instantiation
// site, with a note at the generic code and one for each instantiation that
// led there (at most four).
//
// Limits (GEN-5, E0449): a type argument that an instance made by
// substitution (it names a type parameter where the generic code writes it)
// nests at most MaxArgHeight levels and has at most MaxArgSize parts as a
// tree; a type written in the source is never past them (it is as large as
// the source). A package has at most LowerOptions::MaxInstances instances, or
// one per byte of its source if that is more, and the walk visits at most
// LowerOptions::MaxWalkedNodes nodes of code, or 16 per byte of the source.
// The first instance or node past a limit is the one error; the walk then
// makes no more instances, so an input that would expand without end stops at
// once.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_LIB_GIR_INSTANCES_H
#define GOPP_LIB_GIR_INSTANCES_H

#include "Symbols.h"

#include "gopp/AST/AST.h"
#include "gopp/Basic/Diagnostics.h"
#include "gopp/Sema/Sema.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

#include <memory>
#include <optional>
#include <set>
#include <string>

namespace gopp {
namespace gir {
namespace impl {

namespace S = gopp::sema;

/// One instance of generic code: a generic function, or a method of an
/// instance of a generic type.
struct GenericInst {
  /// The declared function or method (its AST, parameters and result
  /// objects are the generic ones).
  const S::Func *Decl = nullptr;
  /// A method of a generic type: the instantiated method (the object a
  /// selector of the instance's receiver finds). Null for a generic function.
  const S::Func *Method = nullptr;
  /// Every other object that is this instance: the same method found on
  /// another spelling of the same instance (`Box[byte]` and `Box[uint8]` are
  /// two checker types and one symbol).
  llvm::SmallVector<const S::Func *, 1> Aliases;
  /// The type arguments: the receiver's, then the function's own.
  llvm::SmallVector<S::Type *, 4> TArgs;
  /// The type parameters of Decl (the receiver's, then its own) to TArgs.
  S::MethodSets::TypeMap Map;
  /// The unit parameters whose run-time descriptor the instance takes as a
  /// hidden parameter after the others, in this order (the parameters with
  /// Use::UnitDict; a generic function only, GEN-14).
  llvm::SmallVector<const S::TypeParamType *, 2> DictParams;
  /// The position of each in the type arguments.
  llvm::SmallVector<unsigned, 2> DictArgIdx;
  /// Where the first user named this instance, and who that was.
  SourceLoc Site;
  const GenericInst *From = nullptr;
  /// The instantiations between plain code and this instance (0: plain code
  /// asked for it).
  unsigned Depth = 0;
  /// The symbol (D4.13), which is also the key.
  std::string Symbol;
  /// An error was reported for this instance: it is not walked or lowered.
  bool Bad = false;
  /// GEN-14 (P04.24): the type parameters whose type arguments carry a unit
  /// of the generic code that asked for the instance, a body shared across
  /// unit arguments (its unit parameters are the empty unit there, so `W[U]`
  /// is `W[]` and `float64[U]` is `float64`): bit K for type parameter K (the
  /// receiver's, then the function's own; bit 63 for 63 and up). Decided from
  /// where the instance came from, never from the value of a unit: plain code
  /// makes `W[KM*IKM]` (the empty unit too) and `float64`. Part of the key and
  /// of the symbol (sym::Kind::SharedUnits), so such an instance is never the
  /// one plain code makes, in any order.
  uint64_t Shared = 0;
  /// The type parameters the instance does not know the unit of (the one
  /// predicate, InstanceWalk::mentionsErasedUnit): those in Shared, and every
  /// unit parameter of a generic function (its body is one for every unit).
  llvm::SmallPtrSet<const S::TypeParamType *, 4> Erased;
  /// Those in Shared alone.
  llvm::SmallPtrSet<const S::TypeParamType *, 4> FromCaller;
  /// With Shared: the call in the outermost body shared across unit
  /// arguments that the units came from (where a decision that needs them is
  /// refused, InstanceWalk::refuseErased).
  SourceLoc Origin;
  /// The predicates' memos (types as the generic code wrote them).
  mutable llvm::DenseMap<const S::Type *, bool> ErasedMemo, CallerMemo;
  /// Its position in the list of instances.
  size_t Index = 0;
  /// Substituted types and selections, per instance (the walk and the
  /// lowering ask the same questions).
  llvm::DenseMap<const S::Type *, S::Type *> Types;
  llvm::DenseMap<const ast::SelectorExpr *, std::optional<S::Selection>> Sels;
};

/// Gives every type declared in a plain function its symbol (the function's
/// node and the declaration's ordinal among the function's types, in source
/// order; Symbols::Names.declareLocalType), before the walk names an instance
/// with such a type as a type argument and before any body is lowered. A type
/// in a function literal (of a function or of a package variable's
/// initializer) is named by the literal's closure node, as the closure is.
/// The one place that numbers them, for `gopp check` and the build. A type
/// in generic code is not numbered here (P04.28).
void declareLocalTypes(const S::CheckedPackage &CP,
                       llvm::ArrayRef<const ast::File *> Files, Symbols &Syms);

class InstanceWalk {
public:
  /// The deepest nest of a type argument (`F[[]T]` calling itself would make
  /// one without end; the checker rejects the plain cases as instantiation
  /// cycles, Mono.cpp, this bounds what it cannot see).
  static constexpr unsigned MaxArgHeight = 48;
  /// The most parts of a type argument as a tree (a chain of generics that
  /// each call the next with `Pair[T, T]` doubles it at every level): every
  /// name of an instance, Go's type name of its descriptor among them, is
  /// then bounded too.
  static constexpr uint64_t MaxArgSize = 4096;

  /// Quiet: `gopp check`; the instances the lowering cannot name or take
  /// (E0439) are left to it, only the per-instance errors are reported.
  /// MaxInstances, MaxWalked: LowerOptions::MaxInstances, MaxWalkedNodes.
  InstanceWalk(const S::CheckedPackage &CP,
               llvm::ArrayRef<const ast::File *> Files, S::MethodSets &MS,
               Symbols &Syms, DiagnosticsEngine &Diags, bool Quiet,
               size_t MaxInstances, uint64_t MaxWalked);

  /// Walks the package. False after reporting an error.
  bool run();
  /// The text `F[int, string]`, `Pair[int, string].Swap` (also for the
  /// lowering's messages about an instance).
  std::string nameOf(const GenericInst &In);

  llvm::ArrayRef<std::unique_ptr<GenericInst>> list() const { return Insts; }

  /// T, a type from Info, at the instance Cur (null: plain code): the
  /// type arguments applied, every instance expanded. The one way the
  /// lowering reads a checker type.
  S::Type *inst(GenericInst *Cur, S::Type *T);
  /// The selection the checker recorded for E, resolved at the instance
  /// (the plain code's own lookup on the instance's receiver); null if there
  /// is none.
  const S::Selection *selection(GenericInst *Cur, const ast::SelectorExpr *E);
  /// The generic function instance that the identifier or selector Key names
  /// in the code of Cur (Key is an Info::Instances key); null if none.
  const GenericInst *funcRef(const GenericInst *Cur,
                             const ast::Expr *Key) const;
  /// The instance a method of an instance of a generic type is (any of its
  /// spellings); null for a method of a plain type.
  const GenericInst *methodInst(const S::Func *F) const;
  /// The symbol node of In (sym::Kind::SharedUnits around it with a Shared
  /// mask): its closures are named by it.
  const sym::Node *nodeOf(const GenericInst &In);

  /// THE predicate of GEN-14's shared bodies (P04.24): does T, a type as the
  /// code of Cur writes it, mention a unit Cur does not know (a unit
  /// parameter of a generic function, or a type parameter whose argument
  /// carries a unit of the generic code that asked for Cur: GenericInst::
  /// Erased)? Every decision that depends on a unit asks it, in the walk and
  /// in the lowering: a type descriptor (boxing, a type assertion or switch,
  /// `==` of interfaces, a map key of interface type), a method that needs
  /// the unit of its type, an instance asked for (it is Shared then), a
  /// collapse site, an integer quotient, a descriptor passed on, a debugger
  /// name. Null Cur (plain code): false.
  bool mentionsErasedUnit(const GenericInst *Cur, S::Type *T);
  /// The same for one type parameter.
  static bool erasedParam(const GenericInst *Cur, const S::TypeParamType *P);

  /// The method M of an instance of a generic type as an instance, asked for
  /// by the lowering when it needs the method without a call that names it
  /// (a type descriptor or an itab lists the whole method set; the walk has
  /// taken those already, see the file comment): the walk takes it and what
  /// its body needs. Null after reporting an error (or when M is no method of
  /// an instance).
  const GenericInst *needMethod(const S::Func *M, SourceLoc Site);

  /// Steps taken (test-only counting, M8).
  uint64_t steps() const { return Steps; }
  /// Test-only: look an instance up by a scan of the list (the control of the
  /// stress test, LowerOptions::ScanInstances).
  bool ScanTable = false;
  /// Test-only: take a type's method set at every conversion, not once (the
  /// control of the stress test, LowerOptions::RedescribeTypes).
  bool Redescribe = false;
  /// Test-only: mentionsErasedUnit walks the type at every question, not
  /// once per instance (the control of the stress test, LowerOptions::
  /// ForgetErasedUnits).
  bool ForgetErased = false;

private:
  /// An `Option[X]` or `View[X]` that is no type at an instance.
  struct Problem {
    enum Kind : uint8_t { Option, View } K;
    S::Type *Formed; // the Option[X] or View[X]
    const char *Why;
  };
  /// How the body uses the results of a call (checkedResult).
  struct ResultUse {
    /// `return f()` into the function's own results, as many.
    bool Forward = false;
    /// The results dropped (`_`, or a call used as a statement), by index.
    uint64_t Dropped = 0;
  };
  const S::CheckedPackage &CP;
  const S::Info &I;
  llvm::ArrayRef<const ast::File *> Files;
  S::MethodSets &MS;
  Symbols &Syms;
  DiagnosticsEngine &Diags;
  bool Quiet;
  bool Failed = false;
  /// A limit was reached (E0449, said once): no more instances are made.
  bool Stopped = false;
  size_t MaxInstances;
  /// The nodes of code walked in all, and the most: the plain code and the
  /// bodies of the instances, which the lowering makes into GIR too.
  uint64_t Walked = 0;
  uint64_t MaxWalked;
  /// The instances whose bodies have been walked.
  size_t Scanned = 0;
  uint64_t Steps = 0;
  std::vector<std::unique_ptr<GenericInst>> Insts;
  llvm::StringMap<GenericInst *> BySymbol;
  /// The function instances by their symbol node.
  llvm::DenseMap<const sym::Node *, GenericInst *> ByNode;
  /// The method instances by method and Shared mask; ByMethod has the one
  /// with real units if there is one (methodInst), else the first.
  llvm::DenseMap<std::pair<const S::Func *, uint64_t>, GenericInst *> ByKey;
  llvm::DenseMap<const S::Func *, GenericInst *> ByMethod;
  /// Mentions T (a type of Cur's code as written) a unit of Cur's that comes
  /// from a caller (GenericInst::FromCaller)?
  bool fromCaller(const GenericInst *Cur, S::Type *T);
  /// One error for a decision in the code of Cur, at At, that needs a unit
  /// that T (as Cur's code writes it) mentions and Cur does not know (What
  /// says which): at the call it came from (Cur->Origin) if a caller shared
  /// it, else at At. The instance is not walked further.
  void refuseErased(GenericInst *Cur, SourceLoc At, S::Type *T,
                    const std::string &What);
  /// The symbol node of the instance (its Shared mark included).
  const sym::Node *nodeOf(const S::Func *Decl, const S::Func *M,
                          llvm::ArrayRef<S::Type *> TArgs, uint64_t Shared);

  llvm::DenseMap<std::pair<const GenericInst *, const ast::Expr *>,
                 const GenericInst *>
      Refs;
  /// Types met (the walk of a type is done once), and what each reaches that
  /// is invalid at an instance: null for clean.
  llvm::DenseMap<const S::Type *, const Problem *> Taint;
  std::vector<std::unique_ptr<Problem>> TaintProblems;
  /// The types whose method sets have been taken (their values become
  /// interface values, or a type test names them).
  llvm::DenseSet<const S::Type *> Described;
  /// The types whose glue's methods userGlue has requested.
  llvm::DenseSet<const S::Type *> GlueAsked;
  std::set<std::pair<unsigned, std::string>> Said;
  llvm::DenseMap<const ast::CallExpr *, ResultUse> ResultUses;

  /// Walks the bodies of the instances found and not walked yet.
  void drain();
  void scanFunc(GenericInst *Cur, const ast::FuncDecl *FD, const S::Func *Obj);
  void scanTree(GenericInst *Cur, const ast::Node *Root, const S::Func *Obj);
  void visitExpr(GenericInst *Cur, const ast::Expr *E);
  /// A value of T (Generic as the code writes it) in a body shared across
  /// unit arguments: the `drop` and `Clone() T` methods the program wrote
  /// that its glue calls are those of the types the instance names (`Rec[U]`
  /// is `Rec[]`), so each is requested as a call on a receiver of unknown
  /// units would be, and its walk refuses what needs the unit (P04.09 review
  /// R17). Each type is walked once (GlueAsked).
  void userGlue(GenericInst *Cur, S::Type *Generic, S::Type *T, SourceLoc At);
  /// The method calls an operator expression or statement stands for
  /// (Info::Operators, P04.23) are no node of the tree: their selectors are
  /// met here, so a method of an instance of a generic type is requested.
  void visitOperator(GenericInst *Cur, const ast::Node *N);
  /// A copy the language makes at N (Info::ImplicitClones) of a value whose
  /// type is a type parameter's: at this instance it must be cloneable, by
  /// the rule of the plain code (E0453, P04.09 R5).
  void implicitClone(GenericInst *Cur, const ast::Node *N);
  void onInstance(GenericInst *Cur, const ast::Expr *E, const S::Instance &In);
  void onSelection(GenericInst *Cur, const ast::SelectorExpr *E);
  /// (FnSig: the signature, as written, of the function or function literal
  /// whose body holds the call or the statement; null outside any.)
  void checkedResult(GenericInst *Cur, const ast::CallExpr *C,
                     S::SignatureType *FnSig);
  /// Records how a statement uses the results of the call it holds.
  void noteResultUse(const ast::Node *N, S::SignatureType *FnSig);
  /// The value of type T (at Cur) becomes an interface value, or a type test
  /// names T: the lowering makes T's type descriptor, which lists T's method
  /// set, so the methods of an instance of a generic type are taken.
  /// (Generic: T as the generic code wrote it, before the substitution; it
  /// says which type arguments of T an instance made, madeArg.)
  void described(GenericInst *Cur, S::Type *T, S::Type *Generic, SourceLoc At);
  /// Walks T (substituted): reports (once per instance) what is invalid in it
  /// at Cur, and an instance of a generic type in it past the limits.
  void meet(GenericInst *Cur, S::Type *T, SourceLoc At);
  /// Generic: the type arguments as the code that names the instance wrote
  /// them (Info::Instances; empty for none), so only those made by
  /// substitution are bounded.
  GenericInst *requestFunc(GenericInst *Cur, const S::Func *Decl,
                           llvm::ArrayRef<S::Type *> TArgs, SourceLoc Site,
                           llvm::ArrayRef<S::Type *> Generic = {});
  /// GenericRecv: the receiver type as the code that names the method wrote
  /// it (null: none, the lowering's request), for madeArg. AllShared: every
  /// unit argument of the receiver counts as one Cur does not know (userGlue).
  GenericInst *requestMethod(GenericInst *Cur, const S::Func *M, SourceLoc Site,
                             S::Type *GenericRecv = nullptr,
                             bool AllShared = false);
  GenericInst *make(GenericInst *Cur, const S::Func *Decl, const S::Func *M,
                    llvm::ArrayRef<S::Type *> TArgs, std::string Symbol,
                    SourceLoc Site, uint64_t Shared, SourceLoc Origin);
  /// True (after E0449) if a type argument of Name that an instance made
  /// (Made says which) is past the limits. A type written in the source is
  /// never too large: it is as large as the source is.
  bool argsTooLarge(GenericInst *Cur, llvm::ArrayRef<S::Type *> TArgs,
                    llvm::function_ref<bool(size_t)> Made, SourceLoc Site,
                    const std::string &Name);
  /// Does the type Generic, as the code of the instance Cur writes it, make a
  /// type at Cur by substitution: it names a type parameter inside a larger
  /// type (`[]T`, `Pair[T, T]`; not `T` itself, passed on as it is)?
  bool madeType(GenericInst *Cur, S::Type *Generic);
  /// Is type argument K of the instance of a generic type that the code of Cur
  /// writes as Generic (behind pointers and Options) made at Cur (madeType)?
  bool madeArg(GenericInst *Cur, S::Type *Generic, size_t K);
  /// Go++'s spelling of T in a message, cut short for a type past the size
  /// limit (its full text could be as long as the tree is large).
  std::string typeText(S::Type *T);
  /// E0446 or E0447 for the instance Cur: P at At in its generic code.
  void invalid(GenericInst *Cur, SourceLoc At, const Problem &P);
  /// The notes of an instance error: At in the generic code (What says
  /// what is there), then the instantiations that led to Cur.
  void notes(DiagnosticBuilder &B, const GenericInst &Cur, SourceLoc At,
             const char *What);
  /// A note for each instantiation from From up (at most four, then one
  /// that counts the rest).
  void chainNotes(DiagnosticBuilder &B, const GenericInst *From);
  /// E0449 for the instance of Name asked for at Site: Why it is too large.
  /// Said once; the walk stops.
  void tooLarge(GenericInst *Cur, SourceLoc Site, const std::string &Name,
                const std::string &Why);
  void unsupported(SourceLoc At, const std::string &What);
  bool isOurs(const S::Func *F) const;
};

/// The parts of a value of T that its drop and clone glue walk (gir.md
/// section 7): a struct's fields, an array's or a slice's elements, a map's
/// keys and values, an Option's value, a tuple's elements. A pointer, a view,
/// an interface and a func value have none here (their glue is no program's).
void forEachGluePart(S::Type *T, llvm::function_ref<void(S::Type *)> Fn);

} // namespace impl
} // namespace gir
} // namespace gopp

#endif // GOPP_LIB_GIR_INSTANCES_H
