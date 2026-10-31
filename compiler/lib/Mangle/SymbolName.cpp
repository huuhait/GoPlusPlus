//===- SymbolName.cpp - Symbol names (D4.13, P04.17) ----------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The mangler, the parser and the printer. The format is in
// docs/spec/appendix-a-symbol-names.md. Rules that this file keeps in ONE
// place each:
//   - the tag of every kind and its arity: Kinds[];
//   - how an identifier is written: appendIdent / readIdent;
//   - the header of every kind: writeHeader / readHeader;
//   - the name of an operator method: opIdent / opSpelling;
//   - when a repeated type becomes a back reference: emit() (the one test is
//     `isType() && Size >= BackRefMin`);
//   - what a child may be: kidOk().
// Every walk uses an explicit stack, so the depth of a name is limited by
// memory only.
//
//===----------------------------------------------------------------------===//

#include "gopp/Mangle/SymbolName.h"

#include "gopp/Basic/WorkCount.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <vector>

using namespace gopp;
using namespace gopp::sym;
using llvm::StringRef;

/// Test-only work counters (M8), per thread.
static thread_local WorkCounts Counts;
WorkCounts sym::workCounts() { return Counts; }

//===----------------------------------------------------------------------===//
// The table of kinds
//===----------------------------------------------------------------------===//

