/* Independent adversarial tests of the production IPC core. No private input,
 * guest code execution, Apple framework, or production response encoder. */
#include "hle_ipc.h"
#include "ipc_wire_fixture.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint64_t base, span, seen_address, seen_size;
    unsigned validates, fills, prepares, sequence;
    size_t prepared_size;
    int permit;
    unsigned char output[512];
} memory_test;

static int validate_output(void *ctx, uint64_t address, uint64_t size) {
    memory_test *m = ctx;
    ++m->validates;
    m->seen_address = address;
    m->seen_size = size;
    return m->permit && address >= m->base && size <= m->span &&
           address - m->base <= m->span - size;
}
static void fill_output(void *ctx, uint64_t address, size_t size) {
    memory_test *m = ctx;
    assert(m->validates && m->permit);
    assert(address == m->seen_address && size == m->seen_size);
    ++m->fills;
    /* A fake address-space provider: large descriptor tests must never turn
     * their numeric guest address into a callable/dereferenceable host value. */
    if (size <= sizeof m->output && address - m->base <= sizeof m->output - size)
        for (size_t i = 0; i < size; ++i)
            m->output[address - m->base + i] = (unsigned char)m->sequence++;
}
static void prepare_message(void *ctx, void *message, size_t size) {
    memory_test *m = ctx;
    assert(message != NULL && size <= IPC_TEST_CAP);
    ++m->prepares;
    m->prepared_size = size;
}
static dm_ipc_memory memory_ops(memory_test *m) {
    dm_ipc_memory ops = {m, validate_output, fill_output, prepare_message};
    return ops;
}
static memory_test memory_new(void) {
    memory_test m = {0};
    m.base = UINT64_C(0x1234567800);
    m.span = sizeof m.output;
    m.permit = 1;
    memset(m.output, 0xa5, sizeof m.output);
    return m;
}
static uint32_t service(dm_ipc *ipc, const char name[8]) {
    uint32_t h = 0;
    assert(dm_ipc_connect_service(ipc, (const uint8_t *)name, &h) == 0);
    assert(h && dm_ipc_owns_handle(h));
    return h;
}
static ipc_test_reply send_ok(dm_ipc *ipc, uint32_t h, ipc_test_packet *q,
                              memory_test *m) {
    dm_ipc_memory ops = memory_ops(m);
    unsigned prepared = m->prepares;
    assert(dm_ipc_dispatch(ipc, h, q->bytes, sizeof q->bytes, &ops) == 0);
    assert(m->prepares == prepared + 1);
    ipc_test_reply r = ipc_test_decode(q->bytes, sizeof q->bytes);
    assert(m->prepared_size >= r.raw_end);
    return r;
}
static void reject(dm_ipc *ipc, uint32_t h, ipc_test_packet q, size_t capacity,
                   memory_test *m) {
    unsigned char before[IPC_TEST_CAP], output[sizeof m->output];
    memcpy(before, q.bytes, sizeof before);
    memcpy(output, m->output, sizeof output);
    unsigned fills = m->fills, prepared = m->prepares, sequence = m->sequence;
    dm_ipc_memory ops = memory_ops(m);
    assert(dm_ipc_dispatch(ipc, h, q.bytes, capacity, &ops) != 0);
    assert(memcmp(before, q.bytes, sizeof before) == 0);
    assert(memcmp(output, m->output, sizeof output) == 0);
    assert(m->fills == fills && m->prepares == prepared && m->sequence == sequence);
}
static void reject_prefix(dm_ipc *ipc, uint32_t h, const ipc_test_packet *q,
                          size_t capacity, memory_test *m) {
    /* The allocation ends exactly at the supplied span. ASan can therefore
     * detect a premature descriptor/domain read, not just an incorrect result. */
    unsigned char *bytes = malloc(capacity ? capacity : 1);
    assert(bytes);
    memcpy(bytes, q->bytes, capacity);
    dm_ipc_memory ops = memory_ops(m);
    unsigned fills = m->fills, prepared = m->prepares, sequence = m->sequence;
    assert(dm_ipc_dispatch(ipc, h, bytes, capacity, &ops) != 0);
    assert(memcmp(bytes, q->bytes, capacity) == 0);
    assert(m->fills == fills && m->prepares == prepared && m->sequence == sequence);
    free(bytes);
}

