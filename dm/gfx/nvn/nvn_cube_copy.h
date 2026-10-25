// Bounded admission policy for the opt-in 2D-to-six-face compatibility copy. No Metal dependency.
#ifndef NVN_CUBE_COPY_H
#define NVN_CUBE_COPY_H
#include <stdint.h>
enum { NVN_CUBE_COPY_SLOTS = 16, NVN_CUBE_COPY_EDGE = 256, NVN_CUBE_COPY_BYTES = 16 * 1024 * 1024 };
static inline int nvn_cube_copy_shape(uint64_t w, uint64_t h, uint64_t layers, uint64_t levels, uint64_t samples) {
    if (!w || w > NVN_CUBE_COPY_EDGE || w != h || layers != 1 || samples != 1 || !levels) return 0;
    unsigned max_levels = 1; for (uint64_t n = w; n > 1; n >>= 1) max_levels++;
    return levels <= max_levels;
}
static inline int nvn_cube_copy_budget(uint64_t used, uint64_t need) {
    return need && used <= NVN_CUBE_COPY_BYTES && need <= NVN_CUBE_COPY_BYTES - used;
}
static inline int nvn_cube_copy_evictable(uint64_t fence, uint64_t done, uint64_t prepared, uint64_t use) {
    return fence <= done && prepared != use;
}
#endif
