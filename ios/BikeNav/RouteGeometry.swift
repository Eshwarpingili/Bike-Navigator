import CoreLocation
import MapKit

enum Geo {
    static let metresPerDegree = 111_320.0

    /// Equirectangular distance: accurate to well under 1% over route-segment lengths.
    static func distance(_ a: CLLocationCoordinate2D, _ b: CLLocationCoordinate2D) -> Double {
        let x = (b.longitude - a.longitude) * cos((a.latitude + b.latitude) / 2 * .pi / 180)
        let y = b.latitude - a.latitude
        return (x * x + y * y).squareRoot() * metresPerDegree
    }

    /// Compass bearing from a to b: 0 = north, 90 = east.
    static func bearing(_ a: CLLocationCoordinate2D, _ b: CLLocationCoordinate2D) -> Double {
        let x = (b.longitude - a.longitude) * cos((a.latitude + b.latitude) / 2 * .pi / 180)
        let y = b.latitude - a.latitude
        return atan2(x, y) * 180 / .pi
    }

    /// Signed difference `to - from` folded into (-180, 180]; positive = clockwise (right).
    static func angleDiff(_ to: Double, _ from: Double) -> Double {
        var d = (to - from).truncatingRemainder(dividingBy: 360)
        if d > 180 { d -= 360 }
        if d <= -180 { d += 360 }
        return d
    }
}

extension MKPolyline {
    var coordinates: [CLLocationCoordinate2D] {
        var coords = [CLLocationCoordinate2D](repeating: kCLLocationCoordinate2DInvalid, count: pointCount)
        getCoordinates(&coords, range: NSRange(location: 0, length: pointCount))
        return coords
    }
}

/// A route as a polyline with cumulative distances, for "how far along am I".
struct RoutePath {
    struct Projection {
        let along: Double   // metres from the route start
        let offset: Double  // metres from the route line
        let segment: Int
    }

    let points: [CLLocationCoordinate2D]
    let cumulative: [Double]

    var length: Double { cumulative.last ?? 0 }

    init(points: [CLLocationCoordinate2D]) {
        self.points = points
        var cum = [Double](repeating: 0, count: points.count)
        if points.count > 1 {
            for i in 1..<points.count {
                cum[i] = cum[i - 1] + Geo.distance(points[i - 1], points[i])
            }
        }
        cumulative = cum
    }

    /// Closest point on segments [from, from + window].
    func project(_ p: CLLocationCoordinate2D, from: Int, window: Int) -> Projection? {
        guard points.count >= 2 else { return nil }
        let lo = max(0, from)
        let hi = min(points.count - 2, from + window)
        guard lo <= hi else { return nil }

        let k = cos(p.latitude * .pi / 180) * Geo.metresPerDegree
        var best: Projection?
        for i in lo...hi {
            let a = points[i], b = points[i + 1]
            let ax = (a.longitude - p.longitude) * k, ay = (a.latitude - p.latitude) * Geo.metresPerDegree
            let bx = (b.longitude - p.longitude) * k, by = (b.latitude - p.latitude) * Geo.metresPerDegree
            let dx = bx - ax, dy = by - ay
            let len2 = dx * dx + dy * dy
            let t = len2 > 0 ? min(max(-(ax * dx + ay * dy) / len2, 0), 1) : 0
            let cx = ax + t * dx, cy = ay + t * dy
            let offset = (cx * cx + cy * cy).squareRoot()
            if best == nil || offset < best!.offset {
                let along = cumulative[i] + t * (cumulative[i + 1] - cumulative[i])
                best = Projection(along: along, offset: offset, segment: i)
            }
        }
        return best
    }

    func coordinate(at along: Double) -> CLLocationCoordinate2D {
        guard let first = points.first, let last = points.last else { return kCLLocationCoordinate2DInvalid }
        if along <= 0 { return first }
        if along >= length { return last }
        var lo = 0, hi = cumulative.count - 1
        while hi - lo > 1 {
            let mid = (lo + hi) / 2
            if cumulative[mid] <= along { lo = mid } else { hi = mid }
        }
        let seg = cumulative[hi] - cumulative[lo]
        let t = seg > 0 ? (along - cumulative[lo]) / seg : 0
        let a = points[lo], b = points[hi]
        return CLLocationCoordinate2D(latitude: a.latitude + (b.latitude - a.latitude) * t,
                                      longitude: a.longitude + (b.longitude - a.longitude) * t)
    }

    /// Heading of the route arriving at `along` (averaged over `span` metres).
    func heading(before along: Double, span: Double = 25) -> Double {
        Geo.bearing(coordinate(at: along - span), coordinate(at: along))
    }

    /// Heading of the route leaving `along`.
    func heading(after along: Double, span: Double = 25) -> Double {
        Geo.bearing(coordinate(at: along), coordinate(at: along + span))
    }
}
