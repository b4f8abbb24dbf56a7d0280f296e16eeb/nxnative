// Independent synthetic sparse-core tests over the real provider/SVC.
#define main implementation_fixture_main
#include "t_standard_allocator_provider.c"
#undef main

#define SENTINEL ((void *)(uintptr_t)0x123)
#define OK(expr) assert((expr) == DM_SA_OK)

typedef struct {
    dm_sa_provider inner;
    dm_sa_span leases[32];
    size_t acquisitions, changes, metadata_calls, metadata_fail_at, metadata_live;
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
    assert(commit); // This slice must never add automatic decommit.
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
static dm_sa_result reviewed_change(void *ctx, uintptr_t base, size_t size, int commit) {
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

static void exact_missing_cost_and_live_boundary(void) {
    sparse_fixture f; sf_init(&f, 4 * POOL);
    void *anchor = alloc(&f, 32, 16); memset(anchor, 0x77, 32);
    memset((unsigned char *)anchor + 32, 0xc2, PAGE - 32);
    decommit(&f, 0, PAGE, POOL - PAGE);
    size_t mapped = alias_mapped(); set_headroom(&f, 2 * PAGE);
    f.c.no_reservations = 1;
    void *p = alloc(&f, 2 * PAGE + 32, 16);
    assert((uintptr_t)p >= (uintptr_t)anchor + 32 && (uintptr_t)p < (uintptr_t)anchor + PAGE);
    assert(f.c.acquisitions == 1);
    assert(alias_mapped() == mapped + competitor_size + 2 * PAGE && page_budget(&f) == 0);
    pattern(anchor, 32, 0x77);
    size_t prefix = (uintptr_t)anchor + PAGE - (uintptr_t)p;
    pattern(p, prefix, 0xc2);
    pattern((unsigned char *)p + prefix, 2 * PAGE + 32 - prefix, 0);
    f.c.inner.begin(f.c.inner.context);
    dm_sa_page_run run = get_run(&f.c.inner, &f.c.leases[0], 3 * PAGE, PAGE);
    assert(!run.flags && run.size == PAGE); f.c.inner.end(f.c.inner.context);
    clear_headroom(); OK(dm_sa_free(f.a, p)); sf_finish(&f);
}
static void metadata_failure_before_commit(void) {
    for (size_t failure = 1; failure <= 2; ++failure) {
        sparse_fixture f; sf_init(&f, 4 * POOL);
        void *anchor = alloc(&f, 32, 16); memset(anchor, 0x6c, 32);
        decommit(&f, 0, PAGE, POOL - PAGE);
        size_t mapped = alias_mapped(), meta = f.c.metadata_live, changes = f.c.changes;
        f.c.metadata_fail_at = f.c.metadata_calls + failure;
        void *out = SENTINEL;
        assert(dm_sa_allocate(f.a, 2 * PAGE, 256, &out) == DM_SA_NO_MEMORY);
        assert(out == SENTINEL && alias_mapped() == mapped && f.c.changes == changes);
        assert(f.c.metadata_live == meta && read_stats(&f).allocation_count == 1);
        pattern(anchor, 32, 0x6c); f.c.metadata_fail_at = 0; sf_finish(&f);
    }
}
static void realloc_old_source_races_commit(void) {
    sparse_fixture f; sf_init(&f, 4 * POOL);
    void *old = alloc(&f, PAGE, PAGE); memset(old, 0x5a, PAGE);
    void *blocker = alloc(&f, PAGE, PAGE); memset(blocker, 0x72, PAGE);
    decommit(&f, 0, 2 * PAGE, POOL - 2 * PAGE);
    size_t mapped = alias_mapped();
    borrowed_old = (uintptr_t)old; protect_borrowed_old = 1;
    void *out = SENTINEL;
    assert(dm_sa_reallocate(f.a, old, 3 * PAGE, &out) == DM_SA_BUSY);
    assert(out == SENTINEL && f.o.borrowed == (uintptr_t)old);
    assert(alias_mapped() == mapped + 3 * PAGE); // retained, never unsafe decommit rollback
    assert(read_stats(&f).allocation_count == 2);
    size_t size; OK(dm_sa_get_size(f.a, old, &size)); assert(size == PAGE);
    assert(!mprotect((void *)((uintptr_t)old & ~(host_page() - 1)), host_page(), PROT_READ | PROT_WRITE));
    return_borrow(&f.o); protect_borrowed_old = 0;
    pattern(old, PAGE, 0x5a); pattern(blocker, PAGE, 0x72); sf_finish(&f);
}


static void inplace_growth_boundary_and_borrows(void) {
    for (int race = 0; race <= 2; ++race) {
        sparse_fixture f; sf_init(&f, POOL);
        unsigned char *old = alloc(&f, PAGE + 32, 16); memset(old, 0x64, PAGE + 32);
        memset(old + PAGE + 32, 0xc8, PAGE - 32);
        decommit(&f, 0, 2 * PAGE, POOL - 2 * PAGE);
        size_t mapped = alias_mapped();
        if (race == 1) { borrowed_old = (uintptr_t)old; protect_borrowed_old = 1; }
        if (race == 2) f.o.borrow_after_commit = 1;
        void *out = SENTINEL;
        dm_sa_result result = dm_sa_reallocate(f.a, old, 3 * PAGE + 32, &out);
        assert(result == (race ? DM_SA_BUSY : DM_SA_OK));
        assert(alias_mapped() == mapped + 2 * PAGE);
        size_t actual; OK(dm_sa_get_size(f.a, old, &actual));
        assert(actual == (race ? PAGE + 32 : 3 * PAGE + 32));
        if (race) {
            assert(out == SENTINEL && f.o.borrowed);
            if (race == 1) assert(!mprotect((void *)((uintptr_t)old & ~(host_page() - 1)), host_page(), PROT_READ | PROT_WRITE));
            return_borrow(&f.o); protect_borrowed_old = 0;
        } else assert(out == old);
        pattern(old, PAGE + 32, 0x64); pattern(old + PAGE + 32, PAGE - 32, 0xc8);
        assert(read_stats(&f).allocation_count == 1);
        sf_finish(&f);
    }
}
static void affordable_later_candidate(void) {
    sparse_fixture f; sf_init(&f, POOL);
    void *anchor = alloc(&f, 32, 16); memset(anchor, 0x36, 32);
    memset((unsigned char *)anchor + 8 * PAGE, 0xb4, 8 * PAGE);
    decommit(&f, 0, PAGE, 7 * PAGE);
    set_headroom(&f, 0);
    size_t mapped = alias_mapped(), changes = f.c.changes;
    void *p = alloc(&f, PAGE, 16);
    assert((uintptr_t)p >= (uintptr_t)anchor + 8 * PAGE);
    assert((uintptr_t)p + PAGE <= (uintptr_t)anchor + POOL);
    assert(alias_mapped() == mapped && f.c.changes == changes && f.c.acquisitions == 1);
    pattern(p, PAGE, 0xb4); pattern(anchor, 32, 0x36);
    clear_headroom(); sf_finish(&f);
}

static void stats_shared_physical_budget(void) {
    for (int no_reservations = 0; no_reservations <= 1; ++no_reservations) {
        sparse_fixture f; sf_init(&f, 4 * POOL);
        void *anchor = alloc(&f, 32, 16); memset(anchor, 0x38, 32);
        decommit(&f, 0, PAGE, 8 * PAGE);
        f.c.no_reservations = no_reservations;
        size_t backed_free = POOL - 8 * PAGE - 32;
        set_headroom(&f, 4 * PAGE);
        dm_sa_stats s = read_stats(&f);
        assert(s.allocated_bytes == 32 && s.allocation_count == 1);
        assert(s.total_free == backed_free + 4 * PAGE);
        assert(s.largest_free == 11 * PAGE);
        clear_headroom(); set_headroom(&f, 0);
        s = read_stats(&f);
        assert(s.total_free == backed_free && s.largest_free == 7 * PAGE);
        borrow(&f.o, (uintptr_t)anchor + 9 * PAGE);
        s = read_stats(&f);
        assert(s.total_free == backed_free - PAGE && s.largest_free == 6 * PAGE);
        return_borrow(&f.o); clear_headroom(); sf_finish(&f);
    }
}

static uint32_t rng_state = 0x7c152af3;
static uint32_t rnd(void) {
    uint32_t x = rng_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}
typedef struct { void *ptr; size_t n; unsigned char value; } model_block;
static int compare_blocks(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const model_block *)a)->ptr;
    uintptr_t y = (uintptr_t)((const model_block *)b)->ptr;
    return x < y ? -1 : x > y;
}
static void randomized_sparse_lifetimes(void) {
    sparse_fixture f; sf_init(&f, POOL);
    model_block blocks[32] = {0};
    blocks[0] = (model_block){alloc(&f, 32, 16), 32, 0xa1};
    memset(blocks[0].ptr, blocks[0].value, blocks[0].n);
    for (size_t iteration = 0; iteration < 1400; ++iteration) {
        size_t i = 1 + rnd() % 31;
        model_block *b = &blocks[i];
        void *out = SENTINEL;
        if (!b->ptr) {
            size_t n = 1 + rnd() % (4 * PAGE), align = (size_t)16 << (rnd() % 9);
            dm_sa_result r = dm_sa_allocate(f.a, n, align, &out);
            if (r == DM_SA_OK) {
                size_t rounded = (n + 15) & ~(size_t)15;
                *b = (model_block){out, rounded, (unsigned char)(1 + rnd() % 254)};
                assert(!((uintptr_t)out & (align - 1))); memset(out, b->value, b->n);
            } else assert(r == DM_SA_NO_MEMORY && out == SENTINEL);
        } else if (rnd() % 3 == 0) {
            OK(dm_sa_free(f.a, b->ptr)); memset(b, 0, sizeof *b);
        } else if (rnd() & 1) {
            size_t n = 1 + rnd() % (4 * PAGE);
            dm_sa_result r = dm_sa_reallocate(f.a, b->ptr, n, &out);
            if (r == DM_SA_OK) {
                size_t rounded = (n + 15) & ~(size_t)15;
                pattern(out, b->n < rounded ? b->n : rounded, b->value);
                b->ptr = out; b->n = rounded; memset(out, b->value, b->n);
            } else assert(r == DM_SA_NO_MEMORY && out == SENTINEL);
        } else {
            size_t n = (1 + rnd() % (b->n / 16)) * 16, actual = SIZE_MAX, changes = f.c.changes;
            OK(dm_sa_shrink(f.a, b->ptr, n, &actual)); assert(actual == n);
            b->n = n; assert(f.c.changes == changes); // no automatic Shrink decommit
        }
        size_t count = 0, used = 0;
        model_block sorted[32];
        for (size_t j = 0; j < 32; ++j) if (blocks[j].ptr) {
            size_t actual; OK(dm_sa_get_size(f.a, blocks[j].ptr, &actual)); assert(actual == blocks[j].n);
            pattern(blocks[j].ptr, blocks[j].n, blocks[j].value);
            sorted[count++] = blocks[j]; used += blocks[j].n;
        }
        qsort(sorted, count, sizeof *sorted, compare_blocks);
        uintptr_t base = (uintptr_t)f.c.leases[0].base, prev = base;
        size_t largest = 0;
        for (size_t j = 0; j < count; ++j) {
            assert((uintptr_t)sorted[j].ptr >= prev);
            size_t gap = (uintptr_t)sorted[j].ptr - prev;
            if (gap > largest) largest = gap;
            prev = (uintptr_t)sorted[j].ptr + sorted[j].n;
        }
        if (base + POOL - prev > largest) largest = base + POOL - prev;
        for (size_t page = 0; page < POOL / PAGE; ++page) {
            uintptr_t lo = base + page * PAGE, hi = lo + PAGE;
            int live = 0;
            for (size_t j = 0; j < count; ++j)
                live |= lo < (uintptr_t)sorted[j].ptr + sorted[j].n && (uintptr_t)sorted[j].ptr < hi;
            if (!live && rnd() % 3 == 0) decommit(&f, 0, page * PAGE, PAGE);
        }
        dm_sa_stats stats = read_stats(&f);
        assert(stats.allocation_count == count && stats.allocated_bytes == used);
        assert(stats.total_free == POOL - used && stats.largest_free == largest);
        assert(f.c.acquisitions == 1);
    }
    sf_finish(&f);
}

