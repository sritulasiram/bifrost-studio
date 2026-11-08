/*
 * Bifrost Studio core: stable C API.
 *
 * Conventions
 *  - Every function that can fail takes a `bf_error*` (may be NULL) and
 *    returns NULL / false on failure. On success the error is cleared.
 *  - Objects returned by bf_*_create / load / read / plan functions are owned
 *    by the caller and released with the matching bf_*_free function.
 *  - Strings returned by accessors are borrowed: they stay valid until the
 *    owning object is freed.
 *  - Long operations (package loading, probe, PIT read, flash) block the
 *    calling thread and invoke the context's callbacks on that thread. One
 *    long operation at a time per context; a second concurrent call fails
 *    with BF_E_ILLEGAL_STATE.
 *  - bf_cancel() may be called from any thread. A cancel request persists
 *    until bf_cancel_reset(); a stale request makes the next operation stop
 *    before data (fail-safe), so reset it explicitly before starting a new one.
 *  - All sizes are uint64_t.
 */
#ifndef BIFROST_BIFROST_H
#define BIFROST_BIFROST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define BF_API __attribute__((visibility("default")))
#else
#define BF_API
#endif

/* Incremented on any incompatible change to this header. */
#define BF_API_VERSION 1

BF_API uint32_t bf_api_version(void);
BF_API const char *bf_version(void);

/* ---- errors ------------------------------------------------------------- */

typedef enum bf_code {
    BF_OK = 0,
    BF_E_INVALID_ARGUMENT = 1,
    BF_E_IO = 2,
    BF_E_PACKAGE_INVALID = 3,
    BF_E_CHECKSUM_MISMATCH = 4,
    BF_E_ARCHIVE_UNSUPPORTED = 5,
    BF_E_LZ4_INVALID = 6,
    BF_E_INSUFFICIENT_SPACE = 7,
    BF_E_FILE_CHANGED = 8,
    BF_E_PIT_INVALID = 9,
    BF_E_PLAN_REFUSED = 10,
    BF_E_MODEL_MISMATCH = 11,
    BF_E_USB_DISCONNECTED = 12,
    BF_E_USB_TIMEOUT = 13,
    BF_E_USB_STALL = 14,
    BF_E_USB_ACCESS = 15,
    BF_E_USB_OTHER = 16,
    BF_E_PROTOCOL_VIOLATION = 17,
    BF_E_BOOTLOADER_REJECTED = 18,
    BF_E_CANCELLED = 19,
    BF_E_ILLEGAL_STATE = 20,
    BF_E_SERVICE_UNAVAILABLE = 21, /* background agent or USB helper unreachable */
    BF_E_UNSUPPORTED = 100 /* e.g. built without USB support */
} bf_code;

/* What the device may have received when an operation failed. */
typedef enum bf_data_state {
    BF_DATA_NONE = 0,     /* no partition data was sent */
    BF_DATA_PARTIAL = 1,  /* some partition data may have been written */
    BF_DATA_COMPLETE = 2  /* all data acknowledged; only close/reboot failed */
} bf_data_state;

#define BF_ERROR_MESSAGE_SIZE 1024

typedef struct bf_error {
    int32_t code;       /* bf_code */
    int32_t data_state; /* bf_data_state */
    int32_t native_code;
    char message[BF_ERROR_MESSAGE_SIZE]; /* UTF-8, NUL-terminated, may be truncated */
} bf_error;

/* Stable kebab-case identifiers (e.g. "usb-stall", "partial") for logs and
 * localization keys; not user-facing text. */
BF_API const char *bf_code_name(int32_t code);
BF_API const char *bf_data_state_name(int32_t state);

/* ---- context, callbacks, cancellation ----------------------------------- */

typedef struct bf_ctx bf_ctx;

typedef enum bf_log_level { BF_LOG_INFO = 0, BF_LOG_WARNING = 1, BF_LOG_ERROR = 2 } bf_log_level;

