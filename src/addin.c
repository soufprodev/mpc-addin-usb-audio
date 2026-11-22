/* addin.c: LD_PRELOAD entry points.
 *
 * Inside MPC's process only (checked via /proc/self/exe), this:
 *   1. hooks usbg_enable_gadget: when MPC is about to bind its standalone gadget, a UAC2 function is
 *      created and linked with MPC's own libusbgx handles, so MPC's recursive teardown removes it.
 *      Where MPC builds no gadget in standalone mode, the addin builds one of its own instead
 *      (standalone_create, own_gadget_main);
 *   2. hooks snd_pcm_open/hw_params/close to find MPC's playback and capture PCMs on the codec card;
 *   3. hooks snd_pcm_writei/readi (and the non-interleaved variants) to copy main out and inputs into
 *      lock-free rings, and to mix computer playback into what MPC reads from its inputs.
 *   4. with wide_out/wide_in, hooks MPC's channel negotiation on the codec so MPC opens it with more
 *      channels than the codec has (up to 32). The codec gets its real channels; the rest exist only
 *      on USB. If MPC routes tracks to those extra outputs, each arrives on its own USB channel.
 * The gadget's ALSA PCMs are serviced by the forwarder thread (fwd.c).
 *
 * Audio-thread rules (writei/readi/writen/readn): no locks, no allocation, no syscalls beyond the real
 * call, no logging. Everything else runs on MPC's setup threads or the forwarder.
 * In any other process every hook is a plain pass-through.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "addin.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gadget.h"
#include "log.h"
#include "send.h"

/* Only the hooks (and test helpers) are exported; everything else is built hidden. */
#define EXPORT __attribute__((visibility("default")))

mpcua_state g_ua;

#define RING_SAMPLES 65536u   /* per ring: 4096 frames of 16 ch, or 32768 frames of 2 ch */
static int32_t out_store[RING_SAMPLES], in_store[RING_SAMPLES], host_store[RING_SAMPLES];

/* ---- real functions ------------------------------------------------------------------------- */

static int (*real_open)(snd_pcm_t **, const char *, int, int);
static int (*real_close)(snd_pcm_t *);
static int (*real_hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *);
static snd_pcm_sframes_t (*real_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*real_readi)(snd_pcm_t *, void *, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*real_writen)(snd_pcm_t *, void **, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*real_readn)(snd_pcm_t *, void **, snd_pcm_uframes_t);

static void resolve_reals(void) {
  *(void **)&real_open = dlsym(RTLD_NEXT, "snd_pcm_open");
  *(void **)&real_close = dlsym(RTLD_NEXT, "snd_pcm_close");
  *(void **)&real_hw_params = dlsym(RTLD_NEXT, "snd_pcm_hw_params");
  *(void **)&real_writei = dlsym(RTLD_NEXT, "snd_pcm_writei");
  *(void **)&real_readi = dlsym(RTLD_NEXT, "snd_pcm_readi");
  *(void **)&real_writen = dlsym(RTLD_NEXT, "snd_pcm_writen");
  *(void **)&real_readn = dlsym(RTLD_NEXT, "snd_pcm_readn");
}

/* ---- activation ------------------------------------------------------------------------------ */

static int exe_is_mpc(void) {
  char p[256];
  ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
  if (n <= 0) return 0;
  p[n] = 0;
  const char *b = strrchr(p, '/');
  return strcmp(b ? b + 1 : p, "MPC") == 0;
}

/* A USB gadget's own ALSA card (its id is "UAC2Gadget", for example): never MPC's audio. */
static int is_gadget_card(int card) {
  char p[64], id[64] = "";
  snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
  FILE *f = fopen(p, "re");
  if (!f) return 0;
  if (!fgets(id, sizeof id, f)) id[0] = 0;
  fclose(f);
  return strstr(id, "Gadget") != NULL;
}

/* The folder this .so was loaded from (the installer puts its settings and log there), "" if unknown. */
static void addin_dir(char *out, size_t n) {
  out[0] = 0;
  FILE *f = fopen("/proc/self/maps", "re");
  if (!f) return;
  unsigned long me = (unsigned long)(uintptr_t)&addin_dir;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    unsigned long a, b;
    char *path = strchr(line, '/');
    if (!path || sscanf(line, "%lx-%lx", &a, &b) != 2 || me < a || me >= b) continue;
    path[strcspn(path, "\n")] = 0;
    char *slash = strrchr(path, '/');
    if (slash) *slash = 0;
    if (strlen(path) < n) memcpy(out, path, strlen(path) + 1);   /* too long: unknown, never a truncated path */
    break;
  }
  fclose(f);
}

/* The codec card: same rule as MPC's own controller-mode forwarder (/dev/snd/by-path/platform-sound).
 * -1 when there is none (models whose audio is a USB device): snd_pcm_open then adopts the first card
 * MPC opens. */
static int resolve_tap_card(const mpcua_cfg *c) {
  if (c->tap_card >= 0) return c->tap_card;
  char t[64];
  ssize_t n = readlink("/dev/snd/by-path/platform-sound", t, sizeof t - 1);
  if (n <= 0) return -1;
  t[n] = 0;
  const char *s = strstr(t, "controlC");
  return s ? atoi(s + 8) : -1;
}

/* Some models (the Force; the Live II and X by their device trees) have their device port behind the
 * internal USB hub, with a mux that gives the hub to a computer only in Computer Mode. In standalone
 * mode a gadget there is enumerated by the device itself: build nothing, and say why (see gadget.h). */
static _Atomic int hub_internal;

static const char *mux_dir(void) {
  const char *d = getenv("MPC_USB_AUDIO_MUX_DIR");   /* tests */
  return d && *d ? d : "/sys/devices/platform/usb-mux";
}

static const char *usb_dir(void) {
  const char *d = getenv("MPC_USB_AUDIO_USB_DIR");   /* tests */
  return d && *d ? d : "/sys/bus/usb/devices";
}

/* MPC builds its standalone gadget within a second or two of starting. If it never does, say so once:
 * the addin has nothing to add USB audio to (reported on an MPC Live II in standalone mode).
 * MPC_USB_AUDIO_GADGET_WAIT overrides the wait, in seconds (tests). */
