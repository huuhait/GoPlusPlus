//===- gopp/Import/CABI.h - C ABI classification through Clang --*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.11 (D4.4, D4.11 b; docs/design/c-abi.md). Clang's CodeGen only
// *classifies* a C signature (CodeGenABITypes: arrangeFreeFunctionCall); what
// the classification means for the IR is Go++'s own code (CodeGen/CABILower.h).
// This header is the boundary between the two and includes no Clang header:
//
//   Classifier   per module, per target. Maps GIR types to Clang QualTypes
//                (structs and Option become C records; pointers, funcs
//                `void *`), asks Clang for the CGFunctionInfo of the signature
//                and copies the ABIArgInfo of every argument and of the
//                result into plain data (ArgInfo). A cache keyed by the
//                signature's GIR type (types are uniqued, so the pointer is
//                the key) makes a second ask free, so a classifier must not
//                outlive the gir::Context of its types. The Clang world
//                behind it (target, AST context, code generator) is made once
//                per target per process and shared by every classifier
//                (M3); a mutex makes it callable from many threads, and the
//                answer is a pure function of the signature and the target,
//                never of who asked first.
//   ArgInfo      the ABIArgInfo kinds as data, with the LLVM types they name
//                as LTy (a description of a type that belongs to no
//                LLVMContext), so one answer serves every module and thread.
//   classifyScalars
//                the answer for a signature of scalars only, without Clang
//                (a program that passes no aggregate to C never starts it).
//
// P07's header import uses the same Classifier for the signatures of the C
// functions it imports and the same mapping for the layout checks (layoutOf);
// P04.18's `@Export` and callback thunks run the lowering of the same
// CallInfo in the other direction.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_IMPORT_CABI_H
#define GOPP_IMPORT_CABI_H

#include "gopp/GIR/Type.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class LLVMContext;
class Type;
} // namespace llvm

namespace gopp {
namespace cabi {

/// An LLVM type an ABI decision names, free of any LLVMContext. Only the
/// shapes Clang's ABI code produces for a C signature exist.
struct LTy {
  enum Kind : uint8_t {
    Void,
    Int,
    Half,
    BFloat,
    Float,
    Double,
    X86FP80,
    FP128,
    Ptr,
    Vector, // Count x Elems[0]
    Array,  // Count x Elems[0]
    Struct  // literal struct of Elems
  };
  Kind K = Void;
  unsigned Bits = 0;  // Int
  uint64_t Count = 0; // Vector, Array
  bool Packed = false;
  std::vector<LTy> Elems;

  bool isVoid() const { return K == Void; }
  /// The type in C (as `i64`, `[2 x double]`, `{ i64, i32 }`): for dumps.
  std::string str() const;
  llvm::Type *get(llvm::LLVMContext &C) const;
};

/// How one argument or the result crosses the C ABI: ABIArgInfo as data.
struct ArgInfo {
  enum class Kind : uint8_t {
    Direct,          // in registers, as the scalar or as Coerce
    Extend,          // a Direct integer with zeroext or signext
    Indirect,        // by a hidden pointer (byval on the stack, or a copy)
    IndirectAliased, // refused: no reachable target uses it
    Ignore,          // no IR argument (empty struct)
    Expand,          // one IR argument per field
    CoerceAndExpand, // one IR argument per element of Coerce (not padding)
    InAlloca,        // refused: 32-bit Windows only
    TargetSpecific   // refused: scalable-vector tuples (RISC-V, SVE)
  };
  Kind K = Kind::Direct;
  /// The Go++ type (null for a void result).
  const gir::Type *Ty = nullptr;

  /// Direct, Extend: the IR type when it is not the type's own value type
  /// (HasCoerce), at byte Offset of the value. CoerceAndExpand: the whole
  /// struct, of which ExpandIdx lists the elements that are passed (the
  /// rest is padding).
  bool HasCoerce = false;
  LTy Coerce;
  unsigned Offset = 0;
  /// A Direct struct Coerce is passed as one IR argument per element.
  bool Flatten = false;
  std::vector<unsigned> ExpandIdx;
  bool InReg = false;
  bool SExt = false, ZExt = false;
  /// Direct, Extend: the alignment of the argument's slot on the stack
  /// (the `alignstack` attribute), 0 for the default.
  unsigned StackAlign = 0;
  /// A dummy IR argument of this type goes before the real one (x86-32).
  bool HasPadding = false;
  bool PaddingInReg = false;
  LTy Padding;

