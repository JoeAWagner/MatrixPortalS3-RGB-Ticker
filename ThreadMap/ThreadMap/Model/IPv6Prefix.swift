import Foundation

/// A parsed IP address as seen on the wire, kept in both text and byte form so
/// we can do prefix arithmetic without re-parsing strings.
struct IPAddress: Hashable, Identifiable, Codable, Sendable {
    enum Family: String, Codable, Sendable { case v4, v6 }

    var id: String { text }
    let family: Family
    /// Numeric form, e.g. `fd11:22::1a2b` — never a `.local` hostname.
    let text: String
    /// 4 bytes for v4, 16 for v6.
    let bytes: [UInt8]
    /// Scope ID for link-local v6 addresses (zero otherwise).
    let scopeID: UInt32

    var isIPv6: Bool { family == .v6 }

    /// `fe80::/10`
    var isLinkLocal: Bool {
        guard family == .v6, bytes.count == 16 else { return false }
        return bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80
    }

    /// `fc00::/7` — Thread mesh-local and OMR addresses both live in here.
    var isUniqueLocal: Bool {
        guard family == .v6, bytes.count == 16 else { return false }
        return (bytes[0] & 0xFE) == 0xFC
    }

    /// `2000::/3`
    var isGlobalUnicast: Bool {
        guard family == .v6, bytes.count == 16 else { return false }
        return (bytes[0] & 0xE0) == 0x20
    }

    /// An address a Thread device would plausibly be reachable on from the
    /// infrastructure link: not link-local, not loopback.
    var isRoutable: Bool {
        guard !isLinkLocal else { return false }
        if family == .v4 { return bytes != [127, 0, 0, 1] }
        return bytes != Array(repeating: 0, count: 15) + [1]
    }
}

/// An IPv6 prefix such as an OMR (Off-Mesh-Routable) prefix advertised by a
/// border router in its MeshCoP `omr` TXT key.
struct IPv6Prefix: Hashable, Codable, Sendable, CustomStringConvertible {
    let bytes: [UInt8]      // always 16, zero-padded past the prefix length
    let length: Int         // bits

    init?(bytes: [UInt8], length: Int) {
        guard (0...128).contains(length) else { return nil }
        var padded = bytes
        if padded.count < 16 { padded += Array(repeating: 0, count: 16 - padded.count) }
        guard padded.count == 16 else { return nil }
        self.bytes = Array(padded.prefix(16))
        self.length = length
    }

    /// Parses the wire encoding used by the MeshCoP `omr` TXT value:
    /// one length byte followed by the significant prefix bytes.
    init?(meshcopEncoded data: Data) {
        guard let first = data.first else { return nil }
        let bits = Int(first)
        let body = Array(data.dropFirst())
        guard bits <= 128, body.count >= (bits + 7) / 8 else { return nil }
        self.init(bytes: body, length: bits)
    }

    /// Parses `fd12:3456:789a::/64`.
    init?(string: String) {
        let parts = string.split(separator: "/", maxSplits: 1)
        guard let head = parts.first,
              let bits = parts.count > 1 ? Int(parts[1]) : 64,
              let addr = IPAddressParser.parse(String(head)),
              addr.family == .v6
        else { return nil }
        self.init(bytes: addr.bytes, length: bits)
    }

    func contains(_ address: IPAddress) -> Bool {
        guard address.family == .v6, address.bytes.count == 16 else { return false }
        let wholeBytes = length / 8
        let remainderBits = length % 8
        for i in 0..<wholeBytes where address.bytes[i] != bytes[i] { return false }
        if remainderBits > 0 {
            let mask = UInt8(0xFF << (8 - remainderBits))
            if (address.bytes[wholeBytes] & mask) != (bytes[wholeBytes] & mask) { return false }
        }
        return true
    }

    var description: String {
        let significant = (length + 7) / 8
        var trimmed = Array(bytes.prefix(max(significant, 2)))
        if trimmed.count % 2 == 1 { trimmed.append(0) }
        var groups: [String] = []
        for i in stride(from: 0, to: trimmed.count, by: 2) {
            groups.append(String(format: "%x", (UInt16(trimmed[i]) << 8) | UInt16(trimmed[i + 1])))
        }
        return groups.joined(separator: ":") + "::/\(length)"
    }
}
