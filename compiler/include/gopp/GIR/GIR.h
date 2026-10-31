//===- gopp/GIR/GIR.h - The GIR data structures -----------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.01 (D4.1), docs/design/gir.md. GIR is shaped like Rust's MIR: a function
// is a list of locals and a control-flow graph of basic blocks; a block is a
// list of statements and one terminator; values live in *places* (a local plus
// projections) and are computed by *rvalues*. It is typed and not SSA.
//
// Everything refers to its parts by index (LocalId, BlockId, FuncId, string
// id), never by pointer, so a body can be copied, substituted, serialized
// (Serialize.h) and compared byte for byte. Types are the only pointers; they
// are uniqued in a gir::Context.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_GIR_H
#define GOPP_GIR_GIR_H

#include "gopp/GIR/Type.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

#include <memory>
#include <string>
#include <vector>

namespace gopp {
namespace gir {

using LocalId = uint32_t;
using BlockId = uint32_t;
using FuncId = uint32_t;
constexpr uint32_t NoId = UINT32_MAX;
/// A debug scope of a function (P04.12): 0 is the function itself, N > 0 is
/// Function::Scopes[N - 1].
using ScopeId = uint32_t;

/// A source position that needs no SourceManager: File indexes
/// Module::Files plus one (0 = none), Line and Col are 1-based (D4.7).
struct Span {
  uint32_t File = 0, Line = 0, Col = 0;
  bool isValid() const { return File != 0; }
  bool operator==(const Span &O) const {
    return File == O.File && Line == O.Line && Col == O.Col;
  }
};

//===----------------------------------------------------------------------===//
// Places, constants, operands
//===----------------------------------------------------------------------===//

/// One step into a value. Ty is the type after the step.
struct PlaceElem {
  enum Kind : uint8_t {
    Deref,      // *p: the operand is a pointer
    Field,      // .N of a String, Slice, Tuple or Struct
    Index,      // [_L] of an Array, a Slice or a String (a byte): Val is a
                // local of an integer type; bounds are checked by an Assert
                // before, not here
    ConstIndex, // [N] of an Array
    Downcast,   // the payload of variant N of an enum-like type (interface
                // data, `extends` parts: P04.06, P04.22)
  };
  Kind K = Deref;
  uint32_t Val = 0;
  Type *Ty = nullptr;
  bool operator==(const PlaceElem &O) const {
    return K == O.K && Val == O.Val && Ty == O.Ty;
  }
};

struct Place {
  LocalId Local = NoId;
  llvm::SmallVector<PlaceElem, 1> Proj;
  /// The type of the place (the type of the local if Proj is empty).
  Type *Ty = nullptr;
  bool operator==(const Place &O) const {
    return Local == O.Local && Proj == O.Proj && Ty == O.Ty;
  }
};

struct Constant {
  enum Kind : uint8_t {
    Int,    // Bits: the value, truncated to the width of Ty
    Float,  // Bits: the IEEE bits of a double (a f32 is rounded first)
    Bool,   // Bits: 0 or 1
    Str,    // Bits: the string id in Module::Strings; Ty is `string`
    Func,   // Bits: the FuncId; Ty is the function's pointer type
    Null,   // the nil pointer of Ty
    Zero,   // the all-zero value of Ty (a zero value in the language's sense)
    Global, // Bits: the GlobalId; Ty is a pointer to the global's type (the
            // address of the global; a place reaches it by a Deref)
  };
  Kind K = Zero;
  Type *Ty = nullptr;
  uint64_t Bits = 0;
  bool operator==(const Constant &O) const {
    return K == O.K && Ty == O.Ty && Bits == O.Bits;
  }
};

struct Operand {
  enum Kind : uint8_t {
    Copy, // read the place; the value stays valid
    Move, // read the place; the source is moved-from (zeroed, D4.2 step 3)
    Const,
  };
  Kind K = Const;
  Place P;
  Constant C;
  Type *type() const { return K == Const ? C.Ty : P.Ty; }
};

//===----------------------------------------------------------------------===//
// Rvalues and statements
//===----------------------------------------------------------------------===//

enum class BinOp : uint8_t {
  Add,
  Sub,
  Mul,
  Div,
  Rem,
  And,
  Or,
  Xor,
  AndNot,
  Shl,
  Shr, // arithmetic
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge, // comparison
};
enum class UnOp : uint8_t { Neg, Not, Compl };
enum class CastKind : uint8_t {
  IntToInt, // truncate, or extend by the signedness of the source
  IntToFloat,
  FloatToInt, // saturating (D4.12)
  FloatToFloat,
  PtrToInt,
  IntToPtr,
  PtrToPtr,
};
enum class AggKind : uint8_t {
  Tuple,  // Ops are the fields
  Array,  // Ops are the elements
  String, // Ops: ptr, len, cap
  Slice,  // Ops: ptr, len, cap, alloc (ADR-0029)
  Struct, // Ops are the fields in declaration order
};

const char *getBinOpName(BinOp Op);
const char *getUnOpName(UnOp Op);
const char *getCastKindName(CastKind K);
const char *getAggKindName(AggKind K);
bool isComparison(BinOp Op);
bool isShift(BinOp Op);

struct Rvalue {
  enum Kind : uint8_t {
    Use,       // A
    AddrOf,    // &P (Mut: a writable borrow, P is the borrowed place)
    Binary,    // A Bin B
    Unary,     // Un A
    Cast,      // Cast A as Ty
    Aggregate, // Agg(Ops...)
    Len,       // the length of the String, Slice or Array in P (an u64)
  };
  Kind K = Use;
  Type *Ty = nullptr; // the type of the value
  Operand A, B;
  Place P;
  BinOp Bin = BinOp::Add;
  UnOp Un = UnOp::Neg;
  CastKind CastOp = CastKind::IntToInt;
  AggKind Agg = AggKind::Tuple;
  bool Mut = false;
  llvm::SmallVector<Operand, 3> Ops;
};

struct Statement {
  enum Kind : uint8_t {
    Assign,      // Dest = RV
    StorageLive, // the local L starts to exist (its slot is usable)
    StorageDead, // the local L stops to exist
    Nop,
  };
  Kind K = Nop;
  Place Dest;
  Rvalue RV;
  LocalId L = NoId;
  Span Loc;
  /// A copy made by GIR inlining (Inline.h) for a parameter or a result: the
  /// code generator's FMA matcher does not look through it, so inlining a
  /// call never fuses a product with a sum that the call kept apart.
  bool Boundary = false;
  /// The debug scope the statement belongs to (Function::Scopes, V074). In
  /// code that GIR inlining copied the span is a position in the callee and
  /// the scope lies under the `Inlined` scope of the call (P04.12).
  ScopeId Scope = 0;
};

//===----------------------------------------------------------------------===//
// Terminators
//===----------------------------------------------------------------------===//

/// What happens when a call (or a drop, or an assert) unwinds (D4.6).
struct UnwindAction {
  enum Kind : uint8_t {
    Continue,    // no cleanup here: keep unwinding to the caller
    Cleanup,     // run the cleanup block Target, which ends in Resume
    Terminate,   // an unwind here aborts (a drop that panics while unwinding)
    Unreachable, // the callee cannot unwind (nounwind)
  };
  Kind K = Continue;
  BlockId Target = NoId;
  bool operator==(const UnwindAction &O) const {
    return K == O.K && Target == O.Target;
  }
};

/// What a failed Assert reports. The terminator's Args are the values the
/// message shows: Bounds takes the index (an i64, or a u64 for an unsigned
/// index) and the length (a u64); the others take none (V065). The position
/// is the terminator's Loc: a constant, passed to the panic function by the
/// code generator (gir.md section 6).
enum class AssertKind : uint8_t {
  DivByZero,
  NegativeShift,
  Bounds,
  Nil,
  Overflow,
  // Slice expressions (Go's boundsErrors, runtime/error.go). Each takes the
  // offending index X (an i64, or a u64 for an unsigned index) and a u64 Y.
  // s[:x] needs x <= cap(s) (a string or an array: length): SliceAcap,
  // SliceAlen (Y is the capacity, the length); s[x:y] needs x <= y: SliceB;
  // s[::x] and s[:x:y] and s[x:y:] are the three-index forms.
  SliceAlen,
  SliceAcap,
  SliceB,
  Slice3Alen,
  Slice3Acap,
  Slice3B,
  Slice3C,
  // [N]T(s) and (*[N]T)(s) need len(s) >= N: X is the length, Y is N
  // ("cannot convert slice with length X to array or pointer to array with
  // length Y").
  SliceConvert,
};
/// The kinds whose Args are an index and a length (V065): Bounds and the
/// slice kinds.
inline bool takesIndexAndLength(AssertKind K) {
  return K == AssertKind::Bounds || K >= AssertKind::SliceAlen;
}
const char *getAssertKindName(AssertKind K);

struct SwitchCase {
  uint64_t Value = 0;
  BlockId Target = NoId;
};

struct Terminator {
  enum Kind : uint8_t {
    Goto,        // -> Target
    Switch,      // on Discr (an integer or bool): Cases, else Otherwise
    Return,      // return local 0
    Unreachable, // control cannot get here
    Call,        // Dest = Callee(Args); continue at Target (NoId: the callee
                 // is NoReturn); Unwind says what a panic does
    Drop,        // run the drop glue of the owner in Dest, then Target
    Assert,      // if Discr != Expected: fail (AK). Continue at Target
    Resume,      // cleanup blocks only: continue the unwind
  };
  Kind K = Unreachable;
  BlockId Target = NoId;
  Operand Discr; // Switch: the value; Assert: the condition; Call: the callee
  llvm::SmallVector<SwitchCase, 2> Cases;
  BlockId Otherwise = NoId;
  llvm::SmallVector<Operand, 4>
      Args;   // Call: the arguments; Assert: AK's values
  Place Dest; // Call: the result place; Drop: the place dropped
  UnwindAction Unwind;
  bool Expected = true;
  AssertKind AK = AssertKind::DivByZero;
  Span Loc;
  ScopeId Scope = 0; // as Statement::Scope
  bool Set = false;  // false until the builder sets it
};

struct BasicBlock {
  std::vector<Statement> Stmts;
  Terminator Term;
  /// A cleanup block runs on the unwind path only (D4.6). It is reached only
  /// by Cleanup unwind edges and from other cleanup blocks, and it ends in
  /// Resume, Unreachable or an edge to another cleanup block (V023, V025).
  bool Cleanup = false;
  /// A catch block is a cleanup block that may also end the unwind: it is the
  /// catch-all handler of a function with `defer` (D4.6, P04.07). It runs the
  /// deferred calls and `recover`, so it may Return, branch to normal blocks,
  /// and its calls may unwind onward (a deferred call that panics replaces
  /// the panic). Catch implies Cleanup (V024); only a catch block may branch
  /// into one (V023). A plain drop pad keeps the strict rules (V025).
  bool Catch = false;
};

//===----------------------------------------------------------------------===//
// Functions and the module
//===----------------------------------------------------------------------===//

struct Local {
  enum Kind : uint8_t { Return, Arg, Var, Temp } K = Temp;
  Type *Ty = nullptr;
  std::string Name; // for the dump and the debugger; may be empty
  bool Mutable = true;
  /// A temporary that holds a value which owns memory (a string, slice or
  /// map just made by `+`, `make`, `append`, a conversion, a literal) and that
  /// no variable took over yet. Drop elaboration (P04.09) drops it at the end
  /// of its statement unless a `Move` of it was made; until then it leaks
  /// (gir.md section 7). Only temporaries are owned (V014).
  bool Owned = false;
  /// A parameter, variable, named result or range variable the program
  /// declared (not a name the lowering gave to a helper's variable). Only
  /// these are variables to the debugger (P04.12); a parameter or variable
  /// only (V011).
  bool Source = false;
  /// A pointer that stands for the place it points to (P04.18): a parameter
  /// passed by a hidden pointer to the caller's place (PAR-10) and a `&`
  /// range variable (RNG-1a). The debugger shows that place, with its type
  /// (`DW_OP_deref`), not the pointer.
  bool Deref = false;
  Span Loc;
  /// The debug scope the variable is visible in (Function::Scopes, V074).
  ScopeId Scope = 0;
  /// The 1-based position of a parameter, kept when GIR inlining makes the
  /// parameter an ordinary local of the caller (0: not a parameter; V074).
  uint32_t ArgNo = 0;
  /// A local that GIR inlining copied from a callee: its id in the function
  /// whose body the nearest `Inlined` scope above Scope is (NoId: a local of
  /// this function; V074). The debugger shows that function's variable for
  /// all the copies.
  LocalId Origin = NoId;
};

/// A lexical block of the source, or the body of a call that GIR inlining put
/// into its caller (an inline site; P04.12, docs/design/debug-info.md). The
/// scopes of a function form a tree: statements, terminators and locals name
/// the scope they are in, and the code generator turns a `Block` into a
/// `DILexicalBlock` and an `Inlined` scope into the callee's subprogram with
/// an `inlinedAt` location at the call, so a debugger shows the callee's own
/// file and line inside the call and lists the callee as an inlined frame
/// (V073).
struct DebugScope {
  enum Kind : uint8_t {
    Block,   // `{ ... }` of the source; Loc is where it begins
    Inlined, // the body of Callee, inlined at the call at Loc
  };
  Kind K = Block;
  /// The enclosing scope: a smaller id (0: the function).
  ScopeId Parent = 0;
  Span Loc;
  /// Inlined: the function whose body it is.
  FuncId Callee = NoId;
  /// A Block that GIR inlining copied from a callee: its id among the scopes
  /// of the function whose body the nearest `Inlined` scope above it is (0: a
  /// block of this function).
  ScopeId Origin = 0;
  bool operator==(const DebugScope &O) const {
    return K == O.K && Parent == O.Parent && Loc == O.Loc &&
           Callee == O.Callee && Origin == O.Origin;
  }
};

enum class Linkage : uint8_t { External, Internal, LinkOnceODR };
class Function {
public:
  std::string Name;    // the final symbol: gir::symbolOf (Lower.h) makes it
  Type *Sig = nullptr; // a Func type
  Linkage Link = Linkage::External;
  /// The convention of Sig (V075): a function is Go++'s unless it is a C
  /// function (a declaration, a C-ABI thunk, the entry point).
  CallConv CC = CallConv::Go;
  bool NoUnwind = false;
  /// Never returns: a call of it may have no return target (V028).
  bool NoReturn = false;
  Span Loc;
  /// Local 0 is the return place, locals 1..N the parameters, then the rest.
  std::vector<Local> Locals;
  std::vector<BasicBlock> Blocks; // block 0 is the entry; empty: a declaration
  /// The debug scopes below the function (ScopeId N is Scopes[N - 1]; V073).
  std::vector<DebugScope> Scopes;
  bool isDeclaration() const { return Blocks.empty(); }
  unsigned numParams() const { return Sig->elems().size(); }

