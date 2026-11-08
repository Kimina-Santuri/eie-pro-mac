// EIE Pro: a small menu-bar app for the EIE Pro driver. Status, sample rate, input
// meters, keep-awake, and the MIDI ports (which macOS only lets a user-session app make).

import SwiftUI
import ServiceManagement

@main
struct EIEProApp: App {
    @StateObject private var link = DeviceLink()

    var body: some Scene {
        MenuBarExtra {
            MenuView(link: link)
        } label: {
            Image(systemName: link.connected ? "waveform.circle.fill" : "waveform.circle")
        }
        .menuBarExtraStyle(.window)
    }
}

private func rateLabel(_ r: Double) -> String {
    r.truncatingRemainder(dividingBy: 1000) == 0 ? "\(Int(r / 1000))k" : String(format: "%.1fk", r / 1000)
}

struct MenuView: View {
    @ObservedObject var link: DeviceLink

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Circle()
                    .fill(link.connected ? Color.green : Color.secondary.opacity(0.5))
                    .frame(width: 8, height: 8)
                Text("EIE Pro").font(.headline)
                Spacer()
                Text(link.connected ? "Firmware \(link.firmware)" : "Not connected")
                    .font(.caption).foregroundStyle(.secondary)
            }

            if link.connected {
                VStack(alignment: .leading, spacing: 6) {
                    Text("Sample rate").font(.caption).foregroundStyle(.secondary)
                    Picker("Sample rate", selection: Binding(get: { link.rate }, set: { link.setRate($0) })) {
                        ForEach(kRates, id: \.self) { Text(rateLabel($0)).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                }

                VStack(alignment: .leading, spacing: 5) {
                    Text(link.streaming ? "Inputs" : "Inputs (nothing is using the EIE Pro)")
                        .font(.caption).foregroundStyle(.secondary)
                    ForEach(0..<4, id: \.self) { i in
                        MeterRow(label: "\(i + 1)", db: link.levels[i])
                    }
                    Text("Outputs").font(.caption).foregroundStyle(.secondary).padding(.top, 4)
                    ForEach(0..<4, id: \.self) { i in
                        MeterRow(label: "\(i + 1)", db: link.outLevels[i])
                    }
                }

                VStack(alignment: .leading, spacing: 2) {
                    Toggle("Keep the box awake", isOn: $link.keepAwake)
                    Text("No tick when sound starts, and MIDI works with no audio playing. Uses a little power.")
                        .font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }

                VStack(alignment: .leading, spacing: 2) {
                    Text("MIDI: \(link.midiIn) bytes in, \(link.midiOut) out")
                    Text(link.midi.ready ? "Ports: EIE Pro MIDI In / EIE Pro MIDI Out" : "MIDI ports could not be created")
                        .foregroundStyle(.secondary)
                    Text(link.restarts == 0 ? "No dropouts" : "\(link.restarts) recovered dropouts this session")
                        .foregroundStyle(link.restarts == 0 ? Color.secondary : Color.orange)
                }
                .font(.caption)
            } else {
                Text("Plug in the EIE Pro. If it is plugged in, check that the EIE Pro driver is installed.")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Divider()
            Toggle("Open at login", isOn: $link.openAtLogin)
            HStack {
                Button("Quit EIE Pro") { NSApplication.shared.terminate(nil) }
                Spacer()
                Button("Uninstall…") { Uninstaller.run() }
                    .foregroundStyle(.secondary)
            }
        }
        .padding(14)
        .frame(width: 300)
        .onAppear { link.menuOpen = true }
        .onDisappear { link.menuOpen = false }
    }
}

struct MeterRow: View {
    let label: String
    let db: Float

    var body: some View {
        HStack(spacing: 8) {
            Text(label).font(.caption.monospacedDigit()).frame(width: 10)
            GeometryReader { g in
                let frac = CGFloat(max(0, min(1, (db + 60) / 60)))
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 2).fill(Color.secondary.opacity(0.15))
                    RoundedRectangle(cornerRadius: 2)
                        .fill(db > -3 ? Color.red : db > -12 ? Color.yellow : Color.green)
                        .frame(width: g.size.width * frac)
                }
            }
            .frame(height: 8)
            Text(db <= -60 ? "–∞" : String(format: "%.0f", db))
                .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                .frame(width: 30, alignment: .trailing)
        }
    }
}

/// Removes the driver and this app after a confirmation, using the macOS password dialog.
enum Uninstaller {
    static func run() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Uninstall EIE Pro?"
        alert.informativeText = "This removes the EIE Pro driver and this app. Sound stops for a second while macOS restarts its audio. You will be asked for your Mac password."
        alert.addButton(withTitle: "Uninstall")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        try? SMAppService.mainApp.unregister()
        let cmd = "rm -rf /Library/Audio/Plug-Ins/HAL/EIEPro.driver '/Applications/EIE Pro.app'; pkgutil --forget co.ke.kimina.eie-pro >/dev/null 2>&1; killall coreaudiod"
        var err: NSDictionary?
        NSAppleScript(source: "do shell script \"\(cmd)\" with administrator privileges")?.executeAndReturnError(&err)
        if err == nil {
            let done = NSAlert()
            done.messageText = "EIE Pro was removed."
            done.runModal()
            NSApplication.shared.terminate(nil)
        }
    }
}
