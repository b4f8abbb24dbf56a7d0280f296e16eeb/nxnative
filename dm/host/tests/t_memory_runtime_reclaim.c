// Explicit page-capable runtime -> actual adapters -> native Vamm -> real SVC.
// Reuse only the portable harness and tiny synthetic original constructors.
// No replacement runtime factory, provider owner callback or proprietary code.
#define main unused_whole_lease_runtime_main
#define dm_vamm_svc_change_pages reclaim_native_svc_change_pages
#include "t_memory_runtime.c"
#undef dm_vamm_svc_change_pages
#undef main

#if defined(DM_SA_ADAPTER_TESTING) && defined(DM_VAMM_ADAPTER_TESTING)
#define SHRINK(s,p,n) S(13, size_t (*)(void *,void *,size_t))(s,p,n)
#define REALLOC(s,p,n) S(12, void *(*)(void *,void *,size_t))(s,p,n)
#define TRIM(s) S(18, void (*)(const void *))(s)
// Only synchronize at the real native SVC bridge. The runtime's provider and
// owner callbacks are unchanged. A separate thread exercises source-lock release.
enum { RACE_DECOMMIT_BORROW = 1, RACE_COMMIT_BORROW, RACE_COMMIT_BUDGET };
static unsigned race_mode;
static uintptr_t race_source;
static void *race_worker(void *unused) {
    (void)unused;
    if (race_mode == RACE_COMMIT_BUDGET) assert(!call(0x2c, race_source, PAGE, 0));
    else assert(!call(4, info(I_StackAddr), race_source, PAGE));
    return NULL;
}
static void run_race(uintptr_t fallback) {
    if (!race_source) race_source = fallback;
    pthread_t thread; assert(!pthread_create(&thread, NULL, race_worker, NULL));
    assert(!pthread_join(thread, NULL)); race_mode = 0;
}
#endif
uint32_t dm_vamm_svc_change_pages(uintptr_t base, size_t size, int commit) {
#if defined(DM_SA_ADAPTER_TESTING) && defined(DM_VAMM_ADAPTER_TESTING)
    if ((!commit && race_mode == RACE_DECOMMIT_BORROW) ||
        (commit && race_mode == RACE_COMMIT_BUDGET)) run_race(base);
#endif
    uint32_t result = reclaim_native_svc_change_pages(base, size, commit);
#if defined(DM_SA_ADAPTER_TESTING) && defined(DM_VAMM_ADAPTER_TESTING)
    if (!result && commit && race_mode == RACE_COMMIT_BORROW) run_race(base);
#endif
    return result;
}

