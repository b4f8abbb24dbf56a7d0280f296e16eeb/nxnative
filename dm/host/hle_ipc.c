// Original implementation of a deliberately small HIPC/CMIF service boundary.
// Semantic references: libnx sf/{hipc,cmif}.h, services/{sm,csrng}.c;
// Atmosphere stratosphere/sm/source/sm_user_service.cpp (no code copied).
#include "hle_ipc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { SERVICE_SM = 1, SERVICE_CSRNG, SERVICE_OLSC };
typedef struct {
    uint32_t generation;
    unsigned service, initialized, domain, closed;
} ipc_session;
struct dm_ipc {
    pthread_mutex_t mutex;
    unsigned capacity;
    ipc_session sessions[DM_IPC_MAX_SESSIONS];
    _Atomic unsigned probe_active;
    pthread_t probe_thread;
    uint64_t probe_address;
    size_t probe_size;
    dm_ipc_csrng_probe probe;
};

#define HANDLE_PREFIX UINT32_C(0xd0000000)
#define GENERATION_MAX UINT32_C(0x003fffff)
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void write32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static int fits(size_t offset, size_t size, size_t capacity) {
    return offset <= capacity && size <= capacity - offset;
}
int dm_ipc_owns_handle(uint32_t handle) { return (handle & UINT32_C(0xf0000000)) == HANDLE_PREFIX; }
static ipc_session *lookup(dm_ipc *ipc, uint32_t handle) {
    unsigned i = handle & 63u;
    uint32_t generation = (handle & UINT32_C(0x0fffffff)) >> 6;
    if (!dm_ipc_owns_handle(handle) || i >= ipc->capacity || !generation) return NULL;
    ipc_session *s = &ipc->sessions[i];
    return s->service && s->generation == generation ? s : NULL;
}
static uint32_t create_session(dm_ipc *ipc, unsigned service, const ipc_session *copy, uint32_t *out) {
    for (unsigned i = 0; i < ipc->capacity; ++i) {
        ipc_session *s = &ipc->sessions[i];
        if (s->service || s->generation == GENERATION_MAX) continue;
        uint32_t generation = s->generation + 1;
        *s = copy ? *copy : (ipc_session){0};
        s->generation = generation; s->service = service; s->closed = 0;
        *out = HANDLE_PREFIX | (generation << 6) | i;
        if (ipc->probe_active && service == SERVICE_CSRNG) ++ipc->probe.csrng_connections;
        return 0;
    }
    return DM_IPC_OUT_OF_HANDLES;
}
static unsigned service_id(const uint8_t name[8]) {
    static const uint8_t csrng[8] = {'c','s','r','n','g',0,0,0};
    static const uint8_t olsc[8] = {'o','l','s','c',':','u',0,0};
    return !memcmp(name, csrng, 8) ? SERVICE_CSRNG : !memcmp(name, olsc, 8) ? SERVICE_OLSC : 0;
}
static int service_name_valid(const uint8_t name[8]) {
    if (!name[0]) return 0;
    unsigned ended = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (!name[i]) ended = 1;
        else if (ended) return 0;
    }
    return 1;
}
dm_ipc *dm_ipc_create(unsigned max_sessions) {
    if (!max_sessions || max_sessions > DM_IPC_MAX_SESSIONS) return NULL;
    dm_ipc *ipc = calloc(1, sizeof *ipc);
    if (!ipc) return NULL;
    atomic_init(&ipc->probe_active, 0);
    if (pthread_mutex_init(&ipc->mutex, NULL)) { free(ipc); return NULL; }
    ipc->capacity = max_sessions;
    return ipc;
}
void dm_ipc_destroy(dm_ipc *ipc) {
    if (ipc) { pthread_mutex_destroy(&ipc->mutex); free(ipc); }
}
static unsigned live_sessions(dm_ipc *ipc) {
    unsigned n = 0;
    for (unsigned i = 0; i < ipc->capacity; ++i) n += ipc->sessions[i].service != 0;
    return n;
}
uint32_t dm_ipc_csrng_probe_begin(dm_ipc *ipc, uint64_t address, size_t size) {
    if (!ipc) return DM_IPC_INVALID_HANDLE;
    if (size > 4097 || size > UINT64_MAX - address) return DM_IPC_INVALID_SIZE;
    pthread_mutex_lock(&ipc->mutex);
    uint32_t result = ipc->probe_active ? DM_IPC_NOT_SUPPORTED : 0;
    if (!result) {
        ipc->probe = (dm_ipc_csrng_probe){.sessions_before = live_sessions(ipc)};
        ipc->probe_address = address; ipc->probe_size = size;
        ipc->probe_thread = pthread_self(); ipc->probe_active = 1;
    }
    pthread_mutex_unlock(&ipc->mutex); return result;
}
uint32_t dm_ipc_csrng_probe_end(dm_ipc *ipc, dm_ipc_csrng_probe *out) {
    if (!ipc || !out) return DM_IPC_INVALID_MEMORY;
    pthread_mutex_lock(&ipc->mutex);
    uint32_t result = !ipc->probe_active || !pthread_equal(ipc->probe_thread, pthread_self()) ? DM_IPC_NOT_SUPPORTED : 0;
    if (!result) {
        ipc->probe.sessions_after = live_sessions(ipc); *out = ipc->probe;
        ipc->probe_active = 0; ipc->probe_address = 0; ipc->probe_size = 0;
    }
    pthread_mutex_unlock(&ipc->mutex); return result;
}
void dm_ipc_csrng_probe_route(dm_ipc *ipc, unsigned route) {
    if (!ipc || !atomic_load(&ipc->probe_active)) return;
    pthread_mutex_lock(&ipc->mutex);
    if (ipc->probe_active) {
        if (route < DM_IPC_PROBE_ROUTES) ++ipc->probe.route[route];
        else ++ipc->probe.mismatches;
        if (!pthread_equal(ipc->probe_thread, pthread_self())) ++ipc->probe.mismatches;
    }
    pthread_mutex_unlock(&ipc->mutex);
}
uint32_t dm_ipc_connect_port(dm_ipc *ipc, const char *name, size_t length, uint32_t *out) {
    if (!ipc || !name || !out) return DM_IPC_INVALID_MEMORY;
    if (length != 3 || memcmp(name, "sm:", 3)) return DM_IPC_NOT_FOUND;
    pthread_mutex_lock(&ipc->mutex);
    uint32_t result = create_session(ipc, SERVICE_SM, NULL, out);
    pthread_mutex_unlock(&ipc->mutex); return result;
}
uint32_t dm_ipc_connect_service(dm_ipc *ipc, const uint8_t name[8], uint32_t *out) {
    if (!ipc || !name || !out) return DM_IPC_INVALID_MEMORY;
    if (!service_name_valid(name)) return DM_IPC_INVALID_SERVICE_NAME;
    unsigned service = service_id(name);
    if (!service) return DM_IPC_NOT_REGISTERED;
    pthread_mutex_lock(&ipc->mutex);
    uint32_t result = create_session(ipc, service, NULL, out);
    pthread_mutex_unlock(&ipc->mutex); return result;
}
uint32_t dm_ipc_close(dm_ipc *ipc, uint32_t handle) {
    if (!ipc) return DM_IPC_INVALID_HANDLE;
    pthread_mutex_lock(&ipc->mutex);
    ipc_session *s = lookup(ipc, handle);
    uint32_t result = s ? 0 : DM_IPC_INVALID_HANDLE;
    if (s) s->service = 0;
    pthread_mutex_unlock(&ipc->mutex); return result;
}
uint32_t dm_ipc_clone(dm_ipc *ipc, uint32_t handle, uint32_t *out) {
    if (!ipc || !out) return DM_IPC_INVALID_HANDLE;
    pthread_mutex_lock(&ipc->mutex);
    ipc_session *s = lookup(ipc, handle);
    uint32_t result = !s ? DM_IPC_INVALID_HANDLE : s->closed ? DM_IPC_SESSION_CLOSED : create_session(ipc, s->service, s, out);
    pthread_mutex_unlock(&ipc->mutex); return result;
}
uint32_t dm_ipc_convert_domain(dm_ipc *ipc, uint32_t handle, uint32_t *out) {
    if (!ipc || !out) return DM_IPC_INVALID_HANDLE;
    pthread_mutex_lock(&ipc->mutex);
    ipc_session *s = lookup(ipc, handle);
    uint32_t result = !s ? DM_IPC_INVALID_HANDLE : s->closed ? DM_IPC_SESSION_CLOSED : s->service != SERVICE_OLSC ? DM_IPC_NOT_SUPPORTED : 0;
    if (!result) { s->domain = 1; *out = 1; }
    pthread_mutex_unlock(&ipc->mutex); return result;
}
uint32_t dm_ipc_pointer_size(dm_ipc *ipc, uint32_t handle, uint16_t *out) {
    if (!ipc || !out) return DM_IPC_INVALID_HANDLE;
    pthread_mutex_lock(&ipc->mutex);
    ipc_session *s = lookup(ipc, handle);
    uint32_t result = !s ? DM_IPC_INVALID_HANDLE : s->closed ? DM_IPC_SESSION_CLOSED : 0;
    if (!result) *out = 0;
    pthread_mutex_unlock(&ipc->mutex); return result;
}

