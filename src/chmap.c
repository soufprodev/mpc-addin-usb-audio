/* chmap.c: channel map parsing and frame assembly (see chmap.h). */
#include "chmap.h"

#include <ctype.h>
#include <string.h>

static int parse_num(const char *s, size_t len, unsigned *out) {
  unsigned v = 0;
  if (!len) return -1;
  for (size_t i = 0; i < len; i++) {
    if (!isdigit((unsigned char)s[i])) return -1;
    v = v * 10 + (unsigned)(s[i] - '0');
    if (v > MPCUA_MAX_CH) return -1;
  }
  if (v < 1) return -1;
  *out = v;
  return 0;
}

/* One comma-separated entry into out[0..room-1]. Returns the number of channels it stands for
 * (a range gives several), or -1. */
static int parse_one(const char *s, size_t len, unsigned allowed, mpcua_chsrc *out, int room) {
  while (len && isspace((unsigned char)*s)) { s++; len--; }
  while (len && isspace((unsigned char)s[len - 1])) len--;
  if (!len || room < 1) return -1;
  if ((len == 1 && (*s == '0' || *s == '-'))) { out->kind = MPCUA_SRC_NONE; out->idx = 0; return 1; }
  static const struct { const char *name; uint8_t kind; } kinds[] = {
    {"out", MPCUA_SRC_OUT}, {"in", MPCUA_SRC_IN}, {"host", MPCUA_SRC_HOST}, {"send", MPCUA_SRC_SEND},
  };
  for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
    size_t nl = strlen(kinds[k].name);
    if (len <= nl || strncmp(s, kinds[k].name, nl) != 0) continue;
    if (!(allowed & kinds[k].kind)) return -1;
    const char *num = s + nl, *dash = memchr(num, '-', len - nl);
    unsigned a, b;
    if (dash) {
      if (parse_num(num, (size_t)(dash - num), &a) < 0) return -1;
      if (parse_num(dash + 1, len - nl - (size_t)(dash - num) - 1, &b) < 0 || b < a) return -1;
    } else {
      if (parse_num(num, len - nl, &a) < 0) return -1;
      b = a;
    }
    if ((int)(b - a + 1) > room) return -1;
    if (kinds[k].kind == MPCUA_SRC_SEND && b > MPCUA_SEND_MAX) return -1;
    for (unsigned v = a; v <= b; v++) { out[v - a].kind = kinds[k].kind; out[v - a].idx = (uint8_t)(v - 1); }
    return (int)(b - a + 1);
  }
  return -1;
}

int mpcua_chmap_parse(const char *spec, unsigned allowed, mpcua_chsrc *map, int max) {
  if (!spec || max <= 0) return -1;
  int n = 0;
  const char *p = spec;
  for (;;) {
    const char *c = strchr(p, ',');
    size_t len = c ? (size_t)(c - p) : strlen(p);
    int k = parse_one(p, len, allowed, &map[n], max - n);
    if (k < 0) return -1;
    n += k;
    if (!c) break;
    p = c + 1;
  }
  return n;
}

unsigned mpcua_chmap_sends(const mpcua_chsrc *map, int n) {
  unsigned hi = 0;
  for (int i = 0; i < n; i++)
    if (map[i].kind == MPCUA_SRC_SEND && map[i].idx + 1u > hi) hi = map[i].idx + 1u;
  return hi;
}

void mpcua_chmap_frame(const mpcua_chsrc *map, int n, const int32_t *outf, unsigned out_ch, unsigned send_ch,
                       const int32_t *inf, unsigned in_ch, const int32_t *hostf, unsigned host_ch,
                       int32_t *dst) {
  for (int i = 0; i < n; i++) {
    const mpcua_chsrc *m = &map[i];
    int32_t v = 0;
    switch (m->kind) {
    case MPCUA_SRC_OUT: if (outf && m->idx < out_ch) v = outf[m->idx]; break;
    case MPCUA_SRC_SEND: if (outf && m->idx < send_ch) v = outf[out_ch + m->idx]; break;
    case MPCUA_SRC_IN: if (inf && m->idx < in_ch) v = inf[m->idx]; break;
    case MPCUA_SRC_HOST: if (hostf && m->idx < host_ch) v = hostf[m->idx]; break;
    default: break;
    }
    dst[i] = v;
  }
}
