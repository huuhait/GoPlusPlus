// Shared and Weak handles (OWN-P8). A handle is the address of a value
// on the heap. Two counts are in front of the value: the Shared handles,
// and the Weak handles with one more for all the Shared handles
// together. The value ends with its last Shared handle, by the code that
// the compiler makes for the type of the handle; the room is freed with
// the last handle of either kind. The counts are atomic: a handle may be
// given to another goroutine (CON-19).

#include "internal.h"

void free(void *room);

typedef struct {
  uint64_t shared;
  uint64_t weak;
} gopp_counts;

static gopp_counts *counts_of(uint8_t *value) { return (gopp_counts *)value - 1; }

// A value on the heap with the `size` bytes at `value`, and one Shared
// handle to it.
uint8_t *_gopp_shared_new(const void *value, uint64_t size) {
  gopp_counts *counts = _gopp_alloc(sizeof(gopp_counts) + size);
  counts->shared = 1;
  counts->weak = 1;
  uint8_t *held = (uint8_t *)(counts + 1);
  __builtin_memcpy(held, value, size);
  return held;
}

void _gopp_shared_retain(uint8_t *value) {
  __atomic_fetch_add(&counts_of(value)->shared, 1, __ATOMIC_RELAXED);
}

// One Shared handle less. True for the last one: the caller ends the
// value and then gives up the Weak handle of the Shared handles, with
// `_gopp_weak_release`.
bool _gopp_shared_release(uint8_t *value) {
  return __atomic_fetch_sub(&counts_of(value)->shared, 1, __ATOMIC_ACQ_REL) == 1;
}

void _gopp_weak_retain(uint8_t *value) {
  __atomic_fetch_add(&counts_of(value)->weak, 1, __ATOMIC_RELAXED);
}

void _gopp_weak_release(uint8_t *value) {
  gopp_counts *counts = counts_of(value);
  if (__atomic_fetch_sub(&counts->weak, 1, __ATOMIC_ACQ_REL) == 1) {
    free(counts);
  }
}

// `w.Lock()`: one more Shared handle if the value has not ended, null
// if it has.
uint8_t *_gopp_weak_lock(uint8_t *value) {
  gopp_counts *counts = counts_of(value);
  uint64_t shared = __atomic_load_n(&counts->shared, __ATOMIC_RELAXED);
  while (shared != 0) {
    if (__atomic_compare_exchange_n(&counts->shared, &shared, shared + 1, true, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      return value;
    }
  }
  return 0;
}
