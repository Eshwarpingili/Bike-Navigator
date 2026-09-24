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

            // Measure the approach and the departure over a stretch that fits
            // inside their own steps. A fixed span reaches across a short step
            // into the turn before or after it, and then reports a corner that
            // is partly someone else's.
            let inHeading = path.heading(before: along, span: span(for: steps[i - 1].distance))
            let outHeading = path.heading(after: along, span: span(for: step.distance))
            let turn = Geo.angleDiff(outHeading, inHeading)

            var exitTurn: Double?
            let circular = isRoundabout(text)
            if circular {
                // Where the ring is left, not part way round it. The old probe
                // stopped 40-90 m in, which on a small roundabout is still on
                // the circle - so the arrow pointed along the kerb rather than
                // down the road actually being taken.
                let leaves = min(path.length, along + step.distance)
                let nextLen = i + 1 < steps.count ? steps[i + 1].distance : 60
                exitTurn = Geo.angleDiff(path.heading(after: leaves, span: span(for: nextLen)),
                                         inHeading)
            }
            let code = classify(text, turn: turn, exitTurn: exitTurn, isLast: isLast,
                                leftHandTraffic: leftHandTraffic)

            // A road that changes name without changing direction is a step in
            // MapKit's eyes but not in a rider's. Kept, it becomes the next
            // instruction and hides the actual turn behind it - the board would
            // read "straight on, 200 m" while the junction that matters is the
            // one after. Only dropped when the geometry agrees it is straight.
            if !isLast && code == Dir.straight && abs(turn) < 20 { continue }

            maneuvers.append(Maneuver(along: along, direction: code,
                                      street: isLast ? "Destination" : label(from: text, circular: circular),
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

    /// How far either side of a corner to measure its heading over.
    ///
    /// Half the step, because a heading taken over more than the step it
    /// belongs to is partly measuring the next corner; and never less than
    /// 8 m, because below that the polyline's own rounding is the loudest
    /// thing in the answer.
    static func span(for stepDistance: Double) -> Double {
        guard stepDistance.isFinite, stepDistance > 0 else { return 25 }
        return max(8, min(30, stepDistance * 0.5))
    }

    /// What gets printed under the arrow.
    ///
    /// The board's line is narrow and the tail is what gets the ellipsis, so
    /// the most useful thing goes first. At a roundabout that is the exit
    /// number - "At the roundabout, take the 2nd exit" truncated to "At the
    /// roundabout,..." told the rider nothing they could not already see.
    static func label(from text: String, circular: Bool) -> String {
        let road = roadName(from: text)
        if circular, let n = exitNumber(from: text) {
            return road.map { "\(ordinal(n)) exit, \($0)" } ?? "\(ordinal(n)) exit"
        }
        return road ?? text
    }

    /// "Turn right onto MG Road" -> "MG Road". Nil when the instruction names
    /// no road, so the caller can decide what to say instead of printing a
    /// sentence that will be cut off mid-word.
    static func roadName(from text: String) -> String? {
        for key in [" onto ", " towards ", " toward ", " on "] {
            if let r = text.range(of: key, options: [.caseInsensitive, .backwards]) {
                let s = text[r.upperBound...].trimmingCharacters(in: .whitespaces)
                if !s.isEmpty { return String(s) }
            }
        }
        return nil
    }

    /// The exit to take, from "take the 2nd exit" or "take the second exit".
    /// Anything it does not recognise yields nothing rather than a guess: a
    /// wrong exit number is worse than none at a five-armed roundabout.
    static func exitNumber(from text: String) -> Int? {
        let words = text.lowercased()
            .replacingOccurrences(of: ",", with: " ")
            .split(separator: " ")
            .map(String.init)
        guard let i = words.firstIndex(of: "exit"), i > 0 else { return nil }
        let previous = words[i - 1]
        let spelled = ["first": 1, "second": 2, "third": 3, "fourth": 4,
                       "fifth": 5, "sixth": 6, "seventh": 7, "eighth": 8]
        if let n = spelled[previous] { return n }
        let digits = previous.prefix { $0.isNumber }
        if let n = Int(digits), n >= 1, n <= 20 { return n }
        return nil
    }

    static func ordinal(_ n: Int) -> String {
        if (11...13).contains(n % 100) { return "\(n)th" }
        switch n % 10 {
        case 1: return "\(n)st"
        case 2: return "\(n)nd"
        case 3: return "\(n)rd"
        default: return "\(n)th"
        }
    }
}
