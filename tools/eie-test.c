/* eie-test: check an Akai EIE Pro from the terminal.
 *
 *   eie-test info                      firmware, and a 2 s streaming check (silent)
 *   eie-test tone [options]            a quiet sine on outputs 1-2, input levels shown
 *   eie-test record FILE.wav [options] record the 4 inputs (24-bit WAV)
 *
 * Options: -r RATE (48000)  -s SECONDS  -l LEVEL_DB (tone, -30)  -f HZ (440)  -o OUTS (12)
 */
#include "../src/eie_usb.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>

static volatile sig_atomic_t stop_now = 0;
static void on_sigint(int s) { (void)s; stop_now = 1; }

typedef struct {
    double phase, step, amp;
    int outs[EIE_CHANNELS];
    long fade_len, done;              /* 50 ms fade in */
    double peak[EIE_CHANNELS], sumsq[EIE_CHANNELS];
    long meter_frames;
    FILE *wav; long wav_frames;
} state_t;

static void render(void *ctx, float *out, int frames, long long pos) {
    state_t *s = ctx;
    for (int i = 0; i < frames; i++) {
        double g = s->done < s->fade_len ? (double)s->done / s->fade_len : 1.0;
        float v = (float)(sin(s->phase) * s->amp * g);
        s->phase += s->step; if (s->phase > 2 * M_PI) s->phase -= 2 * M_PI;
        for (int c = 0; c < EIE_CHANNELS; c++) out[i * EIE_CHANNELS + c] = s->outs[c] ? v : 0.f;
        s->done++;
    }
}

static void capture(void *ctx, const float *in, int frames, long long pos) {
    state_t *s = ctx;
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < EIE_CHANNELS; c++) {
            double v = in[i * EIE_CHANNELS + c];
            if (fabs(v) > s->peak[c]) s->peak[c] = fabs(v);
            s->sumsq[c] += v * v;
        }
    s->meter_frames += frames;
    if (s->wav) {
        unsigned char b[3 * EIE_CHANNELS];
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < EIE_CHANNELS; c++) {
                int v = (int)lrint(in[i * EIE_CHANNELS + c] * 8388607.0);
                if (v > 8388607) v = 8388607; else if (v < -8388608) v = -8388608;
                b[c * 3] = v & 0xff; b[c * 3 + 1] = (v >> 8) & 0xff; b[c * 3 + 2] = (v >> 16) & 0xff;
            }
            fwrite(b, 1, sizeof b, s->wav);
        }
        s->wav_frames += frames;
    }
}

static void le32(FILE *f, unsigned v) { unsigned char b[4] = {v, v >> 8, v >> 16, v >> 24}; fwrite(b, 1, 4, f); }
static void le16(FILE *f, unsigned v) { unsigned char b[2] = {v, v >> 8}; fwrite(b, 1, 2, f); }
static void wav_header(FILE *f, unsigned rate, long frames) {
    unsigned data = (unsigned)frames * 3 * EIE_CHANNELS;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); le32(f, 36 + data); fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16); le16(f, 1); le16(f, EIE_CHANNELS); le32(f, rate);
    le32(f, rate * 3 * EIE_CHANNELS); le16(f, 3 * EIE_CHANNELS); le16(f, 24);
    fwrite("data", 1, 4, f); le32(f, data);
}

static double db(double x) { return x > 1e-12 ? 20 * log10(x) : -240; }