  ScopeId addScope(DebugScope S) {
    Scopes.push_back(S);
    return Scopes.size();
  }

  LocalId addLocal(Local L) {
    Locals.push_back(std::move(L));
    return Locals.size() - 1;
  }
  BlockId addBlock(bool Cleanup = false, bool Catch = false) {
    Blocks.emplace_back();
    Blocks.back().Cleanup = Cleanup || Catch;
    Blocks.back().Catch = Catch;
    return Blocks.size() - 1;
  }
};

/// A package variable or a block of constant data (P04.04). Its address is
/// `Constant::Global`; code reaches the value through a Deref of it. Init is
/// a scalar or string constant, or Zero: a variable whose initializer is not
/// a constant is set by the package's init function (the order of the
/// checker's init-order record).
struct Global {
  std::string Name; // the final symbol (gir::symbolOf)
  Type *Ty = nullptr;
  Linkage Link = Linkage::External;
  /// Constant data: it goes to read-only memory (rodata).
  bool ReadOnly = false;
  Constant Init;
  /// Static data with pointers in it (type descriptors, itabs, method
  /// tables: P04.06). When not empty, the initial value is a tree of
  /// constants: the leaves of Ty in order, depth first through the fields of
  /// a struct or a tuple and the elements of an array. A leaf is a constant
  /// of the leaf's type: an integer, bool, float or string constant, `Null`,
  /// the address of a global or a function in a pointer field, a function
  /// constant in a func field, or `Zero` of any type (one leaf for the whole
  /// part). `Init` is then Zero (V072).
  std::vector<Constant> Parts;
  Span Loc;
  Global() { Init.K = Constant::Zero; }
};

class Module {
public:
  explicit Module(Context &Ctx, std::string Name = "")
      : Ctx(Ctx), Name(std::move(Name)) {}
  Context &Ctx;
  std::string Name; // the package path
  std::vector<std::string> Files;
  /// String literals by id; the bytes have no terminator (Str constants).
  std::vector<std::string> Strings;
  std::vector<std::unique_ptr<Function>> Funcs;
  std::vector<Global> Globals;

