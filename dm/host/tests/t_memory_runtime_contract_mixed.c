// Contract-selected runtime, real owner/provider/adapters and real SVC bitmap.
// Guest constructors and provided-buffer allocator callbacks are synthetic.
#define dm_vamm_svc_info contract_real_svc_info
#define main unused_default_runtime_main
#include "t_memory_runtime.c"
#undef main
#undef dm_vamm_svc_info

static unsigned native_queries[32], child_initializes, child_finalizes;
static int raw_domain;
static void *child_buffer;
uint64_t dm_vamm_svc_info(unsigned id) {
    assert(id < 32); ++native_queries[id];
    if (id == 28) { assert(!raw_domain); return LARGE; }
    return contract_real_svc_info(id);
}
static void child_initialize(void *self, void *buffer, size_t size) {
    assert(self && buffer == child_buffer && size == SMALL);
    assert(!((unsigned char *)self)[0]); ((unsigned char *)self)[0] = 1;
    ++child_initializes;
}
static void child_finalize(void *self) {
    assert(self && ((unsigned char *)self)[0]); ((unsigned char *)self)[0] = 0;
    ++child_finalizes;
}
static size_t sdk_alignment(size_t size, dm_vamm_placement placement) {
    if (size < LARGE) return SMALL;
    if (placement == DM_VAMM_PLACEMENT_MODERN || size < 2 * LARGE) return LARGE;
    if (size < 16 * LARGE) return 2 * LARGE;
    return 16 * LARGE;
}
static void mixed(dm_vamm_native_domain_policy domain, dm_vamm_placement placement) {
    raw_domain = domain == DM_VAMM_DOMAIN_RAW_ALIAS;
    dm_memory_runtime_contract contract = {domain, placement};
    dm_memory_runtime *runtime = NULL;
    assert(dm_memory_runtime_create_with_contract(&runtime, 128, &contract) == DM_SA_OK);
    assert(native_queries[2] == 1 && native_queries[3] == 1 && native_queries[28] == !raw_domain);
    dm_vamm_adapter_config v; dm_sa_adapter_config a;
    assert(dm_memory_runtime_configs(runtime, &v, &a) == DM_SA_OK);
    assert(!v.proofs && !a.proofs && !v.experimental_startup && !a.experimental_startup);
    dm_vamm_symbol vs[DM_VAMM_SYMBOL_COUNT]; dm_sa_adapter_symbol as[DM_SA_ADAPTER_SYMBOL_COUNT];
    census(&v, &a, vs, as);
    as[6].function = (dm_sa_guest_fn)child_initialize;
    as[8].function = (dm_sa_guest_fn)child_finalize;
    assert(!dm_vamm_adapter_install(&v) && dm_sa_adapter_install(&a) == DM_SA_OK);
    unsigned char manager[64] = {0}, self[DM_SA_ADAPTER_OBJECT_SIZE] = {0};
    V(8, void (*)(void *))(manager); V(0, void (*)(void))(); CONSTRUCT(self);
    // Large nominal capacities do not reserve/commit an arena in full mode.
    S(7, void (*)(void *, void *, size_t, unsigned))(self, NULL, (size_t)40 << 30, 0);
    assert(!alias_mapped() && self[0] && !self[1]);
    assert(TOTAL(self) < ((size_t)40 << 30));
    dm_vamm_stats state;
    assert(!dm_vamm_get_stats(v.shared_owner, PAGE, &state) && !state.regions);

    // Reserve an independent sparse range before acquiring a managed lease.
    const size_t sparse_size = 4 * LARGE;
    uintptr_t sparse = 0;
    assert(!V(1, uint32_t (*)(uintptr_t *, size_t))(&sparse, sparse_size));
    size_t alignment = sdk_alignment(sparse_size, placement);
    assert(sparse == ((info(2) + PAGE + alignment - 1) & ~(uintptr_t)(alignment - 1)));
    assert(!V(3, uint32_t (*)(uintptr_t, size_t))(sparse, SMALL));
    assert(!V(3, uint32_t (*)(uintptr_t, size_t))(sparse + sparse_size - SMALL, SMALL));
    memset((void *)sparse, 0x31, SMALL);
    memset((void *)(sparse + sparse_size - SMALL), 0x32, SMALL);
    assert(alias_mapped() == 2 * SMALL);
    query_state(sparse + LARGE, 0, 0, 0); // reservation is not committed backing
    void *managed = S(10, aligned_allocate_fn)(self, 2 * LARGE, SMALL);
    assert(managed && !((uintptr_t)managed & (SMALL - 1)));
    assert((uintptr_t)managed + 2 * LARGE <= sparse || sparse + sparse_size <= (uintptr_t)managed);
    memset(managed, 0x71, 2 * LARGE);
    assert(alias_mapped() == 2 * LARGE + 2 * SMALL);
    assert(!dm_vamm_get_stats(v.shared_owner, PAGE, &state) && state.regions == 2);
    assert(state.reserved_bytes == sparse_size + 2 * LARGE);

    // A provided-buffer child remains original-owned inside the managed lease.
    unsigned char child[DM_SA_ADAPTER_OBJECT_SIZE] = {0}; child_buffer = managed;
    S(6, void (*)(void *, void *, size_t))(child, managed, SMALL);
    assert(!dm_sa_adapter_is_managed(child) && child_initializes == 1 && child[0]);
    S(8, void (*)(void *))(child);
    assert(child_finalizes == 1 && !child[0] && ((unsigned char *)managed)[0] == 0x71);

    // Direct sparse commits reduce the same physical budget used by managed
    // allocation. Grow the heap in its required large-page units, leaving less
    // than one large page of real budget rather than the nominal capacity.
    size_t committed = alias_mapped();
    size_t heap = (APP_POOL - DM_GAME_MEM_USED - committed) & ~(size_t)(LARGE - 1);
    size_t budget = APP_POOL - DM_GAME_MEM_USED - heap - committed;
    assert(!call(1, 0, heap, 0));
    assert(TOTAL(self) <= budget && budget < LARGE);
    assert(!ALLOC(self, 2 * LARGE) && alias_mapped() == committed);
    uintptr_t unchanged = 123;
    assert(V(1, uint32_t (*)(uintptr_t *, size_t))(&unchanged, (size_t)40 << 30) == DM_VAMM_NO_SPACE);
    assert(unchanged == 123 && ((unsigned char *)managed)[0] == 0x71);

    // Failed whole-range release must retain both sparse ends and ownership.
    uintptr_t stack = info(I_StackAddr);
    assert(!call(4, stack, sparse, PAGE));
    assert(*(unsigned char *)stack == 0x31);
    assert(V(4, uint32_t (*)(uintptr_t))(sparse) == 0x803);
    assert(!dm_vamm_get_stats(v.shared_owner, PAGE, &state) && state.regions == 2);
    assert(*(unsigned char *)(sparse + sparse_size - SMALL) == 0x32 &&
           ((unsigned char *)managed)[0] == 0x71);
    assert(!call(5, stack, sparse, PAGE));
    assert(!V(4, uint32_t (*)(uintptr_t))(sparse) && alias_mapped() == 2 * LARGE);
    assert(TOTAL(self) <= budget + 2 * SMALL);
    FREE(self, managed); FINISH(self);
    assert(!alias_mapped() && !call(1, 0, 0, 0));
    assert(dm_sa_adapter_shutdown() == DM_SA_OK && !dm_vamm_adapter_shutdown());
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_OK);
    free(g_alias_bits); free(g_alias_exposed); assert(!munmap(g_as, RESERVE));
}
int main(void) {
    struct rlimit limit = {0, 0}; assert(!setrlimit(RLIMIT_CORE, &limit)); alarm(60);
    // Each publication is process-lifetime; isolated children never reset or
    // reinstall the global adapters merely to test another construction policy.
    for (unsigned domain = 0; domain < 2; ++domain)
        for (unsigned placement = 0; placement < 3; ++placement) {
            pid_t pid = fork(); assert(pid >= 0);
            if (!pid) { mixed((dm_vamm_native_domain_policy)domain, (dm_vamm_placement)placement); _exit(0); }
            int status; assert(waitpid(pid, &status, 0) == pid);
            assert(WIFEXITED(status) && !WEXITSTATUS(status));
        }
    puts("contract runtime: six domain/placement combinations, lazy managed+ sparse raw budgets, child forwarding, failed-free retention: PASS");
    return 0;
}
