/* ca-timing: show or set the EIE Pro driver's output queue and safety margin.
 *
 *   ca-timing                    show queue, margin, safety offset, latencies
 *   ca-timing QUEUE_MS [INPUT_MARGIN_MS]
 *
 * A running engine restarts with the new queue (a short gap in the sound).
 */
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const char *UID = "co.ke.kimina.eie-pro.device";

static void setInt(AudioObjectID dev, AudioObjectPropertySelector sel, int v)
{
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    OSStatus e = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof n, &n);
    CFRelease(n);
    if (e) printf("setting failed (%d): is the new driver installed?\n", (int)e);
}

/* Read from the driver's live status (a plain property read can come from a HAL cache). */
static int statusInt(AudioObjectID dev, CFStringRef key)
{
    AudioObjectPropertyAddress a = {'eSta', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFDictionaryRef d = NULL; UInt32 sz = sizeof d; double v = -1;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &d) || !d) return -1;
    CFNumberRef n = CFDictionaryGetValue(d, key);
    if (n) CFNumberGetValue(n, kCFNumberDoubleType, &v);
    CFRelease(d);
    return (int)v;
}

/* A set sent while the HAL is busy (IO starting or stopping) can be dropped: retry. */
static void setChecked(AudioObjectID dev, AudioObjectPropertySelector sel, CFStringRef key, int v)
{
    for (int i = 0; i < 5; i++) {
        setInt(dev, sel, v);
        usleep(400000);
        if (statusInt(dev, key) == v) return;
    }
    printf("value did not stick after 5 tries\n");
}

static double statNum(CFDictionaryRef d, CFStringRef k)
{
    double v = -1;
    CFNumberRef n = CFDictionaryGetValue(d, k);
    if (n) CFNumberGetValue(n, kCFNumberDoubleType, &v);
    return v;
}

int main(int argc, char **argv)
{
    AudioObjectID dev = 0;
    {
        CFStringRef u = CFStringCreateWithCString(NULL, UID, kCFStringEncodingUTF8);
        AudioValueTranslation t = {&u, sizeof(CFStringRef), &dev, sizeof dev};
        AudioObjectPropertyAddress a = {kAudioHardwarePropertyDeviceForUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 sz = sizeof t;
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &t);
        if (!dev) { printf("EIE Pro device not found\n"); return 1; }
    }
    if (argc > 1) setChecked(dev, 'eQue', CFSTR("queueMs"), atoi(argv[1]));
    if (argc > 2) setChecked(dev, 'eMar', CFSTR("marginMs"), atoi(argv[2]));

    AudioObjectPropertyAddress a = {'eSta', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFDictionaryRef d = NULL; UInt32 sz = sizeof d;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &d) || !d) { printf("no status from driver\n"); return 1; }
    double rate = statNum(d, CFSTR("rate")), q = statNum(d, CFSTR("queueMs")), m = statNum(d, CFSTR("marginMs"));
    double lmax = statNum(d, CFSTR("leadMax")), lmin = statNum(d, CFSTR("leadMin"));
    CFRelease(d);
    UInt32 so = 0, si = 0; sz = 4;
    AudioObjectPropertyAddress s = {kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(dev, &s, 0, NULL, &sz, &so);
    s.mScope = kAudioObjectPropertyScopeInput; sz = 4;
    AudioObjectGetPropertyData(dev, &s, 0, NULL, &sz, &si);
    if (q < 0) { printf("driver has no queue setting: install the new driver\n"); return 1; }
    printf("rate %.0f Hz, output queue %.0f ms, input margin %.0f ms, safety offset out %u / in %u frames (%.1f / %.1f ms)\n",
           rate, q, m, so, si, so * 1000.0 / rate, si * 1000.0 / rate);
    if (lmax > 0) printf("engine lead since start: %.0f-%.0f frames (%.1f-%.1f ms)\n", lmin, lmax, lmin * 1000 / rate, lmax * 1000 / rate);
    return 0;
}
