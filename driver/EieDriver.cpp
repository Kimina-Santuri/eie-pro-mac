// EieDriver.cpp: Core Audio plug-in (AudioServerPlugIn) that makes an Akai EIE Pro
// a normal 4-in / 4-out sound device, using the user-space USB engine in src/.
//
// Built on libASPL (MIT, github.com/gavv/libASPL). Runs inside macOS's audio server.
// Logs: log stream --predicate 'subsystem == "co.ke.kimina.eie-pro"'
//
// Timeline: the device's sample time is the box's own clock (frames it has played,
// from the clock endpoint, smoothed by a DLL in the engine), so apps follow the box
// and nothing drifts. Apps write output into a ring at sample time T; the engine reads
// the ring when it sends frame T, a little before the box plays it. Input is written
// into a second ring at the sample time it was captured.

#include <aspl/Driver.hpp>

#include <CoreAudio/AudioServerPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/IOMessage.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <os/log.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "eie_usb.h"

namespace {

constexpr UInt32 kDefaultRate = 48000;
constexpr Float64 kRates[] = {44100, 48000, 88200, 96000};
constexpr UInt32 kChannels = EIE_CHANNELS;
constexpr UInt32 kRingFrames = 32768;      // power of two, ~340 ms at 96 kHz
constexpr UInt32 kZeroPeriod = 16384;
constexpr double kSafetySeconds = 0.032;   // > the engine's output lead (~24 ms)

bool IsSupportedRate(Float64 r)
{
    for (Float64 k : kRates)
        if (k == r) return true;
    return false;
}

os_log_t Log()
{
    static os_log_t log = os_log_create("co.ke.kimina.eie-pro", "driver");
    return log;
}

AudioStreamBasicDescription FloatFormat(Float64 rate)
{
    AudioStreamBasicDescription f = {};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBitsPerChannel = 32;
    f.mChannelsPerFrame = kChannels;
    f.mBytesPerFrame = 4 * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 4 * kChannels;
    return f;
}

// Link to the EIE Pro app. macOS keeps CoreMIDI per user session and this driver runs
// as a system process, so the app (a normal menu-bar app) hosts the MIDI ports and
// talks to the driver through custom properties on the device:
constexpr AudioObjectPropertySelector kPropStatus = 'eSta';     // get: dictionary
constexpr AudioObjectPropertySelector kPropKeepAwake = 'eKpA';  // get/set: number 0 or 1
constexpr AudioObjectPropertySelector kPropMidiIn = 'eMIn';     // get: data, drains; notified on arrival
constexpr AudioObjectPropertySelector kPropMidiOut = 'eMOu';    // set: data, raw MIDI bytes

class AppLink
{
public:
    AppLink() { queue_ = dispatch_queue_create("co.ke.kimina.eie-pro.applink", DISPATCH_QUEUE_SERIAL); }

    void SetDevice(const aspl::Object* d) { device_ = d; }
    void SetEngine(eie_t* e) { engine_.store(e); }

    // USB thread: raw bytes from the box's MIDI IN socket.
    static void FromBox(void* ctx, const unsigned char* b, int n)
    {
        auto* a = static_cast<AppLink*>(ctx);
        unsigned w = a->w_.load(std::memory_order_relaxed), r = a->r_.load(std::memory_order_acquire);
        for (int i = 0; i < n && (w - r) < kRing; i++) a->ring_[w++ & (kRing - 1)] = b[i];
        a->w_.store(w, std::memory_order_release);
        a->midiIn_ += n;
        if (!a->notifyPending_.exchange(true)) dispatch_async_f(a->queue_, a, &AppLink::Notify);
    }

    // App reads kPropMidiIn.
    CFPropertyListRef TakeMidiIn()
    {
        unsigned r = r_.load(std::memory_order_relaxed), w = w_.load(std::memory_order_acquire);
        UInt8 out[kRing];
        CFIndex n = 0;
        while (r != w) out[n++] = ring_[r++ & (kRing - 1)];
        r_.store(r, std::memory_order_release);
        return CFDataCreate(nullptr, out, n);
    }

    // App writes kPropMidiOut.
    void SendMidi(CFPropertyListRef v)
    {
        if (!v || CFGetTypeID(v) != CFDataGetTypeID()) return;
        auto d = static_cast<CFDataRef>(v);
        eie_t* e = engine_.load();
        if (!e) return;
        midiOut_ += eie_midi_send(e, CFDataGetBytePtr(d), int(CFDataGetLength(d)));
    }

