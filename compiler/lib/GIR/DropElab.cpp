//===- DropElab.cpp - Drop elaboration ------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See DropElab.h for the design and the seam P04.26 plugs its cleanup pads
// into. Every walk here is iterative and visits each block, statement and
// owner-state change a bounded number of times: the cost is linear in the
// function, and the counted steps (dropElabSteps) are what the stress tests
// read.
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/DropElab.h"

#include "llvm/ADT/DenseSet.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::gir;

namespace {
thread_local uint64_t Steps = 0;
constexpr OwnerState Unset = UINT32_MAX;
} // namespace

uint64_t gir::dropElabSteps() { return Steps; }

//===----------------------------------------------------------------------===//
// OwnerStates
//===----------------------------------------------------------------------===//

OwnerState OwnerStates::cons(OwnerState Parent, LocalId L) {
  uint64_t Key = (uint64_t(Parent) << 32) | L;
  auto [It, New] = Index.try_emplace(Key, 0);
  if (New) {
    It->second = Nodes.size();
    Nodes.push_back({Parent, L, Nodes[Parent].Depth + 1});
  }
  return It->second;
}

OwnerState OwnerStates::add(OwnerState S, LocalId L) {
  // The owners above the place L belongs at, newest first.
  llvm::SmallVector<LocalId, 4> Above;
  OwnerState Base = S;
  while (Base != Empty && Nodes[Base].L >= L) {
    ++Steps;
    if (Nodes[Base].L == L)
      return S;
    Above.push_back(Nodes[Base].L);
    Base = Nodes[Base].Parent;
  }
  OwnerState R = cons(Base, L);
  for (size_t K = Above.size(); K > 0; --K)
    R = cons(R, Above[K - 1]);
  return R;
}

OwnerState OwnerStates::remove(OwnerState S, LocalId L) {
  llvm::SmallVector<LocalId, 4> Above;
  OwnerState Cur = S;
  while (Cur != Empty && Nodes[Cur].L > L) {
    ++Steps;
    Above.push_back(Nodes[Cur].L);
    Cur = Nodes[Cur].Parent;
  }
  if (Cur == Empty || Nodes[Cur].L != L)
    return S;
  OwnerState R = Nodes[Cur].Parent;
  for (size_t K = Above.size(); K > 0; --K)
    R = cons(R, Above[K - 1]);
  return R;
}

bool OwnerStates::contains(OwnerState S, LocalId L) const {
  while (S != Empty && Nodes[S].L > L) {
    ++Steps;
    S = Nodes[S].Parent;
  }
  return S != Empty && Nodes[S].L == L;
}

void OwnerStates::members(OwnerState S,
                          llvm::SmallVectorImpl<LocalId> &Out) const {
  for (; S != Empty; S = Nodes[S].Parent)
    Out.push_back(Nodes[S].L);
}

bool OwnerStates::anyInRange(OwnerState S, LocalId Lo, LocalId Hi) const {
  while (S != Empty && Nodes[S].L >= Hi) {
    ++Steps;
    S = Nodes[S].Parent;
  }
  return S != Empty && Nodes[S].L >= Lo;
}

void OwnerStates::rangeMembers(OwnerState S, LocalId Lo, LocalId Hi,
                               llvm::SmallVectorImpl<LocalId> &Out) const {
  while (S != Empty && Nodes[S].L >= Hi) {
    ++Steps;
    S = Nodes[S].Parent;
  }
  for (; S != Empty && Nodes[S].L >= Lo; S = Nodes[S].Parent) {
    ++Steps;
    Out.push_back(Nodes[S].L);
  }
}

