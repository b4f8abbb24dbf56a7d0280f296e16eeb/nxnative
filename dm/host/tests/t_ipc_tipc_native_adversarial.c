/* Reuse the existing production-SVC fixture's synthetic memory/stub providers,
 * but not its test body or TIPC helpers. No thread/TLS lifecycle is under test. */
#define main existing_ipc_native_fixture_main
#include "t_ipc_native.c"
#undef main
#include "tipc_adversarial_fixture.h"

static uint32_t native_tipc(unsigned route, uint32_t handle, uint8_t *tlr,
                            tipc_a_packet q, unsigned command,
                            uint32_t service_result, int moved) {
    uint8_t *buffer = route == 0x21 ? tlr : g_heap + 0x4000;
    size_t capacity = route == 0x21 ? DM_IPC_TLS_CAPACITY : 4096;
    memset(buffer, 0xb7, capacity); memcpy(buffer, q.bytes, sizeof q.bytes);
    uint8_t before[4096]; memcpy(before, buffer, capacity);
    unsigned prep = prepared, entropy = entropy_calls;
    assert(!svc3(route, route == 0x21 ? handle : (uintptr_t)buffer,
                 route == 0x21 ? 0 : capacity, route == 0x21 ? 0 : handle));
    assert(prepared == prep + 1 && entropy_calls == entropy);
    size_t publish = command == 1 ? 20 : 12;
    assert(!memcmp(buffer + publish, before + publish, capacity - publish));
    return tipc_a_reply(buffer, capacity, command, service_result, moved);
}
static uint32_t adapter_tipc(uint32_t handle, uint8_t *tlr, tipc_a_packet q,
                             unsigned command, uint32_t result, int moved) {
    memcpy(tlr, q.bytes, sizeof q.bytes);
    unsigned prep = prepared, entropy = entropy_calls;
    assert(!h_sf_Send(handle, (uint32_t *)tlr, DM_IPC_TLS_CAPACITY));
    assert(prepared == prep + 1 && entropy_calls == entropy);
    size_t publish = command == 1 ? 20 : 12;
    assert(!memcmp(tlr + publish, q.bytes + publish, sizeof q.bytes - publish));
    return tipc_a_reply(tlr, DM_IPC_TLS_CAPACITY, command, result, moved);
}
static void adapter_short_get_reject(uint32_t sm, uint8_t *tlr, tipc_a_packet q) {
    dm_ipc_csrng_probe probe;
    assert(!dm_ipc_csrng_probe_begin(dm_ipc_host(), (uintptr_t)(g_heap + 0x1000), 0));
    for (size_t capacity = 16; capacity < 20; ++capacity) {
        memcpy(tlr, q.bytes, sizeof q.bytes);
        unsigned prep = prepared, entropy = entropy_calls;
        assert(h_sf_Send(sm, (uint32_t *)tlr, capacity) == DM_IPC_INVALID_SIZE);
        assert(!memcmp(tlr, q.bytes, sizeof q.bytes));
        assert(prepared == prep && entropy_calls == entropy);
    }
    assert(!dm_ipc_csrng_probe_end(dm_ipc_host(), &probe));
    assert(probe.sessions_before == probe.sessions_after && !probe.csrng_connections);
    assert(probe.transport_errors == 4 && !probe.filled_bytes);
}
static ipc_test_packet cmif_detach_packet(void) {
    const uint8_t zero[8] = {0};
    return ipc_test_request(4, 4, 1, 0, 0, 0, zero, sizeof zero);
}
int main(void) {
    assert(!pthread_key_create(&g_keytlr, NULL));
    uint8_t *tlr = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(tlr != MAP_FAILED && !pthread_setspecific(g_keytlr, tlr));
    mem_init(); assert(!svc3(1, 0, 4 * 1024 * 1024, 0));
    strcpy((char *)g_heap, "sm:");
    uint64_t r[31] = {UINT64_C(0xdeadbeef), (uintptr_t)g_heap};
    svc(0x1f, r); assert(!r[0] && dm_ipc_owns_handle((uint32_t)r[1]));
    uint32_t sm = (uint32_t)r[1];
    adapter_short_get_reject(sm, tlr, tipc_a_get("csrng\0\0"));
    native_tipc(0x21, sm, tlr, tipc_a_pid(0), 0, 0, 0);
    const char invalid[8] = {0};
    adapter_short_get_reject(sm, tlr, tipc_a_get(invalid));
    adapter_short_get_reject(sm, tlr, tipc_a_get("missing\0"));
    uint32_t rng = native_tipc(0x22, sm, tlr, tipc_a_get("csrng\0\0"), 1, 0, 1);
    uint32_t duplicate = 0; assert(!h_sf_Dup(&duplicate, rng));
    h_sf_Close(rng);
    memset(g_heap + 0x1000, 0xa9, 0x100);
    random_request(duplicate, tlr, g_heap + 0x1001, 31, 0);
    assert(g_heap[0x1000] == 0xa9 && g_heap[0x1020] == 0xa9 && entropy_calls == 1);

    /* TIPC initialization is visible to CMIF and adapters. */
    ipc_test_packet cm = ipc_test_service("csrng\0\0");
    memcpy(tlr, cm.bytes, sizeof cm.bytes);
    assert(!h_sf_Send(sm, (uint32_t *)tlr, DM_IPC_TLS_CAPACITY));
    ipc_test_reply cr = ipc_test_decode(tlr, DM_IPC_TLS_CAPACITY);
    assert(!cr.result && cr.move_handle); assert(!svc3(0x16, cr.move_handle, 0, 0));
    adapter_tipc(sm, tlr, tipc_a_pid(4), 4, 0, 0);
    assert(native_request(sm, tlr, ipc_test_service("csrng\0\0")).result == DM_IPC_INVALID_CLIENT);
    native_tipc(0x22, sm, tlr, tipc_a_get("csrng\0\0"), 1, DM_IPC_INVALID_CLIENT, 0);
    random_request(duplicate, tlr, g_heap + 0x1021, 7, 0); /* detach preserves issued handle */
    assert(!native_request(sm, tlr, ipc_test_register()).result);
    rng = adapter_tipc(sm, tlr, tipc_a_get("csrng\0\0"), 1, 0, 1);
    h_sf_Close(rng);
    cm = cmif_detach_packet(); memcpy(g_heap + 0x4000, cm.bytes, sizeof cm.bytes);
    assert(!svc3(0x22, (uintptr_t)(g_heap + 0x4000), 4096, sm));
    assert(!ipc_test_decode(g_heap + 0x4000, 4096).result);
    adapter_tipc(sm, tlr, tipc_a_get("csrng\0\0"), 1, DM_IPC_INVALID_CLIENT, 0);
    assert(!native_request(sm, tlr, cmif_detach_packet()).result); /* idempotent */
    adapter_tipc(sm, tlr, tipc_a_pid(0), 0, 0, 0);

    /* Malformed detach does not drop registration, write a reply or entropy. */
    tipc_a_packet q = tipc_a_pid(4); tipc_a_put(q.bytes + 4, UINT32_C(0x80000001));
    memcpy(tlr, q.bytes, sizeof q.bytes);
    unsigned prep = prepared, entropy = entropy_calls;
    assert(svc3(0x21, sm, 0, 0) != 0);
    assert(!memcmp(tlr, q.bytes, sizeof q.bytes));
    assert(prepared == prep && entropy_calls == entropy);
    rng = native_tipc(0x21, sm, tlr, tipc_a_get("csrng\0\0"), 1, 0, 1); h_sf_Close(rng);

    /* The packet fits this typed adapter buffer but the handle reply does not.
     * The shared owner must not consume a slot before the capacity check. */
    dm_ipc_csrng_probe probe;
    assert(!dm_ipc_csrng_probe_begin(dm_ipc_host(), (uintptr_t)(g_heap + 0x1000), 0));
    q = tipc_a_get("csrng\0\0"); memcpy(tlr, q.bytes, sizeof q.bytes); prep = prepared;
    assert(h_sf_Send(sm, (uint32_t *)tlr, 16) == DM_IPC_INVALID_SIZE);
    assert(!memcmp(tlr, q.bytes, sizeof q.bytes) && prepared == prep);
    assert(!dm_ipc_csrng_probe_end(dm_ipc_host(), &probe));
    assert(probe.sessions_before == probe.sessions_after && !probe.csrng_connections);
    assert(probe.transport_errors == 1 && !probe.filled_bytes);

    /* Explicit-buffer kernel checks are enforced before parsing the TIPC body. */
    memcpy(g_heap + 0x4000, q.bytes, sizeof q.bytes); prep = prepared;
    assert(svc3(0x22, (uintptr_t)g_heap + 0x4001, 4096, sm) == DM_IPC_INVALID_ADDRESS);
    assert(svc3(0x22, (uintptr_t)g_heap + 0x4000, 256, sm) == DM_IPC_INVALID_SIZE);
    assert(!memcmp(g_heap + 0x4000, q.bytes, sizeof q.bytes) && prepared == prep);
    rng = native_tipc(0x22, sm, tlr, tipc_a_get("csrng\0\0"), 1, 0, 1); h_sf_Close(rng);

    q = tipc_a_close(); memcpy(g_heap + 0x4000, q.bytes, sizeof q.bytes); prep = prepared;
    assert(svc3(0x22, (uintptr_t)g_heap + 0x4000, 4096, sm) == DM_IPC_SESSION_CLOSED);
    assert(!memcmp(g_heap + 0x4000, q.bytes, sizeof q.bytes) && prepared == prep);
    q = tipc_a_pid(0); memcpy(tlr, q.bytes, sizeof q.bytes);
    assert(svc3(0x21, sm, 0, 0) == DM_IPC_SESSION_CLOSED);
    assert(!memcmp(tlr, q.bytes, sizeof q.bytes) && prepared == prep);
    h_sf_Close(sm);
    assert(svc3(0x21, sm, 0, 0) == DM_IPC_INVALID_HANDLE && !other_closes);
    random_request(duplicate, tlr, g_heap + 0x1031, 3, 0);
    assert(!svc3(0x16, duplicate, 0, 0));
    assert(!pthread_setspecific(g_keytlr, NULL)); pthread_key_delete(g_keytlr);
    dm_ipc_destroy(g_ipc); free(g_alias_bits); free(g_alias_exposed);
    assert(!munmap(g_as, RESERVE) && !munmap(tlr, 0x4000));
    puts("TIPC independent native SVC 0x21/0x22 and shared-owner checks passed");
    return 0;
}