static void meters(state_t *s, const char *label) {
    printf("%s", label);
    for (int c = 0; c < EIE_CHANNELS; c++) {
        double rms = s->meter_frames ? sqrt(s->sumsq[c] / s->meter_frames) : 0;
        printf("  in%d %6.1f/%6.1f", c + 1, db(s->peak[c]), db(rms));
        s->peak[c] = 0; s->sumsq[c] = 0;
    }
    s->meter_frames = 0;
    printf("   (peak/rms dBFS)\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: eie-test info | tone | record FILE.wav  [-r rate] [-s secs] [-l dB] [-f Hz] [-o outs]\n");
        return 2;
    }
    const char *cmd = argv[1], *file = NULL;
    int argi = 2;
    if (!strcmp(cmd, "record")) {
        if (argc < 3) { fprintf(stderr, "record needs a file name\n"); return 2; }
        file = argv[2]; argi = 3;
    }
    unsigned rate = 48000; double secs = !strcmp(cmd, "info") ? 2 : 5, level = -30, hz = 440;
    const char *outs = "12";
    optind = argi;
    int o;
    while ((o = getopt(argc, argv, "r:s:l:f:o:")) != -1) {
        switch (o) {
        case 'r': rate = (unsigned)atoi(optarg); break;
        case 's': secs = atof(optarg); break;
        case 'l': level = atof(optarg); break;
        case 'f': hz = atof(optarg); break;
        case 'o': outs = optarg; break;
        default: return 2;
        }
    }
    if (level > -6) { fprintf(stderr, "refusing a tone louder than -6 dBFS\n"); return 2; }

    char err[200];
    eie_t *e = eie_open(err, sizeof err);
    if (!e) { fprintf(stderr, "%s\n", err); return 1; }
    printf("EIE Pro found, firmware %s\n", eie_firmware(e));

    state_t s = {0};
    s.step = 2 * M_PI * hz / rate;
    s.amp = pow(10, level / 20);
    s.fade_len = rate / 20;
    for (const char *p = outs; *p; p++) if (*p >= '1' && *p <= '4') s.outs[*p - '1'] = 1;
    if (file) {
        s.wav = fopen(file, "wb");
        if (!s.wav) { perror(file); eie_close(e); return 1; }
        wav_header(s.wav, rate, 0);
    }
    int tone = !strcmp(cmd, "tone");
    if (tone) printf("Playing %.0f Hz at %.0f dBFS on outputs %s for %.0f s (Ctrl-C stops)\n", hz, level, outs, secs);

    signal(SIGINT, on_sigint);
    int r = eie_start(e, rate, tone ? render : NULL, capture, &s);
    if (r) { fprintf(stderr, "start failed: %s\n", eie_strerror(r)); eie_close(e); return 1; }

    long ticks = (long)(secs * 4);
    for (long t = 0; t < ticks && !stop_now; t++) {
        usleep(250000);
        if (t % 4 == 3) { char lab[32]; snprintf(lab, sizeof lab, "%3lds", (t + 1) / 4); meters(&s, lab); }
    }
    eie_stats_t st; eie_get_stats(e, &st);
    eie_clock_t ck;
    if (eie_get_clock(e, &ck) && ck.valid) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        double ns_per_frame = ck.ticks_per_frame * tb.numer / tb.denom;
        printf("box clock (filtered): %.2f Hz, %+.0f ppm vs nominal\n", 1e9 / ns_per_frame, (1e9 / ns_per_frame / rate - 1) * 1e6);
    }
    eie_stop(e);

    double played = (double)st.clock_frames / rate;
    printf("\nclock: %ld packets, %ld frames (%.0f Hz over %.1f s)\n", st.clock_packets, st.clock_frames,
           secs > 0 ? st.clock_frames / secs : 0, played);
    printf("sent %ld frames, received %ld frames, transfer errors %ld, watchdog restarts %ld\n", st.out_frames, st.in_frames, st.errors, st.restarts);
    long drift = st.out_frames - st.clock_frames;
    printf("out vs clock: %+ld frames (%s)\n", drift, labs(drift) < rate / 25 ? "in step" : "DRIFTING");
    if (s.wav) { wav_header(s.wav, rate, s.wav_frames); fclose(s.wav); printf("wrote %s (%ld frames)\n", file, s.wav_frames); }
#ifdef EIE_TRACE
    { void eie_trace_dump(const char *); eie_trace_dump("build/trace.txt"); }
#endif
    eie_close(e);
    return 0;
}
