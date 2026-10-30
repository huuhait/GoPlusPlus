//===- Serialize.cpp - The GIR bitstream form -----------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/Serialize.h"

#include "gopp/Basic/Version.h"
#include "gopp/GIR/Verify.h"

#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Bitstream/BitstreamWriter.h"

#include <cstring>

using namespace gopp;
using namespace gopp::gir;
using llvm::ArrayRef;
using llvm::Error;
using llvm::Expected;
using llvm::SmallVector;
using llvm::StringRef;

namespace {

// Block ids (LLVM reserves 0..7).
enum BlockID : unsigned {
  IdentBlock = 8,
  ModuleBlock,
  TypesBlock,
  FuncBlock,
  IndexBlock
};

// Record codes. Each block has its own code space.
enum IdentCode : unsigned { I_Version = 1 };
enum ModuleCode : unsigned {
  M_Name = 1,
  M_File,
  M_String,
  M_FuncDecl,
  M_Global
};
enum TypeCode : unsigned {
  T_Unit = 1,
  T_Bool,
  T_Int,
  T_Float,
  T_String,
  T_Ptr,
  T_Slice,
  T_Array,
  T_Option,
  T_Tuple,
  T_Func,
  T_Struct,
  T_StructBody,
  T_Rec,    // a self-naming type: [name chars]
  T_RecBody // [id, kind (a TypeCode), length, n, part ids...]
};
enum FuncCode : unsigned { F_Body = 1, F_Local, F_Block, F_Stmt, F_Term };
enum IndexCode : unsigned { X_Body = 1 };

constexpr unsigned CodeWidth = 3;
constexpr uint8_t Magic[4] = {'G', 'I', 'R', 0xB2};

struct Rec {
  unsigned Code;
  SmallVector<uint64_t, 8> Vals;
};

//===----------------------------------------------------------------------===//
// Writer
//===----------------------------------------------------------------------===//

class Writer {
public:
  explicit Writer(const Module &M) : M(M) {}

  std::string run() {
    // 1. Encode everything in memory; this assigns the type ids in the order
    // the module is walked.
    std::vector<Rec> Decls;
    for (const Global &G : M.Globals)
      Decls.push_back(globalDecl(G));
    for (const auto &F : M.Funcs)
      Decls.push_back(funcDecl(*F));
    std::vector<std::vector<Rec>> Bodies(M.Funcs.size());
    for (FuncId I = 0; I < M.Funcs.size(); ++I)
      if (!M.Funcs[I]->isDeclaration())
        Bodies[I] = body(I, *M.Funcs[I]);
    std::vector<Rec> Types = typeRecords();

    // 2. Emit the blocks.
    SmallVector<char, 0> Buf;
    {
      llvm::BitstreamWriter W(Buf);
      for (uint8_t B : Magic)
        W.Emit(B, 8);
      W.EnterSubblock(IdentBlock, CodeWidth);
      emitRec(W, Rec{I_Version, chars(getGoppVersion())});
      W.ExitBlock();

      W.EnterSubblock(ModuleBlock, CodeWidth);
      emitRec(W, Rec{M_Name, chars(M.Name)});
      for (const std::string &F : M.Files)
        emitRec(W, Rec{M_File, chars(F)});
      for (const std::string &S : M.Strings)
        emitRec(W, Rec{M_String, chars(S)});
      W.EnterSubblock(TypesBlock, CodeWidth);
      for (const Rec &R : Types)
        emitRec(W, R);
      W.ExitBlock();
      for (const Rec &R : Decls)
        emitRec(W, R);
      std::vector<std::pair<FuncId, uint64_t>> Index;
      for (FuncId I = 0; I < M.Funcs.size(); ++I) {
        if (Bodies[I].empty())
          continue;
        Index.push_back({I, W.GetCurrentBitNo()});
        W.EnterSubblock(FuncBlock, CodeWidth);
        for (const Rec &R : Bodies[I])
          emitRec(W, R);
        W.ExitBlock();
      }
      uint64_t IndexBit = W.GetCurrentBitNo();
      W.EnterSubblock(IndexBlock, CodeWidth);
      for (auto [Id, Bit] : Index)
        emitRec(W, Rec{X_Body, {Id, Bit & 0xffffffffu, Bit >> 32}});
      W.ExitBlock();
      W.ExitBlock(); // the module
      W.Emit(static_cast<uint32_t>(IndexBit), 32);
      W.Emit(static_cast<uint32_t>(IndexBit >> 32), 32);
    }
    return std::string(Buf.data(), Buf.size());
  }

private:
  const Module &M;
  llvm::DenseMap<const Type *, unsigned> TypeIds;
  std::vector<const Type *> TypeOrder;
  std::vector<const Type *> PendingBodies;

