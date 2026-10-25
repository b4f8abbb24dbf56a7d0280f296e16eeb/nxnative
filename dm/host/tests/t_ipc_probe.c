/* Independent synthetic checks of the default-off CSRNG observer. This uses
 * numeric fixture addresses and never executes an SDK or game function. */
#include "hle_ipc.h"
#include "ipc_wire_fixture.h"
#include <pthread.h>
#include <stdio.h>

typedef struct {
    unsigned validates, fills, prepares;
    uint64_t bytes, address, size;
    int permit;
} access_test;
static int validate(void *ctx, uint64_t address, uint64_t size) {
    access_test *a = ctx;
    ++a->validates; a->address = address; a->size = size;
    return a->permit;
}
static void fill(void *ctx, uint64_t address, size_t size) {
    access_test *a = ctx;
    assert(a->permit && a->validates && a->address == address && a->size == size);
    ++a->fills; a->bytes += size; /* No numeric address is dereferenced. */
}
static void prepare(void *ctx, void *message, size_t size) {
    access_test *a = ctx;
    assert(message && size > 0 && size <= 256); ++a->prepares;
}
static dm_ipc_memory ops(access_test *a) {
    dm_ipc_memory m = {a, validate, fill, prepare}; return m;
}
static const uint64_t output = UINT64_C(0x1234567811);
static ipc_test_packet request(uint64_t address, size_t size) {
    return ipc_test_request(6, 0, 0, address, size, 1, NULL, 0);
}
static uint32_t connect_rng(dm_ipc *ipc) {
    uint32_t h = 0;
    assert(dm_ipc_connect_service(ipc, (const uint8_t *)"csrng\0\0", &h) == 0);
    return h;
}
static dm_ipc_csrng_probe end(dm_ipc *ipc) {
    dm_ipc_csrng_probe p;
    memset(&p, 0xa5, sizeof p);
    assert(dm_ipc_csrng_probe_end(ipc, &p) == 0);
    return p;
}
static void send_success(dm_ipc *ipc, uint32_t h, ipc_test_packet q, access_test *a) {
    dm_ipc_memory m = ops(a);
    assert(dm_ipc_dispatch(ipc, h, q.bytes, sizeof q.bytes, &m) == 0);
    assert(ipc_test_decode(q.bytes, sizeof q.bytes).result == 0);
}
static void clean(const dm_ipc_csrng_probe *p) {
    assert(p->transport_errors == 0 && p->service_errors == 0 && p->mismatches == 0);
}

static void inactive_and_validation(void) {
    dm_ipc *ipc = dm_ipc_create(4);
    uint32_t h = connect_rng(ipc);
    access_test a = {.permit = 1};
    /* Inactive observer must not impose diagnostic size/version/type policy. */
    dm_ipc_csrng_probe_route(ipc, DM_IPC_PROBE_ROUTES + 20);
    send_success(ipc, h, ipc_test_random(output, 5000), &a);
    assert(a.fills == 1 && a.bytes == 5000);
    dm_ipc_csrng_probe p, before;
    memset(&p, 0xa5, sizeof p); before = p;
    assert(dm_ipc_csrng_probe_end(ipc, &p) == DM_IPC_NOT_SUPPORTED);
    assert(memcmp(&p, &before, sizeof p) == 0);
    assert(dm_ipc_csrng_probe_begin(NULL, output, 1) == DM_IPC_INVALID_HANDLE);
    assert(dm_ipc_csrng_probe_begin(ipc, output, 4098) == DM_IPC_INVALID_SIZE);
    assert(dm_ipc_csrng_probe_begin(ipc, UINT64_MAX, 1) == DM_IPC_INVALID_SIZE);
    assert(dm_ipc_csrng_probe_end(ipc, &p) == DM_IPC_NOT_SUPPORTED);
    assert(memcmp(&p, &before, sizeof p) == 0);
    assert(dm_ipc_csrng_probe_begin(ipc, output, 1) == 0);
    assert(dm_ipc_csrng_probe_begin(ipc, output + 16, 17) == DM_IPC_NOT_SUPPORTED);
    assert(dm_ipc_csrng_probe_end(ipc, NULL) == DM_IPC_INVALID_MEMORY);
    send_success(ipc, h, request(output, 1), &a);
    p = end(ipc); clean(&p);
    assert(p.requests == 1 && p.filled_bytes == 1 && p.sessions_before == 1 && p.sessions_after == 1);
    assert(p.csrng_connections == 0 && p.sm_requests == 0);
    for (unsigned k = 0; k < DM_IPC_PROBE_ROUTES; ++k) assert(p.route[k] == 0);
    /* Ending the probe immediately restores non-diagnostic behavior. */
    send_success(ipc, h, ipc_test_random(output + 64, 5000), &a);
    assert(dm_ipc_close(ipc, h) == 0); dm_ipc_destroy(ipc);
}