typedef struct {
    unsigned type, nx, na, nb, nw, pid, special, domain, domain_close;
    uint32_t version, command, token;
    size_t raw, raw_size, arguments, argument_capacity;
    uint64_t output_address, output_size;
    unsigned output_mode;
} request;

static uint32_t parse(const uint8_t *m, size_t capacity, unsigned domain, request *r) {
    if (!fits(0, 8, capacity)) return DM_IPC_INVALID_SIZE;
    uint32_t h0 = read32(m), h1 = read32(m + 4);
    *r = (request){0}; r->type = h0 & 0xffff;
    r->nx = h0 >> 16 & 15; r->na = h0 >> 20 & 15;
    r->nb = h0 >> 24 & 15; r->nw = h0 >> 28;
    // This bounded client has no receive-static/pointer-buffer facility.
    if (h1 & UINT32_C(0x7ffffc00)) return DM_IPC_NOT_SUPPORTED;
    r->raw_size = (h1 & 1023u) * 4u; r->special = h1 >> 31;
    size_t p = 8;
    if (r->special) {
        if (!fits(p, 4, capacity)) return DM_IPC_INVALID_SIZE;
        uint32_t special = read32(m + p); p += 4;
        if (special & ~1u) return DM_IPC_NOT_SUPPORTED; // input handle transfer unsupported
        r->pid = special & 1;
        if (r->pid) { if (!fits(p, 8, capacity)) return DM_IPC_INVALID_SIZE; p += 8; }
    }
    size_t descriptors = r->nx * 8u + (r->na + r->nb + r->nw) * 12u;
    if (!fits(p, descriptors, capacity)) return DM_IPC_INVALID_SIZE;
    if (r->nb == 1) {
        const uint8_t *b = m + p + r->nx * 8u + r->na * 12u;
        uint32_t high = read32(b + 8);
        r->output_address = read32(b + 4) | ((uint64_t)(high >> 28) << 32) | ((uint64_t)((high >> 2) & 0x3fffff) << 36);
        r->output_size = read32(b) | ((uint64_t)((high >> 24) & 15) << 32);
        r->output_mode = high & 3;
    }
    p += descriptors; r->raw = p;
    if (!fits(p, r->raw_size, capacity)) return DM_IPC_INVALID_SIZE;
    if (r->type == 2) {
        return h0 == 2 && h1 == 0 ? 0 : DM_IPC_NOT_SUPPORTED;
    }
    if (r->type != 4 && r->type != 5 && r->type != 6 && r->type != 7) return DM_IPC_NOT_SUPPORTED;
    size_t cm = (p + 15u) & ~(size_t)15;
    r->domain = domain && (r->type == 4 || r->type == 6);
    size_t extra = r->domain ? 16 : 0;
    if (r->raw_size < 16 + extra || !fits(cm, extra, p + r->raw_size)) return DM_IPC_INVALID_SIZE;
    if (r->domain) {
        uint32_t dh = read32(m + cm), object = read32(m + cm + 4);
        if ((dh & 0xff) != 1 && (dh & 0xff) != 2) return DM_IPC_NOT_SUPPORTED;
        if ((dh & 0xff00) || object != 1 || read32(m + cm + 8)) return DM_IPC_NOT_SUPPORTED;
        r->token = read32(m + cm + 12);
        r->domain_close = (dh & 0xff) == 2;
        if (r->domain_close) return (dh >> 16) == 0 ? 0 : DM_IPC_NOT_SUPPORTED;
        if ((dh >> 16) < 16 || (dh >> 16) > r->raw_size - 16 - extra) return DM_IPC_INVALID_SIZE;
        r->argument_capacity = (dh >> 16) - 16;
    } else {
        if (r->raw_size < 32) return DM_IPC_INVALID_SIZE;
        r->argument_capacity = r->raw_size - 32;
    }
    cm += extra;
    if (!fits(cm, 16, p + r->raw_size) || !fits(cm + 16, r->argument_capacity, p + r->raw_size)) return DM_IPC_INVALID_SIZE;
    if (read32(m + cm) != UINT32_C(0x49434653)) return DM_IPC_NOT_SUPPORTED;
    r->version = read32(m + cm + 4); r->command = read32(m + cm + 8);
    if (r->version > 1) return DM_IPC_NOT_SUPPORTED;
    uint32_t token = read32(m + cm + 12);
    if (!r->domain) r->token = token;
    else if (token) return DM_IPC_NOT_SUPPORTED;
    r->arguments = cm + 16;
    return 0;
}

