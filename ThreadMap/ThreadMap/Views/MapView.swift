import SwiftUI

/// The map: Thread networks as hubs, their border routers on an inner ring,
/// their devices on an outer ring.
struct MapView: View {
    let topology: Topology
    @Binding var selection: MapSelection?

    @State private var graph = Graph()
    @State private var scale: CGFloat = 1
    @State private var committedScale: CGFloat = 1
    @State private var offset: CGSize = .zero
    @State private var committedOffset: CGSize = .zero
    @State private var hasFitted = false
    @State private var showLegend = true

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .bottomLeading) {
                Canvas { context, size in
                    draw(in: &context, size: size)
                }
                .contentShape(Rectangle())
                .gesture(dragGesture)
                .simultaneousGesture(zoomGesture)
                .simultaneousGesture(tapGesture(viewSize: proxy.size))
                .onAppear { fitIfNeeded(in: proxy.size) }
                .onChange(of: proxy.size) { _, newValue in
                    hasFitted = false
                    fitIfNeeded(in: newValue)
                }

                if showLegend { legend.padding(16) }
            }
            .overlay(alignment: .topTrailing) { controls.padding(16) }
        }
        .background(Color(.systemGroupedBackground))
        .onAppear { graph = GraphLayout.build(from: topology) }
        .onChange(of: topology) { _, newValue in
            graph = GraphLayout.build(from: newValue)
            hasFitted = false
        }
    }

    // MARK: - Drawing

    private func draw(in context: inout GraphicsContext, size: CGSize) {
        guard !graph.nodes.isEmpty else {
            let text = Text("Nothing discovered yet.\nTap Scan to look again.")
                .font(.callout)
                .foregroundStyle(.secondary)
            context.draw(context.resolve(text), at: CGPoint(x: size.width / 2, y: size.height / 2))
            return
        }

        let transform = viewTransform(size: size)

        // Edges first so nodes sit on top of them.
        for edge in graph.edges {
            guard let from = graph.node(edge.from), let to = graph.node(edge.to) else { continue }
            var path = Path()
            path.move(to: transform(from.position))
            path.addLine(to: transform(to.position))

            let isCertain = edge.confidence >= .derived
            let style = StrokeStyle(
                lineWidth: isCertain ? 1.6 : 1.2,
                lineCap: .round,
                dash: isCertain ? [] : [4, 5]
            )
            context.stroke(path, with: .color(.secondary.opacity(isCertain ? 0.45 : 0.28)), style: style)
        }

        for node in graph.nodes {
            drawNode(node, in: &context, transform: transform)
        }
    }

    private func drawNode(_ node: GraphNode, in context: inout GraphicsContext, transform: (CGPoint) -> CGPoint) {
        let center = transform(node.position)
        let radius = node.radius * scale
        let isSelected = selection?.nodeID == node.id
        let opacity = node.isDimmed ? 0.55 : 1.0

        let circle = Path(ellipseIn: CGRect(x: center.x - radius, y: center.y - radius,
                                            width: radius * 2, height: radius * 2))
        context.fill(circle, with: .color(node.tint.opacity(0.16 * opacity)))
        context.stroke(circle,
                       with: .color(node.tint.opacity(isSelected ? 1.0 : 0.7 * opacity)),
                       lineWidth: isSelected ? 3 : 1.6)

        // Embedding the symbol in a Text is what lets us tint and size it
        // inside a Canvas — a resolved Image can't carry a foreground style.
        let symbol = Text(Image(systemName: node.symbol))
            .font(.system(size: max(9, radius * 0.85)))
            .foregroundStyle(node.tint.opacity(opacity))
        context.draw(context.resolve(symbol), at: center)

        if let badge = node.badge {
            let badgeText = Text(Image(systemName: badge))
                .font(.system(size: max(7, radius * 0.42)))
                .foregroundStyle(.orange)
            let corner = CGPoint(x: center.x + radius * 0.72, y: center.y - radius * 0.72)
            context.draw(context.resolve(badgeText), at: corner)
        }

        // Labels get illegible below ~55% zoom, so drop them rather than
        // turning the map into grey mush.
        guard scale > 0.55 else { return }

        let title = Text(node.title)
            .font(.system(size: max(8, 11 * min(scale, 1.4)), weight: .medium))
            .foregroundStyle(Color.primary.opacity(opacity))
        context.draw(context.resolve(title),
                     at: CGPoint(x: center.x, y: center.y + radius + 11 * min(scale, 1.4)))

        if let subtitle = node.subtitle, scale > 0.85 {
            let caption = Text(subtitle)
                .font(.system(size: max(7, 9 * min(scale, 1.4))))
                .foregroundStyle(.secondary)
            context.draw(context.resolve(caption),
                         at: CGPoint(x: center.x, y: center.y + radius + 24 * min(scale, 1.4)))
        }
    }

    // MARK: - Transform

    private func viewTransform(size: CGSize) -> (CGPoint) -> CGPoint {
        let graphCenter = CGPoint(x: graph.bounds.midX, y: graph.bounds.midY)
        let viewCenter = CGPoint(x: size.width / 2, y: size.height / 2)
        let scale = self.scale
        let offset = self.offset
        return { point in
            CGPoint(
                x: (point.x - graphCenter.x) * scale + viewCenter.x + offset.width,
                y: (point.y - graphCenter.y) * scale + viewCenter.y + offset.height
            )
        }
    }

    private func graphPoint(from viewPoint: CGPoint, size: CGSize) -> CGPoint {
        let graphCenter = CGPoint(x: graph.bounds.midX, y: graph.bounds.midY)
        let viewCenter = CGPoint(x: size.width / 2, y: size.height / 2)
        return CGPoint(
            x: (viewPoint.x - viewCenter.x - offset.width) / scale + graphCenter.x,
            y: (viewPoint.y - viewCenter.y - offset.height) / scale + graphCenter.y
        )
    }

    private func fitIfNeeded(in size: CGSize) {
        guard !hasFitted, size.width > 0, size.height > 0,
              graph.bounds.width > 0, graph.bounds.height > 0 else { return }
        let fit = min(size.width / graph.bounds.width, size.height / graph.bounds.height)
        scale = min(max(fit * 0.92, 0.25), 1.6)
        committedScale = scale
        offset = .zero
        committedOffset = .zero
        hasFitted = true
    }

    // MARK: - Gestures

    private var dragGesture: some Gesture {
        DragGesture()
            .onChanged { value in
                offset = CGSize(width: committedOffset.width + value.translation.width,
                                height: committedOffset.height + value.translation.height)
            }
            .onEnded { _ in committedOffset = offset }
    }

    private var zoomGesture: some Gesture {
        MagnifyGesture()
            .onChanged { value in
                scale = min(max(committedScale * value.magnification, 0.2), 4.0)
            }
            .onEnded { _ in committedScale = scale }
    }

    private func tapGesture(viewSize: CGSize) -> some Gesture {
        SpatialTapGesture()
            .onEnded { value in
                let point = graphPoint(from: value.location, size: viewSize)
                if let node = graph.hitTest(point, slop: 14 / max(scale, 0.2)) {
                    selection = MapSelection(node: node)
                } else {
                    selection = nil
                }
            }
    }

    // MARK: - Chrome

    private var controls: some View {
        VStack(spacing: 8) {
            Button {
                withAnimation(.easeInOut) { hasFitted = false; scale = 1; offset = .zero; committedOffset = .zero; committedScale = 1 }
            } label: {
                Image(systemName: "arrow.up.left.and.down.right.magnifyingglass")
            }
            .accessibilityLabel("Fit map to screen")

            Button {
                withAnimation { showLegend.toggle() }
            } label: {
                Image(systemName: showLegend ? "list.bullet.rectangle.fill" : "list.bullet.rectangle")
            }
            .accessibilityLabel(showLegend ? "Hide legend" : "Show legend")
        }
        .buttonStyle(.bordered)
        .buttonBorderShape(.circle)
    }

    private var legend: some View {
        VStack(alignment: .leading, spacing: 6) {
            LegendRow(color: .accentColor, symbol: "point.3.filled.connected.trianglepath.dotted", text: "Thread network")
            LegendRow(color: .green, symbol: "wifi.router.fill", text: "Border router (attached)")
            LegendRow(color: .blue, symbol: "sensor.tag.radiowaves.forward.fill", text: "Thread device")
            LegendRow(color: .gray, symbol: "network", text: "Wi-Fi / Ethernet device")
            HStack(spacing: 6) {
                DashedLine().stroke(style: StrokeStyle(lineWidth: 1.2, dash: [4, 5]))
                    .foregroundStyle(.secondary)
                    .frame(width: 18, height: 1)
                Text("Inferred link").font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(10)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }
}

private struct LegendRow: View {
    let color: Color
    let symbol: String
    let text: String

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: symbol).font(.caption2).foregroundStyle(color).frame(width: 18)
            Text(text).font(.caption2).foregroundStyle(.secondary)
        }
    }
}

private struct DashedLine: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX, y: rect.midY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.midY))
        return path
    }
}

/// What the user tapped, in a form the detail sheet can resolve.
struct MapSelection: Identifiable, Hashable {
    let id = UUID()
    let nodeID: String
    let kind: GraphNode.Kind
    let title: String

    init(node: GraphNode) {
        self.nodeID = node.id
        self.kind = node.kind
        self.title = node.title
    }
}
