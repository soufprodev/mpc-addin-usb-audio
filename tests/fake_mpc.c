/* fake_mpc.c: stands in for /usr/bin/MPC (the binary must be named MPC for the addin to activate).
 * Drives the preloaded addin through the same calls MPC makes and checks what it does.
 * Usage: MPC <configfs-root> [inert | auto | own]
 *   inert: running under another name, every hook must pass through
 *   auto:  tap_card=auto with no platform codec and no gadget ever enabled
 *   own:   no gadget from MPC, so the addin builds its own (own_wait=1); then MPC enables one after all */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "t.h"

typedef struct { int card, stream; unsigned ch; } fake_pcm;
typedef struct { int fmt; unsigned ch, rate; int access; unsigned maxch; } fake_hw;
typedef struct { char name[32]; int cfg; } fake_gadget;

int snd_pcm_open(fake_pcm **, const char *, int, int);
int snd_pcm_close(fake_pcm *);
int snd_pcm_hw_params(fake_pcm *, fake_hw *);
long snd_pcm_writei(fake_pcm *, const void *, unsigned long);
long snd_pcm_readi(fake_pcm *, void *, unsigned long);
long snd_pcm_readn(fake_pcm *, void **, unsigned long);
int usbg_enable_gadget(fake_gadget *, void *);
long snd_pcm_writen(fake_pcm *, void **, unsigned long);
int snd_pcm_hw_params_any(fake_pcm *, fake_hw *);
int snd_pcm_hw_params_current(fake_pcm *, fake_hw *);
int snd_pcm_hw_params_get_channels(const fake_hw *, unsigned *);
int snd_pcm_hw_params_get_channels_max(const fake_hw *, unsigned *);
int snd_pcm_hw_params_test_channels(fake_pcm *, fake_hw *, unsigned);
int snd_pcm_hw_params_set_channels(fake_pcm *, fake_hw *, unsigned);
int snd_pcm_hw_params_set_channels_near(fake_pcm *, fake_hw *, unsigned *);
long snd_pcm_frames_to_bytes(fake_pcm *, long);
long snd_pcm_bytes_to_frames(fake_pcm *, long);
static int (*mpcua_send_v1)(unsigned, unsigned, const float *const *, unsigned, uint32_t *);
extern int32_t fake_wbuf[]; extern unsigned long fake_wbuf_samples;
extern int fake_add_calls, fake_enable_calls, fake_rm_calls, fake_cap_s16;

static int (*t_active)(void);
static void (*t_set)(int, int);
static uint32_t (*t_pop)(int, int32_t *, uint32_t, unsigned);
static uint32_t (*t_push)(const int32_t *, uint32_t, unsigned);

