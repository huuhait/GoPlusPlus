// Maps: a hash table with one owner. The slots are in groups of eight;
// each group has a word of eight control bytes, one for each slot: empty,
// deleted, or seven bits of the hash of the key that is there. A key is
// looked for in the group that its hash names and then in the groups
// that follow at growing steps, until a group with an empty slot.
//
// Generated code says what the keys and the values are with a record of
// five numbers (`gopp_mapinfo`), and passes keys and values by address.

#include "internal.h"

void free(void *room);

typedef struct gopp_mapinfo {
  uint64_t key_size;
  uint64_t value_size;
  // Where the value is in a slot, and how large a slot is: never 0.
  uint64_t value_offset;
  uint64_t slot_size;
  // How a key is hashed and compared: by its bytes, as a string, or as
  // a float of 32 or 64 bits.
  uint64_t key_kind;
  // Makes the value at the address a copy of its own; null for a value
  // that is copied by its bytes.
  void (*copy_value)(uint8_t *value);
  // The hash and the equality of keys of the last kind: the glue of
  // their type.
  uint64_t (*hash)(uint8_t *key, uint64_t seed);
  bool (*eq)(uint8_t *a, uint8_t *b);
  // Ends the value at the address; null for a value that holds nothing
  // that is freed.
  void (*drop_value)(uint8_t *value);
} gopp_mapinfo;

enum { KEY_BYTES = 0, KEY_STRING = 1, KEY_FLOAT32 = 2, KEY_FLOAT64 = 3, KEY_GLUE = 4 };

typedef struct gopp_map {
  void *allocator;
  uint64_t count;
  // How many range loops walk the map now.
  uint64_t iterators;
  uint64_t seed;
  // A power of two, or 0 for a map without a table.
  uint64_t groups;
  // How many empty slots may still be filled before the table grows.
  uint64_t growth_left;
  uint64_t deleted;
  uint64_t *control;
  uint8_t *slots;
} gopp_map;

// A range loop: where it started, and how many slots it has passed.
typedef struct gopp_map_iter {
  gopp_map *map;
  uint64_t start;
  uint64_t passed;
} gopp_map_iter;

enum { SLOTS = 8, EMPTY = 0x80, DELETED = 0xfe };
#define LOW_BITS 0x0101010101010101ull
#define HIGH_BITS 0x8080808080808080ull
#define ALL_EMPTY HIGH_BITS
// The most that the slots of a map are asked to hold at once.
#define MOST_BYTES ((uint64_t)1 << 48)

static uint64_t mixed(uint64_t bits) {
  bits ^= bits >> 33;
  bits *= 0xff51afd7ed558ccdull;
  bits ^= bits >> 33;
  bits *= 0xc4ceb9fe1a85ec53ull;
  return bits ^ bits >> 33;
}

// A number that the next one cannot be told from: seeds, and where a
// range loop starts.
static uint64_t random_bits(void) {
  static uint64_t state = 0x9e3779b97f4a7c15ull;
  state += 0x9e3779b97f4a7c15ull;
  return mixed(state ^ (uintptr_t)&state);
}

// The hash of `count` bytes. The hash glue of a type gives each part of
// a value the hash of the parts before it as its seed.
uint64_t _gopp_hash_bytes(const void *first, uint64_t count, uint64_t seed) {
  const uint8_t *bytes = first;
  uint64_t hash = seed ^ 0xcbf29ce484222325ull;
  // Eight bytes at a time, then the rest.
  for (; count >= 8; bytes += 8, count -= 8) {
    uint64_t word;
    __builtin_memcpy(&word, bytes, 8);
    hash = mixed(hash ^ word) * 0x100000001b3ull;
  }
  uint64_t rest = 0;
  if (count > 0) {
    __builtin_memcpy(&rest, bytes, count);
  }
  return mixed(hash ^ rest ^ (count << 56));
}

uint64_t _gopp_hash_string(gopp_string text, uint64_t seed) {
  return _gopp_hash_bytes(text.ptr, text.len, seed);
}

// The two zeros are equal, so they have one hash. What is no number is
// equal to nothing, itself too: each has a hash of its own, and a key
// that holds one is never found again.
uint64_t _gopp_hash_float(double value, uint64_t seed) {
  if (value == 0) {
    value = 0;
  }
  if (value != value) {
    return random_bits();
  }
  return _gopp_hash_bytes(&value, sizeof value, seed);
}

