// Connection to the background agent (bifrost-agent) through libbifrost's
// bf_agent_* API. Every event is copied out of the C callback (its pointers
// are only valid during the call) and delivered on the main queue.
import Bifrost
import Foundation

enum AgentEvent {
    case started(job: UInt64)
    case progress(job: UInt64, JobProgress)
    case log(job: UInt64, LogLine)
    case plan(job: UInt64, Plan)
    case finished(job: UInt64, JobResult)
    case devicesChanged
    case disconnected
}

struct JobSnapshot {
    var id: UInt64
    var kind: JobKind
    var running: Bool
    var device: DeviceID?
    var progress: JobProgress
    var plan: Plan?
    var log: [LogLine]
    var result: JobResult?
}

final class AgentConnection {
    private var handle: OpaquePointer?
    private let onEvent: (AgentEvent) -> Void

    /// Connects and subscribes. `onEvent` runs on the main queue.
    init(onEvent: @escaping (AgentEvent) -> Void) throws {
        self.onEvent = onEvent
        var error = bf_error()
        let user = Unmanaged.passUnretained(self).toOpaque()
        guard let handle = bf_agent_connect({ user, event in
            guard let user, let event else { return }
            let connection = Unmanaged<AgentConnection>.fromOpaque(user).takeUnretainedValue()
            if let copied = AgentConnection.copy(event.pointee) {
                DispatchQueue.main.async { connection.onEvent(copied) }
            }
        }, user, &error) else {
            throw BifrostError(error)
        }
        self.handle = handle
        guard bf_agent_subscribe(handle, &error) else {
            bf_agent_free(handle)
            self.handle = nil
            throw BifrostError(error)
        }
    }

    deinit {
        // No callback starts after bf_agent_free returns.
        if let handle { bf_agent_free(handle) }
    }

    private static func copy(_ e: bf_agent_event) -> AgentEvent? {
        switch e.kind {
        case cInt(BF_EVENT_STARTED): return .started(job: e.job)
        case cInt(BF_EVENT_PROGRESS): return .progress(job: e.job, JobProgress(e.progress))
        case cInt(BF_EVENT_LOG):
            return .log(job: e.job, LogLine(level: e.log_level, message: string(e.log_message)))
        case cInt(BF_EVENT_PLAN): return e.plan.map { .plan(job: e.job, Plan($0)) }
        case cInt(BF_EVENT_FINISHED): return e.result.map { .finished(job: e.job, JobResult($0.pointee)) }
        case cInt(BF_EVENT_DEVICES_CHANGED): return .devicesChanged
        case cInt(BF_EVENT_DISCONNECTED): return .disconnected
        default: return nil
        }
    }

    // The calls below are quick XPC round trips (jobs run in the agent).

    func devices() throws -> [Device] {
        var error = bf_error()
        guard let list = bf_agent_devices(handle, &error) else { throw BifrostError(error) }
        defer { bf_device_list_free(list) }
        var devices: [Device] = []
        for i in 0..<bf_device_list_count(list) {
            var info = bf_device_info()
            if bf_device_list_get(list, i, &info) {
                devices.append(Device(id: DeviceID(info.id), inDownloadMode: info.has_odin_interface))
            }
        }
        return devices
    }

    func startProbe(_ device: DeviceID, resume: Bool) throws -> UInt64 {
        var error = bf_error()
        var job: UInt64 = 0
        var id = device.c
        guard bf_agent_start_probe(handle, &id, resume, &job, &error) else { throw BifrostError(error) }
        return job
    }

    func startReadPit(_ device: DeviceID, resume: Bool) throws -> UInt64 {
        var error = bf_error()
        var job: UInt64 = 0
        var id = device.c
        guard bf_agent_start_read_pit(handle, &id, resume, &job, &error) else { throw BifrostError(error) }
        return job
    }

    struct FlashRequest {
        var device: DeviceID
        var packages: [(Slot, String)]
        var typedModel: String
        var acceptDataErase: Bool
        var reboot: Bool
        var reviewedPit: Data?
    }

    func startFlash(_ request: FlashRequest) throws -> UInt64 {
        let paths = request.packages.map { strdup($0.1) }
        defer { paths.forEach { free($0) } }
        var slots = zip(request.packages, paths).map {
            bf_slot_path(slot: $0.0.0.rawValue, path: UnsafePointer($0.1))
        }
        let model = strdup(request.typedModel)
        defer { free(model) }
        let pit = request.reviewedPit ?? Data()
        var error = bf_error()
        var job: UInt64 = 0
        let ok: Bool = slots.withUnsafeMutableBufferPointer { slotBuffer in
            pit.withUnsafeBytes { pitBytes in
                var c = bf_agent_flash_request()
                c.device = request.device.c
                c.packages = UnsafePointer(slotBuffer.baseAddress)
                c.package_count = slotBuffer.count
                c.typed_model = UnsafePointer(model)
                c.accept_data_erase = request.acceptDataErase
                c.reboot = request.reboot
                c.reviewed_pit = pit.isEmpty ? nil : pitBytes.bindMemory(to: UInt8.self).baseAddress
                c.reviewed_pit_size = pit.count
                return bf_agent_start_flash(handle, &c, &job, &error)
            }
        }
        guard ok else { throw BifrostError(error) }
        return job
    }

    func cancel(afterSequence: Bool) throws {
        var error = bf_error()
        let level = afterSequence ? cInt(BF_CANCEL_AFTER_SEQUENCE) : cInt(BF_CANCEL_BEFORE_DATA)
        guard bf_agent_cancel(handle, level, &error) else { throw BifrostError(error) }
    }

    /// The current or most recent job, for reattaching after a relaunch.
    func status() throws -> JobSnapshot? {
        var error = bf_error()
        guard let job = bf_agent_status(handle, &error) else {
            if error.code != cInt(BF_OK) { throw BifrostError(error) }
            return nil
        }
        defer { bf_job_free(job) }
        var progress = bf_progress()
        bf_job_progress(job, &progress)
        var device = bf_device_id()
        let hasDevice = bf_job_device(job, &device)
        var log: [LogLine] = []
        for i in 0..<bf_job_log_count(job) {
            var level: Int32 = 0
            log.append(LogLine(level: level, message: string(bf_job_log(job, i, &level))))
            log[log.count - 1].level = level
        }
        var result = bf_job_result()
        return JobSnapshot(id: bf_job_id(job),
                           kind: JobKind(rawValue: bf_job_kind_of(job)) ?? .flash,
                           running: bf_job_running(job),
                           device: hasDevice ? DeviceID(device) : nil,
                           progress: JobProgress(progress),
                           plan: bf_job_plan(job).map { Plan($0) },
                           log: log,
                           result: bf_job_result_get(job, &result) ? JobResult(result) : nil)
    }
}