static void successful_sizes_and_routes(void) {
    dm_ipc *ipc = dm_ipc_create(8);
    uint32_t h = 0;
    const size_t sizes[] = {0, 1, 17, 4097};
    for (unsigned i = 0; i < 4; ++i) {
        assert(dm_ipc_csrng_probe_begin(ipc, output, sizes[i]) == 0);
        if (i == 0) {
            dm_ipc_csrng_probe_route(ipc, DM_IPC_PROBE_ADAPTER_CONNECT);
            h = connect_rng(ipc);
        }
        dm_ipc_csrng_probe_route(ipc, DM_IPC_PROBE_ADAPTER_SEND);
        access_test a = {.permit = 1};
        send_success(ipc, h, request(output, sizes[i]), &a);
        dm_ipc_csrng_probe p = end(ipc); clean(&p);
        assert(a.fills == 1 && a.bytes == sizes[i]);
        assert(p.requests == 1 && p.filled_bytes == sizes[i]);
        assert(p.csrng_connections == (i == 0));
        assert(p.sessions_before == (i != 0) && p.sessions_after == 1);
        assert(p.route[DM_IPC_PROBE_ADAPTER_CONNECT] == (i == 0));
        assert(p.route[DM_IPC_PROBE_ADAPTER_SEND] == 1);
    }
    /* An empty descriptor may be canonicalized to null, but not another nonzero address. */
    assert(dm_ipc_csrng_probe_begin(ipc, output, 0) == 0);
    access_test a = {.permit = 1}; send_success(ipc, h, request(0, 0), &a);
    dm_ipc_csrng_probe p = end(ipc); clean(&p);
    assert(p.requests == 1 && p.filled_bytes == 0);
    assert(dm_ipc_close(ipc, h) == 0); dm_ipc_destroy(ipc);
}

static void rejected_shapes(void) {
    dm_ipc *ipc = dm_ipc_create(4);
    uint32_t h = connect_rng(ipc);
    for (unsigned which = 0; which < 10; ++which) {
        size_t wanted = which == 8 ? 0 : 17;
        assert(dm_ipc_csrng_probe_begin(ipc, output, wanted) == 0);
        ipc_test_packet q = request(output, wanted);
        size_t capacity = sizeof q.bytes;
        if (which == 0 || which == 8) q = request(output + 1, wanted);
        if (which == 1) q = request(output, wanted + 1);
        if (which == 2) ipc_test_put32(q.bytes, 4 | UINT32_C(1) << 24);
        if (which == 3) ipc_test_put32(q.bytes + q.cmif + 4, 0);
        if (which == 4) ipc_test_put32(q.bytes + 4, 9);
        if (which == 5) capacity = 255;
        if (which == 6) capacity = 512;
        if (which == 7) q = request(0, wanted);
        if (which == 9) capacity = 52; /* Entire request, wrong transport capacity. */
        unsigned char message[512], saved[512];
        memset(message, 0x5a, sizeof message); memcpy(message, q.bytes, sizeof q.bytes);
        memcpy(saved, message, sizeof saved);
        access_test a = {.permit = 1}; dm_ipc_memory m = ops(&a);
        assert(dm_ipc_dispatch(ipc, h, message, capacity, &m) == DM_IPC_NOT_SUPPORTED);
        assert(memcmp(message, saved, sizeof saved) == 0);
        assert(a.validates == 0 && a.fills == 0 && a.prepares == 0);
        dm_ipc_csrng_probe p = end(ipc);
        assert(p.requests == 1 && p.mismatches == 1 && p.transport_errors == 1);
        assert(p.service_errors == 0 && p.filled_bytes == 0);
    }
    /* One successful request does not authorize a second write in the same call. */
    assert(dm_ipc_csrng_probe_begin(ipc, output, 17) == 0);
    access_test a = {.permit = 1}; send_success(ipc, h, request(output, 17), &a);
    ipc_test_packet q = request(output, 17), saved = q; dm_ipc_memory m = ops(&a);
    assert(dm_ipc_dispatch(ipc, h, q.bytes, sizeof q.bytes, &m) == DM_IPC_NOT_SUPPORTED);
    assert(memcmp(q.bytes, saved.bytes, sizeof q.bytes) == 0);
    assert(a.validates == 1 && a.fills == 1 && a.prepares == 1);
    dm_ipc_csrng_probe p = end(ipc);
    assert(p.requests == 2 && p.filled_bytes == 17 && p.mismatches == 1 && p.transport_errors == 1);
    assert(dm_ipc_close(ipc, h) == 0); dm_ipc_destroy(ipc);
}