static void commit_failure_and_postcommit_borrow(void) {
    for (int race = 0; race <= 2; ++race) {
        sparse_fixture f; sf_init(&f, POOL);
        void *anchor = alloc(&f, 32, 16); memset(anchor, 0x4c, 32);
        decommit(&f, 0, PAGE, POOL - PAGE);
        size_t mapped = alias_mapped(), meta = f.c.metadata_live;
        uintptr_t rival = f.o.base + 0x1200000;
        if (race == 0) f.o.fail_pages = 1;
        if (race == 1) { set_headroom(&f, POOL); f.o.race_commit = rival; }
        if (race == 2) f.o.borrow_after_commit = 1;
        void *out = SENTINEL;
        dm_sa_result r = dm_sa_allocate(f.a, 2 * PAGE, 16, &out);
        assert(r == (race == 0 ? DM_SA_PROVIDER_FAILED : race == 1 ? DM_SA_NO_MEMORY : DM_SA_BUSY));
        assert(out == SENTINEL && f.c.metadata_live == meta);
        if (race == 0) assert(alias_mapped() == mapped);
        if (race == 1) {
            assert(alias_mapped() == mapped + competitor_size + POOL);
            assert(!pages(0, rival, POOL)); clear_headroom();
        }
        if (race == 2) {
            assert(f.o.borrowed && alias_mapped() == mapped + 2 * PAGE);
            return_borrow(&f.o);
        }
        pattern(anchor, 32, 0x4c);
        assert(read_stats(&f).allocation_count == 1);
        sf_finish(&f);
    }
}
static void sparse_finalize_and_retry(void) {
    sparse_fixture f; sf_init(&f, 4 * POOL);
    void *anchor = alloc(&f, PAGE, PAGE); memset(anchor, 0x54, PAGE);
    decommit(&f, 0, PAGE, 3 * PAGE);
    borrow(&f.o, (uintptr_t)anchor);
    assert(dm_sa_finalize(f.a) == DM_SA_BUSY);
    size_t size; OK(dm_sa_get_size(f.a, anchor, &size)); assert(size == PAGE);
    return_borrow(&f.o); pattern(anchor, PAGE, 0x54);
    f.o.fail_release = 1;
    assert(dm_sa_finalize(f.a) == DM_SA_PROVIDER_FAILED);
    assert(owned_count(&f.o) == 1 && alias_mapped() == POOL - 3 * PAGE);
    dm_sa_stats s = read_stats(&f); assert(s.closed && !s.allocation_count && !s.allocated_bytes);
    sf_finish(&f);
}