  static SmallVector<uint64_t, 8> chars(StringRef S) {
    SmallVector<uint64_t, 8> V;
    for (unsigned char C : S)
      V.push_back(C);
    return V;
  }

  static void emitRec(llvm::BitstreamWriter &W, const Rec &R) {
    W.EmitRecord(R.Code, R.Vals);
  }

  /// The id of a type; components get smaller ids than what they make up.
  unsigned typeId(const Type *T) {
    auto It = TypeIds.find(T);
    if (It != TypeIds.end())
      return It->second;
    // Walk the components first, iteratively (type nesting is only as deep
    // as the source, but a walk of the module must not depend on it).
    SmallVector<std::pair<const Type *, unsigned>, 16> Stack;
    Stack.push_back({T, 0});
    while (!Stack.empty()) {
      auto [Cur, Next] = Stack.back();
      if (TypeIds.count(Cur)) {
        Stack.pop_back();
        continue;
      }
      SmallVector<const Type *, 4> Parts;
      // A self-naming type is a leaf here, like a struct: its parts may name
      // it, so they come after all other types (the bodies).
      switch (Cur->isRec() ? Type::Kind::Unit : Cur->getKind()) {
      case Type::Kind::Ptr:
      case Type::Kind::Slice:
      case Type::Kind::Array:
      case Type::Kind::Option:
        Parts.push_back(Cur->elem());
        break;
      case Type::Kind::Tuple:
        Parts.append(Cur->elems().begin(), Cur->elems().end());
        break;
      case Type::Kind::Func:
        Parts.append(Cur->elems().begin(), Cur->elems().end());
        Parts.push_back(Cur->ret());
        break;
      default:
        break; // a struct's fields come after all other types
      }
      if (Next < Parts.size()) {
        Stack.back().second = Next + 1;
        if (!TypeIds.count(Parts[Next]))
          Stack.push_back({Parts[Next], 0});
        continue;
      }
      TypeIds[Cur] = TypeOrder.size();
      TypeOrder.push_back(Cur);
      if ((Cur->getKind() == Type::Kind::Struct || Cur->isRec()) &&
          Cur->hasBody())
        PendingBodies.push_back(Cur);
      Stack.pop_back();
    }
    return TypeIds[T];
  }

  std::vector<Rec> typeRecords() {
    // The bodies of structs name other types: finish them all first.
    std::vector<Rec> Bodies;
    for (size_t I = 0; I < PendingBodies.size(); ++I) {
      const Type *S = PendingBodies[I];
      if (S->isRec()) {
        // The parts of the body: the element (or the tuple's elements).
        SmallVector<const Type *, 4> Parts;
        TypeCode Code = T_Ptr;
        switch (S->getKind()) {
        case Type::Kind::Slice:
          Code = T_Slice;
          Parts.push_back(S->elem());
          break;
        case Type::Kind::Array:
          Code = T_Array;
          Parts.push_back(S->elem());
          break;
        case Type::Kind::Option:
          Code = T_Option;
          Parts.push_back(S->elem());
          break;
        case Type::Kind::Tuple:
          Code = T_Tuple;
          Parts.append(S->elems().begin(), S->elems().end());
          break;
        default:
          Parts.push_back(S->elem());
          break;
        }
        Rec R{T_RecBody, {TypeIds[S], Code, S->length(), Parts.size()}};
        for (const Type *F : Parts)
          R.Vals.push_back(typeId(F));
        Bodies.push_back(std::move(R));
        continue;
      }
      Rec R{T_StructBody, {TypeIds[S], S->elems().size()}};
      for (const Type *F : S->elems())
        R.Vals.push_back(typeId(F));
      Bodies.push_back(std::move(R));
    }
    std::vector<Rec> Out;
    for (const Type *T : TypeOrder) {
      Rec R;
      switch (T->isRec() ? Type::Kind::Unit : T->getKind()) {
      case Type::Kind::Unit:
        R = T->isRec() ? Rec{T_Rec, chars(T->name())} : Rec{T_Unit, {}};
        break;
      case Type::Kind::Bool:
        R = {T_Bool, {}};
        break;
      case Type::Kind::Int:
        R = {T_Int, {T->bits(), T->isSigned()}};
        break;
      case Type::Kind::Float:
        R = {T_Float, {T->bits()}};
        break;
      case Type::Kind::String:
        R = {T_String, {}};
        break;
      case Type::Kind::Ptr:
        R = {T_Ptr, {TypeIds[T->elem()]}};
        break;
      case Type::Kind::Slice:
        R = {T_Slice, {TypeIds[T->elem()]}};
        break;
      case Type::Kind::Array:
        R = {T_Array, {TypeIds[T->elem()], T->length()}};
        break;
      case Type::Kind::Option:
        R = {T_Option, {TypeIds[T->elem()]}};
        break;
      case Type::Kind::Tuple:
        R = {T_Tuple, {}};
        for (const Type *E : T->elems())
          R.Vals.push_back(TypeIds[E]);
        break;
      case Type::Kind::Func:
        R = {T_Func, {TypeIds[T->ret()], T->fails()}};
        for (const Type *E : T->elems())
          R.Vals.push_back(TypeIds[E]);
        break;
      case Type::Kind::Struct:
        R = {T_Struct, chars(T->name())};
        break;
      }
      Out.push_back(std::move(R));
    }
    for (Rec &R : Bodies)
      Out.push_back(std::move(R));
    return Out;
  }

