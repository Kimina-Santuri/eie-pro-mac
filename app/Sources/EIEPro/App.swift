// EIE Pro: a small menu-bar app for the EIE Pro driver. Status, sample rate, input
// meters, keep-awake, and the MIDI ports (which macOS only lets a user-session app make).

import SwiftUI
import ServiceManagement

@main
struct EIEProApp: App {
    @StateObject private var link = DeviceLink()

    init() {
        // `EIEPro --preview`: the menu's contents in an ordinary window, for screenshots.
        if CommandLine.arguments.contains("--preview") {
            DispatchQueue.main.async { PreviewWindow.show() }
        }
    }

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

private func rateText(_ r: Double) -> String {
    r.truncatingRemainder(dividingBy: 1000) == 0 ? "\(Int(r / 1000)) kHz" : String(format: "%.1f kHz", r / 1000)
}

struct MenuView: View {
    @ObservedObject var link: DeviceLink

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            header

            if link.connected {
                Picker("Sample rate", selection: Binding(get: { link.rate }, set: { link.setRate($0) })) {
                    ForEach(kRates, id: \.self) { Text(rateLabel($0)).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()

                MeterPanel(link: link)

                VStack(alignment: .leading, spacing: 3) {
                    SwitchRow(title: "Keep the box awake", isOn: $link.keepAwake)
                    Text("No tick when sound starts, and MIDI works with nothing playing. Uses a little power.")
                        .font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }

                HStack(spacing: 6) {
                    Image(systemName: "pianokeys").foregroundStyle(.secondary)
                    Text(link.midi.ready ? "MIDI \(link.midiIn) in · \(link.midiOut) out" : "MIDI ports unavailable")
                    Spacer()
                    Text(link.restarts == 0 ? "No dropouts" : "\(link.restarts) recovered")
                        .foregroundStyle(link.restarts == 0 ? Color.secondary : Color.orange)
                }
                .font(.caption)
                .foregroundStyle(.secondary)
            } else {
                Text("Plug in the EIE Pro. If it is plugged in, check that the EIE Pro driver is installed.")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Divider().opacity(0.5)
            SwitchRow(title: "Open at login", isOn: $link.openAtLogin)
            HStack {
                Button("Quit") { NSApplication.shared.terminate(nil) }
                Spacer()
                Button("Uninstall…") { Uninstaller.run() }
                    .foregroundStyle(.secondary)
            }
            .buttonStyle(.borderless)
        }
        .toggleStyle(.switch)
        .controlSize(.small)
        .padding(16)
        .frame(width: 300)
        .background(GlassBackground())
        .clipShape(RoundedRectangle(cornerRadius: GlassBackground.radius, style: .continuous))
        .onAppear { link.menuOpen = true }
        .onDisappear { link.menuOpen = false }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Text("EIE Pro").font(.system(size: 15, weight: .semibold))
            Spacer()
            HStack(spacing: 5) {
                Circle().fill(link.connected ? Meter.mint : Color.secondary.opacity(0.5)).frame(width: 6, height: 6)
                Text(link.connected ? "\(rateText(link.rate))\(link.streaming ? " · live" : "")" : "Not connected")
            }
            .font(.caption.weight(.medium))
            .padding(.horizontal, 8).padding(.vertical, 3)
            .background(Capsule().fill(link.connected ? Meter.mint.opacity(0.14) : Color.primary.opacity(0.06)))
            .foregroundStyle(link.connected ? Color.primary : Color.secondary)
            .help(link.connected ? "Firmware \(link.firmware)" : "")
        }
    }
}

/// Label on the left, small switch on the right.
struct SwitchRow: View {
    let title: String
    @Binding var isOn: Bool
    var body: some View {
        HStack {
            Text(title)
            Spacer()
            Toggle(title, isOn: $isOn).labelsHidden()
        }
    }
}

// MARK: meters

enum Meter {
    static let mint = Color(red: 0.31, green: 0.82, blue: 0.65)
    static let amber = Color(red: 0.96, green: 0.72, blue: 0.26)
    static let coral = Color(red: 1.0, green: 0.42, blue: 0.37)
    static let height: CGFloat = 76
    /// dBFS to 0...1 of the bar: -60 dB at the bottom, 0 at the top.
    static func frac(_ db: Float) -> CGFloat { CGFloat(max(0, min(1, (db + 60) / 60))) }
    static let gradient = LinearGradient(stops: [
        .init(color: mint, location: 0), .init(color: mint, location: 0.68),
        .init(color: amber, location: 0.82), .init(color: coral, location: 0.97),
    ], startPoint: .bottom, endPoint: .top)
}

struct MeterPanel: View {
    @ObservedObject var link: DeviceLink

    var body: some View {
        VStack(spacing: 8) {
            HStack(alignment: .bottom, spacing: 0) {
                Scale()
                MeterGroup(title: "IN", levels: link.levels, holds: link.holds)
                    .frame(maxWidth: .infinity)
                Rectangle().fill(Color.primary.opacity(0.08)).frame(width: 1, height: Meter.height + 30)
                MeterGroup(title: "OUT", levels: link.outLevels, holds: link.outHolds)
                    .frame(maxWidth: .infinity)
            }
            if !link.streaming {
                Text("Nothing is playing through the EIE Pro")
                    .font(.caption2).foregroundStyle(.tertiary)
            }
        }
        .padding(.horizontal, 10).padding(.vertical, 10)
        .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color.primary.opacity(0.045)))
        .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(Color.primary.opacity(0.07)))
    }
}