static void *gadget_watch(void *arg) {
  (void)arg;
  prctl(PR_SET_NAME, "usbaudio-wait", 0, 0, 0);
  const char *w = getenv("MPC_USB_AUDIO_GADGET_WAIT");
  int secs = w && atoi(w) > 0 ? atoi(w) : 60;
  for (int i = 0; i < secs; i++) {
    if (atomic_load(&g_ua.gadget_seen)) return NULL;
    struct timespec ts = {1, 0};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
  }
  if (atomic_load(&g_ua.gadget_seen) || atomic_load(&hub_internal)) return NULL;
  char udc[64] = "none", state[64] = "?";
  DIR *d = opendir("/sys/class/udc");
  struct dirent *de;
  while (d && (de = readdir(d)))
    if (de->d_name[0] != '.') { snprintf(udc, sizeof udc, "%.63s", de->d_name); break; }
  if (d) closedir(d);
  if (strcmp(udc, "none")) {
    char p[160];
    snprintf(p, sizeof p, "/sys/class/udc/%s/state", udc);
    FILE *f = fopen(p, "re");
    if (f) { if (fgets(state, sizeof state, f)) state[strcspn(state, "\n")] = 0; fclose(f); }
  }
  mpcua_log("no USB audio: after %d s MPC has not enabled its gadget '%s' (UDC %s: %s). On some "
            "models MPC builds no USB gadget in standalone mode, so there is nothing to add USB audio to",
            secs, g_ua.cfg.gadget, udc, state);
  return NULL;
}

/* ---- our own gadget (standalone_create) ------------------------------------------------------- */

static pthread_mutex_t own_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int own_up;   /* our own gadget exists and holds the UDC */
static int mpc_enabled;      /* MPC has enabled a gadget of its own (under own_lock): never build ours */

static const char *udc_dir(void) {
  const char *d = getenv("MPC_USB_AUDIO_UDC_DIR");   /* tests */
  return d && *d ? d : "/sys/class/udc";
}

/* Give the UDC back: MPC (or something else) is about to use it. */
static void own_remove(const char *why) {
  pthread_mutex_lock(&own_lock);
  if (atomic_load(&own_up)) {
    int r = mpcua_own_gadget_remove(&g_ua.cfg);
    atomic_store(&own_up, 0);
    atomic_store(&g_ua.live_gadget, (const char *)g_ua.cfg.gadget);
    if (r < 0) mpcua_log("%s; removing our gadget '%s' failed: %s", why, g_ua.cfg.own_gadget, strerror(-r));
    else mpcua_log("%s: removed our gadget '%s'", why, g_ua.cfg.own_gadget);
  }
  pthread_mutex_unlock(&own_lock);
}

/* Firmware where MPC builds no gadget in standalone mode (an MPC One on HAKAI 3.9.1, issue #4): when
 * none has appeared after own_wait seconds, build our own and start the forwarder on it. MPC builds
 * its own within a second or two where it builds one at all. For a minute after, give way at once to
 * any other gadget; MPC's own (through usbg_enable_gadget) is handled before it binds. MPC restarts on
 * a switch to controller mode, and the next process removes ours in its constructor. */
static void *own_gadget_main(void *arg) {
  (void)arg;
  prctl(PR_SET_NAME, "usbaudio-own", 0, 0, 0);
  for (int i = 0; i < g_ua.cfg.own_wait; i++) {
    if (atomic_load(&g_ua.gadget_seen)) return NULL;
    struct timespec ts = {1, 0};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
  }
  if (atomic_load(&g_ua.gadget_seen) || atomic_load(&hub_internal)) return NULL;
  if (mpcua_other_gadget_present(&g_ua.cfg)) {
    mpcua_log("MPC built no gadget '%s', but another one exists: not building ours", g_ua.cfg.gadget);
    return NULL;
  }
  char err[200] = "";
  pthread_mutex_lock(&own_lock);
  int r = mpc_enabled || atomic_load(&g_ua.gadget_seen) ? 1 : mpcua_own_gadget_create(&g_ua.cfg, udc_dir(), err, sizeof err);
  if (r == 0) {
    atomic_store(&own_up, 1);
    atomic_store(&g_ua.live_gadget, (const char *)g_ua.cfg.own_gadget);
  }
  pthread_mutex_unlock(&own_lock);
  if (r > 0) return NULL;
  if (r < 0) { mpcua_log("own gadget '%s' not built: %s", g_ua.cfg.own_gadget, err); return NULL; }
  mpcua_log("MPC built no gadget '%s' in %d s: built our own, '%s'", g_ua.cfg.gadget, g_ua.cfg.own_wait,
            g_ua.cfg.own_gadget);
  atomic_store(&g_ua.gadget_seen, 1);
  if (g_ua.alsa_ok) mpcua_fwd_start();
  for (int i = 0; i < 600 && atomic_load(&own_up); i++) {   /* a minute, every 100 ms */
    struct timespec ts = {0, 100 * 1000 * 1000};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
    if (mpcua_other_gadget_present(&g_ua.cfg)) { own_remove("another gadget appeared"); break; }
    /* Backstop for a model whose device port is behind the internal hub but has no mux node: three
     * seconds after binding, our gadget shows up on this device's own USB bus. No computer hears it. */
    if (i == 30 && mpcua_gadget_on_own_bus(usb_dir(), MPCUA_OWN_VID, MPCUA_OWN_PID, MPCUA_OWN_MANUFACTURER)) {
      atomic_store(&hub_internal, 1);
      own_remove("this device enumerated our gadget itself: its USB-B socket is behind the internal USB hub, "
                 "so no computer hears a gadget in standalone mode");
      break;
    }
  }
  return NULL;
}

/* MPC exits: take our gadget away with it, so the computer doesn't keep a silent audio device. */
__attribute__((destructor)) static void mpcua_dtor(void) {
  if (atomic_load(&own_up)) own_remove("MPC exiting");
}

