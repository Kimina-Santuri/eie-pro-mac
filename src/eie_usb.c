/* eie_usb.c: user-space USB engine for the Akai EIE Pro. See eie_usb.h.
 *
 * Protocol facts come from the eie-pro-linux project's notes
 * (github.com/MatTaylorSharrow/eie-pro-linux, README "USB protocol") and from
 * probing a real unit (firmware 1.04). This code is written from scratch.
 *
 *   Interface 0 alt 1:  EP 0x02 iso out  audio, 4 ch x 24-bit LE packed (12 B/frame)
 *                       EP 0x83 bulk in  MIDI in    EP 0x04 bulk out  MIDI out
 *   Interface 1 alt 1:  EP 0x81 iso in   clock: 1 packet per ms, byte 0 = frames
 *                                        the box consumed since the last packet
 *                       EP 0x86 bulk in  audio, 64 B per frame, bit-sliced
 */
#include "eie_usb.h"

#include <libusb-1.0/libusb.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <mach/mach_time.h>

#ifdef EIE_TRACE
#include <stdint.h>
typedef struct { uint64_t t; char kind; int a, b, c, d; } trace_t;
static trace_t trace[200000]; static atomic_int ntrace;
#define TRACE(k,A,B,C,D) do { int _i = atomic_fetch_add(&ntrace,1); if (_i < 200000) trace[_i] = (trace_t){mach_absolute_time(), k, A, B, C, D}; } while (0)
void eie_trace_dump(const char *path) { FILE *f = fopen(path, "w"); mach_timebase_info_data_t tb; mach_timebase_info(&tb);
  int n = atomic_load(&ntrace); if (n > 200000) n = 200000; for (int i = 0; i < n; i++)
  fprintf(f, "%.3f %c %d %d %d %d\n", (double)(trace[i].t - trace[0].t) * tb.numer / tb.denom / 1e6, trace[i].kind, trace[i].a, trace[i].b, trace[i].c, trace[i].d); fclose(f); }
#else
#define TRACE(k,A,B,C,D) do {} while (0)
#endif
#define EP_AUDIO_OUT 0x02
#define EP_CLOCK_IN  0x81
#define EP_AUDIO_IN  0x86
#define EP_MIDI_IN   0x83   /* bulk: raw MIDI bytes, padded with 0xfd */
#define EP_MIDI_OUT  0x04   /* bulk: 9 B = up to 3 MIDI bytes, 0xfd padding, 0xe0 last */
#define MIDI_IN_XFERS  2
#define MIDI_OUT_XFERS 2
#define MIDI_RING      4096 /* outgoing MIDI bytes waiting for USB (power of two) */

/* 6 x 4 ms = 24 ms of output queued: inside the audio server, completions were
 * sometimes 14 ms late, and with only 15 ms queued the box ran dry and halted.
 * 4 ms transfers (was 12 x 2 ms) halve the kernel round trips for output. */
#define OUT_XFERS     6     /* iso out transfers in flight */
#define OUT_PKTS      32    /* microframes per out transfer = 4 ms */
#define OUT_MS        (OUT_PKTS / 8)
#define OUT_MAX_FRAMES (OUT_PKTS * 13)  /* 96 kHz: 12 per microframe, plus slack */
#ifndef CLOCK_XFERS
#define CLOCK_XFERS   16
#endif
#ifndef CLOCK_PKTS
#define CLOCK_PKTS    1     /* packets (ms) per clock transfer: 1, so pacing sees every ms */
#endif
#define CLOCK_PKT_SIZE 64
#ifndef IN_XFERS
#define IN_XFERS      4
#endif
/* Input arrives in 256-frame bulk transfers (5.3 ms at 48 kHz, 2.7 ms at 96): every
 * transfer costs a kernel round trip, and at 32 frames (1500 a second) input was most of
 * the driver's CPU. The extra input delay sits well inside the 32 ms safety offset. */
#ifndef IN_XFER_BYTES
#define IN_XFER_BYTES 16384 /* 256 frames */
#endif
#define IN_FRAME_BYTES 64

struct eie {
    libusb_context *ctx;
    libusb_device_handle *h;
    char firmware[8];

    unsigned rate;
    eie_render_fn render;
    eie_capture_fn capture;
    void *user;