static void slurp(const char *path, char *out, size_t n) {
  FILE *f = fopen(path, "r"); out[0] = 0;
  if (f) { size_t r = fread(out, 1, n - 1, f); out[r] = 0; fclose(f); }
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  const char *root = argv[1];
  *(void **)&t_active = dlsym(RTLD_DEFAULT, "mpcua_test_active");
  *(void **)&t_set = dlsym(RTLD_DEFAULT, "mpcua_test_set");
  *(void **)&t_pop = dlsym(RTLD_DEFAULT, "mpcua_test_pop");
  *(void **)&t_push = dlsym(RTLD_DEFAULT, "mpcua_test_push_host");
  *(void **)&mpcua_send_v1 = dlsym(RTLD_DEFAULT, "mpcua_send_v1");
  CHECK(t_active && t_set && t_pop && t_push && mpcua_send_v1);
  if (t_fail) T_DONE("hooks: test helpers present");

  if (argc > 2 && !strcmp(argv[2], "own")) {
    CHECK_EQ(t_active(), 1);
    char path[512], v[64];
    snprintf(path, sizeof path, "%s/mpcusbaudio/UDC", root);
    for (int i = 0; i < 40; i++) { slurp(path, v, sizeof v); if (v[0] && v[0] != '\n') break; usleep(100000); }
    CHECK(!strcmp(v, "fake.udc"));                  /* built and bound to the only UDC */
    snprintf(path, sizeof path, "%s/mpcusbaudio/idVendor", root);
    slurp(path, v, sizeof v); CHECK(!strcmp(v, "0x1d6b"));
    fake_gadget g = {"standalone", 0};
    CHECK_EQ(usbg_enable_gadget(&g, NULL), 0);      /* MPC builds its own after all: ours goes first */
    snprintf(path, sizeof path, "%s/mpcusbaudio/UDC", root);
    slurp(path, v, sizeof v); CHECK(!strcmp(v, "\n"));
    CHECK_EQ(fake_add_calls, 1); CHECK_EQ(fake_enable_calls, 1);
    T_DONE("hooks: own gadget when MPC builds none, removed when MPC enables one");
  }
  if (argc > 2 && !strcmp(argv[2], "loop")) {
    CHECK_EQ(t_active(), 1);
    char path[512], v[64];
    snprintf(path, sizeof path, "%s/mpcusbaudio/UDC", root);
    for (int i = 0; i < 40; i++) { slurp(path, v, sizeof v); if (v[0] && v[0] != '\n') break; usleep(100000); }
    CHECK(!strcmp(v, "fake.udc"));                  /* built and bound */
    /* MPC_USB_AUDIO_USB_DIR lists our gadget on this device's own bus: the backstop removes it again */
    for (int i = 0; i < 80; i++) { slurp(path, v, sizeof v); if (!strcmp(v, "\n")) break; usleep(100000); }
    CHECK(!strcmp(v, "\n"));
    T_DONE("hooks: own gadget removed when this device enumerates it itself");
  }
  if (argc > 2 && !strcmp(argv[2], "auto")) {
    CHECK_EQ(t_active(), 1);
    /* the first card MPC opens becomes the codec; another card is not tapped */
    fake_pcm *pp, *other_pcm, *cp;
    fake_hw hw = {10, 2, 44100, 3, 0};
    snd_pcm_open(&pp, "hw:3,0", 0, 0);
    snd_pcm_open(&other_pcm, "hw:4,0", 0, 0);
    snd_pcm_open(&cp, "hw:3,0", 1, 0);
    snd_pcm_hw_params(pp, &hw);
    snd_pcm_hw_params(other_pcm, &hw);
    t_set(1, 0);
    int32_t ramp[256], got[256];
    for (int i = 0; i < 256; i++) ramp[i] = i * 3 + 1;
    CHECK_EQ(snd_pcm_writei(pp, ramp, 128), 128);
    CHECK_EQ(snd_pcm_writei(other_pcm, ramp, 128), 128);
    CHECK_EQ(t_pop(0, got, 256, 2), 128);
    for (int i = 0; i < 256; i++) CHECK_EQ(got[i], ramp[i]);
    snd_pcm_close(pp); snd_pcm_close(other_pcm); snd_pcm_close(cp);
    sleep(2);   /* MPC_USB_AUDIO_GADGET_WAIT=1: the watch thread logs that no gadget came */
    T_DONE("hooks: tap_card=auto adopts MPC's first card");
  }
  if (argc > 2 && !strcmp(argv[2], "wide")) {   /* wide_out=8, wide_in=4; the fake codec has 2 channels */
    CHECK_EQ(t_active(), 1);
    fake_pcm *pp, *cp, *other_pcm;
    fake_hw h, oh;
    unsigned v = 0;
    snd_pcm_open(&pp, "hw:0,0", 0, 0);
    snd_pcm_open(&other_pcm, "hw:1,0", 0, 0);
    /* MPC probes the codec: it is told 8, another card keeps its real 2 */
    CHECK_EQ(snd_pcm_hw_params_any(pp, &h), 0);
    CHECK_EQ(snd_pcm_hw_params_get_channels_max(&h, &v), 0); CHECK_EQ(v, 8u);
    CHECK_EQ(snd_pcm_hw_params_test_channels(pp, &h, 8), 0);
    CHECK(snd_pcm_hw_params_test_channels(pp, &h, 9) < 0);
    CHECK_EQ(snd_pcm_hw_params_any(other_pcm, &oh), 0);
    CHECK_EQ(snd_pcm_hw_params_get_channels_max(&oh, &v), 0); CHECK_EQ(v, 2u);
    CHECK(snd_pcm_hw_params_set_channels(other_pcm, &oh, 8) < 0);
    /* MPC opens 8 channels: the codec gets 2, MPC reads 8 back */
    CHECK_EQ(snd_pcm_hw_params_set_channels(pp, &h, 8), 0);
    CHECK_EQ(h.ch, 2u);
    CHECK_EQ(snd_pcm_hw_params_get_channels(&h, &v), 0); CHECK_EQ(v, 8u);
    h.fmt = 10; h.rate = 44100; h.access = 3;
    CHECK_EQ(snd_pcm_hw_params(pp, &h), 0);
    fake_hw cur;
    CHECK_EQ(snd_pcm_hw_params_current(pp, &cur), 0);
    CHECK_EQ(snd_pcm_hw_params_get_channels(&cur, &v), 0); CHECK_EQ(v, 8u);
    CHECK_EQ(snd_pcm_frames_to_bytes(pp, 128), 128 * 8 * 4);   /* MPC sizes its buffers for 8 channels */
    CHECK_EQ(snd_pcm_bytes_to_frames(pp, 4096), 128);
    CHECK_EQ(snd_pcm_frames_to_bytes(other_pcm, 128), 128 * 2 * 4);
    t_set(1, 0);
    static int32_t ramp[64 * 8], got[64 * 8];
    for (int i = 0; i < 64 * 8; i++) ramp[i] = i * 977 + 3;
    CHECK_EQ(snd_pcm_writei(pp, ramp, 64), 64);
    CHECK_EQ(fake_wbuf_samples, 128ul);              /* the codec got frames of its own 2 channels */
    for (int i = 0; i < 64; i++) { CHECK_EQ(fake_wbuf[2 * i], ramp[8 * i]); CHECK_EQ(fake_wbuf[2 * i + 1], ramp[8 * i + 1]); }
    CHECK_EQ(t_pop(0, got, 64, 8), 64);              /* USB gets all 8 */
    for (int i = 0; i < 64 * 8; i++) CHECK_EQ(got[i], ramp[i]);
    /* a large write is split through the bounce buffer: 2100 frames x 8 ch S32 */
    static int32_t big[2100 * 8];
    for (int i = 0; i < 2100 * 8; i++) big[i] = i;
    CHECK_EQ(snd_pcm_writei(pp, big, 2100), 2100);
    CHECK_EQ(t_pop(0, got, 64, 8), 64);
    for (int i = 0; i < 64 * 8; i++) CHECK_EQ(got[i], i);

    /* capture: S16, MPC asks near 6, gets wide_in=4; inputs 3-4 come only from the computer */
    snd_pcm_open(&cp, "hw:0,0", 1, 0);
    fake_hw ch;
    CHECK_EQ(snd_pcm_hw_params_any(cp, &ch), 0);
    v = 6;
    CHECK_EQ(snd_pcm_hw_params_set_channels_near(cp, &ch, &v), 0); CHECK_EQ(v, 4u); CHECK_EQ(ch.ch, 2u);
    ch.fmt = 2; ch.rate = 44100; ch.access = 3;
    CHECK_EQ(snd_pcm_hw_params(cp, &ch), 0);
    t_set(1, 1);
    int32_t host[32 * 4];
    for (int i = 0; i < 32; i++) for (int c = 0; c < 4; c++) host[4 * i + c] = (c * 10 + i) << 16;
    CHECK_EQ(t_push(host, 32, 4), 32);
    int16_t in[32 * 4];
    memset(in, 0x55, sizeof in);
    CHECK_EQ(snd_pcm_readi(cp, in, 32), 32);
    for (int i = 0; i < 32; i++) {
      CHECK_EQ(in[4 * i], 100 + i);                  /* codec input 1 + computer */
      CHECK_EQ(in[4 * i + 1], 110 + i);
      CHECK_EQ(in[4 * i + 2], 20 + i);               /* computer only */
      CHECK_EQ(in[4 * i + 3], 30 + i);
    }
    CHECK_EQ(t_pop(1, got, 32, 4), 32);              /* MPC's inputs, as read, before injection */
    CHECK_EQ(got[0], 100 << 16); CHECK_EQ(got[2], 0); CHECK_EQ(got[3], 0);
    /* non-interleaved: buffers 3-4 are cleared, then the computer's audio lands */
    ch.access = 4;
    CHECK_EQ(snd_pcm_hw_params(cp, &ch), 0);
    CHECK_EQ(t_push(host, 4, 4), 4);
    int16_t b0[4], b1[4], b2[4] = {9, 9, 9, 9}, b3[4] = {9, 9, 9, 9};
    void *bufs[4] = {b0, b1, b2, b3};
    CHECK_EQ(snd_pcm_readn(cp, bufs, 4), 4);
    CHECK_EQ(b0[1], 101); CHECK_EQ(b2[1], 21); CHECK_EQ(b3[3], 33);

    /* MPC asks for 2 (or the codec's own count): nothing is widened */
    while (t_pop(0, got, 64, 8)) {}                  /* the forwarder's session restart does this on the device */
    snd_pcm_close(pp);
    snd_pcm_open(&pp, "hw:0,0", 0, 0);
    CHECK_EQ(snd_pcm_hw_params_any(pp, &h), 0);
    CHECK_EQ(snd_pcm_hw_params_set_channels(pp, &h, 2), 0);
    CHECK_EQ(snd_pcm_hw_params_get_channels(&h, &v), 0); CHECK_EQ(v, 2u);
    h.fmt = 10; h.rate = 44100; h.access = 3;
    snd_pcm_hw_params(pp, &h);
    CHECK_EQ(snd_pcm_writei(pp, ramp, 16), 16);
    CHECK_EQ(fake_wbuf[2], ramp[2]);                 /* passed through untouched */
    CHECK_EQ(t_pop(0, got, 16, 2), 16);
    CHECK_EQ(got[5], ramp[5]);
    snd_pcm_close(pp); snd_pcm_close(cp); snd_pcm_close(other_pcm);
    T_DONE("hooks: wide codec (MPC sees 8 out / 4 in, codec 2/2, USB all)");
  }
  if (argc > 2 && !strcmp(argv[2], "send")) {   /* to_host=out1,out2,send1-4: USB Out plugins on the send bus */
    CHECK_EQ(t_active(), 1);
    fake_pcm *pp;
    fake_hw h = {10, 2, 44100, 3, 0};
    snd_pcm_open(&pp, "hw:0,0", 0, 0);
    snd_pcm_hw_params(pp, &h);
    t_set(1, 0);
    float a[128], b[128], c[128];
    uint32_t ca = 0, cb = 0, cc = 0;                 /* each plugin instance's own cursor */
    for (int i = 0; i < 128; i++) { a[i] = (float)i / 256.0f; b[i] = -(float)i / 512.0f; c[i] = 0.25f; }
    int32_t mainbuf[128 * 2], got[128 * 6];
    for (int round = 0; round < 3; round++) {
      /* MPC renders in 64-frame blocks and writes the codec every 128 frames. Instance A: stereo on
       * send1-2; B: mono on send3; C: mono, also on send3 (summed); round 1 skips B (track idle). */
      for (int blk = 0; blk < 2; blk++) {
        const float *pa[2] = {a + 64 * blk, b + 64 * blk}, *pb[1] = {b + 64 * blk}, *pc[1] = {c + 64 * blk};
        CHECK_EQ(mpcua_send_v1(1, 2, pa, 64, &ca), 0);
        if (round != 1) CHECK_EQ(mpcua_send_v1(3, 1, pb, 64, &cb), 0);
        CHECK_EQ(mpcua_send_v1(3, 1, pc, 64, &cc), 0);
      }
      for (int i = 0; i < 256; i++) mainbuf[i] = round * 1000 + i;
      CHECK_EQ(snd_pcm_writei(pp, mainbuf, 128), 128);
      CHECK_EQ(t_pop(0, got, 128, 6), 128);
      for (int i = 0; i < 128; i++) {
        int32_t *f = got + 6 * i;
        CHECK_EQ(f[0], mainbuf[2 * i]); CHECK_EQ(f[1], mainbuf[2 * i + 1]);
        CHECK(llabs((long long)f[2] - (long long)(a[i] * 2147483648.0)) < 64);       /* send1 = A left */
        CHECK(llabs((long long)f[3] - (long long)(b[i] * 2147483648.0)) < 64);       /* send2 = A right */
        double want3 = (round != 1 ? b[i] : 0.0) + c[i];
        CHECK(llabs((long long)f[4] - (long long)(want3 * 2147483648.0)) < 64);      /* send3 = B + C */
        CHECK_EQ(f[5], 0);                                                           /* send4: nobody */
      }
    }
    CHECK_EQ(mpcua_send_v1(5, 1, (const float *const[]){c}, 64, &cc), -1);    /* beyond send4: refused */
    snd_pcm_close(pp);
    T_DONE("hooks: send bus (USB Out plugins) aligned with the main out, summed, idle-safe");
  }
  if (argc > 2) {  /* running under another name: must be a pure pass-through */
    CHECK_EQ(t_active(), 0);
    fake_gadget g = {"standalone", 0};
    CHECK_EQ(usbg_enable_gadget(&g, NULL), 0);
    CHECK_EQ(fake_add_calls, 0); CHECK_EQ(fake_enable_calls, 1);
    fake_pcm *p; fake_hw hw = {10, 2, 44100, 3, 0};
    snd_pcm_open(&p, "hw:0,0", 0, 0); snd_pcm_hw_params(p, &hw);
    int32_t buf[8] = {0};
    CHECK_EQ(snd_pcm_writei(p, buf, 4), 4);
    snd_pcm_close(p);
    T_DONE("hooks: inert outside MPC");
  }
  CHECK_EQ(t_active(), 1);

  /* 1. gadget: our function only on the standalone gadget, attributes + IAD class written */
  char path[512], v[64];
  fake_gadget g = {"standalone", 0};
  CHECK_EQ(usbg_enable_gadget(&g, NULL), 0);
  CHECK_EQ(fake_add_calls, 1); CHECK_EQ(fake_enable_calls, 1); CHECK_EQ(fake_rm_calls, 0);
  snprintf(path, sizeof path, "%s/standalone/functions/uac2.usbaudio/p_chmask", root);
  slurp(path, v, sizeof v); CHECK(!strcmp(v, "0xf"));
  snprintf(path, sizeof path, "%s/standalone/functions/uac2.usbaudio/function_name", root);
  slurp(path, v, sizeof v); CHECK(!strcmp(v, "MPC USB Audio"));
  snprintf(path, sizeof path, "%s/standalone/bDeviceClass", root);
  slurp(path, v, sizeof v); CHECK(!strcmp(v, "0xEF"));
  fake_gadget other = {"smexstream", 0};
  CHECK_EQ(usbg_enable_gadget(&other, NULL), 0);
  CHECK_EQ(fake_add_calls, 1); CHECK_EQ(fake_enable_calls, 2);

  /* 2. playback tap: S32 stereo on the codec card (tap_card=0 in the test config) */
  fake_pcm *pp, *other_pcm;
  fake_hw hw = {10, 2, 44100, 3, 0};
  snd_pcm_open(&pp, "hw:0,0", 0, 0);
  snd_pcm_open(&other_pcm, "hw:1,0", 0, 0);        /* another card: never tapped */
  snd_pcm_hw_params(pp, &hw);
  snd_pcm_hw_params(other_pcm, &hw);
  int32_t ramp[256], got[256];
  for (int i = 0; i < 256; i++) ramp[i] = i * 1000 - 7;
  CHECK_EQ(snd_pcm_writei(pp, ramp, 128), 128);
  CHECK_EQ(t_pop(0, got, 128, 2), 0);              /* forwarder not running: no tap */
  t_set(1, 0);
  CHECK_EQ(snd_pcm_writei(pp, ramp, 128), 128);
  CHECK_EQ(snd_pcm_writei(other_pcm, ramp, 128), 128);
  CHECK_EQ(t_pop(0, got, 256, 2), 128);
  for (int i = 0; i < 256; i++) CHECK_EQ(got[i], ramp[i]);

  /* 3. capture: S16 stereo. Inputs are tapped before injection; computer audio is summed in. */
  fake_pcm *cp;
  fake_hw chw = {2, 2, 44100, 3, 0};
  snd_pcm_open(&cp, "hw:0,0", 1, 0);
  snd_pcm_hw_params(cp, &chw);
  t_set(1, 1);
  int32_t host[128 * 2];
  for (int i = 0; i < 128; i++) { host[2 * i] = i << 16; host[2 * i + 1] = -(i << 16); }
  CHECK_EQ(t_push(host, 128, 2), 128);
  int16_t in[128 * 2];
  CHECK_EQ(snd_pcm_readi(cp, in, 128), 128);
  for (int i = 0; i < 128; i++) { CHECK_EQ(in[2 * i], 100 + i); CHECK_EQ(in[2 * i + 1], 100 - i); }
  CHECK_EQ(t_pop(1, got, 128, 2), 128);
  for (int i = 0; i < 256; i++) CHECK_EQ(got[i], 100 << 16);

  /* saturation in S16 */
  for (int i = 0; i < 4; i++) host[i] = 0x7fff0000;
  t_push(host, 2, 2);
  CHECK_EQ(snd_pcm_readi(cp, in, 2), 2);
  CHECK_EQ(in[0], 32767);

  /* non-interleaved readn path (MPC reconfigures the capture PCM for RW_NONINTERLEAVED) */
  chw.access = 4;
  snd_pcm_hw_params(cp, &chw);
  for (int i = 0; i < 8; i++) host[i] = 5 << 16;
  t_push(host, 4, 2);
  int16_t l[4], r[4]; void *bufs[2] = {l, r};
  CHECK_EQ(snd_pcm_readn(cp, bufs, 4), 4);
  CHECK_EQ(l[3], 105); CHECK_EQ(r[0], 105);
  CHECK_EQ(snd_pcm_readi(cp, in, 4), 4);            /* wrong access for this setup: untouched */
  chw.access = 3;
  snd_pcm_hw_params(cp, &chw);

  /* 4. computer audio not live: stale data is dropped, input untouched */
  t_push(host, 4, 2);
  t_set(1, 0);
  CHECK_EQ(snd_pcm_readi(cp, in, 4), 4);
  CHECK_EQ(in[0], 100);
  t_set(1, 1);
  CHECK_EQ(snd_pcm_readi(cp, in, 4), 4);
  CHECK_EQ(in[0], 100);                             /* ring was emptied */

  /* 5. rate mismatch: MPC reopens at 48 kHz, gadget is 44.1 kHz -> no tap */
  t_pop(0, got, 128, 2);
  hw.rate = 48000;
  snd_pcm_hw_params(pp, &hw);
  CHECK_EQ(snd_pcm_writei(pp, ramp, 64), 64);
  CHECK_EQ(t_pop(0, got, 64, 2), 0);

  /* 6. close clears the tap */
  hw.rate = 44100;
  snd_pcm_hw_params(pp, &hw);
  snd_pcm_close(pp);
  snd_pcm_open(&pp, "hw:2,0", 0, 0);               /* new handle, other card */
  snd_pcm_hw_params(pp, &hw);
  CHECK_EQ(snd_pcm_writei(pp, ramp, 64), 64);
  CHECK_EQ(t_pop(0, got, 64, 2), 0);
  snd_pcm_close(pp); snd_pcm_close(cp); snd_pcm_close(other_pcm);
  T_DONE("hooks: MPC process");
}
