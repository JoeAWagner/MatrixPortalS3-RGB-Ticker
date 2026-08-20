import Foundation
import ThreadNetwork

/// Reads the Thread networks this iPhone already knows about.
///
/// `ThreadNetwork.framework` is the only first-party window into Thread that
/// iOS gives third-party apps, and it is deliberately narrow: it returns the
/// *credentials* for networks stored in the keychain — network name, extended
/// PAN ID, channel, PAN ID, border agent ID — and nothing about who is on the
/// network. We use it for two things and no more:
///
///  1. Naming networks that a border router advertises anonymously.
///  2. Telling the user which networks this phone could commission onto.
///
/// The credential blobs themselves (`networkKey`, `PSKC`,
/// `activeOperationalDataSet`) are secrets. They are never read into our model,
/// never logged, and never rendered.
struct ThreadCredentialsService {

    struct KnownNetwork: Identifiable, Hashable {
        var id: String { extendedPANID ?? borderAgentID ?? (networkName ?? "unknown") }
        var networkName: String?
        /// 16 uppercase hex characters.
        var extendedPANID: String?
        /// 32 uppercase hex characters — joins to the MeshCoP `id` TXT key.
        var borderAgentID: String?
        var channel: UInt8?
        var panID: String?
        var creationDate: Date?
        var lastModificationDate: Date?
    }

    enum Failure: LocalizedError {
        case entitlementMissing
        case other(String)

        var errorDescription: String? {
            switch self {
            case .entitlementMissing:
                "This build can't read stored Thread credentials. That needs the com.apple.developer.networking.manage-thread-network-credentials entitlement, which Apple grants on request. Border routers and devices are still mapped without it — networks just show up unnamed."
            case .other(let message):
                message
            }
        }
    }

    private let client = THClient()

    /// All Thread networks stored on this device.
    ///
    /// Requires the managed-credentials entitlement. Without it the call throws
    /// and we degrade to whatever the border routers advertise, which is most
    /// of what the map needs anyway.
    func knownNetworks() async throws -> [KnownNetwork] {
        do {
            let credentials = try await client.retrieveAllActiveCredentials()
            return credentials.map(Self.summarize).sorted {
                ($0.networkName ?? "").localizedCaseInsensitiveCompare($1.networkName ?? "") == .orderedAscending
            }
        } catch {
            throw Self.classify(error)
        }
    }

    /// Asks the user to pick a network via the system sheet. This path works
    /// without the bulk-read entitlement, so it is the fallback we offer when
    /// `knownNetworks()` is refused.
    func preferredNetwork() async throws -> KnownNetwork {
        do {
            return Self.summarize(try await client.retrievePreferredCredentials())
        } catch {
            throw Self.classify(error)
        }
    }

    // MARK: - Mapping

    /// Pulls only the non-secret fields across. Deliberately exhaustive so it is
    /// obvious at review time that no key material escapes this function.
    private static func summarize(_ credentials: THCredentials) -> KnownNetwork {
        KnownNetwork(
            networkName: credentials.networkName,
            extendedPANID: credentials.extendedPANID.map(hex),
            borderAgentID: credentials.borderAgentID.map(hex),
            channel: credentials.channel,
            panID: credentials.panID.map(hex),
            creationDate: credentials.creationDate,
            lastModificationDate: credentials.lastModificationDate
        )
    }

    private static func hex(_ data: Data) -> String {
        data.map { String(format: "%02X", $0) }.joined()
    }

    private static func classify(_ error: Error) -> Failure {
        let nsError = error as NSError
        // A missing entitlement comes back as a generic denial rather than a
        // documented code, so match on what we can and keep the message useful.
        if nsError.domain == NSCocoaErrorDomain || nsError.code == 1 || nsError.code == -1 {
            return .entitlementMissing
        }
        return .other(nsError.localizedDescription)
    }
}
