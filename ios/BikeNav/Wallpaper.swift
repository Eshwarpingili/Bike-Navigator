import CoreGraphics
import Foundation
import UIKit

/// Turns a photo into the 320x240 background the board draws behind its cards.
///
/// All of it happens here rather than on the board, and deliberately: this end
/// has Core Graphics and a real processor, while the board has a 320 MHz chip
/// with no floating point worth the name and 48 KB of UI heap. It receives
/// finished pixels and blits them.
///
/// The output is raw RGB565, little endian, which is what the panel wants with
/// `LV_COLOR_DEPTH 16` and `LV_COLOR_16_SWAP 0`. Get the byte order backwards
/// and every colour comes out wrong.
enum Wallpaper {
    static let width = 320
    static let height = 240
    static var byteCount: Int { width * height * 2 }

    /// Which face the picture is for. They are toned differently because they
    /// are read in different light: the night one has to sit UNDER a card
    /// without glowing through it, the day one has to stay pale enough that
    /// black text on a white card still wins in direct sun.
    enum Face {
        case night, day

        var targetBrightness: CGFloat { self == .night ? 0.34 : 0.88 }
        var saturation: CGFloat { self == .night ? 1.10 : 0.62 }
    }

    struct Rendered {
        let image: UIImage          // for the preview
        let pixels: Data            // RGB565, ready for the wire
        /// Share of the picture sitting in hues the board reserves for turn
        /// severity. See `severityFraction`.
        let severityFraction: Double
    }

    // MARK: - The pipeline

    static func render(_ source: UIImage, face: Face, gradient: Bool = true) -> Rendered? {
        guard let cropped = centreCrop(source, aspect: CGFloat(width) / CGFloat(height)),
              var rgb = resize(cropped, to: CGSize(width: width, height: height))
        else { return nil }

        tone(&rgb, face: face)
        if gradient { overlayGradient(&rgb, face: face) }
        let fraction = severityFraction(rgb)
        dither(&rgb)

        return Rendered(image: image(from: rgb) ?? source,
                        pixels: packRGB565(rgb),
                        severityFraction: fraction)
    }

    /// A flat buffer of 8-bit RGB, which every step below works on directly.
    /// Easier to reason about than chaining CIFilters, and the dither needs
    /// per-pixel access anyway.
    private struct Buffer {
        var p: [UInt8]              // w * h * 3
        let w: Int, h: Int
        subscript(x: Int, y: Int, c: Int) -> Int {
            get { Int(p[(y * w + x) * 3 + c]) }
            set { p[(y * w + x) * 3 + c] = UInt8(max(0, min(255, newValue))) }
        }
    }

    private static func centreCrop(_ img: UIImage, aspect: CGFloat) -> CGImage? {
        guard let cg = img.cgImage else { return nil }
        let w = CGFloat(cg.width), h = CGFloat(cg.height)
        var rect = CGRect(x: 0, y: 0, width: w, height: h)
        if w / h > aspect {
            let nw = h * aspect
            rect = CGRect(x: (w - nw) / 2, y: 0, width: nw, height: h)
        } else {
            let nh = w / aspect
            rect = CGRect(x: 0, y: (h - nh) / 2, width: w, height: nh)
        }
        return cg.cropping(to: rect)
    }

    private static func resize(_ cg: CGImage, to size: CGSize) -> Buffer? {
        let w = Int(size.width), h = Int(size.height)
        var raw = [UInt8](repeating: 0, count: w * h * 4)
        guard let ctx = CGContext(data: &raw, width: w, height: h,
                                  bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
        else { return nil }
        ctx.interpolationQuality = .high
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))