namespace {

constexpr int Var = -1; // children until `E`

struct KindInfo {
  const char *Prefix;
  int Arity;
};

// In the order of the enum.
constexpr KindInfo Kinds[] = {
    {"b", 0},   {"i", 0}, {"a", 0}, {"s", 0}, {"l", 0}, {"x", 0}, {"j", 0},
    {"h", 0},   {"t", 0}, {"m", 0}, {"y", 0}, {"z", 0}, {"f", 0}, {"d", 0},
    {"c", 0},   {"e", 0}, {"r", 0}, {"p", 0}, {"P", 1}, // Pointer
    {"S", 1},                                           // Slice
    {"A", 1},                                           // Array
    {"H", 2},                                           // Map
    {"O", 1},                                           // Option
    {"V", 1},                                           // View
    {"W", 1},                                           // Borrowed
    {"X", 1},                                           // Chan
    {"L", Var},                                         // Tuple
    {"G", 2},                                           // Signature
    {"Z", Var},                                         // Struct
    {"q", 1},                                           // Field
    {"Y", Var},                                         // Interface
    {"n", 1},                                           // IMethod
    {"N", 0},                                           // Named
    {"g", 1},                                           // LocalNamed
    {"u", 0},                                           // Universe
    {"I", Var},                                         // Instance
    {"Q", 0},                                           // TypeParam
    {"k", Var},                                         // Unit
    {"U", 1},                                           // UnitType
    {"o", 0},                                           // Dim
    {"w", 0},                                           // ParamDim
    {"B", 0},                                           // BackRef
    {"F", 0},                                           // Func
    {"M", 1},                                           // Method
    {"To", 2},                                          // OpMethod
    {"R", 0},                                           // Var
    {"C", 1},                                           // Closure
    {"J", 0},                                           // PkgInit
    {"K", 0},                                           // InitFunc
    {"Ti", 1},                                          // TypeInfo
    {"Tv", 1},                                          // VTable
    {"Ta", 2},                                          // ITab
    {"Te", 1},                                          // EqGlue
    {"Th", 1},                                          // HashGlue
    {"Td", 1},                                          // DropGlue
    {"Tc", 1},                                          // CloneGlue
    {"TD", 0},                                          // Data
    {"TC", 1},                                          // CABIThunk
    {"TV", 1},                                          // MethodValue
    {"TE", 1},                                          // MethodExpr
    {"TP", 1},                                          // Promoted
    {"TI", 1},                                          // IfaceThunk
    {"Tm", 1},                                          // MethodTable
    {"Tn", 1},                                          // IfaceDesc
};
static_assert(sizeof(Kinds) / sizeof(Kinds[0]) ==
                  static_cast<size_t>(Kind::NumKinds),
              "one entry per kind");

const KindInfo &info(Kind K) { return Kinds[static_cast<size_t>(K)]; }
StringRef prefixOf(Kind K) { return info(K).Prefix; }

/// The kind with this tag at the start of In[Pos..], and the tag length.
bool lookupKind(StringRef In, size_t Pos, Kind &K, size_t &Len) {
  static const struct Table {
    int8_t One[256];
    int8_t T[256];
    Table() {
      std::fill(One, One + 256, -1);
      std::fill(T, T + 256, -1);
      for (size_t I = 0; I < static_cast<size_t>(Kind::NumKinds); ++I) {
        StringRef P = Kinds[I].Prefix;
        if (P.size() == 1)
          One[static_cast<unsigned char>(P[0])] = static_cast<int8_t>(I);
        else
          T[static_cast<unsigned char>(P[1])] = static_cast<int8_t>(I);
      }
    }
  } Tab;
  unsigned char C = In[Pos];
  if (C == 'T') {
    if (Pos + 1 >= In.size())
      return false;
    int V = Tab.T[static_cast<unsigned char>(In[Pos + 1])];
    if (V < 0)
      return false;
    K = static_cast<Kind>(V);
    Len = 2;
    return true;
  }
  int V = Tab.One[C];
  if (V < 0)
    return false;
  K = static_cast<Kind>(V);
  Len = 1;
  return true;
}

bool isBasicKind(Kind K) { return K >= Kind::Bool && K <= Kind::UnsafePointer; }
/// The numeric kinds that can carry a unit (UNI-6): not uintptr.
bool isUnitBase(Kind K) {
  return K >= Kind::Int && K <= Kind::Complex128 && K != Kind::Uintptr;
}

//===----------------------------------------------------------------------===//
// Identifiers
//===----------------------------------------------------------------------===//

bool isPlain(unsigned char C, bool First) {
  return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_' ||
         (!First && C >= '0' && C <= '9');
}

void appendHex(std::string &Out, uint32_t V) {
  static const char Digits[] = "0123456789abcdef";
  char Buf[8];
  int N = 0;
  do {
    Buf[N++] = Digits[V & 15];
    V >>= 4;
  } while (V);
  while (N)
    Out.push_back(Buf[--N]);
}

void appendDec(std::string &Out, uint64_t V) {
  char Buf[24];
  int N = 0;
  do {
    Buf[N++] = static_cast<char>('0' + V % 10);
    V /= 10;
  } while (V);
  while (N)
    Out.push_back(Buf[--N]);
}

/// A well-formed UTF-8 sequence at S[I..]: the code point and the length.
bool decodeUtf8(StringRef S, size_t I, uint32_t &CP, size_t &Len) {
  unsigned char C = S[I];
  if (C < 0x80) {
    CP = C;
    Len = 1;
    return true;
  }
  size_t N;
  uint32_t Min;
  if ((C & 0xE0) == 0xC0) {
    N = 2;
    CP = C & 0x1F;
    Min = 0x80;
  } else if ((C & 0xF0) == 0xE0) {
    N = 3;
    CP = C & 0x0F;
    Min = 0x800;
  } else if ((C & 0xF8) == 0xF0) {
    N = 4;
    CP = C & 0x07;
    Min = 0x10000;
  } else {
    return false;
  }
  if (I + N > S.size())
    return false;
  for (size_t K = 1; K < N; ++K) {
    unsigned char D = S[I + K];
    if ((D & 0xC0) != 0x80)
      return false;
    CP = (CP << 6) | (D & 0x3F);
  }
  if (CP < Min || CP > 0x10FFFF || (CP >= 0xD800 && CP <= 0xDFFF))
    return false;
  Len = N;
  return true;
}

void appendUtf8(std::string &Out, uint32_t CP) {
  if (CP < 0x80) {
    Out.push_back(static_cast<char>(CP));
  } else if (CP < 0x800) {
    Out.push_back(static_cast<char>(0xC0 | (CP >> 6)));
    Out.push_back(static_cast<char>(0x80 | (CP & 0x3F)));
  } else if (CP < 0x10000) {
    Out.push_back(static_cast<char>(0xE0 | (CP >> 12)));
    Out.push_back(static_cast<char>(0x80 | ((CP >> 6) & 0x3F)));
    Out.push_back(static_cast<char>(0x80 | (CP & 0x3F)));
  } else {
    Out.push_back(static_cast<char>(0xF0 | (CP >> 18)));
    Out.push_back(static_cast<char>(0x80 | ((CP >> 12) & 0x3F)));
    Out.push_back(static_cast<char>(0x80 | ((CP >> 6) & 0x3F)));
    Out.push_back(static_cast<char>(0x80 | (CP & 0x3F)));
  }
}

/// The identifier: its length in decimal, then the escaped text. A letter or
/// underscore (a digit, but not as the first character) stands for itself;
/// every other code point is `$` + lowercase hex + `$`; a byte that is not
/// part of well-formed UTF-8 is `$x` + two hex digits + `$`.
void appendIdent(std::string &Out, StringRef Name) {
  std::string Enc;
  Enc.reserve(Name.size());
  for (size_t I = 0; I < Name.size();) {
    ++Counts.MangleSteps;
    unsigned char C = Name[I];
    if (isPlain(C, I == 0)) {
      Enc.push_back(static_cast<char>(C));
      ++I;
      continue;
    }
    uint32_t CP;
    size_t Len;
    if (decodeUtf8(Name, I, CP, Len)) {
      Enc.push_back('$');
      appendHex(Enc, CP);
      Enc.push_back('$');
      I += Len;
    } else {
      static const char Digits[] = "0123456789abcdef";
      Enc += "$x";
      Enc.push_back(Digits[C >> 4]);
      Enc.push_back(Digits[C & 15]);
      Enc.push_back('$');
      ++I;
    }
  }
  appendDec(Out, Enc.size());
  // An empty identifier is `0_`: a bare `0` could run into a digit that
  // follows it.
  if (Enc.empty())
    Out.push_back('_');
  Out += Enc;
}

void appendPath(std::string &Out, StringRef Path) {
  size_t Start = 0;
  for (;;) {
    ++Counts.MangleSteps;
    size_t Slash = Path.find('/', Start);
    if (Slash == StringRef::npos) {
      appendIdent(Out, Path.substr(Start));
      break;
    }
    appendIdent(Out, Path.substr(Start, Slash - Start));
    Start = Slash + 1;
  }
  Out.push_back('E');
}

void appendNum(std::string &Out, uint64_t V) {
  appendDec(Out, V);
  Out.push_back('_');
}

void appendSNum(std::string &Out, uint64_t Bits) {
  int64_t V = static_cast<int64_t>(Bits);
  if (V < 0) {
    Out.push_back('n');
    appendDec(Out, 0 - static_cast<uint64_t>(V));
  } else {
    appendDec(Out, static_cast<uint64_t>(V));
  }
  Out.push_back('_');
}

//===----------------------------------------------------------------------===//
// Operator method names (ADR-0024): the fixed table
//===----------------------------------------------------------------------===//

constexpr unsigned AnyCount = ~0u;

struct OpName {
  const char *Spelling;
  unsigned MinParams, MaxParams;
  const char *Mnemonic;
};

/// The name of an operator method in a symbol is `op_` and a mnemonic from
/// this table, chosen by the spelling and the number of parameters (`-` with
/// none is `op_neg`, with one `op_sub`); a `@Right` method (EXP-26) has
/// `opr_` in place of `op_`. A spelling the table has no row for at that
/// count (a future operator, a malformed declaration) is `opx_` (`oprx_`) and
/// the spelling. Rows are in the order of the appendix.
constexpr OpName OpNames[] = {
    {"+", 1, 1, "add"},
    {"-", 1, 1, "sub"},
    {"*", 1, 1, "mul"},
    {"/", 1, 1, "quo"},
    {"%", 1, 1, "rem"},
    {"&", 1, 1, "and"},
    {"|", 1, 1, "or"},
    {"^", 1, 1, "xor"},
    {"<<", 1, 1, "shl"},
    {">>", 1, 1, "shr"},
    {"&^", 1, 1, "andnot"},
    {"+=", 1, 1, "addassign"},
    {"-=", 1, 1, "subassign"},
    {"*=", 1, 1, "mulassign"},
    {"/=", 1, 1, "quoassign"},
    {"%=", 1, 1, "remassign"},
    {"&=", 1, 1, "andassign"},
    {"|=", 1, 1, "orassign"},
    {"^=", 1, 1, "xorassign"},
    {"<<=", 1, 1, "shlassign"},
    {">>=", 1, 1, "shrassign"},
    {"&^=", 1, 1, "andnotassign"},
    {"-", 0, 0, "neg"},
    {"+", 0, 0, "pos"},
    {"^", 0, 0, "compl"},
    {"!", 0, 0, "not"},
    {"*", 0, 0, "deref"},
    {"++", 0, 0, "inc"},
    {"--", 0, 0, "dec"},
    {"==", 1, 1, "eq"},
    {"<=>", 1, 1, "cmp"},
    {"&&", 1, 1, "land"},
    {"||", 1, 1, "lor"},
    {"[]", 1, AnyCount, "index"},
    {"[]=", 2, AnyCount, "setindex"},
    {"()", 0, AnyCount, "call"},
    {"<-", 1, 1, "send"},
    {"<-", 0, 0, "recv"},
    {"range", 0, AnyCount, "range"},
};

/// The identifier text an operator method is written with.
std::string opIdent(StringRef Spelling, size_t Params, bool Right) {
  std::string Lead = Right ? "opr" : "op";
  for (const OpName &Row : OpNames)
    if (Spelling == Row.Spelling && Params >= Row.MinParams &&
        Params <= Row.MaxParams)
      return Lead + "_" + Row.Mnemonic;
  return Lead + "x_" + Spelling.str();
}

/// The spelling of an operator method written as Ident, and whether it is a
/// `@Right` one; false if Ident is not an operator method name.
bool opSpelling(StringRef Ident, std::string &Spelling, bool &Right) {
  Right = Ident.starts_with("opr");
  if (!Right && !Ident.starts_with("op"))
    return false;
  StringRef Rest = Ident.drop_front(Right ? 3 : 2);
  if (Rest.starts_with("x_")) {
    Spelling = Rest.drop_front(2).str();
    return true;
  }
  if (!Rest.starts_with("_"))
    return false;
  for (const OpName &Row : OpNames)
    if (Rest.drop_front(1) == Row.Mnemonic) {
      Spelling = Row.Spelling;
      return true;
    }
  return false;
}

/// Everything after the tag, for the kinds that have a header.
void writeHeader(const Node &N, std::string &Out) {
  switch (N.K) {
  case Kind::Array:
  case Kind::Struct:
  case Kind::TypeParam:
  case Kind::Closure:
    appendNum(Out, N.Num);
    break;
  case Kind::Chan:
  case Kind::UnitType:
    Out.push_back(N.Sub);
    break;
  case Kind::Signature:
    appendNum(Out, N.Num);
    appendNum(Out, N.Aux);
    break;
  case Kind::Field:
    appendNum(Out, N.Num);
    if (N.Num & FieldPkg)
      appendPath(Out, N.A);
    appendIdent(Out, N.B);
    if (N.Num & FieldTag)
      appendIdent(Out, N.C);
    break;
  case Kind::IMethod:
    Out.push_back(N.A.empty() ? '0' : '1');
    if (!N.A.empty())
      appendPath(Out, N.A);
    appendIdent(Out, N.B);
    break;
  case Kind::Named:
  case Kind::Func:
  case Kind::Var:
    appendPath(Out, N.A);
    appendIdent(Out, N.B);
    break;
  case Kind::LocalNamed:
    appendNum(Out, N.Num);
    appendIdent(Out, N.B);
    break;
  case Kind::Universe:
  case Kind::Method:
  case Kind::Data:
    appendIdent(Out, N.B);
    break;
  case Kind::OpMethod:
    appendIdent(Out, opIdent(N.B, N.Kids[1]->Kids.size(), N.Num != 0));
    break;
  case Kind::Unit: {
    StringRef Num = N.A;
    if (Num.starts_with("-")) {
      Out.push_back('n');
      Num = Num.drop_front();
    }
    Out += Num;
    Out.push_back('_');
    Out += N.B;
    Out.push_back('_');
    break;
  }
  case Kind::Dim:
    appendPath(Out, N.A);
    appendIdent(Out, N.B);
    appendSNum(Out, N.Aux);
    break;
  case Kind::ParamDim:
    appendNum(Out, N.Num);
    appendSNum(Out, N.Aux);
    break;
  case Kind::PkgInit:
    appendPath(Out, N.A);
    break;
  case Kind::InitFunc:
    appendPath(Out, N.A);
    appendNum(Out, N.Num);
    break;
  default:
    break;
  }
}

} // namespace

