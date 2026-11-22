/* send.h: the send bus. "USB Out" plugin instances on MPC tracks (plugin/UsbOut) hand their track's
 * audio to the addin here; the codec playback hook takes it out again, frame for frame alongside
 * MPC's main out, so every send channel is sample-aligned with the main mix.
 *
 * Both sides run on MPC's audio threads (plugins may run on several worker threads at once): the
 * bus is lock-free. Each send channel is a ring of int32 slots that writers add into atomically (so
 * several plugins on one channel are summed), at 1/16 scale for headroom. The reader exchanges each
 * slot with 0 as it takes it. A shared position, advanced by the reader, tells writers where the
 * next frame MPC writes to the codec is.
 */
#ifndef MPCUA_SEND_H
#define MPCUA_SEND_H

#include <stdint.h>

#define MPCUA_SEND_FRAMES 4096u   /* per channel, power of two (~93 ms at 44.1 kHz) */

/* Exported for the plugin (found through a weak reference, so the plugin works without the addin).
 * first: 1-based send channel; nch: 1 or 2; bufs: nch float buffers of `frames`; cursor: the
 * calling instance's own position, 0 on its first call. Returns 0, or -1 when the bus is not running
 * (addin inactive, or the channel is beyond what to_host carries). */
int mpcua_send_v1(unsigned first, unsigned nch, const float *const *bufs, unsigned frames, uint32_t *cursor);

/* Reader (codec playback hook): the next `frames` frames of send channels 1..nsend, as full-scale
 * int32, into dst[i * stride + c]; dst NULL only clears them. Advances the bus position. */
void mpcua_send_take(int32_t *dst, unsigned stride, unsigned nsend, uint32_t frames);

/* Send channels written since the last call (bit 0 = send1), for the stats line. */
uint32_t mpcua_send_seen(void);

/* Bus on (to_host uses sends and the addin is active) or off. */
void mpcua_send_enable(unsigned nsend);

#endif