  /// Indirect: the alignment of the pointee (0: natural), byval or a plain
  /// pointer to the caller's copy, and whether the callee's stack copy must
  /// be realigned.
  unsigned Align = 0;
  bool ByVal = false;
  bool Realign = false;
  unsigned AddrSpace = 0;
  /// The hidden result pointer goes after the first argument (x86-32 C++).
  bool SRetAfterThis = false;

  unsigned InAllocaField = 0;
  bool InAllocaIndirect = false;
  bool InAllocaSRet = false;

  const char *kindName() const;
  /// One word per fact, e.g. `direct <2 x float>`, `indirect byval align 8`.
  std::string str(bool IsResult = false) const;
};

struct CallInfo {
  ArgInfo Ret;
  std::vector<ArgInfo> Args;
  /// One line per argument; the form the tests and the docs show.
  std::string str() const;
};

/// What the classifier is made for. CPU and Features are the target's
/// (CodeGen/Target.h); Swift selects the swiftcall convention, which the
/// tests use to reach CoerceAndExpand (no Go++ function has it).
struct ClassifierOptions {
  std::string Triple, CPU, Features;
  bool Swift = false;
  /// A struct of more than FlatSize bytes reaches Clang as a record of the
  /// same size and alignment with one array field: both Go++ targets pass
  /// such a struct in memory (Indirect, no coerce type), decided by its size
  /// and alignment alone, and Clang never walks its fields (its
  /// homogeneous-aggregate and empty-record walks have no memo, so a DAG of
  /// structs costs Clang every path). Not for a struct of no bytes: inside a
  /// struct that Clang passes Direct, its stand-in would change the coerce
  /// type Clang derives (CABIFlat.FlatStructsClassifyLikeTheOriginals).
  /// Zero turns it off: that test compares both.
  uint64_t FlatSize = 128;
};

/// Counters for the tests.
struct ClassifierStats {
  uint64_t Asked = 0;       // classify() calls
  uint64_t Classified = 0;  // misses: signatures given to Clang
  uint64_t TypesMapped = 0; // GIR types turned into Clang types
};

/// The Clang view of a GIR type: C size, alignment and, for a struct, the
/// offset of each field (an Option has two).
struct ClangLayout {
  uint64_t Size = 0, Align = 0;
  std::vector<uint64_t> Offsets;
};

class Classifier {
public:
  /// Fails if Clang has no target for O.Triple. Makes the target's Clang
  /// world the first time a process asks for it, then reuses it.
  static llvm::Expected<std::unique_ptr<Classifier>>
  create(const ClassifierOptions &O);
  ~Classifier();

  /// The classification of the signature Sig (a gir Func type whose
  /// parameters and result are C types: gir::CTypeRules accepts them). The
  /// result stays valid for the life of the classifier.
  const CallInfo &classify(const gir::Type *Sig);

  /// What Clang says about T (a C type, any use): the layout of the C record
  /// with T's own fields (never the flat stand-in).
  ClangLayout layoutOf(const gir::Type *T);

  /// The data layout Clang uses for the target, as LLVM spells it.
  std::string dataLayout();

  ClassifierStats stats();

private:
  Classifier();
  struct Impl;
  std::unique_ptr<Impl> I;
};

/// The classification Clang gives Sig when every parameter and the result
/// are scalars (bool, integers, floats, pointers; a void result), on
/// linux/amd64 and linux/arm64, made without Clang: amd64 extends a bool and
/// an integer narrower than 32 bits (signext / zeroext), AAPCS64 on Linux
/// extends nothing, and everything else is Direct as its own type (M3: no
/// Clang for programs that pass no aggregate to C; CABIKinds.ScalarsWithout-
/// ClangAreClangs compares every scalar with Clang's answer). None for any
/// other signature or target.
std::optional<CallInfo> classifyScalars(llvm::StringRef Triple,
                                        const gir::Type *Sig);

/// Test-only (M3): the Clang worlds this process has made, one per target.
unsigned clangWorldsMade();

} // namespace cabi
} // namespace gopp

#endif // GOPP_IMPORT_CABI_H