    pthread_t thread;
    atomic_int running;     /* streaming wanted */
    atomic_int thread_on;   /* event thread alive */
    int pending;            /* transfers submitted and not yet finished (USB thread only) */
    int restarting;         /* watchdog is draining transfers to restart the box */
    long skip_in;           /* input frames still to drop after a (re)start */
    long wd_packets;        /* watchdog: clock packet count at last check */
    long wd_in, wd_out;     /* watchdog: in/out frame counts when the clock last moved */
    uint64_t last_clk;      /* abs time of the last clock packet callback */
    double max_gap_ms;      /* longest gap between clock callbacks since start */
    uint64_t last_out; double max_out_gap_ms; long out_bad_pkts, late_out;
    void (*log)(const char *msg);
    uint64_t wd_since;      /* watchdog: when that count last moved (abs time) */

    struct libusb_transfer *out[OUT_XFERS], *clk[CLOCK_XFERS], *in[IN_XFERS];
    struct libusb_transfer *min[MIDI_IN_XFERS], *mout[MIDI_OUT_XFERS];
    unsigned char *minbuf[MIDI_IN_XFERS], *moutbuf[MIDI_OUT_XFERS];
    int mout_busy[MIDI_OUT_XFERS];
    eie_midi_fn midi_in; void *midi_ctx;
    unsigned char midi_ring[MIDI_RING];
    atomic_uint midi_w, midi_r;   /* single producer (eie_midi_send), single consumer (USB thread) */
    unsigned char *outbuf[OUT_XFERS], *clkbuf[CLOCK_XFERS], *inbuf[IN_XFERS];
    float render_buf[OUT_MAX_FRAMES * EIE_CHANNELS];
    float capture_buf[(IN_XFER_BYTES / IN_FRAME_BYTES) * EIE_CHANNELS];

    /* Output pacing: the clock endpoint says how many frames the box consumed. */
    long credit;            /* frames counted by the clock, not yet sent */
    int clock_seen;
    unsigned nominal_acc;   /* fractional nominal frames per transfer (x1000) */
    int settle; long settle_sum, target; double err_avg;

    /* Clicks: silent for the first 250 ms after a (re)start while timing settles,
     * then a 20 ms fade in; eie_stop fades out over 10 ms before switching off. */
    long mute_left; float gain; atomic_int fading_out;

    /* Timeline shared with Core Audio. c_total = frames the box has consumed since
     * eie_start (kept across watchdog restarts); frame number s_total is the next one
     * sent and plays when c_total reaches it; in_pos numbers input frames on the same
     * timeline. A delay-locked loop turns clock packets into smooth host times. */
    long long c_total, s_total, in_pos; int in_pos_set;
    int dll_on; double dll_t, dll_tpf, nominal_tpf;
    atomic_uint ts_seq; long long ts_frames; double ts_host, ts_tpf; unsigned ts_seed;

    _Atomic long st_clock_frames, st_clock_packets, st_out_frames, st_in_frames, st_errors, st_restarts;
};

const char *eie_strerror(int err) { return libusb_error_name(err); }

static int ctrl(eie_t *e, uint8_t type, uint8_t req, uint16_t val, uint16_t idx,
                unsigned char *data, uint16_t len) {
    return libusb_control_transfer(e->h, type, req, val, idx, data, len, 1000);
}

eie_t *eie_open(char *err, size_t errlen) {
    eie_t *e = calloc(1, sizeof *e);
    if (!e) { snprintf(err, errlen, "out of memory"); return NULL; }
    int r = libusb_init(&e->ctx);
    if (r) { snprintf(err, errlen, "libusb_init: %s", libusb_error_name(r)); free(e); return NULL; }
    e->h = libusb_open_device_with_vid_pid(e->ctx, 0x09e8, 0x0010);
    if (!e->h) {
        snprintf(err, errlen, "no EIE Pro found (USB 09e8:0010): is it plugged in and powered?");
        libusb_exit(e->ctx); free(e); return NULL;
    }
    libusb_set_configuration(e->h, 1); /* may report busy if already configured; harmless */
    for (int i = 0; i < 2; i++) {
        r = libusb_claim_interface(e->h, i);
        if (r) {
            snprintf(err, errlen, "claim interface %d: %s (another app using the EIE Pro?)", i, libusb_error_name(r));
            if (i) libusb_release_interface(e->h, 0);
            libusb_close(e->h); libusb_exit(e->ctx); free(e); return NULL;
        }
    }
    unsigned char b[5] = {0};
    r = ctrl(e, 0xc0, 0x56, 0, 0, b, 5);
    if (r >= 3) snprintf(e->firmware, sizeof e->firmware, "%x.%02x", b[1], b[2]);
    else snprintf(e->firmware, sizeof e->firmware, "?");
    return e;
}