  void put(SmallVector<uint64_t, 8> &V, const Span &S) {
    V.push_back(S.File);
    V.push_back(S.Line);
    V.push_back(S.Col);
  }

  /// [ty, link, readonly, init kind, init type, init bits, span(3), name...]
  Rec globalDecl(const Global &G) {
    Rec R{M_Global, {}};
    R.Vals = {typeId(G.Ty),
              static_cast<uint64_t>(G.Link),
              G.ReadOnly,
              static_cast<uint64_t>(G.Init.K),
              typeId(G.Init.Ty ? G.Init.Ty : G.Ty),
              G.Init.Bits};
    put(R.Vals, G.Loc);
    R.Vals.append(chars(G.Name));
    return R;
  }

  Rec funcDecl(const Function &F) {
    Rec R{M_FuncDecl, {}};
    R.Vals = {typeId(F.Sig), static_cast<uint64_t>(F.Link),
              static_cast<uint64_t>(F.CC), F.NoUnwind, F.NoReturn};
    put(R.Vals, F.Loc);
    R.Vals.append(chars(F.Name));
    return R;
  }

  void put(SmallVector<uint64_t, 8> &V, const Place &P) {
    V.push_back(P.Local);
    V.push_back(P.Proj.size());
    for (const PlaceElem &E : P.Proj) {
      V.push_back(E.K);
      V.push_back(E.Val);
      V.push_back(typeId(E.Ty));
    }
  }

  void put(SmallVector<uint64_t, 8> &V, const Operand &O) {
    V.push_back(O.K);
    if (O.K == Operand::Const) {
      V.push_back(O.C.K);
      V.push_back(typeId(O.C.Ty));
      V.push_back(O.C.Bits);
    } else {
      put(V, O.P);
    }
  }

  void put(SmallVector<uint64_t, 8> &V, const Rvalue &R) {
    V.push_back(R.K);
    V.push_back(typeId(R.Ty));
    switch (R.K) {
    case Rvalue::Use:
      put(V, R.A);
      break;
    case Rvalue::AddrOf:
      V.push_back(R.Mut);
      put(V, R.P);
      break;
    case Rvalue::Binary:
      V.push_back(static_cast<uint64_t>(R.Bin));
      put(V, R.A);
      put(V, R.B);
      break;
    case Rvalue::Unary:
      V.push_back(static_cast<uint64_t>(R.Un));
      put(V, R.A);
      break;
    case Rvalue::Cast:
      V.push_back(static_cast<uint64_t>(R.CastOp));
      put(V, R.A);
      break;
    case Rvalue::Aggregate:
      V.push_back(static_cast<uint64_t>(R.Agg));
      V.push_back(R.Ops.size());
      for (const Operand &O : R.Ops)
        put(V, O);
      break;
    case Rvalue::Len:
      put(V, R.P);
      break;
    }
  }

  void put(SmallVector<uint64_t, 8> &V, const UnwindAction &U) {
    V.push_back(U.K);
    V.push_back(U.Target);
  }