std::string sym::encodeIdent(StringRef Name) {
  std::string S;
  appendIdent(S, Name);
  return S;
}

const char *sym::kindPrefix(Kind K) { return prefixOf(K).data(); }

const char *sym::kindName(Kind K) {
  static const char *const Names[] = {
      "Bool",       "Int",        "Int8",          "Int16",       "Int32",
      "Int64",      "Uint",       "Uint8",         "Uint16",      "Uint32",
      "Uint64",     "Uintptr",    "Float32",       "Float64",     "Complex64",
      "Complex128", "String",     "UnsafePointer", "Pointer",     "Slice",
      "Array",      "Map",        "Option",        "View",        "Borrowed",
      "Chan",       "Tuple",      "Signature",     "Struct",      "Field",
      "Interface",  "IMethod",    "Named",         "LocalNamed",  "Universe",
      "Instance",   "TypeParam",  "Unit",          "UnitType",    "Dim",
      "ParamDim",   "BackRef",    "Func",          "Method",      "OpMethod",
      "Var",        "Closure",    "PkgInit",       "InitFunc",    "TypeInfo",
      "VTable",     "ITab",       "EqGlue",        "HashGlue",    "DropGlue",
      "CloneGlue",  "Data",       "CABIThunk",     "MethodValue", "MethodExpr",
      "Promoted",   "IfaceThunk", "MethodTable",   "IfaceDesc",
  };
  static_assert(sizeof(Names) / sizeof(Names[0]) ==
                    static_cast<size_t>(Kind::NumKinds),
                "one name per kind");
  return Names[static_cast<size_t>(K)];
}

//===----------------------------------------------------------------------===//
// Nodes and the Context
//===----------------------------------------------------------------------===//

NodeClass Node::cls() const {
  switch (K) {
  case Kind::Instance:
    return Kids[0]->cls();
  case Kind::Field:
  case Kind::IMethod:
  case Kind::Dim:
  case Kind::ParamDim:
  case Kind::BackRef:
    return NodeClass::Other;
  default:
    return K >= Kind::Func ? NodeClass::Entity : NodeClass::Type;
  }
}

static void profileOf(llvm::FoldingSetNodeID &ID, Kind K, char Sub,
                      uint64_t Num, uint64_t Aux, StringRef A, StringRef B,
                      StringRef C, llvm::ArrayRef<const Node *> Kids) {
  ID.AddInteger(static_cast<unsigned>(K));
  ID.AddInteger(static_cast<unsigned>(static_cast<unsigned char>(Sub)));
  ID.AddInteger(Num);
  ID.AddInteger(Aux);
  ID.AddString(A);
  ID.AddString(B);
  ID.AddString(C);
  ID.AddInteger(Kids.size());
  for (const Node *Kid : counted(Counts.NodeSteps, Kids))
    ID.AddInteger(Kid->Id);
}

void Node::Profile(llvm::FoldingSetNodeID &ID) const {
  profileOf(ID, K, Sub, Num, Aux, A, B, C, Kids);
}

Context::Context() = default;

StringRef Context::save(StringRef S) {
  if (S.empty())
    return StringRef();
  char *Mem = Alloc.Allocate<char>(S.size());
  std::copy(S.begin(), S.end(), Mem);
  return StringRef(Mem, S.size());
}

const Node *Context::make(Kind K, llvm::ArrayRef<const Node *> Kids, char Sub,
                          uint64_t Num, uint64_t Aux, StringRef A, StringRef B,
                          StringRef C) {
  llvm::FoldingSetNodeID ID;
  profileOf(ID, K, Sub, Num, Aux, A, B, C, Kids);
  void *Pos = nullptr;
  if (Node *Old = Set.FindNodeOrInsertPos(ID, Pos))
    return Old;
  Node *N = new (Alloc.Allocate<Node>()) Node();
  N->K = K;
  N->Sub = Sub;
  N->Num = Num;
  N->Aux = Aux;
  N->A = save(A);
  N->B = save(B);
  N->C = save(C);
  if (!Kids.empty()) {
    const Node **Mem = Alloc.Allocate<const Node *>(Kids.size());
    std::copy(Kids.begin(), Kids.end(), Mem);
    N->Kids = llvm::ArrayRef<const Node *>(Mem, Kids.size());
  }
  N->Id = NextId++;
  Scratch.clear();
  writeHeader(*N, Scratch);
  constexpr uint64_t Cap = uint64_t(1) << 48;
  uint64_t Size =
      prefixOf(K).size() + Scratch.size() + (info(K).Arity == Var ? 1 : 0);
  for (const Node *Kid : counted(Counts.NodeSteps, Kids))
    Size = std::min(Cap, Size + Kid->Size);
  N->Size = Size;
  Set.InsertNode(N, Pos);
  return N;
}

//===----------------------------------------------------------------------===//
// Builders
//===----------------------------------------------------------------------===//

static char letterOf(Kind K) {
  assert(isBasicKind(K));
  return Kinds[static_cast<size_t>(K)].Prefix[0];
}

