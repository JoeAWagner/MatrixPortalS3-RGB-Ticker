import Foundation

/// Turns raw discovery output into the graph the map draws.
///
/// The honest shape of the problem: mDNS tells us which *networks* exist and
/// which devices sit on them, HomeKit tells us what those devices are called,
/// and nothing tells us the mesh links inside a Thread network. So the graph we
/// build is three layers deep — network → border routers, network → devices —
/// and every edge that isn't read straight off the wire carries a `Confidence`
/// and a reason string that the UI shows the user.
struct TopologyBuilder {

    struct Input {
        var records: [ServiceRecord] = []
        var knownNetworks: [ThreadCredentialsService.KnownNetwork] = []
        var accessories: [HomeKitAccessory] = []
    }

    func build(_ input: Input) -> Topology {
        var topology = Topology()
        topology.accessories = input.accessories

        let (routerRecords, deviceRecords, leftovers) = partition(input.records)

        var routers = buildBorderRouters(from: routerRecords)
        var networks = buildNetworks(routers: routers, known: input.knownNetworks)

        // Fill in names the border routers didn't advertise but the keychain knows.
        applyCredentialNames(to: &networks, routers: routers, known: input.knownNetworks)

        var devices = buildDevices(from: deviceRecords)

        // OMR prefixes come from Thread 1.4 border routers directly; for older
        // routers we reconstruct them from devices we know are on Thread.
        inferMissingOMRPrefixes(networks: &networks, devices: devices)

        assignNetworks(to: &devices, networks: networks)
        correlateHomeKit(devices: &devices, routers: &routers, accessories: input.accessories)
        nameDevices(&devices, accessories: input.accessories)

        // Wire the child ID lists back onto the networks for easy traversal.
        for index in networks.indices {
            let id = networks[index].id
            networks[index].borderRouterIDs = routers.filter { $0.networkID == id }.map(\.id)
            networks[index].deviceIDs = devices.filter { $0.networkID.value == id }.map(\.id)
        }

        topology.networks = networks.sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
        topology.borderRouters = routers.sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
        topology.devices = devices.sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
        topology.unmatchedRecords = leftovers
        topology.generatedAt = .now
        return topology
    }

    // MARK: - Partitioning

    private func partition(_ records: [ServiceRecord]) -> ([ServiceRecord], [ServiceRecord], [ServiceRecord]) {
        var routers: [ServiceRecord] = []
        var devices: [ServiceRecord] = []
        var others: [ServiceRecord] = []
        for record in records {
            if ServiceType.borderRouterTypes.contains(record.type) { routers.append(record) }
            else if ServiceType.deviceTypes.contains(record.type) { devices.append(record) }
            else { others.append(record) }
        }
        return (routers, devices, others)
    }

    // MARK: - Border routers

    private func buildBorderRouters(from records: [ServiceRecord]) -> [BorderRouter] {
        var routers: [BorderRouter] = []

        // MeshCoP records define the routers; TREL and ePSKc records are extra
        // evidence about a router we've usually already seen.
        for record in records where record.type == .meshcop {
            let meshcop = MeshcopRecord(txt: record.txt)
            let id = meshcop.borderAgentID ?? "instance:\(record.instanceName)"
            var router = BorderRouter(
                id: id,
                displayName: record.instanceName,
                vendorName: meshcop.vendorName,
                modelName: meshcop.modelName,
                borderAgentID: meshcop.borderAgentID,
                networkID: meshcop.extendedPANID ?? syntheticNetworkID(for: record),
                addresses: record.addresses,
                hostname: record.hostname,
                meshcop: meshcop,
                advertisedTypes: [.meshcop],
                lastSeen: record.lastSeen
            )
            router.displayName = Self.friendlyRouterName(record: record, meshcop: meshcop)
            routers.append(router)
        }

        for record in records where record.type != .meshcop {
            // Attach to whichever router shares this hostname or an address.
            if let index = routers.firstIndex(where: { router in
                (record.hostname != nil && router.hostname == record.hostname)
                    || !Set(router.addresses).isDisjoint(with: Set(record.addresses))
            }) {
                routers[index].advertisedTypes.insert(record.type)
                for address in record.addresses where !routers[index].addresses.contains(address) {
                    routers[index].addresses.append(address)
                }
            } else {
                // A TREL-only advertiser is still a Thread router we should show,
                // even though we can't name its network.
                routers.append(
                    BorderRouter(
                        id: "instance:\(record.instanceName)",
                        displayName: record.instanceName,
                        vendorName: nil,
                        modelName: nil,
                        borderAgentID: nil,
                        networkID: nil,
                        addresses: record.addresses,
                        hostname: record.hostname,
                        meshcop: nil,
                        advertisedTypes: [record.type],
                        lastSeen: record.lastSeen
                    )
                )
            }
        }

        return routers
    }