#if defined(DM_SA_ADAPTER_TESTING) && defined(DM_VAMM_ADAPTER_TESTING)
static dm_sa_stats snapshot;
static void record_dump(void *context, const void *self, const dm_sa_stats *stats) {
    assert(context && self && stats); snapshot = *stats;
}
static dm_sa_stats read_stats(const void *self) {
    S(20, void (*)(const void *))(self); return snapshot;
}
static void default_capability_absent(void) {
    const dm_memory_runtime_contract contract = {
        DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA, DM_VAMM_PLACEMENT_MODERN};
    for (unsigned explicit_contract = 0; explicit_contract < 2; ++explicit_contract) {
        dm_memory_runtime *r = NULL;
        assert((explicit_contract ? dm_memory_runtime_create_with_contract(&r, 32, &contract) :
            dm_memory_runtime_create(&r, 32)) == DM_SA_OK);
        dm_vamm_adapter_config v; dm_sa_adapter_config a;
        assert(dm_memory_runtime_configs(r, &v, &a) == DM_SA_OK);
        assert(!v.retain_owner(v.context, v.shared_owner));
        assert(a.retain_context(a.context) == DM_SA_OK);
        dm_vamm_initialize(v.shared_owner);
        unsigned char client[DM_SA_ADAPTER_OBJECT_SIZE] = {0}; dm_sa_config c;
        if (dm_vamm_is_enabled(v.shared_owner)) {
            assert(a.make_config(a.context, client, SMALL, 0, &c) == DM_SA_OK);
            assert(!c.provider.pages.granule && !c.provider.pages.query_run &&
                !c.provider.pages.budget && !c.provider.pages.change);
            a.release_config(a.context, client, &c);
        }
        a.release_context(a.context); v.release_owner(v.context, v.shared_owner);
        assert(dm_memory_runtime_destroy(r) == DM_SA_OK && !alias_mapped());
    }
}
static void full_cycle(void *self, dm_vamm *owner) {
    void *p = ALLOC(self, SMALL); assert(p); memset(p, 0x71, SMALL);
    dm_vamm_stats before, after;
    assert(!dm_vamm_get_stats(owner, SMALL, &before) && before.regions == 1);
    assert(SHRINK(self, p, 32) == 32);
    dm_sa_stats s = read_stats(self);
    assert(s.backing_bytes == SMALL && s.committed_bytes == PAGE && s.allocated_bytes == 32);
    assert(alias_mapped() == PAGE && !dm_vamm_get_stats(owner, SMALL, &after));
    assert(after.regions == before.regions && after.reserved_bytes == before.reserved_bytes);
    for (size_t i = 0; i < 32; ++i) assert(((unsigned char *)p)[i] == 0x71);
    void *q = S(10, void *(*)(void *,size_t,size_t))(self, 2 * PAGE + 16, 16);
    assert(q == (unsigned char *)p + 32 && alias_mapped() == 3 * PAGE);
    for (size_t i = PAGE; i < 2 * PAGE + 48; ++i) assert(!((unsigned char *)p)[i]);
    FREE(self, q); assert(alias_mapped() == 3 * PAGE); // no eager partial Free
    TRIM(self); assert(alias_mapped() == PAGE);
    assert(REALLOC(self, p, 3 * PAGE) == p && alias_mapped() == 3 * PAGE);
    memset(p, 0x62, 3 * PAGE);
    assert(REALLOC(self, p, 48) == p && alias_mapped() == PAGE);
    for (size_t i = 0; i < 48; ++i) assert(((unsigned char *)p)[i] == 0x62);

    uintptr_t stack = info(I_StackAddr);
    assert(!call(4, stack, (uintptr_t)p, PAGE));
    assert(!SHRINK(self, p, 16)); // old allocation still borrowed: no publication
    expect_finalize(self, DM_SA_BUSY);
    assert(read_stats(self).allocated_bytes == 48 && alias_mapped() == PAGE);
    assert(!call(5, stack, (uintptr_t)p, PAGE));
    FREE(self, p); assert(!alias_mapped());
    assert(!dm_vamm_get_stats(owner, SMALL, &after) && !after.regions);
}
static size_t physical_budget(void) {
    dm_sa_svc_scope_begin(); size_t budget = dm_sa_svc_physical_budget_locked();
    dm_sa_svc_scope_end(); return budget;
}
static void fatal_backend_failures(void *self) {
    void *p = ALLOC(self, SMALL); assert(p); memset(p, 0x28, SMALL);
    EXPECT_MEMORY_PROTECTION_FAILURE(SHRINK(self, p, 32), p, SMALL);
    assert(read_stats(self).allocated_bytes == SMALL && alias_mapped() == SMALL);
    assert(SHRINK(self, p, 32) == 32 && alias_mapped() == PAGE);
    EXPECT_MEMORY_PROTECTION_FAILURE(REALLOC(self, p, 8 * PAGE), p, 32);
    EXPECT_MEMORY_PROTECTION_FAILURE((FINISH(self), 0), p, 32);
    assert(read_stats(self).allocated_bytes == 32 && alias_mapped() == PAGE);
    FREE(self, p); assert(!alias_mapped());
}
static void budget_cycle(void *self) {
    void *p = ALLOC(self, SMALL); assert(p); memset(p, 0x51, SMALL);
    assert(SHRINK(self, p, 32) == 32);
    // Leave enough reservation space for the rival's SDK-tier alignment on
    // every ASLR base. Physical headroom alone is not contiguous lease space.
    assert(!call(1, 0, 2 * LARGE, 0));
    size_t rival_size = physical_budget() - 2 * PAGE;
    uintptr_t rival, spare;
    assert(!V(2, uint32_t (*)(uintptr_t *,size_t))(&rival, rival_size));
    assert(!V(1, uint32_t (*)(uintptr_t *,size_t))(&spare, PAGE));
    assert(physical_budget() == 2 * PAGE);
    ((unsigned char *)rival)[0] = 0x43; ((unsigned char *)rival)[rival_size - 1] = 0x43;
    // Existing boundary backing is not charged again. Only two missing pages
    // fit, even though the resulting allocation spans three pages.
    assert(REALLOC(self, p, 3 * PAGE) == p && !physical_budget());
    for (size_t i = 0; i < 32; ++i) assert(((unsigned char *)p)[i] == 0x51);
    for (size_t i = PAGE; i < 3 * PAGE; ++i) assert(!((unsigned char *)p)[i]);
    assert(!REALLOC(self, p, 4 * PAGE) && read_stats(self).allocated_bytes == 3 * PAGE);
    assert(SHRINK(self, p, 32) == 32 && physical_budget() == 2 * PAGE);
    // Consume one page after the provider snapshot, before native commitment.
    // Its strict scope must refuse two missing pages, even in unlimited mode.
    race_source = spare; race_mode = RACE_COMMIT_BUDGET;
    assert(!REALLOC(self, p, 3 * PAGE) && !race_mode && physical_budget() == PAGE);
    assert(read_stats(self).allocated_bytes == 32 && read_stats(self).committed_bytes == PAGE);
    assert(!V(4, uint32_t (*)(uintptr_t))(spare));
    assert(REALLOC(self, p, 3 * PAGE) == p && !physical_budget());
    assert(((unsigned char *)rival)[0] == 0x43 && ((unsigned char *)rival)[rival_size - 1] == 0x43);
    FREE(self, p); assert(!V(4, uint32_t (*)(uintptr_t))(rival) && !alias_mapped());
    assert(!call(1, 0, 0, 0));
}
static void borrow_races(void *self, dm_memory_runtime *runtime) {
    uintptr_t stack = info(I_StackAddr);
    void *p = ALLOC(self, SMALL); assert(p); memset(p, 0x39, SMALL);
    race_source = 0; race_mode = RACE_DECOMMIT_BORROW;
    assert(SHRINK(self, p, 32) == 32 && !race_mode);
    assert(race_source == (uintptr_t)p + PAGE && alias_mapped() == SMALL);
    assert(read_stats(self).allocated_bytes == 32);
    TRIM(self); assert(alias_mapped() == 2 * PAGE);
    for (size_t i = 0; i < PAGE; ++i) assert(((unsigned char *)stack)[i] == 0x39);
    assert(!call(5, stack, race_source, PAGE));
    TRIM(self); assert(alias_mapped() == PAGE);

    // Recommit succeeded but the old prefix was borrowed before scope restore.
    // The adapter returns NULL, preserving the old size and newly owned pages.
    race_source = (uintptr_t)p; race_mode = RACE_COMMIT_BORROW;
    assert(!REALLOC(self, p, 3 * PAGE) && !race_mode && alias_mapped() == 3 * PAGE);
    assert(read_stats(self).allocated_bytes == 32 && read_stats(self).committed_bytes == 3 * PAGE);
    assert(!call(5, stack, (uintptr_t)p, PAGE));
    for (size_t i = 0; i < 32; ++i) assert(((unsigned char *)p)[i] == 0x39);
    assert(REALLOC(self, p, 3 * PAGE) == p);
    assert(SHRINK(self, p, 32) == 32 && alias_mapped() == PAGE);

    // Borrow during whole-lease free: logical Free succeeds, but the exact
    // sparse reservation and runtime remain retained until retry is safe.
    race_source = (uintptr_t)p; race_mode = RACE_DECOMMIT_BORROW;
    FREE(self, p); assert(!race_mode && alias_mapped() == PAGE);
    assert(!read_stats(self).allocated_bytes && read_stats(self).backing_bytes == SMALL);
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_BUSY);
    expect_finalize(self, DM_SA_BUSY);
    assert(!call(5, stack, (uintptr_t)p, PAGE));
    FINISH(self); assert(!alias_mapped()); INIT(self);
}
#endif

