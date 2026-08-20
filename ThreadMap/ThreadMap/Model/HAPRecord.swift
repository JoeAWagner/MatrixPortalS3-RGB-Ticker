import Foundation

/// Decoded HomeKit Accessory Protocol advertisement. `_hap._udp` means the
/// accessory speaks HAP over CoAP, which is only used on Thread — so seeing an
/// accessory here is direct evidence of Thread membership, no inference needed.
struct HAPRecord: Hashable {
    /// The accessory's HAP device ID, formatted like a MAC address.
    var deviceID: String?
    var model: String?
    var configNumber: UInt64?
    var stateNumber: UInt64?
    var protocolVersion: String?
    var categoryIdentifier: UInt64?
    var statusFlags: UInt64?
    var featureFlags: UInt64?
    var setupHash: String?

    /// Status flag bit 0 set means "not paired to any controller yet".
    var isUnpaired: Bool? {
        guard let sf = statusFlags else { return nil }
        return sf & 0x01 == 0x01
    }

    var categoryName: String {
        switch categoryIdentifier ?? 0 {
        case 1:  "Other"
        case 2:  "Bridge"
        case 3:  "Fan"
        case 4:  "Garage Door Opener"
        case 5:  "Lightbulb"
        case 6:  "Door Lock"
        case 7:  "Outlet"
        case 8:  "Switch"
        case 9:  "Thermostat"
        case 10: "Sensor"
        case 11: "Security System"
        case 12: "Door"
        case 13: "Window"
        case 14: "Window Covering"
        case 15: "Programmable Switch"
        case 17: "IP Camera"
        case 18: "Video Doorbell"
        case 19: "Air Purifier"
        case 20: "Heater"
        case 21: "Air Conditioner"
        case 22: "Humidifier"
        case 23: "Dehumidifier"
        case 28: "Sprinkler"
        case 29: "Faucet"
        case 30: "Shower System"
        case 32: "Remote"
        case 33: "Wi-Fi Router"
        case 34: "Audio Receiver"
        case 35: "TV Set-Top Box"
        case 36: "TV Stick"
        default: "Accessory"
        }
    }

    init(txt: [String: Data]) {
        func text(_ key: String) -> String? {
            guard let d = txt[key], !d.isEmpty else { return nil }
            return String(data: d, encoding: .utf8)
        }
        func number(_ key: String) -> UInt64? {
            guard let s = text(key) else { return nil }
            return UInt64(s)
        }

        deviceID           = text("id")
        model              = text("md")
        protocolVersion    = text("pv")
        setupHash          = text("sh")
        configNumber       = number("c#")
        stateNumber        = number("s#")
        categoryIdentifier = number("ci")
        statusFlags        = number("sf")
        featureFlags       = number("ff")
    }
}
