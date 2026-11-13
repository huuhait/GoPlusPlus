// Slices: a buffer with one owner. The elements follow a header with
// the room of the buffer and its allocator; a slice without a buffer of
// its own (nil, empty, a view of another) has no backing. Every
// function gets the size of an element, and takes a slice by the
// address of its four words.

#include "internal.h"

void free(void *room);

typedef struct gopp_slice_buffer {
  uint64_t alloc_cap;
  void *allocator;
} gopp_slice_buffer;

[[noreturn]] static void fail(const char *message) {
  _gopp_write_text("panic: runtime error: ");
  _gopp_write_text(message);
  _gopp_write_text("\n\ngoroutine 1 [running]:\n");
  _gopp_rt_exit(2);
}

// The most that one buffer holds, and the sizes that the allocator of
// Go gives: a slice grows to the capacities that it has in Go.
#define GOPP_MAX_ALLOC ((uint64_t)1 << 48)
enum { GOPP_MAX_SMALL = 32768, GOPP_MALLOC_HEADER = 8, GOPP_HEADER_FROM = 512, GOPP_PAGE = 8192 };
static const uint16_t gopp_size_classes[] = {
    8,    16,   24,   32,   48,   64,   80,    96,    112,   128,   144,   160,   176,   192,
    208,  224,  240,  256,  288,  320,  352,   384,   416,   448,   480,   512,   576,   640,
    704,  768,  896,  1024, 1152, 1280, 1408,  1536,  1792,  2048,  2304,  2688,  3072,  3200,
    3456, 4096, 4864, 5376, 6144, 6528, 6784,  6912,  8192,  9472,  9728,  10240, 10880, 12288,
    13568, 14336, 16384, 18432, 19072, 20480, 21760, 24576, 27264, 28672, 32768};

// `roundupsize` of Go: the bytes that a request of `size` bytes gets.
static uint64_t rounded(uint64_t size, bool no_pointers) {
  uint64_t asked = size;
  if (asked <= GOPP_MAX_SMALL - GOPP_MALLOC_HEADER) {
    if (!no_pointers && asked > GOPP_HEADER_FROM) {
      asked += GOPP_MALLOC_HEADER;
    }
    for (uint64_t at = 0;; at++) {
      if (gopp_size_classes[at] >= asked) {
        return gopp_size_classes[at] - (asked - size);
      }
    }
  }
  asked += GOPP_PAGE - 1;
  return asked < size ? size : asked & ~(uint64_t)(GOPP_PAGE - 1);
}

// `nextslicecap` of Go.
static int64_t next_cap(int64_t new_len, int64_t old_cap) {
  int64_t doubled = old_cap + old_cap;
  if (new_len > doubled) {
    return new_len;
  }
  enum { THRESHOLD = 256 };
  if (old_cap < THRESHOLD) {
    return doubled;
  }
  int64_t cap = old_cap;
  do {
    cap += (cap + 3 * THRESHOLD) >> 2;
  } while ((uint64_t)cap < (uint64_t)new_len);
  return cap <= 0 ? new_len : cap;
}

// The capacity of a slice that grows to `new_len` elements, as
// `growslice` of Go works it out.
static uint64_t grown_cap(uint64_t new_len, uint64_t old_cap, uint64_t size, bool no_pointers) {
  if (new_len > (uint64_t)__INT64_MAX__) {
    fail("growslice: len out of range");
  }
  uint64_t cap = (uint64_t)next_cap((int64_t)new_len, (int64_t)old_cap);
  uint64_t bytes;
  bool overflow = __builtin_mul_overflow(cap, size, &bytes);
  if (!overflow) {
    cap = rounded(bytes, no_pointers) / size;
    overflow = __builtin_mul_overflow(cap, size, &bytes);
  }
  if (overflow || bytes > GOPP_MAX_ALLOC) {
    fail("growslice: len out of range");
  }
  return cap;
}

// What a slice without elements points to: it is not nil.
static uint8_t gopp_zero_base;

// A slice of `len` elements with room for `cap`, in a new buffer; every
// element is zero.
static gopp_slice fresh(uint64_t len, uint64_t cap, uint64_t size) {
  uint64_t bytes = cap * size;
  if (bytes == 0) {
    return (gopp_slice){&gopp_zero_base, len, cap, 0};
  }
  gopp_slice_buffer *buffer = _gopp_alloc(sizeof *buffer + bytes);
  buffer->alloc_cap = cap;
  buffer->allocator = 0;
  __builtin_memset(buffer + 1, 0, bytes);
  return (gopp_slice){buffer + 1, len, cap, (uint8_t *)buffer};
}

void _gopp_slice_make(void *slice, int64_t len, int64_t cap, uint64_t size) {
  uint64_t bytes;
  bool fits = cap >= 0 && !__builtin_mul_overflow((uint64_t)cap, size, &bytes) &&
              bytes <= GOPP_MAX_ALLOC;
  if (!fits || len < 0 || len > cap) {
    bool len_fits = len >= 0 && !__builtin_mul_overflow((uint64_t)len, size, &bytes) &&
                    bytes <= GOPP_MAX_ALLOC;
    fail(len_fits ? "makeslice: cap out of range" : "makeslice: len out of range");
  }
  *(gopp_slice *)slice = fresh((uint64_t)len, (uint64_t)cap, size);
}

