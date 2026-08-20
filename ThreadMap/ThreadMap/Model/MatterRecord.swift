import Foundation

/// Decoded Matter mDNS advertisement, either operational (`_matter._tcp`) or
/// commissionable (`_matterc._udp`).
struct MatterRecord: Hashable {
    /// Matter's own term for a sleepy device. LIT/SIT only exist for devices
    /// that duty-cycle their radio, which in practice means Thread.
    enum ICDMode: UInt64, Hashable {
        case shortIdle = 0, longIdle = 1
        var label: String { self == .longIdle ? "Long Idle (LIT)" : "Short Idle (SIT)" }
    }

    /// 16 hex chars identifying the fabric the node is commissioned into.
    var compressedFabricID: String?
    /// 16 hex chars — matches `HMAccessory.matterNodeID`, our best join key
    /// between the network view and the HomeKit view.
    var nodeIDHex: String?
    var nodeID: UInt64?

    var vendorID: UInt16?
    var productID: UInt16?
    var discriminator: UInt16?
    var deviceType: UInt32?
    var deviceName: String?
    var commissioningMode: UInt64?
    var pairingHint: UInt64?
    var pairingInstruction: String?

    var sessionIdleIntervalMS: UInt64?
    var sessionActiveIntervalMS: UInt64?
    var sessionActiveThresholdMS: UInt64?
    var icdMode: ICDMode?

    /// A device that duty-cycles its radio is almost certainly battery powered
    /// and on Thread rather than Wi-Fi.
    var looksSleepy: Bool {
        if icdMode != nil { return true }
        if let idle = sessionIdleIntervalMS, idle > 1_000 { return true }
        return false
    }

    init(instanceName: String, txt: [String: Data]) {
        func text(_ key: String) -> String? {
            guard let d = txt[key], !d.isEmpty else { return nil }
            return String(data: d, encoding: .utf8)
        }
        /// Matter encodes every TXT value as ASCII, including the numbers.
        func number(_ key: String) -> UInt64? {
            guard let s = text(key) else { return nil }
            return UInt64(s)
        }

        // Operational instance names are `<fabric>-<node>`; commissionable ones
        // are a random 64-bit value with no embedded identity.
        let parts = instanceName.split(separator: "-")
        if parts.count == 2, parts[0].count == 16, parts[1].count == 16 {
            compressedFabricID = String(parts[0]).uppercased()
            nodeIDHex = String(parts[1]).uppercased()
            nodeID = UInt64(parts[1], radix: 16)
        }

        // `VP` is "vendor+product" in decimal, e.g. `4937+8`.
        if let vp = text("VP") {
            let halves = vp.split(separator: "+")
            if let v = halves.first, let n = UInt16(v) { vendorID = n }
            if halves.count > 1, let n = UInt16(halves[1]) { productID = n }
        }
        discriminator      = number("D").map(UInt16.init(truncatingIfNeeded:))
        deviceType         = number("DT").map(UInt32.init(truncatingIfNeeded:))
        deviceName         = text("DN")
        commissioningMode  = number("CM")
        pairingHint        = number("PH")
        pairingInstruction = text("PI")

        sessionIdleIntervalMS    = number("SII")
        sessionActiveIntervalMS  = number("SAI")
        sessionActiveThresholdMS = number("SAT")
        icdMode = number("ICD").flatMap(ICDMode.init(rawValue:))
    }
}
