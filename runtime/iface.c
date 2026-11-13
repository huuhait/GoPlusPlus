// Interface values (TYP-5 to TYP-9): two words. The first says the type
// of the value that is held: it points to the descriptor of the type,
// or to a table of methods whose first word does. The first word of a
// descriptor points to the descriptor itself, so both are read alike. A
// nil interface value is two zero words. The second word is the value
// itself for a type that the descriptor marks so (up to one word that
// owns nothing, TYP-8); for any other type it points to a box that the
// interface value owns alone: there is no count.

#include "internal.h"

void free(void *room);

// The descriptor of the type of the value that is held; null for nil.
static const gopp_type *type_of(const gopp_iface *value) {
  return value->itab == 0 ? 0 : *(const gopp_type *const *)value->itab;
}

// Where the value is that an interface value of the type holds.
static uint8_t *held(gopp_iface *value, const gopp_type *type) {
  return (type->flags & GOPP_TYPE_INLINE) != 0 ? (uint8_t *)&value->data : value->data;
}

// A box with the `size` bytes at `value`, which is no part of what the
// bytes were copied from.
uint8_t *_gopp_iface_box(const void *value, uint64_t size) {
  uint8_t *box = _gopp_alloc(size);
  __builtin_memcpy(box, value, size);
  return box;
}

// The interface value at the address becomes a copy of its own: a box
// of its own with a copy of the value, by the rule of its type.
void _gopp_iface_clone(uint8_t *value) {
  gopp_iface *iface = (gopp_iface *)value;
  const gopp_type *type = type_of(iface);
  if (type == 0 || (type->flags & GOPP_TYPE_INLINE) != 0) {
    return;
  }
  iface->data = _gopp_iface_box(iface->data, type->size);
  if (type->clone != 0) {
    type->clone(iface->data);
  }
}

// The interface value at the address ends: the value that it holds
// ends, and its box is freed.
void _gopp_iface_drop(uint8_t *value) {
  gopp_iface *iface = (gopp_iface *)value;
  const gopp_type *type = type_of(iface);
  if (type == 0 || (type->flags & GOPP_TYPE_INLINE) != 0) {
    return;
  }
  if (type->drop != 0) {
    type->drop(iface->data);
  }
  free(iface->data);
}

// Whether two interface values are equal: both nil, or values of one
// type that are equal. Values of one type that is not compared end the
// program, as in Go.
bool _gopp_iface_eq(uint8_t *a, uint8_t *b) {
  gopp_iface *first = (gopp_iface *)a;
  gopp_iface *second = (gopp_iface *)b;
  const gopp_type *type = type_of(first);
  if (type != type_of(second)) {
    return false;
  }
  if (type == 0) {
    return true;
  }
  if ((type->flags & GOPP_TYPE_COMPARABLE) == 0) {
    _gopp_fail_type("comparing uncomparable type ", type);
  }
  uint8_t *x = held(first, type);
  uint8_t *y = held(second, type);
  return type->eq != 0 ? type->eq(x, y) : _gopp_mem_eq(x, y, type->size);
}

// The hash of an interface value, so that equal values have one hash.
uint64_t _gopp_iface_hash(uint8_t *value, uint64_t seed) {
  gopp_iface *iface = (gopp_iface *)value;
  const gopp_type *type = type_of(iface);
  if (type == 0) {
    return _gopp_hash_bytes(0, 0, seed);
  }
  if ((type->flags & GOPP_TYPE_COMPARABLE) == 0) {
    _gopp_fail_type("hash of unhashable type ", type);
  }
  uint8_t *x = held(iface, type);
  return type->hash != 0 ? type->hash(x, seed) : _gopp_hash_bytes(x, type->size, seed);
}

// The tables of methods that the program made while it ran: one for
// each type and interface type that was asked for, or null for a type
// without the methods of the interface. The table is a set of slots,
// each found from the two addresses and then by walking on.
typedef struct made {
  const gopp_type *type;
  const gopp_itype *wanted;
  uint8_t **table;
} made;

static made *made_tables;
static uint64_t made_room;
static uint64_t made_count;

void _gopp_iface_reset(void) {
  for (uint64_t at = 0; at < made_room; at++) {
    free(made_tables[at].table);
  }
  free(made_tables);
  made_tables = 0;
  made_room = 0;
  made_count = 0;
}

static uint64_t slot_of(const gopp_type *type, const gopp_itype *wanted, uint64_t room) {
  uint64_t pair[2] = {(uint64_t)(uintptr_t)type, (uint64_t)(uintptr_t)wanted};
  return _gopp_hash_bytes(pair, sizeof pair, 0) & (room - 1);
}

