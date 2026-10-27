//===- SourceManagerTest.cpp - SourceManager tests ------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/SourceManager.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

#include <string>

using namespace gopp;

namespace {

FileID add(SourceManager &SM, llvm::StringRef Name, llvm::StringRef Text) {
  llvm::Expected<FileID> F = SM.addMemoryBuffer(Name, Text);
  EXPECT_TRUE(static_cast<bool>(F));
  if (!F) {
    llvm::consumeError(F.takeError());
    return {};
  }
  return *F;
}

TEST(SourceManager, InvalidLocationIsZero) {
  SourceLoc L;
  EXPECT_FALSE(L.isValid());
  SourceManager SM;
  EXPECT_FALSE(SM.getFileID(L).isValid());
  FileID F = add(SM, "a.gopp", "x");
  EXPECT_TRUE(SM.getStartLoc(F).isValid()); // the first file starts at 1
  EXPECT_EQ(SM.getStartLoc(F).getRaw(), 1u);
}

TEST(SourceManager, OffsetsLinesAndColumns) {
  SourceManager SM;
  FileID F = add(SM, "a.gopp", "ab\ncde\n\nf");
  auto At = [&](unsigned Off) { return SM.getLoc(F, Off); };
  EXPECT_EQ(SM.getFileOffset(At(4)), 4u);
  EXPECT_EQ(SM.getLineNumber(At(0)), 1u);
  EXPECT_EQ(SM.getColumnNumber(At(1)), 2u);
  EXPECT_EQ(SM.getLineNumber(At(2)), 1u); // the '\n' belongs to line 1
  EXPECT_EQ(SM.getLineNumber(At(3)), 2u);
  EXPECT_EQ(SM.getColumnNumber(At(3)), 1u);
  EXPECT_EQ(SM.getLineNumber(At(7)), 3u); // the empty line
  EXPECT_EQ(SM.getLineNumber(At(8)), 4u);
  EXPECT_EQ(SM.getNumLines(F), 4u);
  EXPECT_EQ(SM.getLineText(At(4)), "cde");
  EXPECT_EQ(SM.getLineText(At(7)), "");
  EXPECT_EQ(SM.getLineText(At(8)), "f");
  PresumedLoc P = SM.getPresumedLoc(At(5));
  EXPECT_EQ(P.Filename, "a.gopp");
  EXPECT_EQ(P.Line, 2u);
  EXPECT_EQ(P.Column, 3u);
}

TEST(SourceManager, EndOfFileLocationIsUsable) {
  SourceManager SM;
  FileID F = add(SM, "a.gopp", "abc");
  SourceLoc End = SM.getEndLoc(F);
  EXPECT_EQ(SM.getFileOffset(End), 3u);
  EXPECT_EQ(SM.getFileID(End), F);
  EXPECT_EQ(SM.getLineNumber(End), 1u);
  EXPECT_EQ(SM.getColumnNumber(End), 4u);
  // getLoc clamps.
  EXPECT_EQ(SM.getLoc(F, 100), End);
}

TEST(SourceManager, EmptyFile) {
  SourceManager SM;
  FileID E = add(SM, "empty.gopp", "");
  FileID G = add(SM, "g.gopp", "x");
  EXPECT_EQ(SM.getStartLoc(E), SM.getEndLoc(E));
  EXPECT_EQ(SM.getNumLines(E), 1u);
  EXPECT_EQ(SM.getFileID(SM.getEndLoc(E)), E);
  EXPECT_EQ(SM.getFileID(SM.getStartLoc(G)), G);
  EXPECT_NE(SM.getEndLoc(E), SM.getStartLoc(G)); // files never share an offset
  EXPECT_EQ(SM.getLineText(SM.getEndLoc(E)), "");
}

TEST(SourceManager, CRLFAndLoneCR) {
  SourceManager SM;
  FileID F = add(SM, "a.gopp", "a\r\nb\r\nc");
  EXPECT_EQ(SM.getNumLines(F), 3u);
  EXPECT_EQ(SM.getLineText(SM.getLoc(F, 3)), "b"); // no '\r' in the text
  EXPECT_EQ(SM.getLineNumber(SM.getLoc(F, 6)), 3u);
  // A lone CR is not a line break (Go's rule).
  FileID G = add(SM, "b.gopp", "a\rb\nc");
  EXPECT_EQ(SM.getNumLines(G), 2u);
}

TEST(SourceManager, FinalNewlineDoesNotAddALine) {
  SourceManager SM;
  FileID F = add(SM, "a.gopp", "a\nb\n");
  // The position after the last '\n' is on line 3 (an empty line).
  EXPECT_EQ(SM.getLineNumber(SM.getEndLoc(F)), 3u);
  EXPECT_EQ(SM.getNumLines(F), 3u);
}

TEST(SourceManager, MultipleFilesHaveDisjointRanges) {
  SourceManager SM;
  FileID A = add(SM, "a.gopp", "aaaa");
  FileID B = add(SM, "b.gopp", "bb\nbb");
  FileID C = add(SM, "c.gopp", "c");
  EXPECT_EQ(SM.getNumFiles(), 3u);
  EXPECT_EQ(SM.getFilename(B), "b.gopp");
  EXPECT_LT(SM.getEndLoc(A), SM.getStartLoc(B));
  EXPECT_LT(SM.getEndLoc(B), SM.getStartLoc(C));
  for (unsigned I = 0; I <= 4; ++I)
    EXPECT_EQ(SM.getFileID(SM.getLoc(A, I)), A);
  for (unsigned I = 0; I <= 5; ++I)
    EXPECT_EQ(SM.getFileID(SM.getLoc(B, I)), B);
  EXPECT_EQ(SM.getFileID(SM.getLoc(C, 1)), C);
  EXPECT_EQ(SM.getLineNumber(SM.getLoc(B, 4)), 2u);
  EXPECT_EQ(SM.getFileOffset(SM.getLoc(B, 4)), 4u);
  // A location past every file is invalid.
  EXPECT_FALSE(SM.getFileID(SM.getEndLoc(C).getLocWithOffset(5)).isValid());
}

TEST(SourceManager, SnapshotsHaveSeparateOffsetSpaces) {
  SourceManager S1, S2;
  FileID A1 = add(S1, "a.gopp", "package p\n");
  FileID A2 = add(S2, "a.gopp", "package q\nvar x int\n");
  // Both start at the same raw offset: a loc is only valid with its own
  // manager.
  EXPECT_EQ(S1.getStartLoc(A1).getRaw(), S2.getStartLoc(A2).getRaw());
  EXPECT_EQ(S1.getLineText(S1.getStartLoc(A1)), "package p");
  EXPECT_EQ(S2.getLineText(S2.getStartLoc(A2)), "package q");
  // Adding to one does not move the other.
  uint64_t Used2 = S2.getUsedOffsets();
  add(S1, "b.gopp", "more");
  EXPECT_EQ(S2.getUsedOffsets(), Used2);
}

TEST(SourceManager, LinesAreBuiltLazilyAndRepeatably) {
  SourceManager SM;
  std::string Big;
  for (int I = 0; I < 5000; ++I)
    Big += "line " + std::to_string(I) + "\n";
  FileID F = add(SM, "big.gopp", Big);
  EXPECT_EQ(SM.getNumLines(F), 5001u);
  EXPECT_EQ(SM.getLineText(SM.getLoc(F, 0)), "line 0");
  size_t Off = Big.find("line 4999");
  EXPECT_EQ(SM.getLineNumber(SM.getLoc(F, Off)), 5000u);
  EXPECT_EQ(SM.getLineNumber(SM.getLoc(F, Off)), 5000u); // second query
}

TEST(SourceManager, OffsetSpaceExhaustionIsAnError) {
  SourceManager SM(/*MaxOffset=*/20);
  llvm::Expected<FileID> A = SM.addMemoryBuffer("a", std::string(10, 'x'));
  ASSERT_TRUE(static_cast<bool>(A));
  llvm::Expected<FileID> B = SM.addMemoryBuffer("b", std::string(10, 'x'));
  ASSERT_FALSE(static_cast<bool>(B));
  std::string Msg = llvm::toString(B.takeError());
  EXPECT_NE(Msg.find("b"), std::string::npos);
  // A failed add leaves the manager unchanged and usable.
  EXPECT_EQ(SM.getNumFiles(), 1u);
  llvm::Expected<FileID> C = SM.addMemoryBuffer("c", "x");
  EXPECT_TRUE(static_cast<bool>(C));
  if (!C)
    llvm::consumeError(C.takeError());
}

TEST(SourceManager, ExactlyFittingFileIsAccepted) {
  // A file of N bytes takes N+1 slots (the EOF position).
  SourceManager SM(/*MaxOffset=*/11);
  llvm::Expected<FileID> A = SM.addMemoryBuffer("a", std::string(10, 'x'));
  EXPECT_TRUE(static_cast<bool>(A));
  if (!A)
    llvm::consumeError(A.takeError());
}

TEST(SourceManager, FileOfHugeSizeIsRejectedWithoutOverflow) {
  SourceManager SM(/*MaxOffset=*/UINT32_MAX);
  // Fill most of the space with one real file would need 4 GiB; instead use a
  // tiny space to check that wraparound cannot happen.
  SourceManager Small(/*MaxOffset=*/5);
  llvm::Expected<FileID> A = Small.addMemoryBuffer("a", std::string(100, 'x'));
  EXPECT_FALSE(static_cast<bool>(A));
  if (!A)
    llvm::consumeError(A.takeError());
}

TEST(SourceManager, ReadsARealFile) {
  llvm::SmallString<128> Path;
  int FD;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("gopp-sm", "gopp", FD, Path));
  {
    llvm::raw_fd_ostream OS(FD, /*shouldClose=*/true);
    OS << "package p\n\nvar x = 1\n";
  }
  SourceManager SM;
  llvm::Expected<FileID> F = SM.addFile(Path);
  ASSERT_TRUE(static_cast<bool>(F));
  EXPECT_EQ(SM.getNumLines(*F), 4u);
  EXPECT_EQ(SM.getLineText(SM.getLoc(*F, 11)), "var x = 1");
  EXPECT_EQ(SM.getFilename(*F), Path.str());
  llvm::sys::fs::remove(Path);
}

TEST(SourceManager, MissingFileIsAnError) {
  SourceManager SM;
  llvm::Expected<FileID> F = SM.addFile("/nonexistent/dir/none.gopp");
  ASSERT_FALSE(static_cast<bool>(F));
  std::string Msg = llvm::toString(F.takeError());
  EXPECT_NE(Msg.find("none.gopp"), std::string::npos);
  EXPECT_EQ(SM.getNumFiles(), 0u);
}

TEST(SourceManager, ArbitraryBytesAreKept) {
  SourceManager SM;
  std::string Text("a\0b\xff\xfe\n\xef\xbb\xbfz", 10);
  FileID F = add(SM, "bin", Text);
  EXPECT_EQ(SM.getBuffer(F).size(), Text.size());
  EXPECT_EQ(SM.getNumLines(F), 2u);
  EXPECT_EQ(SM.getColumnNumber(SM.getLoc(F, 9)), 4u); // byte columns
}

} // namespace
