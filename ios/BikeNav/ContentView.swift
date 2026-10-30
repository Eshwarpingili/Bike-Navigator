import MapKit
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var nav: Navigator
    @EnvironmentObject private var link: BLELink
    @State private var camera: MapCameraPosition = .userLocation(followsHeading: false, fallback: .automatic)
    @State private var showSearch = false
    @State private var showSettings = false

    private var mapLayer: some View {
        Map(position: $camera) {
            UserAnnotation()
            if let route = nav.route {
                /* Two strokes, not one. A single flat line is the giveaway of a
                 * map nobody styled: the dark casing underneath is what keeps
                 * the route legible when it crosses a motorway of nearly the
                 * same colour, which is most of the time in a city. */
                MapPolyline(route.polyline)
                    .stroke(.black.opacity(0.35), style: StrokeStyle(lineWidth: 11,
                                                                     lineCap: .round,
                                                                     lineJoin: .round))
                MapPolyline(route.polyline)
                    .stroke(Palette.route, style: StrokeStyle(lineWidth: 6,
                                                              lineCap: .round,
                                                              lineJoin: .round))
            }
            if let destination = nav.destination {
                Marker(destination.name ?? "Destination", coordinate: destination.placemark.coordinate)
                    .tint(Palette.route)
            }
        }
        .mapControls {
            MapUserLocationButton()
            MapCompass()
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

    /// Rerouting outranks the manoeuvre: the arrow on screen is about to be
    /// replaced, so it should not be claiming a confident colour meanwhile.
    private var tint: Color? {
        guidance.rerouting ? Palette.turnHard : Dir.severity(guidance.direction).colour
    }

    /// The two moments the board pulses at. Crossing either one is worth a
    /// haptic, because it is exactly when a rider stops reading and starts
    /// positioning. Integer so it fires once per crossing, not per GPS fix.
    private var approachStep: Int {
        guidance.arrived ? 3 : (guidance.distance <= 50 ? 2 : (guidance.distance <= 200 ? 1 : 0))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 16) {
                Image(systemName: Dir.symbol(guidance.direction))
                    .font(.system(size: 38, weight: .bold))
                    .foregroundStyle(tint ?? .primary)
                    .frame(width: 56, height: 56)
                    .background((tint ?? .secondary).opacity(0.14), in: RoundedRectangle(cornerRadius: 14))
                    /* The arrow itself moves when the instruction changes. The
                     * board does the same thing with a scale pulse; this is the
                     * phone's version of the same sentence. */
                    .symbolEffect(.bounce, value: guidance.direction)
                    .animation(.snappy(duration: 0.25), value: guidance.direction)

                VStack(alignment: .leading, spacing: 4) {
                    Text(guidance.arrived ? "Arrived" : formatDistance(guidance.distance))
                        .font(.title.bold())
                        .foregroundStyle(tint ?? .primary)
                        /* Counts down digit by digit instead of snapping, so a
                         * glance catches that it is falling, not just what it
                         * says. */
                        .contentTransition(.numericText(countsDown: true))
                        .animation(.snappy, value: guidance.distance)
                    Text(guidance.rerouting ? "Rerouting\u{2026}" : guidance.street)
                        .lineLimit(2)
                    if let remaining = guidance.remaining, let minutes = guidance.minutesLeft {
                        Text("\(formatKm(remaining)) · \(minutes) min left")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }
                Spacer(minLength: 0)
            }

            /* The same bar as the board's, over the same last 400 m. Hidden
             * until there is something to brace for: a sliver of amber with a
             * kilometre still to run is a warning about nothing. */
            if !guidance.arrived, guidance.distance < 400 {
                GeometryReader { geo in
                    ZStack(alignment: .leading) {
                        Capsule().fill(.quaternary)
                        Capsule()
                            .fill(tint ?? Color.primary)
                            .frame(width: geo.size.width * approachFraction(guidance.distance))
                    }
                }
                .frame(height: 4)
                .animation(.linear(duration: 0.4), value: guidance.distance)
            }
        }
        .sensoryFeedback(trigger: approachStep) { _, step in
            step == 0 ? nil : Dir.severity(guidance.direction).feedback
        }
    }
}

private struct SettingsView: View {
    @EnvironmentObject private var nav: Navigator
    @EnvironmentObject private var link: BLELink
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
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