const Node *Context::basic(Kind K) {
  assert(isBasicKind(K));
  return make(K, {});
}
const Node *Context::array(const Node *T, uint64_t Len) {
  return make(Kind::Array, {T}, 0, Len);
}
const Node *Context::chan(char Dir, const Node *T) {
  assert(Dir == 'b' || Dir == 's' || Dir == 'r');
  return make(Kind::Chan, {T}, Dir);
}
const Node *Context::signature(const Node *Params, const Node *Results,
                               uint64_t Flags, uint64_t MoveMask) {
  assert(Params->K == Kind::Tuple && Results->K == Kind::Tuple);
  return make(Kind::Signature, {Params, Results}, 0, Flags, MoveMask);
}
const Node *Context::structOf(llvm::ArrayRef<const Node *> Fields,
                              bool Strict) {
  return make(Kind::Struct, Fields, 0, Strict ? 1 : 0);
}
const Node *Context::field(StringRef Name, StringRef Pkg, const Node *T,
                           uint64_t Flags, StringRef Tag) {
  Flags &= ~(uint64_t(FieldPkg) | FieldTag);
  if (!Pkg.empty())
    Flags |= FieldPkg;
  if (!Tag.empty())
    Flags |= FieldTag;
  return make(Kind::Field, {T}, 0, Flags, 0, Pkg, Name, Tag);
}
const Node *Context::imethod(StringRef Name, StringRef Pkg, const Node *Sig) {
  assert(Sig->K == Kind::Signature);
  return make(Kind::IMethod, {Sig}, 0, 0, 0, Pkg, Name);
}
const Node *Context::interfaceOf(llvm::ArrayRef<const Node *> Methods) {
  llvm::SmallVector<const Node *, 8> Sorted(Methods.begin(), Methods.end());
  std::stable_sort(Sorted.begin(), Sorted.end(),
                   [](const Node *X, const Node *Y) {
                     if (X->A != Y->A)
                       return X->A < Y->A;
                     return X->B < Y->B;
                   });
  return make(Kind::Interface, Sorted);
}
const Node *Context::named(StringRef Pkg, StringRef Name) {
  return make(Kind::Named, {}, 0, 0, 0, Pkg, Name);
}
const Node *Context::localNamed(const Node *Scope, uint64_t Ordinal,
                                StringRef Name) {
  return make(Kind::LocalNamed, {Scope}, 0, Ordinal, 0, {}, Name);
}
const Node *Context::universe(StringRef Name) {
  return make(Kind::Universe, {}, 0, 0, 0, {}, Name);
}
const Node *Context::instance(const Node *Base,
                              llvm::ArrayRef<const Node *> Args) {
  llvm::SmallVector<const Node *, 8> Kids;
  Kids.push_back(Base);
  Kids.append(Args.begin(), Args.end());
  return make(Kind::Instance, Kids);
}
const Node *Context::typeParam(uint64_t Index) {
  return make(Kind::TypeParam, {}, 0, Index);
}
const Node *Context::unit(StringRef Num, StringRef Den,
                          llvm::ArrayRef<const Node *> Dims) {
  llvm::SmallVector<const Node *, 8> Sorted(Dims.begin(), Dims.end());
  std::stable_sort(Sorted.begin(), Sorted.end(),
                   [](const Node *X, const Node *Y) {
                     if (X->K != Y->K)
                       return X->K == Kind::Dim;
                     if (X->K == Kind::ParamDim)
                       return X->Num < Y->Num;
                     if (X->A != Y->A)
                       return X->A < Y->A;
                     return X->B < Y->B;
                   });
  return make(Kind::Unit, Sorted, 0, 0, 0, Num, Den);
}
const Node *Context::dim(StringRef Pkg, StringRef Name, int64_t Exp) {
  return make(Kind::Dim, {}, 0, 0, static_cast<uint64_t>(Exp), Pkg, Name);
}
const Node *Context::paramDim(uint64_t Index, int64_t Exp) {
  return make(Kind::ParamDim, {}, 0, Index, static_cast<uint64_t>(Exp));
}
const Node *Context::unitType(Kind Base, const Node *U) {
  assert(isUnitBase(Base) && U->K == Kind::Unit);
  return make(Kind::UnitType, {U}, letterOf(Base));
}
const Node *Context::func(StringRef Pkg, StringRef Name) {
  return make(Kind::Func, {}, 0, 0, 0, Pkg, Name);
}
const Node *Context::method(const Node *Recv, StringRef Name,
                            const Node *Params, bool Right) {
  if (Params)
    return make(Kind::OpMethod, {Recv, Params}, 0, Right ? 1 : 0, 0, {}, Name);
  return make(Kind::Method, {Recv}, 0, 0, 0, {}, Name);
}
const Node *Context::var(StringRef Pkg, StringRef Name) {
  return make(Kind::Var, {}, 0, 0, 0, Pkg, Name);
}
const Node *Context::closure(const Node *Parent, uint64_t Ordinal) {
  return make(Kind::Closure, {Parent}, 0, Ordinal);
}
const Node *Context::pkgInit(StringRef Pkg) {
  return make(Kind::PkgInit, {}, 0, 0, 0, Pkg);
}
const Node *Context::initFunc(StringRef Pkg, uint64_t Ordinal) {
  return make(Kind::InitFunc, {}, 0, Ordinal, 0, Pkg);
}
const Node *Context::ofType(Kind K, const Node *T) {
  assert(K == Kind::TypeInfo || K == Kind::VTable || K == Kind::EqGlue ||
         K == Kind::HashGlue || K == Kind::DropGlue || K == Kind::CloneGlue ||
         K == Kind::MethodTable || K == Kind::IfaceDesc);
  return make(K, {T});
}
const Node *Context::itab(const Node *Concrete, const Node *Iface) {
  return make(Kind::ITab, {Concrete, Iface});
}
const Node *Context::data(StringRef Key) {
  return make(Kind::Data, {}, 0, 0, 0, {}, Key);
}
const Node *Context::wrapper(Kind K, const Node *E) {
  assert(K == Kind::CABIThunk || K == Kind::MethodValue ||
         K == Kind::MethodExpr || K == Kind::Promoted || K == Kind::IfaceThunk);
  return make(K, {E});
}

//===----------------------------------------------------------------------===//
// The mangler
//===----------------------------------------------------------------------===//

namespace {

/// Writes the tree rooted at Root to Out. A type that is written a second
/// time (and is long enough to gain) becomes `B<offset>_`, where offset is
/// where its first copy starts in Out.
void emit(std::string &Out, const Node *Root) {
  struct Item {
    const Node *N; // null: the `E` that ends a list
  };
  llvm::SmallVector<Item, 64> Stack;
  Stack.push_back({Root});
  llvm::DenseMap<uint32_t, size_t> First;
  while (!Stack.empty()) {
    ++Counts.MangleSteps;
    const Node *N = Stack.pop_back_val().N;
    if (!N) {
      Out.push_back('E');
      continue;
    }
    if (N->isType() && N->Size >= BackRefMin) {
      auto It = First.find(N->Id);
      if (It != First.end()) {
        Out.push_back('B');
        appendNum(Out, It->second);
        continue;
      }
      First[N->Id] = Out.size();
    }
    Out += prefixOf(N->K);
    writeHeader(*N, Out);
    if (info(N->K).Arity == Var)
      Stack.push_back({nullptr});
    for (size_t I = N->Kids.size(); I-- > 0; ++Counts.MangleSteps)
      Stack.push_back({N->Kids[I]});
  }
}

} // namespace

std::string sym::mangle(const Node *Entity) {
  assert(Entity->isEntity());
  std::string Out(Prefix);
  Out.reserve(std::min<uint64_t>(Entity->Size, uint64_t(1) << 26) + 3);
  emit(Out, Entity);
  return Out;
}

std::string sym::mangleType(const Node *T) {
  assert(T->isType());
  std::string Out;
  emit(Out, T);
  return Out;
}

//===----------------------------------------------------------------------===//
// The parser
//===----------------------------------------------------------------------===//