typedef enum bf_phase {
    BF_PHASE_VERIFYING = 0,
    BF_PHASE_CHECKING_IMAGES = 1,
    BF_PHASE_CONNECTING = 2,
    BF_PHASE_READING_PIT = 3,
    BF_PHASE_PLANNING = 4,
    BF_PHASE_TRANSFERRING = 5,
    BF_PHASE_COMMITTING = 6, /* device is writing a sequence; may take a while */
    BF_PHASE_CLOSING = 7,
    BF_PHASE_DONE = 8
} bf_phase;

typedef struct bf_progress {
    int32_t phase;     /* bf_phase */
    const char *slot;  /* "bl", "ap", ... or "" */
    const char *entry; /* archive entry or file path, or "" */
    uint64_t file_done;
    uint64_t file_total;
    uint64_t total_done;
    uint64_t total;
} bf_progress;

typedef struct bf_plan bf_plan;

typedef void (*bf_log_fn)(void *user, int32_t level, const char *message);
typedef void (*bf_progress_fn)(void *user, const bf_progress *progress);
/* The plan built inside the flash session, before any data is sent. The
 * plan is borrowed for the duration of the callback. */
typedef void (*bf_plan_fn)(void *user, const bf_plan *plan);
/* Called on an internal thread when a Samsung device arrives or leaves. */
typedef void (*bf_hotplug_fn)(void *user);

/* Creates a context using the libusb transport. */
BF_API bf_ctx *bf_create(bf_error *error);
BF_API void bf_destroy(bf_ctx *ctx);

BF_API void bf_set_log_callback(bf_ctx *ctx, bf_log_fn fn, void *user);
BF_API void bf_set_progress_callback(bf_ctx *ctx, bf_progress_fn fn, void *user);
BF_API void bf_set_plan_callback(bf_ctx *ctx, bf_plan_fn fn, void *user);

typedef enum bf_cancel_level {
    BF_CANCEL_NONE = 0,
    /* Stop any time before the first partition byte is sent; ignored after. */
    BF_CANCEL_BEFORE_DATA = 1,
    /* Also stop at the next sequence boundary during transfer. Leaves the
     * device partially written; request only after the user accepts that. */
    BF_CANCEL_AFTER_SEQUENCE = 2
} bf_cancel_level;

BF_API void bf_cancel(bf_ctx *ctx, int32_t level);
BF_API void bf_cancel_reset(bf_ctx *ctx);

/* ---- devices ------------------------------------------------------------- */

typedef struct bf_device_id {
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t bus;
    uint8_t address;
} bf_device_id;

typedef struct bf_device_info {
    bf_device_id id;
    bool has_odin_interface;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_in;
    uint8_t endpoint_out;
} bf_device_info;

typedef struct bf_device_list bf_device_list;

BF_API bf_device_list *bf_devices(bf_ctx *ctx, bf_error *error);
BF_API size_t bf_device_list_count(const bf_device_list *list);
BF_API bool bf_device_list_get(const bf_device_list *list, size_t index, bf_device_info *out);
BF_API size_t bf_device_list_warning_count(const bf_device_list *list);
BF_API const char *bf_device_list_warning(const bf_device_list *list, size_t index);
BF_API void bf_device_list_free(bf_device_list *list);

/* Returns false when hot-plug is unsupported. Replaces any earlier callback. */
BF_API bool bf_start_hotplug(bf_ctx *ctx, bf_hotplug_fn fn, void *user);
BF_API void bf_stop_hotplug(bf_ctx *ctx);

/* ---- firmware packages ---------------------------------------------------- */

typedef enum bf_slot {
    BF_SLOT_BL = 0,
    BF_SLOT_AP = 1,
    BF_SLOT_CP = 2,
    BF_SLOT_CSC = 3,
    BF_SLOT_USERDATA = 4
} bf_slot;

