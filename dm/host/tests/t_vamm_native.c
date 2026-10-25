// Native Vamm bridge through the REAL production SVC manager. Only unrelated
// platform services are stubbed. No SDK code or proprietary inputs are used.
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
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

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
static _Atomic unsigned protect_calls;
static int test_mprotect(void *address, size_t size, int permission) {
    atomic_fetch_add(&protect_calls, 1);
    assert(!((uintptr_t)address & (host_page() - 1)) && !(size & (host_page() - 1)));
    if (atomic_exchange(&fail_protect, 0)) { errno = EACCES; return -1; }
    return mprotect(address, size, permission);
}
#ifdef DM_TEST_SYS_RESOURCE
#undef DM_GAME_SYS_RESOURCE
#define DM_GAME_SYS_RESOURCE DM_TEST_SYS_RESOURCE
#endif
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
static dm_vamm_stats stats(dm_vamm *v) {
    dm_vamm_stats s; assert(!dm_vamm_get_stats(v, SMALL, &s)); return s;
}
static void bad_domain(uint64_t base, uint64_t size, uint64_t extra) {
    uintptr_t b = 7; size_t n = 9;
    assert(dm_vamm_native_decode_domain(base, size, extra, &b, &n) == DM_VAMM_INVALID);
    assert(b == 7 && n == 9);
}
static void domain_tests(void) {
    uintptr_t b = 0; size_t n = 0;
    assert(!dm_vamm_native_decode_domain(0x100000, 0x200000, 0x4000, &b, &n));
    assert(b == 0x100000 && n == 0x1fc000); // a nonzero type 28 really reduces size
    assert(!dm_vamm_native_decode_domain(0x100000, 0x200000, 0, &b, &n));
    assert(n == 0x200000);
    bad_domain(0, 0x200000, 0); bad_domain(0x100001, 0x200000, 0);
    bad_domain(0x100000, 0, 0); bad_domain(0x100000, 0x200001, 0);
    bad_domain(0x100000, 0x200000, 1); bad_domain(0x100000, 0x200000, 0x201000);
    bad_domain(0x100000, 0x200000, 0x200000);
    bad_domain(UINT64_MAX & ~UINT64_C(0xfff), 0x1000, 0);
    bad_domain(0x1000, UINT64_MAX & ~UINT64_C(0xfff), 0x1000);
    assert(dm_vamm_native_decode_domain(0x1000, 0x2000, 0, NULL, &n) == DM_VAMM_INVALID);
    assert(dm_vamm_native_decode_domain(0x1000, 0x2000, 0, &b, NULL) == DM_VAMM_INVALID);
    dm_vamm_config c, old; memset(&c, 0xa5, sizeof c); old = c;
    assert(dm_vamm_native_configure(&c, 0) == DM_VAMM_INVALID && !memcmp(&c, &old, sizeof c));
    assert(dm_vamm_native_configure(NULL, 1) == DM_VAMM_INVALID);
}
static void expect_result_abort(uint32_t result, int commit) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) { (void)dm_vamm_native_svc_result(result, commit); _exit(99); }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
static void result_tests(void) {
    for (int commit = 0; commit <= 1; ++commit) {
        assert(!dm_vamm_native_svc_result(0, commit));
        assert(dm_vamm_native_svc_result(0xce01, commit) == 0x1203);
        assert(dm_vamm_native_svc_result(0xd001, commit) == DM_VAMM_NO_MEMORY);
        assert(dm_vamm_native_svc_result(0x10801, commit) == DM_VAMM_NO_MEMORY);
        assert(dm_vamm_native_svc_result(0x80010801, commit) == DM_VAMM_NO_MEMORY);
        expect_result_abort(0xdead, commit);
        expect_result_abort(0x80000000, commit); // masked zero is NOT success
    }
    assert(dm_vamm_native_svc_result(0xd401, 0) == 0x803);
    assert(dm_vamm_native_svc_result(0x8000d401, 0) == 0x803);
    expect_result_abort(0xd401, 1); // crucial map/unmap distinction
    expect_result_abort(0, 2);
}
static void expect_native_map_abort(const dm_vamm_config *c, uintptr_t base, int backend_failure) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        if (backend_failure) atomic_store(&fail_protect, 1);
        (void)c->change_pages(c->context, base, SMALL, 1); _exit(99);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
