//===- gopp/Sema/Serialize.h - Type and object serialization ----*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// D3.8. Types and package-level objects serialize through two small
// interfaces, so that export data (P06 `.gopx`) can load dependencies without
// their sources, and so that GIR bodies (P04, D4.15) can name types by id:
//
//   RecordWriter / RecordReader  a stream of records: a code, unsigned
//                                operands and an optional blob. That is the
//                                shape of an LLVM bitstream record (abbrev id,
//                                operands, blob), so P06 implements the two
//                                interfaces on llvm::BitstreamWriter/Reader
//                                without changing the encoder.
//   TypeWriter / TypeReader      turn a Type, an Object or a whole Package
//                                into records and back. A type becomes a
//                                small integer id the first time it is
//                                written; later records (GIR instructions,
//                                other objects) refer to it by id.
//
// The encoding is deterministic (ids follow a depth-first walk in a fixed
// order, objects are sorted by name; no pointer order, no hash order), as the
// cache of P06 hashes the bytes. A reader needs the TypeContext of the
// compilation (types are uniqued there, so a decoded `[]int` or `List[int]`
// is the same pointer as one the checker makes) and a PackageResolver for the
// packages the types come from.
//
// MemoryRecordStream is the simple encoder/decoder the tests use; its byte
// form is a varint stream.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_SEMA_SERIALIZE_H
#define GOPP_SEMA_SERIALIZE_H

#include "gopp/Sema/Types.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"

#include <deque>
#include <string>
#include <vector>

namespace gopp {
namespace sema {

struct Record {
  unsigned Code = 0;
  std::vector<uint64_t> Ops;
  std::string Blob;
};

class RecordWriter {
public:
  virtual ~RecordWriter() = default;
  virtual void write(unsigned Code, llvm::ArrayRef<uint64_t> Ops,
                     llvm::StringRef Blob = {}) = 0;
};

class RecordReader {
public:
  virtual ~RecordReader() = default;
  /// The next record; false at the end of the stream.
  virtual bool read(Record &R) = 0;
};

/// Records in memory, with a byte encoding (LEB128 varints).
class MemoryRecordStream : public RecordWriter, public RecordReader {
public:
  void write(unsigned Code, llvm::ArrayRef<uint64_t> Ops,
             llvm::StringRef Blob = {}) override;
  bool read(Record &R) override;
  void rewind() { Pos = 0; }
  const std::vector<Record> &records() const { return Recs; }
  std::string toBytes() const;
  /// False if Bytes is not a valid encoding.
  bool fromBytes(llvm::StringRef Bytes);

private:
  std::vector<Record> Recs;
  size_t Pos = 0;
};

/// The record codes. Operands are listed in Serialize.cpp.
enum RecordCode : unsigned {
  REC_STRING = 1,
  REC_PKGREF,
  REC_BASIC,
  REC_UNIVERSE,
  REC_POINTER,
  REC_SLICE,
  REC_REF,
  REC_ARRAY,
  REC_MAP,
  REC_CHAN,
  REC_TUPLE,
  REC_SIGNATURE,
  REC_STRUCT,
  REC_INTERFACE,
  REC_UNION,
  REC_NAMED_DECL,
  REC_NAMED_DEF,
  REC_NAMED_INST,
  REC_TPARAM_DECL,
  REC_TPARAM_DEF,
  REC_PKG_BEGIN,
  REC_OBJ_CONST,
  REC_OBJ_TYPE,
  REC_OBJ_VAR,
  REC_OBJ_FUNC,
  REC_PKG_END,
  /// `Option[T]`: one operand, the type id of T.
  REC_OPTION,
  /// A unit (ADR-0025): the dimensions (unit or unit parameter, exponent) and
  /// the exact factor. Gets the next unit id.
  REC_UNIT_CANON,
  /// A numeric type with a unit: the base kind and the unit id (the kind
  /// UnitArg is a unit used as a type argument).
  REC_UNIT_TYPE,
  /// A declared unit of the package: name, base?, symbol, unit id.
  REC_OBJ_UNIT,
  /// The display form the package prints for a unit (UNI-5): the unit id and
  /// the parts (unit by package and name, or unit parameter; exponent).
  REC_UNIT_NAME,
  /// Codes from here on belong to the user of the stream (GIR bodies).
  REC_FIRST_USER = 64,
};

class TypeWriter {
public:
  explicit TypeWriter(RecordWriter &W) : W(W) {}
  /// The id of T (0 for null), writing the records T needs first.
  uint64_t type(const Type *T);
  /// The id of a string (0 is the empty string).
  uint64_t string(llvm::StringRef S);
  /// The id of a package reference.
  uint64_t package(const Package *P);
  /// Writes one package: a begin record, the exported objects sorted by name
  /// (and the types they need), an end record. OnlyExported = false writes
  /// every package-level object (tests and tools).
  void writePackage(const Package &P, bool OnlyExported = true);

private:
  uint64_t typeImpl(const Type *T);
  void object(const Object *O);
  void constValue(const ConstValue &V, std::vector<uint64_t> &Ops);
  void declareTypeParam(const TypeParamType *T);
  uint64_t unit(const UnitCanon *U);