static void stats_error_outputs_and_finalize_preflight(void) {
    sparse_fixture f; sf_init(&f, 4 * POOL);
    void *anchor = alloc(&f, PAGE, PAGE); memset(anchor, 0x84, PAGE);
    decommit(&f, 0, PAGE, POOL - PAGE);
    size_t mapped = alias_mapped(), meta = f.c.metadata_live;
    for (int error = 0; error < 6; ++error) {
        dm_sa_stats expected, actual; memset(&expected, 0xa7, sizeof expected); actual = expected;
        if (error == 0) f.c.fail_run_at = f.c.runs + 1;
        if (error == 1) f.c.fail_budget_at = f.c.budgets + 1;
        if (error == 2) f.o.fail_query = 1;
        if (error >= 3) f.c.bad_run_mode = error - 2;
        assert(dm_sa_get_stats(f.a, &actual) == DM_SA_PROVIDER_FAILED);
        assert(!memcmp(&actual, &expected, sizeof actual));
        assert(alias_mapped() == mapped && f.c.metadata_live == meta);
        f.c.fail_run_at = f.c.fail_budget_at = 0; f.o.fail_query = 0; f.c.bad_run_mode = 0;
    }
    f.c.fail_run_at = f.c.runs + 1;
    assert(dm_sa_finalize(f.a) != DM_SA_OK);
    f.c.fail_run_at = 0;
    size_t size; OK(dm_sa_get_size(f.a, anchor, &size)); assert(size == PAGE);
    pattern(anchor, PAGE, 0x84); assert(!read_stats(&f).closed);
    sf_finish(&f);
}
static void multispan_sparse_finalize(void) {
    sparse_fixture f; sf_init(&f, 4 * POOL);
    void *a = alloc(&f, POOL, PAGE); memset(a, 0x71, PAGE);
    void *b = alloc(&f, PAGE, PAGE); memset(b, 0x82, PAGE);
    assert(f.c.acquisitions == 2);
    size_t size; OK(dm_sa_shrink(f.a, a, PAGE, &size)); assert(size == PAGE);
    decommit(&f, 0, PAGE, POOL - PAGE); decommit(&f, 1, PAGE, POOL - PAGE);
    borrow(&f.o, (uintptr_t)b);
    size_t releases = f.o.release_calls;
    assert(dm_sa_finalize(f.a) == DM_SA_BUSY && f.o.release_calls == releases);
    OK(dm_sa_get_size(f.a, a, &size)); assert(size == PAGE);
    OK(dm_sa_get_size(f.a, b, &size)); assert(size == PAGE);
    return_borrow(&f.o); pattern(a, PAGE, 0x71); pattern(b, PAGE, 0x82);
    f.o.fail_release = 1;
    assert(dm_sa_finalize(f.a) == DM_SA_PROVIDER_FAILED);
    assert(owned_count(&f.o) == 1 && alias_mapped() == PAGE);
    dm_sa_stats stats = read_stats(&f);
    assert(stats.closed && !stats.allocated_bytes && !stats.allocation_count);
    assert(stats.backing_bytes == POOL && stats.committed_bytes == PAGE);
    sf_finish(&f);
}

