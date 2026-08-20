import Foundation

/// Decoded `_meshcop._udp` TXT payload — the Thread border agent advertisement
/// defined by the Thread specification (§ MeshCoP "Border Agent" service).
///
/// Key reference:
///   rv  state version (always "1")     tv  Thread version, e.g. "1.4.0"
///   nn  network name                   xp  extended PAN ID (8 bytes)
///   id  border agent ID (16 bytes)     pt  partition ID (4 bytes)
///   at  active timestamp (8 bytes)     sb  state bitmap (4 bytes)
///   vn  vendor name                    mn  model name
///   dn  Thread domain name             sq  BBR sequence number
///   bb  BBR port                       omr off-mesh-routable prefix
struct MeshcopRecord: Hashable {
    /// Border agent connection mode — how a commissioner may attach.
    enum ConnectionMode: UInt32, Hashable {
        case disallowed = 0, pskc = 1, pskd = 2, vendor = 3, x509 = 4

        var label: String {
            switch self {
            case .disallowed: "Not joinable"
            case .pskc:       "PSKc (commissioner)"
            case .pskd:       "PSKd"
            case .vendor:     "Vendor-specific"
            case .x509:       "X.509"
            }
        }
    }

    /// Whether the border router's own Thread interface is up.
    enum InterfaceStatus: UInt32, Hashable {
        case notInitialized = 0, initializedInactive = 1, active = 2

        var label: String {
            switch self {
            case .notInitialized:      "Not initialized"
            case .initializedInactive: "Configured, not attached"
            case .active:              "Attached"
            }
        }
    }

    /// Whether this border router is willing to take on new work.
    enum Availability: UInt32, Hashable {
        case infrequent = 0, high = 1

        var label: String { self == .high ? "High" : "Infrequent" }
    }

    /// Thread 1.4 added the device's mesh role to the state bitmap.
    enum ThreadRole: UInt32, Hashable {
        case disabledOrDetached = 0, child = 1, router = 2, leader = 3

        var label: String {
            switch self {
            case .disabledOrDetached: "Detached"
            case .child:              "Child"
            case .router:             "Router"
            case .leader:             "Leader"
            }
        }
    }

    var threadVersion: String?
    var networkName: String?
    /// 16 uppercase hex characters.
    var extendedPANID: String?
    /// 32 uppercase hex characters — the join key to `THCredentials.borderAgentID`.
    var borderAgentID: String?
    var partitionID: UInt32?
    var activeTimestamp: UInt64?
    var vendorName: String?
    var modelName: String?
    var domainName: String?
    var bbrSequence: UInt32?
    var bbrPort: UInt16?
    var omrPrefix: IPv6Prefix?

    // Decoded from `sb`.
    var connectionMode: ConnectionMode?
    var interfaceStatus: InterfaceStatus?
    var availability: Availability?
    var isBackboneRouterActive: Bool?
    var isPrimaryBackboneRouter: Bool?
    var threadRole: ThreadRole?
    var supportsEphemeralKey: Bool?
    var rawStateBitmap: UInt32?

    init(txt: [String: Data]) {
        func text(_ key: String) -> String? {
            guard let d = txt[key], !d.isEmpty else { return nil }
            return String(data: d, encoding: .utf8)
        }
        func hex(_ key: String) -> String? {
            guard let d = txt[key], !d.isEmpty else { return nil }
            return d.map { String(format: "%02X", $0) }.joined()
        }
        func be(_ key: String) -> UInt64? {
            guard let d = txt[key], !d.isEmpty, d.count <= 8 else { return nil }
            return d.reduce(UInt64(0)) { ($0 << 8) | UInt64($1) }
        }

        threadVersion   = text("tv")
        networkName     = text("nn")
        extendedPANID   = hex("xp")
        borderAgentID   = hex("id")
        vendorName      = text("vn")
        modelName       = text("mn")
        domainName      = text("dn")
        partitionID     = be("pt").map { UInt32(truncatingIfNeeded: $0) }
        activeTimestamp = be("at")
        bbrSequence     = be("sq").map { UInt32(truncatingIfNeeded: $0) }
        bbrPort         = be("bb").map { UInt16(truncatingIfNeeded: $0) }
        if let omr = txt["omr"] { omrPrefix = IPv6Prefix(meshcopEncoded: omr) }

        if let sb = be("sb").map({ UInt32(truncatingIfNeeded: $0) }) {
            rawStateBitmap          = sb
            connectionMode          = ConnectionMode(rawValue: sb & 0b111)
            interfaceStatus         = InterfaceStatus(rawValue: (sb >> 3) & 0b11)
            availability            = Availability(rawValue: (sb >> 5) & 0b11)
            isBackboneRouterActive  = (sb >> 7) & 1 == 1
            isPrimaryBackboneRouter = (sb >> 8) & 1 == 1
            threadRole              = ThreadRole(rawValue: (sb >> 9) & 0b11)
            supportsEphemeralKey    = (sb >> 11) & 1 == 1
        }
    }
}
