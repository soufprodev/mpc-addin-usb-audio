/* chmap.h: channel mapping between MPC's codec streams and the USB audio streams.
 *
 * Spec strings are comma-separated lists, one entry per destination channel:
 *   to_host   = out1,out2,in1,in2   USB channels sent to the computer, taken from MPC's playback
 *                                   (main out) or capture (inputs) channels, 1-based
 *                                   or a send channel (send1.., from the USB Out plugin)
 *   to_mpc_in = host1,host2         for each MPC input channel, which computer channel lands there
 * An entry of "0" or "-" is silence. A range such as out3-27 or host1-8 stands for one entry per
 * channel, in order (out3,out4,...,out27). Unknown names or out-of-range numbers are parse errors.
 */
#ifndef MPCUA_CHMAP_H
#define MPCUA_CHMAP_H

#include <stdint.h>

#define MPCUA_MAX_CH 32
/* The kernel's f_uac2 accepts channel masks up to 0x07FFFFFF: 27 channels per direction on USB. */
#define MPCUA_USB_MAX_CH 27

enum { MPCUA_SRC_NONE = 0, MPCUA_SRC_OUT = 1, MPCUA_SRC_IN = 2, MPCUA_SRC_HOST = 4, MPCUA_SRC_SEND = 8 };

/* Send channels: audio handed to the addin by "USB Out" plugin instances on MPC tracks (send.h).
 * USB channels 1/2 carry MPC's main out, so at most 25 sends fit beside them. */
#define MPCUA_SEND_MAX 25

typedef struct { uint8_t kind; uint8_t idx; /* 0-based */ } mpcua_chsrc;

/* Returns the number of entries parsed (1..max) or -1. `allowed` is a mask of MPCUA_SRC_* kinds. */
int mpcua_chmap_parse(const char *spec, unsigned allowed, mpcua_chsrc *map, int max);

/* Highest send channel a map uses (1-based), 0 when none. */
unsigned mpcua_chmap_sends(const mpcua_chsrc *map, int n);

/* Build one frame of `n` channels from an MPC out frame, an MPC in frame and a computer (host) frame.
 * The out frame holds out_ch codec channels followed by send_ch send channels.
 * Missing sources (index beyond the live channel count, or a NULL frame) give silence. */
void mpcua_chmap_frame(const mpcua_chsrc *map, int n, const int32_t *outf, unsigned out_ch, unsigned send_ch,
                       const int32_t *inf, unsigned in_ch, const int32_t *hostf, unsigned host_ch,
                       int32_t *dst);

#endif
