// Independent synthetic adversarial review. No private inputs or SDK execution.
#define main implementation_fixture_main
#include "t_standard_allocator_provider.c"
#undef main

static uint32_t rng_state = 0xc0ffee13;
static uint32_t rnd(void) {
    uint32_t x = rng_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

enum { MODEL_PAGES = SLOT_SIZE / PAGE };
static unsigned char model[MODEL_PAGES];
static size_t model_count(void) {
    size_t n = 0; for (size_t i = 0; i < MODEL_PAGES; ++i) n += !!model[i];
    return n;
}
static void verify_query(dm_sa_provider *p, dm_sa_span *s, owner *o, size_t off, size_t max) {
    dm_sa_page_run r = {SIZE_MAX, UINT32_MAX};
    assert(p->pages.query_run(p->context, s, off, max, &r) == DM_SA_OK);
    assert(r.size && r.size <= max);
    size_t last = (off + r.size - 1) / PAGE;
    for (size_t i = off / PAGE; i <= last; ++i) {
        unsigned want = model[i] ? DM_SA_PAGE_COMMITTED : 0;
        if (o->borrowed == (uintptr_t)s->base + i * PAGE) want |= DM_SA_PAGE_BORROWED;
        assert(r.flags == want);
    }
}
static void verify_model(dm_sa_provider *p, dm_sa_span *s, owner *o, size_t empty_budget) {
    size_t n = model_count(), budget;
    assert(alias_mapped() == n * PAGE);
    assert(p->pages.budget(p->context, &budget) == DM_SA_OK);
    assert(budget + n * PAGE == empty_budget);
    for (size_t i = 0; i < MODEL_PAGES; ++i) {
        verify_query(p, s, o, i * PAGE + 13, PAGE - 13);
        if (!model[i] || o->borrowed == (uintptr_t)s->base + i * PAGE) continue;
        unsigned char *b = (unsigned char *)s->base + i * PAGE;
        for (size_t k = 0; k < PAGE; ++k) assert(b[k] == model[i]);
    }
    size_t pos = 0;
    while (pos < s->size) {
        dm_sa_page_run r = get_run(p, s, pos, s->size - pos);
        verify_query(p, s, o, pos, r.size);
        pos += r.size;
    }
    for (size_t k = 0; k < 10; ++k) {
        size_t off = rnd() % s->size, max = 1 + rnd() % (s->size - off);
        verify_query(p, s, o, off, max);
    }
}

static void test_identity(dm_sa_provider *p, dm_sa_span *s, owner *o) {
    dm_sa_span invalid[5] = {*s, *s, *s, *s, *s};
    ++invalid[0].token; invalid[1].base = (unsigned char *)s->base + PAGE;
    invalid[2].size -= PAGE; invalid[3].token = 0; invalid[4].base = NULL;
    size_t calls = o->page_calls;
    for (size_t i = 0; i < 6; ++i) {
        dm_sa_span *q = i < 5 ? &invalid[i] : NULL;
        dm_sa_page_run run = {83, 93};
        assert(p->pages.query_run(p->context, q, 0, PAGE, &run) == DM_SA_NOT_FOUND);
        assert(run.size == 83 && run.flags == 93);
        assert(p->pages.change(p->context, q, 0, PAGE, 0) == DM_SA_NOT_FOUND);
        assert(p->pages.change(p->context, q, 0, PAGE, 1) == DM_SA_NOT_FOUND);
    }
    dm_sa_page_run run = {83, 93};
    const size_t pairs[][2] = {{SIZE_MAX,1},{0,SIZE_MAX},{s->size,1},{1,s->size},{0,0}};
    for (size_t i = 0; i < sizeof pairs / sizeof *pairs; ++i) {
        assert(p->pages.query_run(p->context, s, pairs[i][0], pairs[i][1], &run) == DM_SA_INVALID);
        assert(run.size == 83 && run.flags == 93);
        assert(p->pages.change(p->context, s, pairs[i][0], pairs[i][1], 1) == DM_SA_INVALID);
    }
    assert(p->pages.query_run(p->context, s, 0, PAGE, NULL) == DM_SA_INVALID);
    assert(p->pages.budget(p->context, NULL) == DM_SA_INVALID);
    assert(o->page_calls == calls);
}

static void test_model(owner *o, dm_sa_sdk_owner *callbacks) {
    dm_sa_sdk_provider *provider = NULL;
    assert(dm_sa_sdk_provider_create(&provider, callbacks) == DM_SA_OK);
    dm_sa_provider p = dm_sa_sdk_provider_interface(provider);
    dm_sa_span s;
    p.begin(p.context);
    size_t empty_budget; assert(p.pages.budget(p.context, &empty_budget) == DM_SA_OK);
    assert(p.acquire(p.context, SLOT_SIZE, POOL, &s) == DM_SA_OK);
    memset(s.base, 0xa5, s.size); memset(model, 0xa5, sizeof model);
    test_identity(&p, &s, o);
    verify_model(&p, &s, o, empty_budget);
    // Word boundaries, partial host pages, repeated operations and callback failure.
    for (size_t iter = 0; iter < 1200; ++iter) {
        size_t first = rnd() % MODEL_PAGES, n = 1 + rnd() % (MODEL_PAGES - first);
        int commit = rnd() & 1;
        if (iter % 11 == 0 && !o->borrowed) {
            size_t at = rnd() % MODEL_PAGES;
            if (model[at]) { p.end(p.context); borrow(o, (uintptr_t)s.base + at * PAGE); p.begin(p.context); }
        }
        int busy = !commit && o->borrowed &&
            o->borrowed >= (uintptr_t)s.base + first * PAGE &&
            o->borrowed < (uintptr_t)s.base + (first + n) * PAGE;
        int failing = iter % 17 == 0 && !busy;
        if (failing) o->fail_pages = 1;
        size_t before = alias_mapped();
        dm_sa_result result = p.pages.change(p.context, &s, first * PAGE, n * PAGE, commit);
        assert(result == (busy ? DM_SA_BUSY : failing ? DM_SA_PROVIDER_FAILED : DM_SA_OK));
        if (busy || failing) assert(alias_mapped() == before);
        else {
            for (size_t i = first; i < first + n; ++i) {
                if (!commit) model[i] = 0;
                else if (!model[i]) {
                    unsigned char *b = (unsigned char *)s.base + i * PAGE;
                    for (size_t k = 0; k < PAGE; ++k) assert(!b[k]);
                    model[i] = (unsigned char)(1 + i % 250); memset(b, model[i], PAGE);
                }
            }
        }
        verify_model(&p, &s, o, empty_budget);
        if (o->borrowed && iter % 3 == 0) { p.end(p.context); return_borrow(o); p.begin(p.context); }
    }
    if (o->borrowed) { p.end(p.context); return_borrow(o); p.begin(p.context); }
    // A direct legitimate SVC page mutation must be visible immediately, with no provider cache.
    assert(p.pages.change(p.context, &s, 0, s.size, 1) == DM_SA_OK);
    p.end(p.context); assert(!pages(0, (uintptr_t)s.base + 63 * PAGE, 3 * PAGE)); p.begin(p.context);
    dm_sa_page_run r = get_run(&p, &s, 63 * PAGE + 7, 4 * PAGE);
    assert(!r.flags && r.size == 3 * PAGE - 7);
    dm_sa_span old = s;
    assert(p.release(p.context, &s) == DM_SA_OK); // release mixed committed/holes
    assert(p.acquire(p.context, SLOT_SIZE, POOL, &s) == DM_SA_OK);
    assert(s.base == old.base && s.size == old.size && s.token != old.token);
    r = (dm_sa_page_run){83,93};
    assert(p.pages.query_run(p.context, &old, 0, PAGE, &r) == DM_SA_NOT_FOUND);
    assert(r.size == 83 && r.flags == 93);
    assert(p.pages.change(p.context, &old, 0, PAGE, 0) == DM_SA_NOT_FOUND);
    assert(p.release(p.context, &old) == DM_SA_NOT_FOUND);
    assert(p.pages.change(p.context, &s, 0, s.size, 0) == DM_SA_OK);
    assert(p.release(p.context, &s) == DM_SA_OK); // release all holes
    assert(!alias_mapped());
    p.end(p.context); assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_OK);
}


static struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int phase, reached, resume;
} gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};
static void wait_at_phase(int phase) {
    pthread_mutex_lock(&gate.mutex);
    if (gate.phase == phase) {
        gate.reached = 1; pthread_cond_broadcast(&gate.cond);
        while (!gate.resume) pthread_cond_wait(&gate.cond, &gate.mutex);
    }
    pthread_mutex_unlock(&gate.mutex);
}
static dm_sa_result paused_change(void *context, uintptr_t base, size_t size, int commit) {
    wait_at_phase(1);
    dm_sa_result result = change_original(context, base, size, commit);
    wait_at_phase(2);
    return result;
}
typedef struct { dm_sa_provider p; dm_sa_span s; int commit; dm_sa_result result; } change_job;
static void *change_worker(void *arg) {
    change_job *job = arg;
    job->p.begin(job->p.context);
    job->result = job->p.pages.change(job->p.context, &job->s, PAGE, PAGE, job->commit);
    job->p.end(job->p.context);
    return NULL;
}
static void test_threaded_borrow_windows(owner *o, dm_sa_sdk_owner callbacks) {
    callbacks.change_pages = paused_change;
    dm_sa_sdk_provider *provider = NULL;
    assert(dm_sa_sdk_provider_create(&provider, &callbacks) == DM_SA_OK);
    dm_sa_provider p = dm_sa_sdk_provider_interface(provider);
    dm_sa_span s; p.begin(p.context);
    assert(p.acquire(p.context, POOL, POOL, &s) == DM_SA_OK);
    memset(s.base, 0x79, s.size); p.end(p.context);
    for (int phase = 1; phase <= 2; ++phase) {
        if (phase == 2) {
            p.begin(p.context); assert(p.pages.change(p.context, &s, PAGE, PAGE, 0) == DM_SA_OK); p.end(p.context);
        }
        pthread_mutex_lock(&gate.mutex);
        gate.phase = phase; gate.reached = 0; gate.resume = 0;
        pthread_mutex_unlock(&gate.mutex);
        change_job job = {.p = p, .s = s, .commit = phase == 2};
        pthread_t thread; assert(!pthread_create(&thread, NULL, change_worker, &job));
        pthread_mutex_lock(&gate.mutex);
        while (!gate.reached) pthread_cond_wait(&gate.cond, &gate.mutex);
        pthread_mutex_unlock(&gate.mutex);
        p.begin(p.context);
        dm_sa_page_run r = {83, 93};
        assert(!p.available(p.context, &s, 0, PAGE));
        assert(p.pages.query_run(p.context, &s, 0, PAGE, &r) == DM_SA_BUSY);
        assert(r.size == 83 && r.flags == 93);
        assert(p.pages.change(p.context, &s, 0, PAGE, 1) == DM_SA_BUSY);
        assert(p.release(p.context, &s) == DM_SA_BUSY);
        // A different lease can proceed while this callback is suspended.
        dm_sa_span other; assert(p.acquire(p.context, POOL, POOL, &other) == DM_SA_OK);
        assert(other.base != s.base && p.release(p.context, &other) == DM_SA_OK);
        p.end(p.context);
        assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_BUSY);
        borrow(o, (uintptr_t)s.base + PAGE);
        pthread_mutex_lock(&gate.mutex); gate.resume = 1;
        pthread_cond_broadcast(&gate.cond); pthread_mutex_unlock(&gate.mutex);
        assert(!pthread_join(thread, NULL));
        assert(job.result == (phase == 1 ? DM_SA_BUSY : DM_SA_OK));
        p.begin(p.context);
        assert(!p.available(p.context, &s, PAGE, PAGE));
        r = get_run(&p, &s, PAGE + 3, PAGE - 3);
        assert(r.flags == (DM_SA_PAGE_COMMITTED | DM_SA_PAGE_BORROWED));
        assert(p.release(p.context, &s) == DM_SA_BUSY);
        p.end(p.context); return_borrow(o);
        for (size_t i = 0; i < PAGE; ++i) {
            assert(((unsigned char *)s.base)[i] == 0x79);
            assert(((unsigned char *)s.base)[2 * PAGE + i] == 0x79);
        }
        pthread_mutex_lock(&gate.mutex); gate.phase = 0; pthread_mutex_unlock(&gate.mutex);
    }
    p.begin(p.context); assert(p.release(p.context, &s) == DM_SA_OK); p.end(p.context);
    assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_OK);
}


