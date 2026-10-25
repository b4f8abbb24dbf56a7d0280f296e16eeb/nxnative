// Independent real runtime factory -> adapters -> native Vamm -> real SVC.
// Guest constructor stubs are synthetic; no owner callback is replaced.
#define main original_runtime_fixture_main
#include "t_memory_runtime.c"
#undef main
#include <sched.h>
#define RR(s,p,n) S(12,void *(*)(void *,void *,size_t))(s,p,n)
#define SH(s,p,n) S(13,size_t (*)(void *,void *,size_t))(s,p,n)
#define SZ(s,p) S(14,size_t (*)(const void *,const void *))(s,p)
#define TRIM(s) S(18,void (*)(const void *))(s)
#define CLEAR(s) S(17,void (*)(const void *))(s)
#define DUMP(s) S(20,void (*)(const void *))(s)
static void unchanged_authority(dm_vamm_adapter_config *v, dm_sa_adapter_config *a) {
    assert(!v->proofs && !v->symbols && !v->symbol_count && !v->experimental_startup);
    assert(!a->proofs && !a->symbols && !a->symbol_count && !a->experimental_startup);
}
#if defined(DM_SA_ADAPTER_TESTING) && defined(DM_VAMM_ADAPTER_TESTING)
static void pattern(void *p, size_t n, unsigned char byte) {
    for (size_t i = 0; i < n; ++i) assert(((unsigned char *)p)[i] == byte);
}
static unsigned provided_calls, provided_finishes, walk_calls;
static unsigned char supplied_buffer[256];
static void provided_original(void *self, void *buffer, size_t size) {
    assert(self && buffer == supplied_buffer && size == sizeof supplied_buffer);
    memset(self, 0, DM_SA_ADAPTER_OBJECT_SIZE); ++provided_calls;
}
static void provided_finish(void *self) { assert(self); ++provided_finishes; }
static int forbidden_walk_callback(void *pointer, size_t size, void *context) {
    (void)pointer; (void)size; (void)context; ++walk_calls; return 1;
}
static void expect_walk_rejection(void *self) {
    jmp_buf jump; escape = &jump;
    if (!setjmp(jump)) { S(19,int (*)(const void *,dm_sa_walk_fn,void *))(self, forbidden_walk_callback, NULL); abort(); }
    escape = NULL; assert(fatal_result == DM_SA_INVALID && !walk_calls);
}
static void provided_buffer_forwarding(void) {
    unsigned char one[DM_SA_ADAPTER_OBJECT_SIZE], two[DM_SA_ADAPTER_OBJECT_SIZE];
    size_t before = alias_mapped(); memset(supplied_buffer, 0xce, sizeof supplied_buffer);
    S(2,void (*)(void *,void *,size_t))(one, supplied_buffer, sizeof supplied_buffer);
    CONSTRUCT(two); S(6,void (*)(void *,void *,size_t))(two, supplied_buffer, sizeof supplied_buffer);
    assert(provided_calls == 2 && !dm_sa_adapter_is_managed(one) && !dm_sa_adapter_is_managed(two));
    unsigned count = atomic_load(&forwards);
    assert(!ALLOC(one, 32) && !ALLOC(two, 32));
    assert(atomic_load(&forwards) == count + 2 && alias_mapped() == before);
    pattern(supplied_buffer, sizeof supplied_buffer, 0xce);
    FINISH(one); FINISH(two); assert(provided_finishes == 2 && alias_mapped() == before);
}
static dm_sa_stats observed;
static unsigned dump_calls;
static void observe(void *ctx, const void *self, const dm_sa_stats *stats) {
    assert(ctx && self && stats); observed = *stats; ++dump_calls;
}
static dm_sa_stats observe_stats(void *self) {
    unsigned n = dump_calls; DUMP(self); assert(dump_calls == n + 1); return observed;
}
static size_t physical_budget(dm_sa_provider *p) {
    size_t n; p->begin(p->context); assert(p->pages.budget(p->context, &n) == DM_SA_OK); p->end(p->context); return n;
}
static uintptr_t budget_rival;
static size_t budget_rival_size;
static void set_headroom(dm_vamm *owner, dm_sa_provider *p, size_t headroom) {
    assert(!budget_rival && !g_heap_cur);
    size_t total = APP_POOL - DM_GAME_MEM_USED;
    assert(alias_mapped() + headroom <= total);
    size_t need = total - alias_mapped() - headroom;
    size_t heap = need & ~(host_page() - 1);
    assert(!call(1, 0, heap, 0));
    budget_rival_size = need - heap;
    if (budget_rival_size) assert(!dm_vamm_allocate(owner, &budget_rival, budget_rival_size, 1));
    assert(physical_budget(p) == headroom);
}
static void clear_headroom(dm_vamm *owner) {
    if (budget_rival) assert(!dm_vamm_free(owner, budget_rival));
    budget_rival = 0; budget_rival_size = 0; assert(!call(1, 0, 0, 0));
}
static void default_factories_have_no_pages(void) {
    for (int mode = 0; mode < 7; ++mode) {
        dm_memory_runtime *r = NULL;
        dm_memory_runtime_contract contract = {mode & 1 ? DM_VAMM_DOMAIN_RAW_ALIAS : DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA,
            (dm_vamm_placement)((mode / 2) % 3)};
        dm_sa_result rc = mode == 6 ? dm_memory_runtime_create(&r, 64) : dm_memory_runtime_create_with_contract(&r, 64, &contract);
        assert(rc == DM_SA_OK);
        dm_vamm_adapter_config v; dm_sa_adapter_config a;
        assert(dm_memory_runtime_configs(r, &v, &a) == DM_SA_OK); unchanged_authority(&v, &a);
        assert(!v.retain_owner(v.context, v.shared_owner)); assert(a.retain_context(a.context) == DM_SA_OK);
        dm_vamm_initialize(v.shared_owner);
        unsigned char client[DM_SA_ADAPTER_OBJECT_SIZE]; dm_sa_config c;
        if (dm_vamm_is_enabled(v.shared_owner)) {
            assert(a.make_config(a.context, client, SMALL, 0, &c) == DM_SA_OK);
            assert(!c.provider.pages.granule && !c.provider.pages.query_run && !c.provider.pages.budget && !c.provider.pages.change);
            a.release_config(a.context, client, &c);
        } else assert(a.make_config(a.context, client, SMALL, 0, &c) == DM_SA_INVALID);
        a.release_context(a.context); v.release_owner(v.context, v.shared_owner);
        assert(dm_memory_runtime_destroy(r) == DM_SA_OK && !alias_mapped());
    }
}
typedef struct {
    uintptr_t source, dest;
    _Atomic int first_borrowed, release_first;
    unsigned successes, misses;
} race_job;
static void *real_borrower(void *arg) {
    race_job *j = arg;
    assert(!call(4, j->dest, j->source, PAGE)); ++j->successes;
    atomic_store(&j->first_borrowed, 1);
    while (!atomic_load(&j->release_first)) sched_yield();
    assert(!call(5, j->dest, j->source, PAGE));
    for (size_t i = 0; i < 600; ++i) {
        uint64_t rc = call(4, j->dest, j->source, PAGE);
        if (!rc) {
            ++j->successes; memset((void *)j->dest, (unsigned char)i, PAGE); sched_yield();
            assert(!call(5, j->dest, j->source, PAGE));
        } else { assert(rc == 0xd401); ++j->misses; }
    }
    return NULL;
}
static void real_owner_concurrent_trim(dm_vamm *owner, void *self, unsigned char *base) {
    uintptr_t source = (uintptr_t)base + PAGE;
    assert(!dm_vamm_pages(owner, source, PAGE, 1));
    race_job j = {.source = source, .dest = info(I_StackAddr) + PAGE}; pthread_t thread;
    assert(!pthread_create(&thread, NULL, real_borrower, &j));
    while (!atomic_load(&j.first_borrowed)) sched_yield();
    TRIM(self); assert(alias_mapped() == 3 * PAGE);
    atomic_store(&j.release_first, 1);
    for (size_t i = 0; i < 600; ++i) {
        assert(!dm_vamm_pages(owner, source, PAGE, 1));
        if (i & 1) TRIM(self); else CLEAR(self);
    }
    assert(!pthread_join(thread, NULL) && j.successes && j.successes + j.misses == 601);
    TRIM(self); assert(alias_mapped() == 2 * PAGE);
    printf("real owner contention: successful borrows=%u missed-after-decommit=%u\n", j.successes, j.misses);
}
#endif
int main(int argc, char **argv) {
    alarm(120);
    int domain = argc > 1 ? atoi(argv[1]) : 0, placement = argc > 2 ? atoi(argv[2]) : 0;
    assert(domain >= 0 && domain <= 1 && placement >= 0 && placement <= 2);
    dm_memory_runtime_contract contract = {domain ? DM_VAMM_DOMAIN_RAW_ALIAS : DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA,
        (dm_vamm_placement)placement};
    dm_memory_runtime *r = (void *)1;
    dm_memory_runtime_contract bad = contract; bad.domain = (dm_vamm_native_domain_policy)99;
    assert(dm_memory_runtime_create_page_capable_with_contract(&r, 64, &bad) == DM_SA_INVALID && r == (void *)1);
    bad = contract; bad.placement = (dm_vamm_placement)99;
    assert(dm_memory_runtime_create_page_capable_with_contract(&r, 64, &bad) == DM_SA_INVALID && r == (void *)1);
    assert(dm_memory_runtime_create_page_capable_with_contract(&r, SIZE_MAX, &contract) == DM_SA_INVALID && r == (void *)1);
    assert(dm_memory_runtime_create_page_capable_with_contract(&r, 0, &contract) == DM_SA_INVALID && r == (void *)1);
    assert(dm_memory_runtime_create_page_capable_with_contract(NULL, 64, &contract) == DM_SA_INVALID);
    assert(dm_memory_runtime_create_page_capable_with_contract(&r, 64, NULL) == DM_SA_INVALID && r == (void *)1);
    owner_only_runtime_tests();
#if defined(DM_SA_ADAPTER_TESTING) && defined(DM_VAMM_ADAPTER_TESTING)
    default_factories_have_no_pages();
#endif
    assert(dm_memory_runtime_create_page_capable_with_contract(&r, 64, &contract) == DM_SA_OK);
    dm_vamm_adapter_config v; dm_sa_adapter_config a;
    assert(dm_memory_runtime_configs(r, &v, &a) == DM_SA_OK); unchanged_authority(&v, &a);
    assert(!dm_vamm_is_initialized(v.shared_owner) && !alias_mapped());
    dm_vamm_symbol vs[DM_VAMM_SYMBOL_COUNT]; dm_sa_adapter_symbol as[DM_SA_ADAPTER_SYMBOL_COUNT];
    census(&v, &a, vs, as);
#if !defined(DM_SA_ADAPTER_TESTING) || !defined(DM_VAMM_ADAPTER_TESTING)
    assert(dm_vamm_adapter_install(&v) == DM_VAMM_INVALID && dm_sa_adapter_install(&a) == DM_SA_INVALID);
    assert(!dm_vamm_adapter_lookup(vs[0].name) && !dm_sa_adapter_lookup(as[0].name));
    assert(!alias_mapped() && !atomic_load(&constructors));
    assert(dm_memory_runtime_destroy(r) == DM_SA_OK);
    puts("independent page-capable constructor grants no production hook authority: PASS");
    return 0;
#else
    a.dump = observe;
    assert(!a.reject_managed_walk && a.forward_allocate == dm_sa_host_forward_allocate);
    a.reject_managed_walk = 1; // Existing bounded-adapter policy remains enforced.
    as[2].function = as[6].function = (dm_sa_guest_fn)provided_original;
    as[8].function = (dm_sa_guest_fn)provided_finish;
    assert(!dm_vamm_adapter_install(&v)); assert(dm_sa_adapter_install(&a) == DM_SA_OK);
    unsigned char manager[64] = {0}, self[DM_SA_ADAPTER_OBJECT_SIZE] = {0};
    V(8,void (*)(void *))(manager); V(0,void (*)(void))(); CONSTRUCT(self);
    if (!dm_vamm_is_enabled(v.shared_owner)) {
        expect_initialize(self, DM_SA_INVALID);
        assert(!alias_mapped() && !dm_sa_adapter_is_managed(self));
        assert(dm_sa_adapter_shutdown() == DM_SA_OK && !dm_vamm_adapter_shutdown());
        assert(dm_memory_runtime_destroy(r) == DM_SA_OK);
        puts("independent page-capable constructor retains disabled-resource rejection: PASS"); return 0;
    }
    INIT(self);
    unsigned char probe[DM_SA_ADAPTER_OBJECT_SIZE]; dm_sa_config c;
    assert(a.make_config(a.context, probe, SMALL, 0, &c) == DM_SA_OK);
    assert(c.provider.pages.granule == PAGE && c.provider.pages.query_run && c.provider.pages.budget && c.provider.pages.change);
    assert(!alias_mapped()); dm_sa_stats st = observe_stats(self); assert(!st.backing_bytes && !st.committed_bytes);
    unsigned char *p = ALLOC(self, SMALL - 32), *edge = ALLOC(self, 32);
    assert(p && edge == p + SMALL - 32 && alias_mapped() == SMALL);
    memset(p, 0x4c, SMALL - 32); memset(edge, 0x9c, 32);
    provided_buffer_forwarding(); expect_walk_rejection(self);
    assert(SZ(self, p) == SMALL - 32 && SZ(self, edge) == 32 && alias_mapped() == SMALL);
    assert(SH(self, p, 32) == 32 && SZ(self, p) == 32 && alias_mapped() == 2 * PAGE);
    st = observe_stats(self); assert(st.backing_bytes == SMALL && st.committed_bytes == 2 * PAGE && st.allocated_bytes == 64);
    dm_vamm_stats owner_stats; assert(!dm_vamm_get_stats(v.shared_owner, PAGE, &owner_stats));
    assert(owner_stats.regions == 1 && owner_stats.reserved_bytes == SMALL);
    set_headroom(v.shared_owner, &c.provider, 0);
    size_t before = alias_mapped(); assert(!RR(self, p, SMALL - 32) && SZ(self, p) == 32 && alias_mapped() == before);
    uintptr_t ordinary = 0; assert(!dm_vamm_allocate(v.shared_owner, &ordinary, PAGE, 0));
    uint32_t ordinary_result = dm_vamm_pages(v.shared_owner, ordinary, PAGE, 1);
    assert(ordinary_result == (DM_GAME_MEM_MAP_LIMIT ? DM_VAMM_NO_MEMORY : DM_VAMM_OK));
    assert(!dm_vamm_free(v.shared_owner, ordinary) && alias_mapped() == before);
    assert(!RR(self, p, SMALL - 32) && SZ(self, p) == 32 && alias_mapped() == before);
    clear_headroom(v.shared_owner);
    set_headroom(v.shared_owner, &c.provider, SMALL - 2 * PAGE);
    assert(RR(self, p, SMALL - 32) == p && alias_mapped() == SMALL && !physical_budget(&c.provider));
    clear_headroom(v.shared_owner);
    pattern(p, PAGE, 0x4c); pattern(p + PAGE, SMALL - 2 * PAGE, 0);
    pattern(edge - (PAGE - 32), PAGE - 32, 0x4c); pattern(edge, 32, 0x9c);
    uintptr_t stack = info(I_StackAddr); assert(!call(4, stack, (uintptr_t)p, PAGE));
    memset((void *)stack, 0xb5, PAGE);
    assert(!SH(self, p, 32) && !RR(self, p, SMALL + PAGE) && SZ(self, p) == SMALL - 32);
    expect_finalize(self, DM_SA_BUSY); assert(self[0] && dm_memory_runtime_destroy(r) == DM_SA_BUSY);
    assert(!call(5, stack, (uintptr_t)p, PAGE)); pattern(p, PAGE, 0xb5);
    assert(SH(self, p, 32) == 32 && alias_mapped() == 2 * PAGE);
    real_owner_concurrent_trim(v.shared_owner, self, p);
    pattern(p, 32, 0xb5); pattern(edge, 32, 0x9c);
    st = observe_stats(self); assert(st.committed_bytes == 2 * PAGE && st.backing_bytes == SMALL && st.allocated_bytes == 64);
    a.release_config(a.context, probe, &c);
    FINISH(self); assert(!alias_mapped());
    assert(!dm_vamm_get_stats(v.shared_owner, PAGE, &owner_stats) && !owner_stats.regions && !owner_stats.reserved_bytes);
    assert(dm_sa_adapter_shutdown() == DM_SA_OK && !dm_vamm_adapter_shutdown());
    assert(dm_memory_runtime_destroy(r) == DM_SA_OK && atomic_load(&forwards) == 2);
    printf("independent runtime pages: real factory/adapter/Vamm/SVC, strict budget, forwarding/Walk gates, borrow/trim/recommit and teardown passed (domain=%d placement=%d)\n", domain, placement);
    return 0;
#endif
}