int main(void) {
    const dm_memory_runtime_contract contract = {
        DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA, DM_VAMM_PLACEMENT_MODERN};
    dm_memory_runtime *runtime = (void *)1;
    assert(dm_memory_runtime_create_page_capable_with_contract(&runtime, 0, &contract) == DM_SA_INVALID && runtime == (void *)1);
    assert(dm_memory_runtime_create_page_capable_with_contract(&runtime, 32, NULL) == DM_SA_INVALID && runtime == (void *)1);
    assert(dm_memory_runtime_create_page_capable_with_contract(&runtime, 32, &contract) == DM_SA_OK);
    dm_vamm_adapter_config v; dm_sa_adapter_config a;
    assert(dm_memory_runtime_configs(runtime, &v, &a) == DM_SA_OK);
    dm_vamm_symbol vs[DM_VAMM_SYMBOL_COUNT]; dm_sa_adapter_symbol as[DM_SA_ADAPTER_SYMBOL_COUNT];
    census(&v, &a, vs, as);
#if !defined(DM_SA_ADAPTER_TESTING) || !defined(DM_VAMM_ADAPTER_TESTING)
    assert(dm_vamm_adapter_install(&v) == DM_VAMM_INVALID);
    assert(dm_sa_adapter_install(&a) == DM_SA_INVALID);
    assert(!dm_vamm_adapter_lookup(vs[0].name) && !dm_sa_adapter_lookup(as[0].name));
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_OK);
    puts("page-capable construction grants no production activation: PASS");
