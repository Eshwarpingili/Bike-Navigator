import SwiftUI

@main
struct BikeNavApp: App {
    @StateObject private var link: BLELink
    @StateObject private var nav: Navigator

    init() {
        // Before anything else: the Google SDKs refuse every call until they
        // have the key, and they only take it once per launch.
        GoogleMapsSetup.start()

        let link = BLELink()
        _link = StateObject(wrappedValue: link)
        _nav = StateObject(wrappedValue: Navigator(link: link))
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(nav)
                .environmentObject(link)
        }
    }
}
