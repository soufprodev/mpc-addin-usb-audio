/* test_send.c: the send bus under concurrency - four plugin threads write the same blocks (two on
 * one channel, summed) while the codec thread takes frames; nothing is lost or doubled. */
#include "../src/send.h"
#include "t.h"
#include <pthread.h>
#include <stdatomic.h>

#define BLOCKS 2000
#define BF 64
static _Atomic int go_block = -1, done_count;

static void *writer(void *arg) {
  unsigned ch = (unsigned)(uintptr_t)arg;
  float buf[BF];
  for (int i = 0; i < BF; i++) buf[i] = 0.125f;
  const float *p[1] = {buf};
  uint32_t cur = 0;
  for (int b = 0; b < BLOCKS; b++) {
    while (atomic_load(&go_block) < b) {}
    mpcua_send_v1(ch, 1, p, BF, &cur);
    atomic_fetch_add(&done_count, 1);
  }
  return NULL;
}

int main(void) {
  mpcua_send_enable(2);
  pthread_t th[4];
  unsigned chans[4] = {1, 1, 2, 2};
  for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, writer, (void *)(uintptr_t)chans[i]);
  int32_t out[BF * 2];
  int bad = 0;
  for (int b = 0; b < BLOCKS; b++) {
    atomic_store(&go_block, b);                       /* MPC: render the block on all workers... */
    while (atomic_load(&done_count) < 4 * (b + 1)) {}
    mpcua_send_take(out, 2, 2, BF);                    /* ...then write it to the codec */
    for (int i = 0; i < BF * 2; i++) if (out[i] != (int32_t)(0.25 * 2147483648.0)) bad++;
  }
  for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
  CHECK_EQ(bad, 0);
  T_DONE("send bus: concurrent writers summed exactly");
}
