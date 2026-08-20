import Foundation

/// A Thread network, identified by its extended PAN ID. Everything else —
/// border routers, devices, credentials — hangs off one of these.
struct ThreadNetwork: Identifiable, Hashable {
    /// Extended PAN ID as 16 uppercase hex characters, or a synthetic ID when
    /// we know a network exists but never learned its XPAN.
    let id: String
    var name: String?
    var extendedPANID: String?
    var channel: UInt8?
    var panID: String?
    var threadVersion: String?
    /// True when the ThreadNetwork framework handed us stored credentials for
    /// this network (meaning this iPhone can commission onto it).
    var hasStoredCredentials: Bool = false
    var borderRouterIDs: [String] = []
    var deviceIDs: [String] = []
    /// OMR prefixes advertised by this network's border routers.
    var omrPrefixes: [IPv6Prefix] = []

    var isSynthetic: Bool { extendedPANID == nil }

    var displayName: String {
        if let name, !name.isEmpty { return name }
        if let xp = extendedPANID { return "Thread network \(xp.prefix(4))…\(xp.suffix(4))" }
        return "Unidentified Thread network"
    }
}

/// A Thread Border Router: an Apple TV, HomePod, Nest/Echo hub, eero, or an
/// OpenThread box. Discovered purely from its own MeshCoP advertisement.
struct BorderRouter: Identifiable, Hashable {
    /// Border agent ID when present, otherwise the service instance name.
    let id: String
    var displayName: String
    var vendorName: String?
    var modelName: String?
    var borderAgentID: String?
    var networkID: String?
    var addresses: [IPAddress] = []
    var hostname: String?
    var meshcop: MeshcopRecord?
    var advertisedTypes: Set<ServiceType> = []
    var lastSeen: Date = .now
    /// The HomeKit accessory we believe this hardware corresponds to, if any.
    var homeKitAccessoryID: UUID?

    var isLeader: Bool { meshcop?.threadRole == .leader }
    var isPrimaryBackboneRouter: Bool { meshcop?.isPrimaryBackboneRouter == true }
    var isAttached: Bool { meshcop?.interfaceStatus == .active }

    /// Vendor + model, e.g. "Apple Apple TV". Falls back to the instance name.
    var hardwareDescription: String {
        let parts = [vendorName, modelName].compactMap { $0 }.filter { !$0.isEmpty }
        return parts.isEmpty ? displayName : parts.joined(separator: " ")
    }
}

/// Anything on the map that isn't a border router: a bulb, lock, sensor, plug.
struct MeshDevice: Identifiable, Hashable {
    enum Transport: Hashable {
        case thread, wifiOrEthernet, unknown

        var label: String {
            switch self {
            case .thread:         "Thread"
            case .wifiOrEthernet: "Wi-Fi / Ethernet"
            case .unknown:        "Unknown"
            }
        }
    }

    let id: String
    var displayName: String
    var transport: Attributed<Transport>
    /// Which Thread network we believe it sits on, and why.
    var networkID: Attributed<String?>
    var protocols: Set<ServiceType> = []
    var addresses: [IPAddress] = []
    var hostname: String?
    var matter: MatterRecord?
    var hap: HAPRecord?
    var homeKitAccessoryID: UUID?
    var lastSeen: Date = .now

    var isSleepy: Bool { matter?.looksSleepy ?? false }
    var isOnThread: Bool { transport.value == .thread }

    var kindLabel: String {
        if let hap, hap.categoryIdentifier != nil { return hap.categoryName }
        if let name = matter?.deviceName, !name.isEmpty { return name }
        return "Device"
    }
}

/// A HomeKit accessory as HomeKit itself sees it. Kept separate from
/// `MeshDevice` because HomeKit knows names and rooms but nothing about radios,
/// while mDNS knows radios but not names — the join between them is the app's
/// main job.
struct HomeKitAccessory: Identifiable, Hashable {
    let id: UUID
    var name: String
    var homeName: String
    var roomName: String?
    var manufacturer: String?
    var model: String?
    var firmwareVersion: String?
    var category: String
    var isReachable: Bool
    var isBridged: Bool
    var bridgeName: String?
    /// Present only for Matter accessories; 16 hex chars, uppercase.
    var matterNodeIDHex: String?
    var serviceCount: Int = 0
}

/// The whole picture, rebuilt from scratch after every scan pass.
struct Topology: Hashable {
    var networks: [ThreadNetwork] = []
    var borderRouters: [BorderRouter] = []
    var devices: [MeshDevice] = []
    var accessories: [HomeKitAccessory] = []
    /// Service records we saw but could not place, kept for the raw view.
    var unmatchedRecords: [ServiceRecord] = []
    var generatedAt: Date = .now

    func network(_ id: String?) -> ThreadNetwork? {
        guard let id else { return nil }
        return networks.first { $0.id == id }
    }

    func borderRouters(on networkID: String) -> [BorderRouter] {
        borderRouters.filter { $0.networkID == networkID }
    }

    func devices(on networkID: String) -> [MeshDevice] {
        devices.filter { $0.networkID.value == networkID }
    }

    var threadDevices: [MeshDevice] { devices.filter(\.isOnThread) }
    var offMeshDevices: [MeshDevice] { devices.filter { !$0.isOnThread } }

    func accessory(_ id: UUID?) -> HomeKitAccessory? {
        guard let id else { return nil }
        return accessories.first { $0.id == id }
    }

    var isEmpty: Bool {
        networks.isEmpty && borderRouters.isEmpty && devices.isEmpty && accessories.isEmpty
    }
}