// The hash of `count` values of `size` bytes, each by the glue `hash`.
uint64_t _gopp_hash_each(void *first, uint64_t count, uint64_t size, uint64_t seed,
                         uint64_t (*hash)(uint8_t *value, uint64_t seed)) {
  for (uint8_t *at = first; count > 0; count--, at += size) {
    seed = hash(at, seed);
  }
  return seed;
}

bool _gopp_mem_eq(const void *a, const void *b, uint64_t size) {
  return size == 0 || __builtin_memcmp(a, b, size) == 0;
}

// Whether each of `count` values of `size` bytes from `a` on is equal to
// the one from `b` on, by the glue `eq`.
bool _gopp_each_eq(void *a, void *b, uint64_t count, uint64_t size,
                   bool (*eq)(uint8_t *a, uint8_t *b)) {
  for (uint8_t *first = a, *second = b; count > 0; count--, first += size, second += size) {
    if (!eq(first, second)) {
      return false;
    }
  }
  return true;
}

static uint64_t hash_key(const gopp_mapinfo *info, const void *key, uint64_t seed) {
  switch (info->key_kind) {
  case KEY_STRING:
    return _gopp_hash_string(*(const gopp_string *)key, seed);
  case KEY_FLOAT32: {
    float narrow;
    __builtin_memcpy(&narrow, key, sizeof narrow);
    return _gopp_hash_float(narrow, seed);
  }
  case KEY_FLOAT64: {
    double value;
    __builtin_memcpy(&value, key, sizeof value);
    return _gopp_hash_float(value, seed);
  }
  case KEY_GLUE:
    return info->hash((uint8_t *)key, seed);
  default:
    return _gopp_hash_bytes(key, info->key_size, seed);
  }
}

static bool same_key(const gopp_mapinfo *info, const void *a, const void *b) {
  switch (info->key_kind) {
  case KEY_STRING: {
    const gopp_string *first = a, *second = b;
    return first->len == second->len &&
           (first->len == 0 || __builtin_memcmp(first->ptr, second->ptr, first->len) == 0);
  }
  case KEY_FLOAT32: {
    float first, second;
    __builtin_memcpy(&first, a, sizeof first);
    __builtin_memcpy(&second, b, sizeof second);
    return first == second;
  }
  case KEY_FLOAT64: {
    double first, second;
    __builtin_memcpy(&first, a, sizeof first);
    __builtin_memcpy(&second, b, sizeof second);
    return first == second;
  }
  case KEY_GLUE:
    return info->eq((uint8_t *)a, (uint8_t *)b);
  default:
    return _gopp_mem_eq(a, b, info->key_size);
  }
}

// The bytes of a control word that are `byte`, as their high bits. A
// byte beside a match may be marked too; the key is compared anyway.
static uint64_t matches(uint64_t word, uint8_t byte) {
  uint64_t differ = word ^ (LOW_BITS * byte);
  return (differ - LOW_BITS) & ~differ & HIGH_BITS;
}

// Empty is the one control byte with its high bit and no other.
static uint64_t empties(uint64_t word) { return word & ~(word << 6) & HIGH_BITS; }

// Empty or deleted: the high bit.
static uint64_t free_slots(uint64_t word) { return word & HIGH_BITS; }

static uint64_t first_of(uint64_t marks) { return (uint64_t)__builtin_ctzll(marks) >> 3; }

static void set_control(gopp_map *map, uint64_t group, uint64_t at, uint8_t byte) {
  uint64_t shift = at * 8;
  map->control[group] = (map->control[group] & ~((uint64_t)0xff << shift)) | (uint64_t)byte << shift;
}

static uint8_t *slot_of(const gopp_map *map, const gopp_mapinfo *info, uint64_t group, uint64_t at) {
  return map->slots + (group * SLOTS + at) * info->slot_size;
}

// The slot of the key, or null. The hash is worked out by the caller.
static uint8_t *find(const gopp_map *map, const gopp_mapinfo *info, const void *key, uint64_t hash) {
  if (!map || map->groups == 0) {
    return 0;
  }
  uint64_t mask = map->groups - 1;
  uint64_t group = (hash >> 7) & mask;
  for (uint64_t step = 1;; group = (group + step++) & mask) {
    uint64_t word = map->control[group];
    for (uint64_t found = matches(word, hash & 0x7f); found; found &= found - 1) {
      uint8_t *slot = slot_of(map, info, group, first_of(found));
      if (same_key(info, slot, key)) {
        return slot;
      }
    }
    if (empties(word)) {
      return 0;
    }
  }
}

