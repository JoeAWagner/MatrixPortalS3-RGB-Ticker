import Foundation
import Observation

/// Drives a scan and owns the resulting topology.
@MainActor
@Observable
final class ScannerModel {

    enum Phase: Equatable {
        case idle
        case browsing
        case readingThreadCredentials
        case readingHomeKit
        case correlating

        var label: String {
            switch self {
            case .idle:                     "Ready"
            case .browsing:                 "Listening for Thread border routers…"
            case .readingThreadCredentials: "Reading stored Thread networks…"
            case .readingHomeKit:           "Reading your HomeKit homes…"
            case .correlating:              "Matching devices to networks…"
            }
        }
    }

    /// Something the user can act on, kept separate from fatal errors because
    /// almost every one of these is a permission the app can survive without.
    struct Notice: Identifiable, Hashable {
        enum Severity { case info, warning }
        let id = UUID()
        var severity: Severity
        var title: String
        var detail: String
    }

    private(set) var phase: Phase = .idle
    private(set) var topology = Topology()
    private(set) var notices: [Notice] = []
    private(set) var lastScanDate: Date?
    private(set) var hasScannedOnce = false

    var isScanning: Bool { phase != .idle }

    /// Longer browse windows find more sleepy devices, which only wake every
    /// few seconds, at the cost of a slower scan.
    var browseDuration: TimeInterval = 5.0

    private let scanner = BonjourScanner()
    private let credentials = ThreadCredentialsService()
    private let homeKit = HomeKitService()
    private let builder = TopologyBuilder()
    private var scanTask: Task<Void, Never>?

    func scan() {
        guard scanTask == nil else { return }
        scanTask = Task { [weak self] in
            await self?.runScan()
            self?.scanTask = nil
        }
    }

    func cancel() {
        scanTask?.cancel()
        scanTask = nil
        phase = .idle
    }

    private func runScan() async {
        notices.removeAll()

        phase = .browsing
        let scan = await scanner.scan(browseDuration: browseDuration)
        for issue in scan.issues {
            notices.append(Notice(severity: .warning, title: "Discovery problem", detail: issue.message))
        }
        guard !Task.isCancelled else { phase = .idle; return }

        phase = .readingThreadCredentials
        var knownNetworks: [ThreadCredentialsService.KnownNetwork] = []
        do {
            knownNetworks = try await credentials.knownNetworks()
        } catch {
            notices.append(
                Notice(severity: .info,
                       title: "Thread network names unavailable",
                       detail: (error as? LocalizedError)?.errorDescription ?? error.localizedDescription)
            )
        }
        guard !Task.isCancelled else { phase = .idle; return }

        phase = .readingHomeKit
        let (accessories, authorization) = await homeKit.loadAccessories()
        if !authorization.isUsable {
            notices.append(
                Notice(severity: .warning,
                       title: "HomeKit access is off",
                       detail: "Without it, Thread devices show up as bare Matter node IDs instead of the names you gave them. Turn it on in Settings › Privacy & Security › HomeKit.")
            )
        }
        guard !Task.isCancelled else { phase = .idle; return }

        phase = .correlating
        topology = builder.build(
            TopologyBuilder.Input(records: scan.records,
                                  knownNetworks: knownNetworks,
                                  accessories: accessories)
        )

        addInterpretationNotices()
        lastScanDate = .now
        hasScannedOnce = true
        phase = .idle
    }

    /// Explains the map's blind spots in place, rather than letting the user
    /// assume an empty area means "nothing there".
    private func addInterpretationNotices() {
        if topology.borderRouters.isEmpty {
            notices.append(
                Notice(severity: .warning,
                       title: "No Thread border routers found",
                       detail: "Border routers announce themselves over Wi-Fi. Make sure this iPhone is on the same Wi-Fi network (not cellular, not a guest VLAN) as your HomePods, Apple TVs, or hubs.")
            )
        }

        let unplaced = topology.threadDevices.filter { $0.networkID.value == nil }
        if !unplaced.isEmpty {
            notices.append(
                Notice(severity: .info,
                       title: "\(unplaced.count) Thread \(unplaced.count == 1 ? "device" : "devices") not placed",
                       detail: "Their addresses don't fall inside any prefix a border router advertises. This is normal for older (Thread 1.3) border routers, which don't publish their routable prefix.")
            )
        }

        if !topology.borderRouters.isEmpty {
            notices.append(
                Notice(severity: .info,
                       title: "About device-to-router links",
                       detail: "iOS exposes no API for Thread mesh topology, so no app can show which router a given device is attached to, or its signal strength. Devices are placed on their Thread network; the routers shown are all the routers serving that network.")
            )
        }
    }
}