static void bootstrap(void) {
    dm_ipc *ipc = dm_ipc_create(8);
    assert(ipc);
    uint32_t sm = 0, untouched = UINT32_C(0xabcdef01);
    assert(dm_ipc_connect_port(ipc, "bad", 3, &untouched) == DM_IPC_NOT_FOUND);
    assert(untouched == UINT32_C(0xabcdef01));
    assert(dm_ipc_connect_port(ipc, "sm:", 3, &sm) == 0);
    memory_test m = memory_new();
    ipc_test_packet q = ipc_test_pointer_size();
    ipc_test_reply r = send_ok(ipc, sm, &q, &m);
    assert(r.result == 0 && r.payload + 2 <= r.raw_end);
    assert(q.bytes[r.payload] == 0 && q.bytes[r.payload + 1] == 0);
    q = ipc_test_service("csrng\0\0");
    r = send_ok(ipc, sm, &q, &m);
    assert(r.result == DM_IPC_INVALID_CLIENT && r.move_handle == 0);
    q = ipc_test_register();
    assert(q.raw == 20 && q.cmif == 32 && q.extent == 60);
    r = send_ok(ipc, sm, &q, &m);
    assert(r.result == 0 && !r.move_handle);
    q = ipc_test_register();
    assert(send_ok(ipc, sm, &q, &m).result == 0); /* Re-registration. */
    const char invalid_names[][8] = {{0}, {'c','s','r',0,'g',0,0,0}};
    for (unsigned i = 0; i < sizeof invalid_names / sizeof *invalid_names; ++i) {
        untouched = UINT32_C(0xabcdef01);
        assert(dm_ipc_connect_service(ipc, (const uint8_t *)invalid_names[i], &untouched) == DM_IPC_INVALID_SERVICE_NAME);
        assert(untouched == UINT32_C(0xabcdef01));
        q = ipc_test_service(invalid_names[i]);
        r = send_ok(ipc, sm, &q, &m);
        assert(r.result == DM_IPC_INVALID_SERVICE_NAME && !r.move_handle);
    }
    q = ipc_test_service("missing\0");
    r = send_ok(ipc, sm, &q, &m);
    assert(r.result == DM_IPC_NOT_REGISTERED && !r.move_handle);
    q = ipc_test_service("csrng\0\0");
    r = send_ok(ipc, sm, &q, &m);
    assert(r.result == 0 && dm_ipc_owns_handle(r.move_handle));
    uint32_t rng = r.move_handle;
    q = ipc_test_random(m.base + 3, 9);
    r = send_ok(ipc, rng, &q, &m);
    assert(r.result == 0 && !r.move_handle && m.fills == 1);
    for (unsigned i = 0; i < sizeof m.output; ++i)
        assert(m.output[i] == (i >= 3 && i < 12 ? i - 3 : 0xa5));
    assert(dm_ipc_close(ipc, rng) == 0);
    assert(dm_ipc_close(ipc, sm) == 0);
    dm_ipc_destroy(ipc);
}