static void two_cursor_oracle(void) {
    sparse_fixture f; sf_init(&f, 2 * POOL);
    unsigned char *anchor = alloc(&f, 32, 16); memset(anchor, 0x93, 32);
    void *middle = alloc(&f, POOL - 64, 16);
    unsigned char *edge = alloc(&f, 32, 16); assert(edge == anchor + POOL - 32);
    memset(edge, 0x6a, 32); OK(dm_sa_free(f.a, middle));
    const size_t budgets[] = {0, PAGE, 2 * PAGE, 4 * PAGE, 100 * PAGE};
    for (size_t trial = 0; trial < 128; ++trial) {
        unsigned states[POOL / PAGE]; // 0=hole, 1=committed, 2=committed+borrowed
        f.c.inner.begin(f.c.inner.context);
        OK(f.c.inner.pages.change(f.c.inner.context, &f.c.leases[0], 0, POOL, 1));
        f.c.inner.end(f.c.inner.context);
        states[0] = trial % 7 == 0 ? 2 : 1;
        for (size_t i = 1; i < POOL / PAGE; ++i) {
            states[i] = trial < 3 ? (i + trial) % 3 : rnd() % 3;
            if (i == POOL / PAGE - 1) states[i] = states[i] == 2 ? 2 : 1;
            if (!states[i]) decommit(&f, 0, i * PAGE, PAGE);
        }
        // Reverse order intentionally makes the borrow registry unsorted.
        for (size_t i = POOL / PAGE; i-- > 0;)
            if (states[i] == 2) assert(!call(4, f.o.stack + i * PAGE, (uintptr_t)anchor + i * PAGE, PAGE));
        f.c.no_reservations = trial & 1;
        f.c.run_prefix = trial % 4 == 0 ? 1 : trial % 4 == 1 ? 17 : trial % 4 == 2 ? PAGE - 1 : 0;
        for (size_t budget_i = 0; budget_i < sizeof budgets / sizeof *budgets; ++budget_i) {
            size_t headroom = budgets[budget_i]; set_headroom(&f, headroom);
            size_t committed = 0, backed_free = 0, holes = 0;
            for (size_t page = 0; page < POOL / PAGE; ++page) {
                if (states[page]) committed += PAGE;
                if (states[page] == 1) backed_free += page && page != POOL / PAGE - 1 ? PAGE : PAGE - 32;
                if (!states[page]) holes += PAGE;
            }
            size_t extra = f.c.no_reservations ? 0 : headroom < POOL ? headroom : POOL;
            size_t potential = holes + extra;
            if (potential > headroom) potential = headroom;
            size_t largest = extra;
            // Every optimal endpoint is a free-block or page-state boundary;
            // enumerate all such intervals independently of the two-cursor code.
            for (size_t left_page = 0; left_page < POOL / PAGE; ++left_page) {
                size_t left = left_page ? left_page * PAGE : 32, cost = 0;
                for (size_t right_page = left_page; right_page < POOL / PAGE; ++right_page) {
                    if (states[right_page] == 2) break;
                    if (!states[right_page]) cost += PAGE;
                    if (cost > headroom) break;
                    size_t end = (right_page + 1) * PAGE;
                    if (end == POOL) end -= 32;
                    size_t size = end - left;
                    if (size > largest) largest = size;
                }
            }
            dm_sa_stats stats = read_stats(&f);
            assert(stats.committed_bytes == committed);
            if (stats.total_free != backed_free + potential || stats.largest_free != largest) {
                fprintf(stderr, "two-cursor mismatch trial=%zu prefix=%zu budget=%zu: total=%zu/%zu largest=%zu/%zu\n",
                    trial, f.c.run_prefix, headroom, stats.total_free, backed_free + potential, stats.largest_free, largest);
                abort();
            }
            clear_headroom();
        }
        for (size_t i = 0; i < POOL / PAGE; ++i)
            if (states[i] == 2) assert(!call(5, f.o.stack + i * PAGE, (uintptr_t)anchor + i * PAGE, PAGE));
        pattern(anchor, 32, 0x93); pattern(edge, 32, 0x6a);
    }
    f.c.run_prefix = 0; sf_finish(&f);
}