namespace {

bool isNamedType(const Node *N) {
  switch (N->K) {
  case Kind::Named:
  case Kind::LocalNamed:
  case Kind::Universe:
    return true;
  case Kind::Instance:
    return N->isType() && N->Kids[0]->K != Kind::Func;
  default:
    return false;
  }
}

bool isDeclaring(const Node *N) {
  switch (N->K) {
  case Kind::Func:
  case Kind::Method:
  case Kind::OpMethod:
  case Kind::Var:
  case Kind::Closure:
  case Kind::InitFunc:
    return true;
  case Kind::Instance:
    return N->isEntity();
  default:
    return false;
  }
}

/// May Kid be child number Idx of a Parent node?
bool kidOk(Kind Parent, size_t Idx, const Node *Kid) {
  switch (Parent) {
  case Kind::Pointer:
  case Kind::Slice:
  case Kind::Array:
  case Kind::Map:
  case Kind::Option:
  case Kind::View:
  case Kind::Borrowed:
  case Kind::Chan:
  case Kind::Tuple:
  case Kind::Field:
  case Kind::TypeInfo:
  case Kind::VTable:
  case Kind::EqGlue:
  case Kind::HashGlue:
  case Kind::DropGlue:
  case Kind::CloneGlue:
  case Kind::MethodTable:
  case Kind::IfaceDesc:
  case Kind::ITab:
    return Kid->isType();
  case Kind::Signature:
    return Kid->K == Kind::Tuple;
  case Kind::Struct:
    return Kid->K == Kind::Field;
  case Kind::Interface:
    return Kid->K == Kind::IMethod;
  case Kind::IMethod:
    return Kid->K == Kind::Signature;
  case Kind::LocalNamed:
  case Kind::Closure:
    return isDeclaring(Kid);
  case Kind::Instance:
    if (Idx == 0)
      return Kid->K == Kind::Named || Kid->K == Kind::LocalNamed ||
             Kid->K == Kind::Universe || Kid->K == Kind::Func ||
             Kid->K == Kind::Method || Kid->K == Kind::OpMethod;
    return Kid->isType();
  case Kind::Unit:
    return Kid->K == Kind::Dim || Kid->K == Kind::ParamDim;
  case Kind::UnitType:
    return Kid->K == Kind::Unit;
  case Kind::Method:
    return isNamedType(Kid);
  case Kind::OpMethod:
    return Idx == 0 ? isNamedType(Kid) : Kid->K == Kind::Tuple;
  case Kind::CABIThunk:
  case Kind::MethodValue:
  case Kind::MethodExpr:
  case Kind::Promoted:
  case Kind::IfaceThunk:
    return Kid->isEntity();
  default:
    return false;
  }
}

/// Interface methods and unit dimensions are written in one order.
bool orderedKids(Kind K, llvm::ArrayRef<const Node *> Kids) {
  for (size_t I = 1; I < Kids.size(); ++I) {
    ++Counts.ParseSteps;
    const Node *X = Kids[I - 1], *Y = Kids[I];
    if (K == Kind::Interface) {
      if (X->A > Y->A || (X->A == Y->A && X->B > Y->B))
        return false;
    } else if (K == Kind::Unit) {
      if (X->K != Y->K) {
        if (X->K != Kind::Dim)
          return false;
      } else if (X->K == Kind::ParamDim) {
        // Strictly increasing: a dimension twice is not canonical.
        if (X->Num >= Y->Num)
          return false;
      } else if (X->A > Y->A || (X->A == Y->A && X->B >= Y->B)) {
        return false;
      }
    }
  }
  return true;
}

struct Parser {
  Parser(Context &Ctx, StringRef In, ParseError &Err)
      : Ctx(Ctx), In(In), Err(Err) {}

  Context &Ctx;
  StringRef In;
  ParseError &Err;
  size_t Pos = 0;
  std::deque<std::string> Strings; // header strings of open nodes

  struct Open {
    Kind K;
    char Sub = 0;
    uint64_t Num = 0, Aux = 0;
    StringRef A, B, C;
    size_t KidBase = 0;
    size_t Start = 0;
  };
  std::vector<Open> Stack;
  std::vector<const Node *> KidBuf;
  llvm::DenseMap<size_t, const Node *> ByOffset;

  bool fail(const char *Msg, size_t At) {
    if (Err.Message.empty()) {
      Err.Message = Msg;
      Err.Pos = At;
    }
    return false;
  }
  bool fail(const char *Msg) { return fail(Msg, Pos); }

  StringRef keep(std::string S) {
    Strings.push_back(std::move(S));
    return Strings.back();
  }

  bool readDigits(StringRef &Digits) {
    size_t Start = Pos;
    while (Pos < In.size() && In[Pos] >= '0' && In[Pos] <= '9') {
      ++Counts.ParseSteps;
      ++Pos;
    }
    if (Pos == Start)
      return fail("a number is expected", Start);
    Digits = In.substr(Start, Pos - Start);
    if (Digits.size() > 1 && Digits[0] == '0')
      return fail("a number has a leading zero", Start);
    return true;
  }

  bool readU64(uint64_t &V, bool Terminated) {
    StringRef D;
    if (!readDigits(D))
      return false;
    if (D.size() > 20)
      return fail("a number is too large", Pos);
    unsigned __int128 Acc = 0;
    for (char C : D)
      Acc = Acc * 10 + (C - '0');
    if (Acc > UINT64_MAX)
      return fail("a number is too large", Pos);
    V = static_cast<uint64_t>(Acc);
    if (Terminated) {
      if (Pos >= In.size() || In[Pos] != '_')
        return fail("`_` is expected after a number");
      ++Pos;
    }
    return true;
  }

  bool readNum(uint64_t &V) { return readU64(V, true); }

  bool readSNum(uint64_t &Bits) {
    bool Neg = false;
    if (Pos < In.size() && In[Pos] == 'n') {
      Neg = true;
      ++Pos;
    }
    uint64_t V;
    if (!readU64(V, true))
      return false;
    if (Neg) {
      if (V == 0 || V > (uint64_t(1) << 63))
        return fail("a signed number is out of range", Pos);
      Bits = 0 - V;
    } else {
      if (V > uint64_t(INT64_MAX))
        return fail("a signed number is out of range", Pos);
      Bits = V;
    }
    return true;
  }

  /// A unit exponent: never 0 (a dimension to the power 0 is not in a
  /// canonical unit, UNI-5) and at most MaxUnitExp in magnitude, the
  /// checker's limit (UNI-6).
  bool readExponent(uint64_t &Bits) {
    size_t At = Pos;
    if (!readSNum(Bits))
      return false;
    int64_t E = static_cast<int64_t>(Bits);
    if (E == 0)
      return fail("a unit exponent is 0", At);
    if (E > MaxUnitExp || E < -MaxUnitExp)
      return fail("a unit exponent is out of range", At);
    return true;
  }

  bool readIdent(std::string &Out) {
    uint64_t Len;
    size_t At = Pos;
    if (!readU64(Len, false))
      return false;
    if (Len > In.size() - Pos)
      return fail("an identifier is longer than the text", At);
    if (Len == 0) {
      if (Pos >= In.size() || In[Pos] != '_')
        return fail("`_` is expected after an empty identifier");
      ++Pos;
      return true;
    }
    StringRef Enc = In.substr(Pos, Len);
    for (size_t I = 0; I < Enc.size();) {
      ++Counts.ParseSteps;
      unsigned char C = Enc[I];
      if (C != '$') {
        if (!isPlain(C, false))
          return fail("a character that must be escaped", Pos + I);
        Out.push_back(static_cast<char>(C));
        ++I;
        continue;
      }
      size_t Close = Enc.find('$', I + 1);
      if (Close == StringRef::npos)
        return fail("an escape is not closed", Pos + I);
      StringRef Body = Enc.substr(I + 1, Close - I - 1);
      bool Raw = Body.starts_with("x");
      if (Raw)
        Body = Body.drop_front();
      if (Body.empty() || Body.size() > (Raw ? 2u : 6u) ||
          (Raw && Body.size() != 2))
        return fail("a bad escape", Pos + I);
      uint32_t V = 0;
      for (char H : Body) {
        unsigned D;
        if (H >= '0' && H <= '9')
          D = H - '0';
        else if (H >= 'a' && H <= 'f')
          D = H - 'a' + 10;
        else
          return fail("a bad hex digit in an escape", Pos + I);
        V = V * 16 + D;
      }
      if (Raw) {
        Out.push_back(static_cast<char>(V));
      } else {
        if (V > 0x10FFFF || (V >= 0xD800 && V <= 0xDFFF))
          return fail("an escape is not a code point", Pos + I);
        appendUtf8(Out, V);
      }
      I = Close + 1;
    }
    Pos += Len;
    return true;
  }

