import Foundation

/// Asks Google directly, over plain HTTPS, whether this key may compute a route.
///
/// The Navigation SDK reports key, project and billing failures alike as
/// `internalError`, a number that names nothing - which is how this turned into
/// a guessing game. The Routes REST endpoint answers the same question in
/// words: "Billing has not been enabled", "not authorized to use this API",
/// "API key not valid". One request replaces a theory with a quotation.
///
/// It asks twice: once sending the bundle id, the way a key restricted to an
/// iOS app expects, and once without it. The same answer both times means the
/// application restriction is not the thing to change.
enum KeyProbe {
    /// Fixed endpoints, so the probe needs no GPS fix and always costs exactly
    /// one Essentials request. About a kilometre, in Hyderabad.
    private static let body = """
    {"origin":{"location":{"latLng":{"latitude":17.4435,"longitude":78.3772}}},\
    "destination":{"location":{"latLng":{"latitude":17.4517,"longitude":78.3819}}},\
    "travelMode":"DRIVE"}
    """

    static func run(_ done: @escaping (String) -> Void) {
        let key = Settings.shared.apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !key.isEmpty else {
            DispatchQueue.main.async { done("no key saved") }
            return
        }
        let bundleID = Bundle.main.bundleIdentifier ?? ""

        ask(key: key, bundleID: bundleID) { withID in
            ask(key: key, bundleID: nil) { without in
                let line = withID == without
                    ? "\(withID) — same without the bundle id, so the app restriction is not the problem"
                    : "with bundle id: \(withID) / without it: \(without)"
                DispatchQueue.main.async { done(line) }
            }
        }
    }

    private static func ask(key: String, bundleID: String?,
                            _ done: @escaping (String) -> Void) {
        let url = URL(string: "https://routes.googleapis.com/directions/v2:computeRoutes")!
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.timeoutInterval = 15
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue(key, forHTTPHeaderField: "X-Goog-Api-Key")
        request.setValue("routes.duration", forHTTPHeaderField: "X-Goog-FieldMask")
        // The header an iOS-restricted key is matched against.
        if let bundleID { request.setValue(bundleID, forHTTPHeaderField: "X-Ios-Bundle-Identifier") }
        request.httpBody = Data(body.utf8)

        URLSession.shared.dataTask(with: request) { data, response, error in
            if let error {
                done("no answer (\(error.localizedDescription))")
                return
            }
            let code = (response as? HTTPURLResponse)?.statusCode ?? 0
            if code == 200 {
                done("Routes works (HTTP 200)")
                return
            }
            done("HTTP \(code): \(reason(from: data, hiding: key))")
        }.resume()
    }

    /// Google's own words for the refusal, which is the entire point of this.
    /// The key is stripped out first: this line ends up on screen, and in
    /// screenshots, and a key that leaks is a key that has to be replaced.
    private static func reason(from data: Data?, hiding key: String) -> String {
        guard let data,
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let error = root["error"] as? [String: Any] else { return "no detail returned" }
        let status = error["status"] as? String ?? ""
        let message = error["message"] as? String ?? ""
        var line = "\(status) \(message)".trimmingCharacters(in: .whitespaces)
        if !key.isEmpty { line = line.replacingOccurrences(of: key, with: "<key>") }
        return line.count > 200 ? String(line.prefix(200)) + "…" : line
    }
}
