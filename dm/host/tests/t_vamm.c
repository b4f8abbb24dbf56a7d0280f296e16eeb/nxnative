// Host reservation owner through the REAL production SVC manager. All
// callbacks are native synthetic source; no SDK binary/code is read or executed.
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
#include "../hle_standard_allocator_provider.h"
#include "../hle_vamm_adapter.h"
#include "../hle_vamm_native.h"

#ifndef __APPLE__
static void *pthread_get_stackaddr_np(pthread_t thread) { (void)thread; return NULL; }
static size_t pthread_get_stacksize_np(pthread_t thread) { (void)thread; return 0; }
#endif
static size_t host_page(void) {
#ifdef DM_TEST_HOST_PAGE_16K
    assert(getpagesize() <= 0x4000); return 0x4000;
#else
    return (size_t)getpagesize();
#endif
}
static int test_getpagesize(void) { return (int)host_page(); }
static void *test_mmap(void *hint, size_t size, int prot, int flags, int fd, off_t off) {
    assert(!hint && fd == -1 && !off);
    size_t pg = host_page();
    void *raw = mmap(NULL, size + pg, prot, flags, fd, off);
    if (raw == MAP_FAILED) return raw;
    uintptr_t lo = (uintptr_t)raw, a = (lo + pg - 1) & ~(uintptr_t)(pg - 1);
    if (lo != a) assert(!munmap(raw, a - lo));
    assert(!munmap((void *)(a + size), lo + pg - a)); return (void *)a;
}
static _Atomic int fail_protect;
static int test_mprotect(void *address, size_t size, int permission) {
    assert(!((uintptr_t)address & (host_page() - 1)) && !(size & (host_page() - 1)));
    if (atomic_exchange(&fail_protect, 0)) { errno = EACCES; return -1; }
    return mprotect(address, size, permission);
}
#define getpagesize test_getpagesize
#define mmap test_mmap
#define mprotect test_mprotect
#include "../hle_svc.c"
#undef mprotect
#undef mmap
#undef getpagesize

