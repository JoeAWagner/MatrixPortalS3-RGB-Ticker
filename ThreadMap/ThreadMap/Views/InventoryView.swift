import SwiftUI

/// The flat, searchable version of the map. Grouped by Thread network so the
/// "which of my routers is this on" question is answerable without pinching.
struct InventoryView: View {
    let topology: Topology
    @State private var query = ""
    @State private var filter: Filter = .all

    enum Filter: String, CaseIterable, Identifiable {
        case all = "All"
        case thread = "Thread"
        case battery = "Battery"
        case unplaced = "Unplaced"

        var id: String { rawValue }
    }

    var body: some View {
        List {
            if topology.isEmpty {
                ContentUnavailableView("Nothing scanned yet",
                                       systemImage: "antenna.radiowaves.left.and.right",
                                       description: Text("Run a scan to build your inventory."))
            }

            ForEach(topology.networks) { network in
                let devices = filtered(topology.devices(on: network.id))
                let routers = topology.borderRouters(on: network.id)
                if !devices.isEmpty || !routers.isEmpty || query.isEmpty {
                    Section {
                        ForEach(routers) { router in
                            NavigationLink {
                                RouterDetailView(router: router, topology: topology)
                            } label: {
                                RouterRow(router: router, topology: topology)
                            }
                        }
                        ForEach(devices) { device in
                            NavigationLink {
                                DeviceDetailView(device: device, topology: topology)
                            } label: {
                                DeviceRow(device: device, topology: topology)
                            }
                        }
                    } header: {
                        NavigationLink {
                            NetworkDetailView(network: network, topology: topology)
                        } label: {
                            HStack {
                                Text(network.displayName)
                                Spacer()
                                Text("\(routers.count) routers · \(topology.devices(on: network.id).count) devices")
                                    .font(.caption2)
                            }
                        }
                    }
                }
            }

            let leftovers = filtered(topology.devices.filter { $0.networkID.value == nil })
            if !leftovers.isEmpty {
                Section("Not on a mapped network") {
                    ForEach(leftovers) { device in
                        NavigationLink {
                            DeviceDetailView(device: device, topology: topology)
                        } label: {
                            DeviceRow(device: device, topology: topology)
                        }
                    }
                }
            }

            let unmatched = topology.accessories.filter { accessory in
                !topology.devices.contains { $0.homeKitAccessoryID == accessory.id }
                    && !topology.borderRouters.contains { $0.homeKitAccessoryID == accessory.id }
            }
            if !unmatched.isEmpty, query.isEmpty, filter == .all {
                Section {
                    ForEach(unmatched) { accessory in
                        VStack(alignment: .leading, spacing: 2) {
                            Text(accessory.name)
                            Text([accessory.roomName, accessory.manufacturer, accessory.category]
                                .compactMap { $0 }.joined(separator: " · "))
                                .font(.caption2).foregroundStyle(.secondary)
                        }
                    }
                } header: {
                    Text("In HomeKit, not seen on the network (\(unmatched.count))")
                } footer: {
                    Text("Usually Bluetooth or bridged accessories, anything behind a hub that doesn't republish it, or a device that's currently offline.")
                }
            }
        }
        .searchable(text: $query, prompt: "Search devices, rooms, models")
        .safeAreaInset(edge: .top) {
            Picker("Filter", selection: $filter) {
                ForEach(Filter.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented)
            .padding(.horizontal)
            .padding(.bottom, 8)
            .background(.bar)
        }
    }

    private func filtered(_ devices: [MeshDevice]) -> [MeshDevice] {
        devices.filter { device in
            switch filter {
            case .all:      true
            case .thread:   device.isOnThread
            case .battery:  device.isSleepy
            case .unplaced: device.networkID.value == nil
            }
        }
        .filter { device in
            guard !query.isEmpty else { return true }
            let accessory = topology.accessory(device.homeKitAccessoryID)
            let haystack = [
                device.displayName,
                device.kindLabel,
                device.hostname ?? "",
                accessory?.roomName ?? "",
                accessory?.manufacturer ?? "",
                accessory?.model ?? "",
                device.hap?.model ?? ""
            ].joined(separator: " ")
            return haystack.localizedCaseInsensitiveContains(query)
        }
    }
}