const char *eie_firmware(const eie_t *e) { return e->firmware; }

/* Set the sample rate and start the box, following the Windows driver's order. */
static int init_box(eie_t *e, unsigned rate) {
    unsigned char b[5] = {0};
    int r;
    if ((r = libusb_set_interface_alt_setting(e->h, 0, 1)) < 0) return r;
    if ((r = libusb_set_interface_alt_setting(e->h, 1, 1)) < 0) return r;
    if ((r = ctrl(e, 0xc0, 0x56, 0, 0, b, 3)) < 0) return r;
    if ((r = ctrl(e, 0xc0, 0x56, 0, 0, b, 5)) < 0) return r;
    if ((r = ctrl(e, 0xc0, 0x49, 0, 0, b, 1)) < 0) return r;
    if ((r = ctrl(e, 0xa2, 0x81, 0x0100, 0, b, 3)) < 0) return r;
    unsigned char rb[3] = { rate & 0xff, (rate >> 8) & 0xff, (rate >> 16) & 0xff };
    memcpy(b, rb, 3); if ((r = ctrl(e, 0x22, 0x01, 0x0100, 0x86, b, 3)) < 0) return r;
    memcpy(b, rb, 3); if ((r = ctrl(e, 0x22, 0x01, 0x0100, 0x02, b, 3)) < 0) return r;
    memcpy(b, rb, 3); if ((r = ctrl(e, 0x22, 0x01, 0x0100, 0x86, b, 3)) < 0) return r;
    memset(b, 0, 3);  if ((r = ctrl(e, 0xa2, 0x81, 0x0100, 0x86, b, 3)) < 0) return r;
    unsigned got = b[0] | (b[1] << 8) | (b[2] << 16);
    if (got != rate) return LIBUSB_ERROR_INVALID_PARAM;
    if ((r = ctrl(e, 0xc0, 0x49, 0, 0, b, 1)) < 0) return r;
    if ((r = ctrl(e, 0x40, 0x49, 0x0032, 0, b, 0)) < 0) return r;
    return 0;
}

static inline void pack24(unsigned char *p, float v) {
    if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
    int s = (int)(v * 8388607.f);
    p[0] = s & 0xff; p[1] = (s >> 8) & 0xff; p[2] = (s >> 16) & 0xff;
}

/* Decide how many frames the next out transfer (OUT_MS) carries and fill it. */
static void fill_out(eie_t *e, struct libusb_transfer *t) {
    unsigned nom1000 = e->rate * OUT_MS;       /* frames per transfer, x1000 */
    e->nominal_acc += nom1000;
    int nominal = e->nominal_acc / 1000;
    e->nominal_acc -= nominal * 1000;

    /* The box consumes audio at a perfectly steady rate, and its buffer is small:
     * chunks that swing in size crackle. So send the nominal amount and steer by
     * at most 1 frame per transfer towards the lead measured while settling. */
    int n = nominal;
    if (e->clock_seen) {
        const int settle_n = 200 / OUT_MS;          /* first 200 ms: learn the lead */
        if (e->settle < settle_n) {
            e->settle_sum += e->credit;
            if (++e->settle == settle_n) e->target = e->settle_sum / settle_n;
        } else {
            double err = (double)(e->credit - e->target);
            e->err_avg += 0.05 * (err - e->err_avg);
            long corr = lround(e->err_avg / 8);
            if (corr > 1) corr = 1; else if (corr < -1) corr = -1;   /* at most 1 frame per transfer */
            n = nominal + (int)corr;
        }
        e->credit -= n;
    }
    if (n > OUT_MAX_FRAMES) n = OUT_MAX_FRAMES;
    TRACE('o', n, nominal, (int)e->credit, e->clock_seen);

    float *f = e->render_buf;
    if (e->render) e->render(e->user, f, n, e->s_total);
    else memset(f, 0, sizeof(float) * n * EIE_CHANNELS);
    e->s_total += n;
    const float up = 50.f / e->rate, down = 100.f / e->rate;
    int fading = atomic_load(&e->fading_out);
    for (int i = 0; i < n; i++) {
        if (fading) { e->gain -= down; if (e->gain < 0.f) e->gain = 0.f; }
        else if (e->mute_left > 0) { e->mute_left--; e->gain = 0.f; }
        else if (e->gain < 1.f) { e->gain += up; if (e->gain > 1.f) e->gain = 1.f; }
        if (e->gain < 1.f)
            for (int c = 0; c < EIE_CHANNELS; c++) f[i * EIE_CHANNELS + c] *= e->gain;
    }
    unsigned char *p = t->buffer;
    for (int i = 0; i < n * EIE_CHANNELS; i++, p += 3) pack24(p, f[i]);

    int have = 0;
    for (int i = 0; i < OUT_PKTS; i++) {
        int upto = n * (i + 1) / OUT_PKTS;
        t->iso_packet_desc[i].length = (upto - have) * 12;
        have = upto;
    }
    t->length = n * 12;
    atomic_fetch_add(&e->st_out_frames, n);
}