static void table(gopp_map *map, const gopp_mapinfo *info, uint64_t groups) {
  uint64_t slots = groups * SLOTS;
  uint64_t bytes = slots * info->slot_size;
  map->control = _gopp_alloc(groups * sizeof(uint64_t));
  for (uint64_t at = 0; at < groups; at++) {
    map->control[at] = ALL_EMPTY;
  }
  map->slots = _gopp_alloc(bytes);
  __builtin_memset(map->slots, 0, bytes);
  map->groups = groups;
  map->deleted = 0;
  // An eighth of the slots stays empty, so that a search ends.
  map->growth_left = slots - groups;
}

// A slot for a key that is not in the map, whose hash is `hash`.
static uint8_t *place(gopp_map *map, const gopp_mapinfo *info, uint64_t hash) {
  uint64_t mask = map->groups - 1;
  uint64_t group = (hash >> 7) & mask;
  for (uint64_t step = 1;; group = (group + step++) & mask) {
    uint64_t open = free_slots(map->control[group]);
    if (!open) {
      continue;
    }
    uint64_t at = first_of(open);
    bool was_empty = (map->control[group] >> (at * 8) & 0xff) == EMPTY;
    map->growth_left -= was_empty;
    map->deleted -= !was_empty;
    set_control(map, group, at, hash & 0x7f);
    return slot_of(map, info, group, at);
  }
}

// A table with room again: twice as large, or as large without what was
// deleted from it.
static void rehash(gopp_map *map, const gopp_mapinfo *info) {
  gopp_map old = *map;
  uint64_t groups = old.groups == 0 ? 1 : old.groups;
  if (old.groups > 0 && old.deleted < old.groups * SLOTS / 2) {
    groups *= 2;
  }
  table(map, info, groups);
  for (uint64_t group = 0; group < old.groups; group++) {
    for (uint64_t full = ~old.control[group] & HIGH_BITS; full; full &= full - 1) {
      const uint8_t *from = slot_of(&old, info, group, first_of(full));
      uint8_t *to = place(map, info, hash_key(info, from, map->seed));
      __builtin_memcpy(to, from, info->slot_size);
    }
  }
  free(old.control);
  free(old.slots);
}

static gopp_map *fresh(void) {
  gopp_map *map = _gopp_alloc(sizeof *map);
  *map = (gopp_map){.seed = random_bits()};
  return map;
}

void *_gopp_map_make(const void *shape, int64_t hint) {
  const gopp_mapinfo *info = shape;
  gopp_map *map = fresh();
  // As in Go, a hint that is negative or asks for too much is no hint.
  if (hint > 0 && (uint64_t)hint <= MOST_BYTES / info->slot_size / 2) {
    // Room for `hint` entries at seven of eight slots.
    uint64_t groups = 1;
    while (groups * (SLOTS - 1) < (uint64_t)hint) {
      groups *= 2;
    }
    table(map, info, groups);
  }
  return map;
}

uint64_t _gopp_map_len(const void *map) { return map ? ((const gopp_map *)map)->count : 0; }

// `v, ok := m[k]`: the value of the key, or zero for a key that is not
// there.
bool _gopp_map_read(const void *map, const void *shape, const void *key, void *value) {
  const gopp_mapinfo *info = shape;
  const gopp_map *of = map;
  const uint8_t *slot = of && of->count > 0 ? find(of, info, key, hash_key(info, key, of->seed)) : 0;
  if (!slot) {
    __builtin_memset(value, 0, info->value_size);
    return false;
  }
  __builtin_memcpy(value, slot + info->value_offset, info->value_size);
  return true;
}

// `m[k] = v`: where the value of the key is, which the caller writes.
// A key that is new has a zero value there. `place_of_map` is the
// variable that holds the map: a nil map becomes a map.
void *_gopp_map_set(void *place_of_map, const void *shape, const void *key) {
  const gopp_mapinfo *info = shape;
  gopp_map **held = place_of_map;
  if (!*held) {
    *held = fresh();
  }
  gopp_map *map = *held;
  uint64_t hash = hash_key(info, key, map->seed);
  uint8_t *slot = find(map, info, key, hash);
  if (slot) {
    return slot + info->value_offset;
  }
  // A loop that walks the map does not see it grow (RNG-4).
  if (map->iterators > 0) {
    _gopp_fail("map insert during iteration");
  }
  if (map->growth_left == 0) {
    rehash(map, info);
  }
  slot = place(map, info, hash);
  __builtin_memcpy(slot, key, info->key_size);
  __builtin_memset(slot + info->value_offset, 0, info->value_size);
  map->count++;
  return slot + info->value_offset;
}

