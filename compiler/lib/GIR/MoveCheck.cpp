//===- MoveCheck.cpp - Use after move -------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See gopp/GIR/MoveCheck.h for the method.
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/MoveCheck.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/bit.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::gir;

namespace {

thread_local uint64_t Steps = 0;
constexpr uint32_t None = UINT32_MAX;

/// The maybe-moved paths of a point (their preorder numbers): a persistent
/// radix trie of 6-bit digits, the most significant first, hash-consed, so
/// the same set is the same id (Empty is 0). A node at level L holds the
/// digit L of its keys (level 0: a bitmap of the last digit), its children at
/// level L - 1; the root is at the lowest level that holds the largest key.
/// A union walks only where the two sets differ: a child the two share is
/// taken as it is, so the join of a set with itself plus some paths costs
/// those paths, in any order they were added (review R9: a list merged the
/// two whole when the paths were not added in increasing order). Every
/// operation walks at most 6 levels (32-bit keys).
class PathSets {
public:
  using Id = uint32_t;
  static constexpr Id Empty = 0;
  /// Test-only: no shared part is taken as it is (every union walks both
  /// sets whole), the quadratic mode of the stress tests' control.
  bool FullJoins = false;

  PathSets() { Nodes.emplace_back(); }

  Id add(Id S, uint32_t K) {
    if (S == Empty)
      return chain(K, levelOf(K));
    return insertAt(lift(S, std::max(level(S), levelOf(K))), K);
  }
  Id remove(Id S, uint32_t K) {
    if (S == Empty || levelOf(K) > level(S))
      return S;
    return norm(removeAt(S, K));
  }
  bool contains(Id S, uint32_t K) const {
    if (S == Empty || levelOf(K) > level(S))
      return false;
    for (Id N = S;;) {
      ++Steps;
      const Node &X = Nodes[N];
      if (X.Level == 0)
        return X.Bits >> digit(K, 0) & 1;
      N = child(N, digit(K, X.Level));
      if (N == Empty)
        return false;
    }
  }
  Id unite(Id A, Id B) {
    if (A == B || B == Empty)
      return A;
    if (A == Empty)
      return B;
    if (!FullJoins)
      if (auto It = Memo.find({std::min(A, B), std::max(A, B)});
          It != Memo.end())
        return It->second;
    unsigned L = std::max(level(A), level(B));
    Id R = norm(uniteAt(lift(A, L), lift(B, L)));
    if (!FullJoins)
      Memo[{std::min(A, B), std::max(A, B)}] = R;
    return R;
  }
  /// Is any key of S in [Lo, Hi)?
  bool anyInRange(Id S, uint32_t Lo, uint32_t Hi) const {
    return S != Empty && Lo < Hi && anyAt(S, 0, Lo, Hi);
  }
  /// The keys of S in [Lo, Hi), the biggest first.
  void rangeMembers(Id S, uint32_t Lo, uint32_t Hi,
                    llvm::SmallVectorImpl<uint32_t> &Out) const {
    if (S != Empty && Lo < Hi)
      collectAt(S, 0, Lo, Hi, Out);
  }

private:
  struct Node {
    uint8_t Level = 0;
    uint64_t Bits = 0;
    uint32_t Kids = 0; // the first child in Pool (one per bit, in order)
  };
  std::vector<Node> Nodes;
  std::vector<Id> Pool;
  llvm::DenseMap<uint64_t, llvm::SmallVector<Id, 1>> Index;
  llvm::DenseMap<std::pair<Id, Id>, Id> Memo;

