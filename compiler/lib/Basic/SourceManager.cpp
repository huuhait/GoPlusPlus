//===- SourceManager.cpp - Source files and locations ---------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/SourceManager.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/ErrorOr.h"

#include <algorithm>
#include <cassert>
#include <cstring>

using namespace gopp;

llvm::Expected<FileID> SourceManager::addFile(llvm::StringRef Path) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buf =
      llvm::MemoryBuffer::getFile(Path, /*IsText=*/false,
                                  /*RequiresNullTerminator=*/false);
  if (!Buf)
    return llvm::createStringError(Buf.getError(), "cannot read '%s': %s",
                                   Path.str().c_str(),
                                   Buf.getError().message().c_str());
  return addBuffer(std::move(*Buf));
}

llvm::Expected<FileID>
SourceManager::addMemoryBuffer(llvm::StringRef Name, llvm::StringRef Contents) {
  return addBuffer(llvm::MemoryBuffer::getMemBufferCopy(Contents, Name));
}

llvm::Expected<FileID>
SourceManager::addBuffer(std::unique_ptr<llvm::MemoryBuffer> Buf) {
  uint64_t Size = Buf->getBufferSize();
  // The file takes Size + 1 offsets: one per byte and one for the EOF position.
  if (NextBase + Size > MaxOffset)
    return llvm::createStringError(
        llvm::errc::file_too_large,
        "'%s': source files exceed the 4 GiB offset space",
        Buf->getBufferIdentifier().str().c_str());
  FileEntry E;
  E.Name = Buf->getBufferIdentifier().str();
  E.Base = static_cast<uint32_t>(NextBase);
  E.Buf = std::move(Buf);
  NextBase += Size + 1;
  Files.push_back(std::move(E));
  return FileID::fromRaw(Files.size());
}

const SourceManager::FileEntry &SourceManager::entry(FileID F) const {
  assert(F.isValid() && F.getRaw() <= Files.size() && "bad FileID");
  return Files[F.getRaw() - 1];
}

llvm::StringRef SourceManager::getBuffer(FileID F) const {
  return entry(F).Buf->getBuffer();
}

llvm::StringRef SourceManager::getFilename(FileID F) const {
  return entry(F).Name;
}

SourceLoc SourceManager::getStartLoc(FileID F) const {
  return SourceLoc::fromRaw(entry(F).Base);
}

SourceLoc SourceManager::getEndLoc(FileID F) const {
  const FileEntry &E = entry(F);
  return SourceLoc::fromRaw(E.Base + E.size());
}

SourceLoc SourceManager::getLoc(FileID F, uint32_t Offset) const {
  const FileEntry &E = entry(F);
  return SourceLoc::fromRaw(E.Base + std::min(Offset, E.size()));
}

const SourceManager::FileEntry *SourceManager::lookup(SourceLoc Loc) const {
  if (!Loc.isValid() || Files.empty())
    return nullptr;
  // The last file whose Base <= Loc.
  auto It = std::upper_bound(
      Files.begin(), Files.end(), Loc.getRaw(),
      [](uint32_t L, const FileEntry &E) { return L < E.Base; });
  if (It == Files.begin())
    return nullptr;
  --It;
  if (Loc.getRaw() - It->Base > It->size())
    return nullptr;
  return &*It;
}

FileID SourceManager::getFileID(SourceLoc Loc) const {
  const FileEntry *E = lookup(Loc);
  return E ? FileID::fromRaw(E - Files.data() + 1) : FileID();
}

uint32_t SourceManager::getFileOffset(SourceLoc Loc) const {
  const FileEntry *E = lookup(Loc);
  assert(E && "location does not belong to this SourceManager");
  return E ? Loc.getRaw() - E->Base : 0;
}

void SourceManager::buildLines(const FileEntry &E) const {
  if (E.LinesBuilt)
    return;
  E.LineStarts.clear();
  E.LineStarts.push_back(0);
  llvm::StringRef Data = E.Buf->getBuffer();
  const char *Begin = Data.data();
  const char *P = Begin, *End = Begin + Data.size();
  while (P != End) {
    const void *NL = std::memchr(P, '\n', End - P);
    if (!NL)
      break;
    P = static_cast<const char *>(NL) + 1;
    E.LineStarts.push_back(static_cast<uint32_t>(P - Begin));
  }
  E.LinesBuilt = true;
}

PresumedLoc SourceManager::getPresumedLoc(SourceLoc Loc) const {
  const FileEntry *E = lookup(Loc);
  if (!E)
    return {};
  buildLines(*E);
  uint32_t Off = Loc.getRaw() - E->Base;
  auto It = std::upper_bound(E->LineStarts.begin(), E->LineStarts.end(), Off);
  unsigned Line = It - E->LineStarts.begin();
  PresumedLoc P;
  P.Filename = E->Name;
  P.Line = Line;
  P.Column = Off - E->LineStarts[Line - 1] + 1;
  return P;
}

unsigned SourceManager::getLineNumber(SourceLoc Loc) const {
  return getPresumedLoc(Loc).Line;
}

unsigned SourceManager::getColumnNumber(SourceLoc Loc) const {
  return getPresumedLoc(Loc).Column;
}

llvm::StringRef SourceManager::getLineText(SourceLoc Loc) const {
  const FileEntry *E = lookup(Loc);
  if (!E)
    return {};
  buildLines(*E);
  uint32_t Off = Loc.getRaw() - E->Base;
  auto It = std::upper_bound(E->LineStarts.begin(), E->LineStarts.end(), Off);
  size_t Idx = It - E->LineStarts.begin() - 1;
  llvm::StringRef Data = E->Buf->getBuffer();
  uint32_t Begin = E->LineStarts[Idx];
  uint32_t End =
      Idx + 1 < E->LineStarts.size() ? E->LineStarts[Idx + 1] - 1 : Data.size();
  llvm::StringRef Line = Data.slice(Begin, End);
  // The CR of a CRLF terminator is not part of the line.
  if (Idx + 1 < E->LineStarts.size() && Line.ends_with("\r"))
    Line = Line.drop_back();
  return Line;
}

unsigned SourceManager::getNumLines(FileID F) const {
  const FileEntry &E = entry(F);
  buildLines(E);
  return E.LineStarts.size();
}