OwnerState OwnerStates::unite(OwnerState A, OwnerState B) {
  if (A == B || B == Empty)
    return A;
  if (A == Empty)
    return B;
  if (!FullJoins) {
    // A list and its own extension (the lists are hash-consed, so the shorter
    // is then the longer one's ancestor): the union is the longer one.
    OwnerState Long = A, Short = B;
    if (Nodes[Long].Depth < Nodes[Short].Depth)
      std::swap(Long, Short);
    OwnerState Up = Long;
    for (uint32_t D = Nodes[Long].Depth - Nodes[Short].Depth; D > 0; --D) {
      ++Steps;
      Up = Nodes[Up].Parent;
    }
    if (Up == Short)
      return Long;
  }
  llvm::SmallVector<LocalId, 8> X, Y, Merged;
  members(A, X);
  members(B, Y);
  // (Both are in decreasing order.)
  size_t I = 0, J = 0;
  while (I < X.size() || J < Y.size()) {
    ++Steps;
    if (J == Y.size() || (I < X.size() && X[I] > Y[J]))
      Merged.push_back(X[I++]);
    else if (I == X.size() || Y[J] > X[I])
      Merged.push_back(Y[J++]);
    else {
      Merged.push_back(X[I++]);
      ++J;
    }
  }
  OwnerState R = Empty;
  for (size_t K = Merged.size(); K > 0; --K)
    R = cons(R, Merged[K - 1]);
  return R;
}

//===----------------------------------------------------------------------===//
// The dataflow
//===----------------------------------------------------------------------===//

namespace {

/// An owner that the builder brackets with StorageLive/StorageDead: owned,
/// and not a parameter (a parameter lives from the entry to every Return).
bool isScopedOwner(const Function &F, LocalId L) {
  return L < F.Locals.size() && F.Locals[L].Owned &&
         F.Locals[L].K != Local::Arg && F.Locals[L].K != Local::Return;
}

OwnerState transfer(const Function &F, OwnerStates &S, OwnerState State,
                    const Statement &St) {
  if ((St.K == Statement::StorageLive || St.K == Statement::StorageDead) &&
      isScopedOwner(F, St.L))
    return St.K == Statement::StorageLive ? S.add(State, St.L)
                                          : S.remove(State, St.L);
  return State;
}

} // namespace

std::vector<OwnerState> gir::computeOwnerStates(const Function &F,
                                                OwnerStates &States) {
  std::vector<OwnerState> In(F.Blocks.size(), Unset);
  if (F.Blocks.empty())
    return In;
  In[0] = OwnerStates::Empty;
  std::vector<BlockId> Work{0};
  std::vector<uint8_t> Queued(F.Blocks.size(), 0);
  Queued[0] = 1;
  while (!Work.empty()) {
    BlockId B = Work.back();
    Work.pop_back();
    Queued[B] = 0;
    ++Steps;
    OwnerState State = In[B];
    for (const Statement &St : F.Blocks[B].Stmts) {
      ++Steps;
      State = transfer(F, States, State, St);
    }
    forEachSuccessor(F.Blocks[B].Term, [&](const BlockId &T) {
      if (T >= F.Blocks.size())
        return;
      OwnerState New = In[T] == Unset ? State : States.unite(In[T], State);
      if (New == In[T])
        return;
      In[T] = New;
      if (!Queued[T]) {
        Queued[T] = 1;
        Work.push_back(T);
      }
    });
  }
  for (OwnerState &S : In)
    if (S == Unset)
      S = OwnerStates::Empty;
  return In;
}

//===----------------------------------------------------------------------===//
// The elaboration
//===----------------------------------------------------------------------===//

namespace {

Statement zeroStatement(const Place &P, Span Loc, ScopeId Scope) {
  Statement S;
  S.K = Statement::Assign;
  S.Dest = P;
  S.Loc = Loc;
  S.Scope = Scope;
  S.RV.K = Rvalue::Use;
  S.RV.Ty = P.Ty;
  S.RV.A.K = Operand::Const;
  S.RV.A.C.K = Constant::Zero;
  S.RV.A.C.Ty = P.Ty;
  return S;
}

class Elaborator {
public:
  Elaborator(Module &M, Function &F, bool Rescan) : M(M), F(F), Rescan(Rescan) {
    States.FullJoins = Rescan;
  }

  bool run(std::string *Error);

private:
  Module &M;
  Function &F;
  const bool Rescan;
  OwnerStates States;
  std::vector<OwnerState> In;
  std::string Err;