  static unsigned digit(uint64_t K, unsigned L) { return (K >> (6 * L)) & 63; }
  static unsigned levelOf(uint32_t K) {
    unsigned L = 0;
    while (L < 5 && (uint64_t(K) >> (6 * (L + 1))) != 0)
      ++L;
    return L;
  }
  unsigned level(Id S) const { return Nodes[S].Level; }
  static uint64_t span(unsigned L) { return uint64_t(1) << (6 * (L + 1)); }
  Id child(Id N, unsigned D) const {
    const Node &X = Nodes[N];
    if (!(X.Bits >> D & 1))
      return Empty;
    uint64_t Below = D ? X.Bits & ((uint64_t(1) << D) - 1) : 0;
    return Pool[X.Kids + llvm::popcount(Below)];
  }
  Id make(unsigned Level, uint64_t Bits, llvm::ArrayRef<Id> Kids) {
    if (Bits == 0)
      return Empty;
    size_t Hash = llvm::hash_combine(
        Level, Bits, llvm::hash_combine_range(Kids.begin(), Kids.end()));
    // (The top bit clear: never one of DenseMap's reserved keys.)
    llvm::SmallVector<Id, 1> &Bucket = Index[uint64_t(Hash) >> 1];
    for (Id C : Bucket) {
      const Node &X = Nodes[C];
      if (X.Level == Level && X.Bits == Bits &&
          std::equal(Kids.begin(), Kids.end(), Pool.begin() + X.Kids))
        return C;
    }
    Node X;
    X.Level = Level;
    X.Bits = Bits;
    X.Kids = Pool.size();
    Pool.insert(Pool.end(), Kids.begin(), Kids.end());
    Nodes.push_back(X);
    Bucket.push_back(Nodes.size() - 1);
    return Nodes.size() - 1;
  }
  Id chain(uint32_t K, unsigned Top) {
    Id C = make(0, uint64_t(1) << digit(K, 0), {});
    for (unsigned L = 1; L <= Top; ++L)
      C = make(L, uint64_t(1) << digit(K, L), {C});
    return C;
  }
  Id lift(Id S, unsigned Level) {
    while (level(S) < Level)
      S = make(level(S) + 1, 1, {S});
    return S;
  }
  Id norm(Id S) const {
    while (S != Empty && Nodes[S].Level > 0 && Nodes[S].Bits == 1)
      S = Pool[Nodes[S].Kids];
    return S;
  }
  /// N's kids with the one of digit D set to C (or gone when C is Empty).
  Id withChild(Id N, unsigned D, Id C) {
    Node X = Nodes[N];
    llvm::SmallVector<Id, 16> Kids;
    uint64_t Bits = X.Bits;
    for (unsigned E = 0, I = 0; E < 64; ++E) {
      if (!(Bits >> E & 1)) {
        if (E == D && C != Empty)
          Kids.push_back(C);
        continue;
      }
      Id Old = Pool[X.Kids + I++];
      if (E == D) {
        if (C != Empty)
          Kids.push_back(C);
      } else {
        Kids.push_back(Old);
      }
    }
    Bits = C == Empty ? Bits & ~(uint64_t(1) << D) : Bits | uint64_t(1) << D;
    return make(X.Level, Bits, Kids);
  }
  Id insertAt(Id N, uint32_t K) {
    ++Steps;
    Node X = Nodes[N];
    if (X.Level == 0) {
      uint64_t Bits = X.Bits | uint64_t(1) << digit(K, 0);
      return Bits == X.Bits ? N : make(0, Bits, {});
    }
    unsigned D = digit(K, X.Level);
    Id C = child(N, D);
    Id New = C == Empty ? chain(K, X.Level - 1) : insertAt(C, K);
    return New == C ? N : withChild(N, D, New);
  }
  Id removeAt(Id N, uint32_t K) {
    ++Steps;
    Node X = Nodes[N];
    if (X.Level == 0) {
      uint64_t Bits = X.Bits & ~(uint64_t(1) << digit(K, 0));
      return Bits == X.Bits ? N : make(0, Bits, {});
    }
    unsigned D = digit(K, X.Level);
    Id C = child(N, D);
    if (C == Empty)
      return N;
    Id New = removeAt(C, K);
    return New == C ? N : withChild(N, D, New);
  }
  /// The union of two sets at one level.
  Id uniteAt(Id A, Id B) {
    ++Steps;
    if (A == B && !FullJoins)
      return A;
    if (A == Empty)
      return B;
    if (B == Empty)
      return A;
    Node X = Nodes[A], Y = Nodes[B];
    uint64_t Bits = X.Bits | Y.Bits;
    if (X.Level == 0)
      return Bits == X.Bits ? A : Bits == Y.Bits ? B : make(0, Bits, {});
    // (A child the two share is taken as it is, not walked: the union costs
    // the nodes where the sets differ, each at most 64 children wide.)
    llvm::SmallVector<Id, 16> Kids;
    for (unsigned D = 0; D < 64; ++D)
      if (Bits >> D & 1) {
        Id CA = child(A, D), CB = child(B, D);
        Kids.push_back(CA == CB && !FullJoins       ? CA
                       : CA != Empty && CB != Empty ? uniteAt(CA, CB)
                       : CA != Empty                ? CA
                                                    : CB);
      }
    return make(X.Level, Bits, Kids);
  }
  bool anyAt(Id N, uint64_t Base, uint64_t Lo, uint64_t Hi) const {
    ++Steps;
    const Node &X = Nodes[N];
    if (X.Level == 0) {
      uint64_t From = Lo > Base ? Lo - Base : 0;
      uint64_t To = Hi - Base < 64 ? Hi - Base : 64;
      if (From >= To)
        return false;
      uint64_t Mask = (To == 64 ? ~uint64_t(0) : (uint64_t(1) << To) - 1) &
                      ~((uint64_t(1) << From) - 1);
      return (X.Bits & Mask) != 0;
    }
    uint64_t CS = span(X.Level - 1);
    for (unsigned D = 0; D < 64; ++D) {
      uint64_t B = Base + D * CS;
      if (!(X.Bits >> D & 1) || B + CS <= Lo || B >= Hi)
        continue;
      if (anyAt(child(N, D), B, Lo, Hi))
        return true;
    }
    return false;
  }
  void collectAt(Id N, uint64_t Base, uint64_t Lo, uint64_t Hi,
                 llvm::SmallVectorImpl<uint32_t> &Out) const {
    ++Steps;
    const Node &X = Nodes[N];
    if (X.Level == 0) {
      for (unsigned D = 64; D-- > 0;)
        if (X.Bits >> D & 1 && Base + D >= Lo && Base + D < Hi)
          Out.push_back(uint32_t(Base + D));
      return;
    }
    uint64_t CS = span(X.Level - 1);
    for (unsigned D = 64; D-- > 0;) {
      uint64_t B = Base + D * CS;
      if (!(X.Bits >> D & 1) || B + CS <= Lo || B >= Hi)
        continue;
      collectAt(child(N, D), B, Lo, Hi, Out);
    }
  }
};

using SetId = PathSets::Id;
constexpr SetId Unset = UINT32_MAX;

bool trackable(const PlaceElem &E) {
  return E.K == PlaceElem::Field || E.K == PlaceElem::ConstIndex;
}

uint64_t elemKey(const PlaceElem &E) { return (uint64_t(E.K) << 32) | E.Val; }

/// The paths some Move names, as a trie with preorder numbers.
struct Trie {
  struct Node {
    uint32_t Parent = None;
    LocalId L = NoId;
    Span MovedAt;
    uint32_t Pre = 0, End = 0; // the node and its subtree are [Pre, End)
  };
  std::vector<Node> N;
  /// (parent, the step's kind and value) -> node
  llvm::DenseMap<std::pair<uint32_t, uint64_t>, uint32_t> Child;
  llvm::DenseMap<LocalId, uint32_t> Root;
  std::vector<uint32_t> ByPre;

