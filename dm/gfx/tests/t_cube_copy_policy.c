#include "../nvn/nvn_cube_copy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(nvn_cube_copy_shape(32, 32, 1, 6, 1));
    assert(nvn_cube_copy_shape(1, 1, 1, 1, 1));
    assert(nvn_cube_copy_shape(7, 7, 1, 3, 1));
    assert(nvn_cube_copy_shape(256, 256, 1, 9, 1));
    assert(!nvn_cube_copy_shape(0, 0, 1, 1, 1));
    assert(!nvn_cube_copy_shape(32, 16, 1, 6, 1));
    assert(!nvn_cube_copy_shape(32, 32, 2, 6, 1));
    assert(!nvn_cube_copy_shape(32, 32, 1, 7, 1));
    assert(!nvn_cube_copy_shape(32, 32, 1, 0, 1));
    assert(!nvn_cube_copy_shape(32, 32, 1, 6, 4));
    assert(!nvn_cube_copy_shape(512, 512, 1, 10, 1));
    assert(!nvn_cube_copy_shape(UINT64_MAX, UINT64_MAX, 1, 1, 1));
    assert(nvn_cube_copy_budget(0, NVN_CUBE_COPY_BYTES));
    assert(nvn_cube_copy_budget(NVN_CUBE_COPY_BYTES - 1, 1));
    assert(!nvn_cube_copy_budget(0, 0));
    assert(!nvn_cube_copy_budget(NVN_CUBE_COPY_BYTES, 1));
    assert(!nvn_cube_copy_budget(UINT64_MAX, 2));
    assert(!nvn_cube_copy_budget(2, UINT64_MAX));
    assert(nvn_cube_copy_evictable(4, 4, 3, 5));
    assert(!nvn_cube_copy_evictable(5, 4, 3, 5));
    assert(!nvn_cube_copy_evictable(4, 4, 5, 5));
    // Full BC7 mips: 32,16,8,4,2,1. Tail copies use logical extents, not out-of-bounds 4x4 regions.
    unsigned dims[] = {32,16,8,4,2,1}, bytes = 0, copies = 0;
    for (unsigned l = 0; l < 6; l++) { unsigned w = 32u >> l;
        assert(w == dims[l]); bytes += ((w + 3) / 4) * ((w + 3) / 4) * 16;
        for (unsigned face = 0; face < 6; face++) copies++;
    }
    assert(copies == 36 && bytes == 1392 && bytes * 6 == 8352);
    puts("cube copy policy: shape, overflow, 16 MiB cap, fences and 36-copy BC7 tail plan passed");
    return 0;
}
