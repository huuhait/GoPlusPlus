//===- gopp/CodeGen/DebugInfo.h - DWARF for the code generator --*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.12 (D4.7), docs/design/debug-info.md. The one place that makes debug
// metadata: the code generator (LowerToLLVM.cpp) holds a DebugInfoBuilder and
// never calls llvm::DIBuilder itself. It makes
//
//   - the compile unit, the files and the DWARF 5 module flags;
//   - one DISubprogram per function, named by its readable form
//     (`main.Vec2.+(main.Vec2)`, sym::demangle); the `_GP` symbol is the
//     linkage name only on request (debuggers would show it instead);
//   - the scopes of a function (gir::DebugScope): a DILexicalBlock for a
//     block of the source that holds a variable, and for the code GIR
//     inlining copied the callee's subprogram with a real `inlinedAt`
//     location at the call;
//   - the DILocation of every statement, in its scope;
//   - one DILocalVariable per variable of the program (gir::Local::Source),
//     declared at the local's storage with dbg.declare (LLVM's own passes turn
//     it into dbg.value where they promote the slot);
//   - the DIGlobalVariable of every package variable;
//   - the type of every value: typeOf(), the single mapping from a GIR type
//     to a DIType.
//
// Later tasks add a case to typeOf() (and nothing else here) when they add a
// GIR type: P04.06 interfaces and itabs, P04.07 func values (done), P04.26
// defer frames, P04.08 generic instance names, P04.22 `extends` parents, P04.24
// units. A name the GIR type does not hold is recorded on it by the GIR
// lowering (gir::Type::fieldName), next to lowerType.
//
// Limits that keep the work linear and LLVM's own recursion bounded (LLVM's
// DWARF writer recurses once per level of a type and of a scope): a type is
// at most about MaxTypeHeight levels high (a part higher than that is referred
// to through a declaration; see target), lexical blocks nest at most
// MaxBlockDepth deep, a name is at most MaxNameLength bytes.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_CODEGEN_DEBUGINFO_H
#define GOPP_CODEGEN_DEBUGINFO_H

#include "gopp/Basic/NewStack.h"
#include "gopp/GIR/GIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Module.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gopp {

class DebugInfoBuilder {
public:
  /// MemTy: the LLVM type a GIR type has in memory (the code generator's
  /// memTy); the sizes and field offsets of the DWARF types are the data
  /// layout's, so they are the real ones.
  /// LinkageNames: also write the symbol as DW_AT_linkage_name (see
  /// LowerToLLVMOptions::DwarfLinkageNames).
  /// Optimized: the code is optimized (`opt` above 0): the compile unit and
  /// the subprograms say so, and a debugger expects values to be missing.
  DebugInfoBuilder(llvm::Module &LM, const gir::Module &GM,
                   std::function<llvm::Type *(const gir::Type *)> MemTy,
                   bool LinkageNames, bool Optimized);

  /// The compile unit, the files, the module flags. Once, first.
  void start();
  /// Finishes the metadata. Once, last.
  void finish();

  /// The DWARF type of a GIR type, made once per type. THE mapping point
  /// (see the file comment). Never null. A pointer refers to a struct by a
  /// declaration of its name, whose definition the unit keeps (target), so
  /// the definitions form no cycle; a part higher than MaxTypeHeight is
  /// referred to the same way. What a type is depends on the type alone,
  /// never on which type was asked for first, so a type is made once and the
  /// work is linear.
  llvm::DIType *typeOf(const gir::Type *T);
  /// The type as the debugger spells it: Go++ names (`main.Doc`, `[]string`,
  /// `*main.Doc`, `Option[int64]`, `(int64, bool)`, `func(int64) bool`,
  /// `struct { a int64 }`). GIR does not tell `int` from `int64` or `byte` from
  /// `uint8`: the integer is spelled by its width and signedness. A name
  /// longer than MaxNameLength is cut and ends in `...#H`, H a hash of the
  /// whole type, so two types that differ past the cut have two names. (Two
  /// unnamed structs of one spelling have one layout: one DWARF type.)
  std::string spelling(const gir::Type *T);

