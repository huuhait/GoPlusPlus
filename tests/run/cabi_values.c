// The C side of cabi_values.gopp: each function changes what it is
// given in a way that shows every byte of it.
#include <stdbool.h>
#include <stdint.h>

int8_t c_i8(int8_t x) { return (int8_t)(x - 1); }
uint8_t c_u8(uint8_t x) { return (uint8_t)(x + 1); }
int16_t c_i16(int16_t x) { return (int16_t)-x; }
uint16_t c_u16(uint16_t x) { return (uint16_t)(x >> 1); }
int32_t c_i32(int32_t x) { return x * 3; }
uint32_t c_u32(uint32_t x) { return ~x; }
int64_t c_i64(int64_t x) { return x - 1; }
uint64_t c_u64(uint64_t x) { return x ^ 0xff; }
float c_f32(float x) { return x * 2; }
double c_f64(double x) { return x / 4; }
bool c_not(bool x) { return !x; }

// C reads a narrow value as an int: its upper bits must be those of
// its sign.
int64_t c_wide(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
  return (int64_t)a * 1000000 + b * 10000 + c * 100 + d + e;
}

int32_t *c_next(int32_t *at) {
  *at += 1;
  return at + 1;
}

int32_t c_or(const uint8_t *at, int32_t otherwise) { return at ? *at : otherwise; }

typedef struct {
  int32_t a, b;
} Small;
typedef struct {
  int64_t a;
  double b;
} Mixed;
typedef struct {
  float x, y, z, w;
} Quad;
typedef struct {
  double x, y;
} Two;
typedef struct {
  int64_t a, b, c;
} Large;
typedef struct {
  uint8_t bytes[3];
} Odd;
typedef struct {
  int8_t tag;
  Small in;
  int16_t tail[3];
  double d;
} Nested;
typedef struct {
  Mixed m;
  Quad *q;
  bool ok;
} Outer;

typedef struct {
  int8_t a;
  int64_t none[0];
  int8_t b;
} Gap;

Gap c_gap(Gap v) { return (Gap){v.b, {}, (int8_t)(v.a + (int8_t)sizeof v)}; }

Small c_small(Small v) { return (Small){v.b, v.a + 1}; }
Mixed c_mixed(Mixed v) { return (Mixed){v.a * 2, v.b + 0.5}; }
Quad c_quad(Quad v) { return (Quad){v.w, v.z, v.y, v.x + 1}; }
Two c_two(Two v) { return (Two){v.y, -v.x}; }
Large c_large(Large v) { return (Large){v.c, v.a, v.b + 1}; }
Odd c_odd(Odd v) { return (Odd){{v.bytes[2], v.bytes[0], (uint8_t)(v.bytes[1] + 1)}}; }

Nested c_nested(Nested v) {
  return (Nested){(int8_t)-v.tag, {v.in.b, v.in.a}, {v.tail[2], v.tail[1], v.tail[0]}, v.d * 2};
}

Outer c_outer(Outer v) {
  v.q->x += 1;
  return (Outer){{v.m.a + 1, v.m.b}, v.q, !v.ok};
}

void c_nothing(void) {}

// More than the registers of any target take.
double c_many(int8_t p1, double p2, int64_t p3, float p4, uint16_t p5, double p6, int32_t p7,
              float p8, bool p9, double p10, uint8_t p11, double p12, int64_t p13, float p14,
              Small p15, double p16, int16_t p17, Two p18, uint32_t p19, double p20,
              const int32_t *p21, float p22, Mixed p23, int64_t p24) {
  return 1 * p1 + 2 * p2 + 3 * p3 + 4 * p4 + 5 * p5 + 6 * p6 + 7 * p7 + 8 * p8 + 9 * p9 +
         10 * p10 + 11 * p11 + 12 * p12 + 13 * p13 + 14 * p14 + 15 * (p15.a - p15.b) + 16 * p16 +
         17 * p17 + 18 * (p18.x - p18.y) + 19 * p19 + 20 * p20 + 21 * *p21 + 22 * p22 +
         23 * (p23.a + p23.b) + 24 * p24;
}