/// dB marks at the left, lined up with the bars.
struct Scale: View {
    static let marks: [Float] = [0, -12, -24, -48]
    var body: some View {
        ZStack(alignment: .bottomTrailing) {
            ForEach(Self.marks, id: \.self) { m in
                Text(m == 0 ? "0" : "\(Int(m))")
                    .font(.system(size: 8, weight: .medium).monospacedDigit())
                    .foregroundStyle(.tertiary)
                    .offset(y: -(Meter.frac(m) * Meter.height) + 4)
            }
        }
        .frame(width: 18, height: Meter.height, alignment: .bottomTrailing)
        .padding(.bottom, 16)   // the channel numbers under the bars
    }
}

struct MeterGroup: View {
    let title: String
    let levels: [Float]
    let holds: [Float]

    var body: some View {
        VStack(spacing: 5) {
            Text(title).font(.system(size: 9, weight: .semibold)).tracking(1.2).foregroundStyle(.secondary)
            HStack(alignment: .bottom, spacing: 14) {
                ForEach(0..<4, id: \.self) { i in
                    VStack(spacing: 5) {
                        MeterBar(db: levels[i], hold: holds[i])
                        Text("\(i + 1)").font(.system(size: 9, weight: .medium).monospacedDigit()).foregroundStyle(.tertiary)
                    }
                }
            }
        }
    }
}

struct MeterBar: View {
    let db: Float
    let hold: Float

    var body: some View {
        let h = Meter.height
        VStack(spacing: 4) {
            Circle()   // clip light
                .fill(hold > -0.5 ? Meter.coral : Color.primary.opacity(0.08))
                .frame(width: 4, height: 4)
            ZStack(alignment: .bottom) {
                Capsule().fill(Color.primary.opacity(0.07))
                ForEach(Scale.marks.dropFirst(), id: \.self) { m in   // faint scale lines
                    Rectangle().fill(Color.primary.opacity(0.10)).frame(height: 1)
                        .offset(y: -Meter.frac(m) * h)
                }
                Meter.gradient
                    .mask(alignment: .bottom) {
                        Rectangle().frame(height: Meter.frac(db) * h)
                    }
                    .animation(.easeOut(duration: 0.1), value: db)
                if hold > -59 {
                    Rectangle()
                        .fill(hold > -3 ? Meter.coral : hold > -12 ? Meter.amber : Color.primary.opacity(0.55))
                        .frame(height: 1.5)
                        .offset(y: -max(0, Meter.frac(hold) * h - 1.5))
                        .animation(.easeOut(duration: 0.15), value: hold)
                }
            }
            .frame(width: 6, height: h)
            .clipShape(Capsule())
        }
        .help(db <= -60 ? "silent" : String(format: "%.0f dBFS (peak %.0f)", db, hold))
    }
}

// MARK: window look

/// See-through frosted background: blurs what is behind the window, and clears the
/// window's own opaque background so the blur shows.
struct GlassBackground: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView {
        let v = ClearingEffectView()
        v.material = .popover          // follows light / dark mode
        v.blendingMode = .behindWindow
        v.state = .active
        v.alphaValue = 0.55            // 45% see-through (user, 2026-11-09)
        v.maskImage = Self.roundedMask(radius: Self.radius)
        return v
    }
    func updateNSView(_ nsView: NSView, context: Context) {}

    static let radius: CGFloat = 16

    /// Stretchable rounded-rectangle mask: the frosted layer gets rounded corners at any size.
    static func roundedMask(radius r: CGFloat) -> NSImage {
        let side = r * 2 + 1
        let img = NSImage(size: NSSize(width: side, height: side), flipped: false) { rect in
            NSColor.black.setFill()
            NSBezierPath(roundedRect: rect, xRadius: r, yRadius: r).fill()
            return true
        }
        img.capInsets = NSEdgeInsets(top: r, left: r, bottom: r, right: r)
        img.resizingMode = .stretch
        return img
    }

    final class ClearingEffectView: NSVisualEffectView {
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            guard let w = window else { return }
            w.isOpaque = false
            w.backgroundColor = .clear
            w.hasShadow = true
            // Round the whole panel, not just our layer.
            if let cv = w.contentView {
                cv.wantsLayer = true
                cv.layer?.cornerRadius = GlassBackground.radius
                cv.layer?.cornerCurve = .continuous
                cv.layer?.masksToBounds = true
            }
            // The menu-bar panel brings its own frosted layer: make it lighter so ours
            // decides the look.
            if let root = w.contentView?.superview {
                for case let fx as NSVisualEffectView in Self.descendants(of: root) where fx !== self {
                    fx.material = .popover
                    fx.blendingMode = .behindWindow
                    fx.alphaValue = 0.35
                    fx.maskImage = GlassBackground.roundedMask(radius: GlassBackground.radius)
                }
            }
        }
        static func descendants(of v: NSView) -> [NSView] { v.subviews + v.subviews.flatMap { descendants(of: $0) } }
    }
}

enum PreviewWindow {
    static var window: NSWindow?
    static func show() {
        let link = DeviceLink()
        let w = NSWindow(contentRect: NSRect(x: 200, y: 200, width: 300, height: 520),
                         styleMask: [.titled, .closable, .fullSizeContentView], backing: .buffered, defer: false)
        w.titlebarAppearsTransparent = true
        w.title = "EIE Pro preview"
        w.contentView = NSHostingView(rootView: MenuView(link: link))
        w.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        window = w
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