static void finished(eie_t *e, struct libusb_transfer *t) {
    (void)t;
    e->pending--;
}

static void resubmit(eie_t *e, struct libusb_transfer *t) {
    if (!atomic_load(&e->running) || e->restarting || libusb_submit_transfer(t) != 0) finished(e, t);
}

static void out_cb(struct libusb_transfer *t) {
    eie_t *e = t->user_data;
    if (t->status != LIBUSB_TRANSFER_COMPLETED && t->status != LIBUSB_TRANSFER_CANCELLED)
        atomic_fetch_add(&e->st_errors, 1);
    { uint64_t now = mach_absolute_time();
      if (e->last_out) { mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        double g = (double)(now - e->last_out) * tb.numer / tb.denom / 1e6;
        if (g > e->max_out_gap_ms) e->max_out_gap_ms = g;
        if (g > 10) e->late_out++; }
      e->last_out = now;
      if (t->status == LIBUSB_TRANSFER_COMPLETED)
        for (int i = 0; i < t->num_iso_packets; i++)
          if (t->iso_packet_desc[i].status != LIBUSB_TRANSFER_COMPLETED) e->out_bad_pkts++; }
    if (atomic_load(&e->running) && !e->restarting) fill_out(e, t);
    resubmit(e, t);
}

/* Delay-locked loop over clock packets (one per ms): filtered host time of frame
 * c_total and host ticks per frame, about 0.5 Hz bandwidth. Published seqlock-style. */
static void clock_dll(eie_t *e, int n) {
    double now = (double)mach_absolute_time();
    if (!e->dll_on) {
        e->dll_t = now; e->dll_tpf = e->nominal_tpf; e->dll_on = 1;
    } else {
        const double w = 2 * 3.14159265358979 * 0.5 * 0.001, b = 1.41421356 * w, c = w * w;
        double pred = e->dll_t + n * e->dll_tpf;
        double err = now - pred;
        double lim = 48 * e->nominal_tpf * 4;      /* ignore a callback that came very late */
        if (err > lim) err = lim; else if (err < -lim) err = -lim;
        e->dll_t = pred + b * err;
        e->dll_tpf += c * err / (e->rate / 1000.0);
    }
    atomic_fetch_add_explicit(&e->ts_seq, 1, memory_order_acq_rel);
    e->ts_frames = e->c_total; e->ts_host = e->dll_t; e->ts_tpf = e->dll_tpf;
    atomic_fetch_add_explicit(&e->ts_seq, 1, memory_order_acq_rel);
}

int eie_get_clock(const eie_t *e, eie_clock_t *out) {
    eie_t *m = (eie_t *)e;
    for (int tries = 0; tries < 100; tries++) {
        unsigned a = atomic_load_explicit(&m->ts_seq, memory_order_acquire);
        if (a & 1) continue;
        long long f = m->ts_frames; double h = m->ts_host, t = m->ts_tpf; unsigned s = m->ts_seed;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&m->ts_seq, memory_order_relaxed) == a) {
            out->valid = h != 0; out->frames = f; out->host = h; out->ticks_per_frame = t; out->seed = s;
            return 1;
        }
    }
    out->valid = 0;
    return 0;
}

static void clock_cb(struct libusb_transfer *t) {
    eie_t *e = t->user_data;
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i < t->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *d = &t->iso_packet_desc[i];
            { unsigned char *q = t->buffer + i * CLOCK_PKT_SIZE; (void)q; TRACE('c', d->status, d->actual_length, d->actual_length ? q[0] : -1, d->actual_length > 2 ? q[1] | q[2] << 8 : -1); }
            if (d->status == LIBUSB_TRANSFER_COMPLETED && d->actual_length >= 1) {
                unsigned char n = t->buffer[i * CLOCK_PKT_SIZE];
                e->credit += n;
                e->clock_seen = 1;
                e->c_total += n;
                { uint64_t now = mach_absolute_time();
                  if (e->last_clk) { mach_timebase_info_data_t tb; mach_timebase_info(&tb);
                    double g = (double)(now - e->last_clk) * tb.numer / tb.denom / 1e6;
                    if (g > e->max_gap_ms) e->max_gap_ms = g; }
                  e->last_clk = now; }
                clock_dll(e, n);
                atomic_fetch_add(&e->st_clock_frames, n);
                atomic_fetch_add(&e->st_clock_packets, 1);
            }
        }
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) atomic_fetch_add(&e->st_errors, 1);
    resubmit(e, t);
}

