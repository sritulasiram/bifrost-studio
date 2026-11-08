// Swift side of libbifrost: value types copied out of the C API, and thin
// wrappers for the in-process (read-only) calls. Device work goes through
// AgentConnection (AgentConnection.swift).
import Bifrost
import Foundation

// MARK: - C helpers

@inline(__always) func cInt<E: RawRepresentable>(_ value: E) -> Int32 where E.RawValue: BinaryInteger {
    Int32(truncatingIfNeeded: value.rawValue)
}

func string(_ pointer: UnsafePointer<CChar>?) -> String {
    pointer.map { String(cString: $0) } ?? ""
}

extension bf_error {
    static func empty() -> bf_error { bf_error() }

    var text: String {
        var copy = self
        return withUnsafePointer(to: &copy.message) {
            $0.withMemoryRebound(to: CChar.self, capacity: Int(BF_ERROR_MESSAGE_SIZE)) {
                String(cString: $0)
            }
        }
    }
}

// MARK: - Errors

enum DataState: Int32, Codable {
    case none = 0, partial = 1, complete = 2
}

struct BifrostError: Error, Codable, Hashable {
    var code: Int32
    var dataState: DataState
    var message: String

    init(code: Int32, dataState: DataState = .none, message: String) {
        self.code = code
        self.dataState = dataState
        self.message = message
    }

    init(_ error: bf_error) {
        code = error.code
        dataState = DataState(rawValue: error.data_state) ?? .partial
        message = error.text
    }

    var codeName: String { string(bf_code_name(code)) }

    static let serviceUnavailable = cInt(BF_E_SERVICE_UNAVAILABLE)
    static let usbTimeout = cInt(BF_E_USB_TIMEOUT)
    static let usbDisconnected = cInt(BF_E_USB_DISCONNECTED)
    static let usbAccess = cInt(BF_E_USB_ACCESS)
    static let modelMismatch = cInt(BF_E_MODEL_MISMATCH)
    static let planRefused = cInt(BF_E_PLAN_REFUSED)
    static let cancelled = cInt(BF_E_CANCELLED)
    static let checksumMismatch = cInt(BF_E_CHECKSUM_MISMATCH)
    static let fileChanged = cInt(BF_E_FILE_CHANGED)
    static let bootloaderRejected = cInt(BF_E_BOOTLOADER_REJECTED)
    static let illegalState = cInt(BF_E_ILLEGAL_STATE)
    static let pitInvalid = cInt(BF_E_PIT_INVALID)
    static let io: Int32 = 2 // BF_E_IO
}

// MARK: - Domain values

enum Slot: Int32, CaseIterable, Identifiable, Codable {
    case bl = 0, ap, cp, csc, userdata
    var id: Int32 { rawValue }
    var title: String {
        switch self {
        case .bl: return "BL"
        case .ap: return "AP"
        case .cp: return "CP"
        case .csc: return "CSC"
        case .userdata: return "USERDATA"
        }
    }
    var detail: String {
        switch self {
        case .bl: return "Bootloader"
        case .ap: return "Android system (PDA)"
        case .cp: return "Modem"
        case .csc: return "Region: CSC (wipes data) or HOME_CSC (keeps data)"
        case .userdata: return "User data image (erases data)"
        }
    }
}

enum Phase: Int32, Codable {
    case verifying = 0, checkingImages, connecting, readingPit, planning, transferring,
         committing, closing, done
    var title: String {
        switch self {
        case .verifying: return "Verifying firmware"
        case .checkingImages: return "Checking compressed images"
        case .connecting: return "Connecting to the phone"
        case .readingPit: return "Reading the partition table"
        case .planning: return "Planning"
        case .transferring: return "Sending data"
        case .committing: return "Phone is writing (committing)…"
        case .closing: return "Closing the session"
        case .done: return "Done"
        }
    }
}

struct DeviceID: Hashable, Codable {
    var vendorID: UInt16
    var productID: UInt16
    var bus: UInt8
    var address: UInt8

