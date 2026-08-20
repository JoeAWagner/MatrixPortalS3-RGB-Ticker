import SwiftUI

/// Resolves a tapped map node into the right detail screen.
struct SelectionDetailView: View {
    let selection: MapSelection
    let topology: Topology

    var body: some View {
        NavigationStack {
            Group {
                switch selection.kind {
                case .network(let id):
                    if let network = topology.networks.first(where: { $0.id == id }) {
                        NetworkDetailView(network: network, topology: topology)
                    } else { MissingView() }
                case .router(let id):
                    if let router = topology.borderRouters.first(where: { $0.id == id }) {
                        RouterDetailView(router: router, topology: topology)
                    } else { MissingView() }
                case .device(let id):
                    if let device = topology.devices.first(where: { $0.id == id }) {
                        DeviceDetailView(device: device, topology: topology)
                    } else { MissingView() }
                case .cluster:
                    UnplacedDetailView(topology: topology)
                }
            }
            .navigationTitle(selection.title)
            .navigationBarTitleDisplayMode(.inline)
        }
    }
}

private struct MissingView: View {
    var body: some View {
        ContentUnavailableView("Gone from the last scan",
                               systemImage: "clock.arrow.circlepath",
                               description: Text("Scan again to refresh."))
    }
}

// MARK: - Network

struct NetworkDetailView: View {
    let network: ThreadNetwork
    let topology: Topology

    var body: some View {
        List {
            Section("Network") {
                LabeledContent("Name", value: network.displayName)
                if let xpan = network.extendedPANID {
                    LabeledContent("Extended PAN ID", value: xpan.hexGrouped)
                }
                if let panID = network.panID { LabeledContent("PAN ID", value: "0x" + panID) }
                if let channel = network.channel { LabeledContent("Channel", value: "\(channel)") }
                if let version = network.threadVersion { LabeledContent("Thread version", value: version) }
                LabeledContent("Credentials on this iPhone",
                               value: network.hasStoredCredentials ? "Yes" : "No")
            }

            if !network.omrPrefixes.isEmpty {
                Section {
                    ForEach(network.omrPrefixes, id: \.self) { prefix in
                        Text(prefix.description).font(.system(.body, design: .monospaced))
                    }
                } header: {
                    Text("Routable prefixes")
                } footer: {
                    Text("Devices whose IPv6 address falls inside one of these prefixes are definitely on this network. That address match is how this app places devices.")
                }
            }

            let routers = topology.borderRouters(on: network.id)
            Section("Border routers (\(routers.count))") {
                if routers.isEmpty {
                    Text("None visible. The network is known to this iPhone but no router is announcing it right now.")
                        .foregroundStyle(.secondary)
                }
                ForEach(routers) { router in
                    NavigationLink {
                        RouterDetailView(router: router, topology: topology)
                    } label: {
                        RouterRow(router: router, topology: topology)
                    }
                }
            }

            let devices = topology.devices(on: network.id)
            Section("Devices (\(devices.count))") {
                ForEach(devices) { device in
                    NavigationLink {
                        DeviceDetailView(device: device, topology: topology)
                    } label: {
                        DeviceRow(device: device, topology: topology)
                    }
                }
            }
        }
    }
}

// MARK: - Border router

struct RouterDetailView: View {
    let router: BorderRouter
    let topology: Topology

