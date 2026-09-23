import CoreLocation
import MapKit

struct Maneuver {
    let along: Double      // metres from route start where it happens
    let direction: UInt8
    let street: String
    let instruction: String
}

/// Turns an MKRoute into a polyline plus arrow codes for the board.
///
/// MapKit gives each step as text ("Turn right onto MG Road") but no maneuver type,
/// so the type comes from the text when it is explicit and from the road geometry
/// (heading before vs after the step start) otherwise.
enum ManeuverBuilder {
    static func build(route: MKRoute, leftHandTraffic: Bool) -> (RoutePath, [Maneuver]) {
        var points: [CLLocationCoordinate2D] = []
        var stepStart: [Int] = []
        for step in route.steps {
            var coords = step.polyline.coordinates
            if let last = points.last, let first = coords.first, Geo.distance(last, first) < 1 {
                stepStart.append(points.count - 1)
                coords.removeFirst()
            } else {
                stepStart.append(points.count)
            }
            points.append(contentsOf: coords)
        }
        let path = RoutePath(points: points)
        guard points.count >= 2 else { return (path, []) }

        var maneuvers: [Maneuver] = []
        let steps = route.steps
        for (i, step) in steps.enumerated() where i > 0 {
            let text = step.instructions.trimmingCharacters(in: .whitespacesAndNewlines)
            let isLast = i == steps.count - 1
            if text.isEmpty && !isLast { continue }

            let along = isLast ? path.length : path.cumulative[min(stepStart[i], points.count - 1)]
            let inHeading = path.heading(before: along)
            let turn = Geo.angleDiff(path.heading(after: along), inHeading)

            var exitTurn: Double?
            if isRoundabout(text) {
                // Measure the exit heading once we are clear of the ring.
                let probe = along + min(max(step.distance * 0.6, 40), 90)
                exitTurn = Geo.angleDiff(path.heading(after: probe), inHeading)
            }
            let code = classify(text, turn: turn, exitTurn: exitTurn, isLast: isLast,
                                leftHandTraffic: leftHandTraffic)
            maneuvers.append(Maneuver(along: along, direction: code,
                                      street: isLast ? "Destination" : streetName(from: text),
                                      instruction: text))
        }
        if maneuvers.last?.direction != Dir.destination {
            maneuvers.append(Maneuver(along: path.length, direction: Dir.destination,
                                      street: "Destination", instruction: "Arrive"))
        }
        return (path, maneuvers)
    }

    static func isRoundabout(_ text: String) -> Bool {
        let t = text.lowercased()
        return t.contains("roundabout") || t.contains("rotary") || t.contains("traffic circle")
    }

    static func classify(_ text: String, turn: Double, exitTurn: Double?, isLast: Bool,
                         leftHandTraffic: Bool) -> UInt8 {
        // Only look at the verb part, so "Turn left onto Right Street" stays a left.
        let t = (text.lowercased().components(separatedBy: " onto ").first ?? "")
        if isLast || t.hasPrefix("arrive") || t.contains("destination") { return Dir.destination }
        if let e = exitTurn { return Dir.roundabout(exitAngle: e, leftHandTraffic: leftHandTraffic) }

        let saysLeft = t.contains("left"), saysRight = t.contains("right")
        let onlyLeft = saysLeft && !saysRight, onlyRight = saysRight && !saysLeft

        if t.contains("u-turn") || t.contains("u turn") {
            if onlyLeft { return Dir.uTurnLeft }
            if onlyRight { return Dir.uTurnRight }
            return leftHandTraffic ? Dir.uTurnRight : Dir.uTurnLeft
        }
        if t.contains("keep") || t.contains("bear") || t.contains("fork") {
            if onlyLeft { return Dir.keepLeft }
            if onlyRight { return Dir.keepRight }
        }
        if t.contains("exit") || t.contains("ramp") {
            if onlyLeft { return Dir.exitLeft }
            if onlyRight { return Dir.exitRight }
        }
        if t.contains("sharp") {
            if onlyLeft { return Dir.sharpLeft }
            if onlyRight { return Dir.sharpRight }
        }
        if t.contains("slight") {
            if onlyLeft { return Dir.slightLeft }
            if onlyRight { return Dir.slightRight }
        }
        if t.contains("turn") {
            if onlyLeft { return abs(turn) > 135 ? Dir.sharpLeft : Dir.left }
            if onlyRight { return abs(turn) > 135 ? Dir.sharpRight : Dir.right }
        }

        let a = abs(turn)
        if a < 20 { return Dir.straight }
        if a < 45 { return turn < 0 ? Dir.slightLeft : Dir.slightRight }
        if a < 135 { return turn < 0 ? Dir.left : Dir.right }
        if a < 170 { return turn < 0 ? Dir.sharpLeft : Dir.sharpRight }
        return turn < 0 ? Dir.uTurnLeft : Dir.uTurnRight
    }

    /// "Turn right onto MG Road" -> "MG Road"; falls back to the whole instruction.
    static func streetName(from text: String) -> String {
        for key in [" onto ", " towards ", " toward ", " on "] {
            if let r = text.range(of: key, options: [.caseInsensitive, .backwards]) {
                let s = text[r.upperBound...].trimmingCharacters(in: .whitespaces)
                if !s.isEmpty { return s }
            }
        }
        return text
    }
}