typedef enum bf_package_kind {
    BF_KIND_UNKNOWN = 0,
    BF_KIND_BL = 1,
    BF_KIND_AP = 2,
    BF_KIND_CP = 3,
    BF_KIND_CSC = 4,      /* full CSC: may erase user data */
    BF_KIND_HOME_CSC = 5, /* intended to keep user data */
    BF_KIND_USERDATA = 6
} bf_package_kind;

typedef struct bf_slot_path {
    int32_t slot; /* bf_slot */
    const char *path;
} bf_slot_path;

typedef struct bf_package_info {
    int32_t slot;
    const char *path;
    uint64_t archive_size;
    bool checksum_present;
    bool checksum_valid;
    const char *checksum_hex;
    size_t entry_count;
    /* From the file name (a heuristic; never the only evidence). */
    int32_t kind;          /* bf_package_kind */
    const char *build;     /* e.g. "N950FXXU8DUH1" or "" */
    const char *model;     /* e.g. "N950F" or "" */
    const char *csc_code;  /* e.g. "OXM" or "" */
    int32_t bootloader_revision; /* -1 if unknown */
} bf_package_info;

typedef struct bf_entry_info {
    const char *name;
    uint64_t size;  /* stored size (compressed size for .lz4) */
    bool lz4;
} bf_entry_info;

typedef struct bf_packages bf_packages;

/* Verifies (MD5) and parses each package. No device access. Reports
 * BF_PHASE_VERIFYING progress and honours bf_cancel(). */
BF_API bf_packages *bf_packages_load(bf_ctx *ctx, const bf_slot_path *paths, size_t count,
                                     bf_error *error);
BF_API size_t bf_packages_count(const bf_packages *packages);
BF_API bool bf_packages_get(const bf_packages *packages, size_t index, bf_package_info *out);
BF_API bool bf_packages_entry(const bf_packages *packages, size_t package_index,
                              size_t entry_index, bf_entry_info *out);
BF_API void bf_packages_free(bf_packages *packages);

/* ---- PIT ------------------------------------------------------------------ */

typedef struct bf_pit bf_pit;

typedef struct bf_pit_entry {
    uint32_t binary_type; /* 0 = AP, 1 = CP (modem) */
    uint32_t device_type;
    uint32_t identifier;
    uint32_t attributes;
    uint32_t update_attributes;
    uint32_t block_size_or_offset;
    uint32_t block_count;
    uint32_t file_offset;
    uint32_t file_size;
    const char *partition_name;
    const char *flash_filename;
    const char *fota_filename;
} bf_pit_entry;

BF_API bf_pit *bf_pit_parse(const uint8_t *bytes, size_t size, bf_error *error);
BF_API bf_pit *bf_pit_load_file(const char *path, bf_error *error);
/* The PIT embedded in the loaded firmware: the root-level *.pit entry of the
 * packages (normally CSC/HOME_CSC). Lets a plan be reviewed without opening a
 * session on the phone, so the flash can be the first session of its Download
 * Mode entry; the flash still reads the phone's own PIT and refuses if the plan
 * differs. Fails with BF_E_PIT_INVALID when there is none, more than one, or it
 * does not parse. */
BF_API bf_pit *bf_packages_embedded_pit(const bf_packages *packages, bf_error *error);
/* One read-only session that downloads the device PIT. `resume`: skip the
 * ODIN/LOKE handshake because an earlier session in this Download Mode entry
 * ended without a reboot (the bootloader handshakes once per entry). */
BF_API bf_pit *bf_read_pit(bf_ctx *ctx, const bf_device_id *device, bool resume,
                           bf_error *error);
BF_API const uint8_t *bf_pit_bytes(const bf_pit *pit, size_t *size);
BF_API size_t bf_pit_entry_count(const bf_pit *pit);
BF_API bool bf_pit_entry_get(const bf_pit *pit, size_t index, bf_pit_entry *out);
BF_API void bf_pit_free(bf_pit *pit);

/* ---- plans ---------------------------------------------------------------- */

