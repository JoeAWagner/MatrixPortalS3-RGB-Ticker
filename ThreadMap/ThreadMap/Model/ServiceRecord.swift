import Foundation

/// The Bonjour service types we browse for. Each one has to be listed in
/// `NSBonjourServices` in Info.plist or iOS silently returns nothing.
enum ServiceType: String, CaseIterable, Codable, Sendable {
    /// Thread Border Router MeshCoP border agent. This is the single most
    /// valuable record on the network: it names the Thread network, its
    /// extended PAN ID, the border agent ID, and the router's state.
    case meshcop     = "_meshcop._udp"
    /// Border agent reachable over an encrypted (ePSKc) channel, Thread 1.4+.
    case meshcopE    = "_meshcop-e._tcp"
    /// Thread Radio Encapsulation Link — only border routers advertise it.
    case trel        = "_trel._udp"
    /// Commissioned Matter node. Instance name is `<fabric>-<nodeid>`.
    case matter      = "_matter._tcp"
    /// Matter device in commissioning mode (not yet paired).
    case matterC     = "_matterc._udp"
    /// HomeKit accessory over Thread (HAP-over-CoAP/UDP).
    case hapThread   = "_hap._udp"
    /// HomeKit accessory over Wi-Fi/Ethernet (HAP-over-IP/TCP).
    case hapIP       = "_hap._tcp"

    var displayName: String {
        switch self {
        case .meshcop:   "Thread Border Agent"
        case .meshcopE:  "Border Agent (ePSKc)"
        case .trel:      "Thread Radio Link"
        case .matter:    "Matter node"
        case .matterC:   "Matter (commissionable)"
        case .hapThread: "HomeKit over Thread"
        case .hapIP:     "HomeKit over IP"
        }
    }

    /// Types that indicate the advertiser is itself a border router.
    static var borderRouterTypes: [ServiceType] { [.meshcop, .meshcopE, .trel] }
    /// Types that indicate an end device we want on the map.
    static var deviceTypes: [ServiceType] { [.matter, .matterC, .hapThread, .hapIP] }
}

/// One resolved mDNS service instance, straight off the wire.
struct ServiceRecord: Identifiable, Hashable, Sendable {
    var id: String { "\(type.rawValue)|\(instanceName)" }

    let type: ServiceType
    /// The service instance name, e.g. `Front Door 1A2B` or `8FC7…-0000000012345678`.
    let instanceName: String
    let domain: String
    /// `.local` hostname the SRV record points at.
    var hostname: String?
    var port: UInt16
    var addresses: [IPAddress]
    /// Raw TXT key/value pairs. Values are kept as `Data` because several
    /// Thread keys (`xp`, `id`, `sb`) are binary, not text.
    var txt: [String: Data]
    /// When we last saw an advertisement for this instance.
    var lastSeen: Date

    func txtString(_ key: String) -> String? {
        guard let data = txt[key] else { return nil }
        return String(data: data, encoding: .utf8)
    }

    func txtHex(_ key: String) -> String? {
        guard let data = txt[key] else { return nil }
        return data.map { String(format: "%02X", $0) }.joined()
    }

    func txtUInt(_ key: String) -> UInt64? {
        guard let data = txt[key], !data.isEmpty, data.count <= 8 else { return nil }
        // Thread TXT integers are big-endian binary; Matter's are ASCII decimal.
        if let s = String(data: data, encoding: .utf8), let n = UInt64(s), data.allSatisfy({ (0x30...0x39).contains($0) }) {
            return n
        }
        return data.reduce(UInt64(0)) { ($0 << 8) | UInt64($1) }
    }

    var routableAddresses: [IPAddress] { addresses.filter(\.isRoutable) }
}