/* Each input frame is 64 bytes: bit 0 of bytes 0-23 is input 1 and bit 1 is
 * input 3; bytes 32-55 carry inputs 2 and 4 the same way. MSB first. */
static void in_cb(struct libusb_transfer *t) {
    eie_t *e = t->user_data;
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        int frames = t->actual_length / IN_FRAME_BYTES;
        const unsigned char *a = t->buffer;
        if (e->skip_in > 0) {   /* the first frames after a start can hold a full-scale click */
            int drop = e->skip_in < frames ? (int)e->skip_in : frames;
            e->skip_in -= drop; frames -= drop; a += drop * IN_FRAME_BYTES;
        }
        float *f = e->capture_buf;
        for (int k = 0; k < frames; k++, a += IN_FRAME_BYTES) {
            uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
            for (int i = 0; i < 24; i++) {
                c0 = (c0 << 1) | (a[i] & 1);
                c2 = (c2 << 1) | ((a[i] >> 1) & 1);
                c1 = (c1 << 1) | (a[32 + i] & 1);
                c3 = (c3 << 1) | ((a[32 + i] >> 1) & 1);
            }
            const float s = 1.f / 2147483648.f;
            f[k * 4 + 0] = (int32_t)(c0 << 8) * s;
            f[k * 4 + 1] = (int32_t)(c1 << 8) * s;
            f[k * 4 + 2] = (int32_t)(c2 << 8) * s;
            f[k * 4 + 3] = (int32_t)(c3 << 8) * s;
        }
        if (frames && !e->in_pos_set) { e->in_pos = e->c_total - frames; e->in_pos_set = 1; }
        if (frames && e->capture) e->capture(e->user, f, frames, e->in_pos);
        e->in_pos += frames;
        TRACE('i', t->actual_length, frames, 0, 0);
        atomic_fetch_add(&e->st_in_frames, frames);
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED && t->status != LIBUSB_TRANSFER_TIMED_OUT)
        atomic_fetch_add(&e->st_errors, 1);
    resubmit(e, t);
}

/* Ask the scheduler for real-time treatment: about 0.5 ms of work every 2 ms. */
static int make_realtime(void) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double ms = 1e6 * tb.denom / tb.numer;   /* abs time units per ms */
    thread_time_constraint_policy_data_t p = {
        .period = (uint32_t)(2 * ms), .computation = (uint32_t)(0.5 * ms),
        .constraint = (uint32_t)(2 * ms), .preemptible = 1 };
    return thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                             (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

static void midi_in_cb(struct libusb_transfer *t) {
    eie_t *e = t->user_data;
    if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0 && e->midi_in) {
        unsigned char b[512]; int n = 0;
        for (int i = 0; i < t->actual_length && n < (int)sizeof b; i++)
            if (t->buffer[i] != 0xfd) b[n++] = t->buffer[i];
        if (n) e->midi_in(e->midi_ctx, b, n);
    }
    resubmit(e, t);
}

static void midi_out_cb(struct libusb_transfer *t) {
    eie_t *e = t->user_data;
    for (int i = 0; i < MIDI_OUT_XFERS; i++) if (e->mout[i] == t) e->mout_busy[i] = 0;
    finished(e, t);
}

/* USB thread: send waiting MIDI bytes, up to 3 per 9-byte packet. */
static void midi_pump(eie_t *e) {
    if (!atomic_load(&e->running) || e->restarting) return;
    for (int i = 0; i < MIDI_OUT_XFERS; i++) {
        unsigned r = atomic_load_explicit(&e->midi_r, memory_order_relaxed);
        unsigned w = atomic_load_explicit(&e->midi_w, memory_order_acquire);
        if (r == w) return;
        if (e->mout_busy[i]) continue;
        unsigned char *b = e->moutbuf[i];
        int n = 0;
        while (n < 3 && r != w) { b[n++] = e->midi_ring[r & (MIDI_RING - 1)]; r++; }
        for (int k = n; k < 8; k++) b[k] = 0xfd;
        b[8] = 0xe0;
        atomic_store_explicit(&e->midi_r, r, memory_order_release);
        if (libusb_submit_transfer(e->mout[i]) == 0) { e->mout_busy[i] = 1; e->pending++; }
    }
}

