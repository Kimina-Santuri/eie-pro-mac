// DeviceLink: talks to the EIE Pro Core Audio driver. Standard properties for the sample
// rate; the driver's custom properties for status, input peaks, keep-awake and MIDI.

import CoreAudio
import Foundation
import ServiceManagement

let kDeviceUID = "co.ke.kimina.eie-pro.device"
let kRates: [Double] = [44100, 48000, 88200, 96000]

private func fourCC(_ s: String) -> AudioObjectPropertySelector {
    s.utf8.reduce(0) { ($0 << 8) | AudioObjectPropertySelector($1) }
}

private let kPropStatus = fourCC("eSta")
private let kPropKeepAwake = fourCC("eKpA")
private let kPropMidiIn = fourCC("eMIn")
private let kPropMidiOut = fourCC("eMOu")

private func address(_ sel: AudioObjectPropertySelector) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal,
                               mElement: kAudioObjectPropertyElementMain)
}

final class DeviceLink: ObservableObject {
    @Published private(set) var connected = false
    @Published private(set) var firmware = ""
    @Published private(set) var rate: Double = 48000
    @Published private(set) var streaming = false
    @Published private(set) var restarts = 0
    @Published private(set) var levels: [Float] = [-90, -90, -90, -90]   // dBFS, with a slow fall
    @Published private(set) var midiIn = 0
    @Published private(set) var midiOut = 0
    @Published var keepAwake = UserDefaults.standard.bool(forKey: "keepAwake") {
        didSet {
            UserDefaults.standard.set(keepAwake, forKey: "keepAwake")
            applyKeepAwake()
        }
    }

    @Published var openAtLogin = SMAppService.mainApp.status == .enabled {
        didSet {
            guard openAtLogin != (SMAppService.mainApp.status == .enabled) else { return }
            do { openAtLogin ? try SMAppService.mainApp.register() : try SMAppService.mainApp.unregister() }
            catch { DispatchQueue.main.async { self.openAtLogin = SMAppService.mainApp.status == .enabled } }
        }
    }

    let midi = MidiBridge()
    private var device = AudioObjectID(0)
    private var timer: Timer?
    private let queue = DispatchQueue(label: "co.ke.kimina.eie-pro.app.link")
    private var midiListener: AudioObjectPropertyListenerBlock?

    init() {
        midi.onSend = { [weak self] bytes in self?.sendMidi(bytes) }
        var devices = address(kAudioHardwarePropertyDevices)
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &devices, .main) { [weak self] _, _ in
            self?.findDevice()
        }
        findDevice()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 15, repeats: true) { [weak self] _ in self?.poll() }
    }

    // MARK: device

    private func findDevice() {
        var uid = kDeviceUID as CFString
        var id = AudioObjectID(0)
        var a = address(kAudioHardwarePropertyTranslateUIDToDevice)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let err = withUnsafeMutablePointer(to: &uid) { uidPtr in
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a,
                                       UInt32(MemoryLayout<CFString>.size), uidPtr, &size, &id)
        }
        let found = err == noErr ? id : 0
        guard found != device else { return }
        if device != 0, let l = midiListener {
            var m = address(kPropMidiIn)
            AudioObjectRemovePropertyListenerBlock(device, &m, queue, l)
        }
        device = found
        connected = found != 0
        if found != 0 {
            let l: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.readMidiIn() }
            var m = address(kPropMidiIn)
            AudioObjectAddPropertyListenerBlock(found, &m, queue, l)
            midiListener = l
            applyKeepAwake()
        }
    }

    private func getPlist(_ sel: AudioObjectPropertySelector, on dev: AudioObjectID) -> CFPropertyList? {
        var a = address(sel)
        var ref: Unmanaged<CFPropertyList>?
        var size = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
        guard AudioObjectGetPropertyData(dev, &a, 0, nil, &size, &ref) == noErr, let r = ref else { return nil }
        return r.takeRetainedValue()
    }

    @discardableResult
    private func setPlist(_ sel: AudioObjectPropertySelector, _ value: CFPropertyList, on dev: AudioObjectID) -> OSStatus {
        var a = address(sel)
        var v: Unmanaged<CFPropertyList> = Unmanaged.passUnretained(value)   // the property data is the reference
        return AudioObjectSetPropertyData(dev, &a, 0, nil, UInt32(MemoryLayout<Unmanaged<CFPropertyList>>.size), &v)
    }

    // MARK: status and meters (main thread, 15 times a second)

    private func poll() {
        guard device != 0, let d = getPlist(kPropStatus, on: device) as? [String: Any] else {
            if connected { connected = false }
            return
        }
        connected = (d["connected"] as? Bool) ?? false
        firmware = (d["firmware"] as? String) ?? ""
        streaming = (d["streaming"] as? Bool) ?? false
        if let r = d["rate"] as? Double, r != rate { rate = r }
        restarts = Int((d["restarts"] as? Double) ?? 0)
        midiIn = Int((d["midiIn"] as? Double) ?? 0)
        midiOut = Int((d["midiOut"] as? Double) ?? 0)
        if let peaks = d["peaks"] as? [Double], peaks.count == 4 {
            var next = levels
            for i in 0..<4 {
                let db = Float(peaks[i] > 1e-6 ? 20 * log10(peaks[i]) : -90)
                next[i] = max(db, levels[i] - 1.5)   // fall about 22 dB a second
            }
            levels = next
        }
        readMidiIn()   // in case a notification was missed
    }

    // MARK: controls

    func setRate(_ r: Double) {
        guard device != 0 else { return }
        var a = address(kAudioDevicePropertyNominalSampleRate)
        var v = Float64(r)
        AudioObjectSetPropertyData(device, &a, 0, nil, UInt32(MemoryLayout<Float64>.size), &v)
        rate = r
    }

    private func applyKeepAwake() {
        guard device != 0 else { return }
        setPlist(kPropKeepAwake, NSNumber(value: keepAwake ? 1 : 0), on: device)
    }

    // MARK: MIDI

    private func readMidiIn() {
        let dev = device
        queue.async { [weak self] in
            guard let self, dev != 0, let data = self.getPlist(kPropMidiIn, on: dev) as? Data, !data.isEmpty else { return }
            self.midi.receive([UInt8](data))
        }
    }

    private func sendMidi(_ bytes: [UInt8]) {
        let dev = device
        guard dev != 0, !bytes.isEmpty else { return }
        queue.async { [weak self] in
            self?.setPlist(kPropMidiOut, Data(bytes) as CFData, on: dev)
        }
    }
}
