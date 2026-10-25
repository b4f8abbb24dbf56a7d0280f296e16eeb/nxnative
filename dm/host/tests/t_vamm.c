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
    return (dm_vamm_config){info(I_AliasAddr), info(I_AliasSize), max_regions, f, change, resources};
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

    // The raw SVC map failure is atomic. Its nn::os translation is fatal,
    // covered separately by the native bridge fixture, not a returning 0x803.
    dm_vamm_stats before = vstats(&f); base = 9;
    atomic_store(&fail_protect, 1);
    assert(pages(1, c.base + SMALL, SMALL) == 0xd401 && base == 9);
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
    atomic_store(&fail_protect, 1);
    assert(dm_vamm_free(f.v, base) == 0x803 && vstats(&f).regions == 1);
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
    assert(dm_as_reserve(g_alias_space, PAGE, PAGE, &base) == DM_AS_WRONG_MODE);

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
    dm_sa_sdk_owner callbacks = {&f, acquire, relinquish, reservation_stats, context_active, SMALL};
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

static unsigned original_calls;
static void original_never(void) { ++original_calls; abort(); }
static void bootstrap(void *self) { assert(self); ++original_calls; memset(self, 0, 72); }
static dm_vamm_guest_fn hook(unsigned index) {
    return dm_vamm_adapter_lookup(dm_vamm_adapter_symbol_name(index));
}
static void adapter_tests(int disabled, int c2) {
    fixture f = {.disabled = disabled}; dm_vamm_symbol symbols[DM_VAMM_SYMBOL_COUNT];
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i)
        symbols[i] = (dm_vamm_symbol){dm_vamm_adapter_symbol_name(i), original_never};
    symbols[8].function = symbols[9].function = (dm_vamm_guest_fn)bootstrap;
    dm_vamm_adapter_config c = {.proofs = DM_VAMM_PROOF_ALL, .installed_bindings = DM_VAMM_ALL_BINDINGS,
        .symbols = symbols, .symbol_count = DM_VAMM_SYMBOL_COUNT, .owner = config(&f, 32)};
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i) {
        c.installed_bindings = DM_VAMM_ALL_BINDINGS ^ (UINT64_C(1) << i);
        assert(dm_vamm_adapter_install(&c) == DM_VAMM_INVALID && !hook(i));
    }
    c.installed_bindings = DM_VAMM_ALL_BINDINGS;
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
    unsigned char self[72]; ((void (*)(void *))hook(c2 ? 9 : 8))(self);
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
    assert(!dm_vamm_adapter_shutdown());
    // Shutdown never silently enables an SDK owner; table remains intercepted.
    assert(hook(0) && dm_vamm_adapter_install(&c) == DM_VAMM_INVALID);
}
int main(int argc, char **argv) {
    alarm(60);
    core_tests();
    adapter_tests(argc > 1 && !strcmp(argv[1], "disabled"), argc > 1 && !strcmp(argv[1], "c2"));
    puts("vamm: reservation/dispatch/provider/SVC checks passed"); return 0;
}