static void truncation_and_counts(void) {
    dm_ipc *ipc = dm_ipc_create(4);
    uint32_t sm = 0, rng = service(ipc, "csrng\0\0");
    assert(dm_ipc_connect_port(ipc, "sm:", 3, &sm) == 0);
    memory_test m = memory_new();
    ipc_test_packet requests[] = {ipc_test_register(), ipc_test_service("csrng\0\0"),
                                 ipc_test_pointer_size(), ipc_test_random(m.base, 8)};
    for (unsigned k = 0; k < sizeof requests / sizeof *requests; ++k)
        for (size_t n = 0; n < requests[k].extent; ++n)
            reject_prefix(ipc, k == 3 ? rng : sm, &requests[k], n, &m);
    /* Every descriptor-count field independently, every nonzero count. */
    for (unsigned shift = 16; shift <= 28; shift += 4)
        for (unsigned count = 1; count <= 15; ++count) {
            ipc_test_packet q = ipc_test_pointer_size();
            ipc_test_put32(q.bytes, 5 | count << shift);
            reject(ipc, sm, q, sizeof q.bytes, &m);
        }
    for (unsigned recv = 1; recv <= 15; ++recv) {
        ipc_test_packet q = ipc_test_pointer_size();
        ipc_test_put32(q.bytes + 4, 8 | recv << 10);
        reject(ipc, sm, q, sizeof q.bytes, &m);
    }
    for (unsigned copies = 0; copies <= 15; ++copies)
        for (unsigned moves = 0; moves <= 15; ++moves) {
            if (!copies && !moves) continue;
            ipc_test_packet q = ipc_test_register();
            ipc_test_put32(q.bytes + 8, 1 | copies << 1 | moves << 5);
            reject(ipc, sm, q, sizeof q.bytes, &m);
        }
    /* Declared raw extent, not merely physical backing, limits the CMIF body. */
    for (unsigned n = 0; n < 8; ++n) {
        ipc_test_packet q = ipc_test_pointer_size();
        ipc_test_put32(q.bytes + 4, n);
        reject(ipc, sm, q, sizeof q.bytes, &m);
    }
    for (unsigned n = 9; n <= 10; ++n) {
        ipc_test_packet q = ipc_test_register();
        ipc_test_put32(q.bytes + 4, (n - 1) | UINT32_C(0x80000000));
        reject(ipc, sm, q, sizeof q.bytes, &m);
    }
    ipc_test_packet q = ipc_test_random(m.base, 8);
    ipc_test_put32(q.bytes + q.cmif, 0);
    reject(ipc, rng, q, sizeof q.bytes, &m);
    q = ipc_test_random(m.base, 8);
    ipc_test_put32(q.bytes + q.cmif + 4, 2);
    reject(ipc, rng, q, sizeof q.bytes, &m);
    q = ipc_test_random(m.base, 8);
    ipc_test_put32(q.bytes + 4, 1023);
    reject(ipc, rng, q, sizeof q.bytes, &m);
    assert(m.fills == 0);
    /* None of the rejected RegisterClient prefixes may have initialized SM. */
    q = ipc_test_service("csrng\0\0");
    ipc_test_reply r = send_ok(ipc, sm, &q, &m);
    assert(r.result == DM_IPC_INVALID_CLIENT && !r.move_handle);
    assert(dm_ipc_close(ipc, rng) == 0 && dm_ipc_close(ipc, sm) == 0);
    dm_ipc_destroy(ipc);
}

static void descriptors_and_service_errors(void) {
    dm_ipc *ipc = dm_ipc_create(4);
    uint32_t rng = service(ipc, "csrng\0\0");
    memory_test m = memory_new();
    for (unsigned mode = 1; mode != 4; ++mode) {
        ipc_test_packet q = ipc_test_random(m.base, 8);
        ipc_test_put32(q.bytes + 16, ipc_test_get32(q.bytes + 16) | mode);
        reject(ipc, rng, q, sizeof q.bytes, &m);
    }
    m.permit = 0;
    ipc_test_packet q = ipc_test_random(m.base, 8);
    reject(ipc, rng, q, sizeof q.bytes, &m);
    assert(m.seen_address == m.base && m.seen_size == 8);
    /* Exercise all descriptor address and length bits without dereferencing. */
    m.base = UINT64_C(0x2abcdeffedcba98);
    m.span = UINT64_C(0xa76543210);
    q = ipc_test_random(m.base, m.span);
    reject(ipc, rng, q, sizeof q.bytes, &m);
    assert(m.seen_address == m.base && m.seen_size == m.span);
    m = memory_new();
    const size_t lengths[] = {0, 1, 7, 8, 9, 255, 256};
    for (unsigned k = 0; k < sizeof lengths / sizeof *lengths; ++k) {
        q = ipc_test_random(m.base, lengths[k]);
        unsigned old_sequence = m.sequence;
        assert(send_ok(ipc, rng, &q, &m).result == 0);
        assert(m.sequence == old_sequence + lengths[k]);
    }
    /* Empty output need not dereference even a null descriptor address. */
    m.base = m.span = 0;
    q = ipc_test_random(0, 0);
    unsigned old_sequence = m.sequence;
    assert(send_ok(ipc, rng, &q, &m).result == 0);
    assert(m.sequence == old_sequence);
    m.base = UINT64_C(0x1234567800); m.span = sizeof m.output;
    q = ipc_test_request(4, UINT32_C(0x12345678), 0, 0, 0, 0, NULL, 0);
    unsigned fills = m.fills;
    ipc_test_reply r = send_ok(ipc, rng, &q, &m);
    assert(r.result == DM_IPC_UNKNOWN_COMMAND && m.fills == fills && !r.move_handle);
    /* Output overlaps actual in-place IPC message: reject before entropy. */
    q = ipc_test_random(0, 8);
    uint64_t address = (uint64_t)(uintptr_t)(q.bytes + 16);
    ipc_test_put32(q.bytes + 12, (uint32_t)address);
    ipc_test_put32(q.bytes + 16, (uint32_t)((address >> 36) & 0x3fffff) << 2 |
                   (uint32_t)((address >> 32) & 15) << 28);
    /* reject() copies its packet, so dispatch directly for pointer identity. */
    unsigned char saved[IPC_TEST_CAP]; memcpy(saved, q.bytes, sizeof saved);
    dm_ipc_memory ops = memory_ops(&m);
    unsigned prepared = m.prepares;
    assert(dm_ipc_dispatch(ipc, rng, q.bytes, sizeof q.bytes, &ops) != 0);
    assert(memcmp(saved, q.bytes, sizeof saved) == 0);
    assert(m.prepares == prepared && m.fills == fills);
    assert(dm_ipc_close(ipc, rng) == 0);
    dm_ipc_destroy(ipc);
}

