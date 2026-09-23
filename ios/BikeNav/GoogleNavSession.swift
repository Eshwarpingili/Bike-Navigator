import CoreLocation
import Foundation
import GoogleMaps
import GoogleNavigation

/// Turn-by-turn from Google, forwarded to the bike display.
///
/// Uses a headless navigation session rather than a map view: the board draws
/// the arrow, so there is nothing to render on the phone, and a session without
/// a view keeps working with the phone locked in a pocket.
///
/// Google's SDK owns the hard parts - matching position to the route, deciding
/// when a step is done, and rerouting after a missed turn. This class only
/// translates its output into the packet the firmware expects.
@MainActor
final class GoogleNavSession: NSObject, ObservableObject {
    @Published private(set) var isGuiding = false
    @Published private(set) var status: String?

    private let link: BLELink
    private var session: GMSNavigationSession?
    private var lastSent: Guidance?

    /// India drives on the left, which changes how the board draws roundabouts.
    private let leftHandTraffic: Bool

    init(link: BLELink, leftHandTraffic: Bool = true) {
        self.link = link
        self.leftHandTraffic = leftHandTraffic
        super.init()
    }

    // MARK: - Starting and stopping

    func start(to destination: CLLocationCoordinate2D, name: String) {
        guard GoogleMapsSetup.start() else {
            status = "Add your Google API key in Settings, then restart the app."
            return
        }
        GoogleMapsSetup.acceptTermsIfNeeded { [weak self] accepted in
            guard let self else { return }
            guard accepted else {
                self.status = "Google's terms have to be accepted before navigation can run."
                return
            }
            self.beginSession(to: destination, name: name)
        }
    }

    private func beginSession(to destination: CLLocationCoordinate2D, name: String) {
        guard let session = GMSNavigationServices.createNavigationSession() else {
            status = "Could not start a navigation session."
            return
        }
        self.session = session
        session.isStarted = true
        session.navigator?.add(self)
        session.navigator?.sendsBackgroundNotifications = true

        guard let waypoint = GMSNavigationWaypoint(location: destination, title: name) else {
            status = "That destination could not be used."
            return
        }

        session.navigator?.setDestinations([waypoint]) { [weak self] routeStatus in
            guard let self else { return }
            guard routeStatus == .OK else {
                self.status = Self.describe(routeStatus)
                return
            }
            self.session?.navigator?.isGuidanceActive = true
            self.isGuiding = true
            self.status = nil
        }
    }

    func stop() {
        session?.navigator?.isGuidanceActive = false
        session?.navigator?.clearDestinations()
        session?.isStarted = false
        session = nil
        isGuiding = false
        lastSent = nil
        link.send(Packet.idle, force: true)
    }

    // MARK: - Turning the SDK's view of the world into one packet

    private func publish(_ guidance: Guidance) {
        // The link is slow and the board redraws on change, so only send when
        // something the rider would notice has actually changed.
        if let last = lastSent, last == guidance { return }
        lastSent = guidance
        link.send(Packet.navigation(guidance))
    }

    private static func describe(_ status: GMSRouteStatus) -> String {
        switch status {
        case .OK: return ""
        case .noRouteFound: return "No route to that destination."
        case .networkError: return "No network - a route needs data the first time."
        case .quotaExceeded: return "Google API quota exceeded."
        case .apiKeyNotAuthorized: return "API key rejected. Check the key and its bundle ID restriction."
        case .canceled: return "Route canceled."
        default: return "Could not get a route."
        }
    }
}

// MARK: - Google's maneuvers, in the board's vocabulary