  bool readPath(std::string &Out) {
    for (bool First = true;; First = false) {
      ++Counts.ParseSteps;
      if (Pos >= In.size())
        return fail("the text ends inside a package path");
      if (In[Pos] == 'E') {
        ++Pos;
        if (First)
          return fail("a package path has no segment");
        return true;
      }
      if (!First)
        Out.push_back('/');
      if (!readIdent(Out))
        return false;
    }
  }

  bool readChar(char &C, StringRef Allowed) {
    if (Pos >= In.size() || Allowed.find(In[Pos]) == StringRef::npos)
      return fail("a bad flag character");
    C = In[Pos++];
    return true;
  }

  bool readDecimal(std::string &Out) {
    StringRef D;
    if (!readDigits(D))
      return false;
    Out.append(D.begin(), D.end());
    return true;
  }

  bool expectUnderscore() {
    if (Pos >= In.size() || In[Pos] != '_')
      return fail("`_` is expected");
    ++Pos;
    return true;
  }

  bool readHeader(Open &O) {
    std::string S1, S2, S3;
    switch (O.K) {
    case Kind::Array:
    case Kind::Struct:
    case Kind::TypeParam:
    case Kind::Closure:
      if (!readNum(O.Num))
        return false;
      if (O.K == Kind::Struct && O.Num > 1)
        return fail("a bad struct flag");
      break;
    case Kind::Chan:
      if (!readChar(O.Sub, "bsr"))
        return false;
      break;
    case Kind::UnitType: {
      if (Pos >= In.size())
        return fail("the text ends inside a unit type");
      bool Ok = false;
      for (size_t K = 0; K < static_cast<size_t>(Kind::NumKinds); ++K)
        if (isUnitBase(static_cast<Kind>(K)) && Kinds[K].Prefix[0] == In[Pos])
          Ok = true;
      if (!Ok)
        return fail("a unit needs a numeric type");
      O.Sub = In[Pos++];
      break;
    }
    case Kind::Signature:
      if (!readNum(O.Num) || !readNum(O.Aux))
        return false;
      if (O.Num > SigVariadic)
        return fail("a bad signature flag");
      break;
    case Kind::Field:
      if (!readNum(O.Num))
        return false;
      if (O.Num >= 256)
        return fail("a bad field flag");
      if ((O.Num & FieldPkg) && !readPath(S1))
        return false;
      if (!readIdent(S2))
        return false;
      if ((O.Num & FieldTag) && !readIdent(S3))
        return false;
      if (((O.Num & FieldPkg) && S1.empty()) ||
          ((O.Num & FieldTag) && S3.empty()))
        return fail("a field flag without its text");
      break;
    case Kind::IMethod: {
      char F;
      if (!readChar(F, "01"))
        return false;
      if (F == '1' && !readPath(S1))
        return false;
      if (F == '1' && S1.empty())
        return fail("a method flag without its package");
      if (!readIdent(S2))
        return false;
      break;
    }
    case Kind::Named:
    case Kind::Func:
    case Kind::Var:
      if (!readPath(S1) || !readIdent(S2))
        return false;
      break;
    case Kind::LocalNamed:
      if (!readNum(O.Num) || !readIdent(S2))
        return false;
      break;
    case Kind::Universe:
    case Kind::Method:
    case Kind::Data:
      if (!readIdent(S2))
        return false;
      break;
    case Kind::OpMethod: {
      std::string Ident;
      size_t At = Pos;
      if (!readIdent(Ident))
        return false;
      bool Right;
      if (!opSpelling(Ident, S2, Right))
        return fail("not the name of an operator method", At);
      O.Num = Right ? 1 : 0;
      break;
    }
    case Kind::Unit: {
      if (Pos < In.size() && In[Pos] == 'n') {
        ++Pos;
        S1 = "-";
      }
      if (!readDecimal(S1) || !expectUnderscore() || !readDecimal(S2) ||
          !expectUnderscore())
        return false;
      break;
    }
    case Kind::Dim:
      if (!readPath(S1) || !readIdent(S2) || !readExponent(O.Aux))
        return false;
      break;
    case Kind::ParamDim:
      if (!readNum(O.Num) || !readExponent(O.Aux))
        return false;
      break;
    case Kind::PkgInit:
      if (!readPath(S1))
        return false;
      break;
    case Kind::InitFunc:
      if (!readPath(S1) || !readNum(O.Num))
        return false;
      break;
    default:
      break;
    }
    O.A = S1.empty() ? StringRef() : keep(std::move(S1));
    O.B = S2.empty() ? StringRef() : keep(std::move(S2));
    O.C = S3.empty() ? StringRef() : keep(std::move(S3));
    return true;
  }

  /// Makes the node of O whose children are the top of KidBuf.
  const Node *build(const Open &O) {
    llvm::ArrayRef<const Node *> Kids(KidBuf.data() + O.KidBase,
                                      KidBuf.size() - O.KidBase);
    const Node *N = Ctx.make(O.K, Kids, O.Sub, O.Num, O.Aux, O.A, O.B, O.C);
    KidBuf.resize(O.KidBase);
    return N;
  }

  /// N, which started at Start, is complete: give it to its parent, and
  /// close the parents it completes. False on a bad child.
  bool complete(const Node *N, size_t Start, bool Record) {
    for (;;) {
      ++Counts.ParseSteps;
      if (Record && N->isType())
        ByOffset[Start] = N;
      Record = true;
      if (Stack.empty()) {
        Done = N;
        return true;
      }
      Open &O = Stack.back();
      size_t Idx = KidBuf.size() - O.KidBase;
      if (!kidOk(O.K, Idx, N))
        return fail("a child of the wrong kind", Start);
      KidBuf.push_back(N);
      int Arity = info(O.K).Arity;
      if (Arity < 0 || static_cast<int>(Idx + 1) != Arity)
        return true;
      Open Copy = O;
      Stack.pop_back();
      N = build(Copy);
      Start = Copy.Start;
    }
  }

  const Node *Done = nullptr;

  const Node *run(size_t StartPos) {
    Pos = StartPos;
    while (!Done) {
      ++Counts.ParseSteps;
      if (Pos >= In.size()) {
        fail("the text ends before the name is complete");
        return nullptr;
      }
      if (!Stack.empty() && info(Stack.back().K).Arity < 0 && In[Pos] == 'E') {
        ++Pos;
        Open Copy = Stack.back();
        size_t Count = KidBuf.size() - Copy.KidBase;
        if (Copy.K == Kind::Instance && Count < 2) {
          fail("an instance needs a type argument", Pos - 1);
          return nullptr;
        }
        if (!orderedKids(Copy.K, llvm::ArrayRef<const Node *>(
                                     KidBuf.data() + Copy.KidBase, Count))) {
          fail("the children are not in the canonical order", Pos - 1);
          return nullptr;
        }
        Stack.pop_back();
        if (!complete(build(Copy), Copy.Start, true))
          return nullptr;
        continue;
      }
      size_t Start = Pos;
      Kind K;
      size_t Len;
      if (!lookupKind(In, Pos, K, Len)) {
        fail("an unknown tag");
        return nullptr;
      }
      Pos += Len;
      if (K == Kind::BackRef) {
        uint64_t Off;
        if (!readNum(Off))
          return nullptr;
        auto It = ByOffset.find(static_cast<size_t>(Off));
        if (Off > SIZE_MAX / 2 || It == ByOffset.end()) {
          fail("a back reference to nothing", Start);
          return nullptr;
        }
        if (!complete(It->second, Start, false))
          return nullptr;
        continue;
      }
      Open O;
      O.K = K;
      O.Start = Start;
      O.KidBase = KidBuf.size();
      if (!readHeader(O))
        return nullptr;
      if (info(K).Arity == 0) {
        if (!complete(build(O), Start, true))
          return nullptr;
      } else {
        Stack.push_back(O);
      }
    }
    return Done;
  }
};

} // namespace

