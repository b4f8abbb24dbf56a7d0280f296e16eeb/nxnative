#ifndef NVN_POOL_REGISTRATION_H
#define NVN_POOL_REGISTRATION_H
#include <stdint.h>
/* Registration identity is independent of CPU storage. Zero is reserved for
 * legacy pointer-keyed registrations (including sparse runs); ordinary tokens
 * never recycle, even when their metadata slot or CPU bytes do. Caller serializes
 * the issuer; this helper allocates neither storage nor registration metadata. */
static inline int nvn_pool_token_take(uint64_t *next, uint64_t *token) {
    if (!next || !token || !*next) return 0;
    *token = *next;
    *next = *next == UINT64_MAX ? 0 : *next + 1;
    return 1;
}
static inline int nvn_pool_registration_match(uint64_t token, const void *key,
                                               uint64_t wanted, const void *mem) {
    return wanted ? token == wanted : !token && key == mem;
}
#endif