typedef struct bf_planned_file {
    int32_t slot;
    const char *entry;
    const char *partition;
    uint32_t partition_id;
    uint64_t size; /* stored size; decompressed size is known only at flash time */
    bool lz4;
} bf_planned_file;

typedef struct bf_skipped_entry {
    int32_t slot;
    const char *entry;
    const char *reason;
} bf_skipped_entry;

/* Builds a plan against a PIT (e.g. a saved device PIT). No device access. */
BF_API bf_plan *bf_plan_offline(const bf_pit *pit, const bf_packages *packages,
                                bf_error *error);
BF_API bool bf_plan_flashable(const bf_plan *plan);
BF_API bool bf_plan_requires_acknowledgement(const bf_plan *plan);
BF_API bool bf_plan_erases_user_data(const bf_plan *plan);
BF_API const char *bf_plan_detected_model(const bf_plan *plan); /* "" if none */
BF_API int32_t bf_plan_bootloader_revision(const bf_plan *plan); /* -1 if unknown */
BF_API size_t bf_plan_file_count(const bf_plan *plan);
BF_API bool bf_plan_file(const bf_plan *plan, size_t index, bf_planned_file *out);
BF_API size_t bf_plan_skipped_count(const bf_plan *plan);
BF_API bool bf_plan_skipped(const bf_plan *plan, size_t index, bf_skipped_entry *out);
BF_API size_t bf_plan_warning_count(const bf_plan *plan);
BF_API const char *bf_plan_warning(const bf_plan *plan, size_t index,
                                   bool *requires_acknowledgement);
BF_API size_t bf_plan_refusal_count(const bf_plan *plan);
BF_API const char *bf_plan_refusal(const bf_plan *plan, size_t index);
BF_API void bf_plan_free(bf_plan *plan);

/* ---- device sessions ------------------------------------------------------ */

typedef struct bf_session_info {
    uint32_t session_ack; /* raw begin-session response */
    uint16_t protocol_version;
    bool supports_compressed_transfer;
    uint32_t packet_size;
    uint32_t parts_per_sequence;
} bf_session_info;

/* One read-only session: handshake (unless `resume`), begin, end. No PIT or
 * file data. */
BF_API bool bf_probe(bf_ctx *ctx, const bf_device_id *device, bool resume,
                     bf_session_info *out, bf_error *error);

typedef struct bf_flash_options {
    const char *typed_model;  /* what the user typed, e.g. "SM-N950F" (required) */
    bool accept_data_erase;   /* required when the plan erases user data */
    bool reboot;              /* request a reboot after a successful flash */
    /* No `resume`: a flash always opens the first session of a Download Mode
     * entry (fresh handshake). Re-enter Download Mode after a probe or PIT read. */
    /* Optional plan the user reviewed offline; the in-session plan must match. */
    const bf_plan *reviewed_plan;
} bf_flash_options;

typedef struct bf_flash_report {
    bf_session_info session;
    uint64_t bytes_transferred;
    bool rebooted;
} bf_flash_report;

/* Flashes in exactly one Odin session (see flash::flash in flash.hpp). On
 * failure, error->data_state says whether the device may be partially
 * written. `report` may be NULL. */
BF_API bool bf_flash(bf_ctx *ctx, const bf_device_id *device, const bf_packages *packages,
                     const bf_flash_options *options, bf_flash_report *report,
                     bf_error *error);

/* ---- background agent (macOS) -------------------------------------------- */
/*
 * The app drives device jobs through the per-user background agent
 * (bifrost-agent), which keeps running if the app quits and lets it reattach.
 * Event callbacks run on an internal queue; no callback starts after
 * bf_agent_free returns. Pointers inside an event are valid only for the
 * duration of the callback.
 */
#if defined(__APPLE__)

typedef struct bf_agent bf_agent;
typedef struct bf_job bf_job;

typedef enum bf_job_kind { BF_JOB_FLASH = 0, BF_JOB_PROBE = 1, BF_JOB_READ_PIT = 2 } bf_job_kind;