__attribute__((constructor)) static void mpcua_ctor(void) {
  resolve_reals();
  if (!exe_is_mpc()) return;

  char dir[128], defpath[160];   /* dir + "/usbaudio.log" fits log_path */
  addin_dir(dir, sizeof dir);
  snprintf(defpath, sizeof defpath, "%s/%s", dir[0] ? dir : ".", MPCUA_CONF_NAME);
  const char *path = getenv("MPC_USB_AUDIO_CONF");
  if (!path || !*path) path = defpath;
  char err[160] = "";
  int bad = mpcua_cfg_load(&g_ua.cfg, path, err, sizeof err);
  if (!strcmp(g_ua.cfg.log_path, "auto"))
    snprintf(g_ua.cfg.log_path, sizeof g_ua.cfg.log_path, "%s/usbaudio.log", dir[0] ? dir : ".");
  mpcua_log_open(g_ua.cfg.log_path);
  if (bad < 0) mpcua_log("config %s unreadable, using defaults", path);
  else if (bad) mpcua_log("config %s: %d problem(s), first: %s", path, bad, err);
  else mpcua_log("config %s", path);
  /* Our gadget outlives a process that was killed, and would hold the UDC against the gadget MPC is
   * about to build (after a switch to controller mode, say): remove it first, enabled or not. */
  int left = mpcua_own_gadget_remove(&g_ua.cfg);
  if (left > 0) mpcua_log("removed our gadget '%s' left from the last run", g_ua.cfg.own_gadget);
  else if (left < 0) mpcua_log("our gadget '%s' from the last run is still there: %s", g_ua.cfg.own_gadget, strerror(-left));
  atomic_store(&g_ua.live_gadget, (const char *)g_ua.cfg.gadget);
  if (!g_ua.cfg.enabled) { mpcua_log("disabled in config"); return; }

  int missing = mpcua_alsa_resolve(&g_ua.alsa);
  g_ua.alsa_ok = missing == 0;
  if (missing) mpcua_log("%d libasound symbols missing; forwarder disabled", missing);

  mpcua_ring_init(&g_ua.out_ring, out_store, RING_SAMPLES);
  mpcua_ring_init(&g_ua.in_ring, in_store, RING_SAMPLES);
  mpcua_ring_init(&g_ua.host_ring, host_store, RING_SAMPLES);
  for (int i = 0; i < g_ua.cfg.n_to_host; i++)
    if (g_ua.cfg.to_host_map[i].kind == MPCUA_SRC_IN) g_ua.need_in = 1;
  atomic_store(&g_ua.tap_card, resolve_tap_card(&g_ua.cfg));
  g_ua.active = 1;
  mpcua_send_enable(g_ua.cfg.n_send);
  if (g_ua.cfg.n_send)
    mpcua_log("send bus: %u channels for the USB Out plugin (send1..send%u)", g_ua.cfg.n_send, g_ua.cfg.n_send);
  int tap = atomic_load(&g_ua.tap_card);
  if (tap >= 0)
    mpcua_log("active: gadget '%s', %d ch to computer, %u from it, %u Hz, codec card %d",
              g_ua.cfg.gadget, g_ua.cfg.n_to_host, g_ua.cfg.host_channels, g_ua.cfg.rate, tap);
  else
    mpcua_log("active: gadget '%s', %d ch to computer, %u from it, %u Hz, no platform codec: "
              "the first card MPC opens", g_ua.cfg.gadget, g_ua.cfg.n_to_host, g_ua.cfg.host_channels,
              g_ua.cfg.rate);
  if (g_ua.cfg.wide_out || g_ua.cfg.wide_in)
    mpcua_log("wide codec: MPC may open up to %u playback / %u capture channels (0 = as the codec has)",
              g_ua.cfg.wide_out, g_ua.cfg.wide_in);
  if (g_ua.cfg.hs_bint != g_ua.cfg.hs_bint_cfg)
    mpcua_log("hs_bint %u -> %u: %d/%u channels at %u bytes would not fit a 1024-byte USB packet",
              g_ua.cfg.hs_bint_cfg, g_ua.cfg.hs_bint, g_ua.cfg.n_to_host, g_ua.cfg.host_channels,
              g_ua.cfg.sample_bytes);
  char mux[32] = "";
  int hm = g_ua.cfg.hub_mux_check ? mpcua_hub_mux_state(mux_dir(), mux, sizeof mux) : 0;
  if (hm > 0 && !strcmp(mux, "internal")) {
    atomic_store(&hub_internal, 1);
    mpcua_log("no USB audio on this model: its USB hub mux is 'internal' (%s/state). The USB-B socket is wired to "
              "the internal USB hub, which a computer gets only in Computer Mode; in standalone mode a gadget here is "
              "enumerated by the device itself (the Force; by their device trees also the Live II and X). For audio "
              "to a computer in standalone mode see the force-link-audio addin (network). hub_mux_check=0 builds the "
              "gadget anyway", mux_dir());
  } else if (hm > 0) {
    mpcua_log("USB hub mux is '%s': a computer holds the internal hub; building the gadget as usual", mux);
  }
  int e = mpcua_spawn(gadget_watch);
  if (e) mpcua_log("gadget watch thread: %s", strerror(e));
  if (g_ua.cfg.standalone_create && (e = mpcua_spawn(own_gadget_main)))
    mpcua_log("own gadget thread: %s", strerror(e));
}

/* ---- gadget hook (MPC setup thread) --------------------------------------------------------- */

typedef struct usbg_gadget usbg_gadget;
typedef struct usbg_udc usbg_udc;
typedef struct usbg_config usbg_config;
typedef struct usbg_function usbg_function;

static int (*real_enable_gadget)(usbg_gadget *, usbg_udc *);