    /// MeshCoP instance names are usually vendor-set and readable
    /// (`Apple TV 1A2B`), but some routers publish a bare hex blob. Prefer
    /// vendor + model when the instance name looks machine-generated.
    private static func friendlyRouterName(record: ServiceRecord, meshcop: MeshcopRecord) -> String {
        let instance = record.instanceName
        let looksLikeHex = instance.count >= 12 && instance.allSatisfy { $0.isHexDigit || $0 == "-" }
        if looksLikeHex {
            let parts = [meshcop.vendorName, meshcop.modelName].compactMap { $0 }.filter { !$0.isEmpty }
            if !parts.isEmpty { return parts.joined(separator: " ") }
            if let host = record.hostname { return host.replacingOccurrences(of: ".local.", with: "") }
        }
        return instance
    }

    private func syntheticNetworkID(for record: ServiceRecord) -> String {
        "unidentified:\(record.instanceName)"
    }

    // MARK: - Networks

    private func buildNetworks(routers: [BorderRouter],
                               known: [ThreadCredentialsService.KnownNetwork]) -> [ThreadNetwork] {
        var byID: [String: ThreadNetwork] = [:]

        for router in routers {
            guard let networkID = router.networkID else { continue }
            var network = byID[networkID] ?? ThreadNetwork(id: networkID)
            network.extendedPANID = router.meshcop?.extendedPANID
            if network.name == nil { network.name = router.meshcop?.networkName }
            if network.threadVersion == nil { network.threadVersion = router.meshcop?.threadVersion }
            if let omr = router.meshcop?.omrPrefix, !network.omrPrefixes.contains(omr) {
                network.omrPrefixes.append(omr)
            }
            byID[networkID] = network
        }

        // Networks this phone has credentials for but whose routers are offline
        // (or out of range) still deserve a place on the map.
        for credential in known {
            guard let xpan = credential.extendedPANID else { continue }
            var network = byID[xpan] ?? ThreadNetwork(id: xpan)
            network.extendedPANID = xpan
            if network.name == nil { network.name = credential.networkName }
            network.channel = credential.channel
            network.panID = credential.panID
            network.hasStoredCredentials = true
            byID[xpan] = network
        }

        return Array(byID.values)
    }

    /// A router can advertise a border agent ID without a network name. If the
    /// keychain has credentials for that same border agent, borrow the name.
    private func applyCredentialNames(to networks: inout [ThreadNetwork],
                                      routers: [BorderRouter],
                                      known: [ThreadCredentialsService.KnownNetwork]) {
        guard !known.isEmpty else { return }
        let byBorderAgent = Dictionary(known.compactMap { credential in
            credential.borderAgentID.map { ($0, credential) }
        }, uniquingKeysWith: { first, _ in first })

        for router in routers {
            guard let agentID = router.borderAgentID,
                  let credential = byBorderAgent[agentID],
                  let networkID = router.networkID,
                  let index = networks.firstIndex(where: { $0.id == networkID })
            else { continue }
            if networks[index].name == nil { networks[index].name = credential.networkName }
            if networks[index].channel == nil { networks[index].channel = credential.channel }
            if networks[index].panID == nil { networks[index].panID = credential.panID }
            networks[index].hasStoredCredentials = true
        }
    }