// The slot of the pair, or the free one where it goes.
static made *find(made *tables, uint64_t room, const gopp_type *type, const gopp_itype *wanted) {
  uint64_t at = slot_of(type, wanted, room);
  while (tables[at].type != 0 && (tables[at].type != type || tables[at].wanted != wanted)) {
    at = (at + 1) & (room - 1);
  }
  return &tables[at];
}

static int compare(gopp_string a, gopp_string b) {
  uint64_t shared = a.len < b.len ? a.len : b.len;
  int order = shared == 0 ? 0 : __builtin_memcmp(a.ptr, b.ptr, shared);
  if (order != 0) {
    return order;
  }
  return a.len < b.len ? -1 : a.len > b.len;
}

// What the type has for the method of the key; null if it has none.
static uint8_t *method_of(const gopp_type *type, gopp_string key) {
  const gopp_methods *methods = type->methods;
  uint64_t low = 0;
  uint64_t high = methods == 0 ? 0 : methods->count;
  while (low < high) {
    uint64_t middle = low + (high - low) / 2;
    int order = compare(methods->all[middle].key, key);
    if (order == 0) {
      return methods->all[middle].code;
    }
    if (order < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return 0;
}

// A table of the methods of the interface type for the type: the
// descriptor, then what the type has for each method; null if a method
// is missing.
static uint8_t **table_for(const gopp_type *type, const gopp_itype *wanted) {
  uint8_t **table = _gopp_alloc((wanted->count + 1) * sizeof *table);
  table[0] = (uint8_t *)type;
  for (uint64_t at = 0; at < wanted->count; at++) {
    table[at + 1] = method_of(type, wanted->keys[at]);
    if (table[at + 1] == 0) {
      free(table);
      return 0;
    }
  }
  return table;
}

// The first word that the value of an interface value has as a value
// of the interface type `wanted`: its table of methods there. Null for
// nil, and for a value whose type lacks a method of the interface.
// `itab` is the first word that the value has now.
uint8_t *_gopp_iface_table(const uint8_t *itab, const void *wanted) {
  if (itab == 0) {
    return 0;
  }
  const gopp_type *type = *(const gopp_type *const *)itab;
  const gopp_itype *itype = wanted;
  if (itype->count == 0) {
    return (uint8_t *)type;
  }
  // Half of the slots stay free, so that a search ends.
  if (made_count * 2 >= made_room) {
    uint64_t room = made_room == 0 ? 64 : made_room * 2;
    made *grown = _gopp_alloc(room * sizeof *grown);
    __builtin_memset(grown, 0, room * sizeof *grown);
    for (uint64_t at = 0; at < made_room; at++) {
      if (made_tables[at].type != 0) {
        *find(grown, room, made_tables[at].type, made_tables[at].wanted) = made_tables[at];
      }
    }
    free(made_tables);
    made_tables = grown;
    made_room = room;
  }
  made *slot = find(made_tables, made_room, type, itype);
  if (slot->type == 0) {
    *slot = (made){type, itype, table_for(type, itype)};
    made_count++;
  }
  return (uint8_t *)slot->table;
}

// `x.(I)` for an interface type `I` whose methods the value of `x` does
// not have, or for an `x` that is nil.
void _gopp_panic_assert_iface(const uint8_t *itab, const void *wanted) {
  const gopp_itype *itype = wanted;
  const gopp_type *have = itab == 0 ? 0 : *(const gopp_type *const *)itab;
  gopp_string missing = {0};
  for (uint64_t at = 0; have != 0 && at < itype->count; at++) {
    if (method_of(have, itype->keys[at]) == 0) {
      // The name of the method is the start of its key.
      missing = itype->keys[at];
      for (uint64_t end = 0; end < missing.len; end++) {
        if (missing.ptr[end] == '|') {
          missing.len = end;
        }
      }
      break;
    }
  }
  _gopp_fail_convert(itype->name, have, missing);
}

// `x.(T)` for a value that is no `T`: `itab` is the first word of `x`,
// `wanted` the descriptor of `T`, `iface` the type of `x` as the
// program writes it.
void _gopp_panic_assert(const uint8_t *itab, const void *wanted, gopp_string iface) {
  const gopp_type *have = itab == 0 ? 0 : *(const gopp_type *const *)itab;
  _gopp_fail_assert(iface, have, wanted);
}
