// The start and the end of a program, the names of its functions, and
// `min` and `max` of two floats.

#include "internal.h"

[[noreturn]] void exit(int status);

static gopp_func_table *tables;

void _gopp_rt_init(int argc, char **argv) {
  // Nothing reads the arguments yet.
  (void)argc;
  (void)argv;
  // A process may run one program after another: the tables of the one
  // before are gone with its code.
  tables = 0;
  _gopp_iface_reset();
  _gopp_panic_reset();
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

// `min` and `max` of two floats as Go has them: what is no number wins,
// and the zero with the sign is below the zero without.
double _gopp_float_min(double a, double b) {
  if (a != a || b != b) {
    return a != a ? a : b;
  }
  if (a == b) {
    return __builtin_signbit(a) ? a : b;
  }
  return a < b ? a : b;
}

double _gopp_float_max(double a, double b) {
  if (a != a || b != b) {
    return a != a ? a : b;
  }
  if (a == b) {
    return __builtin_signbit(a) ? b : a;
  }
  return a > b ? a : b;
}