static void result_and_connection_accounting(void) {
    dm_ipc *ipc = dm_ipc_create(8);
    uint32_t h = connect_rng(ipc), sm = 0;
    assert(dm_ipc_connect_port(ipc, "sm:", 3, &sm) == 0);
    assert(dm_ipc_csrng_probe_begin(ipc, output, 17) == 0);
    access_test a = {.permit = 1}; dm_ipc_memory m = ops(&a);
    ipc_test_packet q = request(output, 17);
    assert(dm_ipc_dispatch(ipc, 0, q.bytes, sizeof q.bytes, &m) == DM_IPC_INVALID_HANDLE);
    q = ipc_test_request(6, 97, 0, 0, 0, 0, NULL, 0);
    assert(dm_ipc_dispatch(ipc, h, q.bytes, sizeof q.bytes, &m) == 0);
    assert(ipc_test_decode(q.bytes, sizeof q.bytes).result == DM_IPC_UNKNOWN_COMMAND);
    q = ipc_test_register(); send_success(ipc, sm, q, &a);
    uint32_t duplicate = 0;
    assert(dm_ipc_clone(ipc, h, &duplicate) == 0);
    assert(dm_ipc_close(ipc, duplicate) == 0);
    for (unsigned route = 0; route < DM_IPC_PROBE_ROUTES; ++route) dm_ipc_csrng_probe_route(ipc, route);
    dm_ipc_csrng_probe_route(ipc, DM_IPC_PROBE_ROUTES);
    dm_ipc_csrng_probe p = end(ipc);
    assert(p.transport_errors == 1 && p.service_errors == 1 && p.sm_requests == 1);
    assert(p.csrng_connections == 1 && p.sessions_before == 2 && p.sessions_after == 2);
    assert(p.requests == 0 && p.filled_bytes == 0 && p.mismatches == 1);
    for (unsigned route = 0; route < DM_IPC_PROBE_ROUTES; ++route) assert(p.route[route] == 1);
    /* Valid observer shape does not bypass the underlying memory validator. */
    assert(dm_ipc_csrng_probe_begin(ipc, output, 17) == 0);
    a = (access_test){0}; m = ops(&a); q = request(output, 17);
    ipc_test_packet saved = q;
    assert(dm_ipc_dispatch(ipc, h, q.bytes, sizeof q.bytes, &m) == DM_IPC_INVALID_MEMORY);
    assert(memcmp(q.bytes, saved.bytes, sizeof q.bytes) == 0 && a.validates == 1 && a.fills == 0 && a.prepares == 0);
    p = end(ipc); assert(p.requests == 1 && p.transport_errors == 1 && p.mismatches == 0 && p.filled_bytes == 0);
    assert(dm_ipc_close(ipc, sm) == 0 && dm_ipc_close(ipc, h) == 0); dm_ipc_destroy(ipc);
}

typedef struct { dm_ipc *ipc; uint32_t handle; } owner_test;
static void *nonowner(void *opaque) {
    owner_test *t = opaque;
    assert(dm_ipc_csrng_probe_begin(t->ipc, output, 17) == DM_IPC_NOT_SUPPORTED);
    dm_ipc_csrng_probe p, saved;
    memset(&p, 0xa5, sizeof p); saved = p;
    assert(dm_ipc_csrng_probe_end(t->ipc, &p) == DM_IPC_NOT_SUPPORTED);
    assert(memcmp(&p, &saved, sizeof p) == 0);
    dm_ipc_csrng_probe_route(t->ipc, DM_IPC_PROBE_ADAPTER_SEND);
    ipc_test_packet q = request(output, 17), before = q;
    access_test a = {.permit = 1}; dm_ipc_memory m = ops(&a);
    assert(dm_ipc_dispatch(t->ipc, t->handle, q.bytes, sizeof q.bytes, &m) == DM_IPC_NOT_SUPPORTED);
    assert(memcmp(q.bytes, before.bytes, sizeof q.bytes) == 0);
    assert(a.validates == 0 && a.fills == 0 && a.prepares == 0);
    return NULL;
}
static void owner_concurrency(void) {
    dm_ipc *ipc = dm_ipc_create(4);
    owner_test t = {ipc, connect_rng(ipc)};
    for (unsigned k = 0; k < 32; ++k) {
        assert(dm_ipc_csrng_probe_begin(ipc, output, 17) == 0);
        pthread_t thread; assert(pthread_create(&thread, NULL, nonowner, &t) == 0);
        assert(pthread_join(thread, NULL) == 0);
        dm_ipc_csrng_probe p = end(ipc);
        assert(p.requests == 1 && p.transport_errors == 1 && p.mismatches == 2);
        assert(p.route[DM_IPC_PROBE_ADAPTER_SEND] == 1 && p.filled_bytes == 0);
        assert(p.sessions_before == 1 && p.sessions_after == 1);
        /* A fresh owner probe resets rejected other-thread evidence. */
        assert(dm_ipc_csrng_probe_begin(ipc, output, 17) == 0);
        access_test a = {.permit = 1}; send_success(ipc, t.handle, request(output, 17), &a);
        p = end(ipc); clean(&p); assert(p.requests == 1 && p.filled_bytes == 17);
    }
    assert(dm_ipc_close(ipc, t.handle) == 0); dm_ipc_destroy(ipc);
}
int main(void) {
    inactive_and_validation();
    successful_sizes_and_routes();
    rejected_shapes();
    result_and_connection_accounting();
    owner_concurrency();
    puts("CSRNG observer independent synthetic tests passed");
    return 0;
}