static void handles_and_endpoint_close(void) {
    assert(dm_ipc_create(0) == NULL);
    assert(dm_ipc_create(DM_IPC_MAX_SESSIONS + 1) == NULL);
    dm_ipc *ipc = dm_ipc_create(DM_IPC_MAX_SESSIONS);
    uint32_t handles[DM_IPC_MAX_SESSIONS], old, fresh, sentinel;
    for (unsigned i = 0; i < DM_IPC_MAX_SESSIONS; ++i)
        handles[i] = service(ipc, "csrng\0\0");
    sentinel = UINT32_C(0xabcdef01);
    assert(dm_ipc_connect_service(ipc, (const uint8_t *)"csrng\0\0", &sentinel) == DM_IPC_OUT_OF_HANDLES);
    assert(sentinel == UINT32_C(0xabcdef01));
    assert(dm_ipc_clone(ipc, handles[0], &sentinel) == DM_IPC_OUT_OF_HANDLES);
    assert(sentinel == UINT32_C(0xabcdef01));
    old = handles[3];
    assert(dm_ipc_close(ipc, old) == 0);
    fresh = service(ipc, "csrng\0\0");
    assert(fresh != old && dm_ipc_owns_handle(old));
    assert(dm_ipc_close(ipc, old) == DM_IPC_INVALID_HANDLE);
    handles[3] = fresh;
    memory_test m = memory_new();
    ipc_test_packet q = ipc_test_random(m.base, 8);
    reject(ipc, old, q, sizeof q.bytes, &m);
    for (unsigned i = 1; i < DM_IPC_MAX_SESSIONS; ++i)
        assert(dm_ipc_close(ipc, handles[i]) == 0);
    assert(dm_ipc_clone(ipc, handles[0], &fresh) == 0);
    assert(dm_ipc_close(ipc, handles[0]) == 0);
    q = ipc_test_random(m.base, 8);
    assert(send_ok(ipc, fresh, &q, &m).result == 0);
    q = ipc_test_close();
    dm_ipc_memory ops = memory_ops(&m);
    uint32_t rc = dm_ipc_dispatch(ipc, fresh, q.bytes, sizeof q.bytes, &ops);
    assert(rc == 0 || rc == DM_IPC_SESSION_CLOSED);
    q = ipc_test_random(m.base, 8);
    unsigned fills = m.fills;
    assert(dm_ipc_dispatch(ipc, fresh, q.bytes, sizeof q.bytes, &ops) == DM_IPC_SESSION_CLOSED);
    assert(m.fills == fills);
    assert(dm_ipc_close(ipc, fresh) == 0);
    assert(dm_ipc_close(ipc, fresh) == DM_IPC_INVALID_HANDLE);
    dm_ipc_destroy(ipc);

    /* A service-level allocation failure still returns valid SFCO, does not
     * publish a moved handle, and does not damage the SM session. */
    ipc = dm_ipc_create(1);
    uint32_t sm = 0;
    assert(dm_ipc_connect_port(ipc, "sm:", 3, &sm) == 0);
    m = memory_new();
    q = ipc_test_register();
    assert(send_ok(ipc, sm, &q, &m).result == 0);
    for (unsigned i = 0; i < 32; ++i) {
        q = ipc_test_service("csrng\0\0");
        ipc_test_reply r = send_ok(ipc, sm, &q, &m);
        assert(r.result == DM_IPC_OUT_OF_HANDLES && !r.move_handle);
    }
    assert(m.fills == 0);
    assert(dm_ipc_close(ipc, sm) == 0);
    fresh = service(ipc, "csrng\0\0");
    assert(dm_ipc_close(ipc, fresh) == 0);
    dm_ipc_destroy(ipc);
}