typedef enum bf_agent_event_kind {
    BF_EVENT_STARTED = 0,
    BF_EVENT_PROGRESS = 1,
    BF_EVENT_LOG = 2,
    BF_EVENT_PLAN = 3,
    BF_EVENT_FINISHED = 4,
    BF_EVENT_DEVICES_CHANGED = 5,
    BF_EVENT_DISCONNECTED = 6 /* the agent went away; reconnect */
} bf_agent_event_kind;

typedef struct bf_job_result {
    bool ok;
    bf_error error;           /* when !ok; data_state drives recovery guidance */
    bf_session_info session;
    uint64_t bytes_transferred;
    bool rebooted;
    const uint8_t *pit;       /* read-pit jobs */
    size_t pit_size;
} bf_job_result;

typedef struct bf_agent_event {
    int32_t kind;             /* bf_agent_event_kind */
    uint64_t job;
    bf_progress progress;     /* BF_EVENT_PROGRESS */
    int32_t log_level;        /* BF_EVENT_LOG: bf_log_level */
    const char *log_message;  /* BF_EVENT_LOG */
    const bf_plan *plan;      /* BF_EVENT_PLAN (summary only; not usable as reviewed_plan) */
    const bf_job_result *result; /* BF_EVENT_FINISHED */
} bf_agent_event;

typedef void (*bf_agent_event_fn)(void *user, const bf_agent_event *event);

/* Connects to the installed agent and checks its code signature. */
BF_API bf_agent *bf_agent_connect(bf_agent_event_fn fn, void *user, bf_error *error);
BF_API void bf_agent_free(bf_agent *agent);

/* Starts event delivery (and hot-plug notifications) to the callback. */
BF_API bool bf_agent_subscribe(bf_agent *agent, bf_error *error);
BF_API bf_device_list *bf_agent_devices(bf_agent *agent, bf_error *error);

typedef struct bf_agent_flash_request {
    bf_device_id device;
    const bf_slot_path *packages; /* opened here; descriptors are passed to the agent */
    size_t package_count;
    const char *typed_model;
    bool accept_data_erase;
    bool reboot;
    /* No `resume`: see bf_flash_options. */
    const uint8_t *reviewed_pit;  /* optional: the PIT the user reviewed the plan against */
    size_t reviewed_pit_size;
} bf_agent_flash_request;

BF_API bool bf_agent_start_flash(bf_agent *agent, const bf_agent_flash_request *request,
                                 uint64_t *job, bf_error *error);
BF_API bool bf_agent_start_probe(bf_agent *agent, const bf_device_id *device, bool resume,
                                 uint64_t *job, bf_error *error);
BF_API bool bf_agent_start_read_pit(bf_agent *agent, const bf_device_id *device, bool resume,
                                    uint64_t *job, bf_error *error);
BF_API bool bf_agent_cancel(bf_agent *agent, int32_t level, bf_error *error);

/* The current or most recent job. Returns NULL with error->code == BF_OK when
 * no job has run since the agent started. */
BF_API bf_job *bf_agent_status(bf_agent *agent, bf_error *error);
BF_API uint64_t bf_job_id(const bf_job *job);
BF_API int32_t bf_job_kind_of(const bf_job *job);
BF_API bool bf_job_running(const bf_job *job);
BF_API bool bf_job_device(const bf_job *job, bf_device_id *out);
BF_API void bf_job_progress(const bf_job *job, bf_progress *out);
BF_API const bf_plan *bf_job_plan(const bf_job *job); /* NULL if none yet */
BF_API size_t bf_job_log_count(const bf_job *job);
BF_API const char *bf_job_log(const bf_job *job, size_t index, int32_t *level);
BF_API bool bf_job_result_get(const bf_job *job, bf_job_result *out); /* false while running */
BF_API void bf_job_free(bf_job *job);

#endif /* __APPLE__ */

#ifdef __cplusplus
}
#endif

#endif
