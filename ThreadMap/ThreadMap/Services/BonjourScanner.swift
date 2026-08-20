import Foundation
import Network
import dnssd

/// Browses the local link for the service types that reveal Thread topology,
/// then resolves each instance to addresses and TXT data.
///
/// Everything here runs against the infrastructure link (Wi-Fi/Ethernet). Thread
/// devices show up because border routers run an SRP server plus an advertising
/// proxy: devices register their services with the border router over Thread,
/// and the border router republishes them as mDNS on Wi-Fi. That indirection is
/// exactly why we can see the devices at all — and also why we can't see the
/// mesh links between them.
actor BonjourScanner {

    enum ScanIssue: Equatable, Sendable {
        case localNetworkDenied
        case browserFailed(type: ServiceType, message: String)

        var message: String {
            switch self {
            case .localNetworkDenied:
                "Local Network access is off. Thread and HomeKit discovery need it — turn it on in Settings › Privacy & Security › Local Network."
            case .browserFailed(let type, let message):
                "Couldn't browse \(type.rawValue): \(message)"
            }
        }
    }

    struct ScanResult: Sendable {
        var records: [ServiceRecord] = []
        var issues: [ScanIssue] = []
    }

    private struct Instance: Hashable, Sendable {
        let type: ServiceType
        let name: String
        let domain: String
        let interfaceIndex: UInt32
        var browseTXT: [String: Data]

        // Identity is the service instance itself. TXT contents change while we
        // browse — an accessory bumping its state number, for example — and
        // hashing them would put the same device in the set twice.
        static func == (lhs: Instance, rhs: Instance) -> Bool {
            lhs.type == rhs.type && lhs.name == rhs.name && lhs.domain == rhs.domain
        }

        func hash(into hasher: inout Hasher) {
            hasher.combine(type)
            hasher.combine(name)
            hasher.combine(domain)
        }
    }

    private let resolver = DNSSDResolver()

    /// `kDNSServiceErr_PolicyDenied`. Spelled out so the check works regardless
    /// of which SDK version exports the symbol.
    private static let policyDeniedErrorCode: DNSServiceErrorType = -65570

    /// Runs one full discovery pass.
    /// - Parameter browseDuration: how long to leave the browsers open. mDNS is
    ///   chatty at first then quiets down; 3–4s catches the initial burst plus
    ///   the responses to our own queries.
    func scan(types: [ServiceType] = ServiceType.allCases,
              browseDuration: TimeInterval = 4.0) async -> ScanResult {

        let (instances, issues) = await browse(types: types, duration: browseDuration)
        guard !instances.isEmpty else { return ScanResult(records: [], issues: issues) }

        var records: [ServiceRecord] = []
        await withTaskGroup(of: ServiceRecord?.self) { group in
            for instance in instances {
                group.addTask { [resolver] in
                    let resolution = await resolver.resolve(
                        name: instance.name,
                        type: instance.type.rawValue,
                        domain: instance.domain.isEmpty ? "local." : instance.domain,
                        interfaceIndex: instance.interfaceIndex
                    )
                    // Prefer the resolver's TXT: it preserves the binary values
                    // Thread packs into `xp`, `id`, `sb` and `omr`. Fall back to
                    // whatever the browser gave us if the resolve timed out.
                    let resolvedTXT = resolution?.txt ?? [:]
                    let txt = resolvedTXT.isEmpty ? instance.browseTXT : resolvedTXT
                    return ServiceRecord(
                        type: instance.type,
                        instanceName: instance.name,
                        domain: instance.domain,
                        hostname: resolution?.hostname,
                        port: resolution?.port ?? 0,
                        addresses: resolution?.addresses ?? [],
                        txt: txt,
                        lastSeen: .now
                    )
                }
            }
            for await record in group {
                if let record { records.append(record) }
            }
        }

        records.sort { $0.instanceName.localizedCaseInsensitiveCompare($1.instanceName) == .orderedAscending }
        return ScanResult(records: records, issues: issues)
    }

    // MARK: - Browse

    private func browse(types: [ServiceType], duration: TimeInterval) async -> ([Instance], [ScanIssue]) {
        final class Collector: @unchecked Sendable {
            let lock = NSLock()
            var instances: Set<Instance> = []
            var issues: [ScanIssue] = []

            func add(_ instance: Instance) {
                lock.lock(); defer { lock.unlock() }
                instances.update(with: instance)
            }
            func addIssue(_ issue: ScanIssue) {
                lock.lock(); defer { lock.unlock() }
                if !issues.contains(issue) { issues.append(issue) }
            }
            func snapshot() -> ([Instance], [ScanIssue]) {
                lock.lock(); defer { lock.unlock() }
                return (Array(instances), issues)
            }
        }

        let collector = Collector()
        let queue = DispatchQueue(label: "com.threadmap.browse")
        var browsers: [NWBrowser] = []

        for type in types {
            let parameters = NWParameters()
            parameters.includePeerToPeer = true
            let descriptor = NWBrowser.Descriptor.bonjourWithTXTRecord(type: type.rawValue, domain: nil)
            let browser = NWBrowser(for: descriptor, using: parameters)

            browser.stateUpdateHandler = { state in
                switch state {
                case .failed(let error):
                    collector.addIssue(.browserFailed(type: type, message: error.localizedDescription))
                case .waiting(let error):
                    // -65555 / policy denied is how a missing Local Network
                    // grant surfaces; treat any persistent wait as a hint.
                    if case .dns(let code) = error, code == Self.policyDeniedErrorCode {
                        collector.addIssue(.localNetworkDenied)
                    } else {
                        collector.addIssue(.browserFailed(type: type, message: error.localizedDescription))
                    }
                default:
                    break
                }
            }

            browser.browseResultsChangedHandler = { results, _ in
                for result in results {
                    // We already know the type — this browser only reports one.
                    guard case let .service(name, _, domain, _) = result.endpoint else { continue }
                    var txt: [String: Data] = [:]
                    if case let .bonjour(record) = result.metadata {
                        for (key, entry) in record {
                            switch entry {
                            case .string(let value): txt[key] = Data(value.utf8)
                            case .data(let value):   txt[key] = value
                            default:                 txt[key] = Data()
                            }
                        }
                    }
                    let index = result.interfaces.first?.index ?? 0
                    collector.add(Instance(type: type, name: name, domain: domain,
                                           interfaceIndex: UInt32(index), browseTXT: txt))
                }
            }

            browser.start(queue: queue)
            browsers.append(browser)
        }

        try? await Task.sleep(for: .seconds(duration))
        browsers.forEach { $0.cancel() }
        return collector.snapshot()
    }
}