    // MARK: - Devices

    private func buildDevices(from records: [ServiceRecord]) -> [MeshDevice] {
        var devices: [MeshDevice] = []

        for record in records {
            var matter: MatterRecord?
            var hap: HAPRecord?
            switch record.type {
            case .matter, .matterC:
                matter = MatterRecord(instanceName: record.instanceName, txt: record.txt)
            case .hapThread, .hapIP:
                hap = HAPRecord(txt: record.txt)
            default:
                break
            }

            let transport = classifyTransport(record: record, matter: matter)

            // Merge into an existing device when this is another advertisement
            // from the same box: same hostname, same Matter node, or a shared
            // routable address.
            if let index = devices.firstIndex(where: { existing in
                if let host = record.hostname, let existingHost = existing.hostname, host == existingHost { return true }
                if let node = matter?.nodeIDHex, let existingNode = existing.matter?.nodeIDHex, node == existingNode { return true }
                let shared = Set(existing.addresses.filter(\.isRoutable))
                    .intersection(Set(record.routableAddresses))
                return !shared.isEmpty
            }) {
                devices[index].protocols.insert(record.type)
                for address in record.addresses where !devices[index].addresses.contains(address) {
                    devices[index].addresses.append(address)
                }
                if devices[index].matter == nil { devices[index].matter = matter }
                if devices[index].hap == nil { devices[index].hap = hap }
                if devices[index].hostname == nil { devices[index].hostname = record.hostname }
                // A stronger transport signal wins — `_hap._udp` beats a guess.
                if transport.confidence > devices[index].transport.confidence {
                    devices[index].transport = transport
                }
                continue
            }

            devices.append(
                MeshDevice(
                    id: "\(record.type.rawValue)|\(record.instanceName)",
                    displayName: record.instanceName,
                    transport: transport,
                    networkID: Attributed(nil, .unknown, "Not yet assigned."),
                    protocols: [record.type],
                    addresses: record.addresses,
                    hostname: record.hostname,
                    matter: matter,
                    hap: hap,
                    homeKitAccessoryID: nil,
                    lastSeen: record.lastSeen
                )
            )
        }

        return devices
    }

    private func classifyTransport(record: ServiceRecord, matter: MatterRecord?) -> Attributed<MeshDevice.Transport> {
        switch record.type {
        case .hapThread:
            // HAP over CoAP/UDP exists only for Thread accessories. This is the
            // one transport call we can make with certainty.
            return Attributed(.thread, .observed,
                              "Advertises _hap._udp. HomeKit only uses HAP-over-CoAP on Thread, never on Wi-Fi.")
        case .hapIP:
            return Attributed(.wifiOrEthernet, .observed,
                              "Advertises _hap._tcp, which HomeKit uses only over IP (Wi-Fi or Ethernet).")
        case .matter, .matterC:
            if record.addresses.contains(where: { $0.family == .v4 }) {
                return Attributed(.wifiOrEthernet, .derived,
                                  "Has an IPv4 address. Thread is IPv6-only, so this device is on Wi-Fi or Ethernet.")
            }
            if matter?.looksSleepy == true {
                return Attributed(.thread, .inferred,
                                  "Advertises an intermittently-connected (ICD) profile. Duty-cycled radios are effectively Thread-only.")
            }
            if record.routableAddresses.allSatisfy(\.isUniqueLocal), !record.routableAddresses.isEmpty {
                return Attributed(.thread, .inferred,
                                  "Only reachable on unique-local IPv6 addresses, which is how Thread devices appear through a border router.")
            }
            return Attributed(.unknown, .unknown,
                              "A Matter node can run on Thread, Wi-Fi, or Ethernet, and nothing in its advertisement says which.")
        default:
            return Attributed(.unknown, .unknown, "Unrecognised service type.")
        }
    }