static void add_uac2(usbg_gadget *g) {
  const char *(*get_name)(usbg_gadget *) = dlsym(RTLD_NEXT, "usbg_get_gadget_name");
  usbg_config *(*first_config)(usbg_gadget *) = dlsym(RTLD_NEXT, "usbg_get_first_config");
  usbg_function *(*get_function)(usbg_gadget *, int, const char *) = dlsym(RTLD_NEXT, "usbg_get_function");
  int (*create_function)(usbg_gadget *, int, const char *, void *, usbg_function **) =
      dlsym(RTLD_NEXT, "usbg_create_function");
  int (*add_config_function)(usbg_config *, const char *, usbg_function *) =
      dlsym(RTLD_NEXT, "usbg_add_config_function");
  int (*rm_function)(usbg_function *, int) = dlsym(RTLD_NEXT, "usbg_rm_function");
  int (*lookup_type)(const char *) = dlsym(RTLD_NEXT, "usbg_lookup_function_type");
  if (!get_name || !first_config || !get_function || !create_function || !add_config_function ||
      !rm_function || !lookup_type) {
    mpcua_log("libusbgx symbols missing; not adding USB audio");
    return;
  }
  const char *name = get_name(g);
  if (!name || strcmp(name, g_ua.cfg.gadget) != 0) {
    mpcua_log("gadget '%s' enabled: not ours, left alone", name ? name : "?");
    return;
  }
  int type = lookup_type("uac2");
  usbg_config *c = first_config(g);
  if (type < 0 || !c) { mpcua_log("no uac2 type (%d) or no config", type); return; }

  usbg_function *f = get_function(g, type, g_ua.cfg.instance);
  if (f) {
    mpcua_log("uac2.%s already present", g_ua.cfg.instance);
  } else {
    int r = create_function(g, type, g_ua.cfg.instance, NULL, &f);
    if (r != 0 || !f) { mpcua_log("usbg_create_function(uac2) failed: %d", r); return; }
    char fdir[320], err[160] = "";
    snprintf(fdir, sizeof fdir, "%s/%s/functions/uac2.%s", g_ua.cfg.configfs, name, g_ua.cfg.instance);
    if (mpcua_uac2_write_attrs(fdir, &g_ua.cfg, err, sizeof err) < 0) {
      mpcua_log("uac2 attributes: %s; removing function", err);
      rm_function(f, 1 /* USBG_RM_RECURSE */);
      return;
    }
    if (err[0]) mpcua_log("uac2 optional attribute: %s", err);
    r = add_config_function(c, NULL, f);
    if (r != 0) {
      mpcua_log("usbg_add_config_function(uac2) failed: %d; removing function", r);
      rm_function(f, 1);
      return;
    }
    if (g_ua.cfg.iad_class) {
      char gdir[256];
      snprintf(gdir, sizeof gdir, "%s/%s", g_ua.cfg.configfs, name);
      int e = mpcua_gadget_set_iad_class(gdir);
      if (e) mpcua_log("device class EF/02/01 not set: %s", strerror(-e));
    }
    mpcua_log("uac2.%s added to gadget '%s'", g_ua.cfg.instance, name);
  }
  atomic_store(&g_ua.live_gadget, (const char *)g_ua.cfg.gadget);
  atomic_store(&g_ua.gadget_seen, 1);
  if (g_ua.alsa_ok) mpcua_fwd_start();
}

EXPORT int usbg_enable_gadget(usbg_gadget *g, usbg_udc *udc) {
  if (!real_enable_gadget) *(void **)&real_enable_gadget = dlsym(RTLD_NEXT, "usbg_enable_gadget");
  if (g_ua.active && g) {
    pthread_mutex_lock(&own_lock);
    mpc_enabled = 1;
    pthread_mutex_unlock(&own_lock);
    own_remove("MPC is enabling a gadget");
  }
  if (g_ua.active && g) add_uac2(g);
  return real_enable_gadget ? real_enable_gadget(g, udc) : -1;
}

/* ---- PCM tracking (MPC setup threads) ------------------------------------------------------- */

static mpcua_stream *stream_of(snd_pcm_t *pcm) {
  if (!pcm) return NULL;
  if (atomic_load(&g_ua.play.pcm) == pcm) return &g_ua.play;
  if (atomic_load(&g_ua.cap.pcm) == pcm) return &g_ua.cap;
  return NULL;
}

/* ---- wide codec (wide_out / wide_in, MPC setup threads) -------------------------------------
 *
 * MPC sizes its outputs and inputs from what the codec PCM reports. With wide_out=N the codec's
 * playback PCM reports N channels at most (wide_in: capture), and a request for up to N is accepted:
 * the real PCM is set to its own maximum, and the hw_params structure remembers the channel count
 * MPC asked for, so MPC reads that count back. The audio hooks then pass the codec its channels and
 * stream all of them to USB.
 *
 * hw_params structures are tracked by address from snd_pcm_hw_params_any (or _current) on a tapped
 * codec PCM; any other PCM's structures are never touched. */

#define WIDE_SLOTS 16
typedef struct { const snd_pcm_hw_params_t *params; snd_pcm_t *pcm; unsigned virt; } wide_slot;
static wide_slot wide_tab[WIDE_SLOTS];
static unsigned wide_next;
static pthread_mutex_t wide_lock = PTHREAD_MUTEX_INITIALIZER;

static int (*real_hw_any)(snd_pcm_t *, snd_pcm_hw_params_t *);
static int (*real_hw_current)(snd_pcm_t *, snd_pcm_hw_params_t *);
static int (*real_get_ch)(const snd_pcm_hw_params_t *, unsigned *);
static int (*real_get_ch_max)(const snd_pcm_hw_params_t *, unsigned *);
static int (*real_test_ch)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
static int (*real_set_ch)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
static int (*real_set_ch_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned *);
static int (*real_set_ch_min)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned *);

static void resolve_wide(void) {
  if (real_set_ch) return;
  *(void **)&real_hw_any = dlsym(RTLD_NEXT, "snd_pcm_hw_params_any");
  *(void **)&real_hw_current = dlsym(RTLD_NEXT, "snd_pcm_hw_params_current");
  *(void **)&real_get_ch = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_channels");
  *(void **)&real_get_ch_max = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_channels_max");
  *(void **)&real_test_ch = dlsym(RTLD_NEXT, "snd_pcm_hw_params_test_channels");
  *(void **)&real_set_ch_near = dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_channels_near");
  *(void **)&real_set_ch_min = dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_channels_min");
  *(void **)&real_set_ch = dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_channels");
}

/* Channels MPC may see on this stream: wide_out/wide_in, or 0 when the stream is not widened. */
static unsigned wide_of(mpcua_stream *s) {
  if (!s || !g_ua.active) return 0;
  return s == &g_ua.play ? g_ua.cfg.wide_out : g_ua.cfg.wide_in;
}

static const char *what_of(mpcua_stream *s) { return s == &g_ua.play ? "playback" : "capture"; }

/* Remember (or forget, pcm NULL) a hw_params structure. */
static void slot_set(const snd_pcm_hw_params_t *params, snd_pcm_t *pcm, unsigned virt) {
  pthread_mutex_lock(&wide_lock);
  wide_slot *hit = NULL;
  for (unsigned i = 0; i < WIDE_SLOTS && !hit; i++)
    if (wide_tab[i].params == params) hit = &wide_tab[i];
  if (!hit && pcm) { hit = &wide_tab[wide_next]; wide_next = (wide_next + 1) % WIDE_SLOTS; }
  if (hit) {
    hit->params = pcm ? params : NULL;
    hit->pcm = pcm;
    hit->virt = virt;
  }
  pthread_mutex_unlock(&wide_lock);
}