    var body: some View {
        List {
            Section("Hardware") {
                LabeledContent("Advertised name", value: router.displayName)
                if let vendor = router.vendorName { LabeledContent("Vendor", value: vendor) }
                if let model = router.modelName { LabeledContent("Model", value: model) }
                if let accessory = topology.accessory(router.homeKitAccessoryID) {
                    LabeledContent("HomeKit", value: accessory.name)
                    if let room = accessory.roomName { LabeledContent("Room", value: room) }
                }
                if let host = router.hostname { LabeledContent("Hostname", value: host) }
            }

            if let meshcop = router.meshcop {
                Section("Thread state") {
                    if let status = meshcop.interfaceStatus {
                        LabeledContent("Interface", value: status.label)
                    }
                    if let role = meshcop.threadRole {
                        LabeledContent("Mesh role", value: role.label)
                    }
                    if let mode = meshcop.connectionMode {
                        LabeledContent("Joining", value: mode.label)
                    }
                    if let availability = meshcop.availability {
                        LabeledContent("Availability", value: availability.label)
                    }
                    if let active = meshcop.isBackboneRouterActive {
                        LabeledContent("Backbone router",
                                       value: active ? (meshcop.isPrimaryBackboneRouter == true ? "Primary" : "Secondary") : "Off")
                    }
                    if let version = meshcop.threadVersion { LabeledContent("Thread version", value: version) }
                    if let partition = meshcop.partitionID {
                        LabeledContent("Partition ID", value: String(format: "0x%08X", partition))
                    }
                }

                Section {
                    if let agent = meshcop.borderAgentID {
                        LabeledContent("Border agent ID", value: agent.hexGrouped)
                            .font(.system(.body, design: .monospaced))
                    }
                    if let xpan = meshcop.extendedPANID {
                        LabeledContent("Extended PAN ID", value: xpan.hexGrouped)
                            .font(.system(.body, design: .monospaced))
                    }
                } header: {
                    Text("Identifiers")
                } footer: {
                    Text("The border agent ID is what lets this app tie a router on the network to a Thread network stored on your iPhone.")
                }
            }

            Section("Advertises") {
                ForEach(Array(router.advertisedTypes).sorted(by: { $0.rawValue < $1.rawValue }), id: \.self) { type in
                    LabeledContent(type.displayName, value: type.rawValue)
                        .font(.callout)
                }
            }

            AddressSection(addresses: router.addresses)

            Section {
                Text("Two routers on the same network are peers in one mesh. If a partition ID differs between routers that share an extended PAN ID, the mesh has split — devices on one partition can't reach the other.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

// MARK: - Device

struct DeviceDetailView: View {
    let device: MeshDevice
    let topology: Topology

    var body: some View {
        List {
            Section("Device") {
                LabeledContent("Name", value: device.displayName)
                LabeledContent("Kind", value: device.kindLabel)
                if let accessory = topology.accessory(device.homeKitAccessoryID) {
                    LabeledContent("Home", value: accessory.homeName)
                    if let room = accessory.roomName { LabeledContent("Room", value: room) }
                    if let manufacturer = accessory.manufacturer { LabeledContent("Made by", value: manufacturer) }
                    if let model = accessory.model { LabeledContent("Model", value: model) }
                    if let firmware = accessory.firmwareVersion { LabeledContent("Firmware", value: firmware) }
                    LabeledContent("Responding", value: accessory.isReachable ? "Yes" : "No")
                } else if let model = device.hap?.model {
                    LabeledContent("Model", value: model)
                }
            }

            Section {
                EvidenceRow(title: "Radio",
                            value: device.transport.value.label,
                            confidence: device.transport.confidence,
                            reason: device.transport.reason)
                EvidenceRow(title: "Thread network",
                            value: topology.network(device.networkID.value)?.displayName ?? "Not placed",
                            confidence: device.networkID.confidence,
                            reason: device.networkID.reason)
                if device.isSleepy {
                    EvidenceRow(title: "Power",
                                value: "Duty-cycled radio",
                                confidence: .observed,
                                reason: "Advertises a sleepy/ICD polling profile, which means it's almost certainly battery powered.")
                }
            } header: {
                Text("How we worked this out")
            } footer: {
                Text("Anything marked Inferred is a best guess from indirect evidence, not something the network stated.")
            }

            if let networkID = device.networkID.value,
               let network = topology.network(networkID) {
                let routers = topology.borderRouters(on: networkID)
                Section {
                    ForEach(routers) { router in
                        NavigationLink {
                            RouterDetailView(router: router, topology: topology)
                        } label: {
                            RouterRow(router: router, topology: topology)
                        }
                    }
                } header: {
                    Text("Routers serving \(network.displayName)")
                } footer: {
                    Text("This device reaches the rest of your home through one of these — but iOS gives no app a way to see which one, or how strong the link is. Any app claiming otherwise is guessing.")
                }
            }

            if let matter = device.matter {
                Section("Matter") {
                    if let node = matter.nodeIDHex {
                        LabeledContent("Node ID", value: node.hexGrouped)
                            .font(.system(.body, design: .monospaced))
                    }
                    if let fabric = matter.compressedFabricID {
                        LabeledContent("Fabric", value: fabric.hexGrouped)
                            .font(.system(.body, design: .monospaced))
                    }
                    if let vendor = matter.vendorID { LabeledContent("Vendor ID", value: "0x\(String(format: "%04X", vendor))") }
                    if let product = matter.productID { LabeledContent("Product ID", value: "0x\(String(format: "%04X", product))") }
                    if let icd = matter.icdMode { LabeledContent("ICD mode", value: icd.label) }
                    if let idle = matter.sessionIdleIntervalMS { LabeledContent("Idle interval", value: "\(idle) ms") }
                    if let active = matter.sessionActiveIntervalMS { LabeledContent("Active interval", value: "\(active) ms") }
                }
            }

            if let hap = device.hap {
                Section("HomeKit accessory protocol") {
                    if let id = hap.deviceID { LabeledContent("Device ID", value: id).font(.system(.body, design: .monospaced)) }
                    LabeledContent("Category", value: hap.categoryName)
                    if let version = hap.protocolVersion { LabeledContent("HAP version", value: version) }
                    if let unpaired = hap.isUnpaired {
                        LabeledContent("Paired", value: unpaired ? "No — available to set up" : "Yes")
                    }
                    if let config = hap.configNumber { LabeledContent("Config number", value: "\(config)") }
                }
            }

            Section("Advertises") {
                ForEach(Array(device.protocols).sorted(by: { $0.rawValue < $1.rawValue }), id: \.self) { type in
                    LabeledContent(type.displayName, value: type.rawValue).font(.callout)
                }
            }

            AddressSection(addresses: device.addresses)
        }
    }
}

// MARK: - Unplaced cluster

struct UnplacedDetailView: View {
    let topology: Topology

    var body: some View {
        List {
            Section {
                Text("These are devices and routers the scan found but couldn't attach to a Thread network — either because they aren't on Thread at all, or because no border router advertised a prefix matching their address.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }

            let unplaced = topology.threadDevices.filter { $0.networkID.value == nil }
            if !unplaced.isEmpty {
                Section("On Thread, network unknown (\(unplaced.count))") {
                    ForEach(unplaced) { device in
                        NavigationLink {
                            DeviceDetailView(device: device, topology: topology)
                        } label: {
                            DeviceRow(device: device, topology: topology)
                        }
                    }
                }
            }

            let offMesh = topology.offMeshDevices
            if !offMesh.isEmpty {
                Section("On Wi-Fi or Ethernet (\(offMesh.count))") {
                    ForEach(offMesh) { device in
                        NavigationLink {
                            DeviceDetailView(device: device, topology: topology)
                        } label: {
                            DeviceRow(device: device, topology: topology)
                        }
                    }
                }
            }
        }
    }
}

// MARK: - Shared pieces

struct EvidenceRow: View {
    let title: String
    let value: String
    let confidence: Confidence
    let reason: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title)
                Spacer()
                Text(value).foregroundStyle(.secondary)
            }
            HStack(spacing: 5) {
                Image(systemName: confidence.symbolName)
                    .font(.caption2)
                Text(confidence.label).font(.caption2.weight(.semibold))
                Text(reason).font(.caption2).foregroundStyle(.secondary)
            }
            .foregroundStyle(tint)
        }
        .padding(.vertical, 2)
    }

    private var tint: Color {
        switch confidence {
        case .observed: .green
        case .derived:  .blue
        case .inferred: .orange
        case .unknown:  .secondary
        }
    }
}

struct AddressSection: View {
    let addresses: [IPAddress]

    var body: some View {
        if addresses.isEmpty {
            Section("Addresses") {
                Text("None resolved. The service was advertised but didn't answer an address query in time.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        } else {
            Section {
                ForEach(addresses) { address in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(address.text).font(.system(.footnote, design: .monospaced))
                        Text(describe(address)).font(.caption2).foregroundStyle(.secondary)
                    }
                }
            } header: {
                Text("Addresses")
            }
        }
    }

    private func describe(_ address: IPAddress) -> String {
        if address.family == .v4 { return "IPv4 — infrastructure network" }
        if address.isLinkLocal { return "IPv6 link-local — reachable only on one hop" }
        if address.isUniqueLocal { return "IPv6 unique-local — typical of a Thread mesh or off-mesh-routable prefix" }
        if address.isGlobalUnicast { return "IPv6 global unicast" }
        return "IPv6"
    }
}

struct RouterRow: View {
    let router: BorderRouter
    let topology: Topology

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "wifi.router.fill")
                .foregroundStyle(router.isAttached ? .green : .orange)
                .frame(width: 26)
            VStack(alignment: .leading, spacing: 2) {
                Text(router.displayName)
                HStack(spacing: 6) {
                    if router.isLeader { Tag(text: "Leader", color: .orange) }
                    if router.isPrimaryBackboneRouter { Tag(text: "Primary BBR", color: .purple) }
                    Text(router.hardwareDescription).font(.caption2).foregroundStyle(.secondary)
                }
            }
        }
    }
}

struct DeviceRow: View {
    let device: MeshDevice
    let topology: Topology

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: device.isOnThread ? "sensor.tag.radiowaves.forward.fill" : "network")
                .foregroundStyle(device.isOnThread ? .blue : .gray)
                .frame(width: 26)
            VStack(alignment: .leading, spacing: 2) {
                Text(device.displayName)
                HStack(spacing: 6) {
                    Text(device.transport.value.label).font(.caption2).foregroundStyle(.secondary)
                    if device.isSleepy { Tag(text: "Battery", color: .teal) }
                    if let room = topology.accessory(device.homeKitAccessoryID)?.roomName {
                        Text(room).font(.caption2).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
}

struct Tag: View {
    let text: String
    let color: Color

    var body: some View {
        Text(text)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(color.opacity(0.15), in: Capsule())
            .foregroundStyle(color)
    }
}

extension String {
    /// `1A2B3C4D` → `1A2B 3C4D`, so long identifiers stay readable.
    var hexGrouped: String {
        stride(from: 0, to: count, by: 4).map { offset in
            let start = index(startIndex, offsetBy: offset)
            let end = index(start, offsetBy: min(4, count - offset))
            return String(self[start..<end])
        }.joined(separator: " ")
    }
}
