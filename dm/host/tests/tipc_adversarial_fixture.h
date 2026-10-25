/* Independent, synthetic TIPC byte fixtures: no production serializer, SDK
 * input, or guest code. Wire contracts reviewed against libnx sf/tipc.h and
 * Atmosphere 6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8, tipc command serialization.
 * Use nonzero backing slack to expose over-publication and CMIF alignment. */
#ifndef DM_TEST_TIPC_ADVERSARIAL_FIXTURE_H
#define DM_TEST_TIPC_ADVERSARIAL_FIXTURE_H
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { TIPC_A_CAP = 256 };
typedef struct { unsigned char bytes[TIPC_A_CAP]; size_t extent; } tipc_a_packet;
static inline uint32_t tipc_a_word(const unsigned char *p) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)p[i] << (i * 8);
    return value;
}
static inline void tipc_a_put(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (i * 8));
}
static inline tipc_a_packet tipc_a_pid(unsigned command) {
    tipc_a_packet q;
    memset(&q, 0xa6, sizeof q);
    assert(command == 0 || command == 4);
    tipc_a_put(q.bytes, 16 + command);
    tipc_a_put(q.bytes + 4, UINT32_C(0x80000000));
    tipc_a_put(q.bytes + 8, 1);
    /* A deliberately forged placeholder must not select another process. */
    tipc_a_put(q.bytes + 12, UINT32_C(0xfedcba98));
    tipc_a_put(q.bytes + 16, UINT32_C(0x76543210));
    q.extent = 20;
    return q;
}
static inline tipc_a_packet tipc_a_get(const char name[8]) {
    tipc_a_packet q;
    memset(&q, 0xa6, sizeof q);
    tipc_a_put(q.bytes, 17);
    tipc_a_put(q.bytes + 4, 2);
    memcpy(q.bytes + 8, name, 8);
    q.extent = 16;
    return q;
}
static inline tipc_a_packet tipc_a_close(void) {
    tipc_a_packet q;
    memset(&q, 0xa6, sizeof q);
    tipc_a_put(q.bytes, 15);
    tipc_a_put(q.bytes + 4, 0);
    q.extent = 8;
    return q;
}
/* GetServiceHandle has one static moved-handle slot even on service error.
 * InvalidHandle (zero) conveys no ownership; Result follows at byte16. Other
 * supported commands have a bare Result at byte8. No CMIF padding. */
static inline uint32_t tipc_a_reply(const unsigned char *p, size_t capacity,
                                   unsigned command, uint32_t result,
                                   int moved) {
    int move_slot = command == 1;
    assert(moved == (move_slot && !result));
    assert(capacity >= (move_slot ? 20u : 12u));
    assert(tipc_a_word(p) == 16 + command);
    assert(tipc_a_word(p + 4) == (move_slot ? UINT32_C(0x80000001) : 1));
    if (!move_slot) { assert(tipc_a_word(p + 8) == result); return 0; }
    assert(tipc_a_word(p + 8) == 32);
    assert((tipc_a_word(p + 12) != 0) == moved);
    assert(tipc_a_word(p + 16) == result);
    return tipc_a_word(p + 12);
}

#endif
