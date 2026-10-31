extern long write(int, const void *, unsigned long);
extern int GPx(int), _G(int), _gpx(int), _Gx(int);
static void num(const char *name, unsigned long n, int v) {
  char buf[48];
  unsigned long i = 0;
  for (; i < n; i++) buf[i] = name[i];
  buf[i++] = '=';
  buf[i++] = '0' + v / 10;
  buf[i++] = '0' + v % 10;
  buf[i++] = '\n';
  write(1, buf, i);
}
void c_run(void) {
  num("GPx", 3, GPx(10));
  num("_G", 2, _G(10));
  num("_gpx", 4, _gpx(10));
  num("_Gx", 3, _Gx(10));
}
