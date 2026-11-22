/* send.c: see send.h. Audio-thread code only: no locks, no allocation, no syscalls. */
#include "send.h"

#include <stdatomic.h>
#include <stddef.h>

#include "chmap.h"

#define EXPORT __attribute__((visibility("default")))
#define MASK (MPCUA_SEND_FRAMES - 1u)
#define HEADROOM_SHIFT 4   /* slots hold samples / 16: up to 16 full-scale writers sum without wrapping */

static _Atomic int32_t bus[MPCUA_SEND_MAX][MPCUA_SEND_FRAMES];
static _Atomic uint32_t bus_pos;      /* frame the reader takes next */
static _Atomic unsigned bus_sends;    /* 0 = off */
static _Atomic uint32_t bus_seen;

void mpcua_send_enable(unsigned nsend) {
  atomic_store(&bus_sends, nsend > MPCUA_SEND_MAX ? MPCUA_SEND_MAX : nsend);
}

uint32_t mpcua_send_seen(void) { return atomic_exchange(&bus_seen, 0); }

static inline int32_t to_slot(float x) {
  /* full scale (1.0) -> 2^31 / 16 */
  float v = x * (float)(1u << (31 - HEADROOM_SHIFT));
  const float lim = (float)(1u << (31 - HEADROOM_SHIFT)) * 15.99f;   /* ~+24 dB before it clamps */
  if (!(v == v)) v = 0.0f;                                              /* NaN */
  if (v > lim) v = lim;
  if (v < -lim) v = -lim;
  return (int32_t)v;
}

EXPORT int mpcua_send_v1(unsigned first, unsigned nch, const float *const *bufs, unsigned frames, uint32_t *cursor) {
  unsigned n = atomic_load_explicit(&bus_sends, memory_order_relaxed);
  if (!n || !bufs || !cursor || first < 1 || first > n || frames == 0 || frames > MPCUA_SEND_FRAMES / 4) return -1;
  uint32_t pos = atomic_load_explicit(&bus_pos, memory_order_acquire);
  uint32_t w = *cursor;
  int32_t ahead = (int32_t)(w - pos);
  /* Behind the reader (first call, or MPC skipped this plugin for a while): start at the next frame
   * MPC writes. Far ahead (the codec isn't being written, MPC's audio stopped): start over too. */
  if (ahead < 0 || ahead > (int32_t)(MPCUA_SEND_FRAMES / 2)) w = pos;
  for (unsigned c = 0; c < nch && first + c <= n; c++) {
    const float *src = bufs[c];
    if (!src) continue;
    _Atomic int32_t *row = bus[first - 1 + c];
    for (unsigned i = 0; i < frames; i++) {
      int32_t v = to_slot(src[i]);
      if (v) atomic_fetch_add_explicit(&row[(w + i) & MASK], v, memory_order_relaxed);
    }
    atomic_fetch_or_explicit(&bus_seen, 1u << (first - 1 + c), memory_order_relaxed);
  }
  *cursor = w + frames;
  return 0;
}

void mpcua_send_take(int32_t *dst, unsigned stride, unsigned nsend, uint32_t frames) {
  uint32_t pos = atomic_load_explicit(&bus_pos, memory_order_relaxed);
  if (nsend > MPCUA_SEND_MAX) nsend = MPCUA_SEND_MAX;
  for (unsigned c = 0; c < nsend; c++) {
    _Atomic int32_t *row = bus[c];
    for (uint32_t i = 0; i < frames; i++) {
      int32_t v = atomic_exchange_explicit(&row[(pos + i) & MASK], 0, memory_order_relaxed);
      if (!dst) continue;
      int64_t s = (int64_t)v * (1 << HEADROOM_SHIFT);   /* back to full scale, saturated */
      dst[(size_t)i * stride + c] = s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : (int32_t)s;
    }
  }
  atomic_store_explicit(&bus_pos, pos + frames, memory_order_release);
}
