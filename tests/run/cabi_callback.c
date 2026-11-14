// The C side of cabi_callback.gopp: functions that call the function
// that they are given.
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
  int32_t a, b;
} Small;
typedef struct {
  int64_t a, b, c;
} Large;

int32_t c_each(int32_t count, int32_t (*call)(int32_t at)) {
  int32_t sum = 0;
  for (int32_t at = 0; at < count; at++) sum += call(at);
  return sum;
}

int32_t c_triple(int32_t x) { return 3 * x; }

double c_fold(const double *xs, int64_t count, double (*step)(double sum, double x)) {
  double sum = 0;
  for (int64_t at = 0; at < count; at++) sum = step(sum, xs[at]);
  return sum;
}

// A struct of the program has the layout of the struct here and a name
// of its own, so the check of Clang that a function has the type of
// the pointer that it is called through takes it for another type.
#define OWN_NAMES __attribute__((no_sanitize("function")))

OWN_NAMES Small c_twice(Small v, Small (*turn)(Small v)) { return turn(turn(v)); }

OWN_NAMES Large c_grow(Large v, int8_t by, Large (*grow)(Large v, int8_t by, bool up)) {
  return grow(grow(v, by, true), (int8_t)(by + 1), false);
}

void c_visit(int32_t *xs, int64_t count, void (*visit)(int32_t *at)) {
  for (int64_t at = 0; at < count; at++) visit(&xs[at]);
}

static int32_t (*order)(int32_t *a, int32_t *b);

static int compare(const void *a, const void *b) { return order((int32_t *)a, (int32_t *)b); }

void c_sort(int32_t *xs, int64_t count, int32_t (*by)(int32_t *a, int32_t *b)) {
  order = by;
  qsort(xs, (size_t)count, sizeof *xs, compare);
}
