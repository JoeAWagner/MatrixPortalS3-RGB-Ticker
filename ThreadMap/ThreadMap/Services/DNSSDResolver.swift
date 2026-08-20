import Foundation
import dnssd

/// Resolves a Bonjour service instance to its hostname, port, TXT record, and
/// **every** IP address it advertises.
///
/// `NWBrowser` deliberately hides addresses (it hands you an opaque endpoint),
/// but for Thread mapping the addresses are the payload: matching a device's
/// IPv6 address against a border router's advertised OMR prefix is the only
/// supported way to tell which Thread network that device is on. So we drop to
/// the `dnssd` C API, which is fully supported and returns all A/AAAA records.
final class DNSSDResolver: Sendable {

    struct Resolution: Sendable {
        var hostname: String
        var port: UInt16
        var txt: [String: Data]
        var addresses: [IPAddress]
    }

    private let queue = DispatchQueue(label: "com.threadmap.dnssd", qos: .userInitiated)

    /// - Parameter interfaceIndex: 0 means "all interfaces"; pass the index from
    ///   the browse result when known so we query the interface we saw it on.
    func resolve(
        name: String,
        type: String,
        domain: String,
        interfaceIndex: UInt32 = 0,
        timeout: TimeInterval = 4.0
    ) async -> Resolution? {
        guard let srv = await resolveSRV(name: name, type: type, domain: domain,
                                         interfaceIndex: interfaceIndex, timeout: timeout)
        else { return nil }

        let addresses = await addresses(forHost: srv.hostname,
                                        interfaceIndex: interfaceIndex,
                                        timeout: timeout)
        return Resolution(hostname: srv.hostname, port: srv.port, txt: srv.txt, addresses: addresses)
    }

    // MARK: - SRV + TXT

    private final class SRVBox {
        var hostname: String?
        var port: UInt16 = 0
        var txt: [String: Data] = [:]
        var resumed = false
        var continuation: CheckedContinuation<Void, Never>?

        func finish() {
            guard !resumed else { return }
            resumed = true
            continuation?.resume()
            continuation = nil
        }
    }

    private func resolveSRV(
        name: String, type: String, domain: String,
        interfaceIndex: UInt32, timeout: TimeInterval
    ) async -> (hostname: String, port: UInt16, txt: [String: Data])? {

        let box = SRVBox()
        var ref: DNSServiceRef?

        let callback: DNSServiceResolveReply = { _, _, _, errorCode, _, hosttarget, port, txtLen, txtRecord, context in
            guard let context else { return }
            let box = Unmanaged<SRVBox>.fromOpaque(context).takeUnretainedValue()
            guard errorCode == kDNSServiceErr_NoError, let hosttarget else {
                box.finish(); return
            }
            box.hostname = String(cString: hosttarget)
            box.port = UInt16(bigEndian: port)
            box.txt = DNSSDResolver.parseTXT(txtRecord, length: txtLen)
            box.finish()
        }

        let context = Unmanaged.passRetained(box).toOpaque()
        let err = DNSServiceResolve(&ref, 0, interfaceIndex, name, type, domain, callback, context)
        guard err == kDNSServiceErr_NoError, let ref else {
            Unmanaged<SRVBox>.fromOpaque(context).release()
            return nil
        }
        DNSServiceSetDispatchQueue(ref, queue)

        await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
            queue.async {
                if box.resumed { cont.resume() } else { box.continuation = cont }
                self.queue.asyncAfter(deadline: .now() + timeout) { box.finish() }
            }
        }

        queue.sync {
            DNSServiceRefDeallocate(ref)
            Unmanaged<SRVBox>.fromOpaque(context).release()
        }

        guard let hostname = box.hostname else { return nil }
        return (hostname, box.port, box.txt)
    }

    // MARK: - Addresses

    private final class AddrBox {
        var addresses: [IPAddress] = []
        var resumed = false
        var continuation: CheckedContinuation<Void, Never>?

        func finish() {
            guard !resumed else { return }
            resumed = true
            continuation?.resume()
            continuation = nil
        }
    }

    private func addresses(forHost host: String, interfaceIndex: UInt32, timeout: TimeInterval) async -> [IPAddress] {
        let box = AddrBox()
        var ref: DNSServiceRef?

        let callback: DNSServiceGetAddrInfoReply = { _, flags, _, errorCode, _, address, _, context in
            guard let context else { return }
            let box = Unmanaged<AddrBox>.fromOpaque(context).takeUnretainedValue()
            if errorCode == kDNSServiceErr_NoError, let address,
               let parsed = IPAddressParser.from(sockaddr: address),
               !box.addresses.contains(parsed) {
                box.addresses.append(parsed)
            }
            // Keep collecting while the daemon says more records are queued; a
            // Thread device typically reports a link-local, a mesh-local, and
            // an OMR address, and we want all three.
            if flags & kDNSServiceFlagsMoreComing == 0 {
                box.finish()
            }
        }

        let context = Unmanaged.passRetained(box).toOpaque()
        let proto = UInt32(kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6)
        let err = DNSServiceGetAddrInfo(&ref, 0, interfaceIndex, proto, host, callback, context)
        guard err == kDNSServiceErr_NoError, let ref else {
            Unmanaged<AddrBox>.fromOpaque(context).release()
            return []
        }
        DNSServiceSetDispatchQueue(ref, queue)

        await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
            queue.async {
                if box.resumed { cont.resume() } else { box.continuation = cont }
                self.queue.asyncAfter(deadline: .now() + timeout) { box.finish() }
            }
        }

        let result = queue.sync { () -> [IPAddress] in
            DNSServiceRefDeallocate(ref)
            let addrs = box.addresses
            Unmanaged<AddrBox>.fromOpaque(context).release()
            return addrs
        }
        return result
    }

    // MARK: - TXT decoding

    /// Decodes the raw TXT blob. Values stay as `Data` because Thread's keys
    /// (`xp`, `id`, `sb`, `omr`) are binary and would be mangled by UTF-8.
    static func parseTXT(_ pointer: UnsafePointer<UInt8>?, length: UInt16) -> [String: Data] {
        guard let pointer, length > 0 else { return [:] }
        var result: [String: Data] = [:]
        let count = TXTRecordGetCount(length, pointer)
        for index in 0..<count {
            var keyBuffer = [CChar](repeating: 0, count: 256)
            var valueLength: UInt8 = 0
            var valuePointer: UnsafeRawPointer?
            let err = TXTRecordGetItemAtIndex(length, pointer, index,
                                              UInt16(keyBuffer.count), &keyBuffer,
                                              &valueLength, &valuePointer)
            guard err == kDNSServiceErr_NoError else { continue }
            let key = String(cString: keyBuffer)
            if let valuePointer, valueLength > 0 {
                result[key] = Data(bytes: valuePointer, count: Int(valueLength))
            } else {
                result[key] = Data()
            }
        }
        return result
    }
}
