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
  // What a value of the type takes, and what ends it and makes it a
  // copy of its own: null for a value that owns nothing.
  uint64_t size;
  void (*drop)(uint8_t *value);
  void (*clone)(uint8_t *value);
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

void free(void *room);

// The start of the value that `part` is a part of. A part without the
// address of a table is a whole value of the type that the code names:
// nothing made a pointer to a part of it.
static uint8_t *start_of(uint8_t *part) {
  const gopp_table *table = table_of(part);
  return table == 0 ? part : part + table->to_start;
}

// The value that a `Unique` of a parent owns ends, with its box: all of
// it, by the code of its own type. False for a value without the
// address of a table, which the caller ends as the type that it names.
bool _gopp_whole_end(uint8_t *part) {
  const gopp_table *table = table_of(part);
  if (table == 0) {
    return true;
  }
  uint8_t *start = part + table->to_start;
  if (table->whole->drop != 0) {
    table->whole->drop(start);
  }
  free(start);
  return false;
}

// A box of its own with a copy of the whole value that `part` is a part
// of: the address of the same part in the copy. Null for a value
// without the address of a table, which the caller copies itself.
uint8_t *_gopp_whole_copy(uint8_t *part) {
  const gopp_table *table = table_of(part);
  if (table == 0) {
    return 0;
  }
  uint8_t *start = part + table->to_start;
  uint8_t *copy = _gopp_iface_box(start, table->whole->size);
  if (table->whole->clone != 0) {
    table->whole->clone(copy);
  }
  return copy + (part - start);
}

// The counts of a `Shared` or a `Weak` of a parent are those of the
// whole value (OWN-P8).
void _gopp_part_retain(uint8_t *part) { _gopp_shared_retain(start_of(part)); }

void _gopp_part_weak_retain(uint8_t *part) { _gopp_weak_retain(start_of(part)); }

void _gopp_part_weak_release(uint8_t *part) { _gopp_weak_release(start_of(part)); }

uint8_t *_gopp_part_lock(uint8_t *part) {
  return _gopp_weak_lock(start_of(part)) == 0 ? 0 : part;
}

// One `Shared` handle less. The last one ends the whole value by the
// code of its own type. True for the last handle of a value without
// the address of a table, which the caller ends as the type that it
// names.
bool _gopp_part_release(uint8_t *part) {
  const gopp_table *table = table_of(part);
  if (table == 0) {
    return _gopp_shared_release(part);
  }
  uint8_t *start = part + table->to_start;
  if (_gopp_shared_release(start)) {
    if (table->whole->drop != 0) {
      table->whole->drop(start);
    }
    _gopp_weak_release(start);
  }
  return false;
}

// `p.(*T)` for a pointer to a value that is no `T`: `from` is the type
// of `p` as the program writes it, `of` the record of `T`.
void _gopp_panic_part(const uint8_t *part, const void *of, gopp_string from) {
  const gopp_whole *wanted = of;
  _gopp_fail_part(from, table_of(part)->whole->name, wanted->name);
}