static ipc_test_packet domain_request(unsigned close, uint32_t token) {
    ipc_test_packet q = {0};
    q.raw = 8; q.cmif = 32;
    q.extent = close ? 40 : 56;
    ipc_test_put32(q.bytes, token ? 6 : 4);
    ipc_test_put32(q.bytes + 4, close ? 8 : 12);
    ipc_test_put32(q.bytes + 16, close ? 2 : 1 | 16u << 16);
    ipc_test_put32(q.bytes + 20, 1); /* Domain object 1. */
    ipc_test_put32(q.bytes + 28, token);
    if (!close) {
        ipc_test_put32(q.bytes + 32, UINT32_C(0x49434653));
        ipc_test_put32(q.bytes + 36, token != 0);
        ipc_test_put32(q.bytes + 40, 37); /* Existing olsc synthetic policy. */
    }
    return q;
}

static void contexts_controls_and_domains(void) {
    dm_ipc *ipc = dm_ipc_create(8);
    uint32_t rng = service(ipc, "csrng\0\0"), olsc = service(ipc, "olsc:u\0");
    memory_test m = memory_new();
    ipc_test_packet q = ipc_test_random(m.base, 7);
    ipc_test_put32(q.bytes, 6 | UINT32_C(1) << 24);
    ipc_test_put32(q.bytes + q.cmif + 4, 1);
    ipc_test_put32(q.bytes + q.cmif + 12, UINT32_C(0x1234abcd));
    ipc_test_reply r = send_ok(ipc, rng, &q, &m);
    assert(r.result == 0 && r.version == 1 && r.token == UINT32_C(0x1234abcd));
    q = ipc_test_request(7, 3, 0, 0, 0, 0, NULL, 0);
    ipc_test_put32(q.bytes + q.cmif + 12, UINT32_C(0x87654321));
    r = send_ok(ipc, rng, &q, &m);
    assert(r.result == 0 && r.version == 1 && r.token == UINT32_C(0x87654321));
    for (unsigned cmd = 2; cmd <= 4; cmd += 2) {
        unsigned char tag[4] = {0x11, 0x22, 0x33, 0x44};
        q = ipc_test_request(5, cmd, 0, 0, 0, 0, tag, cmd == 4 ? 4 : 0);
        r = send_ok(ipc, rng, &q, &m);
        assert(r.result == 0 && r.move_handle && r.move_handle != rng);
        uint16_t size = 0xffff;
        assert(dm_ipc_pointer_size(ipc, r.move_handle, &size) == 0 && size == 0);
        assert(dm_ipc_close(ipc, r.move_handle) == 0);
    }
    q = ipc_test_request(5, 0, 0, 0, 0, 0, NULL, 0);
    r = send_ok(ipc, olsc, &q, &m);
    assert(r.result == 0 && r.payload + 4 <= r.raw_end);
    assert(ipc_test_get32(q.bytes + r.payload) == 1);
    q = domain_request(0, UINT32_C(0xa1b2c3d4));
    for (size_t n = 0; n < q.extent; ++n) reject_prefix(ipc, olsc, &q, n, &m);
    ipc_test_packet bad = q;
    ipc_test_put32(bad.bytes + 16, 1 | 17u << 16); /* Claimed body too large. */
    reject(ipc, olsc, bad, sizeof bad.bytes, &m);
    bad = q;
    ipc_test_put32(bad.bytes + 16, 1 | 1u << 8 | 16u << 16); /* Input object. */
    reject(ipc, olsc, bad, sizeof bad.bytes, &m);
    bad = q;
    ipc_test_put32(bad.bytes + 20, 2);
    reject(ipc, olsc, bad, sizeof bad.bytes, &m);
    dm_ipc_memory ops = memory_ops(&m);
    assert(dm_ipc_dispatch(ipc, olsc, q.bytes, sizeof q.bytes, &ops) == 0);
    assert(ipc_test_get32(q.bytes + 16) == 0); /* Zero returned domain objects. */
    assert(ipc_test_get32(q.bytes + 32) == UINT32_C(0x4f434653));
    assert(ipc_test_get32(q.bytes + 40) == 0);
    assert(ipc_test_get32(q.bytes + 44) == UINT32_C(0xa1b2c3d4));
    q = domain_request(1, 0);
    assert(dm_ipc_dispatch(ipc, olsc, q.bytes, sizeof q.bytes, &ops) == 0);
    assert(dm_ipc_close(ipc, olsc) == 0 && dm_ipc_close(ipc, rng) == 0);
    dm_ipc_destroy(ipc);
}