  uint32_t addFile(llvm::StringRef Path);
  uint32_t internString(llvm::StringRef S);
  FuncId addFunction(std::unique_ptr<Function> F);
  /// The function called Name, or NoId.
  FuncId lookupFunction(llvm::StringRef Name) const;
  uint32_t addGlobal(Global G);
  /// The global called Name, or NoId.
  uint32_t lookupGlobal(llvm::StringRef Name) const;
  Function &func(FuncId Id) { return *Funcs[Id]; }
  const Function &func(FuncId Id) const { return *Funcs[Id]; }

private:
  llvm::StringMap<uint32_t> FileIds, StringIds, FuncIds, GlobalIds;
};

/// The symbol of a function the compiler writes in GIR (println's helpers,
/// the panic functions of Assert): the mangled name of `gopp.<Name>`. The
/// code generator finds the panic function of a failed Assert by it.
std::string runtimeSymbol(llvm::StringRef Name);

//===----------------------------------------------------------------------===//
// Helpers to make operands
//===----------------------------------------------------------------------===//

inline Place localPlace(LocalId L, Type *Ty) {
  Place P;
  P.Local = L;
  P.Ty = Ty;
  return P;
}
Operand copyOf(Place P);
Operand moveOf(Place P);
Operand constInt(Type *Ty, uint64_t Bits);
Operand constBool(Context &C, bool V);
Operand constFloat(Type *Ty, double V);
Operand constStr(Module &M, llvm::StringRef S);
Operand constFunc(const Module &M, FuncId F);
/// The address of global G (a pointer to its type).
Operand constGlobal(Module &M, uint32_t G);

/// The blocks a terminator can go to, unwind edges included.
template <class Term, class Fn> void forEachSuccessor(Term &T, Fn &&Visit) {
  auto Unwind = [&](auto &U) {
    if (U.K == UnwindAction::Cleanup)
      Visit(U.Target);
  };
  switch (T.K) {
  case Terminator::Goto:
    Visit(T.Target);
    break;
  case Terminator::Switch:
    for (auto &C : T.Cases)
      Visit(C.Target);
    Visit(T.Otherwise);
    break;
  case Terminator::Call:
    if (T.Target != NoId)
      Visit(T.Target);
    Unwind(T.Unwind);
    break;
  case Terminator::Drop:
  case Terminator::Assert:
    Visit(T.Target);
    Unwind(T.Unwind);
    break;
  case Terminator::Return:
  case Terminator::Unreachable:
  case Terminator::Resume:
    break;
  }
}

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_GIR_H
