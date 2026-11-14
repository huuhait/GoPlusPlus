// The C side of cabi_export.gopp: it calls each function that the
// program has for C and says which of them gave what C did not expect.
#include <stdbool.h>
#include <stdint.h>

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

int8_t g_i8(int8_t x);
uint16_t g_u16(uint16_t x);
int64_t g_wide(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e);
double g_f64(float x, double y);
bool g_not(bool x);
void g_count(void);
int32_t g_or(const uint8_t *at, int32_t otherwise);
int32_t *g_next(int32_t *at);
Small g_small(Small v);
Mixed g_mixed(Mixed v);
Quad g_quad(Quad v);
Large g_large(Large v);
Odd g_odd(Odd v);
Nested g_nested(Nested v);
double g_many(int8_t p1, double p2, int64_t p3, float p4, uint16_t p5, double p6, int32_t p7,
              float p8, bool p9, double p10, uint8_t p11, double p12, int64_t p13, float p14,
              Small p15, double p16, int16_t p17, Quad p18, uint32_t p19, double p20,
              const int32_t *p21, float p22, Mixed p23, int64_t p24);

// The number of the first call that went wrong, from 1; 0 if none did.
int32_t c_run(void) {
  int32_t at = 0;
  at++;
  if (g_i8(-128) != 127) return at;
  at++;
  if (g_u16(65535) != 32767) return at;
  at++;
  if (g_wide(-3, 250, -7, 65000, true) != -3000000 + 2500000 - 700 + 65000 + 1) return at;
  at++;
  if (g_f64(1.5f, 0.25) != 1.75) return at;
  at++;
  if (!g_not(false) || g_not(true)) return at;
  at++;
  g_count();
  g_count();
  uint8_t byte = 9;
  at++;
  if (g_or(&byte, 1) != 9 || g_or(0, 1) != 1) return at;
  int32_t xs[3] = {10, 20, 30};
  at++;
  if (g_next(xs) != &xs[1] || xs[0] != 11) return at;
  at++;
  Small small = g_small((Small){3, 4});
  if (small.a != 4 || small.b != 4) return at;
  at++;
  Mixed mixed = g_mixed((Mixed){21, 1.5});
  if (mixed.a != 42 || mixed.b != 2) return at;
  at++;
  Quad quad = g_quad((Quad){1, 2, 3, 4});
  if (quad.x != 4 || quad.y != 3 || quad.z != 2 || quad.w != 2) return at;
  at++;
  Large large = g_large((Large){1, 2, 3});
  if (large.a != 3 || large.b != 1 || large.c != 3) return at;
  at++;
  Odd odd = g_odd((Odd){{1, 2, 3}});
  if (odd.bytes[0] != 3 || odd.bytes[1] != 1 || odd.bytes[2] != 3) return at;
  at++;
  Nested nested = g_nested((Nested){5, {6, 7}, {8, 9, 10}, 1.5});
  if (nested.tag != -5 || nested.in.a != 7 || nested.in.b != 6 || nested.tail[0] != 10 ||
      nested.tail[1] != 9 || nested.tail[2] != 8 || nested.d != 3)
    return at;
  at++;
  int32_t pointed = -2;
  double many = g_many(-1, 2, 3, 4, 5, 6, -7, 8, true, 10, 11, 12, -13, 14, (Small){20, 5}, 16,
                       -17, (Quad){3, 1, 0, 0}, 19, 20, &pointed, 22, (Mixed){2, 0.5}, 24);
  double want = -1.0 + 4 + 9 + 16 + 25 + 36 - 49 + 64 + 9 + 100 + 121 + 144 - 169 + 196 +
                15 * 15 + 256 - 289 + 36 + 361 + 400 - 42 + 484 + 23 * 2.5 + 576;
  if (many != want) return at;
  return 0;
}
