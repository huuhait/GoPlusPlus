//===- gopp/Basic/SourceManager.h - Source files and locations --*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Clang-style source locations (D2.4). Every file added to a SourceManager gets
// a slice of one 32-bit offset space, so a SourceLoc is 4 bytes. Offset 0 is
// the invalid location. Line tables are built lazily on the first query.
//
// An LSP snapshot owns its own SourceManager, so snapshots have separate offset
// spaces (P14 D14.3). A SourceManager is not thread safe; a SourceLoc is only
// meaningful with the SourceManager that made it.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_BASIC_SOURCEMANAGER_H
#define GOPP_BASIC_SOURCEMANAGER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gopp {

/// A position in the global offset space of one SourceManager.
class SourceLoc {
  uint32_t Offset = 0;

public:
  SourceLoc() = default;
  static SourceLoc fromRaw(uint32_t Raw) {
    SourceLoc L;
    L.Offset = Raw;
    return L;
  }
  bool isValid() const { return Offset != 0; }
  uint32_t getRaw() const { return Offset; }
  SourceLoc getLocWithOffset(int64_t Delta) const {
    return fromRaw(static_cast<uint32_t>(Offset + Delta));
  }
  friend bool operator==(SourceLoc A, SourceLoc B) {
    return A.Offset == B.Offset;
  }
  friend bool operator!=(SourceLoc A, SourceLoc B) {
    return A.Offset != B.Offset;
  }
  friend bool operator<(SourceLoc A, SourceLoc B) {
    return A.Offset < B.Offset;
  }
  friend bool operator<=(SourceLoc A, SourceLoc B) {
    return A.Offset <= B.Offset;
  }
};

/// A half-open range [Begin, End).
class SourceRange {
  SourceLoc B, E;

public:
  SourceRange() = default;
  SourceRange(SourceLoc Begin, SourceLoc End) : B(Begin), E(End) {}
  explicit SourceRange(SourceLoc L) : B(L), E(L) {}
  SourceLoc getBegin() const { return B; }
  SourceLoc getEnd() const { return E; }
  bool isValid() const { return B.isValid() && E.isValid(); }
  friend bool operator==(SourceRange A, SourceRange B) {
    return A.B == B.B && A.E == B.E;
  }
};

/// Identifies a file inside one SourceManager. 0 is invalid.
class FileID {
  uint32_t ID = 0;

public:
  FileID() = default;
  static FileID fromRaw(uint32_t Raw) {
    FileID F;
    F.ID = Raw;
    return F;
  }
  bool isValid() const { return ID != 0; }
  uint32_t getRaw() const { return ID; }
  friend bool operator==(FileID A, FileID B) { return A.ID == B.ID; }
  friend bool operator!=(FileID A, FileID B) { return A.ID != B.ID; }
};

/// A location decoded for humans: 1-based line and 1-based byte column.
struct PresumedLoc {
  llvm::StringRef Filename;
  unsigned Line = 0;
  unsigned Column = 0;
  bool isValid() const { return Line != 0; }
};

class SourceManager {
public:
  /// MaxOffset is the largest offset the space may reach; the default is the
  /// whole 32-bit range. A smaller value exists so tests can reach the limit.
  explicit SourceManager(uint64_t MaxOffset = UINT32_MAX)
      : MaxOffset(MaxOffset) {}
  SourceManager(const SourceManager &) = delete;
  SourceManager &operator=(const SourceManager &) = delete;

  /// Maps the file (llvm::MemoryBuffer mmaps large files). Fails if the file
  /// cannot be read or the offset space is exhausted.
  llvm::Expected<FileID> addFile(llvm::StringRef Path);
  /// Copies the contents.
  llvm::Expected<FileID> addMemoryBuffer(llvm::StringRef Name,
                                         llvm::StringRef Contents);
  llvm::Expected<FileID> addBuffer(std::unique_ptr<llvm::MemoryBuffer> Buf);

  unsigned getNumFiles() const { return Files.size(); }

  llvm::StringRef getBuffer(FileID F) const;
  llvm::StringRef getFilename(FileID F) const;
  /// The location of the first byte of the file.
  SourceLoc getStartLoc(FileID F) const;
  /// The location one past the last byte (the EOF position).
  SourceLoc getEndLoc(FileID F) const;
  /// The location of byte Offset of the file. Offset is clamped to the size.
  SourceLoc getLoc(FileID F, uint32_t Offset) const;

  /// The file that contains Loc, or an invalid FileID.
  FileID getFileID(SourceLoc Loc) const;
  /// Byte offset of Loc inside its file. Loc must be valid.
  uint32_t getFileOffset(SourceLoc Loc) const;

  /// 1-based line and byte column. Builds the line table of the file once.
  PresumedLoc getPresumedLoc(SourceLoc Loc) const;
  unsigned getLineNumber(SourceLoc Loc) const;
  unsigned getColumnNumber(SourceLoc Loc) const;
  /// The text of the line that contains Loc, without the line terminator.
  llvm::StringRef getLineText(SourceLoc Loc) const;
  /// Number of lines of the file (builds the line table).
  unsigned getNumLines(FileID F) const;

  /// Total size of the offset space in use.
  uint64_t getUsedOffsets() const { return NextBase; }

private:
  struct FileEntry {
    std::unique_ptr<llvm::MemoryBuffer> Buf;
    std::string Name;
    uint32_t Base = 0; // global offset of byte 0
    // Offsets of line starts; built on demand.
    mutable std::vector<uint32_t> LineStarts;
    mutable bool LinesBuilt = false;
    uint32_t size() const { return Buf->getBufferSize(); }
  };

  const FileEntry &entry(FileID F) const;
  const FileEntry *lookup(SourceLoc Loc) const;
  void buildLines(const FileEntry &E) const;

  std::vector<FileEntry> Files;
  uint64_t NextBase = 1; // offset 0 is reserved for the invalid location
  uint64_t MaxOffset;
};

} // namespace gopp

#endif // GOPP_BASIC_SOURCEMANAGER_H
