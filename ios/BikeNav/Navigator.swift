import CoreLocation
import MapKit
import SwiftUI

/// Plans the route, follows the rider along it and feeds the display.
final class Navigator: NSObject, ObservableObject {
    enum Phase { case idle, planning, ready, navigating }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var route: MKRoute?
    @Published private(set) var destination: MKMapItem?
    @Published private(set) var guidance: Guidance?
    @Published private(set) var lastLocation: CLLocation?
    /// Non-nil while Google is providing the guidance.
    private var google: GoogleNavSession?
    @Published var message: String?
    @Published var leftHandTraffic: Bool {
        didSet {
            UserDefaults.standard.set(leftHandTraffic, forKey: "leftHandTraffic")
            if let route { rebuild(with: route) }
        }
    }

    let link: BLELink
    private let manager = CLLocationManager()
    private var path: RoutePath?
    private var maneuvers: [Maneuver] = []
    private var matchedSegment = 0
    private var offRouteFixes = 0
    private var lastRerouteAt = Date.distantPast
    private var rerouting = false
    private var arrivedAt: Date?
    private var heartbeat: Timer?
    private var demoTimer: Timer?

    init(link: BLELink) {
        self.link = link
        let saved = UserDefaults.standard.object(forKey: "leftHandTraffic") as? Bool
        self.leftHandTraffic = saved ?? Navigator.regionDrivesOnLeft()
        super.init()

        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyBestForNavigation
        manager.activityType = .automotiveNavigation
        manager.distanceFilter = kCLDistanceFilterNone
        manager.pausesLocationUpdatesAutomatically = false
        manager.requestWhenInUseAuthorization()
        manager.startUpdatingLocation()

        link.onReady = { [weak self] in self?.resendState() }
        heartbeat = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            guard let self, self.phase == .navigating, self.google == nil,
                  let g = self.guidance else { return }
            self.link.send(Packet.navigation(g))
        }
    }

    static func regionDrivesOnLeft() -> Bool {
        let leftHand: Set<String> = ["IN", "GB", "IE", "AU", "NZ", "JP", "ZA", "SG", "MY", "TH", "ID", "PK",
                                     "BD", "LK", "NP", "BT", "MV", "HK", "MO", "KE", "UG", "TZ", "ZW", "ZM",
                                     "MU", "CY", "MT", "JM", "TT", "BS", "BB"]
        return leftHand.contains(Locale.current.region?.identifier ?? "IN")
    }

    // MARK: Planning

    func plan(to item: MKMapItem) {
        stopDemo()
        destination = item
        route = nil
        message = nil
        phase = .planning
        calculate(from: nil)
    }

    func start() {
        guard route != nil else { return }
        phase = .navigating
        arrivedAt = nil
        manager.allowsBackgroundLocationUpdates = true
        manager.showsBackgroundLocationIndicator = true
        link.send(Packet.clock(), force: true)

        // With a key, Google drives the guidance: it matches position to the
        // route and reroutes, which is the part worth not writing by hand. The
        // MapKit path stays as the fallback for when there is no key.
        if Settings.shared.hasKey, let coordinate = destination?.placemark.coordinate {
            let session = GoogleNavSession(link: link, leftHandTraffic: leftHandTraffic)
            session.onGuidance = { [weak self] g in self?.guidance = g }
            session.onStatus = { [weak self] text in self?.message = text }
            google = session
            session.start(to: coordinate, name: destination?.name ?? "Destination")
            return
        }
        if let loc = lastLocation { update(with: loc) }
    }

    func stop() {
        google?.stop()
        google = nil
        phase = .idle
        route = nil
        destination = nil
        guidance = nil
        path = nil
        maneuvers = []
        rerouting = false
        manager.allowsBackgroundLocationUpdates = false
        link.send(Packet.idle, force: true)
    }

    private func calculate(from origin: CLLocationCoordinate2D?) {
        guard let destination else { return }
        let request = MKDirections.Request()
        if let origin {
            request.source = MKMapItem(placemark: MKPlacemark(coordinate: origin))
        } else {
            request.source = MKMapItem.forCurrentLocation()
        }
        request.destination = destination
        request.transportType = .automobile

        MKDirections(request: request).calculate { [weak self] response, error in
            guard let self else { return }
            if let route = response?.routes.first {
                self.rebuild(with: route)
            } else {
                self.message = "No route found" + (error.map { ": \($0.localizedDescription)" } ?? "")
                self.rerouting = false
                if self.phase == .planning { self.phase = .idle }
            }
        }
    }

    private func rebuild(with route: MKRoute) {
        self.route = route
        let built = ManeuverBuilder.build(route: route, leftHandTraffic: leftHandTraffic)
        path = built.0
        maneuvers = built.1
        matchedSegment = 0
        offRouteFixes = 0
        rerouting = false
        if phase == .planning { phase = .ready }
        if phase == .navigating, let loc = lastLocation { update(with: loc) }
    }

    // MARK: Following the route

    private func update(with loc: CLLocation) {
        guard let path, !maneuvers.isEmpty else { return }

        let weakFix = loc.horizontalAccuracy < 0 || loc.horizontalAccuracy > 40
        var projection = path.project(loc.coordinate, from: matchedSegment - 3, window: 150)
        if projection == nil || projection!.offset > 60,
           let global = path.project(loc.coordinate, from: 0, window: path.points.count),
           global.offset < (projection?.offset ?? .infinity) {
            projection = global
        }
        guard let p = projection else { return }

        let tolerance = max(35, min(loc.horizontalAccuracy * 1.5, 80))
        if p.offset > tolerance && !weakFix {
            offRouteFixes += 1
        } else {
            offRouteFixes = 0
            matchedSegment = p.segment
        }
        if offRouteFixes >= 3 { reroute(from: loc.coordinate) }

        let remaining = max(0, path.length - p.along)
        let index = maneuvers.firstIndex { $0.along > p.along + 3 } ?? (maneuvers.count - 1)
        let next = maneuvers[index]
        var then = Dir.none
        if index + 1 < maneuvers.count, maneuvers[index + 1].along - next.along < 400 {
            then = maneuvers[index + 1].direction
        }
        let fraction = path.length > 0 ? remaining / path.length : 0
        let minutes = route.map { Int(($0.expectedTravelTime * fraction / 60).rounded(.up)) }
        let arrived = remaining < 25
        if arrived && arrivedAt == nil { arrivedAt = Date() }

        let g = Guidance(direction: rerouting ? Dir.offRoute : next.direction,
                         distance: max(0, next.along - p.along),
                         street: next.street,
                         thenDirection: then,
                         remaining: remaining,
                         minutesLeft: minutes,
                         speedKmh: loc.speed >= 0 ? Int((loc.speed * 3.6).rounded()) : nil,
                         rerouting: rerouting,
                         arrived: arrived,
                         gpsWeak: weakFix)
        guidance = g
        link.send(Packet.navigation(g))

        if let t = arrivedAt, Date().timeIntervalSince(t) > 20 { stop() }
    }

    private func reroute(from coordinate: CLLocationCoordinate2D) {
        guard !rerouting, Date().timeIntervalSince(lastRerouteAt) > 8 else { return }
        rerouting = true
        lastRerouteAt = Date()
        offRouteFixes = 0
        calculate(from: coordinate)
    }

    private func resendState() {
        link.send(Packet.clock(), force: true)
        if phase == .navigating, let g = guidance {
            link.send(Packet.navigation(g), force: true)
        } else if demoTimer == nil {
            link.send(Packet.idle, force: true)
        }
    }

    // MARK: Display test

    /// Plays a short fake ride on the display, without GPS.
    func runDemo() {
        guard phase != .navigating else { return }
        stopDemo()
        let legs: [(UInt8, Double, String, UInt8)] = [
            (Dir.right, 300, "Old Mumbai Highway", Dir.roundabout(exitAngle: 0, leftHandTraffic: leftHandTraffic)),
            (Dir.roundabout(exitAngle: 0, leftHandTraffic: leftHandTraffic), 200, "Biodiversity Junction", Dir.keepLeft),
            (Dir.keepLeft, 350, "Flyover", Dir.none),
            (Dir.slightRight, 150, "Road No. 36", Dir.left),
            (Dir.left, 250, "Jubilee Hills Road No. 45", Dir.destination),
            (Dir.destination, 100, "Destination", Dir.none),
        ]
        var leg = 0
        var distance = legs[0].1
        link.send(Packet.clock(), force: true)
        demoTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self else { return }
            guard leg < legs.count else {
                self.stopDemo()
                return
            }
            let (dir, _, street, then) = legs[leg]
            let g = Guidance(direction: dir, distance: distance, street: street, thenDirection: then,
                             remaining: nil, minutesLeft: nil, speedKmh: 30)
            self.guidance = g
            self.link.send(Packet.navigation(g), force: true)
            distance -= 15
            if distance < 0 {
                leg += 1
                if leg < legs.count { distance = legs[leg].1 }
            }
        }
    }

    func stopDemo() {
        guard let t = demoTimer else { return }
        t.invalidate()
        demoTimer = nil
        if phase != .navigating {
            guidance = nil
            link.send(Packet.idle, force: true)
        }
    }

    var isDemoRunning: Bool { demoTimer != nil }
}

extension Navigator: CLLocationManagerDelegate {
    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        if google != nil {
            lastLocation = locations.last ?? lastLocation
            return // Google owns the packet stream while it is running
        }
        guard let loc = locations.last else { return }
        lastLocation = loc
        if phase == .navigating { update(with: loc) }
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        switch manager.authorizationStatus {
        case .denied, .restricted:
            message = "Location is off for BikeNav. Turn it on in Settings > Privacy > Location Services."
        case .authorizedWhenInUse, .authorizedAlways:
            manager.startUpdatingLocation()
        default:
            break
        }
    }
}
