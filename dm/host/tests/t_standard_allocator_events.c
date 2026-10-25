// Typed native-observer tests reuse only synthetic originals/provider backing.
// The observer never calls the adapter: concurrent lifecycle work runs on a
// different thread, after the diagnostic callback has begun waiting.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type" // included main is renamed, never called
#define main standard_allocator_adapter_fixture_main
#include "t_standard_allocator_adapter.c"
#undef main
#pragma GCC diagnostic pop

static _Atomic unsigned query_calls;
static _Atomic int query_failure;
static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_changed = PTHREAD_COND_INITIALIZER;
static dm_sa_adapter_event events[64], waiting_snapshot;
static unsigned event_count;
static int block_failure, observer_waiting, observer_continue;

static dm_sa_result counted_query(void *c, size_t alignment, dm_sa_provider_stats *out) {
    atomic_fetch_add(&query_calls, 1);
    return atomic_load(&query_failure) ? DM_SA_PROVIDER_FAILED : query(c, alignment, out);
}
static dm_sa_result event_config(void *c, void *self, size_t capacity,
                                 unsigned cache, dm_sa_config *out) {
    dm_sa_result r = make_config(c, self, capacity, cache, out);
    if (r == DM_SA_OK) out->provider.query = counted_query;
    return r;
}
static void observer(void *context, const dm_sa_adapter_event *event) {
    (void)context;
    pthread_mutex_lock(&event_mutex);
    assert(event_count < sizeof events / sizeof *events);
    events[event_count++] = *event;
    if (block_failure && !observer_waiting && event->kind == DM_SA_EVENT_ALLOCATE_FAILED) {
        waiting_snapshot = *event;
        observer_waiting = 1; pthread_cond_broadcast(&event_changed);
        while (!observer_continue) pthread_cond_wait(&event_changed, &event_mutex);
        // The original core and registry entry were destroyed/replaced while
        // waiting. The callback's identity and by-value snapshot remain valid.
        assert(event->self == waiting_snapshot.self);
        assert(event->generation == waiting_snapshot.generation);
        assert(event->failure_count == waiting_snapshot.failure_count);
        assert(event->stats_result == DM_SA_OK);
        assert(event->stats.allocation_count == 1 && event->stats.allocated_bytes == 32);
        assert(event->stats.backing_bytes == waiting_snapshot.stats.backing_bytes);
    }
    pthread_mutex_unlock(&event_mutex);
}
static dm_sa_adapter_event last_event(void) {
    pthread_mutex_lock(&event_mutex);
    assert(event_count); dm_sa_adapter_event result = events[event_count - 1];
    pthread_mutex_unlock(&event_mutex); return result;
}
static unsigned count_events(void) {
    pthread_mutex_lock(&event_mutex); unsigned n = event_count;
    pthread_mutex_unlock(&event_mutex); return n;
}
static void clear_events(void) {
    pthread_mutex_lock(&event_mutex); event_count = 0;
    pthread_mutex_unlock(&event_mutex);
}
static void install_observer(int enabled) {
    dm_sa_adapter_symbol symbols[22];
    for (unsigned n = 0; n < 22; ++n)
        symbols[n] = (dm_sa_adapter_symbol){dm_sa_adapter_symbol_name(n), fixture[n]};
    dm_sa_adapter_config c = {.proofs = DM_SA_PROOF_ALL, .symbols = symbols, .symbol_count = 22,
        .forward_allocate = profile_allocate, .allocation_alignment = allocation_alignment,
        .make_config = event_config, .release_config = release_config,
        .retain_context = retain_context, .release_context = release_context,
        .fatal = fatal, .event = enabled ? observer : NULL};
    assert(dm_sa_adapter_install(&c) == DM_SA_OK);
}
static void optional_observer_absent(void) {
    // Install cannot be reset, so exercise the ordinary optional-NULL settings
    // in a fresh child before the parent publishes its observer-enabled table.
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        install_observer(0);
        unsigned char self[56]; NEW(self); INIT(self, 16384, 0);
        assert(!ALLOC(self, SIZE_MAX) && !REALLOC(self, NULL, SIZE_MAX));
        FINISH(self); NEW(self);
        assert(!atomic_load(&query_calls) && !count_events());
        assert(dm_sa_adapter_shutdown() == DM_SA_OK); _exit(0);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void query_errors_and_failed_init(void) {
    unsigned char self[56]; NEW(self);
    atomic_store(&query_failure, 1);
    INIT(self, 16384, 0); // failed diagnostic query must not undo successful init
    dm_sa_adapter_event e = last_event();
    assert(e.kind == DM_SA_EVENT_INITIALIZED && e.result == DM_SA_OK);
    assert(e.stats_result == DM_SA_PROVIDER_FAILED && self[0]);
    atomic_store(&query_failure, 0);
    void *p = ALLOC(self, 32); assert(p); memset(p, 0x69, 32);
    atomic_store(&query_failure, 1);
    assert(!ALLOC(self, SIZE_MAX)); e = last_event();
    assert(e.kind == DM_SA_EVENT_ALLOCATE_FAILED && e.result == DM_SA_INVALID);
    assert(e.stats_result == DM_SA_PROVIDER_FAILED && e.failure_count == 1);
    assert(!REALLOC(self, p, SIZE_MAX)); e = last_event();
    assert(e.kind == DM_SA_EVENT_REALLOCATE_FAILED && e.result == DM_SA_INVALID);
    assert(e.stats_result == DM_SA_PROVIDER_FAILED && e.failure_count == 2);
    assert(SIZE(self, p) == 32 && HASH(self).block_count == 1);
    for (unsigned n = 0; n < 32; ++n) assert(((unsigned char *)p)[n] == 0x69);
    atomic_store(&query_failure, 0);
    FREE(self, p);
    unsigned queries = atomic_load(&query_calls);
    FINISH(self); e = last_event();
    assert(e.kind == DM_SA_EVENT_FINALIZED && e.result == DM_SA_OK);
    assert(e.stats_result == DM_SA_CLOSED && atomic_load(&query_calls) == queries);
    NEW(self); unsigned before = count_events();
    factory_target = self; factory_mode = FACTORY_FAIL;
    expect_thread_failure(initialize_instance, self, DM_SA_PROVIDER_FAILED);
    assert(count_events() == before && !self[0] && !dm_sa_adapter_is_managed(self));
    factory_mode = FACTORY_NORMAL; factory_target = NULL;
}
static void sampling_and_retained_finalize(void) {
    unsigned char self[56]; NEW(self); INIT(self, 16384, 0);
    uint64_t generation = last_event().generation;
    clear_events(); unsigned queries = atomic_load(&query_calls);
    for (unsigned n = 1; n <= 17; ++n)
        assert(n & 1 ? !ALLOC(self, SIZE_MAX) : !REALLOC(self, NULL, SIZE_MAX));
    static const uint64_t expected[] = {1, 2, 3, 4, 8, 16};
    assert(count_events() == sizeof expected / sizeof *expected);
    assert(atomic_load(&query_calls) == queries + count_events());
    for (unsigned n = 0; n < count_events(); ++n) {
        assert(events[n].generation == generation && events[n].failure_count == expected[n]);
        assert(events[n].stats_result == DM_SA_OK);
    }
    FINISH(self); NEW(self); INIT(self, 16384, 0);
    assert(last_event().generation != generation);
    assert(!ALLOC(self, SIZE_MAX));
    assert(last_event().failure_count == 1); // a new generation restarts sampling
    assert(ALLOC(self, 32)); fail_release = 1;
    expect_thread_failure(finalize_instance, self, DM_SA_PROVIDER_FAILED);
    dm_sa_adapter_event e = last_event();
    assert(e.kind == DM_SA_EVENT_FINALIZED && e.result == DM_SA_PROVIDER_FAILED);
    assert(e.stats_result == DM_SA_OK && e.stats.closed && e.stats.backing_bytes && self[0]);
    fail_release = 0; FINISH(self); NEW(self); clear_events();
}
static void *failed_allocate_worker(void *self) { assert(!ALLOC(self, SIZE_MAX)); return NULL; }
static void observer_does_not_pin_old_core(void) {
    unsigned char self[56]; NEW(self); INIT(self, 16384, 0);
    assert(ALLOC(self, 32));
    pthread_mutex_lock(&event_mutex); block_failure = 1;
    pthread_mutex_unlock(&event_mutex);
    pthread_t thread; assert(!pthread_create(&thread, NULL, failed_allocate_worker, self));
    pthread_mutex_lock(&event_mutex);
    while (!observer_waiting) pthread_cond_wait(&event_changed, &event_mutex);
    uint64_t old_generation = waiting_snapshot.generation;
    pthread_mutex_unlock(&event_mutex);
    // This is not callback reentry. The observing worker is blocked; this
    // independent thread must be free to destroy the core and registry entry.
    FINISH(self); NEW(self); INIT(self, 16384, 0);
    assert(last_event().generation != old_generation);
    void *p = ALLOC(self, 64); assert(p); memset(p, 0x47, 64);
    pthread_mutex_lock(&event_mutex); observer_continue = 1;
    pthread_cond_broadcast(&event_changed); pthread_mutex_unlock(&event_mutex);
    assert(!pthread_join(thread, NULL));
    assert(SIZE(self, p) == 64 && *(unsigned char *)p == 0x47);
    FREE(self, p); FINISH(self); NEW(self);
}
int main(void) {
    alarm(20); optional_observer_absent(); install_observer(1);
    query_errors_and_failed_init(); sampling_and_retained_finalize();
    observer_does_not_pin_old_core();
    assert(!leased && acquisitions == releases && configs_made == configs_released);
    assert(dm_sa_adapter_shutdown() == DM_SA_OK && !context_refs);
    alarm(0);
    puts("StandardAllocator typed events: query failures, sampling, absent observer and concurrent identity reuse: PASS");
    return 0;
}