    init(_ id: bf_device_id) {
        vendorID = id.vendor_id
        productID = id.product_id
        bus = id.bus
        address = id.address
    }

    var c: bf_device_id {
        bf_device_id(vendor_id: vendorID, product_id: productID, bus: bus, address: address)
    }

    var label: String {
        String(format: "%04x:%04x · bus %u · address %u", vendorID, productID, bus, address)
    }
}

struct Device: Identifiable, Hashable {
    var id: DeviceID
    var inDownloadMode: Bool
}

struct JobProgress: Codable, Equatable {
    var phase: Phase = .verifying
    var slot = ""
    var entry = ""
    var fileDone: UInt64 = 0
    var fileTotal: UInt64 = 0
    var totalDone: UInt64 = 0
    var total: UInt64 = 0

    init() {}
    init(_ p: bf_progress) {
        phase = Phase(rawValue: p.phase) ?? .verifying
        slot = string(p.slot)
        entry = string(p.entry)
        fileDone = p.file_done
        fileTotal = p.file_total
        totalDone = p.total_done
        total = p.total
    }

    var fraction: Double { total > 0 ? Double(totalDone) / Double(total) : 0 }
    var fileFraction: Double { fileTotal > 0 ? Double(fileDone) / Double(fileTotal) : 0 }
}

struct PlannedFile: Identifiable, Codable, Hashable {
    var id: String { "\(slot)-\(entry)" }
    var slot: Slot
    var entry: String
    var partition: String
    var partitionID: UInt32
    var size: UInt64
    var lz4: Bool
}

struct SkippedEntry: Identifiable, Codable, Hashable {
    var id: String { "\(slot)-\(entry)" }
    var slot: Slot
    var entry: String
    var reason: String
}

struct PlanWarning: Identifiable, Codable, Hashable {
    var id: String { message }
    var message: String
    var requiresAcknowledgement: Bool
}

struct Plan: Codable, Equatable {
    var files: [PlannedFile] = []
    var skipped: [SkippedEntry] = []
    var warnings: [PlanWarning] = []
    var refusals: [String] = []
    var detectedModel = ""
    var bootloaderRevision: Int32 = -1
    var erasesUserData = false
    var requiresAcknowledgement = false
    var flashable = false

    init() {}
    init(_ plan: OpaquePointer) {
        flashable = bf_plan_flashable(plan)
        requiresAcknowledgement = bf_plan_requires_acknowledgement(plan)
        erasesUserData = bf_plan_erases_user_data(plan)
        detectedModel = string(bf_plan_detected_model(plan))
        bootloaderRevision = bf_plan_bootloader_revision(plan)
        for i in 0..<bf_plan_file_count(plan) {
            var f = bf_planned_file()
            if bf_plan_file(plan, i, &f) {
                files.append(PlannedFile(slot: Slot(rawValue: f.slot) ?? .ap, entry: string(f.entry),
                                         partition: string(f.partition), partitionID: f.partition_id,
                                         size: f.size, lz4: f.lz4))
            }
        }
        for i in 0..<bf_plan_skipped_count(plan) {
            var s = bf_skipped_entry()
            if bf_plan_skipped(plan, i, &s) {
                skipped.append(SkippedEntry(slot: Slot(rawValue: s.slot) ?? .ap,
                                            entry: string(s.entry), reason: string(s.reason)))
            }
        }
        for i in 0..<bf_plan_warning_count(plan) {
            var ack = false
            let message = string(bf_plan_warning(plan, i, &ack))
            warnings.append(PlanWarning(message: message, requiresAcknowledgement: ack))
        }
        for i in 0..<bf_plan_refusal_count(plan) {
            refusals.append(string(bf_plan_refusal(plan, i)))
        }
    }
}

struct SessionInfo: Codable, Hashable {
    var ack: UInt32 = 0
    var protocolVersion: UInt16 = 0
    var compressed = false
    var packetSize: UInt32 = 0
    var partsPerSequence: UInt32 = 0

