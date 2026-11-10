// App state. All mutation happens on the main actor; blocking work (MD5
// verification, PIT parsing) runs on a background queue.
import AppKit
import Foundation

enum FlashStep: Int, CaseIterable, Identifiable {
    case setup, review, flash
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .setup: return "Setup"
        case .review: return "Review & Confirm"
        case .flash: return "Flash"
        }
    }
}

/// User preferences (Settings). Read at the moment of use.
enum Prefs {
    static let reboot = "rebootByDefault"
    static let showGuide = "showDownloadGuide"
    static let showHandshake = "showHandshakeOverride"
    static let historyLimit = "historyLimit"

    static var rebootDefault: Bool { UserDefaults.standard.object(forKey: reboot) as? Bool ?? true }
    /// 0 keeps every entry.
    static var historyKeep: Int { UserDefaults.standard.object(forKey: historyLimit) as? Int ?? 200 }
}

/// A firmware file chosen for a slot, remembered with a job so it can be retried.
struct HistoryPath: Codable, Hashable {
    var slot: Slot
    var path: String
}

enum AgentState: Equatable {
    case connecting
    case connected
    case unavailable(String)
}

/// A device job as the UI sees it (live, or reattached after a relaunch).
struct JobState {
    enum Purpose { case flash, backupPit(URL), probe }
    var id: UInt64
    var kind: JobKind
    var purpose: Purpose
    var running = true
    var progress = JobProgress()
    var plan: Plan?
    var log: [LogLine] = []
    var result: JobResult?
    var startedAt = Date()
    /// Captured when the job starts: the selection can change before it ends.
    var device: DeviceID?
    var packagePaths: [HistoryPath] = []
    var bytesPerSecond: Double = 0
    fileprivate var lastSample: (date: Date, bytes: UInt64)?

    var secondsRemaining: Double? {
        guard bytesPerSecond > 0, progress.total > progress.totalDone else { return nil }
        return Double(progress.total - progress.totalDone) / bytesPerSecond
    }
}

struct HistoryEntry: Identifiable, Codable, Hashable {
    var id = UUID()
    var date: Date
    var kind: JobKind
    var device: String
    var model: String
    var files: [String]
    var error: BifrostError?
    var bytesTransferred: UInt64
    var session: SessionInfo
    var log: [LogLine]
    // Added later; optional so older history files still load.
    var duration: Double?
    var packagePaths: [HistoryPath]?

    var succeeded: Bool { error == nil }
}

@MainActor
final class AppModel: ObservableObject {
    // Agent and devices
    @Published var agentState: AgentState = .connecting
    @Published var devices: [Device] = []
    @Published var selectedDevice: DeviceID?
    /// True when a session already ran in this Download Mode entry, so the next
    /// one must skip the handshake (--resume). Heuristic, user-overridable.
    @Published var handshakeDone = false
    @Published var deviceMessage: String?

    // Firmware
    @Published var paths: [Slot: String] = [:]
    @Published var packages: LoadedPackages?
    @Published var loadingProgress: JobProgress?
    @Published var firmwareError: BifrostError?

    // Review and confirm. The plan is reviewed against a PIT that needs no
    // session on the phone (the firmware's own, or a file), so the flash is the
    // first and only session of its Download Mode entry. The flash still reads
    // the phone's PIT and refuses before any data if the plan differs.
    @Published var devicePit: PitTable?
    /// Where `devicePit` came from, e.g. "CSC: NOBLELTE_AUS_XSA.pit".
    @Published var pitSource = ""
    @Published var plan: Plan?
    @Published var reviewError: BifrostError?
    @Published var acceptDataErase = false
    @Published var acknowledgeUnknownModel = false
    @Published var rebootWhenDone = Prefs.rebootDefault

    // Job and history
    @Published var job: JobState?
    @Published var actionError: BifrostError?
    @Published var history: [HistoryEntry] = []
    @Published var step: FlashStep = .setup

    // Navigation and small UI state
    @Published var section: SidebarItem = .flash
    @Published var selectedHistoryID: UUID?
    @Published var showServiceSetup = false
    @Published var lastBackup: URL?
    /// The last device job timed out before sending data: the handshake guess may be wrong.
    @Published var handshakeSuspect = false
    /// The phone stayed silent. Every attempt resets its USB, so further attempts
    /// in the same Download Mode entry only make it worse: wait for a restart.
    @Published var needsRestart = false