  std::vector<Rec> body(FuncId Id, const Function &F) {
    std::vector<Rec> Out;
    Out.push_back({F_Body, {Id}});
    for (const Local &L : F.Locals) {
      Rec R{F_Local, {L.K, typeId(L.Ty), L.Mutable, L.Owned}};
      put(R.Vals, L.Loc);
      R.Vals.append(chars(L.Name));
      Out.push_back(std::move(R));
    }
    for (const BasicBlock &B : F.Blocks) {
      Out.push_back({F_Block, {B.Cleanup, B.Catch}});
      for (const Statement &S : B.Stmts) {
        Rec R{F_Stmt, {S.K}};
        put(R.Vals, S.Loc);
        switch (S.K) {
        case Statement::Assign:
          put(R.Vals, S.Dest);
          put(R.Vals, S.RV);
          break;
        case Statement::StorageLive:
        case Statement::StorageDead:
          R.Vals.push_back(S.L);
          break;
        case Statement::Nop:
          break;
        }
        Out.push_back(std::move(R));
      }
      const Terminator &T = B.Term;
      Rec R{F_Term, {T.K}};
      put(R.Vals, T.Loc);
      switch (T.K) {
      case Terminator::Goto:
        R.Vals.push_back(T.Target);
        break;
      case Terminator::Switch:
        put(R.Vals, T.Discr);
        R.Vals.push_back(T.Otherwise);
        R.Vals.push_back(T.Cases.size());
        for (const SwitchCase &C : T.Cases) {
          R.Vals.push_back(C.Value);
          R.Vals.push_back(C.Target);
        }
        break;
      case Terminator::Return:
      case Terminator::Unreachable:
      case Terminator::Resume:
        break;
      case Terminator::Call:
        put(R.Vals, T.Discr);
        put(R.Vals, T.Dest);
        R.Vals.push_back(T.Target);
        put(R.Vals, T.Unwind);
        R.Vals.push_back(T.Args.size());
        for (const Operand &O : T.Args)
          put(R.Vals, O);
        break;
      case Terminator::Drop:
        put(R.Vals, T.Dest);
        R.Vals.push_back(T.Target);
        put(R.Vals, T.Unwind);
        break;
      case Terminator::Assert:
        put(R.Vals, T.Discr);
        R.Vals.push_back(T.Expected);
        R.Vals.push_back(static_cast<uint64_t>(T.AK));
        R.Vals.push_back(T.Target);
        put(R.Vals, T.Unwind);
        R.Vals.push_back(T.Args.size());
        for (const Operand &O : T.Args)
          put(R.Vals, O);
        break;
      }
      Out.push_back(std::move(R));
    }
    return Out;
  }
};

//===----------------------------------------------------------------------===//
// Reader
//===----------------------------------------------------------------------===//

Error bad(const llvm::Twine &Msg) {
  return llvm::createStringError(std::errc::invalid_argument,
                                 "bad GIR file: %s", Msg.str().c_str());
}

/// Reads the values of one record; any overrun or bad id is an error that
/// the caller reports once (Failed).
struct Vals {
  ArrayRef<uint64_t> V;
  size_t Pos = 0;
  bool Failed = false;
  uint64_t next() {
    if (Pos >= V.size()) {
      Failed = true;
      return 0;
    }
    return V[Pos++];
  }
  std::string rest() {
    std::string S;
    while (Pos < V.size())
      S += static_cast<char>(V[Pos++]);
    return S;
  }
};

class Reader {
public:
  Reader(StringRef Bytes, Context &Ctx, const ReadOptions &Opts)
      : Bytes(Bytes), Ctx(Ctx), Opts(Opts) {}

  Expected<std::unique_ptr<Module>> run() {
    if (Bytes.size() < 4 + 8 || Bytes.size() % 4 != 0 ||
        std::memcmp(Bytes.data(), Magic, 4) != 0)
      return bad("not a GIR file");
    uint64_t IndexBit = 0;
    for (int I = 7; I >= 0; --I)
      IndexBit =
          IndexBit << 8 | static_cast<uint8_t>(Bytes[Bytes.size() - 8 + I]);
    if (IndexBit / 8 >= Bytes.size())
      return bad("the index is outside the file");

    llvm::BitstreamCursor C(llvm::MemoryBufferRef(Bytes, "gir"));
    for (int I = 0; I < 4; ++I)
      if (Expected<uint64_t> B = C.Read(8); !B)
        return B.takeError();
    // 1. The identification block.
    if (Error E = identification(C))
      return std::move(E);
    // 2. The module block: the index first, then the tables.
    Expected<llvm::BitstreamEntry> En = C.advance();
    if (!En)
      return En.takeError();
    if (En->Kind != llvm::BitstreamEntry::SubBlock || En->ID != ModuleBlock)
      return bad("no module block");
    if (Error E = C.EnterSubBlock(ModuleBlock))
      return std::move(E);
    uint64_t ModuleStart = C.GetCurrentBitNo();
    std::vector<std::pair<FuncId, uint64_t>> Index;
    if (Error E = index(C, IndexBit, Index))
      return std::move(E);
    if (Error E = C.JumpToBit(ModuleStart))
      return std::move(E);
    if (Error E = tables(C))
      return std::move(E);
    // 3. The bodies.
    for (auto [Id, Bit] : Index) {
      if (Id >= Funcs.size() || Bit / 8 >= Bytes.size())
        return bad("index names a function body that does not exist");
      if (!Opts.OnlyFunction.empty() && Funcs[Id]->Name != Opts.OnlyFunction)
        continue;
      if (Error E = C.JumpToBit(Bit))
        return std::move(E);
      Expected<llvm::BitstreamEntry> B = C.advance();
      if (!B)
        return B.takeError();
      if (B->Kind != llvm::BitstreamEntry::SubBlock || B->ID != FuncBlock)
        return bad("index does not point at a function body");
      if (Error E = C.EnterSubBlock(FuncBlock))
        return std::move(E);
      if (Error E = functionBody(C, Id))
        return std::move(E);
    }
    std::vector<VerifyError> Errs = verify(*Mod, 1);
    if (!Errs.empty())
      return bad("it does not verify: " + Errs.front().str());
    return std::move(Mod);
  }

private:
  StringRef Bytes;
  Context &Ctx;
  const ReadOptions &Opts;
  std::unique_ptr<Module> Mod;
  std::vector<Type *> Types;
  std::vector<Function *> Funcs;