extension GoogleNavSession {
    /// Google names far more maneuvers than the board draws arrows for, so the
    /// unmapped ones fall back to "straight on" rather than showing nothing.
    /// Roundabouts are handled separately: the board picks its arrow from the
    /// exit angle, and drives round the other way in left-hand traffic.
    static func direction(for maneuver: GMSNavigationManeuver,
                          leftHandTraffic: Bool) -> UInt8 {
        switch maneuver {
        case .destination, .destinationLeft, .destinationRight:
            return Dir.destination
        case .depart:
            return Dir.start
        case .turnLeft, .onRampLeft, .offRampLeft:
            return maneuver == .turnLeft ? Dir.left : Dir.exitLeft
        case .turnRight, .onRampRight, .offRampRight:
            return maneuver == .turnRight ? Dir.right : Dir.exitRight
        case .turnSlightLeft, .forkLeft:
            return maneuver == .turnSlightLeft ? Dir.slightLeft : Dir.keepLeft
        case .turnSlightRight, .forkRight:
            return maneuver == .turnSlightRight ? Dir.slightRight : Dir.keepRight
        case .turnSharpLeft:
            return Dir.sharpLeft
        case .turnSharpRight:
            return Dir.sharpRight
        case .turnUTurnClockwise:
            return leftHandTraffic ? Dir.uTurnRight : Dir.uTurnLeft
        case .turnUTurnCounterClockwise:
            return leftHandTraffic ? Dir.uTurnLeft : Dir.uTurnRight
        case .mergeLeft, .mergeUnspecified:
            return Dir.keepLeft
        case .mergeRight:
            return Dir.keepRight
        case .straight, .nameChange:
            return Dir.straight
        default:
            // Includes the roundabout family, which is resolved by exit angle
            // before this is ever consulted.
            return Dir.straight
        }
    }

    static func isRoundabout(_ maneuver: GMSNavigationManeuver) -> Bool {
        switch maneuver {
        case .roundaboutClockwise, .roundaboutCounterClockwise,
             .roundaboutStraightClockwise, .roundaboutStraightCounterClockwise,
             .roundaboutLeftClockwise, .roundaboutLeftCounterClockwise,
             .roundaboutRightClockwise, .roundaboutRightCounterClockwise,
             .roundaboutSharpLeftClockwise, .roundaboutSharpLeftCounterClockwise,
             .roundaboutSharpRightClockwise, .roundaboutSharpRightCounterClockwise,
             .roundaboutSlightLeftClockwise, .roundaboutSlightLeftCounterClockwise,
             .roundaboutSlightRightClockwise, .roundaboutSlightRightCounterClockwise,
             .roundaboutUTurnClockwise, .roundaboutUTurnCounterClockwise,
             .roundaboutExitClockwise, .roundaboutExitCounterClockwise:
            return true
        default:
            return false
        }
    }

    /// Roughly where the exit leaves the circle, relative to the way in.
    /// Google does not give an angle, only a named turn, so this converts the
    /// name back into one the board can draw.
    static func roundaboutExitAngle(_ maneuver: GMSNavigationManeuver) -> Double {
        switch maneuver {
        case .roundaboutSharpLeftClockwise, .roundaboutSharpLeftCounterClockwise: return -135
        case .roundaboutLeftClockwise, .roundaboutLeftCounterClockwise: return -90
        case .roundaboutSlightLeftClockwise, .roundaboutSlightLeftCounterClockwise: return -45
        case .roundaboutSlightRightClockwise, .roundaboutSlightRightCounterClockwise: return 45
        case .roundaboutRightClockwise, .roundaboutRightCounterClockwise: return 90
        case .roundaboutSharpRightClockwise, .roundaboutSharpRightCounterClockwise: return 135
        case .roundaboutUTurnClockwise, .roundaboutUTurnCounterClockwise: return 180
        default: return 0 // straight across
        }
    }
}

// MARK: - GMSNavigatorListener

extension GoogleNavSession: GMSNavigatorListener {
    nonisolated func navigator(_ navigator: GMSNavigator, didArriveAt waypoint: GMSNavigationWaypoint) {
        Task { @MainActor in
            var g = Guidance(direction: Dir.destination, distance: 0, street: waypoint.title,
                             thenDirection: Dir.none)
            g.arrived = true
            self.publish(g)
            self.isGuiding = false
        }
    }

    nonisolated func navigatorDidChangeRoute(_ navigator: GMSNavigator) {
        Task { @MainActor in
            guard var g = self.lastSent else { return }
            g.rerouting = true
            self.publish(g)
        }
    }
}
