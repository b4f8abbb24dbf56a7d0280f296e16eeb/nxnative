// Exercise the actual SVC dispatch/backend, not a second implementation.
// Only unrelated host services and the failed-mprotect injection are stubbed.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../dm.h"

#ifndef __APPLE__
static void *pthread_get_stackaddr_np(pthread_t thread) { (void)thread; return NULL; }
static size_t pthread_get_stacksize_np(pthread_t thread) { (void)thread; return 0; }
#endif
static _Atomic int fail_protect;
static int test_mprotect(void *address, size_t size, int permission) {
    if (atomic_exchange(&fail_protect, 0)) { errno = EACCES; return -1; }
    return mprotect(address, size, permission);
}
static _Noreturn void test_heap_abort(void);
#define abort test_heap_abort
#define mprotect test_mprotect
#include "../hle_svc.c"
#undef mprotect
#undef abort

module_t g_mod[MAX_MOD];
int g_nmod;
struct dm_flags g_flag;
static size_t write_prepares;
static uintptr_t prepared_base;
static size_t prepared_size;
void dm_guest_write_prepare(void *p, size_t size) {
    // Called under the physical-map lock. A failed protection operation must
    // not notify the dirty tracker; a successful one covers the whole host run.
    ++write_prepares; prepared_base = (uintptr_t)p; prepared_size = size;
}
module_t *module_of(const void *p) { (void)p; return NULL; }
void hlecount_svc(int num) { (void)num; }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void guest_clock_read_wait(void) {}
uint64_t guest_ns(void) { return 0; }
uint64_t dm_entropy64(void) { return 0; }
int thr_limit_used(void) { return 0; }
int dm_ue_heap_owns(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; } // No Unreal allocator in this fixture.

#include "memory_failure_test.h"

#include "heap_failure_test.h"

