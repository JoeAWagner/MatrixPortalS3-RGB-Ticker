import Foundation

/// How sure we are about a piece of derived information.
///
/// The whole point of this app is to never blur the line between something the
/// network actually told us and something we guessed, so every inferred
/// relationship carries one of these plus a human-readable reason.
enum Confidence: Int, Comparable, Hashable, Codable {
    /// Read directly out of a service record, TXT key, or system framework.
    case observed = 3
    /// Derived from an unambiguous join key (OMR prefix match, Matter node ID).
    case derived = 2
    /// A guess from weaker evidence (only one network present, fuzzy name match).
    case inferred = 1
    /// We genuinely don't know.
    case unknown = 0

    static func < (lhs: Confidence, rhs: Confidence) -> Bool { lhs.rawValue < rhs.rawValue }

    var label: String {
        switch self {
        case .observed: "Observed"
        case .derived:  "Derived"
        case .inferred: "Inferred"
        case .unknown:  "Unknown"
        }
    }

    var symbolName: String {
        switch self {
        case .observed: "checkmark.seal.fill"
        case .derived:  "arrow.triangle.branch"
        case .inferred: "questionmark.circle"
        case .unknown:  "questionmark.diamond"
        }
    }
}

/// A value plus the evidence trail that produced it.
struct Attributed<Value>: Hashable where Value: Hashable {
    var value: Value
    var confidence: Confidence
    /// Short sentence explaining *why*, surfaced verbatim in the detail sheet.
    var reason: String

    init(_ value: Value, _ confidence: Confidence, _ reason: String) {
        self.value = value
        self.confidence = confidence
        self.reason = reason
    }
}