static int candidate_cost(const unsigned *states, size_t off, size_t n, size_t *cost) {
    *cost = 0;
    for (size_t page = off / PAGE; page <= (off + n - 1) / PAGE; ++page) {
        if (states[page] == 2) return 0;
        if (!states[page]) *cost += PAGE;
    }
    return 1;
}
static void allocation_candidate_oracle(void) {
    sparse_fixture f; sf_init(&f, POOL);
    unsigned char *anchor = alloc(&f, 32, 16);
    void *middle = alloc(&f, POOL - 64, 16);
    unsigned char *edge = alloc(&f, 32, 16); OK(dm_sa_free(f.a, middle));
    f.c.no_reservations = 1;
    const size_t budgets[] = {0, PAGE, 2 * PAGE, 4 * PAGE};
    const size_t requests[] = {16, PAGE - 16, PAGE + 16, 2 * PAGE + 32, 5 * PAGE};
    const size_t alignments[] = {16, 256, PAGE, 4 * PAGE};
    for (size_t trial = 0; trial < 16; ++trial) {
        unsigned states[POOL / PAGE];
        f.c.inner.begin(f.c.inner.context);
        OK(f.c.inner.pages.change(f.c.inner.context, &f.c.leases[0], 0, POOL, 1));
        f.c.inner.end(f.c.inner.context);
        memset(anchor, 0x4f, POOL); memset(anchor, 0x93, 32); memset(edge, 0x6a, 32);
        states[0] = states[POOL / PAGE - 1] = 1;
        for (size_t page = 1; page < POOL / PAGE - 1; ++page) {
            states[page] = trial < 3 ? (page + trial) % 3 : rnd() % 3;
            if (!states[page]) decommit(&f, 0, page * PAGE, PAGE);
        }
        for (size_t page = POOL / PAGE; page-- > 0;)
            if (states[page] == 2) assert(!call(4, f.o.stack + page * PAGE, (uintptr_t)anchor + page * PAGE, PAGE));
        f.c.run_prefix = trial & 1 ? 17 : 0;
        for (size_t bi = 0; bi < sizeof budgets / sizeof *budgets; ++bi) {
            size_t headroom = budgets[bi]; set_headroom(&f, headroom);
            for (size_t ri = 0; ri < sizeof requests / sizeof *requests; ++ri)
            for (size_t ai = 0; ai < sizeof alignments / sizeof *alignments; ++ai) {
                size_t n = requests[ri], align = alignments[ai], feasible = 0, backed = 0;
                size_t first = ((uintptr_t)anchor + 32 + align - 1) & ~(uintptr_t)(align - 1);
                for (size_t off = first - (uintptr_t)anchor; off + n <= POOL - 32; off += align) {
                    size_t cost;
                    if (candidate_cost(states, off, n, &cost) && cost <= headroom) {
                        feasible = 1; if (!cost) backed = 1;
                    }
                }
                size_t before = alias_mapped(), changes = f.c.changes;
                void *out = SENTINEL;
                dm_sa_result result = dm_sa_allocate(f.a, n, align, &out);
                if (!feasible) {
                    if (result != DM_SA_NO_MEMORY || out != SENTINEL) {
                        fprintf(stderr, "unexpected candidate trial=%zu budget=%zu n=%zu align=%zu result=%u\n", trial, headroom, n, align, result);
                        abort();
                    }
                    assert(alias_mapped() == before);
                } else {
                    if (result != DM_SA_OK) {
                        fprintf(stderr, "missed candidate trial=%zu budget=%zu n=%zu align=%zu result=%u\n", trial, headroom, n, align, result);
                        abort();
                    }
                    size_t off = (uintptr_t)out - (uintptr_t)anchor, cost;
                    assert(!((uintptr_t)out & (align - 1)) && off >= 32 && off + n <= POOL - 32);
                    assert(candidate_cost(states, off, n, &cost) && cost <= headroom);
                    assert(!backed || !cost);
                    assert(alias_mapped() == before + cost);
                    if (!cost) assert(f.c.changes == changes);
                    for (size_t byte = 0; byte < n; ++byte)
                        assert(((unsigned char *)out)[byte] == (states[(off + byte) / PAGE] ? 0x4f : 0));
                    OK(dm_sa_free(f.a, out));
                    // Restore only original, wholly free holes; no production
                    // allocator operation is allowed to initiate decommit.
                    for (size_t page = 1; page < POOL / PAGE - 1; ++page)
                        if (!states[page]) decommit(&f, 0, page * PAGE, PAGE);
                    assert(alias_mapped() == before);
                }
                pattern(anchor, 32, 0x93); pattern(edge, 32, 0x6a);
            }
            clear_headroom();
        }
        for (size_t page = 0; page < POOL / PAGE; ++page)
            if (states[page] == 2) assert(!call(5, f.o.stack + page * PAGE, (uintptr_t)anchor + page * PAGE, PAGE));
    }
    f.c.run_prefix = 0; sf_finish(&f);
}

