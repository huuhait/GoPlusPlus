// The built-in `print` and `println`: each value as the runtime of Go
// writes it, byte for byte.

#include "internal.h"

void *realloc(void *room, __SIZE_TYPE__ size);
void free(void *room);

// While a message is kept, what is written goes into it and not to the
// standard error stream: a panic has its message before anything is
// written, because the calls that it runs may write first.
static bool keeping;
static uint8_t *kept;
static uint64_t kept_len;
static uint64_t kept_room;

static void out(const uint8_t *bytes, uint64_t count) {
  if (!keeping) {
    _gopp_os_write(bytes, count);
    return;
  }
  if (kept_len + count > kept_room) {
    kept_room = (kept_len + count) * 2;
    kept = realloc(kept, kept_room);
    if (kept == 0) {
      _gopp_rt_exit(2);
    }
  }
  if (count > 0) {
    __builtin_memcpy(kept + kept_len, bytes, count);
  }
  kept_len += count;
}

void _gopp_keep_start(void) {
  keeping = true;
  kept = 0;
  kept_len = 0;
  kept_room = 0;
}

// What was written since the start, as a string of its own.
gopp_string _gopp_keep_end(void) {
  keeping = false;
  gopp_string text = _gopp_string_concat((gopp_string){kept, kept_len, 0}, (gopp_string){0, 0, 0});
  free(kept);
  kept = 0;
  return text;
}

void _gopp_write_text(const char *text) {
  uint64_t count = 0;
  while (text[count]) {
    count++;
  }
  out((const uint8_t *)text, count);
}

void _gopp_write_string(gopp_string text) { out(text.ptr, text.len); }

void _gopp_write_uint(uint64_t value) {
  uint8_t digits[20];
  uint64_t at = sizeof digits;
  do {
    digits[--at] = (uint8_t)('0' + value % 10);
    value /= 10;
  } while (value > 0);
  out(digits + at, sizeof digits - at);
}

void _gopp_write_int(int64_t value) {
  if (value < 0) {
    _gopp_write_text("-");
    // The least value has no positive of its type.
    _gopp_write_uint(0 - (uint64_t)value);
    return;
  }
  _gopp_write_uint((uint64_t)value);
}

void _gopp_write_hex(uint64_t value) {
  uint8_t digits[18];
  uint64_t at = sizeof digits;
  do {
    digits[--at] = (uint8_t) "0123456789abcdef"[value % 16];
    value /= 16;
  } while (value > 0);
  digits[--at] = 'x';
  digits[--at] = '0';
  out(digits + at, sizeof digits - at);
}

// The steps of `printfloat` of Go, one for one: seven digits, which are
// not always the nearest ones, and that is what a program of Go prints.
static void write_float(double value) {
  if (value != value) {
    _gopp_write_text("NaN");
    return;
  }
  if (value + value == value && value > 0) {
    _gopp_write_text("+Inf");
    return;
  }
  if (value + value == value && value < 0) {
    _gopp_write_text("-Inf");
    return;
  }
  enum { DIGITS = 7 };
  uint8_t text[DIGITS + 7];
  text[0] = '+';
  int exponent = 0;
  if (value == 0) {
    if (1 / value < 0) {
      text[0] = '-';
    }
  } else {
    if (value < 0) {
      value = -value;
      text[0] = '-';
    }
    while (value >= 10) {
      exponent++;
      value /= 10;
    }
    while (value < 1) {
      exponent--;
      value *= 10;
    }
    double half = 5.0;
    for (int at = 0; at < DIGITS; at++) {
      half /= 10;
    }
    value += half;
    if (value >= 10) {
      exponent++;
      value /= 10;
    }
  }
  for (int at = 0; at < DIGITS; at++) {
    int digit = (int)value;
    text[at + 2] = (uint8_t)(digit + '0');
    value -= (double)digit;
    value *= 10;
  }
  text[1] = text[2];
  text[2] = '.';
  text[DIGITS + 2] = 'e';
  text[DIGITS + 3] = '+';
  if (exponent < 0) {
    exponent = -exponent;
    text[DIGITS + 3] = '-';
  }
  text[DIGITS + 4] = (uint8_t)(exponent / 100 + '0');
  text[DIGITS + 5] = (uint8_t)(exponent / 10 % 10 + '0');
  text[DIGITS + 6] = (uint8_t)(exponent % 10 + '0');
  out(text, sizeof text);
}

void _gopp_print_bool(bool value) { _gopp_write_text(value ? "true" : "false"); }

void _gopp_print_int(int64_t value) { _gopp_write_int(value); }

void _gopp_print_uint(uint64_t value) { _gopp_write_uint(value); }

void _gopp_print_float(double value) { write_float(value); }

void _gopp_print_string(gopp_string value) { _gopp_write_string(value); }

void _gopp_print_pointer(uint8_t *value) { _gopp_write_hex((uintptr_t)value); }

void _gopp_print_space(void) { _gopp_write_text(" "); }

void _gopp_print_newline(void) { _gopp_write_text("\n"); }