  RecordWriter &W;
  llvm::DenseMap<const UnitCanon *, uint64_t> UnitIds;
  uint64_t NextUnit = 1;
  llvm::StringMap<uint64_t> Strings;
  llvm::DenseMap<const Package *, uint64_t> Pkgs;
  llvm::DenseMap<const Type *, uint64_t> Ids;
  uint64_t NextType = 1;
  /// Nesting of type() calls; type parameters declared inside get their
  /// bound (TPARAM_DEF) when the outermost call ends.
  unsigned Depth = 0;
  std::deque<const TypeParamType *> PendingTParams;
};

/// Finds the package a type of the stream comes from, other than the one
/// being read.
class PackageResolver {
public:
  virtual ~PackageResolver() = default;
  virtual Package *resolve(llvm::StringRef Path) = 0;
};

class TypeReader {
public:
  TypeReader(RecordReader &R, TypeContext &Ctx, Arena &Mem, PackageResolver &PR)
      : R(R), Ctx(Ctx), Mem(Mem), PR(PR) {}
  /// Reads one package written by TypeWriter::writePackage. Null with Error
  /// set if the stream is malformed: the reader checks every record, so a
  /// corrupt stream never crashes it, never changes a type it did not make
  /// (the universe's, another package's) and never makes a type deeper than
  /// MaxTypeDepth (P03.11).
  Package *readPackage(std::string &Error);
  static constexpr unsigned MaxTypeDepth = 512;
  /// The type of an id read so far (null for 0 or an unknown id).
  Type *type(uint64_t Id) const;

private:
  bool fail(const char *Msg);
  bool record(const Record &Rec);
  bool constValue(const std::vector<uint64_t> &Ops, size_t &I, ConstValue &V);
  const std::string *str(uint64_t Id) const;

  RecordReader &R;
  TypeContext &Ctx;
  Arena &Mem;
  PackageResolver &PR;
  std::string Error;
  Package *Pkg = nullptr;
  std::vector<std::string> Strings{std::string()};
  std::vector<Package *> Pkgs{nullptr};
  std::vector<Type *> Types{nullptr};
  /// The units read so far (id 0 is none), and the units of this package that
  /// a unit refers to before their own declaration has come.
  std::vector<const UnitCanon *> Canons{nullptr};
  llvm::StringMap<UnitObj *> PendingUnits;
  /// The display table of the package (REC_UNIT_NAME), in record order.
  std::vector<UnitNameEntry> UnitNamesRead;
  /// Depths[Id]: how deep the type Types[Id] nests.
  std::vector<unsigned> Depths{0};
  /// Named types and type parameters made by a DECL record whose DEF record
  /// has not come yet: only they can be defined, once.
  llvm::SmallPtrSet<const Type *, 16> Undefined;
};

/// Prints the declarations of a package's exported API (OnlyExported =
/// false: of every package-level object), one per line, in a canonical form:
/// tests compare a package with its decoded copy by this text.
void dumpPackageAPI(llvm::raw_ostream &OS, const Package &P,
                    bool OnlyExported = true);

} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_SERIALIZE_H