/* The tracked PCM of a hw_params structure, and the channel count MPC chose on it (0: none yet). */
static snd_pcm_t *slot_get(const snd_pcm_hw_params_t *params, unsigned *virt) {
  snd_pcm_t *pcm = NULL;
  pthread_mutex_lock(&wide_lock);
  for (unsigned i = 0; i < WIDE_SLOTS; i++)
    if (wide_tab[i].params == params) { pcm = wide_tab[i].pcm; if (virt) *virt = wide_tab[i].virt; break; }
  pthread_mutex_unlock(&wide_lock);
  return pcm;
}

/* The codec's own channel maximum for this configuration space. */
static unsigned hw_max(const snd_pcm_hw_params_t *params) {
  unsigned m = 0;
  if (!real_get_ch_max || real_get_ch_max(params, &m) < 0) return 0;
  return m;
}

EXPORT int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
  resolve_wide();
  if (!real_hw_any) return -ENOSYS;
  int r = real_hw_any(pcm, params);
  if (r >= 0 && g_ua.active) slot_set(params, wide_of(stream_of(pcm)) ? pcm : NULL, 0);
  return r;
}

EXPORT int snd_pcm_hw_params_current(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
  resolve_wide();
  if (!real_hw_current) return -ENOSYS;
  int r = real_hw_current(pcm, params);
  if (r >= 0 && g_ua.active) {
    mpcua_stream *s = stream_of(pcm);
    int wide = wide_of(s) && atomic_load(&s->wide_hw);
    slot_set(params, wide ? pcm : NULL, wide ? s->ch : 0);
  }
  return r;
}

EXPORT int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *params, unsigned *val) {
  resolve_wide();
  if (!real_get_ch_max) return -ENOSYS;
  int r = real_get_ch_max(params, val);
  snd_pcm_t *pcm = r >= 0 && g_ua.active ? slot_get(params, NULL) : NULL;
  mpcua_stream *s = pcm ? stream_of(pcm) : NULL;
  unsigned n = wide_of(s);
  if (n > *val) {
    static unsigned told[2];
    unsigned *t = &told[s == &g_ua.cap];
    if (*t != n) { *t = n; mpcua_log("%s: MPC asked for the codec's channel count: told it %u (the codec has %u)", what_of(s), n, *val); }
    *val = n;
  }
  return r;
}

EXPORT int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *params, unsigned *val) {
  resolve_wide();
  if (!real_get_ch) return -ENOSYS;
  int r = real_get_ch(params, val);
  unsigned virt = 0;
  if (g_ua.active && slot_get(params, &virt) && virt) { *val = virt; return 0; }
  return r;
}

EXPORT int snd_pcm_hw_params_test_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned val) {
  resolve_wide();
  if (!real_test_ch) return -ENOSYS;
  unsigned n = wide_of(stream_of(pcm)), m = n ? hw_max(params) : 0;
  if (n && m && val > m && val <= n) return real_test_ch(pcm, params, m);
  return real_test_ch(pcm, params, val);
}

/* MPC asks for `want` channels on a widened stream: give the codec its maximum, remember `want`. */
static int widen(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned want, int (*set)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned)) {
  mpcua_stream *s = stream_of(pcm);
  unsigned m = hw_max(params);
  int r = set(pcm, params, m);
  if (r >= 0) {
    slot_set(params, pcm, want);
    mpcua_log("%s: MPC asked for %u channels; the codec gets %u, channels %u-%u go to USB only", what_of(s), want, m, m + 1, want);
  }
  return r;
}

static int set_exact(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned v) { return real_set_ch(pcm, params, v); }
static int set_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned v) { return real_set_ch_near(pcm, params, &v); }
static int set_min(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned v) { return real_set_ch_min(pcm, params, &v); }

EXPORT int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned val) {
  resolve_wide();
  if (!real_set_ch) return -ENOSYS;
  mpcua_stream *s = stream_of(pcm);
  unsigned n = wide_of(s), m = n ? hw_max(params) : 0;
  if (n) mpcua_log("%s: MPC sets %u channels (wide_%s=%u, codec max %u)", what_of(s), val, s == &g_ua.play ? "out" : "in", n, m);
  if (n && m && val > m && val <= n) return widen(pcm, params, val, set_exact);
  return real_set_ch(pcm, params, val);
}

EXPORT int snd_pcm_hw_params_set_channels_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned *val) {
  resolve_wide();
  if (!real_set_ch_near) return -ENOSYS;
  unsigned n = wide_of(stream_of(pcm)), m = n ? hw_max(params) : 0;
  if (n && m && *val > m) {
    unsigned want = *val < n ? *val : n;
    int r = widen(pcm, params, want, set_near);
    if (r >= 0) *val = want;
    return r;
  }
  return real_set_ch_near(pcm, params, val);
}

EXPORT int snd_pcm_hw_params_set_channels_min(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned *val) {
  resolve_wide();
  if (!real_set_ch_min) return -ENOSYS;
  unsigned n = wide_of(stream_of(pcm)), m = n ? hw_max(params) : 0;
  if (n && m && *val > m && *val <= n) return widen(pcm, params, *val, set_min);
  return real_set_ch_min(pcm, params, val);
}

/* Byte/frame conversions on a widened PCM follow MPC's channel count, so buffers MPC sizes with them
 * hold its wide frames. */
static snd_pcm_sframes_t (*real_f2b)(snd_pcm_t *, snd_pcm_sframes_t);
static snd_pcm_sframes_t (*real_b2f)(snd_pcm_t *, ssize_t);

static mpcua_stream *wide_stream(snd_pcm_t *pcm) {
  mpcua_stream *s = g_ua.active ? stream_of(pcm) : NULL;
  return s && atomic_load(&s->wide_hw) && mpcua_fmt_bytes(s->fmt) ? s : NULL;
}

EXPORT ssize_t snd_pcm_frames_to_bytes(snd_pcm_t *pcm, snd_pcm_sframes_t frames) {
  if (!real_f2b) *(void **)&real_f2b = dlsym(RTLD_NEXT, "snd_pcm_frames_to_bytes");
  mpcua_stream *s = wide_stream(pcm);
  if (s) return (ssize_t)frames * (ssize_t)(mpcua_fmt_bytes(s->fmt) * s->ch);
  return real_f2b ? real_f2b(pcm, frames) : -ENOSYS;
}

EXPORT snd_pcm_sframes_t snd_pcm_bytes_to_frames(snd_pcm_t *pcm, ssize_t bytes) {
  if (!real_b2f) *(void **)&real_b2f = dlsym(RTLD_NEXT, "snd_pcm_bytes_to_frames");
  mpcua_stream *s = wide_stream(pcm);
  if (s) return (snd_pcm_sframes_t)(bytes / (ssize_t)(mpcua_fmt_bytes(s->fmt) * s->ch));
  return real_b2f ? real_b2f(pcm, bytes) : -ENOSYS;
}

