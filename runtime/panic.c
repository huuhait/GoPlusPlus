// Panics: the functions with `defer` statements that have not returned,
// the deferred calls that a panic makes, `recover`, and where a program
// ends that nothing stopped: the message of Go, the Go++ functions of
// the stack, and status 2.

#include "internal.h"

void free(void *room);

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

// The functions that have `defer` statements and have not returned, the
// innermost first: each with the function that makes its deferred calls
// and with where it goes on if one of them stops a panic.
static gopp_frame *frames;

// A panic that has not ended: its value, the function whose deferred
// calls it runs now, and whether one of them stopped it. A panic in a
// deferred call comes on top of the one that the call ran for.
typedef struct gopp_panicking {
  struct gopp_panicking *below;
  gopp_iface value;
  gopp_frame *at;
  bool recovered;
  // It has the value of the panic below it, which was stopped: the
  // program raised that panic again.
  bool again;
  // The keys of `Error() string` and `String() string`, by which the
  // value says its text.
  gopp_string error_key;
  gopp_string string_key;
} gopp_panicking;

static gopp_panicking *panics;

// A panic that was stopped and now leaves the functions between where
// it started and the function that goes on: each of them ends what it
// owns. One that starts while a value ends comes on top.
typedef struct gopp_unwinding {
  struct gopp_unwinding *outer;
  // The function that goes on.
  gopp_frame *target;
  // The list of frames when the unwinding started: a panic that is
  // raised while a value ends must be stopped before it gets there.
  gopp_frame *limit;
  // What the unwinder of the system keeps of the unwinding.
  _Alignas(16) uint8_t room[GOPP_UNWIND_ROOM];
} gopp_unwinding;

static gopp_unwinding *unwindings;

// The trace of the calls with `!` that failed (ERR-14): where each was,
// the first that failed first, for the error that the last of them
// passed on. It is no part of the error, and at most this long: the
// oldest places are dropped.
enum { GOPP_TRACE = 32 };
static gopp_string trace[GOPP_TRACE];
static uint32_t traced;
static gopp_iface trace_of;

// A call with `!` at the place failed with the error of the two
// words. If the error is the one that the last such call passed on,
// the place is one more of its trace; if not, the trace starts over
// with it.
void _gopp_err_trace(uint8_t *itab, uint8_t *data, gopp_string position) {
  if (itab != trace_of.itab || data != trace_of.data) {
    traced = 0;
    trace_of = (gopp_iface){itab, data};
  }
  if (traced == GOPP_TRACE) {
    __builtin_memmove(trace, trace + 1, (GOPP_TRACE - 1) * sizeof trace[0]);
    traced--;
  }
  trace[traced++] = position;
}

// A panic ends the program at once: no deferred call is made for it
// (`-panic=abort`).
static bool aborts;

void _gopp_panic_aborts(void) { aborts = true; }

void _gopp_panic_reset(void) {
  frames = 0;
  panics = 0;
  unwindings = 0;
  aborts = false;
  traced = 0;
  trace_of = (gopp_iface){0, 0};
}

// A panic left the end of a value (`drop`): nothing can go on.
[[noreturn]] void _gopp_panic_drop(void) {
  _gopp_write_text("fatal error: a panic left the end of a value");
  die();
}

// A function with a frame ended what it owns but its results, because
// a panic left one of its calls: whether it is the function that goes
// on, which keeps its results for its caller. The unwinding is over
// then. Where the unwinder of the system ran this for the function,
// the function goes on from here, and this does not come back.
bool _gopp_defer_left(uint8_t *frame) {
  gopp_unwinding *unwinding = unwindings;
  if (unwinding == 0 || unwinding->target != (gopp_frame *)frame) {
    return false;
  }
  unwindings = unwinding->outer;
  free(unwinding);
  _gopp_os_resume((gopp_frame *)frame);
  return true;
}

// A function with a frame says where the record of what it owns is.
// Nothing is done with it: what is known outside the function is in
// memory at every call, where a panic may leave the function, and the
// function reads it there when it goes on.
void _gopp_defer_owns(uint8_t *owned) { (void)owned; }