static const Node *parseImpl(Context &Ctx, StringRef Text, ParseError &Err,
                             bool Symbol) {
  Err = ParseError();
  size_t Start = 0;
  if (Symbol) {
    if (!Text.starts_with(Prefix)) {
      Err.Message = "not a Go++ symbol (it does not start with _GP)";
      return nullptr;
    }
    Start = Prefix.size();
  }
  Parser P(Ctx, Text, Err);
  const Node *N = P.run(Start);
  if (!N)
    return nullptr;
  if (P.Pos != Text.size()) {
    Err.Message = "unexpected text after the name";
    Err.Pos = P.Pos;
    return nullptr;
  }
  if (Symbol ? !N->isEntity() : !N->isType()) {
    Err.Message =
        Symbol ? "the name is not an entity" : "the name is not a type";
    Err.Pos = Start;
    return nullptr;
  }
  // The one canonical form: whatever was accepted must be what the mangler
  // writes (a back reference where one belongs and nowhere else).
  std::string Again = Symbol ? mangle(N) : mangleType(N);
  if (Again != Text) {
    Err.Message = "not in the canonical form";
    Err.Pos = 0;
    size_t I = 0;
    while (I < Again.size() && I < Text.size() && Again[I] == Text[I]) {
      ++Counts.ParseSteps;
      ++I;
    }
    Err.Pos = I;
    return nullptr;
  }
  return N;
}

const Node *sym::parseSymbol(Context &Ctx, StringRef Text, ParseError &Err) {
  return parseImpl(Ctx, Text, Err, true);
}
const Node *sym::parseType(Context &Ctx, StringRef Text, ParseError &Err) {
  return parseImpl(Ctx, Text, Err, false);
}

//===----------------------------------------------------------------------===//
// The printer
//===----------------------------------------------------------------------===//

namespace {

const char *basicName(Kind K) {
  static const char *Names[] = {
      "bool",       "int",     "int8",          "int16",   "int32",
      "int64",      "uint",    "uint8",         "uint16",  "uint32",
      "uint64",     "uintptr", "float32",       "float64", "complex64",
      "complex128", "string",  "unsafe.Pointer"};
  return Names[static_cast<size_t>(K)];
}

/// How a node is printed where it is: the last parameter of a variadic
/// signature, the signature of an interface method (no `func`), a field of a
/// `@Strict` struct (only a written `@Required` is shown, as TypeString does).
enum Mode : unsigned { Normal, Variadic, SigOnly, StrictField };

struct Item {
  const Node *N; // null: Text
  StringRef Text;
  unsigned M = Normal;
};

struct Printer {
  explicit Printer(size_t MaxOut) : MaxOut(MaxOut) {}
  size_t MaxOut;
  bool Ok = true;
  std::string Out;
  std::vector<Item> Stack;

  // Items are collected in reading order, then pushed in reverse.
  std::vector<Item> Pending;
  // Every piece of output is pushed here, so the printer's count is made at
  // these three places (and in the loops over unit dimensions below).
  void lit(StringRef S) {
    ++Counts.PrintSteps;
    Pending.push_back({nullptr, S});
  }
  void node(const Node *N, unsigned M = Normal) {
    ++Counts.PrintSteps;
    Pending.push_back({N, StringRef(), M});
  }
  std::deque<std::string> Owned;
  void dyn(std::string S) {
    Counts.PrintSteps += 1 + S.size() / 16;
    Owned.push_back(std::move(S));
    Pending.push_back({nullptr, Owned.back()});
  }
  void flush() {
    for (size_t I = Pending.size(); I-- > 0;)
      Stack.push_back(Pending[I]);
    Pending.clear();
  }
  void list(llvm::ArrayRef<const Node *> L, StringRef Sep, bool LastVariadic) {
    for (size_t I = 0; I < L.size(); ++I) {
      if (I)
        lit(Sep);
      node(L[I], LastVariadic && I + 1 == L.size() ? Variadic : Normal);
    }
  }

  static std::string unitText(const Node *U) {
    std::string F;
    StringRef Num = U->A, Den = U->B;
    if (Num != "1" || Den != "1") {
      F = Num.str();
      if (Den != "1")
        F = "(" + F + "/" + Den.str() + ")";
    }
    // Mag is the exponent's magnitude (as unsigned: -INT64_MIN is not an
    // int64).
    auto Term = [](const Node *D, uint64_t Mag) {
      std::string T;
      if (D->K == Kind::ParamDim) {
        T = "$u" + std::to_string(D->Num);
      } else {
        T = D->A.str();
        if (!T.empty())
          T += '.';
        T += D->B.str();
      }
      if (Mag != 1)
        T += "^" + std::to_string(Mag);
      return T;
    };
    std::string Pos, Neg;
    unsigned NegCount = 0;
    for (const Node *D : counted(Counts.PrintSteps, U->Kids)) {
      int64_t E = static_cast<int64_t>(D->Aux);
      if (E > 0) {
        if (!Pos.empty())
          Pos += '*';
        Pos += Term(D, static_cast<uint64_t>(E));
      }
    }
    for (const Node *D : counted(Counts.PrintSteps, U->Kids)) {
      int64_t E = static_cast<int64_t>(D->Aux);
      if (E < 0) {
        if (!Neg.empty())
          Neg += '*';
        Neg += Term(D, 0 - static_cast<uint64_t>(E));
        ++NegCount;
      }
    }
    std::string Dims = Pos;
    if (!Neg.empty()) {
      if (NegCount > 1)
        Neg = "(" + Neg + ")";
      Dims = (Pos.empty() ? std::string("1") : Pos) + "/" + Neg;
    }
    if (F.empty())
      return Dims.empty() ? "1" : Dims;
    return Dims.empty() ? F : F + " " + Dims;
  }

  void put(StringRef S) {
    if (!Ok)
      return;
    if (Out.size() + S.size() > MaxOut) {
      Out.append(S.substr(0, MaxOut - Out.size()));
      Out += "...";
      Ok = false;
      return;
    }
    Out.append(S.begin(), S.end());
  }

  void signature(const Node *N, bool WithFunc) {
    if (WithFunc)
      lit("func");
    lit("(");
    list(N->Kids[0]->Kids, ", ", N->Num & SigVariadic);
    lit(")");
    llvm::ArrayRef<const Node *> R = N->Kids[1]->Kids;
    if (R.size() == 1) {
      lit(" ");
      node(R[0]);
    } else if (R.size() > 1) {
      lit(" (");
      list(R, ", ", false);
      lit(")");
    }
  }

