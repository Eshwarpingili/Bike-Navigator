import SwiftUI

/// The same two palettes the board carries, so the phone in your hand and the
/// display on the bars are visibly one product. `set_palette()` in `ui.c` holds
/// the identical hexes; change one and change the other.
///
/// Light is the daytime face and is built for sunlight: deep inks on a pale
/// ground rather than bright tints, which wash out behind a visor.
enum Palette {
    static let turnHard = adaptive(light: 0xE53935, dark: 0xE53935)
    static let turnCaution = adaptive(light: 0xB45309, dark: 0xFFB300)
    static let turnGentle = adaptive(light: 0x1D4ED8, dark: 0x5AB0FF)
    static let arrival = adaptive(light: 0x047857, dark: 0x3DDC84)

    /// Identity, never severity. Violet is the music surface on both devices,
    /// teal is the map. They sit far from the turn colours on purpose: a hue
    /// that can mean either "where you are" or "brake" means neither.
    static let route = adaptive(light: 0x115E59, dark: 0x2DD4BF)
    static let media = adaptive(light: 0x5B21B6, dark: 0xA78BFA)

    static func adaptive(light: UInt32, dark: UInt32) -> Color {
        Color(UIColor { $0.userInterfaceStyle == .dark ? UIColor(hex: dark) : UIColor(hex: light) })
    }
}

extension Dir.Severity {
    /// Ordinary turns get no colour at all. If every manoeuvre were coloured,
    /// the ones that mean "slow down" would not stand out - which is the only
    /// reason to colour any of them.
    var colour: Color? {
        switch self {
        case .arrival: return Palette.arrival
        case .hard: return Palette.turnHard
        case .caution: return Palette.turnCaution
        case .gentle: return Palette.turnGentle
        case .ordinary: return nil
        }
    }

    /// Spoken by the haptic engine rather than the screen: the rider is looking
    /// at the road, and a wrist-level thump is read without looking down.
    var feedback: SensoryFeedback {
        switch self {
        case .hard: return .warning
        case .arrival: return .success
        default: return .impact(weight: .light)
        }
    }
}

extension Color {
    /// Whatever the severity is, the text on top of it has to stay readable.
    static func turnTint(_ code: UInt8) -> Color {
        Dir.severity(code).colour ?? .primary
    }
}

extension UIColor {
    convenience init(hex: UInt32) {
        self.init(red: CGFloat((hex >> 16) & 0xFF) / 255,
                  green: CGFloat((hex >> 8) & 0xFF) / 255,
                  blue: CGFloat(hex & 0xFF) / 255,
                  alpha: 1)
    }
}

/// The last 400 m as a fraction, matching `NEAR_FROM_M` on the board. Both
/// screens fill the same bar over the same distance, so the shape a rider
/// learns on one is the shape they already know on the other.
func approachFraction(_ metres: Double) -> Double {
    guard metres.isFinite, metres >= 0 else { return 0 }
    return min(1, max(0, 1 - metres / 400))
}

/// An SF Symbol that is certain to draw. A name iOS does not have renders as
/// nothing at all - no compiler error, no CI failure, no warning on the device,
/// just an empty space where the manoeuvre should be. The newer road symbols
/// are exactly the ones at risk, so each is asked for and checked, and anything
/// missing falls back to a symbol that has existed since iOS 13.
enum Symbols {
    static func resolve(_ name: String, fallback: String) -> String {
        UIImage(systemName: name) != nil ? name : fallback
    }

    /// Resolved once; the answer cannot change while the app is running.
    static let flyover = resolve("road.lanes.curved.right", fallback: "arrow.up.right")
    static let underpass = resolve("tunnel", fallback: "arrow.down.right")

    static func manoeuvre(_ code: UInt8) -> String {
        switch code {
        case Dir.flyover: return flyover
        case Dir.underpass: return underpass
        default: return Dir.symbol(code)
        }
    }
}
