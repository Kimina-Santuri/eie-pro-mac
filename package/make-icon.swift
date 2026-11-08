// Draws the EIE Pro app icon (our own drawing, in the style of the menu-bar symbol:
// a white disc with waveform bars cut out, on a dark macOS rounded square) and writes
// an .iconset. Run: swift package/make-icon.swift OUT.iconset, then iconutil -c icns.

import AppKit

func render(_ px: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                               colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let ctx = NSGraphicsContext.current!.cgContext
    let s = CGFloat(px) / 1024
    ctx.scaleBy(x: s, y: s)

    // macOS icon grid: 824 x 824 rounded square centred in 1024, corner ~185
    let tile = CGRect(x: 100, y: 100, width: 824, height: 824)
    let tilePath = CGPath(roundedRect: tile, cornerWidth: 185, cornerHeight: 185, transform: nil)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10), blur: 24, color: NSColor.black.withAlphaComponent(0.35).cgColor)
    ctx.addPath(tilePath)
    ctx.setFillColor(NSColor(white: 0.10, alpha: 1).cgColor)
    ctx.fillPath()
    ctx.restoreGState()
    ctx.saveGState()
    ctx.addPath(tilePath)
    ctx.clip()
    let grad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                          colors: [NSColor(white: 0.20, alpha: 1).cgColor, NSColor(white: 0.06, alpha: 1).cgColor] as CFArray,
                          locations: [0, 1])!
    ctx.drawLinearGradient(grad, start: CGPoint(x: 512, y: 924), end: CGPoint(x: 512, y: 100), options: [])
    ctx.restoreGState()

    // white disc
    let r: CGFloat = 300
    let c = CGPoint(x: 512, y: 512)
    ctx.addEllipse(in: CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r))
    ctx.setFillColor(NSColor.white.cgColor)
    ctx.fillPath()

    // waveform bars, cut out of the disc (drawn in the tile's colour)
    let heights: [CGFloat] = [0.22, 0.46, 0.78, 0.56, 0.92, 0.50, 0.70, 0.36, 0.20]
    let barW: CGFloat = 30, gap: CGFloat = 22
    let total = CGFloat(heights.count) * barW + CGFloat(heights.count - 1) * gap
    var x = c.x - total / 2
    ctx.setFillColor(NSColor(white: 0.12, alpha: 1).cgColor)
    for h in heights {
        let bh = h * 360
        ctx.addPath(CGPath(roundedRect: CGRect(x: x, y: c.y - bh / 2, width: barW, height: bh),
                           cornerWidth: barW / 2, cornerHeight: barW / 2, transform: nil))
        ctx.fillPath()
        x += barW + gap
    }
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

let out = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.iconset")
try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
    try! render(base).write(to: out.appendingPathComponent("icon_\(base)x\(base).png"))
    try! render(base * 2).write(to: out.appendingPathComponent("icon_\(base)x\(base)@2x.png"))
}
print("Wrote \(out.path)")
