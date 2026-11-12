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
