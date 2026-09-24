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
/// MapKit gives each step as text ("Turn right onto MG Road") but no maneuver
/// type at all, so the type comes from the wording where it is explicit and
/// from the road geometry everywhere else.
///
/// # What the words mean
///
/// A junction turn and a road bend are different events on two wheels. A turn
/// is a decision: slow, look, commit. A bend is not - the road simply curves,
/// and what it asks for is lean and a line. Calling both of them "sharp left"
/// tells the rider nothing about whether to brake, so they are separate here,
/// with separate arrows.
///
/// The severity words are the same everywhere they appear:
///
///     slight   a hint of a change
///     (plain)  an ordinary one
///     sharp    needs real slowing
///     steep    needs a lot of it
///
/// The angles behind those words differ between turns and bends on purpose. A
/// 60 degree junction is ordinary, because you were slowing for the junction
/// anyway; a 60 degree bend taken at speed is not. The word describes what it
/// asks of the rider, not the protractor.
enum ManeuverBuilder {
    /// A bend has to be this far from the junctions either side of it before it
    /// is worth mentioning separately - otherwise it is just the corner the
    /// rider was already told about, announced twice.
    private static let bendClearance = 60.0
    /// And it has to bend at least this much to be worth a card at all.
    private static let bendThreshold = 25.0

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

        func startAlong(_ i: Int) -> Double {
            path.cumulative[min(stepStart[i], points.count - 1)]
        }

        for (i, step) in steps.enumerated() {
            let text = step.instructions.trimmingCharacters(in: .whitespacesAndNewlines)
            let isLast = i == steps.count - 1
            let from = startAlong(i)
            let to = isLast ? path.length : startAlong(i + 1)

            if i > 0 && !(text.isEmpty && !isLast) {
                let along = isLast ? path.length : from

                // Measure the approach and the departure over a stretch that
                // fits inside their own steps. A fixed span reaches across a
                // short step into the turn before or after it, and then reports
                // a corner that is partly someone else's.
                let inHeading = path.heading(before: along, span: span(for: steps[i - 1].distance))
                let outHeading = path.heading(after: along, span: span(for: step.distance))
                let turn = Geo.angleDiff(outHeading, inHeading)

                var exitTurn: Double?
                let circular = isRoundabout(text)
                if circular {
                    // Where the ring is left, not part way round it. The old
                    // probe stopped 40-90 m in, which on a small roundabout is
                    // still on the circle - so the arrow pointed along the kerb
                    // rather than down the road actually being taken.
                    let leaves = min(path.length, along + step.distance)
                    let nextLen = i + 1 < steps.count ? steps[i + 1].distance : 60
                    exitTurn = Geo.angleDiff(path.heading(after: leaves, span: span(for: nextLen)),
                                             inHeading)
                }

                var code = classify(text, turn: turn, exitTurn: exitTurn, isLast: isLast,
                                    leftHandTraffic: leftHandTraffic)
                // Going over or under outranks the shape of the manoeuvre: on a
                // flyover the lane choice is the whole instruction, and the
                // gentle curve of the ramp is beside the point.
                if !isLast, let g = gradeSeparation(text) { code = g }

                // A road that changes name without changing direction is a step
                // in MapKit's eyes but not in a rider's. Kept, it becomes the
                // next instruction and hides the actual turn behind it - the
                // board reading "straight on" while the junction that matters is
                // the one after. Only dropped when the geometry agrees.
                if !(!isLast && code == Dir.straight && abs(turn) < 20) {
                    maneuvers.append(Maneuver(
                        along: along, direction: code,
                        street: isLast ? "Destination"
                                       : describe(code: code, text: text, exitTurn: exitTurn),
                        instruction: text))
                }
            }

            // A curve within the step, which MapKit says nothing about because
            // there is no decision to make. There is still a bike to lean.
            if !isLast, let bend = sharpestBend(in: path, from: from, to: to) {
                let code = Dir.bend(angle: bend.angle)
                maneuvers.append(Maneuver(along: bend.at, direction: code,
                                          street: describe(code: code, text: "", exitTurn: nil),
                                          instruction: "Bend"))
            }
        }

        maneuvers.sort { $0.along < $1.along }
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

    /// Flyover or underpass, from the wording. MapKit exposes no bridge or
    /// tunnel flag, so this is all there is - and only the verb is searched, so
    /// a road *named* Subway Road does not send the rider underground.
    static func gradeSeparation(_ text: String) -> UInt8? {
        let verb = (text.lowercased().components(separatedBy: " onto ").first ?? "")
        if verb.contains("underpass") || verb.contains("tunnel") || verb.contains("subway") {
            return Dir.underpass
        }
        if verb.contains("flyover") || verb.contains("overpass") || verb.contains("over bridge") {
            return Dir.flyover
        }
        return nil
    }

    /// Where a step bends most, and by how much.
    ///
    /// Sampled rather than solved: the heading 20 m before a point against the
    /// heading 20 m after it, stepped along the road. Nil when the step runs
    /// essentially straight, or when the bend sits so close to a junction that
    /// it is the same corner told twice.
    static func sharpestBend(in path: RoutePath, from: Double, to: Double) -> (at: Double, angle: Double)? {
        let first = from + bendClearance
        let last = to - bendClearance
        guard last - first > 20 else { return nil }

        var bestAt = first
        var bestAngle = 0.0
        var d = first
        while d <= last {
            let a = Geo.angleDiff(path.heading(after: d + 20, span: 20),
                                  path.heading(before: d - 20, span: 20))
            if abs(a) > abs(bestAngle) {
                bestAngle = a
                bestAt = d
            }
            d += 20
        }
        guard abs(bestAngle) >= bendThreshold else { return nil }
        return (bestAt, bestAngle)
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

    /// The line under the arrow.
    ///
    /// The board's line is narrow and the tail is what gets the ellipsis, so
    /// the thing the rider cannot work out by looking goes first. At a
    /// roundabout that is the exit number and which way it leaves - the arrow
    /// shows the shape, the words settle the count. At a bend it is how hard.
    /// Everywhere else it is the name of the road being joined, because the
    /// arrow has already said which way.
    static func describe(code: UInt8, text: String, exitTurn: Double?) -> String {
        let road = roadName(from: text)

        if Dir.isRoundabout(code) {
            // Always led by what it is or which exit, never by the side alone:
            // a line that just says "left" at a roundabout is worse than no
            // line, because it reads like an ordinary turn.
            var parts = [exitNumber(from: text).map { "\(ordinal($0)) exit" } ?? "Roundabout"]
            if let side = sideWord(exitTurn) { parts.append(side) }
            let head = parts.joined(separator: " ")
            return road.map { "\(head), \($0)" } ?? head
        }
        if code == Dir.flyover {
            return road.map { "Flyover, \($0)" } ?? "Take the flyover"
        }
        if code == Dir.underpass {
            return road.map { "Underpass, \($0)" } ?? "Take the underpass"
        }
        if let bend = Dir.bendWords(code) {
            return bend
        }
        return road ?? text
    }

    /// Which way a roundabout exit leaves, relative to the way in. Straight
    /// across gets no word: "2nd exit" already says everything, and "ahead"
    /// would only crowd out the road name.
    static func sideWord(_ exitTurn: Double?) -> String? {
        guard let e = exitTurn else { return nil }
        if e <= -35 { return "left" }
        if e >= 35 { return "right" }
        return nil
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