  Error identification(llvm::BitstreamCursor &C) {
    Expected<llvm::BitstreamEntry> En = C.advance();
    if (!En)
      return En.takeError();
    if (En->Kind != llvm::BitstreamEntry::SubBlock || En->ID != IdentBlock)
      return bad("no identification block");
    if (Error E = C.EnterSubBlock(IdentBlock))
      return E;
    std::string Version;
    SmallVector<uint64_t, 16> V;
    while (true) {
      Expected<llvm::BitstreamEntry> R = C.advance();
      if (!R)
        return R.takeError();
      if (R->Kind == llvm::BitstreamEntry::EndBlock)
        break;
      if (R->Kind != llvm::BitstreamEntry::Record)
        return bad("unexpected entry in the identification block");
      V.clear();
      Expected<unsigned> Code = C.readRecord(R->ID, V);
      if (!Code)
        return Code.takeError();
      if (*Code == I_Version)
        for (uint64_t Ch : V)
          Version += static_cast<char>(Ch);
    }
    std::string Want = Opts.Version.empty() ? getGoppVersion() : Opts.Version;
    if (Version != Want)
      return llvm::createStringError(
          std::errc::invalid_argument,
          "GIR file was written by Go++ %s, this is %s: rebuild it",
          Version.c_str(), Want.c_str());
    return Error::success();
  }

  Error index(llvm::BitstreamCursor &C, uint64_t IndexBit,
              std::vector<std::pair<FuncId, uint64_t>> &Out) {
    if (Error E = C.JumpToBit(IndexBit))
      return E;
    Expected<llvm::BitstreamEntry> En = C.advance();
    if (!En)
      return En.takeError();
    if (En->Kind != llvm::BitstreamEntry::SubBlock || En->ID != IndexBlock)
      return bad("the trailer does not point at the index");
    if (Error E = C.EnterSubBlock(IndexBlock))
      return E;
    SmallVector<uint64_t, 4> V;
    while (true) {
      Expected<llvm::BitstreamEntry> R = C.advance();
      if (!R)
        return R.takeError();
      if (R->Kind == llvm::BitstreamEntry::EndBlock)
        return Error::success();
      if (R->Kind != llvm::BitstreamEntry::Record)
        return bad("unexpected entry in the index");
      V.clear();
      Expected<unsigned> Code = C.readRecord(R->ID, V);
      if (!Code)
        return Code.takeError();
      if (*Code != X_Body || V.size() != 3)
        return bad("bad index record");
      Out.push_back({static_cast<FuncId>(V[0]), V[1] | V[2] << 32});
    }
  }

  Type *type(Vals &R) {
    uint64_t Id = R.next();
    if (Id >= Types.size() || !Types[Id]) {
      R.Failed = true;
      return Ctx.unit();
    }
    return Types[Id];
  }

