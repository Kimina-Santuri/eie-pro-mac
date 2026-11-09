/* ca-glitch: long play-and-record check for dropouts through a loopback cable.
 *
 *   ca-glitch [seconds=60] [rate] [buffer_frames] [out_ch=1] [in_ch=1]
 *
 * Patch output <out_ch> into input <in_ch> first (input gain low). Plays a steady
 * 997 Hz sine at -18 dBFS and checks every recorded sample: a pure sine obeys
 * x[n] = 2cos(w) x[n-1] - x[n-2], so the residual of that prediction is ~noise for a
 * clean signal and spikes at any click, dropout or repeated block, on either the
 * output or the input side. Also counts gaps in the input timestamps, Core Audio
 * overloads, and the driver's own restarts and USB errors.
 */
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *UID = "co.ke.kimina.eie-pro.device";
static const AudioObjectPropertySelector kPropStatus = 'eSta';

static double gRate, w, c2, amp, inLevel;
static int outCh, inCh;
static double startT = -1;
static double x1v, x2v; static long have;          /* last two input samples */
static double lastInEnd = -1;
static long inFrames, glitches, gaps, warm;
static double worst, noiseSum; static long noiseN;
static double glitchAt[64]; static int nGlitchAt;
static volatile long overloads;
static int selftest; static double inSq; static long inSqN;

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
    if (startT < 0) startT = outTime->mSampleTime;
    UInt32 b, c;
    if (out) {
        for (UInt32 x = 0; x < out->mNumberBuffers; x++) memset(out->mBuffers[x].mData, 0, out->mBuffers[x].mDataByteSize);
        if (locate(out, outCh, &b, &c)) {
            AudioBuffer *ab = &out->mBuffers[b];
            UInt32 nch = ab->mNumberChannels, n = ab->mDataByteSize / (4 * nch);
            float *f = ab->mData;
            /* phase from the sample time: a skipped or repeated cycle shows as a glitch */
            for (UInt32 i = 0; i < n; i++) {
                double t = outTime->mSampleTime - startT + i;
                if (selftest && t > gRate * 5) t += 10;              /* fake a 10-frame dropout */
                double g = t < gRate * 0.05 ? t / (gRate * 0.05) : 1;   /* 50 ms fade in */
                f[i * nch + c] = (float)(amp * g * sin(w * fmod(t, gRate * 1000)));
            }
        }
    }
    if (in && locate(in, inCh, &b, &c)) {
        const AudioBuffer *ab = &in->mBuffers[b];
        UInt32 nch = ab->mNumberChannels, n = ab->mDataByteSize / (4 * nch);
        const float *f = ab->mData;
        if (lastInEnd >= 0 && fabs(inTime->mSampleTime - lastInEnd) > 0.5) { gaps++; have = 0; }
        lastInEnd = inTime->mSampleTime + n;
        for (UInt32 i = 0; i < n; i++) {
            double x = f[i * nch + c];
            if (warm > (long)(gRate * 1.0)) { inSq += x * x; inSqN++; }
            /* learn the received tone's peak between 0.5 and 1 s; glitches are judged against it */
            if (warm > (long)(gRate * 0.5) && warm <= (long)(gRate * 1.0) && fabs(x) > inLevel) inLevel = fabs(x);
            if (have >= 2) {
                double r = fabs(x - c2 * x1v + x2v);
                if (warm > (long)(gRate * 1.0)) {            /* skip the first second */
                    noiseSum += r * r; noiseN++;
                    if (r > worst) worst = r;
                    if (r > 0.1 * inLevel) {
                        glitches++;
                        if (nGlitchAt < 64) glitchAt[nGlitchAt++] = (inTime->mSampleTime - startT + i) / gRate;
                        have = 0;                            /* one count per event */
                        x2v = x1v; x1v = x; continue;
                    }
                }
            }
            x2v = x1v; x1v = x; have++; warm++;
        }
        inFrames += n;
    }
    return 0;
}

static OSStatus onOverload(AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a, void *u)
{
    overloads++;
    return 0;
}

