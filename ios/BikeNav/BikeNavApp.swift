import SwiftUI

@main
struct BikeNavApp: App {
    @StateObject private var link: BLELink
    @StateObject private var nav: Navigator

    init() {
        // Routing is Apple's now: no key, no account, no billing, nothing to
        // set up before the first launch. Drop the key the Google build stored.
        LegacyKeychain.purge()

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
