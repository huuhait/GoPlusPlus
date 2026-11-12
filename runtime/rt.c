// The start and the end of a program, and the names of its functions.

#include "internal.h"

[[noreturn]] void exit(int status);

static gopp_func_table *tables;

void _gopp_rt_init(int argc, char **argv) {
  // Nothing reads the arguments yet.
  (void)argc;
  (void)argv;
  _gopp_os_init();
}

void _gopp_rt_exit(int code) {
  // `exit` writes out what the C library still holds.
  exit(code);
}

void _gopp_rt_funcs(gopp_func_table *table) {
  table->next = tables;
  tables = table;
}

// Only a failure asks, so the tables are searched as they are.
const char *_gopp_func_name(uintptr_t start) {
  for (const gopp_func_table *table = tables; table; table = table->next) {
    for (uint64_t at = 0; at < table->count; at++) {
      if ((uintptr_t)table->funcs[at].pc == start) {
        return table->funcs[at].name;
      }
    }
  }
  return 0;
}