static void test_unsorted_multiple_borrows(owner *o, dm_sa_sdk_owner callbacks) {
    dm_sa_sdk_provider *provider = NULL;
    assert(dm_sa_sdk_provider_create(&provider, &callbacks) == DM_SA_OK);
    dm_sa_provider p = dm_sa_sdk_provider_interface(provider);
    dm_sa_span s; p.begin(p.context);
    assert(p.acquire(p.context, POOL, POOL, &s) == DM_SA_OK);
    memset(s.base, 0x58, s.size); p.end(p.context);
    const size_t sources[] = {9, 1, 12, 4};
    unsigned borrowed[POOL / PAGE] = {0};
    for (size_t i = 0; i < 4; ++i) {
        assert(!call(4, o->stack + i * 2 * PAGE, (uintptr_t)s.base + sources[i] * PAGE, PAGE));
        borrowed[sources[i]] = 1;
    }
    for (size_t removed = 0; removed <= 4; ++removed) {
        p.begin(p.context);
        for (size_t i = 0; i < POOL / PAGE; ++i) {
            size_t off = i * PAGE + 19;
            dm_sa_page_run r = get_run(&p, &s, off, s.size - off);
            for (size_t j = off / PAGE; j <= (off + r.size - 1) / PAGE; ++j)
                assert(r.flags == (DM_SA_PAGE_COMMITTED | (borrowed[j] ? DM_SA_PAGE_BORROWED : 0)));
        }
        assert(p.pages.change(p.context, &s, 0, POOL, 0) == (removed < 4 ? DM_SA_BUSY : DM_SA_OK));
        p.end(p.context);
        if (removed < 4) {
            // Deletion swaps the last registry entry into this slot: query must
            // remain independent of insertion and current registry ordering.
            assert(!call(5, o->stack + removed * 2 * PAGE,
                (uintptr_t)s.base + sources[removed] * PAGE, PAGE));
            borrowed[sources[removed]] = 0;
        }
    }
    p.begin(p.context); assert(p.release(p.context, &s) == DM_SA_OK); p.end(p.context);
    assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_OK);
}

