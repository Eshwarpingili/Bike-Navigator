import Foundation
import GoogleMaps
import GoogleNavigation

/// Hands the Google SDKs their API key.
///
/// The key is read from the keychain at launch, so it can be entered on the
/// phone rather than compiled in. The SDKs only accept a key once per process,
/// which is why this is guarded: after changing the key in settings the app has
/// to be restarted for it to take effect.
enum GoogleMapsSetup {
    private static var provided = false

    @discardableResult
    static func start() -> Bool {
        guard !provided else { return true }
        let key = Settings.shared.apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !key.isEmpty else { return false }
        provided = GMSServices.provideAPIKey(key)
        return provided
    }

    /// Google requires the rider to accept its terms before navigation can run.
    /// It only asks once, and remembers the answer.
    static func acceptTermsIfNeeded(companyName: String = "BikeNav",
                                    _ done: @escaping (Bool) -> Void) {
        if GMSNavigationServices.areTermsAndConditionsAccepted() {
            done(true)
            return
        }
        GMSNavigationServices.showTermsAndConditionsDialogIfNeeded(withCompanyName: companyName) { accepted in
            done(accepted)
        }
    }

    static var openSourceLicenses: String {
        GMSServices.openSourceLicenseInfo()
    }
}