// Whether a panic is making the deferred calls of the innermost
// function that has any, and none of them has stopped it.
bool _gopp_panicking(void) {
  return panics != 0 && !panics->recovered && panics->at != 0 && panics->at == frames;
}

void _gopp_defer_push(uint8_t *frame) {
  gopp_frame *pushed = (gopp_frame *)frame;
  pushed->next = frames;
  frames = pushed;
}

void _gopp_defer_pop(uint8_t *frame) { frames = ((gopp_frame *)frame)->next; }

// Generated code has this as a macro, which also marks where the
// function goes on after a panic was stopped.
#undef _gopp_defer_enter
bool _gopp_defer_enter(uint8_t *frame) {
  _gopp_defer_push(frame);
  return false;
}

// What a method of the value says, if the type has the method of the
// key.
static bool write_said(gopp_iface *value, const gopp_type *type, gopp_string key) {
  uint8_t *code = key.len == 0 ? 0 : _gopp_method_of(type, key);
  if (code == 0) {
    return false;
  }
  gopp_string text = ((gopp_text_fn)code)((uint8_t *)&value->data);
  _gopp_write_string(text);
  _gopp_string_release(text);
  return true;
}

// The value of a panic as Go writes it: what an error or a `String`
// method says, a value of a basic type as `print` writes it, with its
// type around it if the type has a name, and the type and an address
// for any other value.
static void write_value(gopp_panicking *panic) {
  gopp_iface *value = &panic->value;
  if (value->itab == 0) {
    _gopp_write_text("nil");
    return;
  }
  const gopp_type *type = *(const gopp_type *const *)value->itab;
  if (write_said(value, type, panic->error_key) || write_said(value, type, panic->string_key)) {
    return;
  }
  bool in_word = (type->flags & GOPP_TYPE_INLINE) != 0;
  const uint8_t *held = in_word ? (const uint8_t *)&value->data : value->data;
  bool named = (type->flags & GOPP_TYPE_NAMED) != 0;
  if (type->kind == GOPP_KIND_OTHER) {
    _gopp_write_text("(");
    _gopp_write_string(type->name);
    _gopp_write_text(") 0x");
    _gopp_write_hex((uint64_t)(uintptr_t)value->data);
    return;
  }
  if (named) {
    _gopp_write_string(type->name);
    _gopp_write_text(type->kind == GOPP_KIND_STRING ? "(\"" : "(");
  }
  uint64_t bits = 0;
  if (type->kind != GOPP_KIND_STRING) {
    __builtin_memcpy(&bits, held, type->size);
  }
  switch (type->kind) {
  case GOPP_KIND_BOOL:
    _gopp_write_text(bits != 0 ? "true" : "false");
    break;
  case GOPP_KIND_INT: {
    unsigned shift = 64 - 8 * (unsigned)type->size;
    _gopp_write_int((int64_t)(bits << shift) >> shift);
    break;
  }
  case GOPP_KIND_UINT:
    _gopp_write_uint(bits);
    break;
  case GOPP_KIND_FLOAT:
    if (type->size == 4) {
      float small;
      __builtin_memcpy(&small, held, sizeof small);
      _gopp_print_float(small);
    } else {
      double wide;
      __builtin_memcpy(&wide, held, sizeof wide);
      _gopp_print_float(wide);
    }
    break;
  default:
    _gopp_write_string(*(const gopp_string *)held);
    break;
  }
  if (named) {
    _gopp_write_text(type->kind == GOPP_KIND_STRING ? "\")" : ")");
  }
}

// The panics that have not ended, the oldest first, as Go writes them.
// A panic that was raised again is written once.
static void write_panics(gopp_panicking *panic, bool newest) {
  gopp_panicking *below = panic->again ? panic->below->below : panic->below;
  if (below != 0) {
    write_panics(below, false);
    _gopp_write_text("\tpanic: ");
  } else {
    _gopp_write_text("panic: ");
  }
  write_value(panic);
  if (panic->again) {
    _gopp_write_text(" [recovered, repanicked]");
  } else if (panic->recovered) {
    _gopp_write_text(" [recovered]");
  }
  if (!newest) {
    _gopp_write_text("\n");
  }
}