typedef struct { dm_ipc *ipc; uint32_t source; } concurrent_test;
static void *clone_send_close(void *opaque) {
    concurrent_test *t = opaque;
    memory_test m = memory_new();
    for (unsigned k = 0; k < 500; ++k) {
        uint32_t h = 0;
        assert(dm_ipc_clone(t->ipc, t->source, &h) == 0);
        ipc_test_packet q = ipc_test_random(m.base, 9);
        assert(send_ok(t->ipc, h, &q, &m).result == 0);
        assert(dm_ipc_close(t->ipc, h) == 0);
        uint16_t untouched = 0xbeef;
        assert(dm_ipc_pointer_size(t->ipc, h, &untouched) == DM_IPC_INVALID_HANDLE);
        assert(untouched == 0xbeef);
    }
    return NULL;
}
static void concurrent_handles(void) {
    dm_ipc *ipc = dm_ipc_create(8);
    concurrent_test t = {ipc, service(ipc, "csrng\0\0")};
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) assert(pthread_create(&threads[i], NULL, clone_send_close, &t) == 0);
    for (unsigned i = 0; i < 4; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(dm_ipc_close(ipc, t.source) == 0);
    dm_ipc_destroy(ipc);
}

static void mutation_safety(void) {
    dm_ipc *ipc = dm_ipc_create(8);
    uint32_t rng = service(ipc, "csrng\0\0");
    memory_test seed = memory_new();
    ipc_test_packet original = ipc_test_random(seed.base + 16, 16);
    for (size_t byte = 0; byte < original.extent; ++byte)
        for (unsigned bit = 0; bit < 8; ++bit) {
            ipc_test_packet q = original;
            q.bytes[byte] ^= (unsigned char)(1u << bit);
            unsigned char before[IPC_TEST_CAP]; memcpy(before, q.bytes, sizeof before);
            memory_test m = memory_new();
            dm_ipc_memory ops = memory_ops(&m);
            uint32_t rc = dm_ipc_dispatch(ipc, rng, q.bytes, sizeof q.bytes, &ops);
            if (rc) {
                assert(memcmp(before, q.bytes, sizeof before) == 0);
                assert(m.fills == 0 && m.prepares == 0 && m.sequence == 0);
            } else {
                ipc_test_reply r = ipc_test_decode(q.bytes, sizeof q.bytes);
                if (r.result) assert(m.fills == 0 && m.sequence == 0);
                if (r.move_handle) assert(dm_ipc_close(ipc, r.move_handle) == 0);
            }
        }
    /* Deterministic random garbage with exact allocation-size redzones. */
    uint32_t random = UINT32_C(0x8291cafe);
    for (unsigned k = 0; k < 10000; ++k) {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        size_t size = random % (IPC_TEST_CAP + 1);
        unsigned char *bytes = malloc(size ? size : 1), before[IPC_TEST_CAP];
        assert(bytes);
        for (size_t j = 0; j < size; ++j) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            bytes[j] = before[j] = (unsigned char)(random >> 24);
        }
        memory_test m = memory_new(); m.permit = 0;
        dm_ipc_memory ops = memory_ops(&m);
        uint32_t rc = dm_ipc_dispatch(ipc, rng, bytes, size, &ops);
        if (rc) assert(memcmp(before, bytes, size) == 0 && m.prepares == 0);
        assert(m.fills == 0 && m.sequence == 0);
        free(bytes);
    }
    assert(dm_ipc_close(ipc, rng) == 0);
    dm_ipc_destroy(ipc);
}

int main(void) {
    bootstrap();
    truncation_and_counts();
    descriptors_and_service_errors();
    handles_and_endpoint_close();
    contexts_controls_and_domains();
    concurrent_handles();
    mutation_safety();
    puts("IPC independent wire/adversarial tests passed");
    return 0;
}