  /// The owned locals known to hold all-zero bytes at the point being
  /// elaborated (after `L = zero` or a move out of L, in the same original
  /// block; a local whose address is taken is never tracked). A drop of one
  /// does nothing, so none is made.
  llvm::SmallDenseSet<LocalId, 8> Zero;
  std::vector<bool> Escaped;
  bool knownZero(const Place &P) const {
    // (A local the pass made has no address taken and is never an owner.)
    return P.Proj.empty() && (P.Local >= Escaped.size() || !Escaped[P.Local]) &&
           Zero.count(P.Local);
  }
  /// A statement wrote P: zero bytes when ZeroStore (a whole local), else it
  /// is no longer known to be zero.
  void wrote(const Place &P, bool ZeroStore) {
    if (ZeroStore && P.Proj.empty())
      Zero.insert(P.Local);
    else
      Zero.erase(P.Local);
  }

  /// The block being filled (its statements are appended; a drop ends it and
  /// continues in a new block of the same kind).
  BlockId Cur = 0;
  bool CleanupSide = false, Cold = false, CatchSide = false;

  /// The unwind action of a terminator that can run user code, at the point
  /// where the owners of State are live. P04.26 builds the cleanup pad of
  /// State here (OwnerStates::members(State) is its drop order); until then a
  /// panic does not drop, and the action stays what the lowering set.
  UnwindAction unwindFor(OwnerState State, const UnwindAction &Current) {
    (void)State;
    return Current;
  }