// Whether the two values are one value. Values of a type that has no
// `==` are not.
static bool same(gopp_iface *a, gopp_iface *b) {
  if (a->itab == 0 || a->itab != b->itab) {
    return false;
  }
  const gopp_type *type = *(const gopp_type *const *)a->itab;
  return (type->flags & GOPP_TYPE_COMPARABLE) != 0 && _gopp_iface_eq((uint8_t *)a, (uint8_t *)b);
}

static bool listed(const gopp_frame *frame) {
  for (const gopp_frame *at = frames; at != 0; at = at->next) {
    if (at == frame) {
      return true;
    }
  }
  return false;
}

// The panic runs the deferred calls of every function that has not
// returned, the innermost first and on top of the stack as it is, as in
// Go. If one of the calls stops the panic, the function whose call it
// was goes on where it marked, and returns to its caller. If none
// does, the panics are written and the program ends.
[[noreturn]] static void raise(gopp_panicking *panic) {
  panic->below = panics;
  panic->again = panics != 0 && panics->recovered && same(&panics->value, &panic->value);
  panics = panic;
  if (aborts) {
    write_panics(panic, true);
    die();
  }
  for (;;) {
    // A panic that started while a value ended, where another panic
    // left a function, and that nothing stopped before its end.
    if (unwindings != 0 && frames == unwindings->limit) {
      write_panics(panic, true);
      _gopp_write_text("\n");
      _gopp_panic_drop();
    }
    if (frames == 0) {
      break;
    }
    gopp_frame *frame = frames;
    panic->at = frame;
    frame->run(frame->record);
    frames = frame->next;
    if (!panic->recovered) {
      continue;
    }
    // An older panic is over too if the call that it ran was left
    // behind: its function is this one or one that this one called,
    // which is in the list no more.
    gopp_panicking *rest = panic->below;
    _gopp_iface_drop((uint8_t *)&panic->value);
    free(panic);
    while (rest != 0 && !listed(rest->at)) {
      gopp_panicking *below = rest->below;
      _gopp_iface_drop((uint8_t *)&rest->value);
      free(rest);
      rest = below;
    }
    panics = rest;
    // Every function between here and the one that goes on ends what
    // it owns while the stack is unwound, and so does that one, which
    // asks `_gopp_defer_left` last and goes on where it marked.
    gopp_unwinding *unwinding = _gopp_alloc(sizeof *unwinding);
    *unwinding = (gopp_unwinding){.outer = unwindings, .target = frame, .limit = frames};
    unwindings = unwinding;
    _gopp_os_unwind(unwinding->room, frame);
  }
  write_panics(panic, true);
  die();
}

void _gopp_panic(uint8_t *value, gopp_string error_key, gopp_string string_key) {
  // `recover` gives nil where no panic runs, so no panic has nil.
  if (((gopp_iface *)value)->itab == 0) {
    _gopp_fail("panic called with nil argument");
  }
  gopp_panicking *panic = _gopp_alloc(sizeof *panic);
  *panic = (gopp_panicking){.value = *(gopp_iface *)value,
                            .error_key = error_key,
                            .string_key = string_key};
  raise(panic);
}

// `recover()`: the value of the panic whose deferred calls run, which
// is stopped; nil if no panic runs the deferred calls of the innermost
// function that has any.
void _gopp_recover(uint8_t *value) {
  gopp_iface *to = (gopp_iface *)value;
  *to = (gopp_iface){0, 0};
  gopp_panicking *panic = panics;
  if (panic != 0 && !panic->recovered && panic->at != 0 && panic->at == frames) {
    // The panic keeps its value: it is written if a later panic ends
    // the program.
    *to = panic->value;
    _gopp_iface_clone(value);
    panic->recovered = true;
  }
}

static void text_clone(uint8_t *value) { _gopp_string_retain(*(gopp_string *)value); }

static void text_drop(uint8_t *value) { _gopp_string_release(*(gopp_string *)value); }

static bool text_eq(uint8_t *a, uint8_t *b) {
  return _gopp_string_eq(*(gopp_string *)a, *(gopp_string *)b);
}

