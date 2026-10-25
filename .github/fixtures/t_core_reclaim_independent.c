// Independent synthetic stage-3 free-run reclamation tests.
// Native allocator policy, not an SDK reclamation-parity assertion.
#define main implementation_fixture_main
#include "t_standard_allocator_provider.c"
#undef main

#define SENTINEL ((void *)(uintptr_t)0x123)
#define OK(expr) assert((expr) == DM_SA_OK)

typedef struct {
    dm_sa_provider inner;
    dm_sa_span leases[32];
    size_t acquisitions, changes, metadata_calls, metadata_fail_at, metadata_live;
    size_t commit_calls, decommit_calls, fail_change_at;
    int fail_all_decommits;
    size_t last_change_offset, last_change_size;
    size_t runs, fail_run_at, budgets, fail_budget_at, run_prefix;
    int no_reservations, bad_run_mode;
} capture;
static void cap_begin(void *ctx) { capture *c = ctx; c->inner.begin(c->inner.context); }
static void cap_end(void *ctx) { capture *c = ctx; c->inner.end(c->inner.context); }
static dm_sa_result cap_acquire(void *ctx, size_t n, size_t align, dm_sa_span *out) {
    capture *c = ctx;
    dm_sa_result r = c->inner.acquire(c->inner.context, n, align, out);
    if (r == DM_SA_OK) { assert(c->acquisitions < 32); c->leases[c->acquisitions++] = *out; }
    return r;
}
static dm_sa_result cap_release(void *ctx, const dm_sa_span *s) {
    capture *c = ctx; return c->inner.release(c->inner.context, s);
}
static int cap_available(void *ctx, const dm_sa_span *s, size_t off, size_t n) {
    capture *c = ctx; return c->inner.available(c->inner.context, s, off, n);
}
static dm_sa_result cap_query(void *ctx, size_t align, dm_sa_provider_stats *out) {
    capture *c = ctx;
    dm_sa_result r = c->inner.query(c->inner.context, align, out);
    if (r == DM_SA_OK && c->no_reservations) *out = (dm_sa_provider_stats){0};
    return r;
}
static dm_sa_result cap_run(void *ctx, const dm_sa_span *s, size_t off, size_t max, dm_sa_page_run *out) {
    capture *c = ctx;
    if (++c->runs == c->fail_run_at) return DM_SA_PROVIDER_FAILED;
    dm_sa_result r = c->inner.pages.query_run(c->inner.context, s, off, max, out);
    if (r == DM_SA_OK && c->run_prefix && out->size > c->run_prefix) out->size = c->run_prefix;
    if (r == DM_SA_OK && c->bad_run_mode == 1) out->size = 0;
    if (r == DM_SA_OK && c->bad_run_mode == 2) out->size = max + 1;
    if (r == DM_SA_OK && c->bad_run_mode == 3) out->flags = 4;
    return r;
}
static dm_sa_result cap_budget(void *ctx, size_t *out) {
    capture *c = ctx;
    if (++c->budgets == c->fail_budget_at) return DM_SA_PROVIDER_FAILED;
    return c->inner.pages.budget(c->inner.context, out);
}
static dm_sa_result cap_change(void *ctx, const dm_sa_span *s, size_t off, size_t n, int commit) {
    capture *c = ctx; ++c->changes; c->last_change_offset = off; c->last_change_size = n;
    if (commit) ++c->commit_calls; else ++c->decommit_calls;
    if (c->changes == c->fail_change_at || (!commit && c->fail_all_decommits))
        return DM_SA_PROVIDER_FAILED;
    return c->inner.pages.change(c->inner.context, s, off, n, commit);
}
static void *cap_metadata_alloc(void *ctx, size_t n) {
    capture *c = ctx;
    if (++c->metadata_calls == c->metadata_fail_at) return NULL;
    void *p = malloc(n); if (p) ++c->metadata_live; return p;
}
static void cap_metadata_free(void *ctx, void *p) {
    capture *c = ctx; assert(c->metadata_live); --c->metadata_live; free(p);
}
static uintptr_t borrowed_old;
static int protect_borrowed_old;
static int borrow_on_decommit_thread;
typedef struct { owner *o; uintptr_t address; } borrow_job;
static void *borrow_worker(void *arg) {
    borrow_job *j = arg; borrow(j->o, j->address); return NULL;
}
static dm_sa_result reviewed_change(void *ctx, uintptr_t base, size_t size, int commit) {
    if (!commit && borrow_on_decommit_thread) {
        borrow_on_decommit_thread = 0;
        borrow_job j = {ctx, base}; pthread_t thread;
        assert(!pthread_create(&thread, NULL, borrow_worker, &j));
        assert(!pthread_join(thread, NULL));
    }
    dm_sa_result r = change_original(ctx, base, size, commit);
    if (r == DM_SA_OK && commit && borrowed_old) {
        owner *o = ctx; borrow(o, borrowed_old);
        if (protect_borrowed_old) assert(!mprotect((void *)(borrowed_old & ~(host_page() - 1)), host_page(), PROT_NONE));
        borrowed_old = 0;
    }
    return r;
}
typedef struct { owner o; dm_sa_sdk_provider *sdk; capture c; dm_sa_allocator *a; } sparse_fixture;
static void sf_init_granule(sparse_fixture *f, size_t capacity, size_t granule) {
    memset(f, 0, sizeof *f);
    assert(!pthread_mutex_init(&f->o.mutex, NULL));
    f->o.base = ((info(I_AliasAddr) + POOL - 1) & ~(uintptr_t)(POOL - 1)) + SLOT_SIZE;
    f->o.stack = info(I_StackAddr);
    dm_sa_sdk_owner callbacks = {.context = &f->o, .allocate_memory = allocate_original,
        .free_address_region = free_original, .query_reservations = reservations,
        .guest_context_active = context_active, .guaranteed_alignment = POOL,
        .change_pages = reviewed_change};
    OK(dm_sa_sdk_provider_create(&f->sdk, &callbacks));
    f->c.inner = dm_sa_sdk_provider_interface(f->sdk);
    dm_sa_provider p = {.context = &f->c, .granule = granule, .begin = cap_begin, .end = cap_end,
        .acquire = cap_acquire, .release = cap_release, .available = cap_available, .query = cap_query,
        .pages = {granule, cap_run, cap_budget, cap_change}};
    dm_sa_config config = {.provider = p, .capacity = capacity, .pool_size = POOL,
        .metadata_context = &f->c, .metadata_alloc = cap_metadata_alloc, .metadata_free = cap_metadata_free};
    OK(dm_sa_create(&f->a, &config));
}
static void sf_init(sparse_fixture *f, size_t capacity) { sf_init_granule(f, capacity, PAGE); }
static void sf_finish(sparse_fixture *f) {
    OK(dm_sa_destroy(f->a)); OK(dm_sa_sdk_provider_destroy(f->sdk));
    assert(!f->c.metadata_live && !owned_count(&f->o) && !alias_mapped());
    assert(!pthread_mutex_destroy(&f->o.mutex));
}
static void *alloc(sparse_fixture *f, size_t n, size_t align) {
    void *p = SENTINEL; OK(dm_sa_allocate(f->a, n, align, &p)); assert(p != SENTINEL); return p;
}
static size_t competitor_size;
static uintptr_t competitor_base;
static dm_sa_stats read_stats(sparse_fixture *f) {
    dm_sa_stats s; OK(dm_sa_get_stats(f->a, &s));
    size_t reserved = 0; for (size_t i = 0; i < SLOT_COUNT; ++i) reserved += f->o.sizes[i];
    assert(s.backing_bytes == reserved);
    assert(s.committed_bytes == alias_mapped() - competitor_size);
    return s;
}
static void pattern(void *p, size_t n, unsigned char value) {
    for (size_t i = 0; i < n; ++i) assert(((unsigned char *)p)[i] == value);
}
static void decommit(sparse_fixture *f, size_t lease, size_t off, size_t n) {
    dm_sa_provider *p = &f->c.inner; p->begin(p->context);
    OK(p->pages.change(p->context, &f->c.leases[lease], off, n, 0)); p->end(p->context);
}
static size_t page_budget(sparse_fixture *f) {
    dm_sa_provider *p = &f->c.inner; size_t n;
    p->begin(p->context); OK(p->pages.budget(p->context, &n)); p->end(p->context); return n;
}
static void set_headroom(sparse_fixture *f, size_t headroom) {
    assert(!competitor_size && !g_heap_cur);
    size_t total = APP_POOL - DM_GAME_MEM_USED;
    assert(alias_mapped() + headroom <= total);
    size_t need = total - alias_mapped() - headroom;
    size_t heap = need & ~(host_page() - 1);
    assert(!call(1, 0, heap, 0));
    competitor_size = need - heap;
    competitor_base = f->o.base + 0x1000000;
    if (competitor_size) assert(!pages(1, competitor_base, competitor_size));
    assert(page_budget(f) == headroom);
}
static void clear_headroom(void) {
    if (competitor_size) assert(!pages(0, competitor_base, competitor_size));
    competitor_size = 0; assert(!call(1, 0, 0, 0));
}