// The slice with room for `more` elements after its own, which it
// keeps. A buffer that it leaves is given back.
static gopp_slice grown(gopp_slice old, uint64_t more, uint64_t size, bool no_pointers,
                        uint8_t **left) {
  *left = 0;
  uint64_t new_len = old.len + more;
  if (new_len < old.len) {
    fail("growslice: len out of range");
  }
  if (size == 0) {
    old.cap = new_len > old.cap ? new_len : old.cap;
    return old;
  }
  // Only a buffer of its own has room that is its own.
  if (old.backing && new_len <= old.cap) {
    return old;
  }
  gopp_slice wider = fresh(old.len, grown_cap(new_len, old.cap, size, no_pointers), size);
  if (old.len > 0) {
    __builtin_memcpy(wider.ptr, old.ptr, old.len * size);
  }
  *left = old.backing;
  return wider;
}

void _gopp_slice_grow(void *slice, uint64_t more, uint64_t size, bool no_pointers) {
  gopp_slice *of = slice;
  uint8_t *left;
  *of = grown(*of, more, size, no_pointers, &left);
  free(left);
}

// `append(to, from...)`: `from` is a slice or a string, which may be
// the elements of `to` itself.
void _gopp_slice_append(void *slice, const void *from, uint64_t size, bool no_pointers) {
  gopp_slice *to = slice;
  const gopp_slice *added = from;
  uint64_t count = added->len;
  const void *elements = added->ptr;
  uint8_t *left;
  *to = grown(*to, count, size, no_pointers, &left);
  if (count > 0 && size > 0) {
    __builtin_memmove((uint8_t *)to->ptr + to->len * size, elements, count * size);
  }
  to->len += count;
  free(left);
}

// The slice becomes a copy of itself: the same elements in a buffer of
// its own.
void _gopp_slice_copy(void *slice, uint64_t size) {
  gopp_slice *of = slice;
  if (of->len == 0) {
    *of = (gopp_slice){of->ptr, 0, 0, 0};
    return;
  }
  gopp_slice copy = fresh(of->len, of->len, size);
  if (size > 0) {
    __builtin_memcpy(copy.ptr, of->ptr, of->len * size);
  }
  *of = copy;
}

// `copy(&to, from)`: as many elements as both have, which may overlap.
uint64_t _gopp_slice_copy_into(const void *to, const void *from, uint64_t size) {
  const gopp_slice *target = to;
  const gopp_slice *source = from;
  uint64_t count = target->len < source->len ? target->len : source->len;
  if (count > 0 && size > 0) {
    __builtin_memmove(target->ptr, source->ptr, count * size);
  }
  return count;
}

void _gopp_slice_clear(const void *slice, uint64_t size) {
  const gopp_slice *of = slice;
  if (of->len > 0 && size > 0) {
    __builtin_memset(of->ptr, 0, of->len * size);
  }
}

// `[]byte(text)`.
void _gopp_bytes_from_string(void *slice, gopp_string text) {
  gopp_slice bytes = fresh(text.len, text.len, 1);
  if (text.len > 0) {
    __builtin_memcpy(bytes.ptr, text.ptr, text.len);
  }
  *(gopp_slice *)slice = bytes;
}

// `[]rune(text)`: the code points of the string, as `range` gives them.
void _gopp_runes_from_string(void *slice, gopp_string text) {
  uint64_t count = 0;
  for (uint64_t at = 0; at < text.len; at += _gopp_string_decode(text, at) >> 32) {
    count++;
  }
  gopp_slice runes = fresh(count, count, sizeof(int32_t));
  int32_t *points = runes.ptr;
  for (uint64_t at = 0; at < text.len;) {
    uint64_t packed = _gopp_string_decode(text, at);
    *points++ = (int32_t)(uint32_t)packed;
    at += packed >> 32;
  }
  *(gopp_slice *)slice = runes;
}

// Calls `each` with the address of every one of `count` values of
// `size` bytes from `first` on: the elements of a slice or an array,
// each of which is made a copy of its own.
void _gopp_each(void *first, uint64_t count, uint64_t size, void (*each)(uint8_t *value)) {
  for (uint8_t *at = first; count > 0; count--, at += size) {
    each(at);
  }
}

// The slice ends with its owner: its buffer is freed. Its elements were
// ended before, if they hold what is freed.
void _gopp_slice_free(const void *slice) {
  free(((const gopp_slice *)slice)->backing);
}

// `copy(&to, from)` for elements that hold what is freed. `from` is a
// slice of its own that ends here: as many elements as both have are
// moved over those of `to`, which are ended first by `drop`, as are the
// elements of `from` that are left.
uint64_t _gopp_slice_move_into(const void *to, const void *from, uint64_t size,
                               void (*drop)(uint8_t *value)) {
  const gopp_slice *target = to;
  const gopp_slice *source = from;
  uint64_t count = target->len < source->len ? target->len : source->len;
  _gopp_each(target->ptr, count, size, drop);
  if (count > 0 && size > 0) {
    __builtin_memcpy(target->ptr, source->ptr, count * size);
  }
  _gopp_each((uint8_t *)source->ptr + count * size, source->len - count, size, drop);
  free(source->backing);
  return count;
}