    long MidiInTotal() const { return midiIn_.load(); }
    long MidiOutTotal() const { return midiOut_.load(); }

private:
    static void Notify(void* p)
    {
        auto* a = static_cast<AppLink*>(p);
        a->notifyPending_ = false;
        if (a->device_) a->device_->NotifyPropertyChanged(kPropMidiIn);
    }

    static constexpr unsigned kRing = 8192;
    UInt8 ring_[kRing];
    std::atomic<unsigned> w_{0}, r_{0};
    std::atomic<bool> notifyPending_{false};
    std::atomic<long> midiIn_{0}, midiOut_{0};
    std::atomic<eie_t*> engine_{nullptr};
    const aspl::Object* device_ = nullptr;
    dispatch_queue_t queue_;
};

AppLink& TheLink()
{
    static AppLink* a = new AppLink;
    return *a;
}

// Every newly opened engine gets the same logging and MIDI hooks.
void ConfigureEngine(eie_t* e)
{
    eie_set_log(e, [](const char* m) { os_log(Log(), "%{public}s", m); });
    eie_set_midi(e, &AppLink::FromBox, &TheLink());
    TheLink().SetEngine(e);
}

class EieDevice;

// A stream whose format follows the device's sample rate (and asks the device to
// change rate when an app picks a stream format with another rate).
class EieStream : public aspl::Stream
{
public:
    EieStream(std::shared_ptr<const aspl::Context> context, std::shared_ptr<EieDevice> device,
        const aspl::StreamParameters& params);

    // Inside a HAL configuration change only.
    void ApplyRate(Float64 rate)
    {
        aspl::Stream::SetPhysicalFormatImpl(FloatFormat(rate));
        aspl::Stream::SetVirtualFormatImpl(FloatFormat(rate));
    }

protected:
    OSStatus SetPhysicalFormatImpl(const AudioStreamBasicDescription& format) override;
    OSStatus SetVirtualFormatImpl(const AudioStreamBasicDescription& format) override;

private:
    std::weak_ptr<EieDevice> device_;
};

// One EIE Pro, as seen by Core Audio. The engine pointer is null while unplugged.
class EieDevice : public aspl::Device
{
public:
    EieDevice(std::shared_ptr<const aspl::Context> context, const aspl::DeviceParameters& params)
        : aspl::Device(std::move(context), params)
    {
    }

    std::atomic<eie_t*> engine{nullptr};
    std::mutex controlMutex;            // start / stop / plug / unplug / rate
    std::atomic<UInt64> startHostTime{0};
    bool ioRunning = false;             // under controlMutex: Core Audio wants I/O
    bool streaming = false;             // under controlMutex: the box is actually streaming
    bool keepAwake = false;             // under controlMutex: the app asked to keep streaming
    std::atomic<float> peak[kChannels] = {};      // input peaks since the app last read them
    std::atomic<float> outPeak[kChannels] = {};   // output peaks: the mix actually sent to the box
    std::vector<std::shared_ptr<EieStream>> eieStreams;

    alignas(64) float outRing[kRingFrames * kChannels] = {};
    alignas(64) float inRing[kRingFrames * kChannels] = {};

    eie_render_fn render = nullptr;
    eie_capture_fn capture = nullptr;

    UInt32 GetSafetyOffset() const override
    {
        return UInt32(GetNominalSampleRate() * kSafetySeconds);
    }

    // Start the engine at the current rate. Caller holds controlMutex.
    int StartEngineLocked()
    {
        eie_t* e = engine.load();
        if (!e) return -1;
        std::memset(outRing, 0, sizeof outRing);
        std::memset(inRing, 0, sizeof inRing);
        startHostTime = mach_absolute_time();
        int r = eie_start(e, UInt32(GetNominalSampleRate()), render, capture, this);
        streaming = (r == 0);
        return r;
    }