  void expand(const Item &It) {
    const Node *N = It.N;
    if (!N) {
      put(It.Text);
      return;
    }
    if (It.M == Variadic) {
      if (N->K == Kind::Slice) {
        put("...");
        node(N->Kids[0]);
        flush();
        return;
      }
    }
    switch (N->K) {
    default:
      if (isBasicKind(N->K))
        put(basicName(N->K));
      return;
    case Kind::Pointer:
      lit("*");
      node(N->Kids[0]);
      break;
    case Kind::Slice:
      lit("[]");
      node(N->Kids[0]);
      break;
    case Kind::Array:
      lit("[");
      dyn(std::to_string(N->Num));
      lit("]");
      node(N->Kids[0]);
      break;
    case Kind::Map:
      lit("map[");
      node(N->Kids[0]);
      lit("]");
      node(N->Kids[1]);
      break;
    case Kind::Option:
      lit("Option[");
      node(N->Kids[0]);
      lit("]");
      break;
    case Kind::View:
      lit("View[");
      node(N->Kids[0]);
      lit("]");
      break;
    case Kind::Borrowed:
      lit("borrowed ");
      node(N->Kids[0]);
      break;
    case Kind::Chan: {
      bool Paren = N->Sub == 'b' && N->Kids[0]->K == Kind::Chan &&
                   N->Kids[0]->Sub == 'r';
      lit(N->Sub == 's' ? "chan<- " : N->Sub == 'r' ? "<-chan " : "chan ");
      if (Paren)
        lit("(");
      node(N->Kids[0]);
      if (Paren)
        lit(")");
      break;
    }
    case Kind::Tuple:
      lit("(");
      list(N->Kids, ", ", false);
      lit(")");
      break;
    case Kind::Signature:
      signature(N, It.M != SigOnly);
      break;
    case Kind::Struct:
      lit(N->Num ? "@Strict struct{" : "struct{");
      for (size_t I = 0; I < N->Kids.size(); ++I) {
        if (I)
          lit("; ");
        node(N->Kids[I], N->Num ? StrictField : Normal);
      }
      lit("}");
      break;
    case Kind::Field:
      if (N->Num & FieldParent)
        lit("extends ");
      else if (!(N->Num & FieldEmbedded)) {
        put(N->B);
        lit(" ");
      }
      node(N->Kids[0]);
      if (N->Num & (It.M == StrictField ? FieldMarkedRequired : FieldRequired))
        lit(" @Required");
      if (N->Num & FieldOption)
        lit(" @Option");
      if (N->Num & FieldTag) {
        lit(" \"");
        Pending.push_back({nullptr, N->C});
        lit("\"");
      }
      break;
    case Kind::Interface:
      // The empty method set is `any` and `interface{}`: one type.
      if (N->Kids.empty()) {
        lit("any");
        break;
      }
      lit("interface{");
      list(N->Kids, "; ", false);
      lit("}");
      break;
    case Kind::IMethod:
      Pending.push_back({nullptr, N->B});
      node(N->Kids[0], SigOnly);
      break;
    case Kind::Named:
    case Kind::Func:
    case Kind::Var:
      if (!N->A.empty()) {
        Pending.push_back({nullptr, N->A});
        lit(".");
      }
      Pending.push_back({nullptr, N->B});
      break;
    case Kind::Universe:
    case Kind::Data:
      if (N->K == Kind::Data)
        lit("data ");
      Pending.push_back({nullptr, N->B});
      break;
    case Kind::LocalNamed:
      node(N->Kids[0]);
      lit(".");
      Pending.push_back({nullptr, N->B});
      lit("#");
      dyn(std::to_string(N->Num));
      break;
    case Kind::Instance:
      node(N->Kids[0]);
      lit("[");
      list(N->Kids.drop_front(), ", ", false);
      lit("]");
      break;
    case Kind::TypeParam:
      lit("$");
      dyn(std::to_string(N->Num));
      break;
    case Kind::Unit:
      put(unitText(N));
      return;
    case Kind::UnitType: {
      const char *Name = nullptr;
      for (size_t K = 0; K < static_cast<size_t>(Kind::NumKinds); ++K)
        if (isUnitBase(static_cast<Kind>(K)) && Kinds[K].Prefix[0] == N->Sub)
          Name = basicName(static_cast<Kind>(K));
      put(Name ? Name : "?");
      lit("[");
      node(N->Kids[0]);
      lit("]");
      break;
    }
    case Kind::Method:
      node(N->Kids[0]);
      lit(".");
      Pending.push_back({nullptr, N->B});
      break;
    case Kind::OpMethod:
      node(N->Kids[0]);
      lit(".");
      Pending.push_back({nullptr, N->B});
      node(N->Kids[1]);
      if (N->Num)
        lit(" @Right");
      break;
    case Kind::Closure:
      node(N->Kids[0]);
      lit(".func");
      dyn(std::to_string(N->Num));
      break;
    case Kind::PkgInit:
      Pending.push_back({nullptr, N->A});
      lit(".init");
      break;
    case Kind::InitFunc:
      Pending.push_back({nullptr, N->A});
      lit(".init.");
      dyn(std::to_string(N->Num));
      break;
    case Kind::TypeInfo:
      lit("typeinfo for ");
      node(N->Kids[0]);
      break;
    case Kind::VTable:
      lit("vtable for ");
      node(N->Kids[0]);
      break;
    case Kind::ITab:
      lit("itab for ");
      node(N->Kids[0]);
      lit(" as ");
      node(N->Kids[1]);
      break;
    case Kind::EqGlue:
      lit("eq glue for ");
      node(N->Kids[0]);
      break;
    case Kind::HashGlue:
      lit("hash glue for ");
      node(N->Kids[0]);
      break;
    case Kind::DropGlue:
      lit("drop glue for ");
      node(N->Kids[0]);
      break;
    case Kind::CloneGlue:
      lit("clone glue for ");
      node(N->Kids[0]);
      break;
    case Kind::MethodTable:
      lit("method table for ");
      node(N->Kids[0]);
      break;
    case Kind::IfaceDesc:
      lit("interface descriptor for ");
      node(N->Kids[0]);
      break;
    case Kind::CABIThunk:
      lit("C-ABI thunk for ");
      node(N->Kids[0]);
      break;
    case Kind::MethodValue:
      lit("method value ");
      node(N->Kids[0]);
      break;
    case Kind::MethodExpr:
      lit("method expression ");
      node(N->Kids[0]);
      break;
    case Kind::Promoted:
      lit("promoted ");
      node(N->Kids[0]);
      break;
    case Kind::IfaceThunk:
      lit("interface thunk for ");
      node(N->Kids[0]);
      break;
    case Kind::Dim:
    case Kind::ParamDim:
    case Kind::BackRef:
      return;
    }
    flush();
  }

  // Pending items that are literals refer to node strings or string
  // literals; both live long enough.
};

} // namespace

Printed sym::print(const Node *Root, size_t MaxOut) {
  Printer P(MaxOut);
  P.Stack.push_back({Root, StringRef()});
  while (!P.Stack.empty() && P.Ok) {
    ++Counts.PrintSteps;
    Item It = P.Stack.back();
    P.Stack.pop_back();
    P.expand(It);
  }
  Printed R;
  R.Ok = P.Ok;
  R.Text = std::move(P.Out);
  return R;
}

DemangleResult sym::demangle(StringRef Symbol, size_t MaxOut) {
  DemangleResult R;
  Context Ctx;
  ParseError E;
  const Node *N = parseSymbol(Ctx, Symbol, E);
  if (!N) {
    R.Error = E.Message;
    R.ErrorPos = E.Pos;
    return R;
  }
  Printed P = print(N, MaxOut);
  R.Text = std::move(P.Text);
  if (!P.Ok) {
    R.Error = "the readable name is longer than the limit";
    return R;
  }
  R.Ok = true;
  return R;
}
