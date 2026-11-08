// MidiBridge: the EIE Pro's MIDI sockets as CoreMIDI ports.
//   "EIE Pro MIDI In"  - a source: what arrives at the box's MIDI IN socket
//   "EIE Pro MIDI Out" - a destination: what apps send goes out of the box's MIDI OUT
// The driver passes raw bytes; they're cut into whole messages here.

import CoreMIDI
import Foundation

final class MidiBridge {
    var onSend: (([UInt8]) -> Void)?
    private(set) var ready = false
    private var client = MIDIClientRef()
    private var source = MIDIEndpointRef()
    private var dest = MIDIEndpointRef()

    // parser state (touched from one queue only)
    private var status: UInt8 = 0
    private var need = 0
    private var msg: [UInt8] = []
    private var sysex: [UInt8]?

    init() {
        guard MIDIClientCreateWithBlock("EIE Pro" as CFString, &client, nil) == noErr else { return }
        MIDISourceCreate(client, "EIE Pro MIDI In" as CFString, &source)
        MIDIDestinationCreateWithBlock(client, "EIE Pro MIDI Out" as CFString, &dest) { [weak self] list, _ in
            self?.fromApps(list)
        }
        // Same identity every launch, so DJ software remembers its mappings.
        MIDIObjectSetIntegerProperty(source, kMIDIPropertyUniqueID, 0x4549_4531)
        MIDIObjectSetIntegerProperty(dest, kMIDIPropertyUniqueID, 0x4549_4532)
        MIDIObjectSetStringProperty(source, kMIDIPropertyManufacturer, "Akai Professional" as CFString)
        MIDIObjectSetStringProperty(dest, kMIDIPropertyManufacturer, "Akai Professional" as CFString)
        ready = true
    }

    // MARK: apps -> box

    private func fromApps(_ list: UnsafePointer<MIDIPacketList>) {
        var bytes: [UInt8] = []
        let dataOffset = MemoryLayout<MIDIPacket>.offset(of: \MIDIPacket.data)!
        for p in list.unsafeSequence() {
            let n = Int(p.pointee.length)
            let raw = UnsafeRawPointer(p).advanced(by: dataOffset).assumingMemoryBound(to: UInt8.self)
            bytes.append(contentsOf: UnsafeBufferPointer(start: raw, count: n))
        }
        onSend?(bytes)
    }

    // MARK: box -> apps

    func receive(_ bytes: [UInt8]) {
        for b in bytes { parse(b) }
    }

    private static func dataLength(_ s: UInt8) -> Int {
        switch s & 0xF0 {
        case 0xC0, 0xD0: return 1
        case 0xF0:
            switch s {
            case 0xF1, 0xF3: return 1
            case 0xF2: return 2
            default: return 0
            }
        default: return 2
        }
    }

    private func parse(_ b: UInt8) {
        if b >= 0xF8 { emit([b]); return }                 // real-time: alone, any time
        if b == 0xF0 { sysex = [b]; return }
        if sysex != nil {
            if b == 0xF7 { sysex!.append(b); emit(sysex!); sysex = nil; return }
            if b < 0x80 { if sysex!.count < 4096 { sysex!.append(b) }; return }
            emit(sysex!); sysex = nil                       // cut short by a new status byte
        }
        if b >= 0x80 {
            status = b
            need = Self.dataLength(b)
            msg = [b]
            if need == 0 { emit([b]); status = b >= 0xF0 ? 0 : b }
            else if b >= 0xF0 { /* system common: no running status after it */ }
            return
        }
        guard status != 0 else { return }                   // stray data byte
        if msg.isEmpty { msg = [status] }                   // running status
        msg.append(b)
        if msg.count == need + 1 {
            emit(msg)
            msg = []
            if status >= 0xF0 { status = 0 }
        }
    }

    private func emit(_ d: [UInt8]) {
        guard ready, !d.isEmpty else { return }
        let size = MemoryLayout<MIDIPacketList>.size + d.count + 64
        let buf = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 8)
        defer { buf.deallocate() }
        let list = buf.bindMemory(to: MIDIPacketList.self, capacity: 1)
        var pkt = MIDIPacketListInit(list)
        d.withUnsafeBufferPointer { p in
            pkt = MIDIPacketListAdd(list, size, pkt, 0, d.count, p.baseAddress!)
        }
        MIDIReceived(source, list)
    }
}