module_t g_mod[MAX_MOD];
int g_nmod;
struct dm_flags g_flag;
void dm_guest_write_prepare(void *p, size_t size) { (void)p; (void)size; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void hlecount_svc(int num) { (void)num; }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void guest_clock_read_wait(void) {}
uint64_t guest_ns(void) { return 0; }
uint64_t dm_entropy64(void) { return 0; }
int thr_limit_used(void) { return 0; }

#include "memory_failure_test.h"

static uint64_t call(unsigned svc, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t regs[31] = {a, b, c}; svc_dispatch(svc, regs, 0); return regs[0];
}
static uint64_t info(unsigned id) {
    uint64_t regs[31] = {0, id}; svc_dispatch(0x29, regs, 0); assert(!regs[0]); return regs[1];
}
static uint64_t pages(int commit, uintptr_t base, size_t size) {
    return call(commit ? 0x2c : 0x2d, base, size, 0);
}
static void query_state(uintptr_t address, unsigned state, unsigned attr, unsigned permission) {
    svc_memory_info mi;
    assert(!call(6, (uintptr_t)&mi, 0, address));
    assert(mi.state == state && mi.attr == attr && mi.perm == permission);
}

enum { PAGE = 0x1000, SMALL = 0x10000, LARGE = 0x200000 };
typedef struct {
    dm_vamm *v; int disabled; unsigned page_calls;
    int override_resources; dm_vamm_resource_usage override;
} fixture;
static uint32_t change(void *context, uintptr_t base, size_t size, int commit) {
    fixture *f = context; ++f->page_calls;
    // Synthetic context/counters, but the verified native result policy.
    return dm_vamm_native_svc_result((uint32_t)pages(commit, base, size), commit);
}
static dm_vamm_resource_usage resources(void *context) {
    fixture *f = context;
    if (f->override_resources) return f->override;
    if (f->disabled) return (dm_vamm_resource_usage){0, 0};
    return (dm_vamm_resource_usage){info(I_SysResTotal), info(I_SysResUsed)};
}
static dm_vamm_config config(fixture *f, size_t max_regions) {
    return (dm_vamm_config){.base = info(I_AliasAddr), .size = info(I_AliasSize),
        .max_regions = max_regions, .context = f, .change_pages = change, .resources = resources};
}
static dm_vamm_stats vstats(fixture *f) {
    dm_vamm_stats s; assert(!dm_vamm_get_stats(f->v, SMALL, &s)); return s;
}
static void empty(fixture *f) {
    assert(!vstats(f).regions && !vstats(f).reserved_bytes && !alias_mapped());
}
static uint32_t acquire(void *context, uintptr_t *out, size_t size) {
    return dm_vamm_allocate(((fixture *)context)->v, out, size, 1);
}
static uint32_t relinquish(void *context, uintptr_t base) {
    return dm_vamm_free(((fixture *)context)->v, base);
}
static dm_sa_result reservation_stats(void *context, size_t alignment, dm_sa_provider_stats *out) {
    dm_vamm_stats s;
    if (dm_vamm_get_stats(((fixture *)context)->v, alignment, &s)) return DM_SA_PROVIDER_FAILED;
    *out = (dm_sa_provider_stats){s.available_bytes, s.largest_region}; return DM_SA_OK;
}
static int context_active(void *context) { (void)context; return 1; }
static void *worker(void *context) {
    fixture *f = context;
    for (unsigned i = 0; i < 200; ++i) {
        uintptr_t base;
        assert(!dm_vamm_allocate(f->v, &base, PAGE, 1));
        memset((void *)base, 0x34, PAGE);
        assert(!dm_vamm_free(f->v, base));
    }
    return NULL;
}
static void core_tests(void) {
    fixture f = {0}; dm_vamm_config c = config(&f, 128);
    assert(!dm_vamm_create(&f.v, &c));
    uintptr_t base = 7, other;
    assert(dm_vamm_allocate(f.v, &base, PAGE, 0) == DM_VAMM_INVALID && base == 7);
    f.disabled = 1; dm_vamm_initialize(f.v);
    assert(!dm_vamm_is_enabled(f.v) && !dm_vamm_resources(f.v).total && !dm_vamm_resources(f.v).used);
    assert(dm_vamm_allocate(f.v, &base, PAGE, 0) == DM_VAMM_INVALID && !f.page_calls);
    f.disabled = 0; assert(dm_vamm_is_enabled(f.v));
    dm_vamm_initialize(f.v); dm_vamm_initialize(f.v);
    assert(!f.page_calls && !alias_mapped());
    dm_vamm_resource_usage r = dm_vamm_resources(f.v);
    assert(r.total == info(I_SysResTotal) && r.used == info(I_SysResUsed) + 0x80000);
    f.override_resources = 1; f.override = (dm_vamm_resource_usage){0x10000, 0};
    r = dm_vamm_resources(f.v); assert(r.used == r.total && r.total == 0x10000);
    f.override = (dm_vamm_resource_usage){UINT64_MAX, UINT64_MAX};
    assert(dm_vamm_resources(f.v).used == UINT64_MAX); f.override_resources = 0;
    assert(!dm_vamm_allocate(f.v, &base, PAGE, 0) && !(base % SMALL));
    assert(base >= c.base + PAGE && base + 2 * PAGE <= c.base + c.size);
    assert(!alias_mapped() && vstats(&f).regions == 1);
    assert(dm_vamm_destroy(f.v) == DM_VAMM_INVALID);
    assert(!dm_vamm_allocate(f.v, &other, LARGE, 0) && !(other % LARGE));
    assert(other >= base + 3 * PAGE);
    assert(dm_vamm_pages(f.v, base - PAGE, PAGE, 1) == DM_VAMM_INVALID);
    assert(dm_vamm_pages(f.v, base, 2 * PAGE, 1) == DM_VAMM_INVALID);
    assert(dm_vamm_pages(f.v, base + 1, PAGE, 1) == DM_VAMM_INVALID);
    assert(dm_vamm_free(f.v, other + PAGE) == DM_VAMM_INVALID);
    assert(!dm_vamm_pages(f.v, base, PAGE, 1));
    memset((void *)base, 0x6b, PAGE);
    assert(!dm_vamm_pages(f.v, base, PAGE, 1) && *(unsigned char *)base == 0x6b);
    assert(!dm_vamm_free(f.v, base) && !dm_vamm_free(f.v, other)); empty(&f);

    // Host protection failure is fatal before memory state publication.
    dm_vamm_stats before = vstats(&f); base = 9;
    EXPECT_MEMORY_PROTECTION_FAILURE(pages(1, c.base + SMALL, SMALL), NULL, 0);
    assert(base == 9);
    assert(vstats(&f).available_bytes == before.available_bytes); empty(&f);
    assert(dm_vamm_allocate(f.v, &base, (size_t)62 << 30, 0) == DM_VAMM_NO_SPACE && base == 9);
    assert(dm_vamm_allocate(f.v, &base, SIZE_MAX, 0) == DM_VAMM_INVALID);
    // The provider's existing strict physical scope applies atomically even in
    // unlimited profiles; ordinary Vamm/SVC retains that profile's old policy.
    assert(!call(1, 0, 0x1e00000, 0));
    dm_sa_svc_owner_allocation_begin();
    assert(dm_vamm_allocate(f.v, &base, PAGE, 1) == DM_VAMM_NO_MEMORY && base == 9);
    dm_sa_svc_owner_allocation_end(); empty(&f);
#if DM_GAME_MEM_MAP_LIMIT
    assert(dm_vamm_allocate(f.v, &base, PAGE, 1) == DM_VAMM_NO_MEMORY && base == 9);
#else
    assert(!dm_vamm_allocate(f.v, &base, PAGE, 1) && !dm_vamm_free(f.v, base));
#endif
    assert(!call(1, 0, 0, 0)); empty(&f);
    assert(!dm_vamm_allocate(f.v, &base, SMALL, 1));
    memset((void *)base, 0xa5, SMALL);
    EXPECT_MEMORY_PROTECTION_FAILURE(dm_vamm_free(f.v, base), (void *)base, SMALL);
    assert(vstats(&f).regions == 1);
    assert(*(unsigned char *)base == 0xa5 && alias_mapped() == SMALL);
    // Source borrowing rejects the whole decommit and preserves the owner.
    uintptr_t stack = info(I_StackAddr);
    assert(!call(4, stack, base, PAGE));
    assert(dm_vamm_free(f.v, base) == 0x803 && vstats(&f).regions == 1 && alias_mapped() == SMALL);
    assert(!call(5, stack, base, PAGE));
    assert(!dm_vamm_free(f.v, base)); empty(&f);

    // Kernel-style raw maps are independent of reservation membership. The
    // adapter must not turn every SVC into an SDK reservation lookup.
    uintptr_t raw = c.base + c.size - PAGE;
    assert(!pages(1, raw, PAGE)); query_state(raw, 5, 0, 3);
    assert(dm_vamm_pages(f.v, raw, PAGE, 1) == DM_VAMM_INVALID);
    assert(!pages(0, raw, PAGE));

    // Largest-region stats must be a legal exact-size lease, never a raw
    // uncommitted interval or a virtual extent that ignores guard/alignment.
    size_t largest = vstats(&f).largest_region;
    assert(largest >= LARGE && !dm_vamm_allocate(f.v, &base, largest, 0));
    assert(!dm_vamm_free(f.v, base));
    assert(dm_vamm_allocate(f.v, &base, largest + PAGE, 0) == DM_VAMM_NO_SPACE);
    uintptr_t fragmented[4];
    for (unsigned i = 0; i < 4; ++i) assert(!dm_vamm_allocate(f.v, &fragmented[i], LARGE, 0));
    assert(!dm_vamm_free(f.v, fragmented[1]));
    size_t fragmented_largest = vstats(&f).largest_region;
    assert(!dm_vamm_allocate(f.v, &base, fragmented_largest, 0));
    assert(!dm_vamm_free(f.v, base));
    assert(dm_vamm_allocate(f.v, &base, fragmented_largest + PAGE, 0) == DM_VAMM_NO_SPACE);
    for (unsigned i = 0; i < 4; ++i) if (i != 1) assert(!dm_vamm_free(f.v, fragmented[i]));
    assert(vstats(&f).largest_region == largest); empty(&f);

    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, worker, &f));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    empty(&f);

    // Complete StandardAllocator -> current host owner -> production SVC path.
    dm_sa_sdk_owner callbacks = {&f, acquire, relinquish, reservation_stats, context_active, SMALL, NULL};
    dm_sa_sdk_provider *provider;
    assert(dm_sa_sdk_provider_create(&provider, &callbacks) == DM_SA_OK);
    dm_sa_config sc = {.provider = dm_sa_sdk_provider_interface(provider), .capacity = (size_t)62 << 30};
    dm_sa_allocator *a; assert(dm_sa_create(&a, &sc) == DM_SA_OK); empty(&f);
    void *p = NULL; assert(dm_sa_allocate(a, 100, 16, &p) == DM_SA_OK);
    memset(p, 0x5a, 100);
    assert(dm_sa_free(a, p) == DM_SA_OK && dm_sa_destroy(a) == DM_SA_OK);
    assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_OK); empty(&f);
    assert(!dm_vamm_destroy(f.v));

    // Metadata exhaustion is not virtual-space exhaustion or a smaller lease.
    c.max_regions = 1; assert(!dm_vamm_create(&f.v, &c)); dm_vamm_initialize(f.v);
    assert(!dm_vamm_allocate(f.v, &base, PAGE, 0)); other = 11;
    assert(dm_vamm_allocate(f.v, &other, PAGE, 0) == DM_VAMM_NO_MEMORY && other == 11);
    assert(!vstats(&f).available_bytes && !vstats(&f).largest_region);
    assert(!dm_vamm_free(f.v, base)); assert(!dm_vamm_destroy(f.v));

    // Threshold edge: a real 3 MiB domain can fit almost 2 MiB at 64 KiB
    // alignment but cannot fit 2 MiB once that request requires 2 MiB alignment.
    c = config(&f, 8);
    c.base = ((c.base + LARGE - 1) & ~(uintptr_t)(LARGE - 1)) + PAGE;
    c.size = 0x300000;
    assert(!dm_vamm_create(&f.v, &c)); dm_vamm_initialize(f.v);
    assert(vstats(&f).largest_region == LARGE - PAGE);
    assert(!dm_vamm_allocate(f.v, &base, LARGE - PAGE, 0));
    assert(!dm_vamm_free(f.v, base)); other = 13;
    assert(dm_vamm_allocate(f.v, &other, LARGE, 0) == DM_VAMM_NO_SPACE && other == 13);
    assert(!dm_vamm_destroy(f.v));
}