    // The USB handle can go stale (after sleep, or a box that reset). Close it and
    // open the box again. Caller holds controlMutex.
    bool ReopenLocked()
    {
        eie_t* old = engine.exchange(nullptr);
        TheLink().SetEngine(nullptr);
        streaming = false;
        if (old) {
            usleep(50 * 1000);   // let a real-time reader that loaded the pointer finish
            eie_close(old);
        }
        char err[256];
        eie_t* e = eie_open(err, sizeof err);
        if (!e) {
            os_log_error(Log(), "reopen failed: %{public}s", err);
            return false;
        }
        ConfigureEngine(e);
        engine = e;
        os_log(Log(), "reopened the EIE Pro (firmware %{public}s)", eie_firmware(e));
        return true;
    }

    // Start streaming; if that fails, reopen the box once and try again.
    // Caller holds controlMutex.
    int StartWithRecoveryLocked()
    {
        int r = StartEngineLocked();
        if (r == 0) return 0;
        os_log_error(Log(), "engine start failed (%{public}s); reopening the box", eie_strerror(r));
        if (!ReopenLocked()) return r;
        return StartEngineLocked();
    }

    // Change rate: device, both streams, and the box if it is streaming.
    // Called inside a HAL configuration change.
    OSStatus ApplyRate(Float64 rate)
    {
        if (!IsSupportedRate(rate)) return kAudioDeviceUnsupportedFormatError;
        aspl::Device::SetNominalSampleRateImpl(rate);
        for (auto& s : eieStreams) s->ApplyRate(rate);
        std::lock_guard<std::mutex> lock(controlMutex);
        if (ioRunning) {
            if (eie_t* e = engine.load()) eie_stop(e);
            streaming = false;
            int r = StartWithRecoveryLocked();
            if (r) os_log_error(Log(), "rate change: engine failed to restart at %.0f Hz: %{public}s", rate, eie_strerror(r));
        }
        os_log(Log(), "sample rate now %.0f Hz", rate);
        return kAudioHardwareNoError;
    }

protected:
    OSStatus SetNominalSampleRateImpl(Float64 rate) override
    {
        return ApplyRate(rate);
    }

    OSStatus GetZeroTimeStampImpl(UInt32, Float64* outSampleTime, UInt64* outHostTime,
        UInt64* outSeed) override
    {
        eie_clock_t c;
        eie_t* e = engine.load();
        if (e && eie_get_clock(e, &c) && c.valid && c.frames >= 0) {
            const long long zero = (c.frames / kZeroPeriod) * kZeroPeriod;
            *outSampleTime = Float64(zero);
            *outHostTime = UInt64(c.host - double(c.frames - zero) * c.ticks_per_frame);
            *outSeed = c.seed + 1;
        } else {   // clock not running yet: hold at zero
            *outSampleTime = 0;
            *outHostTime = startHostTime.load();
            *outSeed = 1;
        }
        return kAudioHardwareNoError;
    }
};

EieStream::EieStream(std::shared_ptr<const aspl::Context> context, std::shared_ptr<EieDevice> device,
    const aspl::StreamParameters& params)
    : aspl::Stream(std::move(context), device, params)
    , device_(device)
{
}

OSStatus EieStream::SetPhysicalFormatImpl(const AudioStreamBasicDescription& format)
{
    if (!IsSupportedRate(format.mSampleRate)) return kAudioDeviceUnsupportedFormatError;
    auto d = device_.lock();
    if (d && format.mSampleRate != d->GetNominalSampleRate()) return d->ApplyRate(format.mSampleRate);
    return aspl::Stream::SetPhysicalFormatImpl(format);
}

OSStatus EieStream::SetVirtualFormatImpl(const AudioStreamBasicDescription& format)
{
    return SetPhysicalFormatImpl(format);
}

class Handler : public aspl::ControlRequestHandler, public aspl::IORequestHandler
{
public:
    explicit Handler(EieDevice* device)
        : dev_(device)
    {
    }

    OSStatus OnStartIO() override
    {
        std::lock_guard<std::mutex> lock(dev_->controlMutex);
        eie_t* e = dev_->engine.load();
        if (!e) {
            os_log_error(Log(), "StartIO: no EIE Pro connected");
            return kAudioHardwareNotRunningError;
        }
        if (dev_->streaming) {   // already running (kept awake by the app): just join
            dev_->ioRunning = true;
            os_log(Log(), "StartIO: joined the running stream at %.0f Hz", dev_->GetNominalSampleRate());
            return kAudioHardwareNoError;
        }
        int r = dev_->StartWithRecoveryLocked();
        if (r) {
            os_log_error(Log(), "StartIO: engine failed to start: %{public}s", eie_strerror(r));
            return kAudioHardwareUnspecifiedError;
        }
        dev_->ioRunning = true;
        os_log(Log(), "StartIO: streaming at %.0f Hz", dev_->GetNominalSampleRate());
        return kAudioHardwareNoError;
    }

