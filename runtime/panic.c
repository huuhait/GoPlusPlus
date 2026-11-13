// Where a program ends that cannot go on: the message of Go, the Go++
// functions of the stack, and status 2.

#include "internal.h"

static void frame(uintptr_t start, uintptr_t at) {
  const char *name = _gopp_func_name(start);
  // What is no Go++ function is no part of the trace: the runtime
  // itself, and what started the program.
  if (!name) {
    return;
  }
  _gopp_write_text(name);
  _gopp_write_text("()\n\t+");
  _gopp_write_hex(at - start);
  _gopp_write_text("\n");
}

// Ends the line of the message and the program.
[[noreturn]] static void die(void) {
  _gopp_write_text("\n\ngoroutine 1 [running]:\n");
  _gopp_os_frames(frame);
  _gopp_rt_exit(2);
}

[[noreturn]] void _gopp_fail(const char *message) {
  _gopp_write_text("panic: ");
  _gopp_write_text(message);
  die();
}

[[noreturn]] static void runtime_error(const char *message) {
  _gopp_write_text("panic: runtime error: ");
  _gopp_write_text(message);
  die();
}

[[noreturn]] void _gopp_fail_type(const char *message, const gopp_type *type) {
  _gopp_write_text("panic: runtime error: ");
  _gopp_write_text(message);
  _gopp_write_string(type->name);
  die();
}

[[noreturn]] void _gopp_fail_assert(gopp_string iface, const gopp_type *have,
                                    const gopp_type *wanted) {
  _gopp_write_text("panic: interface conversion: ");
  _gopp_write_string(iface);
  _gopp_write_text(" is ");
  if (have == 0) {
    _gopp_write_text("nil");
  } else {
    _gopp_write_string(have->name);
  }
  _gopp_write_text(", not ");
  _gopp_write_string(wanted->name);
  die();
}

// A failed check of an index or of the bounds of a slice. `%x` in the
// form is `x`, which is signed if `is_signed` says so, and `%y` is `y`,
// which is always signed; a negative `x` has the form without `y`.
[[noreturn]] static void bounds(const char *form, const char *negative, uint64_t x, uint64_t y,
                                bool is_signed) {
  if (is_signed && (int64_t)x < 0) {
    form = negative;
  }
  _gopp_write_text("panic: runtime error: ");
  for (const char *at = form; *at; at++) {
    if (*at != '%') {
      _gopp_os_write((const uint8_t *)at, 1);
      continue;
    }
    at++;
    if (*at == 'y') {
      _gopp_write_int((int64_t)y);
    } else if (is_signed) {
      _gopp_write_int((int64_t)x);
    } else {
      _gopp_write_uint(x);
    }
  }
  die();
}

void _gopp_panic_bounds(uint64_t index, uint64_t length, bool is_signed) {
  bounds("index out of range [%x] with length %y", "index out of range [%x]", index, length,
         is_signed);
}

void _gopp_panic_slice_alen(uint64_t index, uint64_t length, bool is_signed) {
  bounds("slice bounds out of range [:%x] with length %y", "slice bounds out of range [:%x]",
         index, length, is_signed);
}

void _gopp_panic_slice_acap(uint64_t index, uint64_t capacity, bool is_signed) {
  bounds("slice bounds out of range [:%x] with capacity %y", "slice bounds out of range [:%x]",
         index, capacity, is_signed);
}

void _gopp_panic_slice_b(uint64_t low, uint64_t high, bool is_signed) {
  bounds("slice bounds out of range [%x:%y]", "slice bounds out of range [%x:]", low, high,
         is_signed);
}

void _gopp_panic_slice3_alen(uint64_t index, uint64_t length, bool is_signed) {
  bounds("slice bounds out of range [::%x] with length %y", "slice bounds out of range [::%x]",
         index, length, is_signed);
}

void _gopp_panic_slice3_acap(uint64_t index, uint64_t capacity, bool is_signed) {
  bounds("slice bounds out of range [::%x] with capacity %y", "slice bounds out of range [::%x]",
         index, capacity, is_signed);
}

void _gopp_panic_slice3_b(uint64_t high, uint64_t max, bool is_signed) {
  bounds("slice bounds out of range [:%x:%y]", "slice bounds out of range [:%x:]", high, max,
         is_signed);
}

void _gopp_panic_slice3_c(uint64_t low, uint64_t high, bool is_signed) {
  bounds("slice bounds out of range [%x:%y:]", "slice bounds out of range [%x::]", low, high,
         is_signed);
}

void _gopp_panic_slice_convert(uint64_t wanted, uint64_t length, bool is_signed) {
  const char *form = "cannot convert slice with length %y to array or pointer to array with length %x";
  bounds(form, form, wanted, length, is_signed);
}

void _gopp_panic_nil(void) { runtime_error("invalid memory address or nil pointer dereference"); }

void _gopp_panic_divide(void) { runtime_error("integer divide by zero"); }

void _gopp_panic_shift(void) { runtime_error("negative shift amount"); }

void _gopp_panic_must(gopp_string expr, gopp_string position, gopp_string message) {
  _gopp_write_text("panic: must: ");
  _gopp_write_string(expr);
  _gopp_write_text(" at ");
  _gopp_write_string(position);
  _gopp_write_text(": ");
  _gopp_write_string(message);
  die();
}