// The type of the value of a panic that the runtime starts: its text.
static const gopp_type runtime_error = {
    .type = &runtime_error,
    .size = sizeof(gopp_string),
    .flags = GOPP_TYPE_COMPARABLE,
    .clone = text_clone,
    .drop = text_drop,
    .eq = text_eq,
    .name = {(uint8_t *)"runtime.Error", 13, 0},
    .kind = GOPP_KIND_STRING,
};

// A panic with what was written since the message was started.
[[noreturn]] static void raise_kept(void) {
  gopp_string text = _gopp_keep_end();
  gopp_panicking *panic = _gopp_alloc(sizeof *panic);
  *panic = (gopp_panicking){
      .value = {(uint8_t *)&runtime_error, _gopp_iface_box(&text, sizeof text)}};
  raise(panic);
}

[[noreturn]] void _gopp_fail(const char *message) {
  _gopp_keep_start();
  _gopp_write_text(message);
  raise_kept();
}

[[noreturn]] static void runtime_error_of(const char *message) {
  _gopp_keep_start();
  _gopp_write_text("runtime error: ");
  _gopp_write_text(message);
  raise_kept();
}

[[noreturn]] void _gopp_fail_type(const char *message, const gopp_type *type) {
  _gopp_keep_start();
  _gopp_write_text("runtime error: ");
  _gopp_write_text(message);
  _gopp_write_string(type->name);
  raise_kept();
}

[[noreturn]] void _gopp_fail_assert(gopp_string iface, const gopp_type *have,
                                    const gopp_type *wanted) {
  _gopp_keep_start();
  _gopp_write_text("interface conversion: ");
  _gopp_write_string(iface);
  _gopp_write_text(" is ");
  if (have == 0) {
    _gopp_write_text("nil");
  } else {
    _gopp_write_string(have->name);
  }
  _gopp_write_text(", not ");
  _gopp_write_string(wanted->name);
  raise_kept();
}

[[noreturn]] void _gopp_fail_convert(gopp_string iface, const gopp_type *have,
                                     gopp_string missing) {
  _gopp_keep_start();
  _gopp_write_text("interface conversion: ");
  if (have == 0) {
    _gopp_write_text("interface is nil, not ");
    _gopp_write_string(iface);
    raise_kept();
  }
  _gopp_write_string(have->name);
  _gopp_write_text(" is not ");
  _gopp_write_string(iface);
  _gopp_write_text(": missing method ");
  _gopp_write_string(missing);
  raise_kept();
}

// A failed check of an index or of the bounds of a slice. `%x` in the
// form is `x`, which is signed if `is_signed` says so, and `%y` is `y`,
// which is always signed; a negative `x` has the form without `y`.
[[noreturn]] static void bounds(const char *form, const char *negative, uint64_t x, uint64_t y,
                                bool is_signed) {
  if (is_signed && (int64_t)x < 0) {
    form = negative;
  }
  _gopp_keep_start();
  _gopp_write_text("runtime error: ");
  for (const char *at = form; *at; at++) {
    if (*at != '%') {
      _gopp_write_string((gopp_string){(uint8_t *)at, 1, 0});
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
  raise_kept();
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

void _gopp_panic_nil(void) { runtime_error_of("invalid memory address or nil pointer dereference"); }

void _gopp_panic_divide(void) { runtime_error_of("integer divide by zero"); }

void _gopp_panic_shift(void) { runtime_error_of("negative shift amount"); }

void _gopp_panic_must(gopp_string expr, gopp_string position, gopp_string message) {
  _gopp_keep_start();
  _gopp_write_text("must: ");
  _gopp_write_string(expr);
  _gopp_write_text(" at ");
  _gopp_write_string(position);
  _gopp_write_text(": ");
  _gopp_write_string(message);
  // The message was made for this, and is in the text of the panic.
  _gopp_string_release(message);
  // The places of the calls that passed the error on before this one,
  // which is the last of the trace and is named above.
  for (uint32_t at = 0; at + 1 < traced; at++) {
    _gopp_write_text("\n\t");
    _gopp_write_string(trace[at]);
  }
  raise_kept();
}