static double lateIn, lateInMax, lateOut, lateOutMax;
static int driverStats(AudioObjectID dev, double *restarts, double *errors)
{
    AudioObjectPropertyAddress a = {kPropStatus, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFDictionaryRef d = NULL; UInt32 sz = sizeof d;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &d) || !d) return 0;
    CFNumberRef r = CFDictionaryGetValue(d, CFSTR("restarts")), e = CFDictionaryGetValue(d, CFSTR("errors"));
    *restarts = *errors = -1;
    if (r) CFNumberGetValue(r, kCFNumberDoubleType, restarts);
    if (e) CFNumberGetValue(e, kCFNumberDoubleType, errors);
    CFNumberRef x;
    if ((x = CFDictionaryGetValue(d, CFSTR("inLate")))) CFNumberGetValue(x, kCFNumberDoubleType, &lateIn);
    if ((x = CFDictionaryGetValue(d, CFSTR("inLateMax")))) CFNumberGetValue(x, kCFNumberDoubleType, &lateInMax);
    if ((x = CFDictionaryGetValue(d, CFSTR("outLate")))) CFNumberGetValue(x, kCFNumberDoubleType, &lateOut);
    if ((x = CFDictionaryGetValue(d, CFSTR("outLateMax")))) CFNumberGetValue(x, kCFNumberDoubleType, &lateOutMax);
    CFRelease(d);
    return 1;
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 60, rate = argc > 2 ? atof(argv[2]) : 0;
    UInt32 bufFrames = argc > 3 ? (UInt32)atoi(argv[3]) : 0;
    outCh = (argc > 4 ? atoi(argv[4]) : 1) - 1;
    inCh = (argc > 5 ? atoi(argv[5]) : 1) - 1;
    if (outCh < 0 || outCh > 3 || inCh < 0 || inCh > 3) { fprintf(stderr, "channels are 1-4\n"); return 2; }

    AudioObjectID dev = 0;
    {
        CFStringRef u = CFStringCreateWithCString(NULL, UID, kCFStringEncodingUTF8);
        AudioValueTranslation t = {&u, sizeof(CFStringRef), &dev, sizeof dev};
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
    selftest = getenv("GLITCH_SELFTEST") != NULL;
    w = 2 * M_PI * 997.0 / rate; c2 = 2 * cos(w); amp = pow(10, -18 / 20.0);

    AudioDeviceIOProcID pid;
    if (AudioDeviceCreateIOProcID(dev, ioProc, NULL, &pid)) { printf("IOProc failed\n"); return 1; }
    if (bufFrames) {   /* a set while the HAL is busy can be dropped: retry until it reads back */
        AudioObjectPropertyAddress a = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        for (int i = 0; i < 10; i++) {
            AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof bufFrames, &bufFrames);
            usleep(200000);
            UInt32 got = 0, sz = sizeof got;
            AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &got);
            if (got == bufFrames) break;
        }
    }
    AudioObjectPropertyAddress ov = {kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectAddPropertyListener(dev, &ov, onOverload, NULL);
    double r0 = 0, e0 = 0, r1 = 0, e1 = 0;
    driverStats(dev, &r0, &e0);
    if (AudioDeviceStart(dev, pid)) { printf("start failed\n"); return 1; }
    UInt32 buf = 0;
    { AudioObjectPropertyAddress a = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
      UInt32 sz = sizeof buf; AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &buf); }
    UInt32 safeO = 0, safeI = 0;
    { AudioObjectPropertyAddress a = {kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
      UInt32 sz = 4; AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &safeO);
      a.mScope = kAudioObjectPropertyScopeInput; AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &safeI); }
    printf("EIE Pro %.0f Hz, buffer %u, safety out %u / in %u, out %d -> in %d, %.0f s ...\n",
           rate, buf, safeO, safeI, outCh + 1, inCh + 1, secs);
    fflush(stdout);
    for (double t = 0; t < secs; t += 0.25) usleep(250000);
    driverStats(dev, &r1, &e1);
    AudioDeviceStop(dev, pid);
    AudioDeviceDestroyIOProcID(dev, pid);
    AudioObjectRemovePropertyListener(dev, &ov, onOverload, NULL);

    double noise = noiseN ? sqrt(noiseSum / noiseN) : 0;
    printf("  input %.1f s, residual rms %.1f dB below the tone, worst %.1f dB below\n",
           inFrames / rate, noise > 0 ? -20 * log10(noise / amp) : 0.0, worst > 0 ? -20 * log10(worst / amp) : 0.0);
    printf("  glitches %ld, input timestamp gaps %ld, overloads %ld, driver restarts %+.0f, USB errors %+.0f\n",
           glitches, gaps, overloads, r1 - r0, e1 - e0);
    printf("  driver: input read early %.0f times (worst %.0f frames), output sent unwritten %.0f times (worst %.0f frames)\n",
           lateIn, lateInMax, lateOut, lateOutMax);
    if (nGlitchAt) {
        printf("  glitch times (s):");
        for (int i = 0; i < nGlitchAt; i++) printf(" %.2f", glitchAt[i]);
        printf("\n");
    }
    if (noiseN == 0) { printf("  -> no signal checked: is the cable patched?\n"); return 1; }
    double lvl = inSqN ? sqrt(2 * inSq / inSqN) : 0;
    printf("  input level %.1f dBFS peak\n", lvl > 0 ? 20 * log10(lvl) : -999.0);
    if (lvl < amp * 0.01) { printf("  -> input too quiet to judge: check the cable and gain\n"); return 1; }
    int clean = !glitches && !gaps && !overloads && r1 == r0;
    printf("  -> %s\n", clean ? "CLEAN" : "NOT CLEAN");
    return clean ? 0 : 3;
}