  /// The subprogram of a function with a body (made on the first ask, also
  /// for an inlined callee that is lowered later).
  llvm::DISubprogram *subprogramOf(gir::FuncId Id);
  /// The readable form of a symbol: sym::demangle of a `_GP` name, else the
  /// name itself.
  static std::string readableName(llvm::StringRef Symbol);

  /// A package variable. Constant data (read-only globals) and the
  /// compiler's own variables (no source position) have none.
  void declareGlobal(const gir::Global &G, llvm::GlobalVariable *GV);

  /// The function being lowered: attaches its subprogram and makes the table
  /// of its scopes (one step per scope); code with no source position goes
  /// to NoSourceSection.
  void beginFunction(gir::FuncId Id, llvm::Function &LF);
  /// The location for a position of the current function, in scope Scope
  /// (gir::DebugScope; 0: the function itself). A position of another file
  /// than the one of the function that holds the scope (the callee, for
  /// inlined code), and none, give line 0 (code the compiler made).
  llvm::DILocation *location(const gir::Span &S, gir::ScopeId Scope);
  /// Declares local L of the current function, stored at Addr, before
  /// InsertBefore (the end of the entry block). Nothing for a local that is
  /// no user variable. An aggregate parameter (Addr is the caller's address,
  /// an llvm::Argument) gets a slot of its own that holds the address, read
  /// through `DW_OP_deref`, as Clang does for an indirect argument. Returns
  /// the slot that the `opt 0` pipeline must keep in memory for the debugger
  /// (LowerToLLVMOptions::KeepVariables), or null: every variable of the
  /// function itself, and the copies of an inlined callee's variables of at
  /// most MaxKeptBits (a 512 KB parameter inlined 40 times must not be 40
  /// frame slots, run/core-argslots.test).
  llvm::Value *declareLocal(gir::LocalId L, llvm::Value *Addr,
                            llvm::Instruction *InsertBefore);
  /// Is local L of the current function a variable the debugger shows (a
  /// named parameter or variable of the program, not a temporary)? For a copy
  /// GIR inlining made, the callee's local and position decide.
  bool isUserVariable(gir::LocalId L) const;

  /// Non-zero if the nesting of types was too deep for the stacks that
  /// could be had (the depth, as DeepWalk::failedAt()).
  unsigned failedAt() const { return Walk.failedAt(); }

  /// The section of the code that has no source position (beginFunction).
  static constexpr const char *NoSourceSection = ".text.gopp_nosource";

  /// The limits of the file comment.
  static constexpr unsigned MaxTypeHeight = 48;
  static constexpr unsigned MaxBlockDepth = 64;
  static constexpr size_t MaxNameLength = 200;
  static constexpr uint64_t MaxKeptBits = 4096 * 8;

  /// Test-only (M8): the types, variables, locations and scopes visited so
  /// far, to prove linear work.
  struct Counts {
    uint64_t TypeSteps = 0, VariableSteps = 0, LocationSteps = 0,
             ScopeSteps = 0;
  };
  const Counts &counts() const { return Work; }

private:
  llvm::Module &LM;
  const gir::Module &GM;
  llvm::LLVMContext &C;
  llvm::DIBuilder DIB;
  bool LinkageNames, Optimized;
  std::function<llvm::Type *(const gir::Type *)> MemTy;
  llvm::SmallVector<llvm::DIFile *, 4> Files;
  llvm::DICompileUnit *CU = nullptr;

