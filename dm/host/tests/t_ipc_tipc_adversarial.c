/* Independent TIPC malformed-packet/state audit of the production IPC core.
 * No private inputs, SDK code, guest execution, or core encoder is used. */
#include "hle_ipc.h"
#include "ipc_wire_fixture.h"
#include "tipc_adversarial_fixture.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { unsigned prepares; size_t size; } observer;
static unsigned rejected;
static int forbidden_validate(void *ctx, uint64_t a, uint64_t n) {
    (void)ctx; (void)a; (void)n; assert(!"SM must not inspect output memory"); return 0;
}
static void forbidden_fill(void *ctx, uint64_t a, size_t n) {
    (void)ctx; (void)a; (void)n; assert(!"SM must not fill output memory");
}
static void observe_prepare(void *ctx, void *message, size_t size) {
    observer *o = ctx; assert(message); ++o->prepares; o->size = size;
}
static dm_ipc_memory memory(observer *o) {
    return (dm_ipc_memory){o, forbidden_validate, forbidden_fill, observe_prepare};
}
static uint32_t connect_sm(dm_ipc *ipc) {
    uint32_t sm = UINT32_C(0xdeadbeef);
    assert(!dm_ipc_connect_port(ipc, "sm:", 3, &sm));
    assert(sm != UINT32_C(0xdeadbeef) && dm_ipc_owns_handle(sm));
    return sm;
}
static uint32_t exchange(dm_ipc *ipc, uint32_t sm, tipc_a_packet q,
                         size_t capacity, unsigned command, uint32_t result,
                         int moved) {
    unsigned char before[TIPC_A_CAP]; memcpy(before, q.bytes, sizeof before);
    observer o = {0}; dm_ipc_memory ops = memory(&o);
    assert(!dm_ipc_dispatch(ipc, sm, q.bytes, capacity, &ops));
    size_t published = command == 1 ? 20 : 12;
    assert(o.prepares == 1 && o.size == published);
    assert(!memcmp(q.bytes + published, before + published, sizeof before - published));
    return tipc_a_reply(q.bytes, capacity, command, result, moved);
}
static void init(dm_ipc *ipc, uint32_t sm) {
    assert(!exchange(ipc, sm, tipc_a_pid(0), TIPC_A_CAP, 0, 0, 0));
}
static uint32_t get(dm_ipc *ipc, uint32_t sm, uint32_t expected) {
    return exchange(ipc, sm, tipc_a_get("csrng\0\0"), TIPC_A_CAP, 1,
                    expected, !expected);
}
static void reject_packet(dm_ipc *ipc, uint32_t h, tipc_a_packet q, size_t size) {
    assert(size <= TIPC_A_CAP);
    /* Exact backing extent, not merely a short capacity in a larger array. */
    unsigned char *short_buffer = malloc(size ? size : 1);
    assert(short_buffer); memcpy(short_buffer, q.bytes, size);
    observer o = {0}; dm_ipc_memory ops = memory(&o);
    assert(dm_ipc_dispatch(ipc, h, short_buffer, size, &ops));
    assert(!memcmp(short_buffer, q.bytes, size));
    assert(!o.prepares);
    free(short_buffer); ++rejected;
}
static void reject_state(tipc_a_packet q, size_t size, int initialized) {
    dm_ipc *ipc = dm_ipc_create(2); assert(ipc);
    uint32_t sm = connect_sm(ipc);
    if (initialized) init(ipc, sm);
    reject_packet(ipc, sm, q, size);
    uint32_t rng = get(ipc, sm, initialized ? 0 : DM_IPC_INVALID_CLIENT);
    if (!initialized) assert(!dm_ipc_connect_service(ipc, (const uint8_t *)"csrng\0\0", &rng));
    /* The only spare slot remains available: no failed-request allocation. */
    assert(dm_ipc_owns_handle(rng) && !dm_ipc_close(ipc, rng));
    uint16_t size_out = 99;
    assert(!dm_ipc_pointer_size(ipc, sm, &size_out) && !size_out);
    assert(!dm_ipc_close(ipc, sm)); dm_ipc_destroy(ipc);
}
static void malformed_shapes(void) {
    for (unsigned state = 0; state < 2; ++state) {
        tipc_a_packet forms[] = {tipc_a_pid(0), tipc_a_pid(4), tipc_a_get("csrng\0\0"), tipc_a_close()};
        for (unsigned f = 0; f < sizeof forms / sizeof *forms; ++f) {
            for (size_t n = 0; n < forms[f].extent; ++n) reject_state(forms[f], n, state);
            for (unsigned shift = 16; shift <= 28; shift += 4)
                for (unsigned count = 1; count <= 15; ++count) {
                    tipc_a_packet q = forms[f];
                    tipc_a_put(q.bytes, tipc_a_word(q.bytes) | count << shift);
                    reject_state(q, TIPC_A_CAP, state);
                }
            for (unsigned bit = 10; bit < 31; ++bit) {
                tipc_a_packet q = forms[f];
                tipc_a_put(q.bytes + 4, tipc_a_word(q.bytes + 4) | UINT32_C(1) << bit);
                reject_state(q, TIPC_A_CAP, state);
            }
        }
        for (unsigned command = 0; command <= 4; command += 4) {
            tipc_a_packet q = tipc_a_pid(command);
            /* Missing PID, a present-but-empty special header, transferred
             * handles, reserved special flags and nonempty raw payload. */
            tipc_a_put(q.bytes + 4, 0); reject_state(q, TIPC_A_CAP, state);
            q = tipc_a_pid(command); tipc_a_put(q.bytes + 8, 0); reject_state(q, TIPC_A_CAP, state);
            for (unsigned bit = 1; bit < 32; ++bit) {
                q = tipc_a_pid(command); tipc_a_put(q.bytes + 8, 1 | UINT32_C(1) << bit);
                reject_state(q, TIPC_A_CAP, state);
            }
            for (unsigned copies = 0; copies <= 15; ++copies)
                for (unsigned moves = 0; moves <= 15; ++moves) {
                    if (!copies && !moves) continue;
                    q = tipc_a_pid(command); tipc_a_put(q.bytes + 8, 1 | copies << 1 | moves << 5);
                    reject_state(q, TIPC_A_CAP, state);
                }
            for (unsigned words = 1; words <= 63; ++words) {
                q = tipc_a_pid(command); tipc_a_put(q.bytes + 4, UINT32_C(0x80000000) | words);
                reject_state(q, TIPC_A_CAP, state);
            }
        }
        for (unsigned words = 0; words <= 63; ++words) {
            if (words == 2) continue;
            tipc_a_packet q = tipc_a_get("csrng\0\0"); tipc_a_put(q.bytes + 4, words);
            reject_state(q, TIPC_A_CAP, state);
        }
        for (unsigned pid = 0; pid <= 1; ++pid) {
            tipc_a_packet q = tipc_a_get("csrng\0\0");
            tipc_a_put(q.bytes + 4, UINT32_C(0x80000002));
            tipc_a_put(q.bytes + 8, pid);
            reject_state(q, TIPC_A_CAP, state);
            q = tipc_a_close(); tipc_a_put(q.bytes + 4, UINT32_C(0x80000000));
            tipc_a_put(q.bytes + 8, pid); reject_state(q, TIPC_A_CAP, state);
        }
        tipc_a_packet q = tipc_a_close(); tipc_a_put(q.bytes + 4, 1);
        reject_state(q, TIPC_A_CAP, state);
        const unsigned tags[] = {0, 1, 3, 8, 14, 18, 19, 21, 0xffff};
        for (unsigned i = 0; i < sizeof tags / sizeof *tags; ++i) {
            q = tipc_a_get("csrng\0\0"); tipc_a_put(q.bytes, tags[i]);
            reject_state(q, TIPC_A_CAP, state);
        }
    }
    /* CMIF detach shares registration state but retains CMIF framing. Every
     * truncated PID/header/body prefix must also be transactionally rejected. */
    for (unsigned state = 0; state < 2; ++state) {
        const unsigned char zero[8] = {0};
        ipc_test_packet cm = ipc_test_request(4, 4, 1, 0, 0, 0, zero, sizeof zero);
        tipc_a_packet q; memcpy(q.bytes, cm.bytes, sizeof q.bytes); q.extent = cm.extent;
        for (size_t n = 0; n < q.extent; ++n) reject_state(q, n, state);
        cm = ipc_test_request(4, 4, 0, 0, 0, 0, zero, sizeof zero);
        memcpy(q.bytes, cm.bytes, sizeof q.bytes); reject_state(q, TIPC_A_CAP, state);
        cm = ipc_test_request(4, 4, 1, 0, 0, 0, zero, 4);
        memcpy(q.bytes, cm.bytes, sizeof q.bytes); reject_state(q, TIPC_A_CAP, state);
    }
    /* Well-formed GetServiceHandle fits in 16 bytes; its moved-handle reply
     * needs 20. All intervening capacities must fail before allocation. */
    for (size_t n = 16; n < 20; ++n) reject_state(tipc_a_get("csrng\0\0"), n, 1);
}
static void short_error_replies(void) {
    /* The error signature also needs the invalid moved-handle slot. A packet
     * that fits its input but not the static20-byte output must not publish
     * an abbreviated error or consume a session slot. */
    const char invalid[][8] = {{0}, {'c','s','r',0,'n','g',0,0}};
    for (size_t n = 16; n < 20; ++n) {
        reject_state(tipc_a_get("csrng\0\0"), n, 0); /* invalid client */
        for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i)
            reject_state(tipc_a_get(invalid[i]), n, 1);
        reject_state(tipc_a_get("missing\0"), n, 1);
        dm_ipc *ipc = dm_ipc_create(2); assert(ipc);
        uint32_t sm = connect_sm(ipc); init(ipc, sm);
        uint32_t rng = get(ipc, sm, 0);
        reject_packet(ipc, sm, tipc_a_get("csrng\0\0"), n); /* full table */
        exchange(ipc, sm, tipc_a_get("csrng\0\0"), 20, 1, DM_IPC_OUT_OF_HANDLES, 0);
        assert(!dm_ipc_close(ipc, rng));
        uint32_t replacement = get(ipc, sm, 0); assert(replacement != rng);
        assert(!dm_ipc_close(ipc, replacement) && !dm_ipc_close(ipc, sm));
        dm_ipc_destroy(ipc);
    }
}
static ipc_test_reply cmif(dm_ipc *ipc, uint32_t sm, unsigned command) {
    const unsigned char placeholder[8] = {0};
    ipc_test_packet q = ipc_test_request(4, command, 1, 0, 0, 0, placeholder, sizeof placeholder);
    assert(!dm_ipc_dispatch(ipc, sm, q.bytes, sizeof q.bytes, NULL));
    return ipc_test_decode(q.bytes, sizeof q.bytes);
}
static void lifecycle_and_ownership(void) {
    dm_ipc *ipc = dm_ipc_create(4); assert(ipc);
    uint32_t sm = connect_sm(ipc), other = connect_sm(ipc);
    assert(!get(ipc, sm, DM_IPC_INVALID_CLIENT));
    assert(!exchange(ipc, sm, tipc_a_pid(4), 20, 4, 0, 0)); /* idempotent */
    assert(!cmif(ipc, sm, 0).result); /* CMIF init enables TIPC get. */
    uint32_t rng = get(ipc, sm, 0); assert(rng);
    assert(!get(ipc, other, DM_IPC_INVALID_CLIENT)); /* per-session state */
    assert(!exchange(ipc, sm, tipc_a_pid(4), 20, 4, 0, 0));
    assert(!get(ipc, sm, DM_IPC_INVALID_CLIENT));
    uint16_t size_out = 99;
    assert(!dm_ipc_pointer_size(ipc, rng, &size_out) && !size_out); /* issued handle lives */
    init(ipc, sm); assert(!cmif(ipc, sm, 4).result); /* CMIF detach clears TIPC init. */
    assert(!cmif(ipc, sm, 4).result && !get(ipc, sm, DM_IPC_INVALID_CLIENT));
    init(ipc, sm); init(ipc, sm);
    const char invalid[][8] = {{0}, {'c','s','r',0,'n','g',0,0}};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i)
        exchange(ipc, sm, tipc_a_get(invalid[i]), TIPC_A_CAP, 1, DM_IPC_INVALID_SERVICE_NAME, 0);
    exchange(ipc, sm, tipc_a_get("missing\0"), 20, 1, DM_IPC_NOT_REGISTERED, 0);
    uint32_t olsc = exchange(ipc, sm, tipc_a_get("olsc:u\0"), 20, 1, 0, 1);
    exchange(ipc, sm, tipc_a_get("csrng\0\0"), TIPC_A_CAP, 1, DM_IPC_OUT_OF_HANDLES, 0);
    /* Neither new command nor TIPC endpoint close leaks into other services,
     * including the synthetic olsc success/zero-fill policy. */
    uint32_t services[] = {rng, olsc};
    for (unsigned i = 0; i < 2; ++i) {
        reject_packet(ipc, services[i], tipc_a_pid(0), TIPC_A_CAP);
        reject_packet(ipc, services[i], tipc_a_get("csrng\0\0"), TIPC_A_CAP);
        reject_packet(ipc, services[i], tipc_a_pid(4), TIPC_A_CAP);
        reject_packet(ipc, services[i], tipc_a_close(), TIPC_A_CAP);
        size_out = 99; assert(!dm_ipc_pointer_size(ipc, services[i], &size_out) && !size_out);
    }
    tipc_a_packet q = tipc_a_close(), before = q; observer o = {0}; dm_ipc_memory ops = memory(&o);
    assert(dm_ipc_dispatch(ipc, sm, q.bytes, q.extent, &ops) == DM_IPC_SESSION_CLOSED);
    assert(!o.prepares && !memcmp(q.bytes, before.bytes, sizeof q.bytes));
    q = tipc_a_pid(0); before = q;
    assert(dm_ipc_dispatch(ipc, sm, q.bytes, sizeof q.bytes, &ops) == DM_IPC_SESSION_CLOSED);
    assert(!memcmp(q.bytes, before.bytes, sizeof q.bytes));
    uint32_t untouched = UINT32_C(0xdeadbeef);
    assert(dm_ipc_clone(ipc, sm, &untouched) == DM_IPC_SESSION_CLOSED && untouched == UINT32_C(0xdeadbeef));
    assert(!dm_ipc_close(ipc, sm));
    uint32_t replacement = connect_sm(ipc); assert(replacement != sm);
    assert(dm_ipc_dispatch(ipc, sm, q.bytes, sizeof q.bytes, &ops) == DM_IPC_INVALID_HANDLE);
    assert(!get(ipc, replacement, DM_IPC_INVALID_CLIENT));
    assert(!dm_ipc_close(ipc, replacement)); assert(!dm_ipc_close(ipc, other));
    assert(!dm_ipc_close(ipc, rng)); assert(!dm_ipc_close(ipc, olsc)); dm_ipc_destroy(ipc);
}
int main(void) {
    malformed_shapes(); short_error_replies(); lifecycle_and_ownership();
    printf("TIPC independent adversarial wire/state checks passed (%u rejected packets)\n", rejected);
    return 0;
}