static uint64_t committed_mask(sparse_fixture *f, size_t lease) {
    uint64_t mask = 0;
    dm_sa_provider *p = &f->c.inner; p->begin(p->context);
    assert(f->c.leases[lease].size <= 64 * PAGE);
    for (size_t i = 0; i < f->c.leases[lease].size / PAGE; ++i) {
        dm_sa_page_run run = get_run(p, &f->c.leases[lease], i * PAGE, PAGE);
        if (run.flags & DM_SA_PAGE_COMMITTED) mask |= UINT64_C(1) << i;
    }
    p->end(p->context); return mask;
}
static void shrink_full_cycle(void) {
    for (int realloc_path = 0; realloc_path < 2; ++realloc_path) {
        sparse_fixture f; sf_init(&f, POOL);
        unsigned char *a = alloc(&f, POOL - 32, 16);
        unsigned char *edge = alloc(&f, 32, 16);
        memset(a, 0x73, POOL - 32); memset(edge, 0x9b, 32);
        size_t changes = f.c.changes, actual = SIZE_MAX;
        set_headroom(&f, 0); size_t mapped = alias_mapped(), budgets = f.c.budgets;
        if (realloc_path) { void *out = SENTINEL; OK(dm_sa_reallocate(f.a, a, 32, &out)); assert(out == a); }
        else { OK(dm_sa_shrink(f.a, a, 32, &actual)); assert(actual == 32); }
        assert(f.c.decommit_calls && f.c.changes > changes && f.c.budgets == budgets);
        assert(committed_mask(&f, 0) == 0x8001 && alias_mapped() == mapped - 14 * PAGE);
        assert(page_budget(&f) == 14 * PAGE);
        pattern(a, PAGE, 0x73); pattern(edge - (PAGE - 32), PAGE - 32, 0x73); pattern(edge, 32, 0x9b);
        OK(dm_sa_get_size(f.a, a, &actual)); assert(actual == 32);
        void *out = SENTINEL; OK(dm_sa_reallocate(f.a, a, POOL - 32, &out)); assert(out == a);
        assert(committed_mask(&f, 0) == 0xffff && alias_mapped() == mapped && !page_budget(&f));
        pattern(a, PAGE, 0x73); pattern(a + PAGE, 14 * PAGE, 0);
        pattern(edge - (PAGE - 32), PAGE - 32, 0x73); pattern(edge, 32, 0x9b);
        clear_headroom(); sf_finish(&f);
    }
}
static void shrink_metadata_and_best_effort_failures(void) {
    for (int realloc_path = 0; realloc_path < 2; ++realloc_path) {
        sparse_fixture f; sf_init(&f, POOL);
        unsigned char *a = alloc(&f, 3 * PAGE, 16), *b = alloc(&f, PAGE, 16);
        memset(a, 0x44, 3 * PAGE); memset(b, 0x55, PAGE);
        size_t mapped = alias_mapped(), changes = f.c.changes, meta = f.c.metadata_live;
        f.c.metadata_fail_at = f.c.metadata_calls + 1;
        size_t actual = SIZE_MAX; void *out = SENTINEL;
        dm_sa_result r = realloc_path ? dm_sa_reallocate(f.a, a, PAGE, &out) : dm_sa_shrink(f.a, a, PAGE, &actual);
        assert(r == DM_SA_NO_MEMORY && out == SENTINEL && actual == SIZE_MAX);
        assert(alias_mapped() == mapped && f.c.changes == changes && f.c.metadata_live == meta);
        OK(dm_sa_get_size(f.a, a, &actual)); assert(actual == 3 * PAGE);
        pattern(a, 3 * PAGE, 0x44); pattern(b, PAGE, 0x55);
        f.c.metadata_fail_at = 0; f.c.fail_all_decommits = 1;
        if (realloc_path) { OK(dm_sa_reallocate(f.a, a, PAGE, &out)); assert(out == a); }
        else { OK(dm_sa_shrink(f.a, a, PAGE, &actual)); assert(actual == PAGE); }
        assert(alias_mapped() == mapped && f.c.decommit_calls);
        OK(dm_sa_get_size(f.a, a, &actual)); assert(actual == PAGE);
        changes = f.c.changes; OK(dm_sa_trim(f.a));
        assert(f.c.changes > changes && f.c.changes - changes <= POOL / PAGE);
        assert(alias_mapped() == mapped); pattern(a, PAGE, 0x44); pattern(b, PAGE, 0x55);
        f.c.fail_all_decommits = 0; OK(dm_sa_trim(f.a));
        assert(committed_mask(&f, 0) == 0x9);
        pattern(a, PAGE, 0x44); pattern(b, PAGE, 0x55); sf_finish(&f);
    }
    for (int fault = 0; fault < 4; ++fault) {
        sparse_fixture f; sf_init(&f, POOL);
        unsigned char *a = alloc(&f, 3 * PAGE, 16), *b = alloc(&f, PAGE, 16);
        memset(a, 0x66, 3 * PAGE); memset(b, 0x77, PAGE);
        size_t mapped = alias_mapped(), changes = f.c.changes, actual = SIZE_MAX;
        if (!fault) f.c.fail_run_at = f.c.runs + 1; else f.c.bad_run_mode = fault;
        OK(dm_sa_shrink(f.a, a, PAGE, &actual)); assert(actual == PAGE);
        assert(alias_mapped() == mapped && f.c.changes == changes);
        f.c.fail_run_at = 0; f.c.bad_run_mode = 0;
        pattern(a, PAGE, 0x66); pattern(b, PAGE, 0x77);
        OK(dm_sa_trim(f.a)); assert(committed_mask(&f, 0) == 0x9); sf_finish(&f);
    }
}
static void trim_finer_borrow_skip_and_partial_host_neighbors(void) {
    sparse_fixture f; sf_init_granule(&f, POOL, 4 * PAGE);
    unsigned char *a = alloc(&f, 32, 16); memset(a, 0x83, POOL);
    borrow(&f.o, (uintptr_t)a + 5 * PAGE);
    memset((void *)f.o.stack, 0xd2, PAGE);
    assert(!mprotect(a + 4 * PAGE, 4 * PAGE, PROT_NONE));
    f.c.run_prefix = 17;
    OK(dm_sa_trim(f.a));
    assert(committed_mask(&f, 0) == 0x00ff);
    assert(f.c.last_change_offset >= 8 * PAGE && f.c.last_change_size <= 8 * PAGE);
    pattern(a, 32, 0x83); pattern((void *)f.o.stack, PAGE, 0xd2);
    assert(!mprotect(a + 4 * PAGE, 4 * PAGE, PROT_READ | PROT_WRITE));
    return_borrow(&f.o); pattern(a + 5 * PAGE, PAGE, 0xd2);
    pattern(a + 4 * PAGE, PAGE, 0x83); pattern(a + 6 * PAGE, 2 * PAGE, 0x83);
    OK(dm_sa_trim(f.a)); assert(committed_mask(&f, 0) == 0x000f);
    pattern(a, 32, 0x83); sf_finish(&f);
}
static void trim_borrow_during_change_and_failures_progress(void) {
    for (int fault = 0; fault < 4; ++fault) {
        sparse_fixture f; sf_init(&f, POOL);
        unsigned char *a = alloc(&f, PAGE, PAGE); memset(a, 0x92, PAGE);
        size_t mapped = alias_mapped(), changes = f.c.changes;
        if (!fault) f.o.borrow_change = 1;
        if (fault == 1) f.o.fail_pages = 1;
        if (fault == 2) f.c.fail_all_decommits = 1;
        if (fault == 3) borrow_on_decommit_thread = 1;
        OK(dm_sa_trim(f.a));
        assert(f.c.changes > changes && f.c.changes - changes <= POOL / PAGE);
        assert(alias_mapped() == mapped); pattern(a, PAGE, 0x92);
        if (!fault || fault == 3) { assert(f.o.borrowed); memset((void *)f.o.stack, 0xb3, PAGE); return_borrow(&f.o); }
        f.c.fail_all_decommits = 0; OK(dm_sa_trim(f.a));
        assert(committed_mask(&f, 0) == 1); pattern(a, PAGE, 0x92); sf_finish(&f);
    }
    // Existing holes and short query prefixes must neither cause callback loops
    // nor stop reclamation of later complete committed runs.
    sparse_fixture f; sf_init(&f, POOL);
    unsigned char *a = alloc(&f, PAGE, PAGE); memset(a, 0x65, PAGE);
    decommit(&f, 0, 3 * PAGE, PAGE); decommit(&f, 0, 7 * PAGE, PAGE);
    f.c.run_prefix = 1; size_t changes = f.c.changes;
    OK(dm_sa_trim(f.a)); assert(committed_mask(&f, 0) == 1);
    assert(f.c.changes > changes && f.c.changes - changes <= POOL / PAGE);
    changes = f.c.changes; OK(dm_sa_trim(f.a)); assert(f.c.changes == changes);
    pattern(a, PAGE, 0x65); sf_finish(&f);
}
static void partial_reclaim_failure_retains_only_failed_runs(void) {
    for (int fail_query = 0; fail_query < 2; ++fail_query) {
        sparse_fixture f; sf_init(&f, POOL);
        unsigned char *a = alloc(&f, PAGE, PAGE); memset(a, 0x4d, PAGE);
        decommit(&f, 0, 3 * PAGE, PAGE); decommit(&f, 0, 7 * PAGE, PAGE);
        size_t changes = f.c.changes;
        if (fail_query) f.c.fail_run_at = f.c.runs + 3;
        else f.c.fail_change_at = f.c.changes + 2;
        OK(dm_sa_trim(f.a));
        // Each page callback is transactional, but optional Trim cleanup is
        // best-effort per run, not an all-or-nothing whole-lease transaction.
        assert(committed_mask(&f, 0) == (fail_query ? 0xff71 : 0x71));
        assert(f.c.changes - changes == (fail_query ? 1 : 3));
        f.c.fail_run_at = f.c.fail_change_at = 0;
        pattern(a, PAGE, 0x4d); OK(dm_sa_trim(f.a));
        assert(committed_mask(&f, 0) == 1); pattern(a, PAGE, 0x4d); sf_finish(&f);
    }
}
static void free_and_dead_span_release_policy(void) {
    sparse_fixture f; sf_init(&f, 2 * POOL);
    unsigned char *a = alloc(&f, PAGE, PAGE), *b = alloc(&f, PAGE, PAGE), *c = alloc(&f, PAGE, PAGE);
    memset(a, 0x34, PAGE); memset(c, 0x56, PAGE);
    size_t changes = f.c.changes, mapped = alias_mapped();
    OK(dm_sa_free(f.a, b)); assert(f.c.changes == changes && alias_mapped() == mapped);
    OK(dm_sa_trim(f.a)); assert(committed_mask(&f, 0) == 5);
    pattern(a, PAGE, 0x34); pattern(c, PAGE, 0x56); sf_finish(&f);

    sf_init(&f, 2 * POOL);
    a = alloc(&f, POOL, PAGE); b = alloc(&f, PAGE, PAGE); memset(b, 0x87, PAGE);
    f.o.fail_release = 1; changes = f.c.changes; mapped = alias_mapped();
    OK(dm_sa_free(f.a, a));
    assert(f.c.changes == changes && alias_mapped() == mapped && owned_count(&f.o) == 2);
    f.o.fail_release = 1;
    assert(dm_sa_trim(f.a) == DM_SA_PROVIDER_FAILED);
    assert(committed_mask(&f, 0) == 0xffff && owned_count(&f.o) == 2);
    assert(committed_mask(&f, 1) == 1); pattern(b, PAGE, 0x87);
    OK(dm_sa_trim(f.a)); assert(owned_count(&f.o) == 1); pattern(b, PAGE, 0x87); sf_finish(&f);
}
static void cross_word_wide_span_and_high_alignment(void) {
    enum { WIDE = 8 * POOL };
    sparse_fixture f; sf_init(&f, WIDE);
    unsigned char *a = alloc(&f, WIDE - 32, 16), *edge = alloc(&f, 32, 16);
    assert(f.c.acquisitions == 1); memset(a, 0x7c, WIDE - 32); memset(edge, 0xae, 32);
    f.c.fail_all_decommits = 1; size_t actual;
    OK(dm_sa_shrink(f.a, a, 32, &actual)); assert(actual == 32);
    f.c.fail_all_decommits = 0;
    size_t bit_base = ((uintptr_t)a - info(I_AliasAddr)) / PAGE;
    size_t boundary = 64 - bit_base % 64;
    assert(boundary > 1 && boundary + 1 < WIDE / PAGE - 1);
    size_t borrows[] = {boundary, boundary - 1, boundary + 1};
    for (size_t i = 0; i < 3; ++i) {
        assert(!call(4, f.o.stack + i * PAGE, (uintptr_t)a + borrows[i] * PAGE, PAGE));
        memset((void *)(f.o.stack + i * PAGE), 0x31 + i, PAGE);
    }
    f.c.run_prefix = 17; OK(dm_sa_trim(f.a));
    assert(alias_mapped() == 5 * PAGE);
    f.c.inner.begin(f.c.inner.context);
    for (size_t p = 0; p < WIDE / PAGE; ++p) {
        dm_sa_page_run run = get_run(&f.c.inner, &f.c.leases[0], p * PAGE, PAGE);
        int borrowed = p >= boundary - 1 && p <= boundary + 1;
        unsigned flags = (p == 0 || p == WIDE / PAGE - 1 || borrowed) ? DM_SA_PAGE_COMMITTED : 0;
        if (borrowed) flags |= DM_SA_PAGE_BORROWED;
        assert(run.flags == flags);
    }
    f.c.inner.end(f.c.inner.context);
    pattern(a, 32, 0x7c); pattern(edge, 32, 0xae);
    for (size_t i = 0; i < 3; ++i) {
        pattern((void *)(f.o.stack + i * PAGE), PAGE, 0x31 + i);
        assert(!call(5, f.o.stack + i * PAGE, (uintptr_t)a + borrows[i] * PAGE, PAGE));
        pattern(a + borrows[i] * PAGE, PAGE, 0x31 + i);
    }
    OK(dm_sa_trim(f.a)); assert(alias_mapped() == 2 * PAGE);
    set_headroom(&f, 3 * PAGE);
    unsigned char *p = alloc(&f, 2 * PAGE + 32, 16 * PAGE);
    assert(!((uintptr_t)p & (16 * PAGE - 1)) && !page_budget(&f));
    assert(p >= a + PAGE && p + 2 * PAGE + 32 <= edge - (PAGE - 32));
    pattern(p, 2 * PAGE + 32, 0); pattern(a, 32, 0x7c); pattern(edge, 32, 0xae);
    assert(alias_mapped() == competitor_size + 5 * PAGE && f.c.acquisitions == 1);
    clear_headroom(); OK(dm_sa_free(f.a, p)); OK(dm_sa_trim(f.a)); assert(alias_mapped() == 2 * PAGE);
    void *out = SENTINEL; OK(dm_sa_reallocate(f.a, a, WIDE - 32, &out)); assert(out == a);
    assert(alias_mapped() == WIDE);
    pattern(a, PAGE, 0x7c); pattern(a + PAGE, WIDE - 2 * PAGE, 0);
    pattern(edge - (PAGE - 32), PAGE - 32, 0x7c); pattern(edge, 32, 0xae); sf_finish(&f);
}
static uint32_t random_seed = 0x3ca98b72;
static uint32_t next_random(void) { uint32_t n = random_seed; n ^= n << 13; n ^= n >> 17; n ^= n << 5; return random_seed = n; }
static void randomized_full_reclaim_cycles(void) {
    sparse_fixture f; sf_init(&f, POOL);
    struct { unsigned char *p; size_t n; unsigned char value; } live[24] = {0};
    live[0].p = alloc(&f, 32, 16); live[0].n = 32; live[0].value = 0xe1;
    memset(live[0].p, live[0].value, live[0].n);
    uintptr_t base = (uintptr_t)f.c.leases[0].base;
    for (size_t iteration = 0; iteration < 1200; ++iteration) {
        size_t i = 1 + next_random() % 23;
        uint64_t before = committed_mask(&f, 0);
        void *out = SENTINEL;
        if (!live[i].p) {
            size_t n = 1 + next_random() % (4 * PAGE), align = (size_t)16 << (next_random() % 10);
            dm_sa_result r = dm_sa_allocate(f.a, n, align, &out);
            if (r == DM_SA_OK) {
                live[i].p = out; live[i].n = (n + 15) & ~(size_t)15; live[i].value = 1 + next_random() % 250;
                for (size_t j = 0; j < live[i].n; ++j)
                    if (!(before & (UINT64_C(1) << (((uintptr_t)out + j - base) / PAGE)))) assert(!((unsigned char *)out)[j]);
                memset(out, live[i].value, live[i].n);
            } else assert(r == DM_SA_NO_MEMORY && out == SENTINEL);
        } else if (next_random() % 3 == 0) {
            OK(dm_sa_free(f.a, live[i].p)); memset(&live[i], 0, sizeof live[i]);
        } else if (next_random() & 1) {
            size_t n = 1 + next_random() % (4 * PAGE);
            dm_sa_result r = dm_sa_reallocate(f.a, live[i].p, n, &out);
            if (r == DM_SA_OK) {
                size_t nn = (n + 15) & ~(size_t)15;
                pattern(out, nn < live[i].n ? nn : live[i].n, live[i].value);
                for (size_t j = live[i].n; j < nn; ++j)
                    if (!(before & (UINT64_C(1) << (((uintptr_t)out + j - base) / PAGE)))) assert(!((unsigned char *)out)[j]);
                live[i].p = out; live[i].n = nn; memset(out, live[i].value, nn);
            } else assert(r == DM_SA_NO_MEMORY && out == SENTINEL);
        } else {
            size_t n = (1 + next_random() % (live[i].n / 16)) * 16, actual = SIZE_MAX;
            OK(dm_sa_shrink(f.a, live[i].p, n, &actual)); assert(actual == n); live[i].n = n;
        }
        uint64_t required = 0; size_t used = 0, count = 0;
        for (size_t j = 0; j < 24; ++j) if (live[j].p) {
            pattern(live[j].p, live[j].n, live[j].value); used += live[j].n; ++count;
            size_t lo = ((uintptr_t)live[j].p - base) / PAGE, hi = ((uintptr_t)live[j].p - base + live[j].n - 1) / PAGE;
            for (size_t p = lo; p <= hi; ++p) required |= UINT64_C(1) << p;
        }
        uint64_t borrowed = 0, current = committed_mask(&f, 0);
        if (iteration % 7 == 0) for (size_t p = 1; p < POOL / PAGE; ++p)
            if (!(required & (UINT64_C(1) << p)) && (current & (UINT64_C(1) << p))) {
                borrowed = UINT64_C(1) << p; borrow(&f.o, base + p * PAGE); memset((void *)f.o.stack, 0xd6, PAGE); break;
            }
        f.c.run_prefix = iteration % 3 ? 17 : 0;
        OK(dm_sa_trim(f.a));
        assert(committed_mask(&f, 0) == (required | borrowed));
        if (borrowed) { uintptr_t source = f.o.borrowed; pattern((void *)f.o.stack, PAGE, 0xd6); return_borrow(&f.o); pattern((void *)source, PAGE, 0xd6); }
        for (size_t j = 0; j < 24; ++j) if (live[j].p) pattern(live[j].p, live[j].n, live[j].value);
        dm_sa_stats s = read_stats(&f); assert(s.allocation_count == count && s.allocated_bytes == used && f.c.acquisitions == 1);
        assert(s.total_free == POOL - used);
    }
    sf_finish(&f);
}
int main(void) {
    alarm(120);
    shrink_full_cycle();
    shrink_metadata_and_best_effort_failures();
    trim_finer_borrow_skip_and_partial_host_neighbors();
    trim_borrow_during_change_and_failures_progress();
    partial_reclaim_failure_retains_only_failed_runs();
    free_and_dead_span_release_policy();
    cross_word_wide_span_and_high_alignment();
    const uint32_t seeds[] = {0x3ca98b72, 0x92e301d5, 0x731acc49};
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; ++i) {
        random_seed = seeds[i]; randomized_full_reclaim_cycles();
    }
    puts("independent core reclaim: 3600 live cycles, cross-word/high-alignment, finer borrows, best-effort failures and recommit preservation passed");
    return 0;
}
