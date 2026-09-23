import MapKit
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var nav: Navigator
    @EnvironmentObject private var link: BLELink
    @State private var camera: MapCameraPosition = .userLocation(followsHeading: false, fallback: .automatic)
    @State private var showSearch = false
    @State private var showSettings = false

    /// Google's own map once a key is set - it is the same view the navigator
    /// is attached to, so the route and position it draws are the ones guiding
    /// the board. Apple's map is the fallback when there is no key.
    @ViewBuilder private var mapLayer: some View {
        if Settings.shared.hasKey {
            GoogleMapScreen().ignoresSafeArea()
        } else {
            Map(position: $camera) {
                UserAnnotation()
                if let route = nav.route {
                    MapPolyline(route.polyline).stroke(.blue, lineWidth: 6)
                }
                if let destination = nav.destination {
                    Marker(destination.name ?? "Destination", coordinate: destination.placemark.coordinate)
                }
            }
            .mapControls {
                MapUserLocationButton()
                MapCompass()
            }
        }
    }

    var body: some View {
        mapLayer
        .safeAreaInset(edge: .top) {
            HStack {
                LinkBadge(state: link.state)
                Spacer()
                Button {
                    showSettings = true
                } label: {
                    Image(systemName: "gearshape.fill")
                        .font(.title3)
                        .padding(10)
                        .background(.regularMaterial, in: Circle())
                }
            }
            .padding(.horizontal)
        }
        .safeAreaInset(edge: .bottom) {
            BottomPanel(showSearch: $showSearch)
                .padding()
        }
        .sheet(isPresented: $showSearch) {
            SearchView(near: nav.lastLocation?.coordinate) { item in
                nav.plan(to: item)
            }
        }
        .sheet(isPresented: $showSettings) {
            SettingsView()
        }
        .onChange(of: nav.phase) { _, phase in
            if phase == .navigating {
                camera = .userLocation(followsHeading: true, fallback: .automatic)
            }
        }
    }
}

private struct LinkBadge: View {
    let state: BLELink.State

    var body: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(state == .connected ? Color.green : (state == .connecting ? .orange : .red))
                .frame(width: 10, height: 10)
            Text(state.rawValue).font(.subheadline.weight(.medium))
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.regularMaterial, in: Capsule())
    }
}

private struct BottomPanel: View {
    @EnvironmentObject private var nav: Navigator
    @Binding var showSearch: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let message = nav.message {
                Text(message).font(.footnote).foregroundStyle(.red)
            }
            if let debug = nav.debugLine, nav.phase == .navigating {
                Text(debug).font(.caption2.monospaced()).foregroundStyle(.secondary)
            }
            switch nav.phase {
            case .idle:
                if let g = nav.guidance, nav.isDemoRunning {
                    GuidanceCard(guidance: g)
                    Button("Stop test", role: .cancel) { nav.stopDemo() }
                } else {
                    Button {
                        showSearch = true
                    } label: {
                        Label("Where to?", systemImage: "magnifyingglass")
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                }
            case .planning:
                HStack {
                    ProgressView()
                    Text("Finding a route...")
                }
            case .ready:
                if let route = nav.route {
                    Text(nav.destination?.name ?? "Destination").font(.headline)
                    Text("\(formatKm(route.distance)) · \(Int((route.expectedTravelTime / 60).rounded())) min")
                        .foregroundStyle(.secondary)
                }
                HStack {
                    Button("Cancel", role: .cancel) { nav.stop() }
                        .buttonStyle(.bordered)
                    Button {
                        nav.start()
                    } label: {
                        Text("Start").frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                }
                .controlSize(.large)
            case .navigating:
                if let g = nav.guidance {
                    GuidanceCard(guidance: g)
                } else {
                    Text("Waiting for GPS...")
                }
                Button("End route", role: .destructive) { nav.stop() }
                    .buttonStyle(.bordered)
            }
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
    }
}

private struct GuidanceCard: View {
    let guidance: Guidance

    var body: some View {
        HStack(spacing: 16) {
            Image(systemName: Dir.symbol(guidance.direction))
                .font(.system(size: 44, weight: .bold))
                .frame(width: 56)
            VStack(alignment: .leading, spacing: 4) {
                Text(guidance.arrived ? "Arrived" : formatDistance(guidance.distance))
                    .font(.title.bold())
                Text(guidance.rerouting ? "Rerouting..." : guidance.street)
                    .lineLimit(2)
                if let remaining = guidance.remaining, let minutes = guidance.minutesLeft {
                    Text("\(formatKm(remaining)) · \(minutes) min left")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }
}

private struct SettingsView: View {
    @EnvironmentObject private var nav: Navigator
    @EnvironmentObject private var link: BLELink
    @ObservedObject private var settings = Settings.shared
    @Environment(\.dismiss) private var dismiss
    @State private var probe: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    SecureField("Google API key", text: $settings.apiKey)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    Toggle("Motorcycle routing", isOn: $settings.twoWheeler)
                    // The SDK will not say what is wrong with a key, so ask the
                    // REST endpoint, which answers in plain words. Worth having
                    // here rather than only after a failed route: a key can be
                    // checked standing still, before it matters.
                    Button("Check this key with Google") {
                        probe = "checking…"
                        KeyProbe.run { probe = $0 }
                    }
                    .disabled(!settings.hasKey)
                    if let probe {
                        Text(probe)
                            .font(.footnote)
                            .foregroundStyle(probe.hasPrefix("Routes works") ? Color.green : Color.red)
                            .textSelection(.enabled)
                    }
                } header: {
                    Text("Google")
                } footer: {
                    // The SDK takes a key once, at launch, and ignores later
                    // ones - so pasting it here is not enough on its own.
                    Text(settings.hasKey
                         ? "Saved to the keychain. Close and reopen BikeNav for a new key to take effect."
                         : "Needed for navigation. Stored only on this phone, in the keychain.")
                }

                Section("Display") {
                    LabeledContent("Status", value: link.state.rawValue)
                    Button("Test the display") {
                        nav.runDemo()
                        dismiss()
                    }
                    .disabled(nav.phase == .navigating)
                    Button("Forget this display", role: .destructive) { link.forget() }
                }
                Section {
                    Toggle("Traffic drives on the left", isOn: $nav.leftHandTraffic)
                } footer: {
                    Text("On in India, the UK and similar countries. Changes how roundabout arrows are drawn.")
                }
                Section("Tips") {
                    Text("Hold a finger on the display for 2 seconds to turn the picture upside down.")
                    Text("Directions keep working with the phone locked in your pocket. Leave BikeNav open in the background.")
                }
            }
            .navigationTitle("Settings")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}

func formatDistance(_ m: Double) -> String {
    if m < 20 { return "Now" }
    if m < 1000 { return "\(Int((m / 10).rounded()) * 10) m" }
    return formatKm(m)
}

func formatKm(_ m: Double) -> String {
    m < 10_000 ? String(format: "%.1f km", m / 1000) : "\(Int((m / 1000).rounded())) km"
}
