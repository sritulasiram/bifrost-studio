// Renders the Bifrost Studio icon into an .icns. Usage:
//   swift create_app_icon.swift OUTPUT.icns [PREVIEW_DIR]
// Geometry mirrors icon/bifrost-icon.svg (1024-unit canvas, y grows down);
// every size is drawn from vectors, not scaled from a bitmap.
import AppKit
import Foundation

func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

func render(pixels: Int) -> Data {
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    let context = CGContext(data: nil, width: pixels, height: pixels, bitsPerComponent: 8,
                            bytesPerRow: 0, space: space,
                            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    let scale = CGFloat(pixels) / 1024
    // Flip to the SVG coordinate system (origin top-left, units of 1024).
    context.translateBy(x: 0, y: CGFloat(pixels))
    context.scaleBy(x: scale, y: -scale)
    context.setShouldAntialias(true)

    // Flat night plate.
    context.setFillColor(rgb(0x0E1020))
    context.addPath(CGPath(roundedRect: CGRect(x: 100, y: 100, width: 824, height: 824),
                           cornerWidth: 185, cornerHeight: 185, transform: nil))
    context.fillPath()

    // The gate is drawn at 1.12x about the plate centre (see icon/bifrost-icon.svg).
    context.translateBy(x: 512, y: 512)
    context.scaleBy(x: 1.12, y: 1.12)
    context.translateBy(x: -512, y: -515)

    let gold = rgb(0xE8B64C)
    // In this flipped space an increasing angle is the visual clockwise of the SVG.
    func angle(_ p: CGPoint, from c: CGPoint) -> CGFloat {
        var a = atan2(p.y - c.y, p.x - c.x)
        if a < 0 { a += 2 * .pi }
        return a
    }

    // Gold gate: two 380-radius arcs meeting at a point (lancet), 48 wide.
    let apex = CGPoint(x: 512, y: 231)
    let leftCentre = CGPoint(x: 702, y: 560)   // draws the left side
    let rightCentre = CGPoint(x: 322, y: 560)  // draws the right side
    let gate = CGMutablePath()
    gate.move(to: CGPoint(x: 322, y: 760))
    gate.addLine(to: CGPoint(x: 322, y: 560))
    gate.addArc(center: leftCentre, radius: 380, startAngle: .pi,
                endAngle: angle(apex, from: leftCentre), clockwise: false)
    gate.addArc(center: rightCentre, radius: 380, startAngle: angle(apex, from: rightCentre),
                endAngle: 2 * .pi, clockwise: false)
    gate.addLine(to: CGPoint(x: 702, y: 760))
    context.setStrokeColor(gold)
    context.setLineWidth(48)
    context.setLineJoin(.miter)
    context.setMiterLimit(20)
    context.setLineCap(.butt)
    context.addPath(gate)
    context.strokePath()

    // Two door leaves with the Bifrost spectrum showing between them.
    let leftTop = CGPoint(x: 494, y: 318.4)
    let rightTop = CGPoint(x: 530, y: 318.4)
    let leaves = CGMutablePath()
    leaves.move(to: CGPoint(x: 366, y: 760))
    leaves.addLine(to: CGPoint(x: 366, y: 560))
    leaves.addArc(center: CGPoint(x: 658, y: 560), radius: 292, startAngle: .pi,
                  endAngle: angle(leftTop, from: CGPoint(x: 658, y: 560)), clockwise: false)
    leaves.addLine(to: CGPoint(x: 494, y: 760))
    leaves.closeSubpath()
    leaves.move(to: CGPoint(x: 530, y: 760))
    leaves.addLine(to: rightTop)
    leaves.addArc(center: CGPoint(x: 366, y: 560), radius: 292,
                  startAngle: angle(rightTop, from: CGPoint(x: 366, y: 560)),
                  endAngle: 2 * .pi, clockwise: false)
    leaves.addLine(to: CGPoint(x: 658, y: 760))
    leaves.closeSubpath()
    context.saveGState()
    context.addPath(leaves)
    context.clip()
    let spectrum = CGGradient(colorsSpace: space,
                              colors: [rgb(0x8B7BFF), rgb(0x4FE0C8), rgb(0xFFC24B), rgb(0xFF6B6B)] as CFArray,
                              locations: [0, 0.4, 0.72, 1])!
    context.drawLinearGradient(spectrum, start: CGPoint(x: 0, y: 300), end: CGPoint(x: 0, y: 760),
                               options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
    context.restoreGState()

    // Threshold.
    context.setFillColor(gold)
    context.addPath(CGPath(roundedRect: CGRect(x: 262, y: 784, width: 500, height: 44),
                           cornerWidth: 22, cornerHeight: 22, transform: nil))
    context.fillPath()

    let image = context.makeImage()!
    let rep = NSBitmapImageRep(cgImage: image)
    return rep.representation(using: .png, properties: [:])!
}

let arguments = CommandLine.arguments
guard arguments.count >= 2 else {
    FileHandle.standardError.write("usage: create_app_icon.swift OUTPUT.icns [PREVIEW_DIR]\n".data(using: .utf8)!)
    exit(2)
}
let output = URL(fileURLWithPath: arguments[1])
let iconset = FileManager.default.temporaryDirectory
    .appendingPathComponent("BifrostStudio-\(ProcessInfo.processInfo.processIdentifier).iconset")
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for points in [16, 32, 128, 256, 512] {
    try render(pixels: points).write(to: iconset.appendingPathComponent("icon_\(points)x\(points).png"))
    try render(pixels: points * 2).write(to: iconset.appendingPathComponent("icon_\(points)x\(points)@2x.png"))
}
if arguments.count >= 3 {
    let preview = URL(fileURLWithPath: arguments[2], isDirectory: true)
    try FileManager.default.createDirectory(at: preview, withIntermediateDirectories: true)
    for pixels in [16, 32, 64, 128, 1024] {
        try render(pixels: pixels).write(to: preview.appendingPathComponent("icon-\(pixels).png"))
    }
}
let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", iconset.path, "-o", output.path]
try iconutil.run()
iconutil.waitUntilExit()
try? FileManager.default.removeItem(at: iconset)
exit(iconutil.terminationStatus)
