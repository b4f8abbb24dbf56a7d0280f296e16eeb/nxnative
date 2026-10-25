// Handwritten synthetic ABI probe, not copied SDK/game instructions. On native
// AArch64, prove a 24-byte C return consumes x8 while self remains in x0. This
// fixture intentionally skips other architectures rather than claiming x86
// aggregate-return tests validate the guest/Apple AArch64 boundary.
#include "hle_standard_allocator_adapter.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
_Static_assert(sizeof(dm_sa_hash)==24 && offsetof(dm_sa_hash,digest)==16, "Hash layout");
#if defined(__aarch64__)
#ifdef __APPLE__
#define ABI_SYMBOL "_dm_sa_synthetic_hash_x8"
#else
#define ABI_SYMBOL "dm_sa_synthetic_hash_x8"
#endif
__asm__(".text\n.p2align 2\n.globl " ABI_SYMBOL "\n" ABI_SYMBOL ":\n"
        "mov x9, #0x1234\n"
        "stp x0, x9, [x8]\n"
        "mov x9, #0x5678\n"
        "str x9, [x8, #16]\n"
        "ret\n");
extern dm_sa_hash dm_sa_synthetic_hash_x8(const void *self);
#endif
int main(void) {
#if defined(__aarch64__)
    unsigned char self[56]={0};
    dm_sa_hash (*volatile target)(const void *)=dm_sa_synthetic_hash_x8;
    dm_sa_hash h=target(self); uint64_t digest; memcpy(&digest,h.digest,8);
    assert(h.block_count==(uintptr_t)self && h.allocated_bytes==0x1234 && digest==0x5678);
    puts("StandardAllocator AArch64 x0/self + x8/24-byte result fixture passed");
#else
    puts("SKIP: StandardAllocator live x8 ABI fixture requires native AArch64");
#endif
}
