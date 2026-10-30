import CoreLocation
import Foundation

/// Direction codes shared with the board. See PROTOCOL.md.
enum Dir {
    static let none: UInt8 = 0
    static let start: UInt8 = 1
    static let slightLeft: UInt8 = 2
    static let slightRight: UInt8 = 3
    static let destination: UInt8 = 4
    static let via: UInt8 = 5
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

    /// A curve in the road rather than a junction. No decision to make, so it
    /// gets its own arrows: a rider needs to know whether the next thing is
    /// "brake and choose" or "lean and hold".
    static let bendLeftSlight: UInt8 = 39
    static let bendLeftSharp: UInt8 = 40
    static let bendLeftSteep: UInt8 = 41
    static let bendRightSlight: UInt8 = 42
    static let bendRightSharp: UInt8 = 43
    static let bendRightSteep: UInt8 = 44
    /// Grade separation: over the junction, or under it.
    static let flyover: UInt8 = 45
    static let underpass: UInt8 = 46

    /// Bends are graded harder than junction turns of the same angle, because
    /// they are taken at speed: 60 degrees at a junction you were slowing for
    /// anyway is ordinary, 60 degrees mid-road is not.
    static func bend(angle: Double) -> UInt8 {
        let left = angle < 0
        switch abs(angle) {
        case ..<45: return left ? bendLeftSlight : bendRightSlight
        case ..<90: return left ? bendLeftSharp : bendRightSharp
        default: return left ? bendLeftSteep : bendRightSteep
        }
    }

    /// The words for a bend code, matching the arrow exactly.
    static func bendWords(_ code: UInt8) -> String? {
        switch code {
        case bendLeftSlight: return "Slight bend left"
        case bendLeftSharp: return "Sharp bend left"
        case bendLeftSteep: return "Steep bend left"
        case bendRightSlight: return "Slight bend right"
        case bendRightSharp: return "Sharp bend right"
        case bendRightSteep: return "Steep bend right"
        default: return nil
        }
    }

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
        case slightLeft, bendLeftSlight: return "arrow.up.left"
        case slightRight, bendRightSlight: return "arrow.up.right"
        case left, sharpLeft: return "arrow.turn.up.left"
        case right, sharpRight: return "arrow.turn.up.right"
        case bendLeftSharp, bendLeftSteep: return "arrow.turn.up.left"
        case bendRightSharp, bendRightSteep: return "arrow.turn.up.right"
        case uTurnLeft: return "arrow.uturn.left"
        case uTurnRight: return "arrow.uturn.right"
        case keepLeft, keepRight, exitLeft, exitRight: return "arrow.triangle.branch"
        case flyover: return "road.lanes.curved.right"
        case underpass: return "tunnel"
        case destination: return "mappin.circle.fill"
        case offRoute: return "exclamationmark.triangle.fill"
        case _ where isRoundabout(code): return "arrow.triangle.2.circlepath"
        default: return "arrow.up"
        }
    }

    /// How hard the manoeuvre is. The board colours its arrow from exactly this
    /// ladder (`direction_colour()` in `ui.c`), and the phone has to agree: a
    /// turn that is red on the handlebars and grey in your hand is worse than
    /// no colour at all, because it teaches the rider the colour means nothing.
    ///
    /// Severity is the *only* thing these colours say. Nothing decorative is
    /// allowed to borrow them.
    enum Severity {
        case arrival    // green
        case hard       // red: sharp, steep, U-turn, off-route
        case caution    // amber: roundabouts, exits, flyovers, underpasses
        case gentle     // blue: slight turns, keeps, gentle bends
        case ordinary   // no colour; most turns are this
    }

    static func severity(_ code: UInt8) -> Severity {
        if isRoundabout(code) { return .caution }
        switch code {
        case destination, via: return .arrival
        case offRoute, sharpLeft, sharpRight, uTurnLeft, uTurnRight,
             bendLeftSteep, bendRightSteep:
            return .hard
        case exitLeft, exitRight, bendLeftSharp, bendRightSharp, flyover, underpass:
            return .caution
        case slightLeft, slightRight, keepLeft, keepRight,
             bendLeftSlight, bendRightSlight:
            return .gentle
        default: return .ordinary
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
        // Matches NAV_STREET_MAX on the board, and 16 + 96 still fits its
        // 128-byte receive buffer. 48 cut the longer instructions mid-word.
        d.append(asciiText(g.street, maxBytes: 96))
        return d
    }

    private static func metres(_ m: Double) -> UInt32 {
        guard m.isFinite else { return 0 }
        return UInt32(min(max(m, 0), 4_000_000_000).rounded())
    }

    // MARK: - Wallpaper (0x07)

    /// How much picture rides in one packet. The board's receive buffer is
    /// 128 bytes and the header takes 6, so 112 leaves room and stays a round
    /// number that divides a 4096-byte sector evenly enough.
    static let wallpaperChunkBytes = 112

    /// 0x07 0x00 | face | sectors(2) | bytes(4)
    /// Announces the transfer so the board can erase its flash sector before
    /// anything arrives, rather than erasing under a write already in flight.
    static func wallpaperBegin(face: Wallpaper.Face, sectors: UInt16, bytes: UInt32) -> Data {
        var d = Data([0x07, 0x00, face == .night ? 0 : 1])
        d.appendLE(sectors)
        d.appendLE(bytes)
        return d
    }

    /// 0x07 0x01 | sector(2) | offset(2) | pixels...
    /// RGB565 little endian, the order the panel wants at LV_COLOR_DEPTH 16
    /// with LV_COLOR_16_SWAP 0.
    static func wallpaperChunk(sector: UInt16, offset: UInt16, bytes: Data) -> Data {
        var d = Data([0x07, 0x01])
        d.appendLE(sector)
        d.appendLE(offset)
        d.append(bytes)
        return d
    }

    /// 0x07 0x02 - every sector is in; commit and start drawing it.
    static func wallpaperEnd() -> Data { Data([0x07, 0x02]) }

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
