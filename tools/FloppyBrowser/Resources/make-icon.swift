// Draws the app icon (a 3.5" floppy disk) and writes AppIcon.icns.
// Usage: swift make-icon.swift OUTPUT.icns
import AppKit

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.icns"
let iconset = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("AppIcon-\(getpid()).iconset")
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)

func draw(_ px: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                               colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(px) / 1024
    func r(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> NSRect { NSRect(x: x * s, y: y * s, width: w * s, height: h * s) }

    // Body with a cut corner (top right), drawn on the standard icon grid.
    let body = NSBezierPath()
    let L: CGFloat = 132 * s, B: CGFloat = 112 * s, R: CGFloat = 892 * s, T: CGFloat = 892 * s, cut: CGFloat = 70 * s, rad: CGFloat = 40 * s
    body.move(to: NSPoint(x: L + rad, y: B))
    body.line(to: NSPoint(x: R - rad, y: B))
    body.curve(to: NSPoint(x: R, y: B + rad), controlPoint1: NSPoint(x: R, y: B), controlPoint2: NSPoint(x: R, y: B))
    body.line(to: NSPoint(x: R, y: T - cut))
    body.line(to: NSPoint(x: R - cut, y: T))
    body.line(to: NSPoint(x: L + rad, y: T))
    body.curve(to: NSPoint(x: L, y: T - rad), controlPoint1: NSPoint(x: L, y: T), controlPoint2: NSPoint(x: L, y: T))
    body.line(to: NSPoint(x: L, y: B + rad))
    body.curve(to: NSPoint(x: L + rad, y: B), controlPoint1: NSPoint(x: L, y: B), controlPoint2: NSPoint(x: L, y: B))
    body.close()
    let shadow = NSShadow()
    shadow.shadowBlurRadius = 24 * s
    shadow.shadowOffset = NSSize(width: 0, height: -10 * s)
    shadow.shadowColor = NSColor.black.withAlphaComponent(0.35)
    NSGraphicsContext.saveGraphicsState()
    shadow.set()
    NSGradient(starting: NSColor(calibratedRed: 0.30, green: 0.33, blue: 0.40, alpha: 1),
               ending: NSColor(calibratedRed: 0.16, green: 0.18, blue: 0.23, alpha: 1))!.draw(in: body, angle: -90)
    NSGraphicsContext.restoreGraphicsState()

    // Metal shutter with its window.
    let shutter = NSBezierPath(roundedRect: r(300, 600, 400, 292), xRadius: 14 * s, yRadius: 14 * s)
    NSGradient(starting: NSColor(calibratedWhite: 0.93, alpha: 1), ending: NSColor(calibratedWhite: 0.70, alpha: 1))!.draw(in: shutter, angle: -90)
    NSColor(calibratedWhite: 0.25, alpha: 1).setFill()
    NSBezierPath(roundedRect: r(540, 660, 90, 190), xRadius: 10 * s, yRadius: 10 * s).fill()

    // Label.
    let label = NSBezierPath(roundedRect: r(212, 112, 600, 400), xRadius: 22 * s, yRadius: 22 * s)
    NSColor(calibratedWhite: 0.97, alpha: 1).setFill()
    label.fill()
    NSColor(calibratedRed: 0.20, green: 0.45, blue: 0.85, alpha: 1).setFill()
    NSBezierPath(rect: r(212, 440, 600, 34)).fill()
    NSColor(calibratedWhite: 0.80, alpha: 1).setFill()
    for y in [360, 290, 220] as [CGFloat] { NSBezierPath(rect: r(262, y, 500, 8)).fill() }

    // Write-protect hole.
    NSColor(calibratedWhite: 0.08, alpha: 1).setFill()
    NSBezierPath(rect: r(170, 150, 30, 46)).fill()

    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

for (name, px) in [("16x16", 16), ("16x16@2x", 32), ("32x32", 32), ("32x32@2x", 64), ("128x128", 128),
                   ("128x128@2x", 256), ("256x256", 256), ("256x256@2x", 512), ("512x512", 512), ("512x512@2x", 1024)] {
    try draw(px).write(to: iconset.appendingPathComponent("icon_\(name).png"))
}
let p = Process()
p.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
p.arguments = ["-c", "icns", iconset.path, "-o", out]
try p.run()
p.waitUntilExit()
try? FileManager.default.removeItem(at: iconset)
exit(p.terminationStatus)