    init() {}
    init(_ s: bf_session_info) {
        ack = s.session_ack
        protocolVersion = s.protocol_version
        compressed = s.supports_compressed_transfer
        packetSize = s.packet_size
        partsPerSequence = s.parts_per_sequence
    }
}

struct JobResult: Codable, Equatable {
    var error: BifrostError?
    var session = SessionInfo()
    var bytesTransferred: UInt64 = 0
    var rebooted = false
    var pit: Data?

    init(_ r: bf_job_result) {
        if !r.ok { error = BifrostError(r.error) }
        session = SessionInfo(r.session)
        bytesTransferred = r.bytes_transferred
        rebooted = r.rebooted
        if let bytes = r.pit, r.pit_size > 0 {
            pit = Data(bytes: bytes, count: r.pit_size)
        }
    }
}

enum JobKind: Int32, Codable {
    case flash = 0, probe = 1, readPit = 2
    var title: String {
        switch self {
        case .flash: return "Flash"
        case .probe: return "Probe"
        case .readPit: return "Read PIT"
        }
    }
}

struct LogLine: Identifiable, Codable, Hashable {
    var id = UUID()
    var date = Date()
    var level: Int32
    var message: String
    var isError: Bool { level == cInt(BF_LOG_ERROR) }
    var isWarning: Bool { level == cInt(BF_LOG_WARNING) }
}

// MARK: - Packages (in-process, read-only)

enum PackageKind: Int32, Codable {
    case unknown = 0, bl, ap, cp, csc, homeCsc, userdata
    var badge: String? {
        switch self {
        case .csc: return "CSC · wipes data"
        case .homeCsc: return "HOME_CSC · keeps data"
        case .userdata: return "USERDATA · erases data"
        default: return nil
        }
    }
}

struct ArchiveEntry: Identifiable, Hashable {
    var id: String { name }
    var name: String
    var size: UInt64
    var lz4: Bool
}

struct PackageInfo: Identifiable, Hashable {
    var id: Slot { slot }
    var slot: Slot
    var path: String
    var fileName: String { (path as NSString).lastPathComponent }
    var checksumPresent: Bool
    var checksumValid: Bool
    var kind: PackageKind
    var build: String
    var model: String
    var cscCode: String
    var entries: [ArchiveEntry]
}

/// Owns a bf_packages handle (verified, parsed packages) for offline planning.
// Immutable after init; the handle is only read.
final class LoadedPackages: @unchecked Sendable {
    let handle: OpaquePointer
    let infos: [PackageInfo]

    init(handle: OpaquePointer) {
        self.handle = handle
        var infos: [PackageInfo] = []
        for i in 0..<bf_packages_count(handle) {
            var p = bf_package_info()
            guard bf_packages_get(handle, i, &p) else { continue }
            var entries: [ArchiveEntry] = []
            for e in 0..<p.entry_count {
                var entry = bf_entry_info()
                if bf_packages_entry(handle, i, e, &entry) {
                    entries.append(ArchiveEntry(name: string(entry.name), size: entry.size, lz4: entry.lz4))
                }
            }
            infos.append(PackageInfo(slot: Slot(rawValue: p.slot) ?? .ap, path: string(p.path),
                                     checksumPresent: p.checksum_present, checksumValid: p.checksum_valid,
                                     kind: PackageKind(rawValue: p.kind) ?? .unknown,
                                     build: string(p.build), model: string(p.model),
                                     cscCode: string(p.csc_code), entries: entries))
        }
        self.infos = infos
    }

    deinit { bf_packages_free(handle) }
}

/// In-process core for read-only work: verifying packages and offline plans.
// Used from one serial work queue at a time (AppModel.work).
final class LocalCore: @unchecked Sendable {
    private let context: OpaquePointer

    init?() {
        var error = bf_error()
        guard let context = bf_create(&error) else { return nil }
        self.context = context
    }

    deinit { bf_destroy(context) }