  uint32_t rootOf(LocalId L, bool Create) {
    auto It = Root.find(L);
    if (It != Root.end())
      return It->second;
    if (!Create)
      return None;
    N.push_back({None, L, {}, 0, 0});
    Root[L] = N.size() - 1;
    return N.size() - 1;
  }
  uint32_t childOf(uint32_t P, const PlaceElem &E, bool Create) {
    std::pair<uint32_t, uint64_t> Key{P, elemKey(E)};
    auto It = Child.find(Key);
    if (It != Child.end())
      return It->second;
    if (!Create)
      return None;
    N.push_back({P, N[P].L, {}, 0, 0});
    Child[Key] = N.size() - 1;
    return N.size() - 1;
  }

  /// The node of the longest tracked prefix of P; Whole says the whole place
  /// is that node.
  uint32_t find(const Place &P, bool &Whole, bool Create) {
    uint32_t Cur = rootOf(P.Local, Create);
    Whole = false;
    if (Cur == None)
      return None;
    size_t K = 0;
    for (; K < P.Proj.size(); ++K) {
      if (!trackable(P.Proj[K]))
        break;
      uint32_t Nx = childOf(Cur, P.Proj[K], Create);
      if (Nx == None)
        break;
      Cur = Nx;
    }
    Whole = K == P.Proj.size();
    return Cur;
  }

