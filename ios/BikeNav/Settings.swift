import Foundation

/// Where the Google API key lives.
///
/// Deliberately not compiled into the app and not in the repository: the key is
/// typed in on the phone and kept in the keychain. That way a copy of the source,
/// or of the .ipa, carries no credential.
final class Settings: ObservableObject {
    static let shared = Settings()

    private static let keyAccount = "google-maps-api-key"

    @Published var apiKey: String {
        didSet { Keychain.set(apiKey, for: Self.keyAccount) }
    }

    /// Motorcycle routing. India's two-wheeler routes use roads and lanes a car
    /// route would not, so this is not merely cosmetic.
    @Published var twoWheeler: Bool {
        didSet { UserDefaults.standard.set(twoWheeler, forKey: "twoWheeler") }
    }

    private init() {
        apiKey = Keychain.get(Self.keyAccount) ?? ""
        twoWheeler = UserDefaults.standard.object(forKey: "twoWheeler") as? Bool ?? true
    }

    var hasKey: Bool { !apiKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
}

enum Keychain {
    static func set(_ value: String, for account: String) {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrAccount as String: account,
        ]
        SecItemDelete(query as CFDictionary)
        guard !value.isEmpty, let data = value.data(using: .utf8) else { return }
        var add = query
        add[kSecValueData as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        SecItemAdd(add as CFDictionary, nil)
    }

    static func get(_ account: String) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }
}