static dm_vamm_stats aligned_stats(fixture *f, size_t alignment) {
    dm_vamm_stats s; assert(!dm_vamm_get_aligned_stats(f->v, alignment, &s)); return s;
}
static void aligned_tests(void) {
    fixture f = {0}; dm_vamm_config c = config(&f, 128);
    assert(!dm_vamm_create(&f.v, &c));
    uintptr_t base = 7;
    assert(dm_vamm_allocate_aligned(f.v, &base, PAGE, 2 * SMALL, 0) == DM_VAMM_INVALID && base == 7);
    assert(!aligned_stats(&f, 2 * SMALL).largest_region);
    dm_vamm_initialize(f.v);

    // Invalid inputs preserve outputs and never call the physical backend.
    const size_t invalid_alignments[] = {0, 16, PAGE / 2, PAGE + 1, 3 * PAGE, SIZE_MAX};
    for (size_t i = 0; i < sizeof invalid_alignments / sizeof *invalid_alignments; ++i) {
        dm_vamm_stats s, old; memset(&s, 0xa5, sizeof s); old = s;
        assert(dm_vamm_get_aligned_stats(f.v, invalid_alignments[i], &s) == DM_VAMM_INVALID);
        assert(!memcmp(&s, &old, sizeof s));
        assert(dm_vamm_allocate_aligned(f.v, &base, PAGE, invalid_alignments[i], 1) == DM_VAMM_INVALID);
        assert(base == 7 && !f.page_calls);
    }
    assert(dm_vamm_allocate_aligned(f.v, &base, 0, LARGE, 1) == DM_VAMM_INVALID);
    assert(dm_vamm_allocate_aligned(f.v, &base, PAGE + 1, LARGE, 1) == DM_VAMM_INVALID);
    assert(dm_vamm_allocate_aligned(f.v, &base, SIZE_MAX, LARGE, 1) == DM_VAMM_INVALID);
    assert(dm_vamm_allocate_aligned(f.v, &base, PAGE, LARGE, 2) == DM_VAMM_INVALID);
    assert(dm_vamm_allocate_aligned(f.v, NULL, PAGE, LARGE, 1) == DM_VAMM_INVALID);
    assert(dm_vamm_allocate_aligned(NULL, &base, PAGE, LARGE, 1) == DM_VAMM_INVALID);
    assert(dm_vamm_get_aligned_stats(f.v, PAGE, NULL) == DM_VAMM_INVALID);
    assert(base == 7 && !f.page_calls);

    const size_t alignments[] = {PAGE, 2 * SMALL, LARGE, 2 * LARGE, 4 * LARGE, 8 * LARGE};
    const size_t sizes[] = {PAGE, SMALL, LARGE, LARGE + PAGE};
    for (size_t i = 0; i < sizeof alignments / sizeof *alignments; ++i) {
        size_t alignment = alignments[i];
        dm_vamm_stats before = aligned_stats(&f, alignment);
        assert(before.available_bytes == before.largest_region && before.largest_region >= LARGE + PAGE);
        for (size_t j = 0; j < sizeof sizes / sizeof *sizes; ++j) {
            size_t size = sizes[j];
            assert(!dm_vamm_allocate_aligned(f.v, &base, size, alignment, 1));
            assert(!(base & (alignment - 1)));
            assert(base >= c.base + PAGE && size + PAGE <= c.base + c.size - base);
            dm_vamm_stats live = aligned_stats(&f, alignment);
            assert(live.regions == 1 && live.reserved_bytes == size && alias_mapped() == size);
            assert(live.metadata_bytes == before.metadata_bytes);
            // Alignment slack and guards are never padded mapped payload.
            query_state(base - PAGE, 0, 0, 0); query_state(base + size, 0, 0, 0);
            query_state(base, 5, 0, 3); query_state(base + size - PAGE, 5, 0, 3);
            memset((void *)base, 0x5a, size);
            assert(dm_vamm_pages(f.v, base - PAGE, PAGE, 1) == DM_VAMM_INVALID);
            assert(dm_vamm_pages(f.v, base, size + PAGE, 1) == DM_VAMM_INVALID);
            assert(!dm_vamm_free(f.v, base)); empty(&f);
            assert(aligned_stats(&f, alignment).available_bytes == before.available_bytes);
        }
        // The exact reported maximum really fits, with the same placement
        // rule. A larger contiguous request must not become smaller success.
        unsigned calls = f.page_calls;
        assert(!dm_vamm_allocate_aligned(f.v, &base, before.largest_region, alignment, 0));
        assert(!(base & (alignment - 1)) && f.page_calls == calls && !alias_mapped());
        assert(aligned_stats(&f, alignment).reserved_bytes == before.largest_region);
        assert(!dm_vamm_free(f.v, base)); base = 9; calls = f.page_calls;
        assert(dm_vamm_allocate_aligned(f.v, &base, before.largest_region + PAGE, alignment, 1) == DM_VAMM_NO_SPACE);
        assert(base == 9 && f.page_calls == calls); empty(&f);
    }
    size_t impossible = SIZE_MAX / 2 + 1;
    unsigned calls = f.page_calls;
    assert(!aligned_stats(&f, impossible).available_bytes && !aligned_stats(&f, impossible).largest_region);
    assert(dm_vamm_allocate_aligned(f.v, &base, PAGE, impossible, 1) == DM_VAMM_NO_SPACE && base == 9);
    assert(dm_vamm_allocate_aligned(f.v, &base, SIZE_MAX & ~(size_t)(PAGE - 1), LARGE, 1) == DM_VAMM_NO_SPACE);
    assert(base == 9 && f.page_calls == calls); empty(&f);

    // Large alignment consumes no extra physical budget: a single page still
    // fits with only one host page left under either legacy mapping policy.
    assert(!call(1, 0, APP_POOL - DM_GAME_MEM_USED - host_page(), 0));
    dm_sa_svc_owner_allocation_begin();
    assert(!dm_vamm_allocate_aligned(f.v, &base, PAGE, 8 * LARGE, 1));
    dm_sa_svc_owner_allocation_end();
    assert(!(base & (8 * LARGE - 1)) && alias_mapped() == PAGE);
    assert(!dm_vamm_free(f.v, base)); base = 11;
    dm_vamm_stats before = aligned_stats(&f, LARGE);
    dm_sa_svc_owner_allocation_begin();
    assert(dm_vamm_allocate_aligned(f.v, &base, host_page() + PAGE, LARGE, 1) == DM_VAMM_NO_MEMORY && base == 11);
    dm_sa_svc_owner_allocation_end();
    assert(aligned_stats(&f, LARGE).available_bytes == before.available_bytes); empty(&f);
    assert(!call(1, 0, 0, 0));
    EXPECT_MEMORY_PROTECTION_FAILURE(dm_vamm_allocate_aligned(f.v, &base, SMALL, LARGE, 1), NULL, 0);
    assert(base == 11 && aligned_stats(&f, LARGE).available_bytes == before.available_bytes); empty(&f);

    // Failed exact release retains the same high-alignment reservation and
    // every mapped byte until its borrowed source has been copied back.
    assert(!dm_vamm_allocate_aligned(f.v, &base, SMALL, LARGE, 1));
    memset((void *)base, 0xa5, SMALL);
    EXPECT_MEMORY_PROTECTION_FAILURE(dm_vamm_free(f.v, base), (void *)base, SMALL);
    uintptr_t stack = info(I_StackAddr);
    assert(!call(4, stack, base, PAGE)); *(unsigned char *)stack = 0x4c;
    assert(dm_vamm_free(f.v, base) == 0x803);
    assert(dm_vamm_pages(f.v, base, SMALL, 0) == 0x803);
    assert(aligned_stats(&f, LARGE).regions == 1 && aligned_stats(&f, LARGE).reserved_bytes == SMALL);
    assert(alias_mapped() == SMALL && *(unsigned char *)(base + PAGE) == 0xa5);
    query_state(base, 5, 1, 0);
    assert(!call(5, stack, base, PAGE) && *(unsigned char *)base == 0x4c);
    assert(!dm_vamm_free(f.v, base)); empty(&f); assert(!dm_vamm_destroy(f.v));

    // Fragmentation is not one contiguous lease. Two aligned holes together
    // have enough bytes for largest+PAGE, but neither can satisfy that request.
    c = config(&f, 8);
    c.base = (c.base + LARGE - 1) & ~(uintptr_t)(LARGE - 1);
    c.size = 4 * LARGE + PAGE;
    assert(!dm_vamm_create(&f.v, &c)); dm_vamm_initialize(f.v);
    uintptr_t fragments[4];
    for (unsigned i = 0; i < 4; ++i)
        assert(!dm_vamm_allocate_aligned(f.v, &fragments[i], LARGE - PAGE, PAGE, 0));
    assert(!dm_vamm_free(f.v, fragments[0]) && !dm_vamm_free(f.v, fragments[2]));
    before = aligned_stats(&f, 2 * SMALL);
    assert(before.available_bytes == 2 * (LARGE - 2 * SMALL));
    assert(before.largest_region == LARGE - 2 * SMALL);
    base = 13; calls = f.page_calls;
    assert(dm_vamm_allocate_aligned(f.v, &base, before.largest_region + PAGE, 2 * SMALL, 0) == DM_VAMM_NO_SPACE);
    assert(base == 13 && f.page_calls == calls && aligned_stats(&f, 2 * SMALL).regions == 2);
    assert(!dm_vamm_allocate_aligned(f.v, &base, before.largest_region, 2 * SMALL, 0));
    assert(base == c.base + 2 * LARGE + 2 * SMALL);
    uintptr_t other;
    assert(!dm_vamm_allocate_aligned(f.v, &other, before.largest_region, 2 * SMALL, 0));
    assert(other == c.base + 2 * SMALL && !aligned_stats(&f, 2 * SMALL).largest_region);
    assert(!dm_vamm_free(f.v, base) && !dm_vamm_free(f.v, other));
    assert(!dm_vamm_free(f.v, fragments[1]) && !dm_vamm_free(f.v, fragments[3]));
    empty(&f); assert(!dm_vamm_destroy(f.v));

    // Explicit native alignment is not secretly promoted at the SDK's 2 MiB
    // payload threshold. The unchanged public operation still uses its default.
    c = config(&f, 8);
    c.base = ((c.base + LARGE - 1) & ~(uintptr_t)(LARGE - 1)) + PAGE;
    c.size = 3 * LARGE / 2;
    assert(!dm_vamm_create(&f.v, &c)); dm_vamm_initialize(f.v);
    assert(vstats(&f).largest_region == LARGE - PAGE);
    before = aligned_stats(&f, PAGE); assert(before.largest_region == c.size - 2 * PAGE);
    assert(dm_vamm_allocate(f.v, &base, LARGE, 0) == DM_VAMM_NO_SPACE);
    assert(!dm_vamm_allocate_aligned(f.v, &base, before.largest_region, PAGE, 0));
    assert(base == c.base + PAGE && base % LARGE);
    assert(!dm_vamm_free(f.v, base) && !dm_vamm_destroy(f.v));

    // Near-address-limit rounding must fail rather than wrap into a fabricated
    // reservation. No physical callback ever sees this synthetic boundary.
    c = config(&f, 8);
    c.base = (UINTPTR_MAX & ~(uintptr_t)(PAGE - 1)) - 4 * PAGE;
    c.size = 4 * PAGE;
    assert(!dm_vamm_create(&f.v, &c)); dm_vamm_initialize(f.v);
    base = 17; calls = f.page_calls;
    assert(!aligned_stats(&f, impossible).largest_region);
    assert(dm_vamm_allocate_aligned(f.v, &base, PAGE, impossible, 0) == DM_VAMM_NO_SPACE);
    assert(base == 17 && f.page_calls == calls && !aligned_stats(&f, PAGE).regions);
    assert(!dm_vamm_destroy(f.v));

    c = config(&f, 1);
    assert(!dm_vamm_create(&f.v, &c)); dm_vamm_initialize(f.v);
    assert(!dm_vamm_allocate_aligned(f.v, &base, PAGE, LARGE, 0));
    before = aligned_stats(&f, LARGE);
    assert(!before.available_bytes && !before.largest_region && before.reserved_bytes == PAGE);
    other = 19;
    assert(dm_vamm_allocate_aligned(f.v, &other, PAGE, LARGE, 0) == DM_VAMM_NO_MEMORY && other == 19);
    assert(!dm_vamm_free(f.v, base)); empty(&f); assert(!dm_vamm_destroy(f.v));
}

