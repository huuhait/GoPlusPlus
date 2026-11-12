// What the runtime asks of Windows.

#include "internal.h"

int _write(int file, const void *bytes, unsigned int count);
int _setmode(int file, int mode);
unsigned short RtlCaptureStackBackTrace(unsigned long skip, unsigned long count, void **frames,
                                        unsigned long *hash);

// The entry of a function in the tables for unwinding: where it starts
// and ends, from the start of its module.
typedef struct gopp_function_entry {
  uint32_t begin;
  uint32_t end;
  uint32_t unwind;
} gopp_function_entry;
gopp_function_entry *RtlLookupFunctionEntry(uint64_t at, uint64_t *base, void *history);

// The system gives at most 62 frames at once.
enum { GOPP_STDERR = 2, GOPP_BINARY = 0x8000, GOPP_FRAMES = 62, GOPP_AT_ONCE = 1 << 30 };

void _gopp_os_init(void) {
  // A newline is one byte, as a program of Go writes it.
  _setmode(GOPP_STDERR, GOPP_BINARY);
}

void _gopp_os_write(const uint8_t *bytes, uint64_t count) {
  while (count > 0) {
    unsigned int part = count < GOPP_AT_ONCE ? (unsigned int)count : GOPP_AT_ONCE;
    int written = _write(GOPP_STDERR, bytes, part);
    if (written <= 0) {
      // The stream is closed: there is nobody to tell.
      return;
    }
    bytes += written;
    count -= (uint64_t)written;
  }
}

void _gopp_os_frames(gopp_frame_fn each) {
  void *frames[GOPP_FRAMES];
  unsigned short count = RtlCaptureStackBackTrace(0, GOPP_FRAMES, frames, 0);
  for (unsigned short at = 0; at < count; at++) {
    uint64_t address = (uint64_t)frames[at];
    uint64_t base = 0;
    // A frame is at the address after its call, which may be past the
    // end of its function.
    gopp_function_entry *entry = RtlLookupFunctionEntry(address - 1, &base, 0);
    if (entry) {
      each(base + entry->begin, address);
    }
  }
}