static void rounded_commit_preserves_borrowed_boundary(void) {
    sparse_fixture f; sf_init_granule(&f, POOL, 4 * PAGE);
    unsigned char *anchor = alloc(&f, PAGE, 16); memset(anchor, 0xa6, PAGE);
    memset(anchor + PAGE, 0xc4, 3 * PAGE);
    decommit(&f, 0, 4 * PAGE, POOL - 4 * PAGE);
    borrow(&f.o, (uintptr_t)anchor);
    memset((void *)f.o.stack, 0xda, PAGE);
    set_headroom(&f, 4 * PAGE); f.c.no_reservations = 1;
    size_t mapped = alias_mapped();
    void *p = alloc(&f, 4 * PAGE + 16, 16);
    assert((uintptr_t)p >= (uintptr_t)anchor + PAGE && (uintptr_t)p < (uintptr_t)anchor + 4 * PAGE);
    assert(f.c.last_change_offset == 0 && f.c.last_change_size == 8 * PAGE);
    assert(alias_mapped() == mapped + 4 * PAGE && page_budget(&f) == 0);
    // The rounded commit contains the borrowed 4 KiB source in an already-
    // committed 16 KiB logical granule. Its guest destination stays intact,
    // source backing stays committed, and later copyback must remain valid.
    pattern((void *)f.o.stack, PAGE, 0xda);
    f.c.inner.begin(f.c.inner.context);
    dm_sa_page_run run = get_run(&f.c.inner, &f.c.leases[0], 0, PAGE);
    assert(run.flags == (DM_SA_PAGE_COMMITTED | DM_SA_PAGE_BORROWED));
    f.c.inner.end(f.c.inner.context);
    pattern(anchor + PAGE, 3 * PAGE, 0xc4);
    return_borrow(&f.o); pattern(anchor, PAGE, 0xda);
    clear_headroom(); sf_finish(&f);
}

