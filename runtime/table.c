// Type tables (TYP-22, EXP-15). A value of a type with methods that may
// be replaced starts with the address of a table, and so does the part
// of each of its parents that has a table of its own. A table says how
// far its part is from the start of the whole value and names the
// record of the type of that value, which lists the parts that a
// pointer to the value may be turned into, after the value itself.

#include "internal.h"

typedef struct {
  // The record of the type of the part, and where the part starts.
  const void *of;
  int64_t offset;
} gopp_part;

typedef struct {
  gopp_string name;
  uint64_t count;
  gopp_part parts[];
} gopp_whole;

typedef struct {
  int64_t to_start;
  const gopp_whole *whole;
} gopp_table;

static const gopp_table *table_of(const uint8_t *part) {
  return *(const gopp_table *const *)part;
}

// `p.(*T)` (EXP-15): the part of the type with the record `of` in the
// value that `part` is a part of, null if the value has none.
uint8_t *_gopp_part_of(uint8_t *part, const void *of) {
  const gopp_table *table = table_of(part);
  uint8_t *start = part + table->to_start;
  const gopp_whole *whole = table->whole;
  if (whole == of) {
    return start;
  }
  for (uint64_t at = 0; at < whole->count; at++) {
    if (whole->parts[at].of == of) {
      return start + whole->parts[at].offset;
    }
  }
  return 0;
}

// `p.(*T)` for a pointer to a value that is no `T`: `from` is the type
// of `p` as the program writes it, `of` the record of `T`.
void _gopp_panic_part(const uint8_t *part, const void *of, gopp_string from) {
  const gopp_whole *wanted = of;
  _gopp_fail_part(from, table_of(part)->whole->name, wanted->name);
}