    // MARK: - OMR prefixes

    /// Thread 1.4 border routers publish their OMR prefix in the `omr` TXT key.
    /// Thread 1.3 ones don't, so reconstruct it from devices we already know are
    /// on Thread: their routable ULA /64 *is* the OMR prefix.
    private func inferMissingOMRPrefixes(networks: inout [ThreadNetwork], devices: [MeshDevice]) {
        let threadNetworks = networks.filter { !$0.isSynthetic }
        guard threadNetworks.count == 1, let index = networks.firstIndex(where: { $0.id == threadNetworks[0].id }) else { return }
        guard networks[index].omrPrefixes.isEmpty else { return }

        var candidates: [IPv6Prefix] = []
        for device in devices where device.transport.value == .thread && device.transport.confidence >= .inferred {
            for address in device.addresses where address.isUniqueLocal && address.isRoutable {
                guard let prefix = IPv6Prefix(bytes: Array(address.bytes.prefix(8)), length: 64) else { continue }
                if !candidates.contains(prefix) { candidates.append(prefix) }
            }
        }
        networks[index].omrPrefixes = candidates
    }

    // MARK: - Device → network

    private func assignNetworks(to devices: inout [MeshDevice], networks: [ThreadNetwork]) {
        let threadNetworks = networks.filter { !$0.isSynthetic }

        for index in devices.indices {
            guard devices[index].transport.value != .wifiOrEthernet else {
                devices[index].networkID = Attributed(nil, .observed, "Not a Thread device — it talks over IP directly.")
                continue
            }

            // Strongest signal: the device's address falls inside a prefix a
            // border router says it routes.
            var matched: (ThreadNetwork, IPv6Prefix)?
            outer: for network in networks {
                for prefix in network.omrPrefixes {
                    if devices[index].addresses.contains(where: { prefix.contains($0) }) {
                        matched = (network, prefix)
                        break outer
                    }
                }
            }

            if let (network, prefix) = matched {
                devices[index].networkID = Attributed(
                    network.id, .derived,
                    "Its IPv6 address is inside \(prefix), the off-mesh-routable prefix advertised for \(network.displayName)."
                )
                continue
            }

            if threadNetworks.count == 1, devices[index].transport.value == .thread {
                devices[index].networkID = Attributed(
                    threadNetworks[0].id, .inferred,
                    "It's on Thread and only one Thread network is visible here, so it must be that one."
                )
                continue
            }

            if threadNetworks.count > 1 {
                devices[index].networkID = Attributed(
                    nil, .unknown,
                    "\(threadNetworks.count) Thread networks are visible and this device's address doesn't fall inside any advertised prefix."
                )
            } else {
                devices[index].networkID = Attributed(
                    nil, .unknown,
                    "No Thread network could be matched to this device."
                )
            }
        }
    }

    // MARK: - HomeKit correlation

