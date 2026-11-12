// Strings: bytes that never change. A string that the program makes
// while it runs has its bytes in a buffer of its own, after a header
// with a count of the strings that share them and with the room that
// the buffer has; a NUL follows the bytes.

#include "internal.h"

void *malloc(__SIZE_TYPE__ size);

typedef struct gopp_string_buffer {
  uint64_t refcount;
  uint64_t byte_cap;
} gopp_string_buffer;

[[noreturn]] static void fatal(const char *message) {
  _gopp_write_text("fatal error: ");
  _gopp_write_text(message);
  _gopp_write_text("\n");
  _gopp_rt_exit(2);
}

void *_gopp_alloc(uint64_t size) {
  void *room = size <= (__SIZE_MAX__ >> 1) ? malloc((__SIZE_TYPE__)size) : 0;
  if (!room) {
    fatal("runtime: out of memory");
  }
  return room;
}

// A string of `len` bytes in a new buffer; the bytes are not set yet.
static gopp_string fresh(uint64_t len) {
  gopp_string_buffer *buffer = _gopp_alloc(sizeof *buffer + len + 1);
  buffer->refcount = 1;
  buffer->byte_cap = len;
  uint8_t *bytes = (uint8_t *)(buffer + 1);
  bytes[len] = 0;
  return (gopp_string){bytes, len, (uint8_t *)buffer};
}

gopp_string _gopp_string_concat(gopp_string a, gopp_string b) {
  uint64_t len = a.len + b.len;
  if (len < a.len) {
    fatal("runtime: string concatenation too long");
  }
  if (len == 0) {
    return (gopp_string){0, 0, 0};
  }
  gopp_string joined = fresh(len);
  if (a.len > 0) {
    __builtin_memcpy(joined.ptr, a.ptr, a.len);
  }
  if (b.len > 0) {
    __builtin_memcpy(joined.ptr + a.len, b.ptr, b.len);
  }
  return joined;
}

bool _gopp_string_eq(gopp_string a, gopp_string b) {
  if (a.len != b.len) {
    return false;
  }
  return a.len == 0 || a.ptr == b.ptr || __builtin_memcmp(a.ptr, b.ptr, a.len) == 0;
}

// Below zero, zero or above: `a` comes before `b`, is `b`, or comes
// after it, byte by byte.
int64_t _gopp_string_compare(gopp_string a, gopp_string b) {
  uint64_t shared = a.len < b.len ? a.len : b.len;
  int order = shared > 0 ? __builtin_memcmp(a.ptr, b.ptr, shared) : 0;
  if (order != 0) {
    return order;
  }
  return (a.len > b.len) - (a.len < b.len);
}

enum { GOPP_RUNE_ERROR = 0xfffd };

// `string(r)`: the UTF-8 of the code point, or of U+FFFD for a number
// that is none.
gopp_string _gopp_string_from_rune(int64_t rune) {
  if (rune < 0 || rune > 0x10ffff || (rune >= 0xd800 && rune <= 0xdfff)) {
    rune = GOPP_RUNE_ERROR;
  }
  uint32_t point = (uint32_t)rune;
  uint64_t len = point < 0x80 ? 1 : point < 0x800 ? 2 : point < 0x10000 ? 3 : 4;
  gopp_string text = fresh(len);
  switch (len) {
  case 1:
    text.ptr[0] = (uint8_t)point;
    break;
  case 2:
    text.ptr[0] = (uint8_t)(0xc0 | point >> 6);
    text.ptr[1] = (uint8_t)(0x80 | (point & 0x3f));
    break;
  case 3:
    text.ptr[0] = (uint8_t)(0xe0 | point >> 12);
    text.ptr[1] = (uint8_t)(0x80 | (point >> 6 & 0x3f));
    text.ptr[2] = (uint8_t)(0x80 | (point & 0x3f));
    break;
  default:
    text.ptr[0] = (uint8_t)(0xf0 | point >> 18);
    text.ptr[1] = (uint8_t)(0x80 | (point >> 12 & 0x3f));
    text.ptr[2] = (uint8_t)(0x80 | (point >> 6 & 0x3f));
    text.ptr[3] = (uint8_t)(0x80 | (point & 0x3f));
  }
  return text;
}

// The code point that starts at byte `at`, which is in the string, in
// the low half, and how many bytes it takes in the high half. Bytes
// that are no UTF-8 give U+FFFD and one byte, as `range` of Go does.
uint64_t _gopp_string_decode(gopp_string text, uint64_t at) {
  const uint8_t *bytes = text.ptr + at;
  uint64_t left = text.len - at;
  uint32_t first = bytes[0];
  if (first < 0x80) {
    return (uint64_t)1 << 32 | first;
  }
  const uint64_t invalid = (uint64_t)1 << 32 | GOPP_RUNE_ERROR;
  // How many bytes the first one asks for, and the range of the second.
  uint64_t width;
  uint32_t low = 0x80, high = 0xbf, point;
  if (first >= 0xc2 && first <= 0xdf) {
    width = 2;
    point = first & 0x1f;
  } else if (first >= 0xe0 && first <= 0xef) {
    width = 3;
    point = first & 0x0f;
    low = first == 0xe0 ? 0xa0 : 0x80;
    high = first == 0xed ? 0x9f : 0xbf;
  } else if (first >= 0xf0 && first <= 0xf4) {
    width = 4;
    point = first & 0x07;
    low = first == 0xf0 ? 0x90 : 0x80;
    high = first == 0xf4 ? 0x8f : 0xbf;
  } else {
    return invalid;
  }
  if (left < width || bytes[1] < low || bytes[1] > high) {
    return invalid;
  }
  point = point << 6 | (bytes[1] & 0x3f);
  for (uint64_t next = 2; next < width; next++) {
    if (bytes[next] < 0x80 || bytes[next] > 0xbf) {
      return invalid;
    }
    point = point << 6 | (bytes[next] & 0x3f);
  }
  return width << 32 | point;
}