  void emit(Statement S) { F.Blocks[Cur].Stmts.push_back(std::move(S)); }
  /// Ends the current block with a drop of P by Glue and goes on in a new
  /// block.
  void dropHere(OwnerState State, const Place &P, FuncId Glue, Span Loc,
                ScopeId Scope);
  /// Rewrites the Move operands of an operand list: each becomes a Copy, and
  /// the moved places are collected.
  void takeMoves(Operand &O, llvm::SmallVectorImpl<Place> &Moved);
  bool hasWork() const;
};

bool Elaborator::hasWork() const {
  for (const Local &L : F.Locals)
    if (L.Owned)
      return true;
  for (const BasicBlock &BB : F.Blocks) {
    for (const Statement &S : BB.Stmts) {
      if (S.K != Statement::Assign)
        continue;
      if (S.DropOld != NoId || S.RV.A.K == Operand::Move ||
          S.RV.B.K == Operand::Move)
        return true;
      for (const Operand &O : S.RV.Ops)
        if (O.K == Operand::Move)
          return true;
    }
    for (const Operand &O : BB.Term.Args)
      if (O.K == Operand::Move)
        return true;
  }
  return false;
}

void Elaborator::dropHere(OwnerState State, const Place &P, FuncId Glue,
                          Span Loc, ScopeId Scope) {
  BlockId Next = F.addBlock(CleanupSide, CatchSide);
  F.Blocks[Next].Cold = Cold;
  Terminator T;
  T.K = Terminator::Drop;
  T.Set = true;
  T.Dest = P;
  T.Discr = constFunc(M, Glue);
  T.Target = Next;
  T.Loc = Loc;
  T.Scope = Scope;
  // A drop in a cleanup region, or in a function that cannot unwind, aborts
  // if the glue panics (V025, V026).
  T.Unwind = (CleanupSide || F.NoUnwind)
                 ? UnwindAction{UnwindAction::Terminate, NoId}
                 : unwindFor(State, UnwindAction{UnwindAction::Continue, NoId});
  F.Blocks[Cur].Term = std::move(T);
  Cur = Next;
}

void Elaborator::takeMoves(Operand &O, llvm::SmallVectorImpl<Place> &Moved) {
  if (O.K != Operand::Move)
    return;
  O.K = Operand::Copy;
  Moved.push_back(O.P);
}

bool Elaborator::run(std::string *Error) {
  if (F.isDeclaration() || !hasWork())
    return true;
  // Every owner that is not a parameter starts as zero bytes, so a drop of it
  // is right whatever path reached it (the entry's own statements first: V077).
  {
    std::vector<Statement> &Entry = F.Blocks[0].Stmts;
    size_t At = 0;
    // (A local the entry sets, a closure cell's pointer, has its value.)
    llvm::DenseSet<LocalId> Set;
    while (At < Entry.size() && Entry[At].Prologue) {
      if (Entry[At].K == Statement::Assign && Entry[At].Dest.Proj.empty())
        Set.insert(Entry[At].Dest.Local);
      ++At;
    }
    std::vector<Statement> Zeros;
    for (LocalId L = 0; L < F.Locals.size(); ++L)
      if (F.Locals[L].Owned && F.Locals[L].K != Local::Arg &&
          F.Locals[L].K != Local::Return && !Set.count(L)) {
        ++Steps;
        Statement Z = zeroStatement(localPlace(L, F.Locals[L].Ty), F.Loc,
                                    F.Locals[L].Scope);
        Z.Prologue = true;
        Zeros.push_back(std::move(Z));
      }
    Entry.insert(Entry.begin() + At, std::make_move_iterator(Zeros.begin()),
                 std::make_move_iterator(Zeros.end()));
  }
  In = computeOwnerStates(F, States);
  Escaped.assign(F.Locals.size() + 64, false);
  for (const BasicBlock &BB : F.Blocks)
    for (const Statement &S : BB.Stmts)
      if (S.K == Statement::Assign && S.RV.K == Rvalue::AddrOf &&
          S.RV.P.Local < Escaped.size())
        Escaped[S.RV.P.Local] = true;
  // The owned parameters, in the order they are dropped at a return (the last
  // one first, as C++ destroys them).
  llvm::SmallVector<LocalId, 4> OwnedParams;
  for (LocalId L = F.numParams(); L > 0; --L)
    if (F.Locals[L].Owned)
      OwnedParams.push_back(L);
  const size_t Orig = F.Blocks.size();
  for (BlockId B = 0; B < Orig; ++B) {
    ++Steps;
    if (Rescan) // (test-only: the states again for every block)
      (void)computeOwnerStates(F, States);
    Cur = B;
    CleanupSide = F.Blocks[B].Cleanup;
    CatchSide = F.Blocks[B].Catch;
    Cold = F.Blocks[B].Cold;
    std::vector<Statement> Stmts = std::move(F.Blocks[B].Stmts);
    Terminator Term = std::move(F.Blocks[B].Term);
    F.Blocks[B].Stmts.clear();
    OwnerState State = In[B];
    Zero.clear();
    if (B == 0)
      for (LocalId L = 0; L < F.Locals.size(); ++L)
        if (F.Locals[L].Owned && F.Locals[L].K != Local::Arg &&
            F.Locals[L].K != Local::Return)
          Zero.insert(L);
    for (Statement &S : Stmts) {
      ++Steps;
      switch (S.K) {
      case Statement::Assign: {
        if (S.DropOld != NoId) {
          // The value a moved operand reads is taken first, then the old
          // value is dropped (`s = s` keeps s's value), then the store.
          auto Hoist = [&](Operand &O) {
            if (O.K != Operand::Move)
              return;
            ++Steps;
            LocalId Tmp = F.addLocal([&] {
              Local L;
              L.K = Local::Temp;
              L.Ty = O.P.Ty;
              L.Loc = S.Loc;
              L.Scope = S.Scope;
              return L;
            }());
            Statement Cp =
                zeroStatement(localPlace(Tmp, O.P.Ty), S.Loc, S.Scope);
            Cp.RV.A = O;
            Cp.RV.A.K = Operand::Copy;
            emit(std::move(Cp));
            emit(zeroStatement(O.P, S.Loc, S.Scope));
            wrote(O.P, true);
            O = copyOf(localPlace(Tmp, O.P.Ty));
          };
          Hoist(S.RV.A);
          Hoist(S.RV.B);
          for (Operand &O : S.RV.Ops)
            Hoist(O);
          if (!knownZero(S.Dest))
            dropHere(State, S.Dest, S.DropOld, S.Loc, S.Scope);
          S.DropOld = NoId;
        }
        llvm::SmallVector<Place, 2> Moved;
        takeMoves(S.RV.A, Moved);
        takeMoves(S.RV.B, Moved);
        for (Operand &O : S.RV.Ops)
          takeMoves(O, Moved);
        Span Loc = S.Loc;
        ScopeId Scope = S.Scope;
        const Place Dest = S.Dest;
        const bool ZeroStore = S.RV.K == Rvalue::Use &&
                               S.RV.A.K == Operand::Const &&
                               S.RV.A.C.K == Constant::Zero;
        emit(std::move(S));
        wrote(Dest, ZeroStore);
        for (const Place &P : Moved) {
          emit(zeroStatement(P, Loc, Scope));
          wrote(P, true);
        }
        break;
      }
      case Statement::StorageDead:
        if (isScopedOwner(F, S.L)) {
          if (!States.contains(State, S.L)) {
            // (Not live: made without a StorageLive. It still holds a value
            // or zero bytes, so it is dropped all the same.)
            Err = "owned local _" + std::to_string(S.L) +
                  " dies without being live";
          }
          if (!knownZero(localPlace(S.L, F.Locals[S.L].Ty)))
            dropHere(State, localPlace(S.L, F.Locals[S.L].Ty),
                     F.Locals[S.L].Glue, S.Loc, S.Scope);
        }
        Zero.erase(S.L);
        State = transfer(F, States, State, S);
        emit(std::move(S));
        break;
      case Statement::StorageLive:
        State = transfer(F, States, State, S);
        emit(std::move(S));
        break;
      case Statement::Nop:
        emit(std::move(S));
        break;
      }
    }
    // The terminator.
    if (Term.K == Terminator::Call) {
      wrote(Term.Dest, false);
      // A moved argument is read and zeroed before the call: the callee owns
      // the copy.
      for (Operand &A : Term.Args) {
        if (A.K != Operand::Move)
          continue;
        ++Steps;
        LocalId Tmp = F.addLocal([&] {
          Local L;
          L.K = Local::Temp;
          L.Ty = A.P.Ty;
          L.Loc = Term.Loc;
          L.Scope = Term.Scope;
          return L;
        }());
        Statement Cp;
        Cp.K = Statement::Assign;
        Cp.Dest = localPlace(Tmp, A.P.Ty);
        Cp.Loc = Term.Loc;
        Cp.Scope = Term.Scope;
        Cp.RV.K = Rvalue::Use;
        Cp.RV.Ty = A.P.Ty;
        Cp.RV.A = A;
        Cp.RV.A.K = Operand::Copy;
        emit(std::move(Cp));
        emit(zeroStatement(A.P, Term.Loc, Term.Scope));
        wrote(A.P, true);
        A = copyOf(localPlace(Tmp, A.P.Ty));
      }
      Term.Unwind = unwindFor(State, Term.Unwind);
    } else if (Term.K == Terminator::Assert) {
      Term.Unwind = unwindFor(State, Term.Unwind);
    } else if (Term.K == Terminator::Return) {
      // What is still live at a return was not dropped by a StorageDead: the
      // builder missed it. Drop it here rather than leak it.
      llvm::SmallVector<LocalId, 4> Live;
      States.members(State, Live);
      for (LocalId L : Live) {
        Err = "owned local _" + std::to_string(L) + " is live at a return";
        dropHere(State, localPlace(L, F.Locals[L].Ty), F.Locals[L].Glue,
                 Term.Loc, Term.Scope);
      }
      for (LocalId L : OwnedParams) {
        ++Steps;
        dropHere(State, localPlace(L, F.Locals[L].Ty), F.Locals[L].Glue,
                 Term.Loc, Term.Scope);
      }
    }
    F.Blocks[Cur].Term = std::move(Term);
  }
  if (Error && !Err.empty())
    *Error = "fn " + F.Name + ": " + Err;
  return Err.empty();
}

} // namespace

bool gir::elaborateDrops(Module &M, Function &F, std::string *Error,
                         bool Rescan) {
  return Elaborator(M, F, Rescan).run(Error);
}

bool gir::elaborateDrops(Module &M, std::string *Error) {
  for (size_t K = 0; K < M.Funcs.size(); ++K)
    if (!elaborateDrops(M, *M.Funcs[K], Error))
      return false;
  return true;
}