    void OnStopIO() override
    {
        std::lock_guard<std::mutex> lock(dev_->controlMutex);
        dev_->ioRunning = false;
        if (dev_->keepAwake) {
            os_log(Log(), "StopIO: kept streaming (keep awake is on)");
            return;
        }
        dev_->streaming = false;
        if (eie_t* e = dev_->engine.load()) {
            eie_stats_t st;
            eie_get_stats(e, &st);
            double gap = eie_max_clock_gap_ms(e);
            eie_stop(e);
            os_log(Log(), "StopIO: clock %ld frames, sent %ld, received %ld, errors %ld, restarts %ld, longest clock gap %.1f ms",
                st.clock_frames, st.out_frames, st.in_frames, st.errors, st.restarts, gap);
        }
    }

    void OnReadClientInput(const std::shared_ptr<aspl::Client>&, const std::shared_ptr<aspl::Stream>&,
        Float64, Float64 timestamp, void* bytes, UInt32 bytesCount) override
    {
        float* dst = static_cast<float*>(bytes);
        const UInt32 frames = bytesCount / (4 * kChannels);
        long long pos = (long long)timestamp;
        for (UInt32 i = 0; i < frames; i++, pos++) {
            const float* src = &dev_->inRing[(pos & (kRingFrames - 1)) * kChannels];
            for (UInt32 c = 0; c < kChannels; c++) dst[i * kChannels + c] = src[c];
        }
    }

    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>&, Float64, Float64 timestamp,
        const void* bytes, UInt32 bytesCount) override
    {
        const float* src = static_cast<const float*>(bytes);
        const UInt32 frames = bytesCount / (4 * kChannels);
        long long pos = (long long)timestamp;
        for (UInt32 i = 0; i < frames; i++, pos++) {
            float* dst = &dev_->outRing[(pos & (kRingFrames - 1)) * kChannels];
            for (UInt32 c = 0; c < kChannels; c++) dst[c] = src[i * kChannels + c];
        }
    }

public:
    // Engine thread: take frames [pos, pos+n) from the output ring, clearing them so
    // nothing repeats if apps stop writing.
    static void Render(void* ctx, float* out, int n, long long pos)
    {
        auto* d = static_cast<EieDevice*>(ctx);
        float pk[kChannels] = {};
        for (int i = 0; i < n; i++, pos++) {
            float* src = &d->outRing[(pos & (kRingFrames - 1)) * kChannels];
            for (UInt32 c = 0; c < kChannels; c++) {
                float v = src[c];
                out[i * kChannels + c] = v;
                src[c] = 0.f;
                v = v < 0 ? -v : v;
                if (v > pk[c]) pk[c] = v;
            }
        }
        for (UInt32 c = 0; c < kChannels; c++)
            if (pk[c] > d->outPeak[c].load(std::memory_order_relaxed)) d->outPeak[c].store(pk[c], std::memory_order_relaxed);
    }

    static void Capture(void* ctx, const float* in, int n, long long pos)
    {
        auto* d = static_cast<EieDevice*>(ctx);
        float pk[kChannels] = {};
        for (int i = 0; i < n; i++, pos++) {
            float* dst = &d->inRing[(pos & (kRingFrames - 1)) * kChannels];
            for (UInt32 c = 0; c < kChannels; c++) {
                float v = in[i * kChannels + c];
                dst[c] = v;
                v = v < 0 ? -v : v;
                if (v > pk[c]) pk[c] = v;
            }
        }
        for (UInt32 c = 0; c < kChannels; c++)
            if (pk[c] > d->peak[c].load(std::memory_order_relaxed)) d->peak[c].store(pk[c], std::memory_order_relaxed);
    }

    EieDevice* dev_;
};