    private var agent: AgentConnection?
    private var connectionGeneration = 0
    private var refreshWork: DispatchWorkItem?
    private var retryTimer: Timer?
    @Published var serviceStatus = BackgroundServices.summary()
    @Published var serviceMessage: String?
    private let local = LocalCore()
    private var absentSince: Date?
    private var verifyGeneration = 0
    private var verifyDebounce: DispatchWorkItem?
    private let work = DispatchQueue(label: "bifrost.app.work", qos: .userInitiated)

    init() {
        loadHistory()
        connect()
    }

    // MARK: Agent

    func connect() {
        agentState = .connecting
        connectionGeneration += 1
        let generation = connectionGeneration
        do {
            agent = try AgentConnection { [weak self] event in
                // Ignore events from a connection that failed or was replaced.
                guard let self, self.connectionGeneration == generation, self.agent != nil else { return }
                self.handle(event)
            }
            agentState = .connected
            stopRetrying()
            refreshDevices()
            reattach()
        } catch let error as BifrostError {
            agent = nil
            agentState = .unavailable(error.message)
            startRetrying()
        } catch {
            agent = nil
            agentState = .unavailable(error.localizedDescription)
            startRetrying()
        }
    }

    /// While the agent is unreachable (not yet registered or approved),
    /// reconnect quietly every few seconds so approval in System Settings
    /// takes effect without another click.
    private func startRetrying() {
        guard retryTimer == nil else { return }
        retryTimer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, self.agent == nil else { return }
                self.serviceStatus = BackgroundServices.summary()
                if BackgroundServices.bothEnabled { self.connect() }
            }
        }
    }

    private func stopRetrying() {
        retryTimer?.invalidate()
        retryTimer = nil
    }

    func registerServices() {
        serviceMessage = BackgroundServices.register()
        refreshServiceStatus()
        if BackgroundServices.bothEnabled { connect() }
    }

    func refreshServiceStatus() { serviceStatus = BackgroundServices.summary() }

    private func reattach() {
        guard let agent, let snapshot = try? agent.status(), snapshot.running else { return }
        var state = JobState(id: snapshot.id, kind: snapshot.kind,
                             purpose: snapshot.kind == .flash ? .flash : .probe)
        state.progress = snapshot.progress
        state.plan = snapshot.plan
        state.log = snapshot.log
        state.device = snapshot.device
        job = state
        if let device = snapshot.device { selectedDevice = device }
        section = .flash
        step = .flash
    }

    func refreshDevices() {
        guard let agent else { return }
        do {
            devices = try agent.devices()
            deviceMessage = nil
        } catch let error as BifrostError {
            deviceMessage = error.message
            if error.code == BifrostError.serviceUnavailable { agentState = .unavailable(error.message) }
        } catch {
            deviceMessage = error.localizedDescription
        }
        let inDownload = devices.filter(\.inDownloadMode)
        // A phone that left Download Mode for more than a few seconds was
        // restarted, so it will expect a fresh handshake. Our own USB release
        // re-enumerates it for well under a second.
        if inDownload.isEmpty {
            if absentSince == nil { absentSince = Date() }
        } else {
            if let since = absentSince, Date().timeIntervalSince(since) > 3 {
                handshakeDone = false
                needsRestart = false
            }
            absentSince = nil
        }
        if let selected = selectedDevice, !inDownload.contains(where: { $0.id == selected }) {
            // Addresses change on every re-enumeration: follow a single phone.
            selectedDevice = inDownload.count == 1 ? inDownload[0].id : nil
        } else if selectedDevice == nil, inDownload.count == 1 {
            selectedDevice = inDownload[0].id
        }
    }

    private func handle(_ event: AgentEvent) {
        switch event {
        case .devicesChanged:
            // Let the USB change settle, and never enumerate during a job.
            refreshWork?.cancel()
            let work = DispatchWorkItem { [weak self] in
                guard let self, !(self.job?.running ?? false) else { return }
                self.refreshDevices()
            }
            refreshWork = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5, execute: work)
        case .disconnected:
            agent = nil
            agentState = .unavailable("The connection to the background agent was lost. Bifrost Studio reconnects automatically; a running flash continues in the background.")
            startRetrying()
        case .started:
            break
        case let .progress(id, progress):
            guard job?.id == id else { return }
            update(progress)
        case let .log(id, line):
            guard job?.id == id else { return }
            job?.log.append(line)
        case let .plan(id, plan):
            guard job?.id == id else { return }
            job?.plan = plan
        case let .finished(id, result):
            guard job?.id == id else { return }
            finish(result)
        }
    }

    private func update(_ progress: JobProgress) {
        guard var state = job else { return }
        state.progress = progress
        let now = Date()
        if progress.phase == .transferring || progress.phase == .committing {
            if let last = state.lastSample, progress.totalDone > last.bytes {
                let seconds = now.timeIntervalSince(last.date)
                if seconds > 0.2 {
                    let rate = Double(progress.totalDone - last.bytes) / seconds
                    state.bytesPerSecond = state.bytesPerSecond == 0 ? rate
                        : 0.3 * rate + 0.7 * state.bytesPerSecond
                    state.lastSample = (now, progress.totalDone)
                }
            } else if state.lastSample == nil {
                state.lastSample = (now, progress.totalDone)
            }
        }
        job = state
    }

    private func finish(_ result: JobResult) {
        guard var state = job else { return }
        state.running = false
        state.result = result
        job = state
        if let error = result.error {
            // Failures before the session could not have consumed the handshake.
            let preSession = [BifrostError.serviceUnavailable, BifrostError.usbAccess,
                              BifrostError.usbDisconnected].contains(error.code)
            let silentTimeout = error.code == BifrostError.usbTimeout && error.dataState == .none
            if !preSession { handshakeDone = true }
            handshakeSuspect = silentTimeout
            if silentTimeout { needsRestart = true }
        } else {
            handshakeSuspect = false
            handshakeDone = !result.rebooted
        }
        record(state)
        defer {
            // Refresh after our own release re-enumeration has settled.
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in self?.refreshDevices() }
        }
        switch state.purpose {
        case let .backupPit(url):
            if let pit = result.pit {
                // The save panel already confirmed any overwrite.
                do {
                    try pit.write(to: url, options: .atomic)
                    lastBackup = url
                } catch {
                    actionError = BifrostError(code: BifrostError.io, message: error.localizedDescription)
                }
            }
        case .flash, .probe:
            break
        }
    }

    // MARK: Device jobs

    var canStartJob: Bool {
        agent != nil && selectedDevice != nil && !(job?.running ?? false) && !needsRestart
    }

    func probe() { start(.probe) { try $0.startProbe($1, resume: self.handshakeDone) } }

    func backupPit(to url: URL) {
        lastBackup = nil
        start(.backupPit(url), kind: .readPit) { try $0.startReadPit($1, resume: self.handshakeDone) }
    }

    /// Plans against the PIT inside the selected firmware. Touches no phone.
    func reviewFromFirmware() {
        guard let packages else { return }
        resetConfirmation()
        reviewError = nil
        plan = nil
        devicePit = nil
        pitSource = ""
        switch PitTable.embedded(in: packages) {
        case let .success(found):
            let slot = packages.infos.first { $0.entries.contains { $0.name == found.name } }?.slot
            buildPlan(from: found.bytes, source: "\(slot?.title ?? "Firmware"): \(found.name)")
        case let .failure(error):
            reviewError = error
        }
    }

    /// Plans against a saved PIT file (e.g. a PIT Backup). Touches no phone.
    func reviewFromFile(_ url: URL) {
        resetConfirmation()
        reviewError = nil
        plan = nil
        devicePit = nil
        pitSource = ""
        do {
            let data = try Data(contentsOf: url)
            guard data.count <= 1 << 20 else {
                reviewError = BifrostError(code: BifrostError.pitInvalid, message: "PIT file is larger than 1 MiB")
                return
            }
            buildPlan(from: data, source: "File: \(url.lastPathComponent)")
        } catch {
            reviewError = BifrostError(code: BifrostError.io, message: error.localizedDescription)
        }
    }

    /// A flash must open the first session of a Download Mode entry.
    func phoneWasRestarted() {
        handshakeDone = false
        handshakeSuspect = false
        needsRestart = false
    }

    /// The model is taken from the firmware; the core still checks it against the plan.
    func startFlash(detectedModel: String) {
        guard let pit = devicePit, let plan, plan.flashable, !handshakeDone else { return }
        let request = AgentConnection.FlashRequest(
            device: selectedDevice!, packages: orderedPaths, typedModel: detectedModel.isEmpty ? "UNKNOWN" : detectedModel,
            acceptDataErase: acceptDataErase, reboot: rebootWhenDone,
            reviewedPit: pit.bytes)
        if start(.flash, kind: .flash, { agent, _ in try agent.startFlash(request) }) {
            step = .flash
            resetConfirmation()
        }
    }

    /// Every acknowledgement apply to one reviewed plan only.
    func resetConfirmation() {
        acceptDataErase = false
        acknowledgeUnknownModel = false
    }

    /// Back to a clean Setup. Keeps the verified firmware, drops everything tied to the last phone.
    func resetFlow() {
        guard !(job?.running ?? false) else { return }
        job = nil
        devicePit = nil
        pitSource = ""
        plan = nil
        reviewError = nil
        resetConfirmation()
        rebootWhenDone = Prefs.rebootDefault
        step = .setup
    }

    /// Re-opens a past flash with the same firmware files, if they still exist.
    func retry(_ entry: HistoryEntry) {
        guard let saved = entry.packagePaths, !saved.isEmpty,
              saved.allSatisfy({ FileManager.default.fileExists(atPath: $0.path) }) else { return }
        guard !(job?.running ?? false) else { return }
        job = nil
        paths = Dictionary(uniqueKeysWithValues: saved.map { ($0.slot, $0.path) })
        devicePit = nil
        pitSource = ""
        plan = nil
        reviewError = nil
        resetConfirmation()
        step = .setup
        section = .flash
        verifyFirmware()
    }

    func canRetry(_ entry: HistoryEntry) -> Bool {
        guard entry.kind == .flash, let saved = entry.packagePaths, !saved.isEmpty else { return false }
        return saved.allSatisfy { FileManager.default.fileExists(atPath: $0.path) }
    }

    func cancel(afterSequence: Bool) {
        do { try agent?.cancel(afterSequence: afterSequence) } catch let error as BifrostError {
            actionError = error
        } catch {}
    }

    @discardableResult
    private func start(_ purpose: JobState.Purpose, kind: JobKind? = nil,
                       _ body: (AgentConnection, DeviceID) throws -> UInt64) -> Bool {
        guard let agent, let device = selectedDevice else { return false }
        actionError = nil
        do {
            let id = try body(agent, device)
            let k: JobKind = kind ?? (purpose.isProbe ? .probe : .flash)
            var state = JobState(id: id, kind: k, purpose: purpose)
            state.device = device
            if k == .flash {
                state.plan = plan
                state.packagePaths = orderedPaths.map { HistoryPath(slot: $0.0, path: $0.1) }
            }
            job = state
            handshakeSuspect = false
            return true
        } catch let error as BifrostError {
            actionError = error
        } catch {
            actionError = BifrostError(code: BifrostError.io, message: error.localizedDescription)
        }
        return false
    }

    // MARK: Firmware

    var orderedPaths: [(Slot, String)] {
        Slot.allCases.compactMap { slot in paths[slot].map { (slot, $0) } }
    }

    /// Sets a slot's file and verifies the set shortly afterwards, so several
    /// files can be dropped in a row without restarting the check each time.
    func setPath(_ path: String?, for slot: Slot) {
        paths[slot] = path
        packages = nil
        plan = nil
        devicePit = nil
        firmwareError = nil
        resetConfirmation()
        verifyGeneration += 1
        verifyDebounce?.cancel()
        if loadingProgress != nil { local?.cancelLoading() }
        guard !orderedPaths.isEmpty else {
            loadingProgress = nil
            return
        }
        let item = DispatchWorkItem { [weak self] in self?.verifyFirmware() }
        verifyDebounce = item
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8, execute: item)
    }

    func verifyFirmware() {
        guard let local, !orderedPaths.isEmpty else { return }
        let paths = orderedPaths
        packages = nil
        plan = nil
        firmwareError = nil
        loadingProgress = JobProgress()
        verifyGeneration += 1
        let generation = verifyGeneration
        work.async {
            let result = local.loadPackages(paths) { progress in
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.verifyGeneration == generation else { return }
                    self.loadingProgress = progress
                }
            }
            DispatchQueue.main.async { [weak self] in
                // A newer set of files replaced this one while it was being checked.
                guard let self, self.verifyGeneration == generation else { return }
                self.loadingProgress = nil
                switch result {
                case let .success(loaded): self.packages = loaded
                case let .failure(error): self.firmwareError = error
                }
            }
        }
    }

    func cancelVerification() { local?.cancelLoading() }

    private func buildPlan(from pit: Data, source: String) {
        guard let packages else { return }
        switch PitTable.parse(pit, packages: packages) {
        case let .success((table, plan)):
            devicePit = table
            pitSource = source
            self.plan = plan
        case let .failure(error):
            reviewError = error
        }
    }

    // MARK: History

    private var historyDirectory: URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent("Bifrost Studio/History", isDirectory: true)
    }

    private func record(_ state: JobState) {
        guard let result = state.result else { return }
        let entry = HistoryEntry(
            date: state.startedAt, kind: state.kind,
            device: state.device?.label ?? "", model: state.plan?.detectedModel ?? "",
            files: state.plan?.files.map { "\($0.entry) → \($0.partition)" } ?? [],
            error: result.error, bytesTransferred: result.bytesTransferred,
            session: result.session, log: Array(state.log.suffix(2000)),
            duration: Date().timeIntervalSince(state.startedAt),
            packagePaths: state.packagePaths.isEmpty ? nil : state.packagePaths)
        history.insert(entry, at: 0)
        guard let directory = historyDirectory else { return }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        if let data = try? encoder.encode(entry) {
            try? data.write(to: historyURL(entry))
        }
        applyRetention()
    }

    func historyURL(_ entry: HistoryEntry) -> URL {
        (historyDirectory ?? FileManager.default.temporaryDirectory)
            .appendingPathComponent("\(entry.id.uuidString).json")
    }

    var historyFolder: URL? { historyDirectory }

    /// Drops the oldest entries beyond the Settings limit (0 keeps all).
    func applyRetention() {
        let limit = Prefs.historyKeep
        guard limit > 0, history.count > limit else { return }
        for entry in history[limit...] { try? FileManager.default.removeItem(at: historyURL(entry)) }
        history = Array(history.prefix(limit))
    }

    func deleteHistory(_ ids: Set<UUID>) {
        for entry in history where ids.contains(entry.id) {
            try? FileManager.default.removeItem(at: historyURL(entry))
        }
        history.removeAll { ids.contains($0.id) }
        if let selected = selectedHistoryID, ids.contains(selected) { selectedHistoryID = nil }
    }

    func clearHistory() { deleteHistory(Set(history.map(\.id))) }

    func exportJSON(_ entry: HistoryEntry) -> Data? {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return try? encoder.encode(entry)
    }

    private func loadHistory() {
        guard let directory = historyDirectory,
              let files = try? FileManager.default.contentsOfDirectory(at: directory,
                                                                       includingPropertiesForKeys: nil)
        else { return }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        history = files.filter { $0.pathExtension == "json" }
            .compactMap { try? decoder.decode(HistoryEntry.self, from: Data(contentsOf: $0)) }
            .sorted { $0.date > $1.date }
        applyRetention()
    }

    func exportText(_ entry: HistoryEntry) -> String {
        var lines = ["Bifrost Studio \(entry.kind.title) — \(entry.date)",
                     "Device: \(entry.device)", "Model: \(entry.model)",
                     "Result: \(entry.error.map { "FAILED (\($0.codeName), \($0.dataState)): \($0.message)" } ?? "OK")",
                     String(format: "Session ACK: 0x%x, protocol %u", entry.session.ack, entry.session.protocolVersion),
                     "Bytes transferred: \(entry.bytesTransferred)",
                     "Duration: \(entry.duration.map { String(format: "%.0f s", $0) } ?? "unknown")", "", "Files:"]
        lines += entry.files.map { "  \($0)" }
        lines += ["", "Log:"]
        lines += entry.log.map { "  \($0.message)" }
        return lines.joined(separator: "\n")
    }
}

private extension JobState.Purpose {
    var isProbe: Bool { if case .probe = self { return true } else { return false } }
}