  /// What typeOf made of a type: the definition and its height (a basic type
  /// and a declaration are 1); for an array, what a pointer to it refers to
  /// (target) and its height; the declaration of its name; whether it is
  /// being made, whether its definition is kept in the unit (a struct), and
  /// whether a struct of its name holds it (a pointer, array or function).
  struct TypeInfo {
    llvm::DIType *Full = nullptr;
    llvm::DIType *Target = nullptr;
    llvm::DIType *Decl = nullptr;
    unsigned Height = 0;
    unsigned TargetHeight = 0;
    bool Making = false;
    bool Kept = false;
    bool Wrapped = false;
  };
  llvm::DenseMap<const gir::Type *, TypeInfo> Types;
  llvm::DenseMap<const gir::Type *, std::string> Spelled;
  llvm::DenseMap<const gir::Type *, uint64_t> Hashes;
  llvm::DenseMap<const gir::Type *, uint64_t> ArraySize;
  /// The structs whose declarations stand for them, still to be made, and
  /// the definitions the unit keeps (finish retains them by name).
  std::vector<const gir::Type *> Pending;
  std::vector<llvm::DIType *> Kept;
  DeepWalk Walk;
  Counts Work;

  /// What a function's own scopes are in DWARF (made on the first ask): per
  /// ScopeId, the subprogram (0), the lexical block of a block that holds a
  /// variable, or the nearest enclosing one; null for a scope that is not the
  /// function's own (an inline site and what it holds). Per local (empty if
  /// none is): whether it is a variable shown with no location, as it shares
  /// a block past MaxBlockDepth with others and has a name another has.
  struct FuncInfo {
    llvm::DISubprogram *SP = nullptr;
    std::vector<llvm::DIScope *> Native;
    std::vector<uint8_t> Hidden;
    bool HasNative = false;
  };
  llvm::DenseMap<gir::FuncId, FuncInfo> Funcs;
  llvm::DenseMap<uint64_t, llvm::DILocalVariable *> Variables;

  // The current function: per scope, the DWARF scope, the inlinedAt location
  // of the inline site it is in (null: none), and the function whose code it
  // holds.
  struct ScopeInfo {
    llvm::DIScope *Scope = nullptr;
    llvm::DILocation *InlinedAt = nullptr;
    gir::FuncId Owner = gir::NoId;
  };
  gir::FuncId CurId = gir::NoId;
  const gir::Function *CurFn = nullptr;
  std::vector<ScopeInfo> Table;

  llvm::DIFile *fileOf(const gir::Span &S) const;
  llvm::DIFile *fileOfFunction(const gir::Function &F) const;
  void makeType(const gir::Type *T);
  llvm::DIType *build(const gir::Type *T, const std::string &Name,
                      unsigned &Height);
  /// What a type being made refers to for a part P that lies inside it (a
  /// member, an element): P's definition, or past MaxTypeHeight target(P).
  /// Both raise Height (the height of the type being made) to what they add.
  llvm::DIType *ref(const gir::Type *P, unsigned &Height);
  /// What a pointer to P refers to (see the definition).
  llvm::DIType *target(const gir::Type *P, unsigned &Height);
  llvm::DIType *arrayTarget(const gir::Type *T);
  llvm::DIType *arrayOf(const gir::Type *T, const std::string &Name,
                        llvm::DIType *Elem);
  llvm::DIType *declOf(const gir::Type *T);
  void keep(const gir::Type *T);
  void wrap(const gir::Type *T, llvm::DIType *Inner);
  uint64_t hashOf(const gir::Type *T);
  uint64_t allocBits(const gir::Type *T);
  uint32_t alignBits(const gir::Type *T);
  llvm::DIBasicType *intType(unsigned Bits, bool Signed);
  llvm::DIDerivedType *member(llvm::DIScope *Scope, llvm::StringRef Name,
                              llvm::DIType *Ty, uint64_t Offset, uint64_t Size);
  llvm::DISubroutineType *subroutineType(const gir::Type *Sig,
                                         unsigned &Height);
  std::string spell(const gir::Type *T);
  std::string capped(std::string Name, const gir::Type *T);
  FuncInfo &funcInfo(gir::FuncId Id);
  std::pair<gir::FuncId, gir::LocalId> originOf(gir::LocalId L) const;
  llvm::DILocalVariable *variable(gir::FuncId Owner, gir::LocalId L);
};

} // namespace gopp

#endif // GOPP_CODEGEN_DEBUGINFO_H