int eie_midi_send(eie_t *e, const unsigned char *bytes, int n) {
    unsigned w = atomic_load_explicit(&e->midi_w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&e->midi_r, memory_order_acquire);
    int sent = 0;
    while (sent < n && (w - r) < MIDI_RING) { e->midi_ring[w & (MIDI_RING - 1)] = bytes[sent++]; w++; }
    atomic_store_explicit(&e->midi_w, w, memory_order_release);
    return sent;
}

void eie_set_midi(eie_t *e, eie_midi_fn in, void *ctx) { e->midi_ctx = ctx; e->midi_in = in; }

/* On macOS, libusb delivers every transfer completion through its own thread
 * ("org.libusb.device-hotplug", started by libusb_init). Inside the audio server that
 * thread ran at normal priority and was sometimes 20+ ms late, long enough for the
 * box to run dry and halt. Give it real-time treatment too. Returns how many threads
 * were raised (0 if not found), or a negative kern_return_t. */
static int boost_libusb_thread(void) {
    thread_act_array_t th; mach_msg_type_number_t n;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return -1;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double ms = 1e6 * tb.denom / tb.numer;
    int found = 0;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        pthread_t p = pthread_from_mach_thread_np(th[i]);
        char name[64] = "";
        if (p) pthread_getname_np(p, name, sizeof name);
        if (strstr(name, "org.libusb")) {
            thread_time_constraint_policy_data_t pol = {
                .period = (uint32_t)(2 * ms), .computation = (uint32_t)(0.3 * ms),
                .constraint = (uint32_t)(2 * ms), .preemptible = 1 };
            kern_return_t r = thread_policy_set(th[i], THREAD_TIME_CONSTRAINT_POLICY,
                                                (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
            found = r == KERN_SUCCESS ? found + 1 : -(int)r;
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_act_t));
    return found;
}

static int submit_all(eie_t *e);
static void cancel_all(eie_t *e) {
    for (int i = 0; i < OUT_XFERS; i++) libusb_cancel_transfer(e->out[i]);
    for (int i = 0; i < CLOCK_XFERS; i++) libusb_cancel_transfer(e->clk[i]);
    for (int i = 0; i < IN_XFERS; i++) libusb_cancel_transfer(e->in[i]);
    for (int i = 0; i < MIDI_IN_XFERS; i++) libusb_cancel_transfer(e->min[i]);
    for (int i = 0; i < MIDI_OUT_XFERS; i++) if (e->mout_busy[i]) libusb_cancel_transfer(e->mout[i]);
}

/* The box sometimes stops streaming (another program poking the USB bus can do
 * it). If its clock goes quiet for 100 ms, drain everything, run the start-up
 * sequence again and carry on. */
static void watchdog(eie_t *e) {
    static double ms_per_abs = 0;
    if (ms_per_abs == 0) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); ms_per_abs = (double)tb.numer / tb.denom / 1e6; }
    uint64_t now = mach_absolute_time();
    long pk = atomic_load(&e->st_clock_packets);
    if (pk != e->wd_packets || e->wd_since == 0) {
        e->wd_packets = pk; e->wd_since = now;
        e->wd_in = atomic_load(&e->st_in_frames); e->wd_out = atomic_load(&e->st_out_frames);
        return;
    }
    if (pk == 0) return;   /* armed once the clock has started; start-up can take a while */
    if ((now - e->wd_since) * ms_per_abs < 100) return;

    if (e->log) {
        char m[200];
        snprintf(m, sizeof m, "watchdog: no clock for %.0f ms (in +%ld, out +%ld frames meanwhile, %d transfers pending, longest clock gap before %.1f ms, longest out gap %.1f ms, out refills later than 10 ms %ld, bad out packets %ld); restarting",
                 (now - e->wd_since) * ms_per_abs, atomic_load(&e->st_in_frames) - e->wd_in,
                 atomic_load(&e->st_out_frames) - e->wd_out, e->pending, e->max_gap_ms, e->max_out_gap_ms, e->late_out, e->out_bad_pkts);
        e->log(m);
    }
    e->restarting = 1;
    cancel_all(e);
    struct timeval tv = {0, 20000};
    for (int i = 0; e->pending > 0 && i < 100; i++) libusb_handle_events_timeout_completed(e->ctx, &tv, NULL);
    e->restarting = 0;
    if (e->pending > 0) { e->wd_since = now; return; }   /* stuck transfers: try again later */
    atomic_fetch_add(&e->st_restarts, 1);
    if (init_box(e, e->rate) == 0) submit_all(e);
    e->wd_since = mach_absolute_time();
}

