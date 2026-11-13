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

// Ends the program as a panic with the message does: the message, the
// Go++ functions of the stack, and status 2.
[[noreturn]] void _gopp_fail(const char *message);

// Ends the program as a runtime error about a type does: the message
// and the name of the type.
[[noreturn]] void _gopp_fail_type(const char *message, const gopp_type *type);

// Ends the program as `x.(T)` does for an `x` of the interface type
// `iface` that holds a value of the type `have`, or nothing for null,
// and no `wanted`.
[[noreturn]] void _gopp_fail_assert(gopp_string iface, const gopp_type *have,
                                    const gopp_type *wanted);

// Ends the program as `x.(I)` does for an interface type named `iface`
// and an `x` that holds a value of the type `have` without the method
// `missing`, or nothing for null.
[[noreturn]] void _gopp_fail_convert(gopp_string iface, const gopp_type *have,
                                     gopp_string missing);

// The tables of methods that the program before made are gone with it.
void _gopp_iface_reset(void);

// Room for `size` bytes, which is never null: a program that gets none
// ends.
void *_gopp_alloc(uint64_t size);

// The name of the Go++ function that starts at the address; null for
// what is no Go++ function.
const char *_gopp_func_name(uintptr_t start);

#endif