  Error typeRecord(unsigned Code, Vals &R) {
    Type *T = nullptr;
    switch (Code) {
    case T_Unit:
      T = Ctx.unit();
      break;
    case T_Bool:
      T = Ctx.boolean();
      break;
    case T_Int: {
      uint64_t Bits = R.next(), S = R.next();
      if (Bits != 8 && Bits != 16 && Bits != 32 && Bits != 64)
        return bad("integer width");
      T = Ctx.intTy(Bits, S);
      break;
    }
    case T_Float: {
      uint64_t Bits = R.next();
      if (Bits != 32 && Bits != 64)
        return bad("float width");
      T = Ctx.floatTy(Bits);
      break;
    }
    case T_String:
      T = Ctx.string();
      break;
    case T_Ptr:
      T = Ctx.ptr(type(R));
      break;
    case T_Slice:
      T = Ctx.slice(type(R));
      break;
    case T_Array: {
      Type *E = type(R);
      T = Ctx.array(E, R.next());
      break;
    }
    case T_Option:
      T = Ctx.option(type(R));
      break;
    case T_Tuple: {
      SmallVector<Type *, 4> Elems;
      while (R.Pos < R.V.size())
        Elems.push_back(type(R));
      T = Ctx.tuple(Elems);
      break;
    }
    case T_Func: {
      Type *Ret = type(R);
      bool Fails = R.next();
      SmallVector<Type *, 4> Params;
      while (R.Pos < R.V.size())
        Params.push_back(type(R));
      T = Ctx.func(Params, Ret, Fails);
      break;
    }
    case T_Struct:
      T = Ctx.structTy(R.rest());
      break;
    case T_Rec:
      T = Ctx.recTy(R.rest());
      break;
    case T_RecBody: {
      uint64_t Id = R.next(), Code = R.next(), Len = R.next(), N = R.next();
      if (Id >= Types.size() || !Types[Id]->isRec())
        return bad("self-naming body for another type");
      SmallVector<Type *, 4> Parts;
      for (uint64_t I = 0; I < N && !R.Failed; ++I)
        Parts.push_back(type(R));
      if (R.Failed || (Code != T_Tuple && Parts.size() != 1))
        return bad("self-naming body");
      Type *Body = nullptr;
      switch (Code) {
      case T_Ptr:
        Body = Ctx.ptr(Parts[0]);
        break;
      case T_Slice:
        Body = Ctx.slice(Parts[0]);
        break;
      case T_Array:
        Body = Ctx.array(Parts[0], Len);
        break;
      case T_Option:
        Body = Ctx.option(Parts[0]);
        break;
      case T_Tuple:
        Body = Ctx.tuple(Parts);
        break;
      default:
        return bad("self-naming body kind");
      }
      // The context may know the type already (a second read of the module).
      if (Types[Id]->hasBody()) {
        if (Types[Id]->getKind() != Body->getKind())
          return bad("self-naming type " + Types[Id]->name() +
                     " has two bodies");
        return Error::success();
      }
      if (!Ctx.setRecBody(Types[Id], Body))
        return bad("self-naming type " + Types[Id]->name() + " has no body");
      return Error::success();
    }
    case T_StructBody: {
      uint64_t Id = R.next(), N = R.next();
      if (Id >= Types.size() || Types[Id]->getKind() != Type::Kind::Struct)
        return bad("struct body for a non-struct");
      SmallVector<Type *, 4> Fields;
      for (uint64_t I = 0; I < N && !R.Failed; ++I)
        Fields.push_back(type(R));
      if (R.Failed)
        return bad("struct body");
      // A struct of this context may already have the same body.
      if (!Ctx.setStructBody(Types[Id], Fields) &&
          Types[Id]->elems() != ArrayRef<Type *>(Fields))
        return bad("struct " + Types[Id]->name() + " has two bodies");
      return Error::success();
    }
    default:
      return bad("unknown type record");
    }
    if (R.Failed)
      return bad("type record");
    Types.push_back(T);
    return Error::success();
  }