static unsigned original_calls;
static void *bootstrap_expected;
static void original_never(void) { ++original_calls; abort(); }
static void bootstrap(void *self) { assert(self && self == bootstrap_expected); ++original_calls; memset(self, 0, 72); }
static dm_vamm_guest_fn hook(unsigned index) {
    return dm_vamm_adapter_lookup(dm_vamm_adapter_symbol_name(index));
}
static void adapter_tests(int disabled, int c2) {
    fixture f = {.disabled = disabled}; dm_vamm_symbol symbols[DM_VAMM_SYMBOL_COUNT];
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i)
        symbols[i] = (dm_vamm_symbol){dm_vamm_adapter_symbol_name(i), original_never};
    symbols[8].function = symbols[9].function = (dm_vamm_guest_fn)bootstrap;
    dm_vamm_adapter_config c = {.proofs = DM_VAMM_PROOF_ALL, .symbols = symbols, .symbol_count = DM_VAMM_SYMBOL_COUNT, .owner = config(&f, 32)};
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i) {
        dm_vamm_guest_fn old_function = symbols[i].function; symbols[i].function = NULL;
        assert(dm_vamm_adapter_install(&c) == DM_VAMM_INVALID && !hook(i)); symbols[i].function = old_function;
    }
    for (unsigned i = 0; i < 6; ++i) {
        c.proofs = DM_VAMM_PROOF_ALL ^ (1u << i);
        assert(dm_vamm_adapter_install(&c) == DM_VAMM_INVALID && !hook(0));
    }
    c.proofs = DM_VAMM_PROOF_ALL;
    const char *saved = symbols[1].name; symbols[1].name = symbols[0].name;
    assert(dm_vamm_adapter_install(&c) == DM_VAMM_INVALID && !hook(0)); symbols[1].name = saved;
    symbols[2].function = NULL;
    assert(dm_vamm_adapter_install(&c) == DM_VAMM_INVALID && !hook(0));
    symbols[2].function = original_never;
    c.owner.size = 1;
    assert(dm_vamm_adapter_install(&c) == DM_VAMM_INVALID && !hook(0));
    c.owner = config(&f, 32);
    assert(!dm_vamm_adapter_install(&c));
    assert(dm_vamm_adapter_install(&c) == DM_VAMM_INVALID);
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i) assert(hook(i));
    // Different synthetic enclosing layouts, deliberately unrelated to SDK
    // offsets. Public hooks must use constructor identity, never decode them.
    unsigned char enclosing[256]; memset(enclosing, 0xa5, sizeof enclosing);
    size_t position = c2 ? 128 : 16;
    void *self = enclosing + position; bootstrap_expected = self;
    ((void (*)(void *))hook(c2 ? 9 : 8))(self);
    assert(original_calls == 1 && !f.page_calls);
    ((void (*)(void))hook(0))(); ((void (*)(void *))hook(10))(self);
    assert(((int (*)(void))hook(7))() == !disabled);
    assert(((int (*)(const void *))hook(11))(self) == !disabled);
    dm_vamm_resource_usage r = ((dm_vamm_resource_usage (*)(void))hook(6))();
    dm_vamm_resource_usage s = ((dm_vamm_resource_usage (*)(void *))hook(17))(self);
    assert(r.total == s.total && r.used == s.used && !!r.total == !disabled);
    uintptr_t base = 5, other = 5;
    if (disabled) {
        assert(((uint32_t (*)(uintptr_t *, size_t))hook(1))(&base, PAGE) == DM_VAMM_INVALID && base == 5);
        assert(!f.page_calls && !alias_mapped());
    } else {
        assert(!((uint32_t (*)(uintptr_t *, size_t))hook(1))(&base, PAGE));
        assert(!((uint32_t (*)(void *, uintptr_t *, size_t))hook(12))(self, &other, PAGE));
        assert(!((uint32_t (*)(uintptr_t, size_t))hook(3))(base, PAGE));
        assert(!((uint32_t (*)(void *, uintptr_t, size_t))hook(14))(self, other, PAGE));
        assert(!((uint32_t (*)(uintptr_t, size_t))hook(5))(base, PAGE));
        assert(!((uint32_t (*)(void *, uintptr_t, size_t))hook(16))(self, other, PAGE));
        assert(dm_vamm_adapter_shutdown() == DM_VAMM_INVALID);
        assert(!((uint32_t (*)(uintptr_t))hook(4))(base));
        assert(!((uint32_t (*)(void *, uintptr_t))hook(15))(self, other));
        assert(!((uint32_t (*)(uintptr_t *, size_t))hook(2))(&base, PAGE));
        assert(!((uint32_t (*)(void *, uintptr_t *, size_t))hook(13))(self, &other, PAGE));
        assert(!((uint32_t (*)(uintptr_t))hook(4))(base));
        assert(!((uint32_t (*)(void *, uintptr_t))hook(15))(self, other));
    }
    assert(original_calls == 1 && !alias_mapped());
    for (size_t i = 0; i < sizeof enclosing; ++i)
        assert(enclosing[i] == (i >= position && i < position + 72 ? 0 : 0xa5));
    assert(!dm_vamm_adapter_shutdown());
    // Shutdown never silently enables an SDK owner; table remains intercepted.
    assert(hook(0) && dm_vamm_adapter_install(&c) == DM_VAMM_INVALID);
}
int main(int argc, char **argv) {
    alarm(60);
    core_tests();
    aligned_tests();
    adapter_tests(argc > 1 && !strcmp(argv[1], "disabled"), argc > 1 && !strcmp(argv[1], "c2"));
    puts("vamm: reservation/dispatch/provider/SVC checks passed"); return 0;
}
