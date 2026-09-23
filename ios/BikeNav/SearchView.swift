import MapKit
import SwiftUI

final class SearchModel: NSObject, ObservableObject, MKLocalSearchCompleterDelegate {
    @Published var query = "" {
        didSet { completer.queryFragment = query }
    }
    @Published private(set) var results: [MKLocalSearchCompletion] = []

    private let completer = MKLocalSearchCompleter()

    init(near centre: CLLocationCoordinate2D?) {
        super.init()
        completer.delegate = self
        completer.resultTypes = [.address, .pointOfInterest]
        if let centre {
            completer.region = MKCoordinateRegion(center: centre, latitudinalMeters: 50_000, longitudinalMeters: 50_000)
        }
    }

    func completerDidUpdateResults(_ completer: MKLocalSearchCompleter) {
        results = completer.results
    }

    func completer(_ completer: MKLocalSearchCompleter, didFailWithError error: Error) {
        results = []
    }

    func resolve(_ completion: MKLocalSearchCompletion, _ done: @escaping (MKMapItem?) -> Void) {
        MKLocalSearch(request: MKLocalSearch.Request(completion: completion)).start { response, _ in
            done(response?.mapItems.first)
        }
    }
}

struct SearchView: View {
    @Environment(\.dismiss) private var dismiss
    @StateObject private var model: SearchModel
    private let onPick: (MKMapItem) -> Void

    init(near centre: CLLocationCoordinate2D?, onPick: @escaping (MKMapItem) -> Void) {
        _model = StateObject(wrappedValue: SearchModel(near: centre))
        self.onPick = onPick
    }

    var body: some View {
        NavigationStack {
            List {
                ForEach(Array(model.results.enumerated()), id: \.offset) { _, result in
                    Button {
                        model.resolve(result) { item in
                            guard let item else { return }
                            onPick(item)
                            dismiss()
                        }
                    } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(result.title).foregroundStyle(.primary)
                            if !result.subtitle.isEmpty {
                                Text(result.subtitle).font(.caption).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }
            .searchable(text: $model.query, placement: .navigationBarDrawer(displayMode: .always),
                        prompt: "Search for a place or address")
            .navigationTitle("Where to?")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
    }
}
