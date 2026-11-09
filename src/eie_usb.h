/* eie_usb.h: user-space USB engine for the Akai EIE Pro (USB 09e8:0010).
 *
 * The EIE Pro is a vendor-specific USB 2.0 device: 4 in / 4 out, always 24-bit,
 * 44.1 / 48 / 88.2 / 96 kHz, input and output at the same rate. This engine talks
 * to it with libusb from an ordinary process (no kernel extension, no DriverKit).
 *
 * Audio is exchanged as interleaved 32-bit float, 4 channels, in [-1, 1].
 * Callbacks run on the engine's USB thread: keep them short and never block.
 */
#ifndef EIE_USB_H
#define EIE_USB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EIE_CHANNELS 4

typedef struct eie eie_t;

/* Fill `out` with `frames` interleaved 4-channel frames. `pos` is the timeline
 * position of the first frame: it plays when the box's clock reaches it. */
typedef void (*eie_render_fn)(void *ctx, float *out, int frames, long long pos);
/* Receive `frames` interleaved 4-channel frames from the inputs, `pos` being the
 * timeline position (box clock) at which the first one was captured. */
typedef void (*eie_capture_fn)(void *ctx, const float *in, int frames, long long pos);

/* The box's clock as Core Audio wants it: timeline position `frames` happened at
 * mach_absolute_time() `host`; `seed` changes when the timeline restarts. */
typedef struct {
    int valid;
    long long frames;
    double host;
    double ticks_per_frame;
    unsigned seed;
} eie_clock_t;

typedef struct {
    long clock_frames;   /* frames the box says it has played (sync endpoint) */
    long clock_packets;
    long out_frames;     /* frames we sent */
    long in_frames;      /* frames we received */
    long errors;         /* failed transfers while running */
    long restarts;       /* times the watchdog restarted a stalled box */
    long lead_max, lead_min; /* frames the engine reads ahead of the box clock since (re)start */
} eie_stats_t;

/* Raw MIDI bytes received from the MIDI IN socket (USB thread). */
typedef void (*eie_midi_fn)(void *ctx, const unsigned char *bytes, int n);

/* Open the first EIE Pro. Returns NULL and writes a reason to err if none. */
eie_t *eie_open(char *err, size_t errlen);

/* Firmware version as "1.04", read at open. */
const char *eie_firmware(const eie_t *e);

/* Start streaming at `rate` (44100, 48000, 88200 or 96000). Either callback may
 * be NULL (silence out / input discarded). Returns 0 or a negative libusb error. */
int eie_start(eie_t *e, unsigned rate, eie_render_fn render, eie_capture_fn capture, void *ctx);

void eie_stop(eie_t *e);

/* Output queue length in ms (2-24, default 24), used from the next start. Shorter means
 * lower latency but less room for late USB completions. eie_queue_ms: the length
 * actually used (rounded to whole transfers). */
void eie_set_queue_ms(eie_t *e, int ms);
int eie_queue_ms(const eie_t *e);
void eie_close(eie_t *e);
void eie_get_stats(const eie_t *e, eie_stats_t *s);
/* Safe from any thread, including real-time ones. Returns 1 if a reading was taken. */
int eie_get_clock(const eie_t *e, eie_clock_t *out);
/* Diagnostics: a function the engine calls with a line of text (from its USB thread,
 * rarely: watchdog restarts), and the longest gap seen between clock packets. */
void eie_set_log(eie_t *e, void (*fn)(const char *msg));
double eie_max_clock_gap_ms(const eie_t *e);

/* MIDI. Works while audio is streaming (the box's MIDI shares its start-up).
 * eie_set_midi: callback for bytes from MIDI IN. eie_midi_send: queue raw MIDI bytes
 * for MIDI OUT; any one thread may call it; returns how many were queued. */
void eie_set_midi(eie_t *e, eie_midi_fn in, void *ctx);
int eie_midi_send(eie_t *e, const unsigned char *bytes, int n);
const char *eie_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif
