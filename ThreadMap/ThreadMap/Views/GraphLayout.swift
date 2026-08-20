import SwiftUI

/// Positions for everything the map draws.
///
/// The layout is deliberately deterministic — concentric rings around each
/// Thread network — rather than a force simulation. A force-directed graph
/// looks impressive and re-shuffles every scan, which makes it useless for
/// "did that sensor move?". Rings keep a device in the same place run to run.
struct Graph {
    var nodes: [GraphNode] = []
    var edges: [GraphEdge] = []
    var bounds: CGRect = .zero

    func node(_ id: String) -> GraphNode? { nodes.first { $0.id == id } }

    /// Nearest node to a point in graph space, within its own radius plus slop.
    func hitTest(_ point: CGPoint, slop: CGFloat = 12) -> GraphNode? {
        var best: (GraphNode, CGFloat)?
        for node in nodes {
            let distance = hypot(node.position.x - point.x, node.position.y - point.y)
            guard distance <= node.radius + slop else { continue }
            if distance < (best?.1 ?? .greatestFiniteMagnitude) { best = (node, distance) }
        }
        return best?.0
    }
}

struct GraphNode: Identifiable, Hashable {
    enum Kind: Hashable {
        case network(id: String)
        case router(id: String)
        case device(id: String)
        /// A holding pen for devices we couldn't place on any Thread network.
        case cluster(id: String)
    }

    let id: String
    var kind: Kind
    var position: CGPoint
    var radius: CGFloat
    var title: String
    var subtitle: String?
    var symbol: String
    var tint: Color
    /// Drawn as a ring around the node — leader, primary BBR, sleepy, etc.
    var badge: String?
    var isDimmed: Bool = false
}

struct GraphEdge: Hashable {
    let from: String
    let to: String
    var confidence: Confidence
    /// Shown when the user taps the edge's child node.
    var reason: String
}

enum GraphLayout {

    private static let networkRadius: CGFloat = 44
    private static let routerRadius: CGFloat = 30
    private static let deviceRadius: CGFloat = 20
    private static let routerRing: CGFloat = 130
    private static let clusterGap: CGFloat = 120

    static func build(from topology: Topology) -> Graph {
        var graph = Graph()
        var originX: CGFloat = 0

        for network in topology.networks {
            let routers = topology.borderRouters(on: network.id)
            let devices = topology.devices(on: network.id)
            let (nodes, edges, width) = layoutCluster(
                center: CGPoint(x: originX, y: 0),
                hub: hubNode(for: network, routerCount: routers.count, deviceCount: devices.count),
                routers: routers,
                devices: devices,
                topology: topology
            )
            graph.nodes.append(contentsOf: nodes)
            graph.edges.append(contentsOf: edges)
            originX += width + clusterGap
        }

        // Border routers whose network we never identified.
        let orphanRouters = topology.borderRouters.filter { router in
            guard let id = router.networkID else { return true }
            return !topology.networks.contains { $0.id == id }
        }
        // Devices that aren't on Thread, plus Thread devices we couldn't place.
        let offMesh = topology.offMeshDevices
        let unplaced = topology.threadDevices.filter { $0.networkID.value == nil }

        if !orphanRouters.isEmpty || !offMesh.isEmpty || !unplaced.isEmpty {
            let hub = GraphNode(
                id: "cluster:unplaced",
                kind: .cluster(id: "unplaced"),
                position: CGPoint(x: originX, y: 0),
                radius: networkRadius,
                title: "Not on a mapped mesh",
                subtitle: "\(offMesh.count) on IP · \(unplaced.count) unplaced",
                symbol: "questionmark.circle",
                tint: .secondary,
                badge: nil,
                isDimmed: true
            )
            let (nodes, edges, width) = layoutCluster(
                center: CGPoint(x: originX, y: 0),
                hub: hub,
                routers: orphanRouters,
                devices: offMesh + unplaced,
                topology: topology
            )
            graph.nodes.append(contentsOf: nodes)
            graph.edges.append(contentsOf: edges)
            originX += width
        }

        graph.bounds = boundingBox(of: graph.nodes)
        return graph
    }

    // MARK: - Clusters

    private static func hubNode(for network: ThreadNetwork, routerCount: Int, deviceCount: Int) -> GraphNode {
        var subtitleParts: [String] = []
        subtitleParts.append("\(routerCount) router\(routerCount == 1 ? "" : "s")")
        subtitleParts.append("\(deviceCount) device\(deviceCount == 1 ? "" : "s")")
        if let channel = network.channel { subtitleParts.append("ch \(channel)") }

        return GraphNode(
            id: "network:\(network.id)",
            kind: .network(id: network.id),
            position: .zero,
            radius: networkRadius,
            title: network.displayName,
            subtitle: subtitleParts.joined(separator: " · "),
            symbol: "point.3.filled.connected.trianglepath.dotted",
            tint: .accentColor,
            badge: network.hasStoredCredentials ? "key.fill" : nil
        )
    }

    /// Routers ride an inner ring, devices an outer ring sized so they never
    /// collide however many there are.
    private static func layoutCluster(
        center: CGPoint,
        hub: GraphNode,
        routers: [BorderRouter],
        devices: [MeshDevice],
        topology: Topology
    ) -> ([GraphNode], [GraphEdge], CGFloat) {

        var nodes: [GraphNode] = []
        var edges: [GraphEdge] = []

        var hub = hub
        hub.position = center
        nodes.append(hub)

        for (index, router) in routers.enumerated() {
            let angle = ringAngle(index: index, count: routers.count, offset: -.pi / 2)
            let position = CGPoint(x: center.x + cos(angle) * routerRing,
                                   y: center.y + sin(angle) * routerRing)
            nodes.append(routerNode(router, at: position, topology: topology))
            edges.append(
                GraphEdge(from: hub.id, to: "router:\(router.id)", confidence: .observed,
                          reason: "This router advertises itself as a border agent for this network.")
            )
        }

        // Enough circumference that every device gets breathing room.
        let minimumSpacing = deviceRadius * 3.4
        let deviceRing = max(routerRing + 110,
                             CGFloat(max(devices.count, 1)) * minimumSpacing / (2 * .pi))

        for (index, device) in devices.enumerated() {
            let angle = ringAngle(index: index, count: devices.count, offset: -.pi / 2 + 0.18)
            let position = CGPoint(x: center.x + cos(angle) * deviceRing,
                                   y: center.y + sin(angle) * deviceRing)
            nodes.append(deviceNode(device, at: position, topology: topology))
            edges.append(
                GraphEdge(from: hub.id, to: "device:\(device.id)",
                          confidence: device.networkID.confidence,
                          reason: device.networkID.reason)
            )
        }

        return (nodes, edges, deviceRing * 2)
    }

    private static func ringAngle(index: Int, count: Int, offset: CGFloat) -> CGFloat {
        guard count > 0 else { return offset }
        return offset + (CGFloat(index) / CGFloat(count)) * 2 * .pi
    }

    // MARK: - Node builders

    private static func routerNode(_ router: BorderRouter, at position: CGPoint, topology: Topology) -> GraphNode {
        var subtitle = router.hardwareDescription
        if let room = topology.accessory(router.homeKitAccessoryID)?.roomName {
            subtitle = room
        }
        var badge: String?
        if router.isLeader { badge = "crown.fill" }
        else if router.isPrimaryBackboneRouter { badge = "star.fill" }

        return GraphNode(
            id: "router:\(router.id)",
            kind: .router(id: router.id),
            position: position,
            radius: routerRadius,
            title: router.displayName,
            subtitle: subtitle,
            symbol: "wifi.router.fill",
            tint: router.isAttached ? .green : .orange,
            badge: badge,
            isDimmed: !router.isAttached
        )
    }

    private static func deviceNode(_ device: MeshDevice, at position: CGPoint, topology: Topology) -> GraphNode {
        let accessory = topology.accessory(device.homeKitAccessoryID)
        let tint: Color = switch device.transport.value {
        case .thread:         .blue
        case .wifiOrEthernet: .gray
        case .unknown:        .yellow
        }

        return GraphNode(
            id: "device:\(device.id)",
            kind: .device(id: device.id),
            position: position,
            radius: deviceRadius,
            title: device.displayName,
            subtitle: accessory?.roomName ?? device.kindLabel,
            symbol: symbolName(for: device, accessory: accessory),
            tint: tint,
            badge: device.isSleepy ? "moon.zzz.fill" : nil,
            isDimmed: device.networkID.confidence <= .inferred
        )
    }

    private static func symbolName(for device: MeshDevice, accessory: HomeKitAccessory?) -> String {
        switch device.hap?.categoryIdentifier ?? 0 {
        case 2:      return "square.stack.3d.up"
        case 3:      return "fan.fill"
        case 4:      return "door.garage.closed"
        case 5:      return "lightbulb.fill"
        case 6:      return "lock.fill"
        case 7:      return "powerplug.fill"
        case 8:      return "switch.2"
        case 9:      return "thermometer"
        case 10:     return "sensor.fill"
        case 11:     return "shield.lefthalf.filled"
        case 12, 13: return "door.left.hand.closed"
        case 14:     return "blinds.horizontal.closed"
        case 15:     return "button.programmable"
        case 17, 18: return "video.fill"
        case 28, 29: return "drop.fill"
        default:     break
        }
        if device.matter != nil { return "shippingbox.fill" }
        return "sensor.tag.radiowaves.forward.fill"
    }

    private static func boundingBox(of nodes: [GraphNode]) -> CGRect {
        guard let first = nodes.first else { return .zero }
        var minX = first.position.x, maxX = first.position.x
        var minY = first.position.y, maxY = first.position.y
        for node in nodes {
            minX = min(minX, node.position.x - node.radius)
            maxX = max(maxX, node.position.x + node.radius)
            minY = min(minY, node.position.y - node.radius)
            maxY = max(maxY, node.position.y + node.radius)
        }
        // Room for the labels that hang below each node.
        return CGRect(x: minX - 60, y: minY - 40, width: (maxX - minX) + 120, height: (maxY - minY) + 110)
    }
}
