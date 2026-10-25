/* Synthetic TIPC SM compatibility tests. No original SDK or game executes. */
#include "hle_ipc.h"
#include "ipc_wire_fixture.h"
#include <stdio.h>

static unsigned prepares;
static size_t prepared_size;
static void prepare(void *context, void *message, size_t size) {
    (void)context; assert(message && size <= IPC_TEST_CAP);
    ++prepares; prepared_size = size;
}
static const dm_ipc_memory memory = {.prepare_message = prepare};
static ipc_test_reply tipc(dm_ipc *ipc, uint32_t sm, unsigned command, int pid,
                           const void *data, size_t size) {
    ipc_test_packet q = ipc_test_tipc(command, pid, data, size);
    unsigned char before[IPC_TEST_CAP]; memcpy(before, q.bytes, sizeof before);
    unsigned old = prepares;
    assert(!dm_ipc_dispatch(ipc, sm, q.bytes, sizeof q.bytes, &memory));
    ipc_test_reply r = ipc_test_tipc_decode(q.bytes, sizeof q.bytes, command);
    assert(prepares == old + 1 && prepared_size == r.raw_end);
    assert(!memcmp(before + r.raw_end, q.bytes + r.raw_end, sizeof before - r.raw_end));
    return r;
}
static ipc_test_reply cmif(dm_ipc *ipc, uint32_t h, ipc_test_packet q) {
    assert(!dm_ipc_dispatch(ipc, h, q.bytes, sizeof q.bytes, &memory));
    return ipc_test_decode(q.bytes, sizeof q.bytes);
}
static uint32_t port(dm_ipc *ipc) {
    uint32_t h = 0;
    assert(!dm_ipc_connect_port(ipc, "sm:", 3, &h)); return h;
}
static void protocols_and_registration(void) {
    dm_ipc *ipc = dm_ipc_create(8); assert(ipc);
    uint32_t a = port(ipc), b = port(ipc);
    assert(tipc(ipc, a, 1, 0, "csrng\0\0", 8).result == DM_IPC_INVALID_CLIENT);
    // Repeated detach is valid even before registration. PID placeholder bits
    // are never an authentication mechanism in this single-process host.
    assert(!tipc(ipc, a, 4, 1, NULL, 0).result);
    assert(!tipc(ipc, a, 4, 1, NULL, 0).result);
    assert(!tipc(ipc, a, 0, 1, NULL, 0).result);
    assert(!tipc(ipc, a, 0, 1, NULL, 0).result);
    assert(tipc(ipc, b, 1, 0, "csrng\0\0", 8).result == DM_IPC_INVALID_CLIENT);
    assert(tipc(ipc, a, 1, 0, "\0\0\0\0\0\0\0", 8).result == DM_IPC_INVALID_SERVICE_NAME);
    assert(tipc(ipc, a, 1, 0, "cs\0rng\0", 8).result == DM_IPC_INVALID_SERVICE_NAME);
    assert(tipc(ipc, a, 1, 0, "missing\0", 8).result == DM_IPC_NOT_REGISTERED);
    ipc_test_reply r = tipc(ipc, a, 1, 0, "csrng\0\0", 8);
    assert(!r.result && dm_ipc_owns_handle(r.move_handle));
    uint32_t rng = r.move_handle;
    assert(!cmif(ipc, rng, ipc_test_pointer_size()).result);
    // Detach across protocols changes only a's registration, not existing
    // service sessions or b's independent registration.
    assert(!cmif(ipc, b, ipc_test_register()).result);
    const uint64_t placeholder = 0;
    assert(!cmif(ipc, a, ipc_test_request(4, 4, 1, 0, 0, 0, &placeholder, 8)).result);
    assert(tipc(ipc, a, 1, 0, "csrng\0\0", 8).result == DM_IPC_INVALID_CLIENT);
    assert(!cmif(ipc, rng, ipc_test_pointer_size()).result);
    r = tipc(ipc, b, 1, 0, "olsc:u\0", 8);
    assert(!r.result && r.move_handle); assert(!dm_ipc_close(ipc, r.move_handle));
    assert(!cmif(ipc, a, ipc_test_register()).result);
    assert(!tipc(ipc, a, 4, 1, NULL, 0).result);
    assert(cmif(ipc, a, ipc_test_service("csrng\0\0")).result == DM_IPC_INVALID_CLIENT);
    assert(!tipc(ipc, a, 0, 1, NULL, 0).result);
    r = cmif(ipc, a, ipc_test_service("csrng\0\0"));
    assert(!r.result && r.move_handle); assert(!dm_ipc_close(ipc, r.move_handle));
    ipc_test_packet q = ipc_test_tipc_close(), original = q;
    unsigned old = prepares;
    assert(dm_ipc_dispatch(ipc, a, q.bytes, sizeof q.bytes, &memory) == DM_IPC_SESSION_CLOSED);
    assert(!memcmp(q.bytes, original.bytes, sizeof q.bytes) && prepares == old);
    q = ipc_test_register(); original = q;
    assert(dm_ipc_dispatch(ipc, a, q.bytes, sizeof q.bytes, &memory) == DM_IPC_SESSION_CLOSED);
    assert(!memcmp(q.bytes, original.bytes, sizeof q.bytes) && prepares == old);
    assert(!dm_ipc_close(ipc, a));
    assert(dm_ipc_close(ipc, a) == DM_IPC_INVALID_HANDLE);
    assert(!cmif(ipc, rng, ipc_test_pointer_size()).result);
    assert(!dm_ipc_close(ipc, rng) && !dm_ipc_close(ipc, b));
    dm_ipc_destroy(ipc);
}
static void response_capacity_and_exhaustion(void) {
    dm_ipc *ipc = dm_ipc_create(2); assert(ipc);
    uint32_t sm = port(ipc);
    assert(!tipc(ipc, sm, 0, 1, NULL, 0).result);
    // Request occupies16 bytes, successful reply needs20. Reject before
    // allocation: one session slot must remain available after all failures.
    for (size_t n = 16; n < 20; ++n) {
        ipc_test_packet q = ipc_test_tipc(1, 0, "csrng\0\0", 8), old = q;
        unsigned count = prepares;
        assert(dm_ipc_dispatch(ipc, sm, q.bytes, n, &memory) == DM_IPC_INVALID_SIZE);
        assert(!memcmp(q.bytes, old.bytes, sizeof q.bytes) && prepares == count);
    }
    ipc_test_reply r = tipc(ipc, sm, 1, 0, "csrng\0\0", 8);
    assert(!r.result && r.move_handle); uint32_t rng = r.move_handle;
    for (unsigned i = 0; i < 8; ++i) {
        r = tipc(ipc, sm, 1, 0, "csrng\0\0", 8);
        assert(r.result == DM_IPC_OUT_OF_HANDLES && !r.move_handle && r.raw_end == 20);
    }
    assert(!dm_ipc_close(ipc, rng));
    r = tipc(ipc, sm, 1, 0, "csrng\0\0", 8);
    assert(!r.result && r.move_handle != rng);
    assert(!dm_ipc_close(ipc, r.move_handle) && !dm_ipc_close(ipc, sm));
    dm_ipc_destroy(ipc);
}
int main(void) {
    protocols_and_registration();
    response_capacity_and_exhaustion();
    puts("TIPC SM registration, detach, capacity and shared-session tests passed");
    return 0;
}
