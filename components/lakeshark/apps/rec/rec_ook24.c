#include "rec_watch.h"
#include <string.h>

static bool near(int value, int expected) {
  return value >= expected * 65 / 100 && value <= expected * 135 / 100;
}

bool rec_decode_ook24(const int32_t *p, int n, rec_ook24_t *out) {
  if (!out)
    return false;
  memset(out, 0, sizeof(*out));
  if (!p || n < 100 || n > REC_WATCH_EDGES)
    return false;
  uint32_t candidate = 0;
  unsigned repeats = 0;
  int previous_end = -1, unit = 0;
  for (int start = 0; start + 49 < n; start++) {
    int t = p[start];
    if (t < 100 || t > 1000 || p[start + 1] >= 0 || p[start + 1] < -50000 ||
        !near(-p[start + 1], t * 31))
      continue;
    uint32_t value = 0;
    bool valid = true;
    for (int bit = 0; bit < 24; bit++) {
      int hi = p[start + 2 + bit * 2], lo = p[start + 3 + bit * 2];
      if (hi <= 0 || lo >= 0 || lo < -10000) {
        valid = false;
        break;
      }
      bool zero = near(hi, t) && near(-lo, 3 * t);
      bool one = near(hi, 3 * t) && near(-lo, t);
      if (!zero && !one) {
        valid = false;
        break;
      }
      value = (value << 1) | one;
    }
    if (!valid)
      continue;
    if (start == previous_end && value == candidate && near(t, unit))
      repeats++;
    else {
      candidate = value;
      repeats = 1;
      unit = t;
    }
    previous_end = start + 50;
    if (repeats >= 2) {
      out->value = value;
      out->repeats = repeats;
      out->unit_us = unit;
    }
    start += 49;
  }
  return out->repeats >= 2;
}
