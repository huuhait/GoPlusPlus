//===- Lines.h - Lines over a file descriptor (private) -------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_LIB_TESTRUN_LINES_H
#define GOPP_LIB_TESTRUN_LINES_H

#include <cerrno>
#include <cstddef>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace gopp::testrun {

/// Writes all of S and a newline; false if the other end is gone.
inline bool writeLine(int Fd, const std::string &S) {
  std::string Buf = S;
  Buf += '\n';
  size_t Done = 0;
  while (Done < Buf.size()) {
    ssize_t N = ::send(Fd, Buf.data() + Done, Buf.size() - Done, MSG_NOSIGNAL);
    if (N < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    Done += size_t(N);
  }
  return true;
}

/// Complete lines out of the bytes that arrive on a descriptor.
class LineBuffer {
  std::string Pending;

public:
  /// Reads once from Fd (call when it is readable). Returns false at the end
  /// of the stream or on an error.
  bool fill(int Fd) {
    char Chunk[4096];
    for (;;) {
      ssize_t N = ::read(Fd, Chunk, sizeof Chunk);
      if (N < 0 && errno == EINTR)
        continue;
      if (N <= 0)
        return false;
      Pending.append(Chunk, size_t(N));
      return true;
    }
  }
  /// The next complete line (without its newline), if there is one.
  bool next(std::string &Line) {
    size_t NL = Pending.find('\n');
    if (NL == std::string::npos)
      return false;
    Line.assign(Pending, 0, NL);
    Pending.erase(0, NL + 1);
    return true;
  }
};

} // namespace gopp::testrun

#endif // GOPP_LIB_TESTRUN_LINES_H
