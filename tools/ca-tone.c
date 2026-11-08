/* ca-tone: play a quiet sine through the "EIE Pro" Core Audio device (via the
 * installed plug-in), without changing the Mac's default output. Also meters the
 * device's inputs while it plays.
 *
 *   ca-tone [seconds=5] [level_dB=-30] [hz=440] [rate]
 *
 * With a rate (44100/48000/88200/96000), first sets the EIE Pro's sample rate, like
 * Audio MIDI Setup does, and checks it took.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const char *UID = "co.ke.kimina.eie-pro.device";
static double phase, step, amp, gRate = 48000;
static long done, fade; static volatile int stopping; static double outGain = 1;
static double inPeak[4], inSum[4]; static long inFrames;

static void outCb(void *u, AudioQueueRef q, AudioQueueBufferRef b) {
    float *f = b->mAudioData;
    UInt32 n = b->mAudioDataBytesCapacity / (4 * 2);
    for (UInt32 i = 0; i < n; i++) {
        if (stopping && outGain > 0) { outGain -= 1.0 / (gRate * 0.02); if (outGain < 0) outGain = 0; }
        double g = (done < fade ? (double)done / fade : 1.0) * outGain;
        float v = (float)(sin(phase) * amp * g);
        phase += step; if (phase > 2 * M_PI) phase -= 2 * M_PI;
        f[2 * i] = f[2 * i + 1] = v;
        done++;
    }
    b->mAudioDataByteSize = n * 8;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static void inCb(void *u, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *t, UInt32 np,
                 const AudioStreamPacketDescription *d) {
    float *f = b->mAudioData;
    UInt32 n = b->mAudioDataByteSize / 16;
    for (UInt32 i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) {
            double v = fabs(f[i * 4 + c]);
            if (v > inPeak[c]) inPeak[c] = v;
            inSum[c] += v * v;
        }
    inFrames += n;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 5, level = argc > 2 ? atof(argv[2]) : -30,
           hz = argc > 3 ? atof(argv[3]) : 440;
    if (level > -6) { fprintf(stderr, "refusing louder than -6 dBFS\n"); return 2; }
    double rate = argc > 4 ? atof(argv[4]) : 0;
    /* find the device by UID */
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
        OSStatus e = AudioObjectSetPropertyData(dev, &ra, 0, NULL, sizeof rate, &rate);
        if (e) { printf("setting %.0f Hz failed (%d)\n", rate, (int)e); return 1; }
        usleep(500000);
    }
    { UInt32 sz = sizeof rate; AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &rate); }
    printf("EIE Pro sample rate: %.0f Hz\n", rate);
    gRate = rate;
    step = 2 * M_PI * hz / rate; amp = pow(10, level / 20); fade = (long)(rate / 20);

    CFStringRef uid = CFStringCreateWithCString(NULL, UID, kCFStringEncodingUTF8);
    AudioStreamBasicDescription out = {rate, kAudioFormatLinearPCM,
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AudioStreamBasicDescription in = {rate, kAudioFormatLinearPCM,
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 16, 1, 16, 4, 32, 0};
    AudioQueueRef oq, iq;
    OSStatus s;
    if ((s = AudioQueueNewOutput(&out, outCb, NULL, NULL, NULL, 0, &oq))) { printf("new output %d\n", (int)s); return 1; }
    if ((s = AudioQueueSetProperty(oq, kAudioQueueProperty_CurrentDevice, &uid, sizeof uid))) {
        printf("EIE Pro device not found (%d): is the plug-in installed and the box plugged in?\n", (int)s); return 1; }
    if ((s = AudioQueueNewInput(&in, inCb, NULL, NULL, NULL, 0, &iq))) { printf("new input %d\n", (int)s); return 1; }
    if ((s = AudioQueueSetProperty(iq, kAudioQueueProperty_CurrentDevice, &uid, sizeof uid))) { printf("input device %d\n", (int)s); return 1; }
    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef b;
        AudioQueueAllocateBuffer(oq, 1024 * 8, &b); outCb(NULL, oq, b);
        AudioQueueAllocateBuffer(iq, 1024 * 16, &b); AudioQueueEnqueueBuffer(iq, b, 0, NULL);
    }
    if ((s = AudioQueueStart(iq, NULL))) printf("input start %d\n", (int)s);
    if ((s = AudioQueueStart(oq, NULL))) { printf("output start %d\n", (int)s); return 1; }
    printf("Playing %.0f Hz at %.0f dBFS on EIE Pro outputs 1-2 for %.0f s\n", hz, level, secs);
    usleep((useconds_t)(secs * 1e6));
    /* fade out over 20 ms before stopping */
    stopping = 1; usleep(150000);
    AudioQueueStop(oq, true); AudioQueueStop(iq, true);
    printf("played %ld frames (%.0f Hz); input %ld frames (%.0f Hz)\n", done, done / secs, inFrames, inFrames / secs);
    for (int c = 0; c < 4; c++)
        printf("  in%d peak %.1f dBFS rms %.1f dBFS\n", c + 1, 20 * log10(inPeak[c] + 1e-12),
               10 * log10(inSum[c] / (inFrames ? inFrames : 1) + 1e-24));
    return 0;
}
