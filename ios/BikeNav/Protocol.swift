import CoreLocation
import Foundation

/// Direction codes shared with the board. See PROTOCOL.md.
enum Dir {
    static let none: UInt8 = 0
    static let start: UInt8 = 1
    static let slightLeft: UInt8 = 2
    static let slightRight: UInt8 = 3
    static let destination: UInt8 = 4
    static let keepLeft: UInt8 = 6
    static let keepRight: UInt8 = 7
    static let left: UInt8 = 8
    static let offRoute: UInt8 = 9
    static let right: UInt8 = 10
    static let sharpLeft: UInt8 = 11
    static let sharpRight: UInt8 = 12
    static let straight: UInt8 = 13
    static let uTurnLeft: UInt8 = 14
    static let uTurnRight: UInt8 = 15
    static let exitLeft: UInt8 = 21
    static let exitRight: UInt8 = 22

    /// Roundabout code for an exit at `exitAngle` degrees relative to the entry
    /// heading (0 = straight across, +90 = right). Left-hand traffic (India) drives
    /// round clockwise, which the board draws differently.
    static func roundabout(exitAngle: Double, leftHandTraffic: Bool) -> UInt8 {
        // Protocol order: SE, E, NE, N, NW, W, SW, S
        let centres: [Double] = [135, 90, 45, 0, -45, -90, -135, 180]
        var best = 0
        var bestDiff = Double.infinity
        for (i, c) in centres.enumerated() {
            let d = abs(Geo.angleDiff(exitAngle, c))
            if d < bestDiff {
                bestDiff = d
                best = i
            }
        }
        return UInt8((leftHandTraffic ? 31 : 23) + best)
    }

    static func isRoundabout(_ code: UInt8) -> Bool { (23...38).contains(code) }

    /// SF Symbol for the in-app card (the board draws its own arrows).
    static func symbol(_ code: UInt8) -> String {
        switch code {
        case slightLeft: return "arrow.up.left"
        case slightRight: return "arrow.up.right"
        case left, sharpLeft: return "arrow.turn.up.left"
        case right, sharpRight: return "arrow.turn.up.right"
        case uTurnLeft: return "arrow.uturn.left"
        case uTurnRight: return "arrow.uturn.right"
        case keepLeft, keepRight, exitLeft, exitRight: return "arrow.triangle.branch"
        case destination: return "mappin.circle.fill"
        case offRoute: return "exclamationmark.triangle.fill"
        case _ where isRoundabout(code): return "arrow.triangle.2.circlepath"
        default: return "arrow.up"
        }
    }
}

/// What the rider needs to know right now; mirrored onto the board.
struct Guidance: Equatable {
    var direction: UInt8
    var distance: Double          // metres to the maneuver
    var street: String
    var thenDirection: UInt8      // the maneuver after, if it comes soon
    var remaining: Double?        // metres to destination
    var minutesLeft: Int?
    var speedKmh: Int?
    var rerouting = false
    var arrived = false
    var gpsWeak = false
}

enum Packet {
    static let idle = Data([0x04])

    static func clock(_ date: Date = Date(), timeZone: TimeZone = .current) -> Data {
        var d = Data([0x03])
        d.appendLE(UInt32(date.timeIntervalSince1970))
        d.appendLE(Int16(timeZone.secondsFromGMT(for: date) / 60))
        return d
    }

    static func navigation(_ g: Guidance) -> Data {
        var flags: UInt8 = 0
        if g.rerouting { flags |= 0x01 }
        if g.arrived { flags |= 0x02 }
        if g.gpsWeak { flags |= 0x04 }

        var d = Data([0x02, g.direction, flags])
        d.appendLE(metres(g.distance))
        d.appendLE(g.remaining.map(metres) ?? UInt32.max)
        d.appendLE(g.minutesLeft.map { UInt16(clamping: $0) } ?? UInt16.max)
        d.append(g.speedKmh.map { UInt8(clamping: min($0, 254)) } ?? 0xFF)
        d.append(0) // speed limit: not available from MapKit
        d.append(g.thenDirection)
        d.append(asciiText(g.street, maxBytes: 48))
        return d
    }

