// What the files of the runtime share. Each of them names what it needs
// of the system itself: no header of the C library is included, so the
// runtime compiles for a target without its C library.

#ifndef GOPP_INTERNAL_H
#define GOPP_INTERNAL_H

#include "gopp_rt.h"

// The system: one file for each. Output goes to the standard error
// stream, as it is, at once.
void _gopp_os_init(void);
void _gopp_os_write(const uint8_t *bytes, uint64_t count);

// Gives `each` the frames of the stack, the innermost first: where the
// function of the frame starts, and where the frame is in it.
typedef void (*gopp_frame_fn)(uintptr_t start, uintptr_t at);
void _gopp_os_frames(gopp_frame_fn each);

// Values as the built-in `print` of Go writes them.
void _gopp_write_text(const char *text);
void _gopp_write_string(gopp_string text);
void _gopp_write_uint(uint64_t value);
void _gopp_write_int(int64_t value);
void _gopp_write_hex(uint64_t value);

// The name of the Go++ function that starts at the address; null for
// what is no Go++ function.
const char *_gopp_func_name(uintptr_t start);

#endif
