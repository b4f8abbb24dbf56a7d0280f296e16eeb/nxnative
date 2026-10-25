/* Independent, synthetic HIPC/CMIF/TIPC wire producer for the IPC tests.
 * Protocol references (not copied implementation code):
 * https://github.com/switchbrew/libnx/blob/master/nx/include/switch/sf/hipc.h
 * https://github.com/switchbrew/libnx/blob/master/nx/include/switch/sf/cmif.h
 * https://github.com/switchbrew/libnx/blob/b156b4774fb2df7b93402169509266da7547fc4c/nx/include/switch/sf/tipc.h
 * All addresses are supplied by the fixture; no guest binary is involved. */
#ifndef DM_TEST_IPC_WIRE_FIXTURE_H
#define DM_TEST_IPC_WIRE_FIXTURE_H
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { IPC_TEST_CAP = 256 };
typedef struct {
    unsigned char bytes[IPC_TEST_CAP];
    size_t raw, cmif, extent;
} ipc_test_packet;

static inline uint32_t ipc_test_get32(const void *p) {
    const unsigned char *b = p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 |
           (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static inline void ipc_test_put32(void *p, uint32_t v) {
    unsigned char *b = p;
    for (unsigned i = 0; i != 4; ++i) b[i] = (unsigned char)(v >> (8 * i));
}
static inline void ipc_test_put64(void *p, uint64_t v) {
    ipc_test_put32(p, (uint32_t)v);
    ipc_test_put32((unsigned char *)p + 4, (uint32_t)(v >> 32));
}

/* Only the supported non-domain request shapes are produced here. Malformed
 * cases modify these independently constructed packets, not a core encoder. */
static inline ipc_test_packet ipc_test_request(unsigned type, uint32_t command,
                                             int pid, uint64_t address,
                                             uint64_t length, int out_buffer,
                                             const void *payload, size_t size) {
    ipc_test_packet q = {0};
    assert(type <= 0xffff && size <= 128);
    assert(address < (UINT64_C(1) << 58));
    assert(length < (UINT64_C(1) << 36));
    size_t pos = 8;
    if (pid) {
        ipc_test_put32(q.bytes + pos, 1);
        /* The kernel, not this client-controlled placeholder, supplies PID. */
        ipc_test_put64(q.bytes + pos + 4, UINT64_C(0xfefdfcfbfaf9f8f7));
        pos += 12;
    }
    if (out_buffer) {
        ipc_test_put32(q.bytes + pos, (uint32_t)length);
        ipc_test_put32(q.bytes + pos + 4, (uint32_t)address);
        ipc_test_put32(q.bytes + pos + 8,
                       (uint32_t)((address >> 36) & 0x3fffff) << 2 |
                       (uint32_t)((length >> 32) & 15) << 24 |
                       (uint32_t)((address >> 32) & 15) << 28);
        pos += 12;
    }
    q.raw = pos;
    q.cmif = (pos + 15) & ~(size_t)15;
    /* CMIF always reserves sixteen alignment-padding bytes in raw_count,
     * including when fewer bytes precede its aligned input header. */
    size_t raw_words = (16 + 16 + size + 3) / 4;
    q.extent = q.raw + raw_words * 4;
    assert(q.extent <= sizeof q.bytes);
    ipc_test_put32(q.bytes, type | (out_buffer ? UINT32_C(1) << 24 : 0));
    ipc_test_put32(q.bytes + 4, (uint32_t)raw_words |
                   (pid ? UINT32_C(1) << 31 : 0));
    ipc_test_put32(q.bytes + q.cmif, UINT32_C(0x49434653));
    ipc_test_put32(q.bytes + q.cmif + 4, type == 6 || type == 7);
    ipc_test_put32(q.bytes + q.cmif + 8, command);
    if (size) memcpy(q.bytes + q.cmif + 16, payload, size);
    return q;
}

static inline ipc_test_packet ipc_test_register(void) {
    const unsigned char zero[8] = {0};
    return ipc_test_request(4, 0, 1, 0, 0, 0, zero, sizeof zero);
}
static inline ipc_test_packet ipc_test_service(const char name[8]) {
    return ipc_test_request(4, 1, 0, 0, 0, 0, name, 8);
}
static inline ipc_test_packet ipc_test_random(uint64_t address, uint64_t length) {
    return ipc_test_request(4, 0, 0, address, length, 1, NULL, 0);
}
static inline ipc_test_packet ipc_test_pointer_size(void) {
    return ipc_test_request(5, 3, 0, 0, 0, 0, NULL, 0);
}
static inline ipc_test_packet ipc_test_close(void) {
    ipc_test_packet q = {0};
    ipc_test_put32(q.bytes, 2);
    q.raw = q.extent = 8;
    return q;
}

/* TIPC has no magic or alignment padding. These packets deliberately have
 * nonzero slack, making accidental CMIF parsing/overpublication observable. */
static inline ipc_test_packet ipc_test_tipc(unsigned command, int pid,
                                          const void *payload, size_t size) {
    ipc_test_packet q;
    memset(&q, 0xa5, sizeof q);
    assert(command <= 0xffef && size <= 128 && size % 4 == 0);
    q.raw = pid ? 20 : 8; q.cmif = 0; q.extent = q.raw + size;
    ipc_test_put32(q.bytes, command + 16);
    ipc_test_put32(q.bytes + 4, (uint32_t)(size / 4) | (pid ? UINT32_C(0x80000000) : 0));
    if (pid) {
        ipc_test_put32(q.bytes + 8, 1);
        ipc_test_put64(q.bytes + 12, UINT64_C(0xfefdfcfbfaf9f8f7));
    }
    if (size) memcpy(q.bytes + q.raw, payload, size);
    return q;
}
static inline ipc_test_packet ipc_test_tipc_close(void) {
    ipc_test_packet q = ipc_test_close();
    ipc_test_put32(q.bytes, 15);
    return q;
}

typedef struct {
    uint32_t result, version, token, move_handle;
    size_t payload, raw_end;
} ipc_test_reply;

static inline ipc_test_reply ipc_test_tipc_decode(const void *buffer, size_t size,
                                                 unsigned command) {
    const unsigned char *b = buffer;
    ipc_test_reply r = {0};
    assert(size >= 12 && ipc_test_get32(b) == command + 16);
    uint32_t header = ipc_test_get32(b + 4);
    assert(header == (command == 1 ? UINT32_C(0x80000001) : 1));
    size_t raw = 8;
    if (header >> 31) {
        assert(size >= 20 && ipc_test_get32(b + 8) == 32);
        r.move_handle = ipc_test_get32(b + 12);
        raw = 16;
    }
    r.result = ipc_test_get32(b + raw);
    if (command == 1) assert(r.result ? r.move_handle == 0 : r.move_handle != 0);
    r.payload = r.raw_end = raw + 4;
    return r;
}

/* Deliberately independent response decoder: checks only the supported reply
 * shapes and exposes exact raw bounds so tests cannot read zero-filled slack. */
static inline ipc_test_reply ipc_test_decode(const void *buffer, size_t size) {
    const unsigned char *b = buffer;
    ipc_test_reply r = {0};
    assert(size >= 8);
    uint32_t a = ipc_test_get32(b), c = ipc_test_get32(b + 4);
    assert(a == 0); /* Reply tag 0; no X/A/B/W descriptors. */
    assert((c & UINT32_C(0x7ffffc00)) == 0);
    size_t pos = 8;
    if (c >> 31) {
        assert(size >= 16);
        assert(ipc_test_get32(b + pos) == 32); /* One moved handle only. */
        r.move_handle = ipc_test_get32(b + pos + 4);
        assert(r.move_handle != 0);
        pos += 8;
    }
    r.raw_end = pos + (c & 1023) * 4;
    assert(r.raw_end <= size);
    pos = (pos + 15) & ~(size_t)15;
    assert(pos + 16 <= r.raw_end);
    assert(ipc_test_get32(b + pos) == UINT32_C(0x4f434653));
    r.version = ipc_test_get32(b + pos + 4);
    r.result = ipc_test_get32(b + pos + 8);
    r.token = ipc_test_get32(b + pos + 12);
    r.payload = pos + 16;
    return r;
}
#endif