    private static func metres(_ m: Double) -> UInt32 {
        guard m.isFinite else { return 0 }
        return UInt32(min(max(m, 0), 4_000_000_000).rounded())
    }

    /// The board's fonts are ASCII only: transliterate ("Hyderabad" stays, "Café" -> "Cafe",
    /// Devanagari/Telugu -> Latin) and drop anything left over.
    /// Where the rider is and which way they are pointing, so the board can
    /// draw the streets it has stored around them.
    ///
    /// Degrees times 1e7 is about a centimetre - far finer than anything here
    /// needs, and it keeps the whole thing to eleven bytes.
    static func position(_ coordinate: CLLocationCoordinate2D, heading: Double) -> Data {
        var d = Data([0x06])
        d.appendLE(Int32((coordinate.latitude * 1e7).rounded()))
        d.appendLE(Int32((coordinate.longitude * 1e7).rounded()))
        var h = heading.isFinite ? heading.truncatingRemainder(dividingBy: 360) : 0
        if h < 0 { h += 360 }
        d.appendLE(UInt16((h * 10).rounded()))
        return d
    }

    /// The shape of the road ahead, ready to draw.
    ///
    /// The phone does the projection because it is the side with the route, the
    /// heading and a floating point unit; the board only scales what arrives to
    /// pixels. Points are turned so that forward is up and measured from where
    /// the rider is, in whole units of `unit` metres, which is chosen so the
    /// furthest point still fits in a signed byte. That keeps the whole path
    /// inside one 128-byte write.
    static func routeShape(path: RoutePath, along: Double, heading: Double) -> Data? {
        let maxPoints = 60
        let behind = 60.0, ahead = 900.0
        guard path.points.count > 1 else { return nil }
        let from = max(0, along - behind)
        let to = min(path.length, along + ahead)
        guard to - from > 20 else { return nil }

        let origin = path.coordinate(at: along)
        guard CLLocationCoordinate2DIsValid(origin) else { return nil }
        let step = (to - from) / Double(maxPoints - 1)
        let ch = cos(heading * .pi / 180), sh = sin(heading * .pi / 180)

        var xs = [Double](), ys = [Double]()
        xs.reserveCapacity(maxPoints)
        ys.reserveCapacity(maxPoints)
        for i in 0..<maxPoints {
            let c = path.coordinate(at: from + step * Double(i))
            guard CLLocationCoordinate2DIsValid(c) else { continue }
            let midLat = (c.latitude + origin.latitude) / 2 * .pi / 180
            let east = (c.longitude - origin.longitude) * cos(midLat) * Geo.metresPerDegree
            let north = (c.latitude - origin.latitude) * Geo.metresPerDegree
            // Rotate the world so the rider's heading points up the screen.
            xs.append(east * ch - north * sh)
            ys.append(north * ch + east * sh)
        }
        guard xs.count >= 2 else { return nil }

        let furthest = max(xs.map { abs($0) }.max() ?? 0, ys.map { abs($0) }.max() ?? 0)
        let unit = max(1, min(255, Int((furthest / 127).rounded(.up))))
        var d = Data([0x05, UInt8(xs.count), UInt8(unit)])
        for i in 0..<xs.count {
            d.append(UInt8(bitPattern: Int8(clamping: Int((xs[i] / Double(unit)).rounded()))))
            d.append(UInt8(bitPattern: Int8(clamping: Int((ys[i] / Double(unit)).rounded()))))
        }
        return d
    }

    static func asciiText(_ s: String, maxBytes: Int) -> Data {
        let latin = s.applyingTransform(.toLatin, reverse: false) ?? s
        let plain = latin.applyingTransform(.stripDiacritics, reverse: false) ?? latin
        let bytes = plain.unicodeScalars
            .filter { $0.value >= 0x20 && $0.value < 0x7F }
            .map { UInt8($0.value) }
        return Data(bytes.prefix(maxBytes))
    }
}

extension Data {
    mutating func appendLE<T: FixedWidthInteger>(_ value: T) {
        var le = value.littleEndian
        Swift.withUnsafeBytes(of: &le) { append(contentsOf: $0) }
    }
}
