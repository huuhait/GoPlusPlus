//===- Verify.cpp - The GIR verifier --------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The rules of docs/design/gir.md section 9. Each rule id is decided at one
// point, the function whose comment names it. The verifier trusts nothing the
// builder wrote and recomputes every type. Every walk is iterative and linear
// in the size of the module: a place is a loop over its projections, the CFG
// a work list, the types a work list that looks at each type once, and the
// struct cycle check a three-colour DFS with an explicit stack.
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/Verify.h"

#include "gopp/Basic/WorkCount.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::gir;

std::string VerifyError::str() const {
  std::string S = Rule + ": ";
  if (!Func.empty()) {
    S += "fn " + Func + ": ";
    if (Block >= 0) {
      S += "bb" + std::to_string(Block);
      if (Index >= 0)
        S += " #" + std::to_string(Index);
      S += ": ";
    }
  }
  return S + Message;
}

namespace {

/// Test-only work counter (M8): the units the verifier visits on this thread.
thread_local uint64_t Steps = 0;

} // namespace

uint64_t gir::verifierSteps() { return Steps; }

namespace {

/// Calls Visit on each block a normal edge of T goes to.
template <class Fn> void forEachNormalEdge(const Terminator &T, Fn &&Visit) {
  switch (T.K) {
  case Terminator::Goto:
    Visit(T.Target);
    return;
  case Terminator::Switch:
    for (const SwitchCase &C : counted(Steps, T.Cases))
      Visit(C.Target);
    Visit(T.Otherwise);
    return;
  case Terminator::Call:
  case Terminator::Drop:
  case Terminator::Assert:
    if (T.Target != NoId)
      Visit(T.Target);
    return;
  case Terminator::Return:
  case Terminator::Unreachable:
  case Terminator::Resume:
    return;
  }
}

/// The terminators that run code that can panic carry an unwind action.
bool hasUnwind(const Terminator &T) {
  return T.K == Terminator::Call || T.K == Terminator::Drop ||
         T.K == Terminator::Assert;
}

const char *unwindName(UnwindAction::Kind K) {
  switch (K) {
  case UnwindAction::Continue:
    return "continue";
  case UnwindAction::Cleanup:
    return "cleanup";
  case UnwindAction::Terminate:
    return "terminate";
  case UnwindAction::Unreachable:
    return "unreachable";
  }
  return "?";
}

class Verifier {
public:
  Verifier(const Module &M, unsigned Max) : M(M), Max(std::max(Max, 1u)) {}

  std::vector<VerifyError> run() {
    functionNames();
    globals();
    for (FuncId I = 0; I < M.Funcs.size() && !full(); ++I, ++Steps)
      if (M.Funcs[I])
        function(*M.Funcs[I]);
    F = nullptr;
    Block = Index = -1;
    typeShapes();
    structCycles();
    return std::move(Errors);
  }

private:
  const Module &M;
  unsigned Max;
  std::vector<VerifyError> Errors;
  const Function *F = nullptr;
  int64_t Block = -1, Index = -1;
  /// The locals whose address some AddrOf takes (V066), of the function.
  llvm::DenseSet<LocalId> AddrTaken;
  /// The types the module uses, each once (V002, V003 look at them last).
  llvm::DenseSet<const Type *> SeenTypes;
  std::vector<const Type *> TypeWork;

  bool full() const { return Errors.size() >= Max; }

  void err(const char *Rule, const std::string &Msg) {
    if (full())
      return;
    VerifyError E;
    E.Rule = Rule;
    if (F)
      E.Func = F->Name;
    E.Block = Block;
    E.Index = Index;
    E.Message = Msg;
    Errors.push_back(std::move(E));
  }

  static std::string str(const Type *T) { return T ? T->str() : "<null>"; }
  static std::string num(uint64_t N) { return std::to_string(N); }

  void noteType(const Type *T) {
    if (T && SeenTypes.insert(T).second)
      TypeWork.push_back(T);
  }

  //===--------------------------------------------------------------------===//
  // The module: V001-V003
  //===--------------------------------------------------------------------===//

  /// V001: every function exists and has a name that no other function has
  /// (the name is the symbol, and Module::lookupFunction finds it by name).
  void functionNames() {
    llvm::StringSet<> Names;
    for (FuncId I = 0; I < M.Funcs.size(); ++I) {
      ++Steps;
      const Function *Fn = M.Funcs[I].get();
      if (!Fn)
        err("V001", "function #" + num(I) + " is null");
      else if (Fn->Name.empty())
        err("V001", "function #" + num(I) + " has no name");
      else if (!Names.insert(Fn->Name).second)
        err("V001", "two functions are named " + Fn->Name);
    }
  }