static size_t forbidden_page_calls;
static dm_sa_result forbidden_query(void *c, const dm_sa_span *s, size_t o, size_t m, dm_sa_page_run *r) {
    (void)c;(void)s;(void)o;(void)m;(void)r; ++forbidden_page_calls; return DM_SA_PROVIDER_FAILED;
}
static dm_sa_result forbidden_budget(void *c, size_t *s) {
    (void)c;(void)s; ++forbidden_page_calls; return DM_SA_PROVIDER_FAILED;
}
static dm_sa_result forbidden_change(void *c, const dm_sa_span *s, size_t o, size_t n, int commit) {
    (void)c;(void)s;(void)o;(void)n;(void)commit; ++forbidden_page_calls; return DM_SA_PROVIDER_FAILED;
}
static void test_core_stays_whole_lease(dm_sa_sdk_owner callbacks) {
    dm_sa_sdk_provider *provider = NULL;
    assert(dm_sa_sdk_provider_create(&provider, &callbacks) == DM_SA_OK);
    dm_sa_provider p = dm_sa_sdk_provider_interface(provider);
    p.pages = (dm_sa_page_provider){PAGE, forbidden_query, forbidden_budget, forbidden_change};
    dm_sa_config cfg = {.capacity = 4 * POOL, .pool_size = POOL, .provider = p};
    dm_sa_allocator *a = NULL; assert(dm_sa_create(&a, &cfg) == DM_SA_OK);
    void *ptr, *replacement;
    assert(dm_sa_allocate(a, 32, 16, &ptr) == DM_SA_OK);
    assert(dm_sa_reallocate(a, ptr, 8192, &replacement) == DM_SA_OK);
    size_t shrunk;
    assert(dm_sa_shrink(a, replacement, 64, &shrunk) == DM_SA_OK && shrunk == 64);
    dm_sa_stats stats;
    assert(dm_sa_get_stats(a, &stats) == DM_SA_OK && stats.allocated_bytes == 64);
    assert(dm_sa_free(a, replacement) == DM_SA_OK);
    assert(dm_sa_allocate(a, 128, 16, &replacement) == DM_SA_OK);
    assert(dm_sa_free(a, replacement) == DM_SA_OK);
    assert(dm_sa_trim(a) == DM_SA_OK);
    assert(dm_sa_destroy(a) == DM_SA_OK);
    assert(!forbidden_page_calls && !alias_mapped());
    assert(dm_sa_sdk_provider_destroy(provider) == DM_SA_OK);
}
int main(void) {
    alarm(120);
    uintptr_t alias = info(I_AliasAddr), stack = info(I_StackAddr);
    owner o = {.mutex = PTHREAD_MUTEX_INITIALIZER,
        .base = ((alias + 0xffff) & ~(uintptr_t)0xffff) + SLOT_SIZE, .stack = stack};
    dm_sa_sdk_owner callbacks = {.context = &o, .allocate_memory = allocate_original,
        .free_address_region = free_original, .query_reservations = reservations,
        .guest_context_active = context_active, .guaranteed_alignment = POOL,
        .change_pages = change_original};
    test_model(&o, &callbacks);
    test_core_stays_whole_lease(callbacks);
    test_threaded_borrow_windows(&o, callbacks);
    test_unsorted_multiple_borrows(&o, callbacks);
    assert(!owned_count(&o) && !alias_mapped());
    assert(!pthread_mutex_destroy(&o.mutex));
    puts("independent partial-page model: 1200 randomized transactions, identity, stale reuse, threaded and unsorted borrows, no core opt-in passed");
    return 0;
}