// Watches for the EIE Pro being plugged in or out (IOKit notifications, no polling)
// and adds or removes the device from Core Audio.
class Hotplug
{
public:
    Hotplug(std::shared_ptr<aspl::Plugin> plugin, std::shared_ptr<EieDevice> device)
        : plugin_(std::move(plugin))
        , device_(std::move(device))
    {
        // High priority: libusb starts its own completion thread from here (at open),
        // and that thread inherits this priority. At normal priority, inside the audio
        // server, it was sometimes late enough for the box to run dry and halt.
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        queue_ = dispatch_queue_create("co.ke.kimina.eie-pro.hotplug", attr);
    }

    void Start()
    {
        port_ = IONotificationPortCreate(kIOMainPortDefault);
        IONotificationPortSetDispatchQueue(port_, queue_);
        IOServiceAddMatchingNotification(port_, kIOFirstMatchNotification, Matching(), &Hotplug::OnArrive,
            this, &arriveIter_);
        IOServiceAddMatchingNotification(port_, kIOTerminatedNotification, Matching(), &Hotplug::OnLeave,
            this, &leaveIter_);

        // Sleep and wake (lid close/open): stop the box cleanly before sleep, and
        // bring it back afterwards if an app was using it.
        powerRoot_ = IORegisterForSystemPower(this, &powerPort_, &Hotplug::OnPower, &powerNotifier_);
        if (powerRoot_) IONotificationPortSetDispatchQueue(powerPort_, queue_);
        else os_log_error(Log(), "could not register for sleep/wake notifications");
        dispatch_async_f(queue_, this, [](void* self) {
            auto* h = static_cast<Hotplug*>(self);
            Drain(h->leaveIter_);
            if (Drain(h->arriveIter_)) h->TryOpen(0);
            else os_log(Log(), "waiting for an EIE Pro to be plugged in");
        });
    }

private:
    static CFMutableDictionaryRef Matching()
    {
        CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
        int vid = 0x09e8, pid = 0x0010;
        CFNumberRef v = CFNumberCreate(nullptr, kCFNumberIntType, &vid);
        CFNumberRef p = CFNumberCreate(nullptr, kCFNumberIntType, &pid);
        CFDictionarySetValue(m, CFSTR("idVendor"), v);
        CFDictionarySetValue(m, CFSTR("idProduct"), p);
        CFRelease(v);
        CFRelease(p);
        return m;   // consumed by IOServiceAddMatchingNotification
    }

    static bool Drain(io_iterator_t it)
    {
        bool any = false;
        while (io_object_t o = IOIteratorNext(it)) {
            IOObjectRelease(o);
            any = true;
        }
        return any;
    }

    static void OnArrive(void* self, io_iterator_t it)
    {
        if (Drain(it)) static_cast<Hotplug*>(self)->TryOpen(0);
    }

    static void OnLeave(void* self, io_iterator_t it)
    {
        if (Drain(it)) static_cast<Hotplug*>(self)->Close();
    }

