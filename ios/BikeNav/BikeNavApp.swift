import SwiftUI

@main
struct BikeNavApp: App {
    @StateObject private var link: BLELink
    @StateObject private var nav: Navigator

    init() {
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
