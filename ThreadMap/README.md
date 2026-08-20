# Thread Map

An iPhone app that discovers the **Thread border routers** on your home network,
the **Thread and HomeKit devices** behind them, and draws the relationship
between the two.

> **Status:** complete source, never compiled. It was written in a Linux
> container with no Swift toolchain, so treat the first `xcodegen && build` as
> the real smoke test. Two SDK details flagged in
> [Before you build](#before-you-build) are the likeliest places it trips.

---

## Read this first: what an iOS app can and cannot see

This is the part most "Thread scanner" projects get wrong, so it's worth being
blunt.

**iOS exposes no API for Thread mesh topology.** Not to third-party apps, not
through HomeKit, not through `ThreadNetwork.framework`. There is no supported
way for any App Store app to see:

- which router a given end device attached to (its *parent*)
- whether a device is acting as a Router, a REED, or an End Device
- link quality, RSSI, LQI, or hop count
- the neighbour table, the routing table, or the network's child table

That data lives inside the border routers and Apple's private daemons. If an app
draws you a line from a light bulb to a specific HomePod, it made that line up.

**What is genuinely reachable**, and what this app is built on:

| Source | What it gives you |
|---|---|
| `_meshcop._udp` mDNS | Every Thread Border Router on the Wi-Fi link, announcing its network name, extended PAN ID, border agent ID, Thread version, partition ID, mesh role, backbone-router status, and (Thread 1.4+) its off-mesh-routable prefix |
| `_hap._udp` mDNS | HomeKit accessories on Thread. HAP-over-CoAP is *only* used on Thread, so this is proof of Thread membership, not a guess |
| `_matter._tcp` / `_matterc._udp` mDNS | Matter nodes, their node ID, vendor/product, and sleepy-device (ICD) profile |
| `_trel._udp` mDNS | Routers that support Thread Radio Encapsulation Link |
| `ThreadNetwork.framework` | Names, channels and PAN IDs of Thread networks stored on *this iPhone* |
| `HomeKit.framework` | Your homes, rooms, accessory names, manufacturers, models, reachability, and — for Matter accessories — the `matterNodeID` |

Thread devices show up on Wi-Fi at all because border routers run an SRP server
plus an *advertising proxy*: devices register their services with the border
router over Thread, and the border router republishes them as mDNS on the
infrastructure link. That indirection is exactly why you can enumerate the
devices and exactly why you can't see the links between them.

### So what does the map actually show?

Three layers, and every edge is labelled with how it was established:

```
Thread network  ──  border routers serving it     (observed: the router says so)
       │
       └────────  devices on it                   (derived or inferred, see below)
```

A device is placed on a network by one of three rules, strongest first:

1. **Derived — address match.** The device's IPv6 address falls inside an
   off-mesh-routable prefix that a border router advertises in its `omr` TXT
   key. This is exact.
2. **Inferred — only one network.** The device is on Thread and exactly one
   Thread network is visible, so it must be that one.
3. **Unknown.** Several networks are visible and nothing ties the device to one.

Tap any device and the detail sheet tells you which rule fired and why, in
words. Inferred edges are drawn dashed. That honesty is the point of the app —
a map that silently guesses is worse than no map.

---

## Building it

```bash
brew install xcodegen
cd ThreadMap
xcodegen generate
open ThreadMap.xcodeproj
```

The `.xcodeproj` is generated from `project.yml` and deliberately not committed,
so the file list can't drift from what's on disk. If you'd rather not use
XcodeGen: make a new iOS App target in Xcode, drag in the `ThreadMap/` folder,
and point Build Settings at `Info.plist` and `ThreadMap.entitlements`.

**Deployment target is iOS 17.0** (`@Observable`, `MagnifyGesture`,
`ContentUnavailableView`).

### It must run on a real device

The Simulator does not join your Wi-Fi network's multicast group in a way that
surfaces border routers, and HomeKit in the Simulator is a stub. A scan on the
Simulator returns approximately nothing. Use hardware, on Wi-Fi, on the same
VLAN as your hubs — not cellular, not a guest network with client isolation.

### Before you build

Two API details were written from documentation and could not be checked
against a real SDK here. If the build fails, look at these first:

1. **`THClient.retrieveAllActiveCredentials()`**
   (`Services/ThreadCredentialsService.swift`). Some SDK versions spell the bulk
   accessor `retrieveAllCredentials()`. If the selector doesn't resolve, swap it
   — the return type and use site are unchanged.
2. **`THCredentials` optionality.** The service assumes `extendedPANID`,
   `borderAgentID` and `panID` are `Data?` and uses `.map(hex)`. If your SDK
   declares them non-optional, drop the `.map` and call `hex(...)` directly.

Neither affects anything but network *naming* — see the entitlement note below.

### Entitlements

| Entitlement | Needed for | If you don't have it |
|---|---|---|
| `com.apple.developer.homekit` | Accessory names, rooms, `matterNodeID` | Devices appear as bare Matter node IDs and hostnames; everything else works |
| `com.apple.developer.networking.manage-thread-network-credentials` | Reading stored Thread networks | Networks show as `Thread network 1A2B…` instead of `My Home`; **all** router and device discovery still works |

The Thread credentials entitlement is granted by Apple on request via the
[Thread network credentials request form][thread-form]. **The app is built to
run without it** — remove the key from `ThreadMap.entitlements` and it degrades
to a clearly-labelled reduced mode rather than failing. It also offers
`retrievePreferredCredentials()`, which shows a system picker and needs no bulk
entitlement, as a fallback path.

You do **not** need the multicast entitlement
(`com.apple.developer.networking.multicast`). That one is for raw multicast
sockets; Bonjour browsing via `NWBrowser` and `dnssd` doesn't require it.

[thread-form]: https://developer.apple.com/contact/request/thread-network

### Permissions the user grants at runtime

- **Local Network** — without it, `NWBrowser` returns zero results *silently*.
  The app detects the policy-denied state and says so rather than showing an
  empty map.
- **HomeKit** — prompted the first time a scan touches `HMHomeManager`.

Every service type the app browses is listed in `NSBonjourServices` in
`Info.plist`. Anything missing from that array is invisible at runtime, so add
a type there whenever you add one to `ServiceType`.

---

## How it's put together

```
Model/
  Confidence.swift      Attributed<T> — a value plus how we know it
  IPv6Prefix.swift      IPAddress, IPv6Prefix, OMR prefix containment
  ServiceRecord.swift   ServiceType (the seven types we browse), raw mDNS record
  MeshcopRecord.swift   Thread border agent TXT decoding, incl. the sb bitmap
  MatterRecord.swift    Matter operational + commissionable TXT decoding
  HAPRecord.swift       HomeKit accessory TXT decoding, category table
  Topology.swift        ThreadNetwork / BorderRouter / MeshDevice / HomeKitAccessory

Services/
  BonjourScanner.swift          NWBrowser across all seven service types
  DNSSDResolver.swift           dnssd resolve + address enumeration
  IPAddressParser.swift         sockaddr ↔ IPAddress
  ThreadCredentialsService.swift THClient wrapper (non-secret fields only)
  HomeKitService.swift          HMHomeManager wrapper
  TopologyBuilder.swift         the correlation engine

ViewModel/ScannerModel.swift    scan orchestration, phases, notices
Views/                          map canvas, layout, inventory, detail, diagnostics
```

Two implementation notes worth knowing:

**Why `dnssd` and not just `NWBrowser`.** `NWBrowser` hands you an opaque
endpoint and hides IP addresses. For Thread mapping the addresses *are* the
payload — matching a device's IPv6 address against a border router's advertised
OMR prefix is the only exact way to place a device on a network. So discovery
uses `NWBrowser` (nice API, gives TXT) and resolution drops to the `dnssd` C
API, which returns every A/AAAA record a host publishes. A Thread device
typically has three: link-local, mesh-local, and OMR.

**Why the layout is rings and not a force simulation.** A force-directed graph
re-shuffles on every scan, which makes "did that sensor move?" unanswerable.
Concentric rings keep a device in the same place run to run.

### Correlating mDNS with HomeKit

HomeKit never exposes an accessory's HAP pairing ID, so there's no exact join
key for HomeKit-over-Thread accessories. The builder does two passes:

1. **Exact** — `HMAccessory.matterNodeID` against the node ID in the
   `_matter._tcp` instance name. Byte-for-byte; no guessing.
2. **Fuzzy** — normalised name similarity with a matching model as a tiebreaker,
   thresholded at 0.72, each accessory claimable once. Names dominate
   deliberately: five identical bulbs would otherwise all score the same on
   model alone.

Accessories HomeKit knows about but the network never showed get their own
section in the inventory, rather than being quietly dropped.

---

## Things it deliberately does not do

- **Never reads or displays Thread key material.** `networkKey`, `PSKC` and
  `activeOperationalDataSet` are never pulled out of `THCredentials`;
  `ThreadCredentialsService.summarize` is written as an exhaustive field list so
  that's checkable at review time.
- **Never writes anything.** No commissioning, no HomeKit mutation, read-only
  throughout.
- **Never leaves the device.** No network calls beyond local mDNS.

## Where you could take it next

- **Partition-split detection.** Two routers sharing an extended PAN ID but
  reporting different partition IDs means the mesh has split. The data is
  already parsed and shown per-router; flagging it on the map would be a genuinely
  useful diagnostic that nothing else surfaces.
- **Change tracking.** Persist each scan and diff them — "this sensor stopped
  answering three days ago" is the question people actually have.
- **Which router proxied a device.** Answering this for real means sending raw
  mDNS queries via `NWConnectionGroup` and recording the *source address* of
  each response, which tells you which border router answered for a device.
  It's the closest supported thing to a device→router edge. It needs the
  multicast entitlement and careful handling of routers that all answer.
- **Commissioning.** `MatterSupport` can add a device to a fabric; combined with
  `retrievePreferredCredentials()` this could become a setup tool, not just a
  viewer.