EXPORT int snd_pcm_open(snd_pcm_t **pcmp, const char *name, int stream, int mode) {
  if (!real_open) resolve_reals();
  if (!real_open) return -ENOSYS;
  int r = real_open(pcmp, name, stream, mode);
  if (r < 0 || !g_ua.active || !g_ua.alsa.info_sizeof || !g_ua.alsa.info || !g_ua.alsa.info_get_card)
    return r;
  snd_pcm_info_t *info = alloca(g_ua.alsa.info_sizeof());
  memset(info, 0, g_ua.alsa.info_sizeof());
  if (g_ua.alsa.info(*pcmp, info) < 0) return r;
  int card = g_ua.alsa.info_get_card(info);
  int tap = atomic_load(&g_ua.tap_card);
  if (tap < 0) {   /* tap_card=auto and no platform codec: MPC's first card is its audio */
    if (card < 0 || is_gadget_card(card)) return r;
    if (atomic_compare_exchange_strong(&g_ua.tap_card, &tap, card))
      mpcua_log("codec card %d: the first card MPC opened ('%s')", card, name ? name : "?");
    tap = atomic_load(&g_ua.tap_card);
  }
  if (card != tap) return r;
  mpcua_stream *s = stream == MPCUA_PCM_PLAYBACK ? &g_ua.play : &g_ua.cap;
  atomic_store(&s->ok, 0);
  atomic_store(&s->wide_hw, 0);
  atomic_store(&s->pcm, *pcmp);
  atomic_fetch_add(&s->gen, 1);
  mpcua_log("tapping %s PCM '%s' (card %d)", stream == MPCUA_PCM_PLAYBACK ? "playback" : "capture",
            name ? name : "?", card);
  return r;
}

EXPORT int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
  if (!real_hw_params) resolve_reals();
  if (!real_hw_params) return -ENOSYS;
  mpcua_stream *s = g_ua.active ? stream_of(pcm) : NULL;
  if (s) atomic_store(&s->ok, 0);
  int r = real_hw_params(pcm, params);
  if (!s || r < 0) return r;
  const mpcua_alsa *a = &g_ua.alsa;
  int fmt = -1, access = -1, dir = 0;
  unsigned ch = 0, rate = 0;
  if (!a->hw_get_format || !a->hw_get_channels || !a->hw_get_rate || !a->hw_get_access) return r;
  a->hw_get_format(params, &fmt);
  a->hw_get_channels(params, &ch);
  a->hw_get_rate(params, &rate, &dir);
  a->hw_get_access(params, &access);
  unsigned virt = 0, codec_ch = 0;
  atomic_store(&s->wide_hw, 0);
  if (wide_of(s) && slot_get(params, &virt) && virt > ch) { codec_ch = ch; ch = virt; }   /* MPC reads/writes virt */
  s->fmt = mpcua_fmt_from_alsa(fmt);
  s->ch = ch;
  s->rate = rate;
  s->interleaved = access == MPCUA_ACCESS_RW_INTERLEAVED;
  atomic_store_explicit(&s->wide_hw, codec_ch, memory_order_release);
  atomic_fetch_add(&s->gen, 1);
  const char *what = s == &g_ua.play ? "playback" : "capture";
  if (s->fmt == MPCUA_FMT_UNKNOWN || ch < 1 || ch > MPCUA_MAX_CH ||
      (access != MPCUA_ACCESS_RW_INTERLEAVED && access != MPCUA_ACCESS_RW_NONINTERLEAVED)) {
    mpcua_log("%s: unsupported format %d / %u ch / access %d; not tapping", what, fmt, ch, access);
    if (atomic_load(&s->wide_hw))
      mpcua_log("%s: MPC opened it wide but the format is unknown: its extra channels cannot be split off", what);
    return r;
  }
  if (rate != g_ua.cfg.rate) {
    mpcua_log("%s: MPC runs at %u Hz but the gadget is set to %u Hz; not tapping", what, rate,
              g_ua.cfg.rate);
    return r;
  }
  atomic_store_explicit(&s->ok, 1, memory_order_release);
  unsigned hw = atomic_load(&s->wide_hw);
  if (hw)
    mpcua_log("%s: %u ch as MPC sees it (codec %u, the rest USB only), format %d, %u Hz, %s", what, ch, hw, fmt,
              rate, s->interleaved ? "interleaved" : "non-interleaved");
  else
    mpcua_log("%s: %u ch, format %d, %u Hz, %s", what, ch, fmt, rate,
              s->interleaved ? "interleaved" : "non-interleaved");
  return r;
}

EXPORT int snd_pcm_close(snd_pcm_t *pcm) {
  if (!real_close) resolve_reals();
  if (!real_close) return -ENOSYS;
  mpcua_stream *s = g_ua.active ? stream_of(pcm) : NULL;
  if (s) {
    atomic_store(&s->ok, 0);
    atomic_store(&s->pcm, NULL);
    atomic_store(&s->wide_hw, 0);
    atomic_fetch_add(&s->gen, 1);
  }
  return real_close(pcm);
}

/* ---- audio thread ---------------------------------------------------------------------------- */

static inline int tapping(mpcua_stream *s) {
  return atomic_load_explicit(&s->ok, memory_order_acquire) &&
         atomic_load_explicit(&g_ua.fwd_running, memory_order_relaxed);
}

/* Statistics for the 10 s log line: the stream's largest |sample| in what was just tapped. */
static void note_peak(mpcua_stream *s, const int32_t *p, uint32_t n) {
  uint32_t m = atomic_load_explicit(&s->peak, memory_order_relaxed);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t a = p[i] < 0 ? (uint32_t)0 - (uint32_t)p[i] : (uint32_t)p[i];
    if (a > m) m = a;
  }
  atomic_store_explicit(&s->peak, m, memory_order_relaxed);
}

