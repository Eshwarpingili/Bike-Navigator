import Foundation

/// Clears out what the Google build left behind.
///
/// Earlier versions kept a Google Maps API key in the keychain. Those versions
/// are gone and nothing reads that key any more, but it would otherwise sit on
/// the phone indefinitely - a live credential with no owner. Deleting it is the
/// last step of removing Google, and it costs nothing to run at every launch.
enum LegacyKeychain {
    private static let googleKeyAccount = "google-maps-api-key"

    static func purge() {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrAccount as String: googleKeyAccount,
        ]
        SecItemDelete(query as CFDictionary)
    }
}