#else
    default_capability_absent(); owner_only_runtime_tests();
    a.dump = record_dump;
    assert(!dm_vamm_adapter_install(&v) && dm_sa_adapter_install(&a) == DM_SA_OK);
    unsigned char manager[64] = {0}; V(8, void (*)(void *))(manager);
    V(0, void (*)(void))();
    unsigned char self[DM_SA_ADAPTER_OBJECT_SIZE] = {0}; CONSTRUCT(self);
    if (info(16)) {
        unsigned char probe[DM_SA_ADAPTER_OBJECT_SIZE] = {0}; dm_sa_config c;
        assert(a.make_config(a.context, probe, SMALL, 0, &c) == DM_SA_OK);
        assert(c.provider.pages.granule == PAGE && c.provider.pages.query_run &&
            c.provider.pages.budget && c.provider.pages.change && !alias_mapped());
        a.release_config(a.context, probe, &c);
        INIT(self); full_cycle(self, v.shared_owner); fatal_backend_failures(self); budget_cycle(self);
        borrow_races(self, runtime); FINISH(self);
    } else {
        expect_initialize(self, DM_SA_INVALID); assert(!dm_sa_adapter_is_managed(self));
    }
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_BUSY);
    assert(dm_sa_adapter_shutdown() == DM_SA_OK);
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_BUSY);
    assert(!dm_vamm_adapter_shutdown() && dm_memory_runtime_destroy(runtime) == DM_SA_OK);
    assert(!alias_mapped() && !g_nmap && !atomic_load(&forwards));
    free(g_alias_bits); free(g_alias_exposed); assert(!munmap(g_as, RESERVE));
    puts("explicit page-capable runtime: full native reclamation/recommit cycle: PASS");
#endif
    return 0;
}
