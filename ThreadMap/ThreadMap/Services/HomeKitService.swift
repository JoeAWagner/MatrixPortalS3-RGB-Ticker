import Foundation
import HomeKit

/// Reads the HomeKit database: homes, rooms, accessories, bridges.
///
/// HomeKit knows the things the network can't tell us — the name you gave the
/// lock, which room it's in, who made it — but knows nothing about radios. For
/// Matter accessories it also exposes `matterNodeID`, which is the one exact
/// join key we have between HomeKit's world and the mDNS world.
@MainActor
final class HomeKitService {

    enum Authorization: Equatable {
        case determined, restricted, notDetermined

        var isUsable: Bool { self == .determined }
    }

    private var manager: HMHomeManager?
    private var observer: HomeManagerObserver?
    private var loadContinuations: [CheckedContinuation<Void, Never>] = []
    private var hasLoaded = false

    /// Instantiating `HMHomeManager` is what triggers the HomeKit permission
    /// prompt, so we defer it until the user actually asks for a scan.
    func loadAccessories() async -> (accessories: [HomeKitAccessory], authorization: Authorization) {
        let manager = ensureManager()
        await waitForInitialLoad()

        let status = manager.authorizationStatus
        let authorization: Authorization
        if status.contains(.authorized) {
            authorization = .determined
        } else if status.contains(.restricted) {
            authorization = .restricted
        } else {
            authorization = .notDetermined
        }

        var result: [HomeKitAccessory] = []
        for home in manager.homes {
            // Bridged accessories are reported both as children and via their
            // bridge, so build a child -> bridge name index first.
            var bridgeNames: [UUID: String] = [:]
            for accessory in home.accessories {
                for childID in accessory.uniqueIdentifiersForBridgedAccessories ?? [] {
                    bridgeNames[childID] = accessory.name
                }
            }

            for accessory in home.accessories {
                result.append(
                    HomeKitAccessory(
                        id: accessory.uniqueIdentifier,
                        name: accessory.name,
                        homeName: home.name,
                        roomName: accessory.room?.name,
                        manufacturer: accessory.manufacturer,
                        model: accessory.model,
                        firmwareVersion: accessory.firmwareVersion,
                        category: Self.categoryLabel(accessory.category),
                        isReachable: accessory.isReachable,
                        isBridged: bridgeNames[accessory.uniqueIdentifier] != nil,
                        bridgeName: bridgeNames[accessory.uniqueIdentifier],
                        matterNodeIDHex: Self.matterNodeHex(accessory),
                        serviceCount: accessory.services.count
                    )
                )
            }
        }

        result.sort { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        return (result, authorization)
    }

    // MARK: - Plumbing

    private func ensureManager() -> HMHomeManager {
        if let manager { return manager }
        let manager = HMHomeManager()
        let observer = HomeManagerObserver()
        observer.onUpdate = { [weak self] in
            Task { @MainActor in self?.releaseLoadWaiters() }
        }
        manager.delegate = observer
        self.observer = observer
        self.manager = manager
        return manager
    }

    /// `HMHomeManager` reports an empty `homes` array until it calls
    /// `homeManagerDidUpdateHomes(_:)`, so a naive read right after init always
    /// looks like "no accessories". Wait for that callback once.
    private func waitForInitialLoad() async {
        guard !hasLoaded else { return }
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            loadContinuations.append(continuation)
            // Don't hang forever if HomeKit is unavailable on this device.
            Task { [weak self] in
                try? await Task.sleep(for: .seconds(5))
                self?.releaseLoadWaiters()
            }
        }
    }

    private func releaseLoadWaiters() {
        hasLoaded = true
        let waiters = loadContinuations
        loadContinuations.removeAll()
        waiters.forEach { $0.resume() }
    }

    /// `matterNodeID` is an `NSNumber` holding a 64-bit node ID. The mDNS
    /// instance name carries the same value as 16 uppercase hex digits, so
    /// normalise to that form for comparison.
    private static func matterNodeHex(_ accessory: HMAccessory) -> String? {
        guard let node = accessory.matterNodeID else { return nil }
        return String(format: "%016llX", node.uint64Value)
    }

    private static func categoryLabel(_ category: HMAccessoryCategory) -> String {
        let type = category.categoryType
        if !category.localizedDescription.isEmpty { return category.localizedDescription }
        return type
    }
}

/// A plain, un-isolated delegate shim.
///
/// `HomeKitService` is `@MainActor`, and conforming it directly to
/// `HMHomeManagerDelegate` means fighting whichever isolation the HomeKit SDK
/// declares for the protocol. Keeping the delegate as a separate object that
/// simply hops back to the main actor sidesteps that entirely.
private final class HomeManagerObserver: NSObject, HMHomeManagerDelegate, @unchecked Sendable {
    var onUpdate: (() -> Void)?

    func homeManagerDidUpdateHomes(_ manager: HMHomeManager) {
        onUpdate?()
    }

    func homeManager(_ manager: HMHomeManager, didUpdate status: HMHomeManagerAuthorizationStatus) {
        onUpdate?()
    }
}