  void number() {
    // Preorder over the forest, without recursion.
    std::vector<std::vector<uint32_t>> Kids(N.size());
    std::vector<uint32_t> Roots;
    for (uint32_t I = 0; I < N.size(); ++I)
      (N[I].Parent == None ? Roots : Kids[N[I].Parent]).push_back(I);
    ByPre.clear();
    struct Frame {
      uint32_t Node;
      size_t Next;
    };
    for (uint32_t R : Roots) {
      llvm::SmallVector<Frame, 16> St{{R, 0}};
      N[R].Pre = ByPre.size();
      ByPre.push_back(R);
      while (!St.empty()) {
        Frame &F = St.back();
        if (F.Next < Kids[F.Node].size()) {
          uint32_t C = Kids[F.Node][F.Next++];
          N[C].Pre = ByPre.size();
          ByPre.push_back(C);
          St.push_back({C, 0});
        } else {
          N[F.Node].End = ByPre.size();
          St.pop_back();
        }
      }
    }
  }
};

class Checker {
public:
  Checker(const Function &F, bool Rescan, bool FullJoins)
      : F(F), Rescan(Rescan) {
    States.FullJoins = FullJoins;
  }
  std::vector<MoveError> run();

private:
  const Function &F;
  const bool Rescan;
  /// A variable that is a pointer local's target (a closure cell, an `&`
  /// range variable: Local::Deref) is that local: `*p.f` is the path `p.f`.
  Place norm(const Place &P) const {
    if (P.Local < F.Locals.size() && F.Locals[P.Local].Deref &&
        !P.Proj.empty() && P.Proj[0].K == PlaceElem::Deref) {
      Place Q = P;
      Q.Proj.erase(Q.Proj.begin());
      return Q;
    }
    return P;
  }
  Trie T;
  PathSets States;
  std::vector<MoveError> Errs;
  llvm::DenseSet<uint64_t> Said;