static uint32_t dispatch_locked(dm_ipc *ipc, ipc_session *s, uint8_t *m, size_t capacity, const dm_ipc_memory *memory) {
    request r;
    uint32_t result = parse(m, capacity, s->domain, &r);
    if (result) return result;
    if (ipc->probe_active && s->service == SERVICE_SM) ++ipc->probe.sm_requests;
    if (r.type == 2) { s->closed = 1; return 0; } // endpoint close, handle still owned
    if (r.domain_close) {
        // Existing olsc adapter regards closing its single synthetic object as a
        // no-op; preserve that behavior, without accepting another object ID.
        if (memory && memory->prepare_message) memory->prepare_message(memory->context, m, 16);
        memset(m, 0, 16); return 0;
    }
    unsigned control = r.type == 5 || r.type == 7;
    unsigned descriptor_count = r.nx + r.na + r.nb + r.nw;
    uint32_t service_result = 0, moved = 0, value = 0;
    size_t value_size = 0;
    unsigned create_service = 0, clone = 0, initialize = 0, convert = 0, fill = 0;
    if (control) {
        if (descriptor_count || r.special) return DM_IPC_NOT_SUPPORTED;
        if (r.command == 3) value_size = 2;
        else if (s->service == SERVICE_SM) service_result = DM_IPC_NOT_SUPPORTED;
        else if (r.command == 0 && s->service == SERVICE_OLSC) { convert = 1; value = 1; value_size = 4; }
        else if (r.command == 2 || r.command == 4) {
            if (r.command == 4 && r.argument_capacity < 4) return DM_IPC_INVALID_SIZE;
            clone = 1;
        } else service_result = DM_IPC_NOT_SUPPORTED;
    } else if (s->service == SERVICE_SM) {
        if (descriptor_count) return DM_IPC_NOT_SUPPORTED;
        if (r.command == 0) {
            if (!r.pid || r.argument_capacity < 8) return DM_IPC_INVALID_SIZE;
            initialize = 1; // PID is supplied by this single-process host, never trusted from the packet
        } else if (r.command == 1) {
            if (r.special) return DM_IPC_NOT_SUPPORTED;
            if (r.argument_capacity < 8) return DM_IPC_INVALID_SIZE;
            if (!s->initialized) service_result = DM_IPC_INVALID_CLIENT;
            else if (!service_name_valid(m + r.arguments)) service_result = DM_IPC_INVALID_SERVICE_NAME;
            else if (!(create_service = service_id(m + r.arguments))) service_result = DM_IPC_NOT_REGISTERED;
        } else service_result = DM_IPC_UNKNOWN_COMMAND;
    } else if (s->service == SERVICE_CSRNG) {
        if (r.command) service_result = DM_IPC_UNKNOWN_COMMAND;
        else {
            if (r.nx || r.na || r.nb != 1 || r.nw || r.special || r.domain || r.output_mode) return DM_IPC_NOT_SUPPORTED;
            uint64_t a = r.output_address, n = r.output_size, base = (uintptr_t)m;
            if (ipc->probe_active) {
                ++ipc->probe.requests;
                if (!pthread_equal(ipc->probe_thread, pthread_self()) || ipc->probe.requests != 1 ||
                    (a != ipc->probe_address && !(n == 0 && a == 0)) || n != ipc->probe_size || r.type != 6 ||
                    r.version != 1 || r.raw_size != 32 || capacity != DM_IPC_TLS_CAPACITY) {
                    ++ipc->probe.mismatches; return DM_IPC_NOT_SUPPORTED;
                }
            }
            if (n > SIZE_MAX || n > UINT64_MAX - a || capacity > UINT64_MAX - base) return DM_IPC_INVALID_MEMORY;
            if (n && a < base + capacity && base < a + n) return DM_IPC_INVALID_MEMORY;
            if (!memory || !memory->validate_output || !memory->fill_output || !memory->validate_output(memory->context, a, n)) return DM_IPC_INVALID_MEMORY;
            fill = 1;
        }
    } // olsc alone retains its pre-existing synthetic success/zero-output policy.

    uint8_t reply[64] = {0};
    size_t cm = 16 + (r.domain ? 16 : 0);
    size_t end = (cm + 16 + value_size + 3) & ~(size_t)3;
    if (capacity < end) return DM_IPC_INVALID_SIZE;
    // No session allocation or visible state change occurs until parsing and
    // response capacity are known valid. Allocation failure is a service error.
    if (!service_result && (create_service || clone))
        service_result = create_session(ipc, clone ? s->service : create_service, clone ? s : NULL, &moved);
    write32(reply + 4, (uint32_t)((end - (moved ? 16 : 8) + 3) / 4) | (moved ? UINT32_C(0x80000000) : 0));
    if (moved) { write32(reply + 8, 1u << 5); write32(reply + 12, moved); }
    write32(reply + cm, UINT32_C(0x4f434653)); write32(reply + cm + 4, r.version);
    write32(reply + cm + 8, service_result); write32(reply + cm + 12, r.token);
    if (value_size && !service_result) write32(reply + cm + 16, value);
    size_t publish = s->service == SERVICE_OLSC && !control ? (capacity < 0x1000 ? capacity : 0x1000) : end;
    if (publish > end) write32(reply + 4, (uint32_t)((publish - 8) / 4));
    if (memory && memory->prepare_message) memory->prepare_message(memory->context, m, publish);
    if (fill && !service_result) {
        memory->fill_output(memory->context, r.output_address, (size_t)r.output_size);
        if (ipc->probe_active) ipc->probe.filled_bytes += r.output_size;
    }
    if (ipc->probe_active && service_result) ++ipc->probe.service_errors;
    if (initialize && !service_result) s->initialized = 1;
    if (convert && !service_result) s->domain = 1;
    memset(m, 0, publish); memcpy(m, reply, end);
    return 0;
}
uint32_t dm_ipc_dispatch(dm_ipc *ipc, uint32_t handle, void *message, size_t capacity, const dm_ipc_memory *memory) {
    if (!ipc) return DM_IPC_INVALID_MEMORY;
    pthread_mutex_lock(&ipc->mutex);
    ipc_session *s = lookup(ipc, handle);
    uint32_t result = !message ? DM_IPC_INVALID_MEMORY : !s ? DM_IPC_INVALID_HANDLE : s->closed ? DM_IPC_SESSION_CLOSED : dispatch_locked(ipc, s, message, capacity, memory);
    if (ipc->probe_active && result) ++ipc->probe.transport_errors;
    pthread_mutex_unlock(&ipc->mutex); return result;
}