static void *event_thread(void *arg) {
    eie_t *e = arg;
    pthread_setname_np("EIE Pro USB");
    int rt = make_realtime();
    int lb = boost_libusb_thread();
    if (e->log) { char m[120]; snprintf(m, sizeof m, "real-time: USB thread %s (%d), libusb completion thread %s (%d)",
                                         rt == 0 ? "granted" : "refused", rt, lb > 0 ? "granted" : "not raised", lb); e->log(m); }
    struct timeval tv = {0, 20000};
    while (atomic_load(&e->running) || e->pending > 0) {
        if (!atomic_load(&e->running)) cancel_all(e);   /* stopping */
        else { watchdog(e); midi_pump(e); }
        libusb_handle_events_timeout_completed(e->ctx, &tv, NULL);
    }
    atomic_store(&e->thread_on, 0);
    return NULL;
}

static void free_transfers(eie_t *e) {
    for (int i = 0; i < OUT_XFERS; i++) { libusb_free_transfer(e->out[i]); free(e->outbuf[i]); e->out[i] = 0; e->outbuf[i] = 0; }
    for (int i = 0; i < CLOCK_XFERS; i++) { libusb_free_transfer(e->clk[i]); free(e->clkbuf[i]); e->clk[i] = 0; e->clkbuf[i] = 0; }
    for (int i = 0; i < IN_XFERS; i++) { libusb_free_transfer(e->in[i]); free(e->inbuf[i]); e->in[i] = 0; e->inbuf[i] = 0; }
    for (int i = 0; i < MIDI_IN_XFERS; i++) { libusb_free_transfer(e->min[i]); free(e->minbuf[i]); e->min[i] = 0; e->minbuf[i] = 0; }
    for (int i = 0; i < MIDI_OUT_XFERS; i++) { libusb_free_transfer(e->mout[i]); free(e->moutbuf[i]); e->mout[i] = 0; e->moutbuf[i] = 0; e->mout_busy[i] = 0; }
}

/* Reset pacing and submit every transfer. Returns 0 or the first libusb error. */
static int submit_all(eie_t *e) {
    e->credit = 0; e->clock_seen = 0; e->nominal_acc = 0;
    e->settle = 0; e->settle_sum = 0; e->target = 0; e->err_avg = 0;
    e->skip_in = e->rate / 100;   /* 10 ms */
    e->mute_left = e->rate / 4; e->gain = 0.f;
    e->s_total = e->c_total;     /* the box's buffer starts empty */
    e->last_clk = 0; e->last_out = 0;
    e->in_pos_set = 0;
    e->dll_on = 0;
    atomic_fetch_add(&e->ts_seq, 1); e->ts_host = 0; e->ts_seed++; atomic_fetch_add(&e->ts_seq, 1);
    for (int i = 0; i < OUT_XFERS; i++) fill_out(e, e->out[i]);
    struct libusb_transfer *all[CLOCK_XFERS + OUT_XFERS + IN_XFERS + MIDI_IN_XFERS]; int n = 0;
    for (int i = 0; i < CLOCK_XFERS; i++) all[n++] = e->clk[i];
    for (int i = 0; i < OUT_XFERS; i++) all[n++] = e->out[i];
    for (int i = 0; i < IN_XFERS; i++) all[n++] = e->in[i];
    for (int i = 0; i < MIDI_IN_XFERS; i++) all[n++] = e->min[i];
    for (int i = 0; i < MIDI_OUT_XFERS; i++) e->mout_busy[i] = 0;
    for (int i = 0; i < n; i++) {
        int r = libusb_submit_transfer(all[i]);
        if (r) return r;
        e->pending++;
    }
    return 0;
}