  /// V071: a global has a name no function or other global has (one symbol
  /// namespace) and a type that holds a value (not unit); its static value
  /// is Zero (also for a variable its package's init function sets), or a
  /// constant of its type that is a scalar or a string, never an address
  /// (Func or Global: P04.04 needs no relocations in data).
  void globals() {
    llvm::StringSet<> Names;
    for (const auto &Fn : counted(Steps, M.Funcs))
      if (Fn)
        Names.insert(Fn->Name);
    for (size_t I = 0; I < M.Globals.size() && !full(); ++I) {
      ++Steps;
      const Global &G = M.Globals[I];
      std::string Who = "global #" + num(I) + " ";
      if (G.Name.empty())
        err("V071", Who + "has no name");
      else if (!Names.insert(G.Name).second)
        err("V071", Who + G.Name + " has the name of another symbol");
      if (!G.Ty || G.Ty->isUnit()) {
        err("V071", Who + "has no type, or the unit type");
        continue;
      }
      noteType(G.Ty);
      span(G.Loc);
      if (G.Init.K == Constant::Zero) {
        if (G.Init.Ty)
          err("V071", Who + G.Name + ": a zero initializer has no type");
        continue;
      }
      if (G.Init.K == Constant::Func || G.Init.K == Constant::Global) {
        err("V071", Who + G.Name + ": an initializer that is an address");
        continue;
      }
      if (!G.Ty->isScalar() && G.Ty->getKind() != Type::Kind::String) {
        err("V071", Who + G.Name + " of type " + str(G.Ty) +
                        " starts with a constant that is not zero");
        continue;
      }
      if (constant(G.Init) && G.Init.Ty != G.Ty)
        err("V071", Who + G.Name + " of type " + str(G.Ty) +
                        " has an initializer of type " + str(G.Init.Ty));
    }
  }

  /// V002: a type is well formed: integers are 8, 16, 32 or 64 bits wide,
  /// floats 32 or 64, a struct the module uses has its body, and no part is
  /// missing. Each type is looked at once; its parts join the work list.
  void typeShapes() {
    for (size_t I = 0; I < TypeWork.size() && !full(); ++I) {
      ++Steps;
      const Type *T = TypeWork[I];
      switch (T->getKind()) {
      case Type::Kind::Int:
        if (T->bits() != 8 && T->bits() != 16 && T->bits() != 32 &&
            T->bits() != 64)
          err("V002", "an integer type of " + num(T->bits()) + " bits");
        break;
      case Type::Kind::Float:
        if (T->bits() != 32 && T->bits() != 64)
          err("V002", "a float type of " + num(T->bits()) + " bits");
        break;
      case Type::Kind::Struct:
        if (!T->hasBody())
          err("V002", "struct " + T->name().str() + " has no body");
        break;
      case Type::Kind::Ptr:
      case Type::Kind::Slice:
      case Type::Kind::Array:
      case Type::Kind::Option:
        if (!T->elem()) {
          err("V002", "a pointer, slice or array type without an element");
          continue;
        }
        noteType(T->elem());
        break;
      case Type::Kind::Func:
        if (!T->ret()) {
          err("V002", "a function type without a result type");
          continue;
        }
        noteType(T->ret());
        break;
      default:
        break;
      }
      for (const Type *E : counted(Steps, T->elems())) {
        if (!E)
          err("V002", "a tuple, struct or function type with a null part");
        noteType(E);
      }
    }
  }

  /// The structs a value of type T holds by value (T itself, the elements of
  /// arrays and tuples); a pointer, a slice or a string holds none.
  static void structsIn(const Type *T,
                        llvm::SmallVectorImpl<const Type *> &Out) {
    llvm::SmallVector<const Type *, 8> Work{T};
    while (!Work.empty()) {
      ++Steps;
      const Type *C = Work.pop_back_val();
      if (!C)
        continue;
      if (C->getKind() == Type::Kind::Struct)
        Out.push_back(C);
      else if (C->getKind() == Type::Kind::Array ||
               C->getKind() == Type::Kind::Option)
        Work.push_back(C->elem());
      else if (C->getKind() == Type::Kind::Tuple)
        Work.append(C->elems().begin(), C->elems().end());
    }
  }