/* Interleaved buffer -> ring as int32. */
static void tap_i(mpcua_stream *s, mpcua_ring *q, const void *buf, uint32_t frames,
                  _Atomic unsigned long *ovf) {
  unsigned ch = s->ch;
  uint32_t fit = mpcua_ring_space(q) / ch;
  if (frames > fit) { frames = fit; atomic_fetch_add_explicit(ovf, 1, memory_order_relaxed); }
  if (!frames) return;
  int32_t *p1, *p2; uint32_t n1, n2;
  mpcua_ring_wseg(q, frames * ch, &p1, &n1, &p2, &n2);
  mpcua_fmt_to_s32(buf, s->fmt, p1, n1);
  if (n2) mpcua_fmt_to_s32((const char *)buf + (size_t)n1 * mpcua_fmt_bytes(s->fmt), s->fmt, p2, n2);
  note_peak(s, p1, n1); note_peak(s, p2, n2);
  atomic_fetch_add_explicit(&s->frames, frames, memory_order_relaxed);
  mpcua_ring_wcommit(q, frames * ch);
}

/* Non-interleaved buffers -> ring as interleaved int32. */
static void tap_n(mpcua_stream *s, mpcua_ring *q, void **bufs, uint32_t frames,
                  _Atomic unsigned long *ovf) {
  unsigned ch = s->ch;
  uint32_t fit = mpcua_ring_space(q) / ch;
  if (frames > fit) { frames = fit; atomic_fetch_add_explicit(ovf, 1, memory_order_relaxed); }
  if (!frames) return;
  int32_t *p[2]; uint32_t n[2];
  mpcua_ring_wseg(q, frames * ch, &p[0], &n[0], &p[1], &n[1]);
  uint32_t j = 0;
  for (int seg = 0; seg < 2; seg++)
    for (uint32_t k = 0; k < n[seg]; k++, j++)
      p[seg][k] = bufs[j % ch] ? mpcua_fmt_get(bufs[j % ch], s->fmt, j / ch) : 0;
  note_peak(s, p[0], n[0]); note_peak(s, p[1], n[1]);
  atomic_fetch_add_explicit(&s->frames, frames, memory_order_relaxed);
  mpcua_ring_wcommit(q, frames * ch);
}

/* Main out with send channels: each frame is MPC's playback channels, then the send bus's channels
 * for the same moment. `bufs` is NULL for an interleaved `buf`. The bus is always taken (so it never
 * fills with stale audio), even while nothing is tapped. */
#define SEND_CHUNK 128
static int32_t send_frames[SEND_CHUNK * MPCUA_MAX_CH * 2];

static void tap_sends(mpcua_stream *s, const void *buf, void **bufs, uint32_t frames) {
  unsigned ns = g_ua.cfg.n_send, pc = s->ch, w = pc + ns;
  int tap = tapping(s) && s->fmt != MPCUA_FMT_UNKNOWN;
  for (uint32_t done = 0; done < frames;) {
    uint32_t k = frames - done < SEND_CHUNK ? frames - done : SEND_CHUNK;
    if (!tap) { mpcua_send_take(NULL, 0, ns, k); done += k; continue; }
    for (uint32_t i = 0; i < k; i++)
      for (unsigned c = 0; c < pc; c++) {
        size_t j = (size_t)(done + i);
        send_frames[(size_t)i * w + c] =
            bufs ? (bufs[c] ? mpcua_fmt_get(bufs[c], s->fmt, j) : 0) : mpcua_fmt_get(buf, s->fmt, j * pc + c);
      }
    mpcua_send_take(send_frames + pc, w, ns, k);
    note_peak(s, send_frames, k * w);
    uint32_t put = mpcua_ring_write_frames(&g_ua.out_ring, send_frames, k, w);
    if (put < k) atomic_fetch_add_explicit(&g_ua.ovf_out, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->frames, put, memory_order_relaxed);
    done += k;
  }
}

/* Mix (or replace) computer audio into what MPC just read from its inputs. `bufs` is NULL for an
 * interleaved `buf`. */
static void inject(void *buf, void **bufs, uint32_t frames) {
  mpcua_stream *s = &g_ua.cap;
  int mode = g_ua.cfg.input_mode;
  if (mode == MPCUA_INPUT_OFF || !atomic_load_explicit(&s->ok, memory_order_acquire)) return;
  unsigned ch = s->ch;
  if (!atomic_load_explicit(&g_ua.host_live, memory_order_acquire)) {
    mpcua_ring_trim(&g_ua.host_ring, 0, ch);   /* consumer side: drop anything stale */
    return;
  }
  uint32_t have = mpcua_ring_fill(&g_ua.host_ring) / ch;
  uint32_t k = frames < have ? frames : have;
  if (k < frames) atomic_fetch_add_explicit(&g_ua.und_host, 1, memory_order_relaxed);
  if (!k) return;
  const int32_t *p[2]; uint32_t n[2];
  mpcua_ring_rseg(&g_ua.host_ring, k * ch, &p[0], &n[0], &p[1], &n[1]);
  uint32_t j = 0;
  for (int seg = 0; seg < 2; seg++)
    for (uint32_t i = 0; i < n[seg]; i++, j++) {
      void *base = buf; size_t idx = j;
      if (bufs) { base = bufs[j % ch]; idx = j / ch; if (!base) continue; }
      if (mode == MPCUA_INPUT_SUM) mpcua_mix_add(base, s->fmt, idx, p[seg][i]);
      else mpcua_fmt_put(base, s->fmt, idx, p[seg][i]);
    }
  mpcua_ring_rcommit(&g_ua.host_ring, k * ch);
}

/* Wide streams: MPC's buffers carry s->ch channels, the codec takes hw of them. These run on MPC's
 * audio thread: static buffers, plain copies, no locks. One buffer per direction (MPC has one
 * playback and one capture thread at most on the codec). */
#define WIDE_BUF_BYTES 65536
static unsigned char wide_wbuf[WIDE_BUF_BYTES], wide_rbuf[WIDE_BUF_BYTES];

static snd_pcm_sframes_t wide_writei(mpcua_stream *s, snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t n,
                                     unsigned hw) {
  size_t sb = mpcua_fmt_bytes(s->fmt), vf = sb * s->ch, hf = sb * hw;
  if (!sb || s->ch <= hw) return real_writei(pcm, buf, n);
  snd_pcm_uframes_t cap = WIDE_BUF_BYTES / hf, done = 0;
  while (done < n) {
    snd_pcm_uframes_t k = n - done < cap ? n - done : cap;
    const unsigned char *src = (const unsigned char *)buf + done * vf;
    for (snd_pcm_uframes_t i = 0; i < k; i++) memcpy(wide_wbuf + i * hf, src + i * vf, hf);
    snd_pcm_sframes_t r = real_writei(pcm, wide_wbuf, k);
    if (r < 0) return done ? (snd_pcm_sframes_t)done : r;
    done += (snd_pcm_uframes_t)r;
    if ((snd_pcm_uframes_t)r < k) break;
  }
  return (snd_pcm_sframes_t)done;
}