void _gopp_map_delete(void *map, const void *shape, const void *key) {
  const gopp_mapinfo *info = shape;
  gopp_map *of = map;
  if (!of || of->count == 0) {
    return;
  }
  uint8_t *slot = find(of, info, key, hash_key(info, key, of->seed));
  if (!slot) {
    return;
  }
  uint64_t index = (uint64_t)(slot - of->slots) / info->slot_size;
  uint64_t group = index / SLOTS;
  // A search for another key may pass this group only if it is full:
  // then the slot stays marked.
  bool passed = !empties(of->control[group]);
  if (info->drop_value) {
    info->drop_value(slot + info->value_offset);
  }
  set_control(of, group, index % SLOTS, passed ? DELETED : EMPTY);
  of->growth_left += !passed;
  of->deleted += passed;
  of->count--;
}

// Ends every value of the map, if values hold what is freed.
static void drop_values(const gopp_map *map, const gopp_mapinfo *info) {
  if (!info->drop_value) {
    return;
  }
  for (uint64_t group = 0; group < map->groups; group++) {
    for (uint64_t full = ~map->control[group] & HIGH_BITS; full; full &= full - 1) {
      info->drop_value(slot_of(map, info, group, first_of(full)) + info->value_offset);
    }
  }
}

// The map ends with its owner: its values, its table and itself.
void _gopp_map_drop(void *map, const void *shape) {
  gopp_map *of = map;
  if (!of) {
    return;
  }
  drop_values(of, shape);
  free(of->control);
  free(of->slots);
  free(of);
}

void _gopp_map_clear(void *map, const void *shape) {
  const gopp_mapinfo *info = shape;
  gopp_map *of = map;
  if (!of) {
    return;
  }
  if (of->iterators > 0) {
    _gopp_fail("map clear during iteration");
  }
  drop_values(of, info);
  for (uint64_t group = 0; group < of->groups; group++) {
    of->control[group] = ALL_EMPTY;
  }
  __builtin_memset(of->slots, 0, of->groups * SLOTS * info->slot_size);
  of->count = 0;
  of->deleted = 0;
  of->growth_left = of->groups * SLOTS - of->groups;
}

// The copy that a new owner gets: a map of its own with the same
// entries. A nil map stays nil.
void *_gopp_map_copy(const void *map, const void *shape) {
  const gopp_mapinfo *info = shape;
  const gopp_map *of = map;
  if (!of) {
    return 0;
  }
  gopp_map *copy = fresh();
  if (of->groups == 0) {
    return copy;
  }
  copy->seed = of->seed;
  table(copy, info, of->groups);
  __builtin_memcpy(copy->control, of->control, of->groups * sizeof(uint64_t));
  __builtin_memcpy(copy->slots, of->slots, of->groups * SLOTS * info->slot_size);
  copy->count = of->count;
  copy->deleted = of->deleted;
  copy->growth_left = of->growth_left;
  if (info->copy_value) {
    for (uint64_t group = 0; group < copy->groups; group++) {
      for (uint64_t full = ~copy->control[group] & HIGH_BITS; full; full &= full - 1) {
        info->copy_value(slot_of(copy, info, group, first_of(full)) + info->value_offset);
      }
    }
  }
  return copy;
}

// A range loop starts at a slot that no program can tell beforehand.
void _gopp_map_iter_start(void *iter, void *map) {
  gopp_map_iter *walk = iter;
  gopp_map *of = map;
  *walk = (gopp_map_iter){of, random_bits(), 0};
  if (of) {
    of->iterators++;
  }
}

// Ends a loop, on every way out of it.
void _gopp_map_iter_end(void *iter) {
  gopp_map_iter *walk = iter;
  if (walk->map) {
    walk->map->iterators--;
  }
}

// The next entry: its key is copied out, and the result is where its
// value is in the map. Null after the last one.
void *_gopp_map_iter_next(void *iter, const void *shape, void *key) {
  const gopp_mapinfo *info = shape;
  gopp_map_iter *walk = iter;
  const gopp_map *map = walk->map;
  uint64_t slots = map ? map->groups * SLOTS : 0;
  while (walk->passed < slots) {
    uint64_t index = (walk->start + walk->passed++) & (slots - 1);
    uint8_t control = (uint8_t)(map->control[index / SLOTS] >> (index % SLOTS * 8));
    if (control & 0x80) {
      continue;
    }
    uint8_t *slot = map->slots + index * info->slot_size;
    __builtin_memcpy(key, slot, info->key_size);
    return slot + info->value_offset;
  }
  return 0;
}
