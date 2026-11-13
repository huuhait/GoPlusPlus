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

// `x.(T)` for a value that is no `T`: `itab` is the first word of `x`,
// `wanted` the descriptor of `T`, `iface` the type of `x` as the
// program writes it.
void _gopp_panic_assert(const uint8_t *itab, const void *wanted, gopp_string iface) {
  const gopp_type *have = itab == 0 ? 0 : *(const gopp_type *const *)itab;
  _gopp_fail_assert(iface, have, wanted);
}
