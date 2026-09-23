import GoogleMaps
import GoogleNavigation
import SwiftUI
import UserNotifications

/// Owns the one navigation-enabled Google map.
///
/// The Navigation SDK's reference says a session works "with or without a view
/// controller", and that is what led to building this headless. In practice
/// every working sample attaches a navigation-enabled GMSMapView, and without
/// one the navigator answers routes with internalError. So there is exactly one
/// map view, it lives here rather than in a SwiftUI view, and it outlives any
/// screen that shows it - otherwise guidance would stop the moment the map
/// scrolled out of view.
final class GoogleMap {
    static let shared = GoogleMap()

    private(set) var mapView: GMSMapView?
    private var navigationReady = false

    private init() {}

    /// Builds the map view if needed. Safe to call repeatedly.
    @discardableResult
    func view() -> GMSMapView? {
        guard GoogleMapsSetup.start() else { return nil }
        if let mapView { return mapView }
        let map = GMSMapView(frame: .zero)
        map.isMyLocationEnabled = true
        map.settings.compassButton = true
        mapView = map
        return map
    }

    /// Navigation can only be switched on once Google's terms are accepted, so
    /// this is separate from building the view.
    func enableNavigation() {
        guard let mapView, !navigationReady else { return }
        mapView.isNavigationEnabled = true
        navigationReady = true

        // Listed as a prerequisite in Google's own sample: the SDK posts
        // guidance notifications while the app is in the background, which is
        // exactly how this gets used.
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert]) { _, _ in }
    }

    var navigator: GMSNavigator? { mapView?.navigator }
}

/// Shows that map view in SwiftUI, without owning it.
struct GoogleMapScreen: UIViewRepresentable {
    func makeUIView(context: Context) -> UIView {
        let container = UIView()
        guard let map = GoogleMap.shared.view() else { return container }
        map.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(map)
        NSLayoutConstraint.activate([
            map.topAnchor.constraint(equalTo: container.topAnchor),
            map.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            map.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            map.trailingAnchor.constraint(equalTo: container.trailingAnchor),
        ])
        return container
    }

    func updateUIView(_ uiView: UIView, context: Context) {}
}