static snd_pcm_sframes_t wide_readi(mpcua_stream *s, snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t n, unsigned hw) {
  size_t sb = mpcua_fmt_bytes(s->fmt), vf = sb * s->ch, hf = sb * hw;
  if (!sb || s->ch <= hw) return real_readi(pcm, buf, n);
  snd_pcm_uframes_t cap = WIDE_BUF_BYTES / hf, done = 0;
  while (done < n) {
    snd_pcm_uframes_t k = n - done < cap ? n - done : cap;
    snd_pcm_sframes_t r = real_readi(pcm, wide_rbuf, k);
    if (r < 0) return done ? (snd_pcm_sframes_t)done : r;
    unsigned char *dst = (unsigned char *)buf + done * vf;
    for (snd_pcm_sframes_t i = 0; i < r; i++) {
      memcpy(dst + (size_t)i * vf, wide_rbuf + (size_t)i * hf, hf);
      memset(dst + (size_t)i * vf + hf, 0, vf - hf);   /* inputs beyond the codec: silence until the computer fills them */
    }
    done += (snd_pcm_uframes_t)r;
    if ((snd_pcm_uframes_t)r < k) break;
  }
  return (snd_pcm_sframes_t)done;
}

EXPORT snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t n) {
  if (!real_writei) return -ENOSYS;
  int mine = pcm == atomic_load_explicit(&g_ua.play.pcm, memory_order_relaxed);
  unsigned hw = mine ? atomic_load_explicit(&g_ua.play.wide_hw, memory_order_acquire) : 0;
  snd_pcm_sframes_t r = hw ? wide_writei(&g_ua.play, pcm, buf, n, hw) : real_writei(pcm, buf, n);
  if (r > 0 && mine) atomic_fetch_add_explicit(&g_ua.play.calls, 1, memory_order_relaxed);
  if (r > 0 && mine && g_ua.cfg.n_send && g_ua.play.interleaved)
    tap_sends(&g_ua.play, buf, NULL, (uint32_t)r);
  else if (r > 0 && mine && tapping(&g_ua.play) && g_ua.play.interleaved)
    tap_i(&g_ua.play, &g_ua.out_ring, buf, (uint32_t)r, &g_ua.ovf_out);
  return r;
}

/* Non-interleaved: the codec simply uses the first hw of MPC's buffers. */
EXPORT snd_pcm_sframes_t snd_pcm_writen(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t n) {
  if (!real_writen) return -ENOSYS;
  snd_pcm_sframes_t r = real_writen(pcm, bufs, n);
  if (r > 0 && pcm == atomic_load_explicit(&g_ua.play.pcm, memory_order_relaxed))
    atomic_fetch_add_explicit(&g_ua.play.calls, 1, memory_order_relaxed);
  if (r > 0 && pcm == atomic_load_explicit(&g_ua.play.pcm, memory_order_relaxed) && !g_ua.play.interleaved && bufs) {
    if (g_ua.cfg.n_send) tap_sends(&g_ua.play, NULL, bufs, (uint32_t)r);
    else if (tapping(&g_ua.play)) tap_n(&g_ua.play, &g_ua.out_ring, bufs, (uint32_t)r, &g_ua.ovf_out);
  }
  return r;
}

EXPORT snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t n) {
  if (!real_readi) return -ENOSYS;
  int mine = pcm == atomic_load_explicit(&g_ua.cap.pcm, memory_order_relaxed);
  unsigned hw = mine ? atomic_load_explicit(&g_ua.cap.wide_hw, memory_order_acquire) : 0;
  snd_pcm_sframes_t r = hw ? wide_readi(&g_ua.cap, pcm, buf, n, hw) : real_readi(pcm, buf, n);
  if (r > 0 && mine) atomic_fetch_add_explicit(&g_ua.cap.calls, 1, memory_order_relaxed);
  if (r > 0 && mine && g_ua.cap.interleaved) {
    if (g_ua.need_in && tapping(&g_ua.cap)) tap_i(&g_ua.cap, &g_ua.in_ring, buf, (uint32_t)r, &g_ua.ovf_in);
    inject(buf, NULL, (uint32_t)r);
  }
  return r;
}

EXPORT snd_pcm_sframes_t snd_pcm_readn(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t n) {
  if (!real_readn) return -ENOSYS;
  snd_pcm_sframes_t r = real_readn(pcm, bufs, n);
  int mine = pcm == atomic_load_explicit(&g_ua.cap.pcm, memory_order_relaxed);
  if (r > 0 && mine) atomic_fetch_add_explicit(&g_ua.cap.calls, 1, memory_order_relaxed);
  unsigned hw = mine ? atomic_load_explicit(&g_ua.cap.wide_hw, memory_order_acquire) : 0;
  if (r > 0 && hw && bufs) {   /* the codec filled the first hw buffers; the rest start silent */
    size_t sb = mpcua_fmt_bytes(g_ua.cap.fmt);
    for (unsigned c = hw; c < g_ua.cap.ch; c++) if (bufs[c]) memset(bufs[c], 0, (size_t)r * sb);
  }
  if (r > 0 && bufs && mine && !g_ua.cap.interleaved) {
    if (g_ua.need_in && tapping(&g_ua.cap)) tap_n(&g_ua.cap, &g_ua.in_ring, bufs, (uint32_t)r, &g_ua.ovf_in);
    inject(NULL, bufs, (uint32_t)r);
  }
  return r;
}

/* ---- test-only introspection (x86 host test build) ------------------------------------------ */
#ifdef MPCUA_TEST_HOOKS
EXPORT int mpcua_test_active(void) { return g_ua.active; }
EXPORT void mpcua_test_set(int fwd_running, int host_live) {
  atomic_store(&g_ua.fwd_running, fwd_running);
  atomic_store(&g_ua.host_live, host_live);
}
EXPORT uint32_t mpcua_test_pop(int which, int32_t *dst, uint32_t frames, unsigned ch) {
  return mpcua_ring_read_frames(which ? &g_ua.in_ring : &g_ua.out_ring, dst, frames, ch);
}
EXPORT uint32_t mpcua_test_push_host(const int32_t *src, uint32_t frames, unsigned ch) {
  return mpcua_ring_write_frames(&g_ua.host_ring, src, frames, ch);
}
#endif