static void malformed_finer_commitment_is_rejected(void) {
    const size_t prefixes[] = {0, 1, 17};
    for (size_t k = 0; k < sizeof prefixes / sizeof prefixes[0]; ++k) {
    sparse_fixture f; sf_init_granule(&f, POOL, 4 * PAGE);
    unsigned char *anchor = alloc(&f, PAGE, 16); memset(anchor, 0xb7, PAGE);
    // Deliberately violate the generic provider's declared logical commitment
    // granule through the lower-level synthetic fixture, never through the core.
    decommit(&f, 0, PAGE, PAGE);
    f.c.run_prefix = prefixes[k];
    size_t before = alias_mapped(), changes = f.c.changes, meta = f.c.metadata_live;
    dm_sa_stats expected, actual; memset(&expected, 0x5a, sizeof expected); actual = expected;
    assert(dm_sa_get_stats(f.a, &actual) == DM_SA_PROVIDER_FAILED);
    assert(!memcmp(&actual, &expected, sizeof actual));
    void *out = SENTINEL;
    assert(dm_sa_allocate(f.a, 2 * PAGE, 16, &out) == DM_SA_PROVIDER_FAILED && out == SENTINEL);
    dm_sa_result result = dm_sa_reallocate(f.a, anchor, 2 * PAGE, &out);
    if (result != DM_SA_PROVIDER_FAILED || out != SENTINEL)
        fprintf(stderr, "malformed finer-commit growth result=%u changes=%zu mapped_delta=%zu\n",
            result, f.c.changes - changes, alias_mapped() - before);
    assert(result == DM_SA_PROVIDER_FAILED && out == SENTINEL);
    assert(alias_mapped() == before && f.c.changes == changes && f.c.metadata_live == meta);
    size_t size; OK(dm_sa_get_size(f.a, anchor, &size)); assert(size == PAGE);
    pattern(anchor, PAGE, 0xb7);
    f.c.inner.begin(f.c.inner.context);
    OK(f.c.inner.pages.change(f.c.inner.context, &f.c.leases[0], PAGE, PAGE, 1));
    f.c.inner.end(f.c.inner.context);
    sf_finish(&f);
    }
}
int main(void) {
    alarm(120);
    malformed_finer_commitment_is_rejected();
#ifdef DM_REVIEW_MALFORMED_ONLY
    return 0;
#endif
#ifndef DM_REVIEW_PHASE2A
    rounded_commit_preserves_borrowed_boundary();
    allocation_candidate_oracle();
#else
    (void)rounded_commit_preserves_borrowed_boundary;
    (void)allocation_candidate_oracle;
#endif
    two_cursor_oracle();
    stats_error_outputs_and_finalize_preflight();
    multispan_sparse_finalize();
#ifdef DM_REVIEW_PHASE2A
    (void)inplace_growth_boundary_and_borrows;
    (void)exact_missing_cost_and_live_boundary;
    (void)metadata_failure_before_commit;
    (void)realloc_old_source_races_commit;
    (void)affordable_later_candidate;
    (void)randomized_sparse_lifetimes;
    (void)commit_failure_and_postcommit_borrow;
    sparse_finalize_and_retry();
    stats_shared_physical_budget();
    puts("independent sparse core 2a: physical budget/statistics, sparse finalize and retained failed release passed");
#else
    inplace_growth_boundary_and_borrows();
    exact_missing_cost_and_live_boundary();
    metadata_failure_before_commit();
    realloc_old_source_races_commit();
    sparse_finalize_and_retry();
    affordable_later_candidate();
    stats_shared_physical_budget();
    randomized_sparse_lifetimes();
    commit_failure_and_postcommit_borrow();
    puts("independent sparse core: missing-cost, live boundaries, metadata, realloc race and sparse finalize passed");
#endif
    return 0;
}