  /// V003: a struct does not contain itself by value, directly or through
  /// other structs, arrays and tuples (it would have no finite size). A
  /// three-colour DFS over the structs the module uses, with its own stack.
  void structCycles() {
    llvm::DenseMap<const Type *, uint8_t> Color; // 1: on the path, 2: done
    struct Frame {
      const Type *S;
      llvm::SmallVector<const Type *, 4> Next;
    };
    llvm::SmallVector<Frame, 16> Stack;
    auto Push = [&](const Type *S) {
      Color[S] = 1;
      Frame Fr{S, {}};
      for (const Type *Fd : counted(Steps, S->elems()))
        structsIn(Fd, Fr.Next);
      Stack.push_back(std::move(Fr));
    };
    for (size_t I = 0; I < TypeWork.size() && !full(); ++I) {
      ++Steps;
      const Type *Root = TypeWork[I];
      if (Root->getKind() != Type::Kind::Struct || Color.count(Root))
        continue;
      Push(Root);
      while (!Stack.empty()) {
        ++Steps;
        Frame &Top = Stack.back();
        if (Top.Next.empty()) {
          Color[Top.S] = 2;
          Stack.pop_back();
          continue;
        }
        const Type *Kid = Top.Next.pop_back_val();
        auto It = Color.find(Kid);
        if (It == Color.end())
          Push(Kid);
        else if (It->second == 1)
          err("V003",
              "struct " + Kid->name().str() + " contains itself by value");
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // A function: V010-V013, V022, V070
  //===--------------------------------------------------------------------===//

  void function(const Function &Fn) {
    ++Steps;
    F = &Fn;
    Block = Index = -1;
    if (!signature(Fn))
      return;
    span(Fn.Loc);
    if (Fn.isDeclaration()) {
      declaration(Fn);
      return;
    }
    if (!locals(Fn))
      return;
    if (Fn.Blocks[0].Cleanup)
      err("V013", "the entry block is a cleanup block");
    AddrTaken.clear();
    for (const BasicBlock &BB : counted(Steps, Fn.Blocks))
      for (const Statement &S : counted(Steps, BB.Stmts))
        if (S.K == Statement::Assign && S.RV.K == Rvalue::AddrOf)
          AddrTaken.insert(S.RV.P.Local);
    for (BlockId B = 0; B < Fn.Blocks.size() && !full(); ++B) {
      Block = B;
      const BasicBlock &BB = Fn.Blocks[B];
      ++Steps;
      for (size_t I = 0; I < BB.Stmts.size(); ++I) {
        ++Steps;
        Index = I;
        statement(BB.Stmts[I]);
      }
      Index = BB.Stmts.size();
      terminator(BB);
    }
    Block = Index = -1;
    reachable(Fn);
  }

  /// V010: the signature is a function type.
  bool signature(const Function &Fn) {
    if (!Fn.Sig || Fn.Sig->getKind() != Type::Kind::Func) {
      err("V010", "the signature " + str(Fn.Sig) + " is not a function type");
      return false;
    }
    noteType(Fn.Sig);
    return true;
  }

  /// V011: local 0 is the return place, of the result type; locals 1..N are
  /// the parameters, of the signature's types in order; every other local is
  /// a variable or a temporary; every local has a type. False if the body
  /// cannot be checked (a local without a type).
  bool locals(const Function &Fn) {
    size_t N = Fn.numParams();
    if (Fn.Locals.size() < N + 1) {
      err("V011", "the function has " + num(Fn.Locals.size()) +
                      " locals, fewer than 1 + " + num(N) + " parameters");
      return false;
    }
    bool Typed = true;
    for (LocalId L = 0; L < Fn.Locals.size(); ++L) {
      ++Steps;
      const Local &Lo = Fn.Locals[L];
      span(Lo.Loc);
      if (!Lo.Ty) {
        err("V011", "local _" + num(L) + " has no type");
        Typed = false;
        continue;
      }
      noteType(Lo.Ty);
      bool KindOk = L == 0   ? Lo.K == Local::Return
                    : L <= N ? Lo.K == Local::Arg
                             : Lo.K == Local::Var || Lo.K == Local::Temp;
      if (!KindOk)
        err("V011", "local _" + num(L) + " has the wrong kind");
      const Type *Want = L == 0   ? Fn.Sig->ret()
                         : L <= N ? Fn.Sig->elems()[L - 1]
                                  : Lo.Ty;
      if (Lo.Ty != Want)
        err("V011", "local _" + num(L) + " has type " + str(Lo.Ty) +
                        ", the signature says " + str(Want));
    }
    return Typed;
  }

  /// V012: a declaration (no blocks) has no locals.
  void declaration(const Function &Fn) {
    if (!Fn.Locals.empty())
      err("V012", "a declaration has locals");
  }

  /// V022: every block is reachable from the entry along normal and unwind
  /// edges (the builder removes the blocks after a return or a break).
  void reachable(const Function &Fn) {
    std::vector<uint8_t> Seen(Fn.Blocks.size(), 0);
    llvm::SmallVector<BlockId, 32> Work{0};
    Seen[0] = 1;
    auto Reach = [&](BlockId S) {
      if (S < Seen.size() && !Seen[S]) {
        Seen[S] = 1;
        Work.push_back(S);
      }
    };
    while (!Work.empty()) {
      ++Steps;
      const Terminator &T = Fn.Blocks[Work.pop_back_val()].Term;
      if (!T.Set)
        continue;
      forEachNormalEdge(T, Reach);
      if (hasUnwind(T) && T.Unwind.K == UnwindAction::Cleanup)
        Reach(T.Unwind.Target);
    }
    for (BlockId B = 0; B < Fn.Blocks.size() && !full(); ++B, ++Steps)
      if (!Seen[B]) {
        Block = B;
        err("V022", "bb" + num(B) + " is unreachable");
      }
    Block = -1;
  }

  /// V070: a source position names a file of the module, or none.
  void span(const Span &S) {
    if (S.File > M.Files.size())
      err("V070", "a source position names file " + num(S.File) +
                      ", the module has " + num(M.Files.size()));
  }

  //===--------------------------------------------------------------------===//
  // Places and operands: V030-V036, V040
  //===--------------------------------------------------------------------===//

  /// V030: the place names an existing local. V033: every projection records
  /// the type its step gives. V031: the place's type is the last one. The
  /// steps themselves are V032, V034 and V035. False after reporting.
  bool place(const Place &P) {
    if (P.Local >= F->Locals.size()) {
      err("V030", "a place names local _" + num(P.Local) +
                      ", the function has " + num(F->Locals.size()));
      return false;
    }
    const Type *Cur = F->Locals[P.Local].Ty;
    for (const PlaceElem &E : counted(Steps, P.Proj)) {
      const Type *Next = step(Cur, E);
      if (!Next)
        return false;
      if (E.Ty != Next) {
        err("V033", "a projection records type " + str(E.Ty) +
                        ", the step gives " + str(Next));
        return false;
      }
      Cur = Next;
    }
    if (P.Ty != Cur) {
      err("V031",
          "the place has type " + str(P.Ty) + ", its local gives " + str(Cur));
      return false;
    }
    return true;
  }

  /// V032: a field, index or constant index applies to a type that has it
  /// (no downcast yet: GIR has no enum types). V034: an index is an integer
  /// local. V035: a deref is of a pointer. The type after the step, or null.
  const Type *step(const Type *Cur, const PlaceElem &E) {
    switch (E.K) {
    case PlaceElem::Deref:
      if (Cur->isPtr())
        return Cur->elem();
      err("V035", "deref of the non-pointer " + str(Cur));
      return nullptr;
    case PlaceElem::Field:
      if (Type *T = Cur->fieldType(E.Val, M.Ctx))
        return T;
      err("V032", "type " + str(Cur) + " has no field " + num(E.Val));
      return nullptr;
    case PlaceElem::Index:
      if (E.Val >= F->Locals.size() || !F->Locals[E.Val].Ty->isInt()) {
        err("V034", "index local _" + num(E.Val) + " is not an integer local");
        return nullptr;
      }
      if (Cur->getKind() == Type::Kind::Array ||
          Cur->getKind() == Type::Kind::Slice)
        return Cur->elem();
      err("V032", "an index into " + str(Cur));
      return nullptr;
    case PlaceElem::ConstIndex:
      if (Cur->getKind() == Type::Kind::Array && E.Val < Cur->length())
        return Cur->elem();
      err("V032", "constant index " + num(E.Val) + " into " + str(Cur));
      return nullptr;
    case PlaceElem::Downcast:
      err("V032", "a downcast of " + str(Cur) + ": GIR has no enum types yet");
      return nullptr;
    }
    err("V032", "an unknown projection");
    return nullptr;
  }

  /// V036: StorageLive and StorageDead name an existing local.
  void storage(LocalId L) {
    if (L >= F->Locals.size())
      err("V036", "storage marker of local _" + num(L) + ", the function has " +
                      num(F->Locals.size()));
  }

  bool operand(const Operand &O) {
    if (O.K == Operand::Const)
      return constant(O.C);
    return place(O.P);
  }

  /// V040: a constant has a type its kind allows and its bits fit: integer
  /// bits truncated to the width, a float of a float type, a bool 0 or 1, a
  /// string id in the module's table, a function id of a function whose
  /// signature is the constant's type, null of a pointer type.
  bool constant(const Constant &C) {
    if (!C.Ty) {
      err("V040", "a constant without a type");
      return false;
    }
    noteType(C.Ty);
    std::string Bad;
    switch (C.K) {
    case Constant::Int:
      if (!C.Ty->isInt())
        Bad = "an integer constant of type " + str(C.Ty);
      else if (C.Ty->bits() < 64 && (C.Bits >> C.Ty->bits()) != 0)
        Bad = "integer constant " + num(C.Bits) + " does not fit " + str(C.Ty);
      break;
    case Constant::Float:
      if (!C.Ty->isFloat())
        Bad = "a float constant of type " + str(C.Ty);
      break;
    case Constant::Bool:
      if (!C.Ty->isBool() || C.Bits > 1)
        Bad = "a bool constant " + num(C.Bits) + " of type " + str(C.Ty);
      break;
    case Constant::Str:
      if (C.Ty->getKind() != Type::Kind::String || C.Bits >= M.Strings.size())
        Bad = "string constant #" + num(C.Bits) + " of type " + str(C.Ty) +
              " (the module has " + num(M.Strings.size()) + " strings)";
      break;
    case Constant::Func:
      if (C.Bits >= M.Funcs.size() || !M.Funcs[C.Bits])
        Bad = "function constant #" + num(C.Bits) + " names no function";
      else if (M.Funcs[C.Bits]->Sig != C.Ty)
        Bad = "function constant @" + M.Funcs[C.Bits]->Name + " has type " +
              str(C.Ty) + ", its signature is " + str(M.Funcs[C.Bits]->Sig);
      break;
    case Constant::Null:
      if (!C.Ty->isPtr())
        Bad = "null of the non-pointer type " + str(C.Ty);
      break;
    case Constant::Zero:
      break;
    case Constant::Global:
      if (C.Bits >= M.Globals.size())
        Bad = "global constant #" + num(C.Bits) + " names no global";
      else if (!C.Ty->isPtr() || C.Ty->elem() != M.Globals[C.Bits].Ty)
        Bad = "the address of global @" + M.Globals[C.Bits].Name +
              " has type " + str(C.Ty) + ", it is a pointer to " +
              str(M.Globals[C.Bits].Ty);
      break;
    }
    if (Bad.empty())
      return true;
    err("V040", Bad);
    return false;
  }

  //===--------------------------------------------------------------------===//
  // Statements and rvalues: V050-V056
  //===--------------------------------------------------------------------===//

  void statement(const Statement &S) {
    span(S.Loc);
    switch (S.K) {
    case Statement::Assign:
      if (place(S.Dest)) {
        assign(S.RV, S.Dest.Ty);
        if (S.RV.K == Rvalue::Aggregate)
          aggregateInPlace(S);
      }
      return;
    case Statement::StorageLive:
    case Statement::StorageDead:
      storage(S.L);
      return;
    case Statement::Nop:
      return;
    }
  }

  /// V050: `Dest = rvalue` stores a value of Dest's type (each kind's rule
  /// checks the rvalue's type against its operands); `use` gives its
  /// operand's type.
  void assign(const Rvalue &R, const Type *Want) {
    if (!R.Ty) {
      err("V050", "an rvalue without a type");
      return;
    }
    noteType(R.Ty);
    if (R.Ty != Want) {
      err("V050", "an rvalue of type " + str(R.Ty) +
                      " is stored in a place of type " + str(Want));
      return;
    }
    switch (R.K) {
    case Rvalue::Use:
      if (operand(R.A) && R.A.type() != R.Ty)
        err("V050", "use of " + str(R.A.type()) + " gives " + str(R.Ty));
      return;
    case Rvalue::Unary:
      unary(R);
      return;
    case Rvalue::Binary:
      binary(R);
      return;
    case Rvalue::Cast:
      cast(R);
      return;
    case Rvalue::AddrOf:
      addrOf(R);
      return;
    case Rvalue::Aggregate:
      aggregate(R);
      return;
    case Rvalue::Len:
      len(R);
      return;
    }
  }

  /// V051: `neg` of an integer or a float, `not` of a bool, `compl` of an
  /// integer; the result has the operand's type.
  void unary(const Rvalue &R) {
    if (!operand(R.A))
      return;
    const Type *A = R.A.type();
    bool Ok = R.Un == UnOp::Neg   ? A->isInt() || A->isFloat()
              : R.Un == UnOp::Not ? A->isBool()
                                  : A->isInt();
    if (!Ok)
      err("V051", std::string(getUnOpName(R.Un)) + " of " + str(A));
    else if (R.Ty != A)
      err("V051", std::string(getUnOpName(R.Un)) + " of " + str(A) + " gives " +
                      str(R.Ty));
  }

  /// V052: a shift takes two integers (of any widths) and gives the first
  /// one's type. Other operators take two operands of one type: arithmetic
  /// (add, sub, mul, div) integers or floats, rem and the bit operators
  /// integers; `eq`/`ne` any scalar and ordering integers or floats, giving
  /// bool; the others give the operand type.
  void binary(const Rvalue &R) {
    bool OkA = operand(R.A), OkB = operand(R.B);
    if (!OkA || !OkB)
      return;
    const Type *A = R.A.type(), *B = R.B.type();
    std::string Op = getBinOpName(R.Bin);
    if (isShift(R.Bin)) {
      if (!A->isInt() || !B->isInt())
        err("V052", Op + " of " + str(A) + " by " + str(B));
      else if (R.Ty != A)
        err("V052", Op + " of " + str(A) + " gives " + str(R.Ty));
      return;
    }
    if (A != B) {
      err("V052", Op + " of different types " + str(A) + " and " + str(B));
      return;
    }
    bool Ok;
    switch (R.Bin) {
    case BinOp::Add:
    case BinOp::Sub:
    case BinOp::Mul:
    case BinOp::Div:
    case BinOp::Lt:
    case BinOp::Le:
    case BinOp::Gt:
    case BinOp::Ge:
      Ok = A->isInt() || A->isFloat();
      break;
    case BinOp::Eq:
    case BinOp::Ne:
      Ok = A->isScalar();
      break;
    default: // rem and the bit operators
      Ok = A->isInt();
      break;
    }
    if (!Ok) {
      err("V052", Op + " is not defined on " + str(A));
      return;
    }
    const Type *Res = isComparison(R.Bin) ? M.Ctx.boolean() : A;
    if (R.Ty != Res)
      err("V052",
          Op + " of " + str(A) + " gives " + str(Res) + ", not " + str(R.Ty));
  }

  /// V053: a cast goes between the kinds of types its kind names.
  void cast(const Rvalue &R) {
    if (!operand(R.A))
      return;
    const Type *A = R.A.type(), *T = R.Ty;
    bool Ok = false;
    switch (R.CastOp) {
    case CastKind::IntToInt:
      Ok = A->isInt() && T->isInt();
      break;
    case CastKind::IntToFloat:
      Ok = A->isInt() && T->isFloat();
      break;
    case CastKind::FloatToInt:
      Ok = A->isFloat() && T->isInt();
      break;
    case CastKind::FloatToFloat:
      Ok = A->isFloat() && T->isFloat();
      break;
    case CastKind::PtrToInt:
      Ok = A->isPtr() && T->isInt();
      break;
    case CastKind::IntToPtr:
      Ok = A->isInt() && T->isPtr();
      break;
    case CastKind::PtrToPtr:
      Ok = A->isPtr() && T->isPtr();
      break;
    }
    if (!Ok)
      err("V053", std::string(getCastKindName(R.CastOp)) + " from " + str(A) +
                      " to " + str(T));
  }

  /// V054: the address of a place is a pointer to the place's type.
  void addrOf(const Rvalue &R) {
    if (!place(R.P))
      return;
    if (!R.Ty->isPtr() || R.Ty->elem() != R.P.Ty)
      err("V054", "the address of a " + str(R.P.Ty) + " has type " + str(R.Ty));
  }

  /// V055: an aggregate builds a value of the type its kind names (a struct
  /// with its body), with one operand of the right type per field or
  /// element.
  void aggregate(const Rvalue &R) {
    const Type *T = R.Ty;
    bool KindOk = false;
    switch (R.Agg) {
    case AggKind::Tuple:
      KindOk = T->getKind() == Type::Kind::Tuple || T->isOption();
      break;
    case AggKind::Array:
      KindOk = T->getKind() == Type::Kind::Array;
      break;
    case AggKind::String:
      KindOk = T->getKind() == Type::Kind::String;
      break;
    case AggKind::Slice:
      KindOk = T->getKind() == Type::Kind::Slice;
      break;
    case AggKind::Struct:
      KindOk = T->getKind() == Type::Kind::Struct && T->hasBody();
      break;
    }
    if (!KindOk) {
      err("V055",
          std::string(getAggKindName(R.Agg)) + " aggregate of type " + str(T));
      return;
    }
    uint64_t N = R.Agg == AggKind::Array ? T->length() : T->numFields();
    if (R.Ops.size() != N) {
      err("V055", "an aggregate of " + str(T) + " has " + num(R.Ops.size()) +
                      " operands, it needs " + num(N));
      return;
    }
    for (unsigned I = 0; I < N; ++I) {
      ++Steps;
      if (!operand(R.Ops[I]))
        continue;
      const Type *Want =
          R.Agg == AggKind::Array ? T->elem() : T->fieldType(I, M.Ctx);
      if (R.Ops[I].type() != Want)
        err("V055", "aggregate operand " + num(I) + " has type " +
                        str(R.Ops[I].type()) + ", it needs " + str(Want));
    }
  }

  /// V057: the code generator builds an aggregate in its destination, one
  /// operand after the other, so the destination is a place of the frame (no
  /// Deref: another pointer may alias it) and no operand reads its local
  /// (`x = Agg(x.1, x.0)` would read a half-built x; the builder goes through
  /// a temporary, whose address is never taken).
  void aggregateInPlace(const Statement &S) {
    for (const PlaceElem &E : counted(Steps, S.Dest.Proj))
      if (E.K == PlaceElem::Deref) {
        err("V057", "an aggregate is built through a pointer");
        return;
      }
    for (const Operand &Op : counted(Steps, S.RV.Ops))
      if (Op.K != Operand::Const && Op.P.Local == S.Dest.Local) {
        err("V057", "an aggregate operand reads _" + num(S.Dest.Local) +
                        ", the local it is built in");
        return;
      }
  }

  /// V056: `len` is of a string, a slice or an array and gives u64.
  void len(const Rvalue &R) {
    if (!place(R.P))
      return;
    Type::Kind K = R.P.Ty->getKind();
    if (K != Type::Kind::String && K != Type::Kind::Slice &&
        K != Type::Kind::Array)
      err("V056", "len of " + str(R.P.Ty));
    else if (!R.Ty->isInt() || R.Ty->bits() != 64 || R.Ty->isSigned())
      err("V056", "len gives u64, not " + str(R.Ty));
  }

  //===--------------------------------------------------------------------===//
  // Terminators and edges: V013, V020-V028, V060-V064
  //===--------------------------------------------------------------------===//

  void terminator(const BasicBlock &BB) {
    const Terminator &T = BB.Term;
    if (!T.Set) {
      err("V020", "the block has no terminator");
      return;
    }
    span(T.Loc);
    switch (T.K) {
    case Terminator::Switch:
      switchOn(T);
      break;
    case Terminator::Assert:
      assertOn(T);
      break;
    case Terminator::Call:
      call(T);
      break;
    case Terminator::Drop:
      drop(T);
      break;
    case Terminator::Goto:
    case Terminator::Return:
    case Terminator::Unreachable:
    case Terminator::Resume:
      break;
    }
    targets(T);
    edges(BB, T);
    cleanupShape(BB, T);
    noUnwind(T);
  }

  /// V060: a switch is on an integer or a bool. V027: its case values fit
  /// the type and are distinct.
  void switchOn(const Terminator &T) {
    if (!operand(T.Discr))
      return;
    const Type *D = T.Discr.type();
    if (!D->isInt() && !D->isBool()) {
      err("V060", "a switch on " + str(D));
      return;
    }
    unsigned Bits = D->isBool() ? 1 : D->bits();
    llvm::SmallDenseSet<uint64_t, 8> Seen;
    for (const SwitchCase &C : counted(Steps, T.Cases)) {
      if (Bits < 64 && (C.Value >> Bits) != 0)
        err("V027", "switch case " + num(C.Value) + " does not fit " + str(D));
      if (!Seen.insert(C.Value).second)
        err("V027", "duplicate switch case " + num(C.Value));
    }
  }

  /// V060: an assert is on a bool. V065: its Args are the values its
  /// message shows: a Bounds assert has two integers (the index, as an i64 or
  /// a u64, then the length as a u64), every other kind has none.
  void assertOn(const Terminator &T) {
    if (operand(T.Discr) && !T.Discr.type()->isBool())
      err("V060", "an assert on " + str(T.Discr.type()));
    size_t Want = T.AK == AssertKind::Bounds ? 2 : 0;
    if (T.Args.size() != Want) {
      err("V065", std::string("a ") + getAssertKindName(T.AK) + " assert has " +
                      num(T.Args.size()) + " values, it needs " + num(Want));
      return;
    }
    for (const Operand &A : counted(Steps, T.Args))
      if (operand(A) && !A.type()->isInt())
        err("V065", "an assert value of type " + str(A.type()));
    if (Want == 2 && T.Args[0].type()->isInt() &&
        T.Args[0].type()->bits() != 64)
      err("V065", "the index of a bounds assert is an i64 or a u64, not " +
                      str(T.Args[0].type()));
    if (Want == 2 && T.Args[1].type()->isInt() &&
        (T.Args[1].type()->bits() != 64 || T.Args[1].type()->isSigned()))
      err("V065", "the length of a bounds assert is a u64, not " +
                      str(T.Args[1].type()));
  }

  /// V061: the callee is a value of a function type. V062: the arguments
  /// match its parameters in number and type. V063: the result goes to a
  /// place of its result type. V066: an aggregate result is written by the
  /// callee through a hidden pointer to the destination while the call runs,
  /// so the destination is a place of the frame (no Deref: another pointer
  /// may alias it) in a temporary whose address is never taken (nothing else
  /// reads it half written or holds it across the call; the builder calls
  /// into a fresh temporary).
  void call(const Terminator &T) {
    if (!operand(T.Discr))
      return;
    const Type *CT = T.Discr.type();
    if (CT->getKind() != Type::Kind::Func) {
      err("V061", "a call of a value of type " + str(CT));
      return;
    }
    llvm::ArrayRef<Type *> Ps = CT->elems();
    if (T.Args.size() != Ps.size()) {
      err("V062", "the call passes " + num(T.Args.size()) +
                      " arguments, the callee takes " + num(Ps.size()));
    } else {
      for (size_t I = 0; I < Ps.size(); ++I, ++Steps)
        if (operand(T.Args[I]) && T.Args[I].type() != Ps[I])
          err("V062", "argument " + num(I) + " has type " +
                          str(T.Args[I].type()) + ", the parameter is " +
                          str(Ps[I]));
    }
    if (place(T.Dest) && T.Dest.Ty != CT->ret())
      err("V063", "the call's result is " + str(CT->ret()) +
                      ", its destination " + str(T.Dest.Ty));
    else if (place(T.Dest) && isAggregateType(T.Dest.Ty))
      hiddenResult(T.Dest);
  }

  static bool isAggregateType(const Type *T) {
    switch (T->getKind()) {
    case Type::Kind::String:
    case Type::Kind::Slice:
    case Type::Kind::Array:
    case Type::Kind::Tuple:
    case Type::Kind::Struct:
    case Type::Kind::Option:
      return true;
    default:
      return false;
    }
  }

  void hiddenResult(const Place &D) {
    for (const PlaceElem &E : counted(Steps, D.Proj))
      if (E.K == PlaceElem::Deref) {
        err("V066", "an aggregate call result is written through a pointer");
        return;
      }
    if (F->Locals[D.Local].K != Local::Temp)
      err("V066", "an aggregate call result goes to _" + num(D.Local) +
                      ", which is not a temporary");
    else if (AddrTaken.count(D.Local))
      err("V066", "an aggregate call result goes to _" + num(D.Local) +
                      ", whose address is taken");
  }

  /// V064: a drop is of a place that holds a value.
  void drop(const Terminator &T) {
    if (place(T.Dest) && T.Dest.Ty->isUnit())
      err("V064", "a drop of a unit place");
  }

  const Function *directCallee(const Terminator &T) const {
    if (T.K == Terminator::Call && T.Discr.K == Operand::Const &&
        T.Discr.C.K == Constant::Func && T.Discr.C.Bits < M.Funcs.size())
      return M.Funcs[T.Discr.C.Bits].get();
    return nullptr;
  }

  /// V028: a call without a return target calls a NoReturn function
  /// directly; a drop and an assert always have a target.
  void targets(const Terminator &T) {
    if (T.Target != NoId)
      return;
    if (T.K == Terminator::Call) {
      const Function *C = directCallee(T);
      if (!C || !C->NoReturn)
        err("V028", "a call without a return target needs a direct callee "
                    "that never returns");
    } else if (T.K == Terminator::Drop || T.K == Terminator::Assert) {
      err("V028", "a drop or an assert without a target");
    }
  }

  static const char *blockKind(const BasicBlock &BB) {
    return BB.Catch ? "catch" : BB.Cleanup ? "cleanup" : "normal";
  }

  /// V021: every edge names an existing block. V013: none goes back to the
  /// entry. V023: a normal edge stays on its side of the cleanup boundary.
  /// V024: an unwind action names a cleanup block exactly when it is
  /// `cleanup`; a catch block is also a cleanup block.
  void edges(const BasicBlock &BB, const Terminator &T) {
    auto Into = [&](BlockId S, const char *What) {
      if (S >= F->Blocks.size()) {
        err("V021", std::string(What) + " to bb" + num(S) +
                        ", the function has " + num(F->Blocks.size()) +
                        " blocks");
        return false;
      }
      if (S == 0)
        err("V013", std::string(What) + " goes back to the entry block");
      return true;
    };
    forEachNormalEdge(T, [&](BlockId S) {
      if (!Into(S, "an edge"))
        return;
      const BasicBlock &To = F->Blocks[S];
      // A catch block may leave the unwind path: it can branch to a normal
      // block (after `recover`). Nothing else crosses the boundary, and only
      // a catch block branches into a catch block.
      bool Ok = BB.Catch ? true : !To.Catch && To.Cleanup == BB.Cleanup;
      if (!Ok)
        err("V023", std::string("an edge from a ") + blockKind(BB) +
                        " block to bb" + num(S) + ", a " + blockKind(To) +
                        " block");
    });
    if (!hasUnwind(T))
      return;
    const UnwindAction &U = T.Unwind;
    if (U.K == UnwindAction::Cleanup) {
      if (Into(U.Target, "an unwind edge") && !F->Blocks[U.Target].Cleanup)
        err("V024", "the unwind edge goes to bb" + num(U.Target) +
                        ", which is not a cleanup block");
    } else if (U.Target != NoId) {
      err("V024", std::string("a ") + unwindName(U.K) +
                      " unwind action names bb" + num(U.Target));
    }
  }

  /// V025: a plain cleanup block does not return; resume is only in a
  /// cleanup or catch block; and code in a plain cleanup block cannot unwind
  /// again (its action is `terminate` or `unreachable`: a panic while
  /// unwinding aborts, D4.6). A catch block (the `defer` handler) is exempt
  /// from the two outer rules: it returns after `recover`, and a deferred
  /// call that panics unwinds onward. V024: a catch block is also `Cleanup`.
  void cleanupShape(const BasicBlock &BB, const Terminator &T) {
    if (BB.Catch && !BB.Cleanup)
      err("V024", "a catch block is not marked as a cleanup block");
    if (BB.Cleanup && !BB.Catch && T.K == Terminator::Return)
      err("V025", "a cleanup block returns");
    if (!BB.Cleanup && T.K == Terminator::Resume)
      err("V025", "resume outside a cleanup block");
    if (BB.Cleanup && !BB.Catch && hasUnwind(T) &&
        (T.Unwind.K == UnwindAction::Continue ||
         T.Unwind.K == UnwindAction::Cleanup))
      err("V025", std::string("unwind ") + unwindName(T.Unwind.K) +
                      " in a cleanup block: a panic while unwinding "
                      "terminates");
  }

  /// V026: a NoUnwind function has no resume and lets no panic out (no
  /// `continue` action); a direct call of a NoUnwind callee has no cleanup
  /// edge; and `unreachable` on a direct call needs a NoUnwind callee (the
  /// call is lowered `nounwind`).
  void noUnwind(const Terminator &T) {
    if (F->NoUnwind && T.K == Terminator::Resume)
      err("V026", "resume in a nounwind function");
    if (!hasUnwind(T))
      return;
    if (F->NoUnwind && T.Unwind.K == UnwindAction::Continue)
      err("V026", "unwind continue in a nounwind function");
    const Function *C = directCallee(T);
    if (!C)
      return;
    if (C->NoUnwind && T.Unwind.K == UnwindAction::Cleanup)
      err("V026",
          "an unwind edge on a call of @" + C->Name + ", which cannot unwind");
    if (!C->NoUnwind && T.Unwind.K == UnwindAction::Unreachable)
      err("V026",
          "unwind unreachable on a call of @" + C->Name + ", which can unwind");
  }
};

} // namespace

std::vector<VerifyError> gir::verify(const Module &M, unsigned MaxErrors) {
  return Verifier(M, MaxErrors).run();
}

bool gir::verify(const Module &M, llvm::raw_ostream &OS) {
  std::vector<VerifyError> Errs = verify(M);
  for (const VerifyError &E : Errs)
    OS << "gir verifier: " << E.str() << '\n';
  return Errs.empty();
}