        var rgb = [UInt8](repeating: 0, count: w * h * 3)
        for i in 0..<(w * h) {
            rgb[i * 3] = raw[i * 4]
            rgb[i * 3 + 1] = raw[i * 4 + 1]
            rgb[i * 3 + 2] = raw[i * 4 + 2]
        }
        return Buffer(p: rgb, w: w, h: h)
    }

    /// Pull the whole picture to one end of the range and take some of the
    /// colour out of it. A photograph at full strength behind a translucent
    /// card is a photograph you are trying to read text off; what is wanted is
    /// a wash that tints the glass.
    private static func tone(_ b: inout Buffer, face: Face) {
        var mean = 0.0
        for i in stride(from: 0, to: b.p.count, by: 3) {
            mean += Double(max(b.p[i], max(b.p[i + 1], b.p[i + 2])))
        }
        mean /= Double(b.w * b.h) * 255.0
        let gain = face.targetBrightness / CGFloat(max(mean, 0.01))
        let sat = face.saturation

        for y in 0..<b.h {
            for x in 0..<b.w {
                let r = Double(b[x, y, 0]), g = Double(b[x, y, 1]), bl = Double(b[x, y, 2])
                let luma = 0.2126 * r + 0.7152 * g + 0.0722 * bl
                for (c, v) in [(0, r), (1, g), (2, bl)] {
                    let desat = luma + (v - luma) * Double(sat)
                    b[x, y, c] = Int((desat * Double(gain)).rounded())
                }
            }
        }
    }

    /// A soft vertical wash, so even a flat photo has somewhere for the light
    /// to come from. Dark at the bottom on the night face and light at the top
    /// on the day one, which is the direction every other surface on the board
    /// is lit from.
    private static func overlayGradient(_ b: inout Buffer, face: Face) {
        for y in 0..<b.h {
            let t = Double(y) / Double(b.h - 1)
            let k: Double = face == .night ? (1.0 - 0.45 * t) : (1.0 - 0.12 * t)
            for x in 0..<b.w {
                for c in 0..<3 { b[x, y, c] = Int(Double(b[x, y, c]) * k) }
            }
        }
    }

    /// Red, amber, green and blue mean exactly one thing on this board: how
    /// hard the next manoeuvre is. A background that wanders into those hues
    /// teaches the rider they can also mean nothing, so the app measures it
    /// and says so rather than quietly shipping a photo that undermines the
    /// one channel that has to stay rigid.
    static func severityFraction(_ b: Buffer) -> Double {
        var total = 0, bad = 0
        for y in stride(from: 0, to: b.h, by: 3) {
            for x in stride(from: 0, to: b.w, by: 3) {
                let r = CGFloat(b[x, y, 0]) / 255, g = CGFloat(b[x, y, 1]) / 255,
                    bl = CGFloat(b[x, y, 2]) / 255
                var hue: CGFloat = 0, sat: CGFloat = 0, bri: CGFloat = 0, a: CGFloat = 0
                UIColor(red: r, green: g, blue: bl, alpha: 1)
                    .getHue(&hue, saturation: &sat, brightness: &bri, alpha: &a)
                total += 1
                if sat > 0.25 && (hue < 0.18 || (hue > 0.25 && hue < 0.47)) { bad += 1 }
            }
        }
        return total == 0 ? 0 : Double(bad) / Double(total)
    }

    /// Floyd-Steinberg down to 5/6/5. Done here, once, so the panel never
    /// shows a band and the board never spends a cycle on it.
    private static func dither(_ b: inout Buffer) {
        let bits = [5, 6, 5]
        var err = [Double](repeating: 0, count: b.w * b.h * 3)
        for y in 0..<b.h {
            for x in 0..<b.w {
                for c in 0..<3 {
                    let i = (y * b.w + x) * 3 + c
                    let v = Double(b[x, y, c]) + err[i]
                    let step = 255.0 / Double((1 << bits[c]) - 1)
                    let q = min(255.0, max(0.0, (v / step).rounded() * step))
                    b[x, y, c] = Int(q)
                    let e = v - q
                    for (dx, dy, f) in [(1, 0, 7.0 / 16), (-1, 1, 3.0 / 16),
                                        (0, 1, 5.0 / 16), (1, 1, 1.0 / 16)] {
                        let nx = x + dx, ny = y + dy
                        guard nx >= 0, nx < b.w, ny >= 0, ny < b.h else { continue }
                        err[(ny * b.w + nx) * 3 + c] += e * f
                    }
                }
            }
        }
    }

    private static func packRGB565(_ b: Buffer) -> Data {
        var out = Data(capacity: b.w * b.h * 2)
        for y in 0..<b.h {
            for x in 0..<b.w {
                let v = UInt16((b[x, y, 0] >> 3) << 11 | (b[x, y, 1] >> 2) << 5 | (b[x, y, 2] >> 3))
                out.append(UInt8(v & 0xFF))
                out.append(UInt8(v >> 8))
            }
        }
        return out
    }

    private static func image(from b: Buffer) -> UIImage? {
        var raw = [UInt8](repeating: 255, count: b.w * b.h * 4)
        for i in 0..<(b.w * b.h) {
            raw[i * 4] = b.p[i * 3]
            raw[i * 4 + 1] = b.p[i * 3 + 1]
            raw[i * 4 + 2] = b.p[i * 3 + 2]
        }
        guard let ctx = CGContext(data: &raw, width: b.w, height: b.h,
                                  bitsPerComponent: 8, bytesPerRow: b.w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue),
              let cg = ctx.makeImage() else { return nil }
        return UIImage(cgImage: cg)
    }
}
