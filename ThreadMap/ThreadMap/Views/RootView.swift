import SwiftUI

struct RootView: View {
    @State private var model = ScannerModel()
    @State private var selection: MapSelection?
    @State private var tab: Tab = .map

    enum Tab: Hashable { case map, inventory, diagnostics }

    var body: some View {
        TabView(selection: $tab) {
            NavigationStack {
                mapTab
            }
            .tabItem { Label("Map", systemImage: "point.3.connected.trianglepath.dotted") }
            .tag(Tab.map)

            NavigationStack {
                InventoryView(topology: model.topology)
                    .navigationTitle("Devices")
                    .toolbar { scanButton }
            }
            .tabItem { Label("Devices", systemImage: "list.bullet") }
            .tag(Tab.inventory)

            NavigationStack {
                DiagnosticsView(topology: model.topology,
                                notices: model.notices,
                                lastScanDate: model.lastScanDate)
                    .toolbar { scanButton }
            }
            .tabItem { Label("Diagnostics", systemImage: "stethoscope") }
            .tag(Tab.diagnostics)
            .badge(model.notices.filter { $0.severity == .warning }.count)
        }
        .task {
            // Kick off the first scan automatically; permission prompts appear
            // as each subsystem is touched, which reads better than three
            // dialogs stacked up on launch.
            if !model.hasScannedOnce { model.scan() }
        }
    }

    private var mapTab: some View {
        ZStack {
            MapView(topology: model.topology, selection: $selection)

            if model.isScanning {
                ScanOverlay(phase: model.phase)
                    .transition(.opacity)
            } else if !model.hasScannedOnce {
                ContentUnavailableView {
                    Label("No scan yet", systemImage: "antenna.radiowaves.left.and.right")
                } description: {
                    Text("Find the Thread border routers on your Wi-Fi and the devices behind them.")
                } actions: {
                    Button("Scan") { model.scan() }.buttonStyle(.borderedProminent)
                }
                .background(.regularMaterial)
            }
        }
        .animation(.easeInOut(duration: 0.2), value: model.isScanning)
        .navigationTitle("Thread Map")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                if let date = model.lastScanDate {
                    Text(date, style: .relative)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            scanButton
        }
        .sheet(item: $selection) { selection in
            SelectionDetailView(selection: selection, topology: model.topology)
                .presentationDetents([.medium, .large])
                .presentationDragIndicator(.visible)
        }
    }

    @ToolbarContentBuilder
    private var scanButton: some ToolbarContent {
        ToolbarItem(placement: .topBarTrailing) {
            Button {
                model.isScanning ? model.cancel() : model.scan()
            } label: {
                if model.isScanning {
                    Image(systemName: "stop.circle")
                } else {
                    Image(systemName: "arrow.clockwise")
                }
            }
            .accessibilityLabel(model.isScanning ? "Stop scanning" : "Scan again")
        }
    }
}

private struct ScanOverlay: View {
    let phase: ScannerModel.Phase

    var body: some View {
        VStack(spacing: 12) {
            ProgressView()
            Text(phase.label)
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .padding(24)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }
}
