// Bounded host-service CMIF/TIPC compatibility. Protocol reference (no copied code):
// switchbrew/libnx nx/include/switch/sf/{hipc,cmif,tipc}.h and services/{sm,csrng}.c.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct dm_ipc dm_ipc;
enum {
    DM_IPC_INVALID_SIZE = (101u << 9) | 1u,
    DM_IPC_INVALID_ADDRESS = (102u << 9) | 1u,
    DM_IPC_OUT_OF_HANDLES = (105u << 9) | 1u,
    DM_IPC_INVALID_MEMORY = (106u << 9) | 1u,
    DM_IPC_INVALID_HANDLE = (114u << 9) | 1u,
    DM_IPC_NOT_FOUND = (121u << 9) | 1u,
    DM_IPC_SESSION_CLOSED = (123u << 9) | 1u,
    DM_IPC_INVALID_CLIENT = (2u << 9) | 21u,
    DM_IPC_INVALID_SERVICE_NAME = (6u << 9) | 21u,
    DM_IPC_NOT_REGISTERED = (7u << 9) | 21u,
    DM_IPC_NOT_SUPPORTED = (1u << 9) | 10u,
    DM_IPC_UNKNOWN_COMMAND = (221u << 9) | 10u,
    DM_IPC_TLS_CAPACITY = 0x100,
    DM_IPC_MAX_SESSIONS = 64
};

// The caller owns/validates the message span and keeps its backing stable for
// dispatch. These callbacks execute with the session lock held; they must not
// call this core. validate_output has no side effects; on success the caller
// guarantees that fill_output cannot fail and keeps backing writable/live until
// dispatch returns. prepare_message runs only immediately before publication.
// Dirty-page preparation alone is NOT range validation or a lifetime pin.
typedef struct {
    void *context;
    int (*validate_output)(void *context, uint64_t address, uint64_t size);
    void (*fill_output)(void *context, uint64_t address, size_t size);
    void (*prepare_message)(void *context, void *message, size_t size);
} dm_ipc_memory;

// max_sessions is 1..64. No client references may remain when destroying.
dm_ipc *dm_ipc_create(unsigned max_sessions);
void dm_ipc_destroy(dm_ipc *ipc);
// Namespace predicate, including closed/stale handles: used before other SVC
// handle dispatchers. It does not claim the handle is live.
int dm_ipc_owns_handle(uint32_t handle);
uint32_t dm_ipc_connect_port(dm_ipc *ipc, const char *name, size_t length, uint32_t *out);
// Exactly eight service-name bytes, zero padded. Adapter path needs no SM handle.
uint32_t dm_ipc_connect_service(dm_ipc *ipc, const uint8_t name[8], uint32_t *out);
uint32_t dm_ipc_close(dm_ipc *ipc, uint32_t handle);
uint32_t dm_ipc_clone(dm_ipc *ipc, uint32_t handle, uint32_t *out);
uint32_t dm_ipc_convert_domain(dm_ipc *ipc, uint32_t handle, uint32_t *out);
uint32_t dm_ipc_pointer_size(dm_ipc *ipc, uint32_t handle, uint16_t *out);
// Return is transport Result; service failures use CMIF SFCO or a TIPC Result
// reply. Invalid packets leave message, output and session state untouched.
// A valid TIPC SM close returns SESSION_CLOSED without writing a reply, while
// marking the endpoint closed; its handle must still be closed by the caller.
uint32_t dm_ipc_dispatch(dm_ipc *ipc, uint32_t handle, void *message,
                         size_t capacity, const dm_ipc_memory *memory);

// Production bridge, implemented beside the memory-domain owner in hle_svc.c.
// Typed HIPC adapters and raw SVCs share this process-lifetime session owner.
dm_ipc *dm_ipc_host(void);
uint32_t dm_ipc_host_send(uint32_t handle, void *message, size_t capacity);

// Bounded diagnostic for one reviewed original-CSRNG call. Inactive by default;
// no output bytes/addresses are logged. The expected span is checked before any
// entropy write (an empty buffer may use its expected address or null). A probe
// owns one call, not a persistent service policy.
enum {
    DM_IPC_PROBE_FORCE_CRYPTO, DM_IPC_PROBE_ADAPTER_CONNECT,
    DM_IPC_PROBE_ADAPTER_SEND, DM_IPC_PROBE_NATIVE_PORT,
    DM_IPC_PROBE_NATIVE_SEND, DM_IPC_PROBE_NATIVE_BUFFER, DM_IPC_PROBE_ROUTES
};
typedef struct {
    uint64_t filled_bytes;
    unsigned requests, csrng_connections, sm_requests;
    unsigned transport_errors, service_errors, mismatches;
    unsigned sessions_before, sessions_after;
    unsigned route[DM_IPC_PROBE_ROUTES];
} dm_ipc_csrng_probe;
uint32_t dm_ipc_csrng_probe_begin(dm_ipc *ipc, uint64_t address, size_t size);
uint32_t dm_ipc_csrng_probe_end(dm_ipc *ipc, dm_ipc_csrng_probe *out);
void dm_ipc_csrng_probe_route(dm_ipc *ipc, unsigned route);
void dm_ipc_host_csrng_probe_route(unsigned route);
