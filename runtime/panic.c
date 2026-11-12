// Where a failed check of generated code ends: the program stops with
// status 2.

#include "gopp_rt.h"

void _gopp_panic_bounds(uint64_t index, uint64_t length) {
  (void)index;
  (void)length;
  _gopp_rt_exit(2);
}

void _gopp_panic_slice_alen(uint64_t index, uint64_t length) {
  (void)index;
  (void)length;
  _gopp_rt_exit(2);
}

void _gopp_panic_slice_acap(uint64_t index, uint64_t capacity) {
  (void)index;
  (void)capacity;
  _gopp_rt_exit(2);
}

void _gopp_panic_slice_b(uint64_t low, uint64_t high) {
  (void)low;
  (void)high;
  _gopp_rt_exit(2);
}

void _gopp_panic_slice3_alen(uint64_t index, uint64_t length) {
  (void)index;
  (void)length;
  _gopp_rt_exit(2);
}

void _gopp_panic_slice3_acap(uint64_t index, uint64_t capacity) {
  (void)index;
  (void)capacity;
  _gopp_rt_exit(2);
}

void _gopp_panic_slice3_b(uint64_t high, uint64_t max) {
  (void)high;
  (void)max;
  _gopp_rt_exit(2);
}

void _gopp_panic_slice3_c(uint64_t low, uint64_t high) {
  (void)low;
  (void)high;
  _gopp_rt_exit(2);
}

void _gopp_panic_slice_convert(uint64_t length, uint64_t wanted) {
  (void)length;
  (void)wanted;
  _gopp_rt_exit(2);
}

void _gopp_panic_nil(void) { _gopp_rt_exit(2); }

void _gopp_panic_divide(void) { _gopp_rt_exit(2); }

void _gopp_panic_shift(void) { _gopp_rt_exit(2); }