int eie_start(eie_t *e, unsigned rate, eie_render_fn render, eie_capture_fn capture, void *ctx) {
    if (atomic_load(&e->thread_on)) eie_stop(e);
    if (rate != 44100 && rate != 48000 && rate != 88200 && rate != 96000) return LIBUSB_ERROR_INVALID_PARAM;
    int r = init_box(e, rate);
    if (r) return r;

    e->rate = rate; e->render = render; e->capture = capture; e->user = ctx;
    e->pending = 0; e->wd_packets = 0; e->wd_since = 0;
    e->c_total = 0; e->last_clk = 0; e->max_gap_ms = 0; e->last_out = 0; e->max_out_gap_ms = 0; e->out_bad_pkts = 0; e->late_out = 0;
    { mach_timebase_info_data_t tb; mach_timebase_info(&tb); e->nominal_tpf = 1e9 * tb.denom / tb.numer / rate; }
    atomic_store(&e->st_clock_frames, 0); atomic_store(&e->st_clock_packets, 0);
    atomic_store(&e->st_out_frames, 0); atomic_store(&e->st_in_frames, 0); atomic_store(&e->st_errors, 0); atomic_store(&e->st_restarts, 0);
    atomic_store(&e->running, 1);

    for (int i = 0; i < CLOCK_XFERS; i++) {
        e->clk[i] = libusb_alloc_transfer(CLOCK_PKTS);
        e->clkbuf[i] = calloc(CLOCK_PKTS, CLOCK_PKT_SIZE);
        libusb_fill_iso_transfer(e->clk[i], e->h, EP_CLOCK_IN, e->clkbuf[i], CLOCK_PKTS * CLOCK_PKT_SIZE,
                                 CLOCK_PKTS, clock_cb, e, 0);
        libusb_set_iso_packet_lengths(e->clk[i], CLOCK_PKT_SIZE);
    }
    for (int i = 0; i < OUT_XFERS; i++) {
        e->out[i] = libusb_alloc_transfer(OUT_PKTS);
        e->outbuf[i] = calloc(OUT_MAX_FRAMES, 12);
        libusb_fill_iso_transfer(e->out[i], e->h, EP_AUDIO_OUT, e->outbuf[i], 0, OUT_PKTS, out_cb, e, 0);
    }
    for (int i = 0; i < IN_XFERS; i++) {
        e->in[i] = libusb_alloc_transfer(0);
        e->inbuf[i] = malloc(IN_XFER_BYTES);
        libusb_fill_bulk_transfer(e->in[i], e->h, EP_AUDIO_IN, e->inbuf[i], IN_XFER_BYTES, in_cb, e, 0);
    }
    for (int i = 0; i < MIDI_IN_XFERS; i++) {
        e->min[i] = libusb_alloc_transfer(0);
        e->minbuf[i] = malloc(512);
        libusb_fill_bulk_transfer(e->min[i], e->h, EP_MIDI_IN, e->minbuf[i], 512, midi_in_cb, e, 0);
    }
    for (int i = 0; i < MIDI_OUT_XFERS; i++) {
        e->mout[i] = libusb_alloc_transfer(0);
        e->moutbuf[i] = malloc(9);
        libusb_fill_bulk_transfer(e->mout[i], e->h, EP_MIDI_OUT, e->moutbuf[i], 9, midi_out_cb, e, 1000);
    }
    r = submit_all(e);
    atomic_store(&e->thread_on, 1);
    pthread_create(&e->thread, NULL, event_thread, e);
    if (r) { eie_stop(e); return r; }
    return 0;
}

void eie_stop(eie_t *e) {
    if (!atomic_load(&e->thread_on)) return;
    atomic_store(&e->fading_out, 1);   /* fade out, then a little silence, then stop */
    usleep(60000);
    atomic_store(&e->running, 0);
    pthread_join(e->thread, NULL);
    atomic_store(&e->fading_out, 0);
    free_transfers(e);
    libusb_set_interface_alt_setting(e->h, 1, 0);
    libusb_set_interface_alt_setting(e->h, 0, 0);
}

void eie_close(eie_t *e) {
    if (!e) return;
    eie_stop(e);
    libusb_release_interface(e->h, 1);
    libusb_release_interface(e->h, 0);
    libusb_close(e->h);
    libusb_exit(e->ctx);
    free(e);
}

void eie_set_log(eie_t *e, void (*fn)(const char *msg)) { e->log = fn; }
double eie_max_clock_gap_ms(const eie_t *e) { return e->max_gap_ms; }

void eie_get_stats(const eie_t *e, eie_stats_t *s) {
    s->clock_frames = atomic_load(&e->st_clock_frames);
    s->clock_packets = atomic_load(&e->st_clock_packets);
    s->out_frames = atomic_load(&e->st_out_frames);
    s->in_frames = atomic_load(&e->st_in_frames);
    s->errors = atomic_load(&e->st_errors);
    s->restarts = atomic_load(&e->st_restarts);
}