    // The box needs a moment after arriving; retry a few times.
    void TryOpen(int attempt)
    {
        struct Ctx { Hotplug* h; int attempt; };
        auto* c = new Ctx{this, attempt};
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (attempt ? 1000 : 500) * NSEC_PER_MSEC), queue_, c,
            [](void* p) {
                auto* c = static_cast<Ctx*>(p);
                c->h->Open(c->attempt);
                delete c;
            });
    }

    void Open(int attempt)
    {
        if (device_->engine.load()) return;
        char err[256];
        eie_t* e = eie_open(err, sizeof err);
        if (!e) {
            os_log_error(Log(), "open failed (attempt %d): %{public}s", attempt + 1, err);
            if (attempt < 15) TryOpen(attempt + 1);   // after a reinstall the old copy can hold the box for a few seconds
            return;
        }
        {
            std::lock_guard<std::mutex> lock(device_->controlMutex);
            device_->engine = e;
        }
        ConfigureEngine(e);
        if (!added_) {
            plugin_->AddDevice(device_);
            added_ = true;
        }
        {
            std::lock_guard<std::mutex> lock(device_->controlMutex);
            if (device_->keepAwake && !device_->streaming) device_->StartWithRecoveryLocked();
        }
        os_log(Log(), "EIE Pro connected, firmware %{public}s", eie_firmware(e));
    }

    static void OnPower(void* self, io_service_t, natural_t type, void* arg)
    {
        auto* h = static_cast<Hotplug*>(self);
        switch (type) {
        case kIOMessageCanSystemSleep:
            IOAllowPowerChange(h->powerRoot_, (long)arg);
            break;
        case kIOMessageSystemWillSleep:
            h->WillSleep();
            IOAllowPowerChange(h->powerRoot_, (long)arg);
            break;
        case kIOMessageSystemHasPoweredOn:
            // give USB a moment to come back before touching the box
            dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), h->queue_, h,
                [](void* p) { static_cast<Hotplug*>(p)->DidWake(); });
            break;
        default:
            break;
        }
    }

    void WillSleep()
    {
        std::lock_guard<std::mutex> lock(device_->controlMutex);
        wasRunning_ = device_->ioRunning;
        if (device_->streaming)
            if (eie_t* e = device_->engine.load()) eie_stop(e);
        device_->streaming = false;
        os_log(Log(), "going to sleep (was streaming: %{public}s)", wasRunning_ ? "yes" : "no");
    }

    void DidWake()
    {
        std::lock_guard<std::mutex> lock(device_->controlMutex);
        if (!device_->engine.load()) {
            os_log(Log(), "woke up; no EIE Pro open");
            return;
        }
        if (!device_->ioRunning && !device_->keepAwake) {
            // Nothing playing: refresh the USB handle now so the next play starts cleanly.
            device_->ReopenLocked();
            os_log(Log(), "woke up; box reopened, idle");
            return;
        }
        if (device_->streaming) {   // Core Audio already restarted it on wake
            os_log(Log(), "woke up; already streaming");
            return;
        }
        int r = device_->StartWithRecoveryLocked();
        os_log(Log(), "woke up; streaming %{public}s", r == 0 ? "resumed" : "could not resume");
    }

    void Close()
    {
        eie_t* e;
        {
            std::lock_guard<std::mutex> lock(device_->controlMutex);
            e = device_->engine.exchange(nullptr);
            TheLink().SetEngine(nullptr);
            device_->ioRunning = false;
            device_->streaming = false;
        }
        if (added_) {
            plugin_->RemoveDevice(device_);
            added_ = false;
        }
        if (e) {
            usleep(200 * 1000);   // let any real-time reader that loaded the pointer finish
            eie_close(e);
        }
        os_log(Log(), "EIE Pro disconnected");
    }

    std::shared_ptr<aspl::Plugin> plugin_;
    std::shared_ptr<EieDevice> device_;
    dispatch_queue_t queue_;
    IONotificationPortRef port_ = nullptr;
    io_iterator_t arriveIter_ = 0, leaveIter_ = 0;
    io_connect_t powerRoot_ = 0;
    IONotificationPortRef powerPort_ = nullptr;
    io_object_t powerNotifier_ = 0;
    bool wasRunning_ = false;
    bool added_ = false;
};

