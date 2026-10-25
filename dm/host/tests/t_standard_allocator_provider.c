// Synthetic existing-SDK-owner callbacks through the REAL production SVC and
// provider. No proprietary code, uploaded binary, or reconstructed SDK routine
// is executed. Fixed test slots model ownership, not production free addresses.
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
int dm_ue_heap_owns(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; } // No Unreal allocator in this fixture.

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

enum { SLOT_COUNT = 8, SLOT_SIZE = 0x100000, PAGE = 0x1000, POOL = 0x10000 };
typedef struct {
    pthread_mutex_t mutex; // synthetic original SDK's Vamm ownership lock
    uintptr_t base, stack, borrowed;
    size_t sizes[SLOT_COUNT], allocation_calls, release_calls, query_calls;
    int fail_allocate, fail_release, fail_query, borrow_acquire, borrow_release;
    uintptr_t borrow_query;
    uintptr_t race_commit;
    uintptr_t grow_metadata_then_fail;
    dm_sa_allocator *reuse_allocator;
    void *reused_pointer;
    size_t page_calls;
    int fail_pages, borrow_change, borrow_after_commit, check_changing;
    dm_sa_provider page_provider;
    dm_sa_span changing_span;
} owner;
static _Thread_local int guest_active = 1;
static int context_active(void *context) { (void)context; return guest_active; }
static void borrow(owner *o, uintptr_t source) {
    assert(!o->borrowed);
    assert(!call(4, o->stack, source, PAGE)); o->borrowed = source;
}
static void return_borrow(owner *o) {
    assert(o->borrowed && !call(5, o->stack, o->borrowed, PAGE)); o->borrowed = 0;
}
static size_t owned_count(owner *o) {
    size_t n = 0; for (unsigned i = 0; i < SLOT_COUNT; ++i) n += !!o->sizes[i]; return n;
}
static void *consume_budget(void *context) {
    // Independent ordinary SVC caller, deliberately outside the provider's
    // thread-local strict scope; this preserves legacy caller policy too.
    uintptr_t address = (uintptr_t)context;
    assert(!pages(1, address, POOL)); return NULL;
}
typedef struct { dm_sa_allocator *allocator; void *pointer; } reuse_job;
static void *reuse_address(void *context) {
    reuse_job *job = context;
    assert(dm_sa_allocate(job->allocator, 32, 16, &job->pointer) == DM_SA_OK);
    return NULL;
}
static uint32_t allocate_original(void *context, uintptr_t *out, size_t size) {
    owner *o = context; assert(guest_active);
    pthread_mutex_lock(&o->mutex); ++o->allocation_calls;
    if (o->grow_metadata_then_fail) {
        assert(!pages(1, o->grow_metadata_then_fail, 0x4000));
        o->grow_metadata_then_fail = 0;
        pthread_mutex_unlock(&o->mutex); return 1;
    }
    if (o->fail_allocate) { o->fail_allocate = 0; pthread_mutex_unlock(&o->mutex); return 1; }
    if (o->race_commit) {
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, consume_budget, (void *)o->race_commit));
        assert(!pthread_join(thread, NULL)); o->race_commit = 0;
    }
    unsigned i = 0; while (i < SLOT_COUNT && o->sizes[i]) ++i;
    if (i == SLOT_COUNT || size > SLOT_SIZE) { pthread_mutex_unlock(&o->mutex); return 1; }
    uintptr_t base = o->base + i * SLOT_SIZE;
    o->sizes[i] = size;
    uint32_t rc = (uint32_t)pages(1, base, size); // recursively takes real g_map_lock
    if (rc) { o->sizes[i] = 0; pthread_mutex_unlock(&o->mutex); return rc; }
    if (o->borrow_acquire) { o->borrow_acquire = 0; borrow(o, base); }
    *out = base; pthread_mutex_unlock(&o->mutex); return 0;
}
static uint32_t free_original(void *context, uintptr_t base) {
    owner *o = context; assert(guest_active);
    pthread_mutex_lock(&o->mutex); ++o->release_calls;
    assert(base >= o->base && !((base - o->base) % SLOT_SIZE));
    unsigned i = (unsigned)((base - o->base) / SLOT_SIZE);
    assert(i < SLOT_COUNT && o->sizes[i]);
    if (o->fail_release) { o->fail_release = 0; pthread_mutex_unlock(&o->mutex); return 0xd401; }
    if (o->borrow_release) { o->borrow_release = 0; borrow(o, base); }
    uint32_t rc = (uint32_t)pages(0, base, o->sizes[i]);
    if (!rc) o->sizes[i] = 0; // failure leaves exact reservation intact
    pthread_mutex_unlock(&o->mutex);
    if (!rc && o->reuse_allocator) {
        // Force another core to reacquire the just-freed address BEFORE this
        // original callback returns and its retiring provider record is erased.
        reuse_job job = {o->reuse_allocator, NULL};
        o->reuse_allocator = NULL;
        pthread_t thread; assert(!pthread_create(&thread, NULL, reuse_address, &job));
        assert(!pthread_join(thread, NULL)); o->reused_pointer = job.pointer;
    }
    return rc;
}
static dm_sa_result change_original(void *context, uintptr_t address, size_t size, int commit) {
    owner *o = context; assert(guest_active);
    pthread_mutex_lock(&o->mutex); ++o->page_calls;
    assert(address >= o->base);
    size_t i = (address - o->base) / SLOT_SIZE, offset = (address - o->base) % SLOT_SIZE;
    assert(i < SLOT_COUNT && offset <= o->sizes[i] && size <= o->sizes[i] - offset);
    if (o->check_changing) {
        dm_sa_provider *p = &o->page_provider;
        dm_sa_page_run run = {123, 456};
        p->begin(p->context);
        assert(!p->available(p->context, &o->changing_span, 0, PAGE));
        assert(p->pages.query_run(p->context, &o->changing_span, 0, PAGE, &run) == DM_SA_BUSY);
        assert(run.size == 123 && run.flags == 456);
        assert(p->pages.change(p->context, &o->changing_span, 0, PAGE, 1) == DM_SA_BUSY);
        assert(p->release(p->context, &o->changing_span) == DM_SA_BUSY);
        p->end(p->context);
        o->check_changing = 0;
    }
    if (o->fail_pages) {
        o->fail_pages = 0; pthread_mutex_unlock(&o->mutex); return DM_SA_PROVIDER_FAILED;
    }
    if (o->borrow_change) { o->borrow_change = 0; borrow(o, address); }
    if (o->race_commit) {
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, consume_budget, (void *)o->race_commit));
        assert(!pthread_join(thread, NULL)); o->race_commit = 0;
    }
    uint64_t rc = pages(commit, address, size);
    if (!rc && commit && o->borrow_after_commit) {
        o->borrow_after_commit = 0; borrow(o, address);
    }
    pthread_mutex_unlock(&o->mutex);
    return !rc ? DM_SA_OK : rc == 0x10801 ? DM_SA_NO_MEMORY :
        rc == 0xd401 ? DM_SA_BUSY : DM_SA_PROVIDER_FAILED;
}
static dm_sa_result reservations(void *context, size_t alignment, dm_sa_provider_stats *out) {
    owner *o = context; assert(guest_active && alignment <= POOL);
    pthread_mutex_lock(&o->mutex); ++o->query_calls;
    if (o->fail_query) { o->fail_query = 0; pthread_mutex_unlock(&o->mutex); return DM_SA_PROVIDER_FAILED; }
    // A real SVC query reacquires BOTH source and physical locks. Thus this test
    // also catches query callbacks accidentally running inside the source scope.
    query_state(o->base + SLOT_COUNT * SLOT_SIZE, 0, 0, 0);
    if (o->borrow_query) { uintptr_t source = o->borrow_query; o->borrow_query = 0; borrow(o, source); }
    size_t slots = SLOT_COUNT - owned_count(o);
    *out = (dm_sa_provider_stats){slots * SLOT_SIZE, slots ? SLOT_SIZE : 0};
    pthread_mutex_unlock(&o->mutex); return DM_SA_OK;
}
static dm_sa_allocator *make_allocator(dm_sa_sdk_provider *provider) {
    dm_sa_config config = {.provider = dm_sa_sdk_provider_interface(provider),
        .capacity = (size_t)62 << 30, .pool_size = POOL};
    dm_sa_allocator *a = NULL; assert(dm_sa_create(&a, &config) == DM_SA_OK); return a;
}
static dm_sa_stats stats(dm_sa_allocator *a) {
    dm_sa_stats result; assert(dm_sa_get_stats(a, &result) == DM_SA_OK); return result;
}
static void no_leases(owner *o, dm_sa_allocator *a) {
    dm_sa_stats s = stats(a);
    assert(!owned_count(o) && !s.backing_bytes && !s.span_count && !s.allocated_bytes);
}
static void *worker(void *context) {
    dm_sa_allocator *a = context;
    for (unsigned i = 0; i < 200; ++i) {
        void *p = NULL, *q = NULL;
        assert(dm_sa_allocate(a, 64, 16, &p) == DM_SA_OK);
        memset(p, (int)i, 64);
        assert(dm_sa_reallocate(a, p, 128, &q) == DM_SA_OK);
        for (unsigned j = 0; j < 64; ++j) assert(((unsigned char *)q)[j] == (unsigned char)i);
        assert(dm_sa_free(a, q) == DM_SA_OK);
    }
    return NULL;
}
static dm_sa_page_run get_run(dm_sa_provider *p, const dm_sa_span *span,
                               size_t offset, size_t maximum) {
    dm_sa_page_run run = {0};
    assert(p->pages.query_run(p->context, span, offset, maximum, &run) == DM_SA_OK);
    assert(run.size && run.size <= maximum);
    return run;
}
static void test_partial_pages(owner *o, dm_sa_sdk_owner callbacks) {
    // Direct lease operations only. Production runtime and allocator Shrink,
    // Free, Trim and reuse have NOT opted in to this experimental capability.
    callbacks.change_pages = change_original;
    dm_sa_sdk_provider *provider = NULL;
    assert(dm_sa_sdk_provider_create(&provider, &callbacks) == DM_SA_OK);
    dm_sa_provider p = dm_sa_sdk_provider_interface(provider);
    assert(p.pages.granule == PAGE && p.pages.query_run && p.pages.budget && p.pages.change);
    dm_sa_config config = {.provider = p, .capacity = POOL};
    dm_sa_allocator *empty = NULL;
    assert(dm_sa_create(&empty, &config) == DM_SA_OK && dm_sa_destroy(empty) == DM_SA_OK);
    for (unsigned i = 0; i < 6; ++i) {
        dm_sa_config invalid = config;
        switch (i) {
        case 0: invalid.provider.pages.granule = 0; break;
        case 1: invalid.provider.pages.granule = 3; break;
        case 2: invalid.provider.pages.granule = 2 * PAGE; break;
        case 3: invalid.provider.pages.query_run = NULL; break;
        case 4: invalid.provider.pages.budget = NULL; break;
        case 5: invalid.provider.pages.change = NULL; break;
        }
        empty = (void *)1;
        assert(dm_sa_create(&empty, &invalid) == DM_SA_INVALID && empty == (void *)1);
    }
    dm_sa_span leases[SLOT_COUNT];
    p.begin(p.context);
    for (size_t i = 0; i < SLOT_COUNT; ++i)
        assert(p.acquire(p.context, POOL, POOL, &leases[i]) == DM_SA_OK);
    dm_sa_span lease = leases[0];
    unsigned char *bytes = lease.base;
    memset(bytes, 0xa5, POOL);
    size_t mapped = alias_mapped(), budget;
    assert(p.pages.budget(p.context, &budget) == DM_SA_OK);
    dm_sa_provider_stats reservation;
    assert(p.query(p.context, POOL, &reservation) == DM_SA_OK);
    assert(!reservation.available_bytes && !reservation.largest_span && budget > POOL);

    dm_sa_page_run run = get_run(&p, &lease, 19, POOL - 19);
    assert(run.size == POOL - 19 && run.flags == DM_SA_PAGE_COMMITTED);
    o->page_provider = p; o->changing_span = lease; o->check_changing = 1;
    assert(p.pages.change(p.context, &lease, PAGE, 2 * PAGE, 0) == DM_SA_OK);
    assert(!o->check_changing && alias_mapped() == mapped - 2 * PAGE);
    assert(o->sizes[0] == POOL && owned_count(o) == SLOT_COUNT);
    assert(!p.available(p.context, &lease, 0, POOL));
    run = get_run(&p, &lease, 19, POOL - 19);
    assert(run.size == PAGE - 19 && run.flags == DM_SA_PAGE_COMMITTED);
    run = get_run(&p, &lease, PAGE + 7, POOL - PAGE - 7);
    assert(run.size == 2 * PAGE - 7 && !run.flags);
    run = get_run(&p, &lease, 3 * PAGE, 37);
    assert(run.size == 37 && run.flags == DM_SA_PAGE_COMMITTED);
    for (size_t i = 0; i < PAGE; ++i) assert(bytes[i] == 0xa5 && bytes[3 * PAGE + i] == 0xa5);
    assert(p.pages.change(p.context, &lease, PAGE, 2 * PAGE, 0) == DM_SA_OK);
    assert(alias_mapped() == mapped - 2 * PAGE); // repeated decommit counts once
    size_t after_budget;
    assert(p.pages.budget(p.context, &after_budget) == DM_SA_OK && after_budget == budget + 2 * PAGE);

    // Exact lease identity, range/alignment, commit mode and unchanged outputs.
    dm_sa_span foreign = lease; ++foreign.token;
    run = (dm_sa_page_run){123, 456};
    size_t calls = o->page_calls;
    assert(p.pages.query_run(p.context, &foreign, 0, PAGE, &run) == DM_SA_NOT_FOUND);
    assert(run.size == 123 && run.flags == 456);
    assert(p.pages.query_run(p.context, &lease, POOL, 1, &run) == DM_SA_INVALID);
    assert(p.pages.query_run(p.context, &lease, 0, 0, &run) == DM_SA_INVALID);
    assert(p.pages.query_run(p.context, &lease, SIZE_MAX, 1, &run) == DM_SA_INVALID);
    assert(p.pages.query_run(p.context, &lease, 0, SIZE_MAX, &run) == DM_SA_INVALID);
    assert(run.size == 123 && run.flags == 456);
    assert(p.pages.change(p.context, &foreign, 0, PAGE, 0) == DM_SA_NOT_FOUND);
    assert(p.pages.change(p.context, &lease, 1, PAGE, 0) == DM_SA_INVALID);
    assert(p.pages.change(p.context, &lease, 0, PAGE + 1, 0) == DM_SA_INVALID);
    assert(p.pages.change(p.context, &lease, POOL, PAGE, 0) == DM_SA_INVALID);
    assert(p.pages.change(p.context, &lease, 0, PAGE, 2) == DM_SA_INVALID);
    assert(o->page_calls == calls && alias_mapped() == mapped - 2 * PAGE);
    o->fail_pages = 1;
    assert(p.pages.change(p.context, &lease, PAGE, 2 * PAGE, 1) == DM_SA_PROVIDER_FAILED);
    assert(alias_mapped() == mapped - 2 * PAGE);
    guest_active = 0; calls = o->page_calls;
    assert(p.pages.change(p.context, &lease, PAGE, 2 * PAGE, 1) == DM_SA_PROVIDER_FAILED);
    assert(o->page_calls == calls && alias_mapped() == mapped - 2 * PAGE);
    guest_active = 1;
    assert(p.pages.change(p.context, &lease, 0, 4 * PAGE, 1) == DM_SA_OK);
    assert(alias_mapped() == mapped);
    for (size_t i = 0; i < 4 * PAGE; ++i)
        assert(bytes[i] == (i >= PAGE && i < 3 * PAGE ? 0 : 0xa5));
    assert(p.pages.change(p.context, &lease, 0, 4 * PAGE, 1) == DM_SA_OK);
    assert(alias_mapped() == mapped); // already committed pages are not charged twice
    EXPECT_MEMORY_PROTECTION_FAILURE(p.pages.change(p.context, &lease, 0, POOL, 0), bytes, POOL);
    p.end(p.context);

    // Both existing and newly raced borrows block decommit; a failed page
    // transaction keeps its reservation, live bytes and exact physical charge.
    borrow(o, (uintptr_t)bytes + PAGE);
    p.begin(p.context);
    run = get_run(&p, &lease, 0, POOL);
    assert(run.size == PAGE && run.flags == DM_SA_PAGE_COMMITTED);
    run = get_run(&p, &lease, PAGE + 3, POOL - PAGE - 3);
    assert(run.size == PAGE - 3 && run.flags == (DM_SA_PAGE_COMMITTED | DM_SA_PAGE_BORROWED));
    calls = o->page_calls;
    assert(p.pages.change(p.context, &lease, 0, 3 * PAGE, 0) == DM_SA_BUSY);
    assert(o->page_calls == calls && alias_mapped() == mapped);
    assert(p.pages.change(p.context, &lease, 0, 3 * PAGE, 1) == DM_SA_OK);
    assert(!p.available(p.context, &lease, PAGE, PAGE));
    p.end(p.context); return_borrow(o);
    p.begin(p.context);
    o->borrow_change = 1;
    assert(p.pages.change(p.context, &lease, 0, PAGE, 0) == DM_SA_BUSY);
    assert(alias_mapped() == mapped && o->sizes[0] == POOL);
    p.end(p.context); return_borrow(o);
    p.begin(p.context);
    assert(p.pages.change(p.context, &lease, PAGE, PAGE, 0) == DM_SA_OK);
    o->borrow_after_commit = 1;
    assert(p.pages.change(p.context, &lease, PAGE, PAGE, 1) == DM_SA_OK);
    assert(alias_mapped() == mapped && !p.available(p.context, &lease, PAGE, PAGE));
    assert(p.release(p.context, &lease) == DM_SA_BUSY);
    p.end(p.context); return_borrow(o);

    // Recommit cannot overdraw strict headroom, including an unlimited legacy
    // profile and a competitor committing after a valid budget snapshot.
    p.begin(p.context);
    assert(p.pages.change(p.context, &lease, 0, POOL, 0) == DM_SA_OK);
    p.end(p.context);
    size_t other = (SLOT_COUNT - 1) * POOL;
    assert(!call(1, 0, 0x1e00000 - other - POOL, 0));
    p.begin(p.context);
    assert(p.pages.budget(p.context, &after_budget) == DM_SA_OK && after_budget == POOL);
    o->race_commit = o->base + 0x1000000;
    uintptr_t competing = o->race_commit;
    assert(p.pages.change(p.context, &lease, 0, POOL, 1) == DM_SA_NO_MEMORY);
    run = get_run(&p, &lease, 0, POOL);
    assert(run.size == POOL && !run.flags && o->sizes[0] == POOL);
    p.end(p.context);
    uint64_t ordinary = pages(1, competing + POOL, PAGE);
    assert(ordinary == (DM_GAME_MEM_MAP_LIMIT ? UINT64_C(0x10801) : 0));
    if (!ordinary) assert(!pages(0, competing + POOL, PAGE));
    assert(!pages(0, competing, POOL));
    assert(!call(1, 0, 0, 0));

    // A lease with every page decommitted is still owned. Failed release must
    // not misclassify holes as a source borrow or lose its reservation/token.
    assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_BUSY);
    p.begin(p.context); o->fail_release = 1;
    assert(p.release(p.context, &lease) == DM_SA_PROVIDER_FAILED);
    assert(o->sizes[0] == POOL);
    for (size_t i = 0; i < SLOT_COUNT; ++i)
        assert(p.release(p.context, &leases[i]) == DM_SA_OK);
    run = (dm_sa_page_run){123, 456};
    assert(p.pages.query_run(p.context, &lease, 0, PAGE, &run) == DM_SA_NOT_FOUND);
    assert(run.size == 123 && run.flags == 456);
    // Walk across bitmap-word boundaries and exercise generation identity
    // after reacquiring the same address. The ledger never treats holes as VA.
    dm_sa_span wide;
    assert(p.acquire(p.context, 192 * PAGE, POOL, &wide) == DM_SA_OK);
    assert(wide.base == lease.base && wide.token != lease.token);
    assert(p.pages.change(p.context, &lease, 0, PAGE, 1) == DM_SA_NOT_FOUND);
    assert(p.pages.change(p.context, &wide, 63 * PAGE, 4 * PAGE, 0) == DM_SA_OK);
    run = get_run(&p, &wide, 17, wide.size - 17);
    assert(run.size == 63 * PAGE - 17 && run.flags == DM_SA_PAGE_COMMITTED);
    run = get_run(&p, &wide, 63 * PAGE + 17, wide.size - 63 * PAGE - 17);
    assert(run.size == 4 * PAGE - 17 && !run.flags);
    run = get_run(&p, &wide, 67 * PAGE, wide.size - 67 * PAGE);
    assert(run.size == wide.size - 67 * PAGE && run.flags == DM_SA_PAGE_COMMITTED);
    run = get_run(&p, &wide, wide.size - 1, 1);
    assert(run.size == 1 && run.flags == DM_SA_PAGE_COMMITTED);
    foreign = wide; foreign.base = (unsigned char *)foreign.base + PAGE;
    assert(p.pages.query_run(p.context, &foreign, 0, PAGE, &run) == DM_SA_NOT_FOUND);
    foreign = wide; foreign.size -= PAGE;
    assert(p.pages.query_run(p.context, &foreign, 0, PAGE, &run) == DM_SA_NOT_FOUND);
    assert(p.pages.change(p.context, &wide, 0, wide.size, 0) == DM_SA_OK);
    EXPECT_MEMORY_PROTECTION_FAILURE(p.pages.change(p.context, &wide, 0, wide.size, 1), NULL, 0);
    assert(p.release(p.context, &wide) == DM_SA_OK);
    p.end(p.context);
    assert(!owned_count(o) && !alias_mapped());
    assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_OK);
}
int main(void) {
    alarm(60); // recursive source-lock regressions fail, rather than hang CI
    uintptr_t alias = info(I_AliasAddr), stack = info(I_StackAddr);
    uintptr_t aligned = (alias + 0xffff) & ~(uintptr_t)0xffff;
    owner o = {.mutex = PTHREAD_MUTEX_INITIALIZER, .base = aligned + SLOT_SIZE, .stack = stack};
    dm_sa_sdk_owner callbacks = {.context = &o, .allocate_memory = allocate_original,
        .free_address_region = free_original, .query_reservations = reservations,
        .guest_context_active = context_active, .guaranteed_alignment = POOL};
    dm_sa_sdk_provider *provider = NULL;
    assert(dm_sa_sdk_provider_create(&provider, &callbacks) == DM_SA_OK);
    dm_sa_allocator *a = make_allocator(provider);
    assert(!o.allocation_calls && stats(a).capacity == (size_t)62 << 30);

    // Actual pool use (heap plus independent alias maps), never arena capacity.
    assert(!call(1, 0, 0x1800000, 0));
    assert(stats(a).total_free == 0x600000);
    assert(!pages(1, aligned + 0x1000000, 0x200000));
    assert(stats(a).total_free == 0x400000);
    assert(!pages(0, aligned + 0x1000000, 0x200000) && !call(1, 0, 0, 0));

    // A real competing SVC consumes all remaining budget AFTER query and BEFORE
    // the original owner maps. The provider's own map fails atomically in both
    // profile policies; it must not rely on query's now-stale physical bound.
    assert(!call(1, 0, 0x1c00000, 0));
    assert(!pages(1, aligned + 0x1000000, 0x200000 - POOL));
    assert(stats(a).total_free == POOL);
    o.race_commit = aligned + 0x1200000;
    void *raced = (void *)1;
    assert(dm_sa_allocate(a, 32, 16, &raced) == DM_SA_PROVIDER_FAILED && raced == (void *)1);
    assert(!owned_count(&o) && stats(a).total_free == 0);
    // Ordinary unlimited-profile maps retain their previous policy, while a
    // limited-profile caller gets the same production pool-limit result.
    uint64_t extra = pages(1, aligned + 0x1300000, POOL);
    assert(extra == (DM_GAME_MEM_MAP_LIMIT ? UINT64_C(0x10801) : 0));
    if (!extra) assert(!pages(0, aligned + 0x1300000, POOL));
    assert(!pages(0, aligned + 0x1200000, POOL));
    assert(!pages(0, aligned + 0x1000000, 0x200000 - POOL));
    assert(!call(1, 0, 0, 0));

    // Owner node metadata may grow before a later payload allocation fails.
    // That remains SDK-owned and physically charged, not a lost core lease or
    // an invented rollback of the SDK's internal resource bookkeeping.
    assert(!call(1, 0, 0x1800000, 0));
    o.grow_metadata_then_fail = aligned;
    raced = (void *)1;
    assert(dm_sa_allocate(a, 32, 16, &raced) == DM_SA_PROVIDER_FAILED && raced == (void *)1);
    assert(!owned_count(&o) && stats(a).backing_bytes == 0 && stats(a).total_free == 0x5fc000);
    query_state(aligned, 5, 0, 3);
    assert(!pages(0, aligned, 0x4000) && !call(1, 0, 0, 0));

    // Failed owner snapshots preserve the public output and restore the source
    // guard, so a subsequent stats query/SVC operation can make progress.
    dm_sa_stats failed_stats, sentinel_stats;
    memset(&failed_stats, 0xa5, sizeof failed_stats); sentinel_stats = failed_stats;
    o.fail_query = 1;
    assert(dm_sa_get_stats(a, &failed_stats) == DM_SA_PROVIDER_FAILED);
    assert(!memcmp(&failed_stats, &sentinel_stats, sizeof failed_stats));
    assert(stats(a).total_free == SLOT_COUNT * SLOT_SIZE);

    // Failed allocation/backend and wrong guest context do not publish output.
    void *p = (void *)1;
    o.fail_allocate = 1;
    assert(dm_sa_allocate(a, 32, 16, &p) == DM_SA_PROVIDER_FAILED && p == (void *)1);
    EXPECT_MEMORY_PROTECTION_FAILURE(dm_sa_allocate(a, 32, 16, &p), NULL, 0);
    assert(p == (void *)1);
    no_leases(&o, a);
    guest_active = 0;
    size_t calls = o.allocation_calls;
    assert(dm_sa_allocate(a, 32, 16, &p) == DM_SA_PROVIDER_FAILED && p == (void *)1);
    assert(o.allocation_calls == calls); guest_active = 1;
    // An original SDK owner without the explicit-alignment operation remains
    // truthfully capped: no speculative call, padded map or lucky placement.
    assert(dm_sa_allocate(a, 32, 0x20000, &p) == DM_SA_NO_MEMORY && p == (void *)1);
    dm_sa_provider fixed = dm_sa_sdk_provider_interface(provider);
    assert(!fixed.pages.granule && !fixed.pages.query_run && !fixed.pages.budget && !fixed.pages.change);
    dm_sa_provider_stats unsupported = {1, 1};
    dm_sa_span unsupported_lease = {(void *)1, 3, 5};
    fixed.begin(fixed.context);
    assert(fixed.query(fixed.context, 0x20000, &unsupported) == DM_SA_OK);
    assert(!unsupported.available_bytes && !unsupported.largest_span);
    assert(fixed.acquire(fixed.context, POOL, 0x20000, &unsupported_lease) == DM_SA_NO_MEMORY);
    fixed.end(fixed.context);
    assert(unsupported_lease.base == (void *)1 && unsupported_lease.size == 3 && unsupported_lease.token == 5);
    assert(o.allocation_calls == calls);

    // Every returned block is backed; source borrow blocks mutations and shows
    // Locked/no-permission in the real QueryMemory state, including subpages.
    assert(dm_sa_allocate(a, PAGE, 16, &p) == DM_SA_OK);
    memset(p, 0x5a, PAGE); query_state((uintptr_t)p, 5, 0, 3);
    borrow(&o, (uintptr_t)p); query_state((uintptr_t)p, 5, 1, 0);
    assert(dm_sa_free(a, p) == DM_SA_BUSY);
    size_t size = 9;
    assert(dm_sa_shrink(a, p, 32, &size) == DM_SA_BUSY && size == 9);
    assert(dm_sa_finalize(a) == DM_SA_BUSY && !stats(a).closed);
    return_borrow(&o); assert(dm_sa_free(a, p) == DM_SA_OK); no_leases(&o, a);

    // Borrow between SDK allocation and publication: no pointer published; the
    // acquired exact lease survives until its source owner returns it.
    o.borrow_acquire = 1; p = (void *)1;
    assert(dm_sa_allocate(a, 32, 16, &p) == DM_SA_BUSY && p == (void *)1);
    assert(owned_count(&o) == 1 && stats(a).backing_bytes == POOL && !stats(a).allocation_count);
    assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_BUSY);
    assert(dm_sa_trim(a) == DM_SA_BUSY);
    return_borrow(&o); assert(dm_sa_trim(a) == DM_SA_OK); no_leases(&o, a);

    // Deterministic retiring-record/address-reuse race, independent of timing.
    dm_sa_allocator *second = make_allocator(provider);
    assert(dm_sa_allocate(a, 32, 16, &p) == DM_SA_OK);
    o.reuse_allocator = second;
    assert(dm_sa_free(a, p) == DM_SA_OK && o.reused_pointer == p);
    assert(stats(a).backing_bytes == 0 && stats(second).allocation_count == 1);
    assert(dm_sa_free(a, o.reused_pointer) == DM_SA_NOT_FOUND);
    assert(dm_sa_free(second, o.reused_pointer) == DM_SA_OK);
    assert(dm_sa_destroy(second) == DM_SA_OK); no_leases(&o, a);

    // Borrow AFTER release's precheck but BEFORE SVC unmap: failure retains the
    // SDK reservation and core token; copyback then a retry drains it exactly.
    assert(dm_sa_allocate(a, PAGE, 16, &p) == DM_SA_OK);
    memset(p, 0x73, PAGE); o.borrow_release = 1;
    assert(dm_sa_free(a, p) == DM_SA_OK);
    assert(owned_count(&o) == 1 && stats(a).backing_bytes == POOL);
    assert(((unsigned char *)o.stack)[0] == 0x73);
    return_borrow(&o); assert(dm_sa_trim(a) == DM_SA_OK); no_leases(&o, a);

    // A query can also permit a borrow of realloc's OLD source; never copy or
    // free it merely because the NEW lease is available after callback return.
    assert(dm_sa_allocate(a, POOL, 16, &p) == DM_SA_OK);
    memset(p, 0xa4, PAGE); o.borrow_query = (uintptr_t)p;
    void *q = (void *)1;
    assert(dm_sa_reallocate(a, p, 2 * POOL, &q) == DM_SA_BUSY && q == (void *)1);
    assert(stats(a).allocation_count == 1);
    assert(dm_sa_get_size(a, p, &size) == DM_SA_OK && size == POOL);
    return_borrow(&o); assert(dm_sa_free(a, p) == DM_SA_OK); no_leases(&o, a);

    // A logical owner release failure is transactional; finalized core remains closed
    // with exact backing until destroy retries (no SDK fallback or lost lease).
    assert(dm_sa_allocate(a, 32, 16, &p) == DM_SA_OK);
    EXPECT_MEMORY_PROTECTION_FAILURE(dm_sa_finalize(a), p, 32);
    o.fail_release = 1;
    assert(dm_sa_finalize(a) == DM_SA_PROVIDER_FAILED);
    assert(stats(a).closed && stats(a).backing_bytes == POOL && owned_count(&o) == 1);
    assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_BUSY);
    assert(dm_sa_destroy(a) == DM_SA_OK && !owned_count(&o));

    // Shared provider, independent cores: no source-scope -> SDK-lock inversion.
    pthread_t threads[4]; dm_sa_allocator *allocators[4];
    for (unsigned i = 0; i < 4; ++i) {
        allocators[i] = make_allocator(provider);
        assert(!pthread_create(&threads[i], NULL, worker, allocators[i]));
    }
    for (unsigned i = 0; i < 4; ++i) {
        assert(!pthread_join(threads[i], NULL)); assert(dm_sa_destroy(allocators[i]) == DM_SA_OK);
    }
    assert(!owned_count(&o) && !alias_mapped());
    assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_OK);
    test_partial_pages(&o, callbacks);
    assert(!pthread_mutex_destroy(&o.mutex));
    alarm(0);
    puts("standard allocator SDK provider: real-SVC ownership, rollback, races, budget and shared-core tests passed");
    return 0;
}