static void resource_test(dm_vamm *v, const dm_vamm_config *c) {
    dm_vamm_resource_usage raw = c->resources(c->context), r = dm_vamm_resources(v);
    assert(raw.total == info(16) && raw.used == info(17));
    assert(r.total == raw.total);
    uint64_t used = raw.used + 0x80000;
    assert(r.used == (used < raw.total ? used : raw.total));
}
static void *worker(void *context) {
    dm_vamm *v = context;
    for (unsigned i = 0; i < 200; ++i) {
        uintptr_t base;
        assert(!dm_vamm_allocate(v, &base, PAGE, 1));
        memset((void *)base, 0x34, PAGE);
        assert(!dm_vamm_free(v, base));
    }
    return NULL;
}
static uint32_t acquire(void *context, uintptr_t *out, size_t size) {
    return dm_vamm_allocate(context, out, size, 1);
}
static uint32_t release(void *context, uintptr_t base) { return dm_vamm_free(context, base); }
static dm_sa_result reservation_stats(void *context, size_t alignment, dm_sa_provider_stats *out) {
    dm_vamm_stats s;
    if (dm_vamm_get_stats(context, alignment, &s)) return DM_SA_PROVIDER_FAILED;
    *out = (dm_sa_provider_stats){s.available_bytes, s.largest_region}; return DM_SA_OK;
}
static int context_active(void *context) { (void)context; return 1; }
static void native_tests(void) {
    dm_vamm_config c; assert(!dm_vamm_native_configure(&c, 128));
    assert(c.base == info(2) && c.size == info(3) - info(28) && !c.context);
    assert(c.max_regions == 128 && !alias_mapped() && !g_nmap);
    dm_vamm *v; assert(!dm_vamm_create(&v, &c));
    assert(!dm_vamm_adapter_lookup(dm_vamm_adapter_symbol_name(0)));
    uintptr_t base = 7;
    assert(dm_vamm_allocate(v, &base, PAGE, 0) == DM_VAMM_INVALID && base == 7);
    unsigned before_init = atomic_load(&protect_calls);
    dm_vamm_initialize(v); dm_vamm_initialize(v); resource_test(v, &c);
    assert(!alias_mapped() && atomic_load(&protect_calls) == before_init);
    if (!info(16)) {
        assert(!dm_vamm_is_enabled(v));
        assert(dm_vamm_allocate(v, &base, PAGE, 1) == DM_VAMM_INVALID && base == 7);
        assert(!dm_vamm_destroy(v)); return;
    }
    assert(dm_vamm_is_enabled(v));
    assert(!dm_vamm_allocate(v, &base, SMALL, 0));
    assert(!(base % SMALL) && !alias_mapped());
    assert(base >= c.base + PAGE && base + SMALL + PAGE <= c.base + c.size);
    assert(dm_vamm_pages(v, base - PAGE, PAGE, 1) == DM_VAMM_INVALID);
    assert(dm_vamm_pages(v, base, SMALL + PAGE, 1) == DM_VAMM_INVALID);
    assert(dm_vamm_free(v, base + PAGE) == DM_VAMM_INVALID);

    // Host protection failure aborts within SVC before state publication.
    EXPECT_MEMORY_PROTECTION_FAILURE(dm_vamm_svc_change_pages(base, SMALL, 1), NULL, 0);
    assert(!alias_mapped());
    query_state(base, 0, 0, 0);
    assert(stats(v).regions == 1 && stats(v).reserved_bytes == SMALL);
    expect_native_map_abort(&c, base, 1);
    assert(!dm_vamm_pages(v, base, SMALL, 1)); memset((void *)base, 0xa5, SMALL);
    assert(!dm_vamm_pages(v, base, SMALL, 1) && alias_mapped() == SMALL);
    assert(*(unsigned char *)base == 0xa5); resource_test(v, &c);

    // Atomic failed unmap leaves both owner ledger and all native bytes live.
    EXPECT_MEMORY_PROTECTION_FAILURE(dm_vamm_free(v, base), (void *)base, SMALL);
    assert(stats(v).regions == 1);
    assert(alias_mapped() == SMALL && *(unsigned char *)base == 0xa5);
    uintptr_t stack = info(I_StackAddr);
    assert(!call(4, stack, base, PAGE));
    *(unsigned char *)stack = 0x56;
    unsigned before = atomic_load(&protect_calls);
    assert(!dm_vamm_svc_change_pages(base, SMALL, 1));
    assert(dm_vamm_pages(v, base, PAGE, 0) == 0x803);
    assert(dm_vamm_free(v, base) == 0x803 && stats(v).regions == 1);
    assert(alias_mapped() == SMALL && atomic_load(&protect_calls) == before);
    assert(!c.change_pages(c.context, base, SMALL, 1));
    assert(*(unsigned char *)base == 0xa5 && *(unsigned char *)stack == 0x56);
    query_state(base, 5, 1, 0);
    assert(!call(5, stack, base, PAGE) && *(unsigned char *)base == 0x56);
    assert(!dm_vamm_free(v, base) && !alias_mapped() && !stats(v).regions);

    // Current-owner strict scope applies atomically and nests; the ordinary
    // legacy-unlimited path retains its existing policy after scope exit.
    assert(!call(1, 0, APP_POOL - DM_GAME_MEM_USED, 0)); base = 9;
    dm_vamm_stats before_budget = stats(v);
    dm_sa_svc_owner_allocation_begin(); dm_sa_svc_owner_allocation_begin();
    assert(dm_vamm_allocate(v, &base, PAGE, 1) == DM_VAMM_NO_MEMORY && base == 9);
    dm_sa_svc_owner_allocation_end();
    assert(dm_vamm_allocate(v, &base, PAGE, 1) == DM_VAMM_NO_MEMORY && base == 9);
    dm_sa_svc_owner_allocation_end();
    assert(!stats(v).regions && stats(v).available_bytes == before_budget.available_bytes);
    assert(!alias_mapped());
#if DM_GAME_MEM_MAP_LIMIT
    assert(dm_vamm_allocate(v, &base, PAGE, 1) == DM_VAMM_NO_MEMORY && base == 9);
#else
    assert(!dm_vamm_allocate(v, &base, PAGE, 1) && !dm_vamm_free(v, base));
#endif
    assert(!call(1, 0, 0, 0));
    // Strict physical cap includes real image size, not merely profile mem_used.
    g_nmod = 1; g_mod[0].size = APP_POOL; base = 11;
    dm_sa_svc_owner_allocation_begin();
    assert(dm_vamm_allocate(v, &base, PAGE, 1) == DM_VAMM_NO_MEMORY && base == 11);
    dm_sa_svc_owner_allocation_end(); g_nmod = 0; memset(g_mod, 0, sizeof g_mod);
    assert(!alias_mapped() && !stats(v).regions);

    // Native physical ownership remains independent from Vamm reservations.
    uintptr_t raw = c.base + c.size - PAGE;
    assert(!pages(1, raw, PAGE)); query_state(raw, 5, 0, 3);
    assert(dm_vamm_pages(v, raw, PAGE, 1) == DM_VAMM_INVALID);
    assert(info(I_UsedMem) == DM_GAME_MEM_USED + (DM_GAME_MEM_MAP_LIMIT ? PAGE : 0));
    assert(!pages(0, raw, PAGE));
    assert(dm_vamm_allocate(v, &base, c.size, 0) == DM_VAMM_NO_SPACE);
    assert(!alias_mapped() && !stats(v).regions);

    // Complete StandardAllocator -> current owner -> native callbacks -> SVC.
    dm_sa_sdk_owner callbacks = {v, acquire, release, reservation_stats, context_active, SMALL, NULL};
    dm_sa_sdk_provider *provider;
    assert(dm_sa_sdk_provider_create(&provider, &callbacks) == DM_SA_OK);
    dm_sa_config sc = {.provider = dm_sa_sdk_provider_interface(provider), .capacity = (size_t)62 << 30};
    dm_sa_allocator *a; assert(dm_sa_create(&a, &sc) == DM_SA_OK);
    void *p = NULL; assert(dm_sa_allocate(a, 100, 16, &p) == DM_SA_OK);
    memset(p, 0x5a, 100);
    assert(dm_sa_free(a, p) == DM_SA_OK && dm_sa_destroy(a) == DM_SA_OK);
    assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_OK);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, worker, v));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    assert(!alias_mapped() && !stats(v).regions); resource_test(v, &c);
    assert(!dm_vamm_destroy(v));

    c.max_regions = 1; assert(!dm_vamm_create(&v, &c)); dm_vamm_initialize(v);
    assert(!dm_vamm_allocate(v, &base, PAGE, 0)); uintptr_t other = 13;
    assert(dm_vamm_allocate(v, &other, PAGE, 0) == DM_VAMM_NO_MEMORY && other == 13);
    assert(dm_vamm_destroy(v) == DM_VAMM_INVALID);
    assert(!dm_vamm_free(v, base) && !dm_vamm_destroy(v));
}
int main(void) {
    alarm(60);
    struct rlimit core = {0, 0}; assert(!setrlimit(RLIMIT_CORE, &core));
    domain_tests(); result_tests(); native_tests();
    assert(!alias_mapped() && !g_nmap && !atomic_load(&g_heap_cur));
    free(g_alias_bits); free(g_alias_exposed);
    assert(!munmap(g_as, RESERVE));
    printf("native Vamm domain/result/resource/SVC tests passed (limit=%d host=%zu resource=%#llx)\n",
           DM_GAME_MEM_MAP_LIMIT, host_page(), (unsigned long long)DM_GAME_SYS_RESOURCE);
    return 0;
}
