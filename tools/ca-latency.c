/* ca-latency: measure the EIE Pro's round-trip latency through a loopback cable.
 *
 *   ca-latency [rate] [buffer_frames] [out_ch=1] [in_ch=1] [level_dB=-18]
 *
 * Patch output <out_ch> into input <in_ch> with a cable first. The tool runs one
 * IOProc on the EIE Pro device, plays 8 short windowed noise bursts, records the input,
 * and cross-correlates each burst against the recording on the device's own sample
 * timeline.
 *
 * "Hardware path" is how many samples later the burst comes back than Core Audio's
 * timestamps say (they never include reported latency). "Unreported" is that minus the
 * latencies the driver reports. If the driver
 * reported its latency correctly this would be ~0 (only the analog path). Apps like
 * Ableton add the reported figures themselves, so the unreported part is what makes
 * recordings land late.
 */
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *UID = "co.ke.kimina.eie-pro.device";

enum { kBursts = 8, kBurstLen = 1024 };
static double gRate;
static float burst[kBurstLen];
static float *outRec, *inRec;          /* indexed by device sample time - base */
static long recLen;
static volatile double base = -1;
static volatile long lastIn;
static long burstAt[kBursts];
static int outCh, inCh;                /* zero-based */

static UInt32 getU32(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope sc)
{
    AudioObjectPropertyAddress a = {s, sc, kAudioObjectPropertyElementMain};
    UInt32 v = 0, sz = sizeof v;
    AudioObjectGetPropertyData(o, &a, 0, NULL, &sz, &v);
    return v;
}

