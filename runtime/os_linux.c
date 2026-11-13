// What the runtime asks of Linux.

#include "internal.h"

// The header of the compiler for the unwinder that every program is
// linked with.
#include <unwind.h>

long write(int file, const void *bytes, unsigned long count);
int *__errno_location(void);

enum { GOPP_STDERR = 2, GOPP_EINTR = 4 };

void _gopp_os_init(void) {}

void _gopp_os_write(const uint8_t *bytes, uint64_t count) {
  while (count > 0) {
    long written = write(GOPP_STDERR, bytes, count);
    if (written < 0) {
      if (*__errno_location() == GOPP_EINTR) {
        continue;
      }
      // The stream is closed: there is nobody to tell.
      return;
    }
    bytes += written;
    count -= (uint64_t)written;
  }
}

static _Unwind_Reason_Code visit(struct _Unwind_Context *context, void *each) {
  uintptr_t at = _Unwind_GetIP(context);
  (*(gopp_frame_fn *)each)(_Unwind_GetRegionStart(context), at);
  return _URC_NO_REASON;
}

void _gopp_os_frames(gopp_frame_fn each) { _Unwind_Backtrace(visit, &each); }

_Static_assert(sizeof(struct _Unwind_Exception) <= GOPP_UNWIND_ROOM, "the room of an unwinding");

// The unwinder asks before each function whether to go on. It always
// does: the function that stops the unwinding takes it over itself.
static _Unwind_Reason_Code go_on(int version, _Unwind_Action actions, _Unwind_Exception_Class class,
                                 struct _Unwind_Exception *exception,
                                 struct _Unwind_Context *context, void *nothing) {
  (void)version, (void)class, (void)exception, (void)context, (void)nothing;
  // The stack ended, and no function went on.
  if ((actions & _UA_END_OF_STACK) != 0) {
    _gopp_panic_drop();
  }
  return _URC_NO_REASON;
}

void _gopp_os_unwind(uint8_t *room) {
  struct _Unwind_Exception *exception = (struct _Unwind_Exception *)room;
  *exception = (struct _Unwind_Exception){0};
  _Unwind_ForcedUnwind(exception, go_on, 0);
  _gopp_panic_drop();
}