std::shared_ptr<aspl::Driver> CreateDriver()
{
    auto context = std::make_shared<aspl::Context>();

    aspl::DeviceParameters params;
    params.Name = "EIE Pro";
    params.Manufacturer = "Akai Professional";
    params.DeviceUID = "co.ke.kimina.eie-pro.device";
    params.ModelUID = "co.ke.kimina.eie-pro.model";
    params.SampleRate = kDefaultRate;
    params.ChannelCount = kChannels;
    params.SafetyOffset = UInt32(kDefaultRate * kSafetySeconds);   // GetSafetyOffset() scales it with the rate
    params.ZeroTimeStampPeriod = kZeroPeriod;
    params.EnableMixing = true;

    auto device = std::make_shared<EieDevice>(context, params);

    std::vector<AudioValueRange> rates;
    std::vector<AudioStreamRangedDescription> formats;
    for (Float64 r : kRates) {
        rates.push_back({r, r});
        AudioStreamRangedDescription f = {};
        f.mFormat = FloatFormat(r);
        f.mSampleRateRange = {r, r};
        formats.push_back(f);
    }
    device->SetAvailableSampleRatesAsync(rates);

    for (auto dir : {aspl::Direction::Output, aspl::Direction::Input}) {
        aspl::StreamParameters sp;
        sp.Direction = dir;
        sp.Format = FloatFormat(kDefaultRate);
        auto s = std::make_shared<EieStream>(context, device, sp);
        s->SetAvailablePhysicalFormatsAsync(formats);
        s->SetAvailableVirtualFormatsAsync(formats);
        device->AddStreamAsync(s);
        device->eieStreams.push_back(s);
    }

    device->render = &Handler::Render;
    device->capture = &Handler::Capture;
    TheLink().SetDevice(device.get());
    EieDevice* dev = device.get();

    device->RegisterCustomProperty(kPropStatus, [dev]() -> CFPropertyListRef {
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        auto num = [&](CFStringRef k, double v) {
            CFNumberRef n = CFNumberCreate(nullptr, kCFNumberDoubleType, &v);
            CFDictionarySetValue(d, k, n);
            CFRelease(n);
        };
        eie_t* e = dev->engine.load();
        CFDictionarySetValue(d, CFSTR("connected"), e ? kCFBooleanTrue : kCFBooleanFalse);
        CFDictionarySetValue(d, CFSTR("streaming"), dev->streaming ? kCFBooleanTrue : kCFBooleanFalse);
        CFDictionarySetValue(d, CFSTR("keepAwake"), dev->keepAwake ? kCFBooleanTrue : kCFBooleanFalse);
        num(CFSTR("rate"), dev->GetNominalSampleRate());
        if (e) {
            CFStringRef fw = CFStringCreateWithCString(nullptr, eie_firmware(e), kCFStringEncodingUTF8);
            CFDictionarySetValue(d, CFSTR("firmware"), fw);
            CFRelease(fw);
            eie_stats_t st;
            eie_get_stats(e, &st);
            num(CFSTR("restarts"), st.restarts);
            num(CFSTR("errors"), st.errors);
        }
        auto peaks = [&](CFStringRef key, std::atomic<float>* src) {
            CFMutableArrayRef a = CFArrayCreateMutable(nullptr, kChannels, &kCFTypeArrayCallBacks);
            for (UInt32 c = 0; c < kChannels; c++) {
                double v = src[c].exchange(0.f);
                CFNumberRef n = CFNumberCreate(nullptr, kCFNumberDoubleType, &v);
                CFArrayAppendValue(a, n);
                CFRelease(n);
            }
            CFDictionarySetValue(d, key, a);
            CFRelease(a);
        };
        peaks(CFSTR("peaks"), dev->peak);
        peaks(CFSTR("outPeaks"), dev->outPeak);
        num(CFSTR("midiIn"), TheLink().MidiInTotal());
        num(CFSTR("midiOut"), TheLink().MidiOutTotal());
        return d;
    });

    device->RegisterCustomProperty(kPropKeepAwake,
        [dev]() -> CFPropertyListRef {
            int v = dev->keepAwake ? 1 : 0;
            return CFNumberCreate(nullptr, kCFNumberIntType, &v);
        },
        [dev](CFPropertyListRef v) {
            int on = 0;
            if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &on);
            else if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) on = CFBooleanGetValue((CFBooleanRef)v);
            std::lock_guard<std::mutex> lock(dev->controlMutex);
            dev->keepAwake = on != 0;
            if (dev->keepAwake && dev->engine.load() && !dev->streaming) dev->StartWithRecoveryLocked();
            if (!dev->keepAwake && dev->streaming && !dev->ioRunning) {
                if (eie_t* e = dev->engine.load()) eie_stop(e);
                dev->streaming = false;
            }
            os_log(Log(), "keep awake %{public}s", dev->keepAwake ? "on" : "off");
        });

    device->RegisterCustomProperty(kPropMidiIn, []() -> CFPropertyListRef { return TheLink().TakeMidiIn(); });
    device->RegisterCustomProperty(kPropMidiOut,
        []() -> CFPropertyListRef { return CFDataCreate(nullptr, nullptr, 0); },
        [](CFPropertyListRef v) { TheLink().SendMidi(v); });

    auto handler = std::make_shared<Handler>(device.get());
    device->SetControlHandler(handler);
    device->SetIOHandler(handler);

    auto plugin = std::make_shared<aspl::Plugin>(context);
    auto driver = std::make_shared<aspl::Driver>(context, plugin);

    static Hotplug* hotplug = new Hotplug(plugin, device);   // lives as long as the audio server
    hotplug->Start();
    os_log(Log(), "EIE Pro driver loaded");
    return driver;
}

} // namespace

extern "C" void* EieProEntryPoint(CFAllocatorRef, CFUUIDRef typeUUID)
{
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) return nullptr;
    static std::shared_ptr<aspl::Driver> driver = CreateDriver();
    return driver->GetReference();
}
