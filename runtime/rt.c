// The start and the end of a program.

#include "gopp_rt.h"

[[noreturn]] void exit(int status);

void _gopp_rt_init(int argc, char **argv) {
  // Nothing reads the arguments yet.
  (void)argc;
  (void)argv;
}

void _gopp_rt_exit(int code) {
  // `exit` writes out what the C library still holds.
  exit(code);
}