  Error tables(llvm::BitstreamCursor &C) {
    Mod = std::make_unique<Module>(Ctx);
    SmallVector<uint64_t, 16> V;
    while (true) {
      Expected<llvm::BitstreamEntry> En = C.advance();
      if (!En)
        return En.takeError();
      if (En->Kind == llvm::BitstreamEntry::EndBlock)
        return Error::success();
      if (En->Kind == llvm::BitstreamEntry::SubBlock) {
        if (En->ID == TypesBlock) {
          if (Error E = C.EnterSubBlock(TypesBlock))
            return E;
          while (true) {
            Expected<llvm::BitstreamEntry> R = C.advance();
            if (!R)
              return R.takeError();
            if (R->Kind == llvm::BitstreamEntry::EndBlock)
              break;
            if (R->Kind != llvm::BitstreamEntry::Record)
              return bad("unexpected entry in the type table");
            V.clear();
            Expected<unsigned> Code = C.readRecord(R->ID, V);
            if (!Code)
              return Code.takeError();
            Vals Rd{V};
            if (Error E = typeRecord(*Code, Rd))
              return E;
          }
          continue;
        }
        // The bodies and the index follow; they are read from the index.
        return Error::success();
      }
      if (En->Kind != llvm::BitstreamEntry::Record)
        return bad("unexpected entry in the module");
      V.clear();
      Expected<unsigned> Code = C.readRecord(En->ID, V);
      if (!Code)
        return Code.takeError();
      Vals R{V};
      switch (*Code) {
      case M_Name:
        Mod->Name = R.rest();
        break;
      case M_File:
        Mod->addFile(R.rest());
        break;
      case M_String:
        Mod->internString(R.rest());
        break;
      case M_Global: {
        Global G;
        G.Ty = type(R);
        G.Link = enumVal(R, Linkage::LinkOnceODR);
        G.ReadOnly = R.next();
        uint64_t CK = R.next();
        if (CK > Constant::Global)
          R.Failed = true;
        G.Init.K = static_cast<Constant::Kind>(CK);
        G.Init.Ty = type(R);
        G.Init.Bits = R.next();
        G.Loc = span(R);
        G.Name = R.rest();
        if (R.Failed)
          return bad("global declaration");
        if (G.Init.K == Constant::Zero)
          G.Init.Ty = nullptr;
        Mod->addGlobal(std::move(G));
        break;
      }
      case M_FuncDecl: {
        auto F = std::make_unique<Function>();
        F->Sig = type(R);
        F->Link = enumVal(R, Linkage::LinkOnceODR);
        F->CC = enumVal(R, CallConv::C);
        F->NoUnwind = R.next();
        F->NoReturn = R.next();
        F->Loc = span(R);
        F->Name = R.rest();
        if (R.Failed || F->Sig->getKind() != Type::Kind::Func)
          return bad("function declaration");
        Funcs.push_back(F.get());
        Mod->addFunction(std::move(F));
        break;
      }
      default:
        return bad("unknown module record");
      }
    }
  }

  Span span(Vals &R) {
    Span S;
    S.File = R.next();
    S.Line = R.next();
    S.Col = R.next();
    return S;
  }

  Place place(Vals &R, const Function &F) {
    Place P;
    P.Local = R.next();
    if (P.Local >= F.Locals.size()) {
      R.Failed = true;
      return P;
    }
    P.Ty = F.Locals[P.Local].Ty;
    uint64_t N = R.next();
    for (uint64_t I = 0; I < N && !R.Failed; ++I) {
      PlaceElem E;
      uint64_t K = R.next();
      if (K > PlaceElem::Downcast)
        R.Failed = true;
      E.K = static_cast<PlaceElem::Kind>(K);
      E.Val = R.next();
      E.Ty = type(R);
      P.Proj.push_back(E);
      P.Ty = E.Ty;
    }
    return P;
  }

  Operand operand(Vals &R, const Function &F) {
    Operand O;
    uint64_t K = R.next();
    if (K > Operand::Const) {
      R.Failed = true;
      return O;
    }
    O.K = static_cast<Operand::Kind>(K);
    if (O.K == Operand::Const) {
      uint64_t CK = R.next();
      if (CK > Constant::Global)
        R.Failed = true;
      O.C.K = static_cast<Constant::Kind>(CK);
      O.C.Ty = type(R);
      O.C.Bits = R.next();
    } else {
      O.P = place(R, F);
    }
    return O;
  }

  template <class E> E enumVal(Vals &R, E Last) {
    uint64_t V = R.next();
    if (V > static_cast<uint64_t>(Last)) {
      R.Failed = true;
      return Last;
    }
    return static_cast<E>(V);
  }

  Rvalue rvalue(Vals &R, const Function &F) {
    Rvalue RV;
    uint64_t K = R.next();
    if (K > Rvalue::Len) {
      R.Failed = true;
      return RV;
    }
    RV.K = static_cast<Rvalue::Kind>(K);
    RV.Ty = type(R);
    switch (RV.K) {
    case Rvalue::Use:
      RV.A = operand(R, F);
      break;
    case Rvalue::AddrOf:
      RV.Mut = R.next();
      RV.P = place(R, F);
      break;
    case Rvalue::Binary:
      RV.Bin = enumVal(R, BinOp::Ge);
      RV.A = operand(R, F);
      RV.B = operand(R, F);
      break;
    case Rvalue::Unary:
      RV.Un = enumVal(R, UnOp::Compl);
      RV.A = operand(R, F);
      break;
    case Rvalue::Cast:
      RV.CastOp = enumVal(R, CastKind::PtrToPtr);
      RV.A = operand(R, F);
      break;
    case Rvalue::Aggregate: {
      RV.Agg = enumVal(R, AggKind::Struct);
      uint64_t N = R.next();
      for (uint64_t I = 0; I < N && !R.Failed; ++I)
        RV.Ops.push_back(operand(R, F));
      break;
    }
    case Rvalue::Len:
      RV.P = place(R, F);
      break;
    }
    return RV;
  }

