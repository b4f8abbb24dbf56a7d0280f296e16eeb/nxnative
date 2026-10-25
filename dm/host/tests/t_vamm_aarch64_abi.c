// Handwritten synthetic ABI probe only. No extracted/reconstructed SDK bytes.
#include "hle_vamm.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
_Static_assert(sizeof(dm_vamm_resource_usage) == 16 && offsetof(dm_vamm_resource_usage, used) == 8, "ResourceUsage layout");
#if defined(__aarch64__)
#ifdef __APPLE__
#define ABI_SYMBOL "_dm_vamm_synthetic_usage"
#else
#define ABI_SYMBOL "dm_vamm_synthetic_usage"
#endif
__asm__(".text\n.p2align 2\n.globl " ABI_SYMBOL "\n" ABI_SYMBOL ":\n"
        "mov x1, #0x5678\nret\n");
extern dm_vamm_resource_usage dm_vamm_synthetic_usage(const void *self);
#endif
int main(void) {
#if defined(__aarch64__)
    unsigned char self[72] = {0};
    dm_vamm_resource_usage (*volatile target)(const void *) = dm_vamm_synthetic_usage;
    dm_vamm_resource_usage r = target(self);
    assert(r.total == (uintptr_t)self && r.used == 0x5678);
    puts("Vamm AArch64 x0/self + x0/x1 resource result fixture passed");
#else
    puts("SKIP: Vamm live x0/x1 ABI fixture requires native AArch64");
#endif
}