static uint64_t info(unsigned id) {
    uint64_t regs[31] = {0}; regs[1] = id;
    svc_dispatch(0x29, regs, 0); assert(!regs[0]); return regs[1];
}
static uint64_t pages(int commit, uintptr_t a, size_t n) {
    uint64_t regs[31] = {a, n};
    svc_dispatch(commit ? 0x2c : 0x2d, regs, 0); return regs[0];
}
static uint64_t heap(size_t n) {
    uint64_t regs[31] = {0, n};
    svc_dispatch(1, regs, 0); return regs[0];
}
static void query(uintptr_t a, uintptr_t expected_base, size_t expected_size, unsigned state) {
    uint64_t mi[5] = {0}, regs[31] = {(uintptr_t)mi, 0, a};
    svc_dispatch(6, regs, 0);
    assert(!regs[0] && !regs[1]);
    assert(mi[0] == expected_base && mi[1] == expected_size);
    assert(((uint32_t *)mi)[4] == state);
    assert(((uint32_t *)mi)[6] == (state ? 3u : 0u));
}
static void *init_worker(void *arg) {
    uintptr_t *out = arg; *out = info(I_AliasAddr); return NULL;
}
static void *map_worker(void *arg) {
    uintptr_t a = (uintptr_t)arg;
    for (unsigned i = 0; i < 400; ++i) {
        assert(!pages(1, a, 0x4000));
        assert(!pages(1, a, 0x4000));
        *(volatile uint32_t *)a = i;
        assert(*(volatile uint32_t *)a == i);
        assert(info(I_UsedMem) >= DM_GAME_MEM_USED);
        uint64_t mi[5], regs[31] = {(uintptr_t)mi, 0, a};
        svc_dispatch(6, regs, 0); assert(!regs[0] && ((uint32_t *)mi)[4] == 5);
        assert(!pages(0, a, 0x4000));
        assert(!pages(0, a, 0x4000));
    }
    return NULL;
}
int main(void) {
    pthread_t threads[8]; uintptr_t aliases[8];
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&threads[i], NULL, init_worker, &aliases[i]));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(threads[i], NULL));
    uintptr_t a = aliases[0], as = info(I_AslrAddr), h = info(I_HeapAddr);
    for (unsigned i = 0; i < 8; ++i) assert(aliases[i] == a);
    assert(a == as + 0x10000000 && info(I_AliasSize) == DM_GAME_MEM_ALIAS);
    assert(info(I_AslrSize) == DM_GAME_MEM_RESERVE);
    assert(info(I_TotalMem) == DM_GAME_APP_POOL);
    assert(info(I_UsedMem) == DM_GAME_MEM_USED);

    assert(!pages(1, a + 0x4000, 0x8000));
    assert(write_prepares == 1 && prepared_base == a + 0x4000 && prepared_size == 0x8000);
    memset((void *)(a + 0x4000), 0x5a, 0x8000);
    assert(!pages(1, a + 0x8000, 0x8000));
    assert(alias_mapped() == 0xc000);
    assert(*(volatile uint8_t *)(a + 0x8000) == 0x5a);
    assert(*(volatile uint8_t *)(a + 0xc000) == 0);
    assert(info(I_UsedMem) == DM_GAME_MEM_USED + (DM_GAME_MEM_MAP_LIMIT ? 0xc000 : 0));
    query(a + 0x8000, a + 0x4000, 0xc000, 5);
    query(a, as, a + 0x4000 - as, 0);
    query(as, as, a + 0x4000 - as, 0);
    query(a + 0x10000, a + 0x10000, h - a - 0x10000, 0);
    query((uintptr_t)g_stack, a + 0x10000, h - a - 0x10000, 0);
    assert(!pages(0, a + 0x8000, 0x4000));
    query(a + 0x8000, a + 0x8000, 0x4000, 0);
    query(a + 0xc000, a + 0xc000, 0x4000, 5);
    assert(*(volatile uint8_t *)(a + 0x4000) == 0x5a);

    size_t prepares_before = write_prepares;
    EXPECT_MEMORY_PROTECTION_FAILURE(pages(1, a + 0x8000, 0x4000), (void *)(a + 0x4000), 0x4000);
    assert(alias_mapped() == 0x8000);
    assert(write_prepares == prepares_before);
    query(a + 0x8000, a + 0x8000, 0x4000, 0);
    EXPECT_MEMORY_PROTECTION_FAILURE(pages(0, a + 0x4000, 0x4000), (void *)(a + 0x4000), 0x4000);
    assert(alias_mapped() == 0x8000);
    assert(*(volatile uint8_t *)(a + 0x4000) == 0x5a);
    query(a + 0x4000, a + 0x4000, 0x4000, 5);
    assert(!pages(1, a + 0x8000, 0x4000));
    for (size_t i = 0; i < 0x4000; ++i) assert(*(volatile uint8_t *)(a + 0x8000 + i) == 0);
    assert(*(volatile uint8_t *)(a + 0x4000) == 0x5a);
    assert(!pages(0, a, 0x10000) && !pages(0, a, 0x10000));
    assert(alias_mapped() == 0);
    assert(pages(1, a, 0) == 0xd401);
    assert(pages(1, a + 1, 0x1000) == 0xd401);
    assert(pages(1, a, 0x1001) == 0xd401);
    assert(pages(1, a - 0x1000, 0x1000) == 0xd401);
    assert(pages(1, a + DM_GAME_MEM_ALIAS - 0x1000, 0x2000) == 0xd401);
    assert(pages(1, UINTPTR_MAX - 0xfff, 0x2000) == 0xd401);
    assert(pages(1, a, SIZE_MAX & ~(size_t)0xfff) == 0xd401);
    assert(alias_mapped() == 0);

    assert(!heap(0x200000));
    *(volatile uint32_t *)h = 123;
    expect_heap_failure(0x400000, (const void *)h, sizeof(uint32_t));
    assert(atomic_load(&g_heap_cur) == 0x200000);
    assert(*(volatile uint32_t *)h == 123);
    assert(info(I_UsedMem) == DM_GAME_MEM_USED + 0x200000);
    assert(!pages(1, a, 0xc00000));
    size_t before = alias_mapped();
    if (DM_GAME_MEM_MAP_LIMIT) {
        assert(pages(1, a + 0xc00000, 0x1000) == 0x10801);
        assert(alias_mapped() == before);
        assert(heap(0x400000) == 0x10801 && atomic_load(&g_heap_cur) == 0x200000);
    } else {
        assert(!pages(1, a + 0xc00000, 0x1000));
        assert(!pages(0, a + 0xc00000, 0x1000));
        assert(alias_mapped() == before);
    }
    assert(!pages(1, a, 0xc00000)); // repeated maps never consume budget twice
    assert(!pages(0, a, 0xc00000));
    assert(!heap(0));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&threads[i], NULL, map_worker, (void *)(a + i * 0x10000)));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(threads[i], NULL));
    assert(alias_mapped() == 0 && info(I_UsedMem) == DM_GAME_MEM_USED);
    free(g_alias_bits); free(g_alias_exposed);
    assert(!munmap(g_as, RESERVE));
    printf("production SVC address-space tests passed (mem_map_limit=%d)\n", DM_GAME_MEM_MAP_LIMIT);
    return 0;
}