    private func correlateHomeKit(devices: inout [MeshDevice],
                                  routers: inout [BorderRouter],
                                  accessories: [HomeKitAccessory]) {
        guard !accessories.isEmpty else { return }
        var claimed = Set<UUID>()

        // Pass 1 — exact: Matter node IDs match byte for byte.
        let byNodeID = Dictionary(accessories.compactMap { accessory in
            accessory.matterNodeIDHex.map { ($0, accessory.id) }
        }, uniquingKeysWith: { first, _ in first })

        for index in devices.indices {
            guard let node = devices[index].matter?.nodeIDHex, let id = byNodeID[node] else { continue }
            devices[index].homeKitAccessoryID = id
            claimed.insert(id)
        }

        // Pass 2 — fuzzy: HomeKit never exposes an accessory's HAP pairing ID,
        // so for HAP devices the best available join is the advertised name and
        // model. Require a strong score to avoid inventing links.
        for index in devices.indices where devices[index].homeKitAccessoryID == nil {
            let device = devices[index]
            let candidates = accessories.filter { !claimed.contains($0.id) }
            var best: (UUID, Double)?
            for accessory in candidates {
                let score = Self.matchScore(deviceName: device.displayName,
                                            deviceModel: device.hap?.model ?? device.matter?.deviceName,
                                            accessory: accessory)
                if score >= 0.72, score > (best?.1 ?? 0) { best = (accessory.id, score) }
            }
            if let (id, _) = best {
                devices[index].homeKitAccessoryID = id
                claimed.insert(id)
            }
        }

        // Border routers: an Apple TV or HomePod also appears in the HomeKit
        // database, which lets us show the room it lives in.
        for index in routers.indices {
            let router = routers[index]
            var best: (UUID, Double)?
            for accessory in accessories where !claimed.contains(accessory.id) {
                let score = Self.matchScore(deviceName: router.displayName,
                                            deviceModel: router.modelName,
                                            accessory: accessory)
                if score >= 0.72, score > (best?.1 ?? 0) { best = (accessory.id, score) }
            }
            if let (id, _) = best {
                routers[index].homeKitAccessoryID = id
                claimed.insert(id)
            }
        }
    }

    /// 0…1. Names dominate; a matching model is a tiebreaker, not a match on
    /// its own — five identical bulbs would otherwise all score the same.
    static func matchScore(deviceName: String, deviceModel: String?, accessory: HomeKitAccessory) -> Double {
        let name = nameSimilarity(deviceName, accessory.name)
        var score = name
        if let deviceModel, let accessoryModel = accessory.model,
           normalize(deviceModel) == normalize(accessoryModel), !deviceModel.isEmpty {
            score = min(1.0, score + 0.15)
        }
        return score
    }

    private static func nameSimilarity(_ lhs: String, _ rhs: String) -> Double {
        let a = normalize(lhs), b = normalize(rhs)
        guard !a.isEmpty, !b.isEmpty else { return 0 }
        if a == b { return 1.0 }

        // Bonjour appends a disambiguating suffix (`Front Door 4C2A`), so a
        // prefix relationship is close to an exact match.
        if a.hasPrefix(b) || b.hasPrefix(a) { return 0.9 }
        if a.contains(b) || b.contains(a) { return 0.8 }

        let aTokens = Set(tokens(lhs)), bTokens = Set(tokens(rhs))
        guard !aTokens.isEmpty, !bTokens.isEmpty else { return 0 }
        let overlap = Double(aTokens.intersection(bTokens).count)
        return overlap / Double(max(aTokens.count, bTokens.count))
    }

    private static func normalize(_ string: String) -> String {
        string.lowercased().filter { $0.isLetter || $0.isNumber }
    }

    private static func tokens(_ string: String) -> [String] {
        string.lowercased()
            .split { !$0.isLetter && !$0.isNumber }
            .map(String.init)
            .filter { $0.count > 1 }
    }

    // MARK: - Naming

    /// Matter's operational instance name is pure hex, so a commissioned Matter
    /// device is nameless until HomeKit tells us what it's called.
    private func nameDevices(_ devices: inout [MeshDevice], accessories: [HomeKitAccessory]) {
        let byID = Dictionary(accessories.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        for index in devices.indices {
            if let id = devices[index].homeKitAccessoryID, let accessory = byID[id] {
                devices[index].displayName = accessory.name
                continue
            }
            if let name = devices[index].matter?.deviceName, !name.isEmpty {
                devices[index].displayName = name
                continue
            }
            let current = devices[index].displayName
            let looksLikeHex = current.count >= 16 && current.allSatisfy { $0.isHexDigit || $0 == "-" }
            if looksLikeHex {
                if let host = devices[index].hostname {
                    devices[index].displayName = host
                        .replacingOccurrences(of: ".local.", with: "")
                        .replacingOccurrences(of: ".local", with: "")
                } else if let node = devices[index].matter?.nodeIDHex {
                    devices[index].displayName = "Matter node \(node.suffix(6))"
                }
            }
        }
    }
}