    /// Verifies (MD5) and parses packages. Blocking; call off the main thread.
    func loadPackages(_ paths: [(Slot, String)],
                      progress: @escaping (JobProgress) -> Void) -> Result<LoadedPackages, BifrostError> {
        final class Box { let fn: (JobProgress) -> Void; init(_ fn: @escaping (JobProgress) -> Void) { self.fn = fn } }
        let box = Box(progress)
        let user = Unmanaged.passRetained(box)
        defer { user.release() }
        bf_set_progress_callback(context, { user, p in
            guard let user, let p else { return }
            Unmanaged<Box>.fromOpaque(user).takeUnretainedValue().fn(JobProgress(p.pointee))
        }, user.toOpaque())
        defer { bf_set_progress_callback(context, nil, nil) }

        let cPaths = paths.map { strdup($0.1) }
        defer { cPaths.forEach { free($0) } }
        var slots = zip(paths, cPaths).map { bf_slot_path(slot: $0.0.0.rawValue, path: $0.1) }
        var error = bf_error()
        bf_cancel_reset(context)
        guard let handle = bf_packages_load(context, &slots, slots.count, &error) else {
            return .failure(BifrostError(error))
        }
        return .success(LoadedPackages(handle: handle))
    }

    func cancelLoading() { bf_cancel(context, cInt(BF_CANCEL_BEFORE_DATA)) }
}

struct PitTable {
    struct Entry: Identifiable, Hashable {
        /// Row position. Identifiers repeat across UFS logical units, so they can't be ids.
        var id: Int { index }
        var index: Int
        var identifier: UInt32
        var binaryType: UInt32
        var deviceType: UInt32
        var blockCount: UInt32
        var partition: String
        var flashFilename: String
    }
    var entries: [Entry]
    var bytes: Data

    /// The PIT inside the loaded firmware (normally CSC/HOME_CSC). No device access.
    static func embedded(in packages: LoadedPackages) -> Result<(name: String, bytes: Data), BifrostError> {
        var error = bf_error()
        guard let pit = bf_packages_embedded_pit(packages.handle, &error) else {
            return .failure(BifrostError(error))
        }
        defer { bf_pit_free(pit) }
        var size = 0
        guard let bytes = bf_pit_bytes(pit, &size), size > 0 else {
            return .failure(BifrostError(code: BifrostError.pitInvalid, message: "The firmware PIT is empty"))
        }
        let name = packages.infos.lazy.flatMap(\.entries).map(\.name)
            .first { !$0.contains("/") && $0.lowercased().hasSuffix(".pit") } ?? "firmware PIT"
        return .success((name, Data(bytes: bytes, count: size)))
    }

    /// Parses PIT bytes; also returns an offline plan when packages are given.
    static func parse(_ data: Data, packages: LoadedPackages? = nil) -> Result<(PitTable, Plan?), BifrostError> {
        var error = bf_error()
        let pit: OpaquePointer? = data.withUnsafeBytes { raw in
            bf_pit_parse(raw.bindMemory(to: UInt8.self).baseAddress, data.count, &error)
        }
        guard let pit else { return .failure(BifrostError(error)) }
        defer { bf_pit_free(pit) }
        var entries: [Entry] = []
        for i in 0..<bf_pit_entry_count(pit) {
            var e = bf_pit_entry()
            if bf_pit_entry_get(pit, i, &e) {
                entries.append(Entry(index: Int(i), identifier: e.identifier, binaryType: e.binary_type,
                                     deviceType: e.device_type, blockCount: e.block_count,
                                     partition: string(e.partition_name),
                                     flashFilename: string(e.flash_filename)))
            }
        }
        var plan: Plan?
        if let packages {
            guard let offline = bf_plan_offline(pit, packages.handle, &error) else {
                return .failure(BifrostError(error))
            }
            plan = Plan(offline)
            bf_plan_free(offline)
        }
        return .success((PitTable(entries: entries, bytes: data), plan))
    }
}

func formatBytes(_ value: UInt64) -> String {
    ByteCountFormatter.string(fromByteCount: Int64(clamping: value), countStyle: .file)
}