static UInt32 streamLatency(AudioObjectID dev, AudioObjectPropertyScope sc)
{
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreams, sc, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &sz) || sz < sizeof(AudioObjectID)) return 0;
    AudioObjectID s[8]; if (sz > sizeof s) sz = sizeof s;
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, s);
    return getU32(s[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
}

/* Find (buffer, channel-within-buffer) for a device-wide channel index. */
static int locate(const AudioBufferList *l, int ch, UInt32 *buf, UInt32 *sub)
{
    for (UInt32 b = 0; b < l->mNumberBuffers; b++) {
        UInt32 n = l->mBuffers[b].mNumberChannels;
        if (ch < (int)n) { *buf = b; *sub = ch; return 1; }
        ch -= n;
    }
    return 0;
}

static OSStatus ioProc(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in,
                       const AudioTimeStamp *inTime, AudioBufferList *out,
                       const AudioTimeStamp *outTime, void *u)
{
    if (base < 0) base = outTime->mSampleTime;
    UInt32 b, c;
    if (out && locate(out, outCh, &b, &c)) {
        AudioBuffer *ab = &out->mBuffers[b];
        UInt32 nch = ab->mNumberChannels, n = ab->mDataByteSize / (4 * nch);
        float *f = ab->mData;
        memset(f, 0, ab->mDataByteSize);
        long t0 = (long)(outTime->mSampleTime - base);
        for (UInt32 i = 0; i < n; i++) {
            long t = t0 + i;
            float v = 0;
            for (int k = 0; k < kBursts; k++)
                if (t >= burstAt[k] && t < burstAt[k] + kBurstLen) v = burst[t - burstAt[k]];
            f[i * nch + c] = v;
            if (t >= 0 && t < recLen) outRec[t] = v;
        }
        for (UInt32 x = 0; x < out->mNumberBuffers; x++)   /* silence the other buffers */
            if (x != b) memset(out->mBuffers[x].mData, 0, out->mBuffers[x].mDataByteSize);
    }
    if (in && locate(in, inCh, &b, &c)) {
        const AudioBuffer *ab = &in->mBuffers[b];
        UInt32 nch = ab->mNumberChannels, n = ab->mDataByteSize / (4 * nch);
        const float *f = ab->mData;
        long t0 = (long)(inTime->mSampleTime - base);
        for (UInt32 i = 0; i < n; i++) {
            long t = t0 + i;
            if (t >= 0 && t < recLen) inRec[t] = f[i * nch + c];
        }
        lastIn = t0 + n;
    }
    return 0;
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    double rate = argc > 1 ? atof(argv[1]) : 0;
    UInt32 bufFrames = argc > 2 ? (UInt32)atoi(argv[2]) : 0;
    outCh = (argc > 3 ? atoi(argv[3]) : 1) - 1;
    inCh = (argc > 4 ? atoi(argv[4]) : 1) - 1;
    double level = argc > 5 ? atof(argv[5]) : -18;
    if (level > -6) { fprintf(stderr, "refusing louder than -6 dBFS\n"); return 2; }
    if (outCh < 0 || outCh > 3 || inCh < 0 || inCh > 3) { fprintf(stderr, "channels are 1-4\n"); return 2; }

    AudioObjectID dev = 0;
    {
        CFStringRef u = CFStringCreateWithCString(NULL, UID, kCFStringEncodingUTF8);
        AudioValueTranslation t = {&u, sizeof u, &dev, sizeof dev};
        AudioObjectPropertyAddress a = {kAudioHardwarePropertyDeviceForUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 sz = sizeof t;
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &t);
        if (!dev) { printf("EIE Pro device not found\n"); return 1; }
    }
    AudioObjectPropertyAddress ra = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    if (rate > 0) {
        if (AudioObjectSetPropertyData(dev, &ra, 0, NULL, sizeof rate, &rate)) { printf("setting %.0f Hz failed\n", rate); return 1; }
        usleep(500000);
    }
    { UInt32 sz = sizeof rate; AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &rate); }
    gRate = rate;
    if (bufFrames) {
        AudioObjectPropertyAddress a = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof bufFrames, &bufFrames);
    }

    /* the burst: seeded white noise under a Hann window */
    double amp = pow(10, level / 20);
    srand(12345);
    for (int i = 0; i < kBurstLen; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / (kBurstLen - 1));
        burst[i] = (float)(amp * w * (2.0 * rand() / RAND_MAX - 1));
    }
    long gap = (long)(rate * 0.4);
    for (int k = 0; k < kBursts; k++) burstAt[k] = (long)(rate * 0.6) + k * gap;
    recLen = burstAt[kBursts - 1] + (long)(rate * 1.0);
    outRec = calloc(recLen, sizeof(float));
    inRec = calloc(recLen, sizeof(float));

    AudioDeviceIOProcID pid;
    if (AudioDeviceCreateIOProcID(dev, ioProc, NULL, &pid)) { printf("IOProc failed\n"); return 1; }
    if (AudioDeviceStart(dev, pid)) { printf("start failed\n"); return 1; }
    while (lastIn < recLen) usleep(20000);
    AudioDeviceStop(dev, pid);
    AudioDeviceDestroyIOProcID(dev, pid);

    /* what the driver reports */
    UInt32 buf = getU32(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    UInt32 oLat = getU32(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput);
    UInt32 iLat = getU32(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput);
    UInt32 oSt = streamLatency(dev, kAudioObjectPropertyScopeOutput);
    UInt32 iSt = streamLatency(dev, kAudioObjectPropertyScopeInput);
    UInt32 oSafe = getU32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);
    UInt32 iSafe = getU32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput);

    /* cross-correlate each burst */
    double energy = 0, inPeak = 0;
    for (int i = 0; i < kBurstLen; i++) energy += burst[i] * burst[i];
    for (long t = 0; t < recLen; t++) if (fabs(inRec[t]) > inPeak) inPeak = fabs(inRec[t]);
    long minLag = -(long)(rate * 0.1), maxLag = (long)(rate * 0.25);
    double lags[kBursts]; int good = 0;
    printf("\nEIE Pro %.0f Hz, buffer %u, out %d -> in %d, input peak %.1f dBFS\n",
           rate, buf, outCh + 1, inCh + 1, inPeak > 0 ? 20 * log10(inPeak) : -999.0);
    for (int k = 0; k < kBursts; k++) {
        long T = burstAt[k];
        double best = 0, bestAbs = 0; long bestL = 0;
        double cc[3] = {0};
        for (long L = minLag; L <= maxLag; L++) {
            if (T + L < 0 || T + L + kBurstLen > recLen) continue;
            double s = 0;
            for (int i = 0; i < kBurstLen; i++) s += burst[i] * inRec[T + L + i];
            if (fabs(s) > bestAbs) { bestAbs = fabs(s); best = s; bestL = L; }
        }
        for (int j = -1; j <= 1; j++) {
            double s = 0;
            for (int i = 0; i < kBurstLen; i++) s += burst[i] * inRec[T + bestL + j + i];
            cc[j + 1] = best < 0 ? -s : s;
        }
        double den = cc[0] - 2 * cc[1] + cc[2];
        double frac = den != 0 ? 0.5 * (cc[0] - cc[2]) / den : 0;
        double lag = bestL + frac, gain = best / energy;
        int ok = fabs(gain) > 0.01;
        printf("  burst %d: %+9.2f samples  (gain %+.3f%s)\n", k + 1, lag, gain,
               ok ? (gain < 0 ? ", polarity flipped" : "") : ", TOO WEAK, ignored");
        if (ok) lags[good++] = lag;
    }
    if (!good) {
        printf("\nNo burst found in the input. Check the cable (output %d -> input %d) and the input gain.\n",
               outCh + 1, inCh + 1);
        return 1;
    }
    qsort(lags, good, sizeof lags[0], cmp);
    UInt32 reported = oLat + oSt + iLat + iSt;
    /* The IOProc's timestamps never include reported latency, so the measured lag is
     * the whole hardware path; what's left after subtracting the reported part is the
     * error an app like Ableton would see. */
    double hw = lags[good / 2], spread = lags[good - 1] - lags[0];
    double med = hw - reported;
    double ms = 1000.0 / rate;
    printf("\nReported latency  out %u+%u, in %u+%u (device+stream) = %u samples (%.2f ms)\n",
           oLat, oSt, iLat, iSt, reported, reported * ms);
    printf("Safety offsets    out %u, in %u\n", oSafe, iSafe);
    printf("Hardware path     %.2f samples (%.2f ms), spread %.2f samples over %d bursts\n",
           hw, hw * ms, spread, good);
    printf("Unreported        %+.2f samples (%.2f ms)\n", med, med * ms);
    double total = hw + oSafe + iSafe + 2.0 * buf;
    printf("Round trip in app %.1f samples (%.2f ms)  = hardware + safety offsets + 2 x buffer\n",
           total, total * ms);
    if (fabs(med) > 6)
        printf("\n-> Recordings land %.1f samples %s in Ableton unless the driver reports that.\n",
               fabs(med), med > 0 ? "LATE" : "EARLY");
    else
        printf("\n-> Reported latency is correct (within 6 samples).\n");
    return 0;
}
