import Foundation
import Darwin

/// Conversions between `sockaddr`, presentation strings, and our `IPAddress`.
enum IPAddressParser {

    /// Parses a numeric address string. Returns nil for hostnames.
    static func parse(_ string: String) -> IPAddress? {
        // Strip a zone index such as `fe80::1%en0` before handing to inet_pton.
        let (literal, zone) = splitZone(string)

        var v6 = in6_addr()
        if literal.withCString({ inet_pton(AF_INET6, $0, &v6) }) == 1 {
            let bytes = withUnsafeBytes(of: &v6) { Array($0) }
            return IPAddress(family: .v6, text: string, bytes: bytes, scopeID: zone)
        }
        var v4 = in_addr()
        if literal.withCString({ inet_pton(AF_INET, $0, &v4) }) == 1 {
            let bytes = withUnsafeBytes(of: &v4) { Array($0) }
            return IPAddress(family: .v4, text: string, bytes: bytes, scopeID: 0)
        }
        return nil
    }

    /// Converts a raw `sockaddr` from `DNSServiceGetAddrInfo` into an `IPAddress`.
    static func from(sockaddr sa: UnsafePointer<sockaddr>) -> IPAddress? {
        switch Int32(sa.pointee.sa_family) {
        case AF_INET6:
            return sa.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) { p in
                var addr = p.pointee.sin6_addr
                let bytes = withUnsafeBytes(of: &addr) { Array($0) }
                guard let text = presentation(sa) else { return nil }
                return IPAddress(family: .v6, text: text, bytes: bytes, scopeID: p.pointee.sin6_scope_id)
            }
        case AF_INET:
            return sa.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { p in
                var addr = p.pointee.sin_addr
                let bytes = withUnsafeBytes(of: &addr) { Array($0) }
                guard let text = presentation(sa) else { return nil }
                return IPAddress(family: .v4, text: text, bytes: bytes, scopeID: 0)
            }
        default:
            return nil
        }
    }

    private static func presentation(_ sa: UnsafePointer<sockaddr>) -> String? {
        var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
        let len = socklen_t(sa.pointee.sa_len)
        let status = getnameinfo(sa, len, &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
        guard status == 0 else { return nil }
        return String(cString: host)
    }

    private static func splitZone(_ string: String) -> (String, UInt32) {
        guard let pct = string.firstIndex(of: "%") else { return (string, 0) }
        let literal = String(string[string.startIndex..<pct])
        let zoneName = String(string[string.index(after: pct)...])
        let index = if_nametoindex(zoneName)
        return (literal, index)
    }
}