  void collect();
  SetId block(BlockId B, SetId S, bool Report);
  SetId read(const Place &Q, SetId S, Span At, bool Report);
  SetId operand(const Operand &O, SetId S, Span At, bool Report);
  SetId write(const Place &P, SetId S, Span At, bool Report);
  SetId kill(uint32_t Node, SetId S);
  void report(LocalId L, uint32_t MovedNode, Span At);
};

void Checker::collect() {
  auto Scan = [&](const Operand &O) {
    if (O.K != Operand::Move)
      return;
    Place P = norm(O.P);
    for (const PlaceElem &E : P.Proj)
      if (!trackable(E))
        return;
    bool Whole;
    uint32_t Nd = T.find(P, Whole, /*Create=*/true);
    (void)Nd;
  };
  for (const BasicBlock &BB : F.Blocks) {
    ++Steps;
    for (const Statement &S : BB.Stmts) {
      ++Steps;
      if (S.K != Statement::Assign)
        continue;
      Scan(S.RV.A);
      Scan(S.RV.B);
      for (const Operand &O : S.RV.Ops)
        Scan(O);
    }
    for (const Operand &O : BB.Term.Args)
      Scan(O);
  }
  T.number();
}

void Checker::report(LocalId L, uint32_t MovedNode, Span At) {
  const Local &Lo = F.Locals[L];
  if (Lo.Name.empty() || !Lo.Source)
    return;
  uint64_t Key = (uint64_t(At.Line) << 40) ^ (uint64_t(At.Col) << 20) ^ L;
  if (!Said.insert(Key).second)
    return;
  Errs.push_back({At, T.N[MovedNode].MovedAt, L});
}

SetId Checker::kill(uint32_t Node, SetId S) {
  llvm::SmallVector<uint32_t, 4> Dead;
  States.rangeMembers(S, T.N[Node].Pre, T.N[Node].End, Dead);
  for (uint32_t P : Dead)
    S = States.remove(S, P);
  return S;
}

SetId Checker::read(const Place &Q0, SetId S, Span At, bool Report) {
  if (Rescan) // (test-only: every moved path, at every read)
    for (uint32_t K = 0; K < T.N.size(); ++K) {
      ++Steps;
      (void)States.contains(S, K);
    }
  if (S == PathSets::Empty)
    return S;
  const Place Q = norm(Q0);
  bool Whole;
  uint32_t Nd = T.find(Q, Whole, /*Create=*/false);
  if (Nd == None)
    return S;
  ++Steps;
  // Something above it, or itself, was moved.
  for (uint32_t A = Nd; A != None; A = T.N[A].Parent) {
    ++Steps;
    if (States.contains(S, T.N[A].Pre)) {
      if (Report)
        report(Q.Local, A, At);
      return S;
    }
  }
  // The whole place is read after a part of it was moved.
  if (Whole && States.anyInRange(S, T.N[Nd].Pre, T.N[Nd].End)) {
    if (Report) {
      llvm::SmallVector<uint32_t, 1> One;
      States.rangeMembers(S, T.N[Nd].Pre, T.N[Nd].End, One);
      report(Q.Local, T.ByPre[One.back()], At);
    }
  }
  return S;
}

SetId Checker::operand(const Operand &O, SetId S, Span At, bool Report) {
  if (O.K == Operand::Const)
    return S;
  S = read(O.P, S, At, Report);
  if (O.K == Operand::Move) {
    const Place P = norm(O.P);
    for (const PlaceElem &E : P.Proj)
      if (!trackable(E))
        return S;
    bool Whole;
    uint32_t Nd = T.find(P, Whole, /*Create=*/false);
    if (Nd != None && Whole) {
      if (!T.N[Nd].MovedAt.isValid())
        T.N[Nd].MovedAt = At;
      S = States.add(S, T.N[Nd].Pre);
    }
  }
  return S;
}

SetId Checker::write(const Place &P0, SetId S, Span At, bool Report) {
  const Place P = norm(P0);
  bool Whole;
  uint32_t Nd = T.find(P, Whole, /*Create=*/false);
  if (Nd == None)
    return S;
  if (Whole) {
    // The place has a value again, and so has everything under it.
    return kill(Nd, S);
  }
  // A write through a pointer or an index reads the place that holds it.
  return read(P0, S, At, Report);
}

SetId Checker::block(BlockId B, SetId S, bool Report) {
  const BasicBlock &BB = F.Blocks[B];
  for (const Statement &St : BB.Stmts) {
    ++Steps;
    switch (St.K) {
    case Statement::Assign: {
      if (St.OldRead)
        break;
      S = operand(St.RV.A, S, St.Loc, Report);
      S = operand(St.RV.B, S, St.Loc, Report);
      for (const Operand &O : St.RV.Ops)
        S = operand(O, S, St.Loc, Report);
      if (St.RV.K == Rvalue::AddrOf || St.RV.K == Rvalue::Len)
        S = read(St.RV.P, S, St.Loc, Report);
      S = write(St.Dest, S, St.Loc, Report);
      break;
    }
    case Statement::StorageLive:
    case Statement::StorageDead: {
      uint32_t R = T.rootOf(St.L, false);
      if (R != None)
        S = kill(R, S);
      break;
    }
    case Statement::Nop:
      break;
    }
  }
  const Terminator &Tm = BB.Term;
  switch (Tm.K) {
  case Terminator::Switch:
  case Terminator::Assert:
    S = operand(Tm.Discr, S, Tm.Loc, Report);
    for (const Operand &O : Tm.Args)
      S = operand(O, S, Tm.Loc, Report);
    break;
  case Terminator::Call:
    S = operand(Tm.Discr, S, Tm.Loc, Report);
    for (const Operand &O : Tm.Args)
      S = operand(O, S, Tm.Loc, Report);
    S = write(Tm.Dest, S, Tm.Loc, Report);
    break;
  case Terminator::Return: {
    Place Ret;
    Ret.Local = 0;
    Ret.Ty = F.Locals.empty() ? nullptr : F.Locals[0].Ty;
    S = read(Ret, S, Tm.Loc, Report);
    break;
  }
  default:
    break;
  }
  return S;
}

std::vector<MoveError> Checker::run() {
  if (F.isDeclaration() || F.Blocks.empty())
    return {};
  collect();
  if (T.N.empty())
    return {};
  std::vector<SetId> In(F.Blocks.size(), Unset);
  In[0] = PathSets::Empty;
  std::vector<BlockId> Work{0};
  std::vector<uint8_t> Queued(F.Blocks.size(), 0);
  Queued[0] = 1;
  while (!Work.empty()) {
    BlockId B = Work.back();
    Work.pop_back();
    Queued[B] = 0;
    ++Steps;
    SetId Out = block(B, In[B], /*Report=*/false);
    forEachSuccessor(F.Blocks[B].Term, [&](const BlockId &Tg) {
      if (Tg >= F.Blocks.size())
        return;
      SetId New = In[Tg] == Unset ? Out : States.unite(In[Tg], Out);
      if (New == In[Tg])
        return;
      In[Tg] = New;
      if (!Queued[Tg]) {
        Queued[Tg] = 1;
        Work.push_back(Tg);
      }
    });
  }
  for (BlockId B = 0; B < F.Blocks.size(); ++B)
    if (In[B] != Unset)
      block(B, In[B], /*Report=*/true);
  return std::move(Errs);
}

} // namespace

std::vector<MoveError> gir::checkMoves(const Function &F, bool Rescan,
                                       bool FullJoins) {
  return Checker(F, Rescan, FullJoins).run();
}

uint64_t gir::moveCheckSteps() { return Steps; }
