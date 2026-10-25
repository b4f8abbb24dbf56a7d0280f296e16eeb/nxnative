#ifndef NVN_POOL_RANGE_H
#define NVN_POOL_RANGE_H
#include <stdint.h>

/* A lookup names at least one byte inside a nonempty half-open pool. */
static inline int nvn_pool_span(uint64_t lo, uint64_t hi, uint64_t addr, uint64_t len) {
    return len && lo < hi && addr >= lo && addr < hi && len <= hi - addr;
}

/* Validate before adding the size or rounding up the end for a no-copy buffer.
 * Failed registrations leave the outputs unchanged. */
static inline int nvn_pool_page_range(uintptr_t lo, uint64_t size, uintptr_t page,
                                      uintptr_t *hi, uintptr_t *base, uintptr_t *end) {
    if (!lo || !size || !page || (page & (page - 1)) || size > UINTPTR_MAX - lo)
        return 0;
    uintptr_t h = lo + (uintptr_t)size;
    if (h > UINTPTR_MAX - (page - 1)) return 0;
    *hi = h;
    *base = lo & ~(page - 1);
    *end = (h + page - 1) & ~(page - 1);
    return 1;
}
#endif