  UnwindAction unwind(Vals &R) {
    UnwindAction U;
    U.K = enumVal(R, static_cast<UnwindAction::Kind>(3));
    U.Target = R.next();
    return U;
  }

  Error functionBody(llvm::BitstreamCursor &C, FuncId Id) {
    Function *F = nullptr;
    SmallVector<uint64_t, 32> V;
    BasicBlock *Cur = nullptr;
    while (true) {
      Expected<llvm::BitstreamEntry> En = C.advance();
      if (!En)
        return En.takeError();
      if (En->Kind == llvm::BitstreamEntry::EndBlock)
        return F ? Error::success() : bad("empty function body");
      if (En->Kind != llvm::BitstreamEntry::Record)
        return bad("unexpected entry in a function body");
      V.clear();
      Expected<unsigned> Code = C.readRecord(En->ID, V);
      if (!Code)
        return Code.takeError();
      Vals R{V};
      if (*Code == F_Body) {
        if (F || R.next() != Id)
          return bad("function body names another function");
        F = Funcs[Id];
        if (!F->Locals.empty() || !F->Blocks.empty())
          return bad("two bodies for @" + F->Name);
        continue;
      }
      if (!F)
        return bad("function body without a header");
      switch (*Code) {
      case F_Local: {
        Local L;
        L.K = enumVal(R, Local::Temp);
        L.Ty = type(R);
        L.Mutable = R.next();
        L.Owned = R.next();
        L.Loc = span(R);
        L.Name = R.rest();
        F->Locals.push_back(std::move(L));
        break;
      }
      case F_Block:
        F->Blocks.emplace_back();
        Cur = &F->Blocks.back();
        Cur->Cleanup = R.next();
        Cur->Catch = R.next();
        break;
      case F_Stmt: {
        if (!Cur)
          return bad("statement outside a block");
        Statement S;
        S.K = enumVal(R, Statement::Nop);
        S.Loc = span(R);
        switch (S.K) {
        case Statement::Assign:
          S.Dest = place(R, *F);
          S.RV = rvalue(R, *F);
          break;
        case Statement::StorageLive:
        case Statement::StorageDead:
          S.L = R.next();
          break;
        case Statement::Nop:
          break;
        }
        Cur->Stmts.push_back(std::move(S));
        break;
      }
      case F_Term: {
        if (!Cur || Cur->Term.Set)
          return bad("terminator outside a block");
        Terminator &T = Cur->Term;
        T.K = enumVal(R, Terminator::Resume);
        T.Loc = span(R);
        T.Set = true;
        switch (T.K) {
        case Terminator::Goto:
          T.Target = R.next();
          break;
        case Terminator::Switch: {
          T.Discr = operand(R, *F);
          T.Otherwise = R.next();
          uint64_t N = R.next();
          for (uint64_t I = 0; I < N && !R.Failed; ++I) {
            SwitchCase SC;
            SC.Value = R.next();
            SC.Target = R.next();
            T.Cases.push_back(SC);
          }
          break;
        }
        case Terminator::Return:
        case Terminator::Unreachable:
        case Terminator::Resume:
          break;
        case Terminator::Call: {
          T.Discr = operand(R, *F);
          T.Dest = place(R, *F);
          T.Target = R.next();
          T.Unwind = unwind(R);
          uint64_t N = R.next();
          for (uint64_t I = 0; I < N && !R.Failed; ++I)
            T.Args.push_back(operand(R, *F));
          break;
        }
        case Terminator::Drop:
          T.Dest = place(R, *F);
          T.Target = R.next();
          T.Unwind = unwind(R);
          break;
        case Terminator::Assert:
          T.Discr = operand(R, *F);
          T.Expected = R.next();
          T.AK = enumVal(R, AssertKind::SliceConvert);
          T.Target = R.next();
          T.Unwind = unwind(R);
          for (uint64_t N = R.next(), I = 0; I < N && !R.Failed; ++I)
            T.Args.push_back(operand(R, *F));
          break;
        }
        break;
      }
      default:
        return bad("unknown function record");
      }
      if (R.Failed)
        return bad("record in the body of " + F->Name);
    }
  }
};

} // namespace

std::string gir::writeModule(const Module &M) { return Writer(M).run(); }

Expected<std::unique_ptr<Module>> gir::readModule(StringRef Bytes, Context &Ctx,
                                                  const ReadOptions &Opts) {
  return Reader(Bytes, Ctx, Opts).run();
}
