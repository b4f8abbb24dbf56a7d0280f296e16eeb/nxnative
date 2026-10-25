// Real production SDK-only authorization -> actual 18-hook transaction -> one
// native owner/SVC. The loader resolver and original SDK calls are synthetic;
// no proprietary code, private offsets or DM_*_TESTING activation flags occur.
#define MEMORY_STARTUP_MAIN unused_sdk_startup_fixture_main
#include "t_memory_runtime_startup.c"
#undef MEMORY_STARTUP_MAIN
#include "../hle_memory_experiment.h"
#if defined(DM_SA_ADAPTER_TESTING) || defined(DM_VAMM_ADAPTER_TESTING) || defined(DM_MEMORY_HOOKS_TESTING)
#error "SDK experiment fixture must exercise production authorization"
#endif
#undef S
#undef V
#define S(n, type) ((type)(uintptr_t)startup_images[0]->slots[n])
#define V(n, type) ((type)(uintptr_t)startup_images[0]->slots[DM_SA_ADAPTER_SYMBOL_COUNT + (n)])

static unsigned sdk_creates, full_creates, sdk_configs, full_configs, destroys;
static unsigned sdk_installs, full_installs, allocator_retains, original_allocations, original_frees;
static int fail_create, fail_config, fail_resolution = -1;
static dm_sa_result track_sdk_create(dm_memory_runtime **out, size_t count, const dm_memory_runtime_contract *contract) {
    assert(contract && contract->domain == DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA &&
           contract->placement == DM_VAMM_PLACEMENT_MODERN && count == 65536);
    ++sdk_creates;
    if (fail_create) return DM_SA_NO_MEMORY;
    dm_sa_result r = dm_memory_runtime_create_vamm_only_with_contract(out, count, contract);
    if (r == DM_SA_OK) startup_runtime = *out;
    return r;
}
static dm_sa_result track_full_create(dm_memory_runtime **out, size_t count, const dm_memory_runtime_contract *contract) {
    assert(contract && contract->domain == DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA &&
           contract->placement == DM_VAMM_PLACEMENT_MODERN && count == 65536);
    ++full_creates;
    dm_sa_result r = dm_memory_runtime_create_with_contract(out, count, contract);
    if (r == DM_SA_OK) startup_runtime = *out;
    return r;
}
static dm_sa_result track_sdk_config(dm_memory_runtime *r, dm_vamm_adapter_config *v) {
    ++sdk_configs;
    if (fail_config) return DM_SA_INVALID;
    return dm_memory_runtime_vamm_config(r, v);
}
static dm_sa_result track_full_configs(dm_memory_runtime *r, dm_vamm_adapter_config *v, dm_sa_adapter_config *a) {
    ++full_configs; return dm_memory_runtime_configs(r, v, a);
}
static dm_sa_result track_destroy(dm_memory_runtime *r) { ++destroys; return dm_memory_runtime_destroy(r); }
static dm_sa_result unused_factory(void *context, void *self, size_t size, unsigned cache, dm_sa_config *out) {
    (void)context; (void)self; (void)size; (void)cache; (void)out;
    assert(!"SDK-only permit reached an SA factory"); return DM_SA_INVALID;
}
static dm_sa_result unused_retain(void *context) {
    assert(context); ++allocator_retains; return DM_SA_OK;
}
static void unused_release(void *context) { assert(context && allocator_retains); --allocator_retains; }
static void preserved_slots(void) {
    for (unsigned k = 0; k < 2; ++k) {
        for (unsigned i = 0; i < DM_SA_ADAPTER_SYMBOL_COUNT; ++i) {
            assert(startup_images[k]->slots[i] == original_slots[k][i]);
            assert(!dm_memory_hooks_lookup(startup_name(i)) && !dm_sa_adapter_lookup(startup_name(i)));
        }
        assert(startup_images[k]->slots[STARTUP_SYMBOLS] == original_slots[k][STARTUP_SYMBOLS]);
    }
}
static void no_provider_config(dm_memory_runtime *runtime) {
    dm_vamm_adapter_config v, vb;
    dm_sa_adapter_config a, ab;
    memset(&v, 0xa5, sizeof v); vb = v;
    memset(&a, 0xa5, sizeof a); ab = a;
    assert(dm_memory_runtime_configs(runtime, &v, &a) == DM_SA_INVALID);
    assert(!memcmp(&v, &vb, sizeof v) && !memcmp(&a, &ab, sizeof a));
}
static uint64_t sdk_resolve(module_t *module, const Elf64_Sym *symbol) {
    assert(!dm_memory_hooks_active() && !dm_memory_hooks_enter_guest());
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    assert(!dm_vamm_is_initialized(startup_vamm.shared_owner) && !alias_mapped());
    preserved_slots(); no_provider_config(startup_runtime);
    if (resolver_calls++ == fail_resolution) return 0;
    for (unsigned i = DM_SA_ADAPTER_SYMBOL_COUNT; i < STARTUP_SYMBOLS; ++i)
        if (!strcmp(module->strtab + symbol->st_name, startup_name(i))) return original_target(i);
    assert(!"SDK-only transaction resolved an original SA import"); return 0;
}
int loader_install_vamm_hooks(const dm_vamm_adapter_config *v) {
    ++sdk_installs; startup_vamm = *v;
    assert(!v->proofs && v->experimental_startup && v->shared_owner && v->fatal);
    assert(dm_memory_experiment_authorized_sdk_only(v->experimental_startup, v->context));
    assert(!dm_memory_experiment_authorized(v->experimental_startup, v->context));
    assert(!dm_memory_experiment_authorized_sdk_only(NULL, v->context));
    assert(!dm_memory_experiment_authorized_sdk_only(v->experimental_startup, &sdk_installs));
    assert(!dm_memory_experiment_authorized_sdk_only((const struct dm_memory_experiment_authorization *)v, v->context));
    no_provider_config(startup_runtime);

    // Otherwise complete, independently valid SA table: the SDK permit itself
    // must reject it, before retention, not merely a missing factory/census.
    dm_sa_adapter_symbol symbols[DM_SA_ADAPTER_SYMBOL_COUNT];
    for (unsigned i = 0; i < DM_SA_ADAPTER_SYMBOL_COUNT; ++i)
        symbols[i] = (dm_sa_adapter_symbol){startup_name(i), (dm_sa_guest_fn)original_target(i)};
    dm_sa_adapter_config a = {.context = v->context, .experimental_startup = v->experimental_startup,
        .symbols = symbols, .symbol_count = DM_SA_ADAPTER_SYMBOL_COUNT, .reject_managed_walk = 1,
        .make_config = unused_factory, .forward_allocate = dm_sa_host_forward_allocate,
        .allocation_alignment = dm_sa_host_allocation_alignment,
        .retain_context = unused_retain, .release_context = unused_release};
    dm_sa_adapter_prepared *prepared = (void *)1;
    assert(dm_sa_adapter_prepare(&a, &prepared) == DM_SA_INVALID && !prepared && !allocator_retains);
    assert(dm_memory_hooks_install(g_mod, (size_t)g_nmod, &a, v, sdk_resolve) != DM_MEMORY_HOOKS_OK);
    assert(!allocator_retains && !resolver_calls && !dm_memory_hooks_active());
    preserved_slots();

    // Proof bits cannot convert the native permit into fixture authority.
    dm_vamm_symbol originals[DM_VAMM_SYMBOL_COUNT];
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i)
        originals[i] = (dm_vamm_symbol){dm_vamm_adapter_symbol_name(i),
            (dm_vamm_guest_fn)original_target(DM_SA_ADAPTER_SYMBOL_COUNT + i)};
    dm_vamm_adapter_config complete = *v;
    complete.symbols = originals; complete.symbol_count = DM_VAMM_SYMBOL_COUNT;
    dm_vamm_adapter_config wrong = complete; wrong.proofs = DM_VAMM_PROOF_ALL;
    dm_vamm_adapter_prepared *vp = (void *)1;
    assert(dm_vamm_adapter_prepare(&wrong, &vp) == DM_VAMM_INVALID && !vp);
    wrong = complete; wrong.context = &sdk_installs;
    vp = (void *)1;
    assert(dm_vamm_adapter_prepare(&wrong, &vp) == DM_VAMM_INVALID && !vp);
    return dm_memory_hooks_install_vamm_only(g_mod, (size_t)g_nmod, v, sdk_resolve);
}
int loader_install_memory_hooks(const dm_sa_adapter_config *a, const dm_vamm_adapter_config *v) {
    ++full_installs;
    assert(dm_memory_experiment_authorized(a->experimental_startup, a->context));
    assert(!dm_memory_experiment_authorized_sdk_only(v->experimental_startup, v->context));
    // Full-group authorization may still authorize its Vamm adapter, but cannot
    // authorize a different transaction mode that leaves the SDK allocator live.
    assert(dm_memory_hooks_install_vamm_only(g_mod, (size_t)g_nmod, v, sdk_resolve) == DM_MEMORY_HOOKS_DISABLED);
    assert(!resolver_calls && !dm_memory_hooks_active()); preserved_slots();
    return DM_MEMORY_HOOKS_INVALID;
}
#define dm_memory_runtime_create_vamm_only_with_contract track_sdk_create
#define dm_memory_runtime_vamm_config track_sdk_config
#define dm_memory_runtime_create_with_contract track_full_create
#define dm_memory_runtime_configs track_full_configs
#define dm_memory_runtime_destroy track_destroy
#include "../hle_memory_experiment.c"
#undef dm_memory_runtime_create_vamm_only_with_contract
#undef dm_memory_runtime_vamm_config
#undef dm_memory_runtime_create_with_contract
#undef dm_memory_runtime_configs
#undef dm_memory_runtime_destroy

static void identities(void) {
    static const char *ids[] = {
        "7222e13ecf6adb3249b5dd5c57fa40b102245fce000000000000000000000000",
        "df7a3960c74c3a4803f826c5871532523aa9cf2a000000000000000000000000"};
    for (unsigned k = 0; k < 2; ++k) {
        g_mod[k] = startup_modules[k];
        memcpy(g_mod[k].build_id, ids[k], 32); g_mod[k].build_id[32] = 0;
        strcpy(g_mod[k].full_build_id, ids[k]);
    }
    g_nmod = 2;
}
static void *sdk_original_allocate(void *self, size_t size, size_t alignment) {
    assert(self && alignment == SMALL); ++original_allocations;
    uintptr_t out = 0;
    return V(2, uint32_t (*)(uintptr_t *,size_t))(&out, size) ? NULL : (void *)out;
}
static void sdk_original_free(void *self, void *pointer) {
    assert(self && pointer); ++original_frees;
    assert(!V(4, uint32_t (*)(uintptr_t))((uintptr_t)pointer));
}
static void active_sdk_calls(void) {
    assert(!dm_memory_experiment_authorized_sdk_only(startup_vamm.experimental_startup, startup_runtime));
    assert(!dm_memory_experiment_authorized(startup_vamm.experimental_startup, startup_runtime));
    assert(dm_memory_hooks_enter_guest()); preserved_slots(); no_provider_config(startup_runtime);
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    unsigned char manager[64] = {0}, self[DM_SA_ADAPTER_OBJECT_SIZE] = {0};
    V(8, void (*)(void *))(manager); V(10, void (*)(void *))(manager);
    assert(dm_vamm_is_initialized(startup_vamm.shared_owner));
    CONSTRUCT(self); assert(atomic_load(&constructors) == 2 && !dm_sa_adapter_is_managed(self));
    assert(!ALLOC(self, SMALL) && atomic_load(&forwards) == 1); // unchanged fixture host shim
    // The retained SDK allocator's multi-GiB request is exact and must not be
    // silently capped, split, given an interior pointer or assigned an SA core.
    uintptr_t too_large = 0x1234;
    assert(V(1, uint32_t (*)(uintptr_t *,size_t))(&too_large, (size_t)12 << 30) == DM_VAMM_NO_SPACE);
    assert(too_large == 0x1234 && !alias_mapped());
    dm_vamm_stats stats;
    assert(!dm_vamm_get_stats(startup_vamm.shared_owner, SMALL, &stats) && !stats.regions);
    void *pointer = S(10, void *(*)(void *,size_t,size_t))(self, SMALL, SMALL);
    assert(pointer && alias_mapped() == SMALL && original_allocations == 1);
    memset(pointer, 0x73, SMALL);
    assert(!dm_sa_adapter_is_managed(self) && !dm_sa_adapter_lookup(dm_sa_adapter_symbol_name(10)));
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    assert(dm_vamm_adapter_shutdown() == DM_VAMM_INVALID); // process-lifetime fence
    assert(((unsigned char *)pointer)[SMALL - 1] == 0x73);
    FREE(self, pointer); assert(original_frees == 1 && !alias_mapped());
    assert(!dm_vamm_get_stats(startup_vamm.shared_owner, SMALL, &stats) && !stats.regions);
    assert(dm_vamm_adapter_shutdown() == DM_VAMM_INVALID); // even empty tables retain ownership
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    preserved_slots();
    const char *why = NULL;
    assert(dm_memory_experiment_startup("0", &why) == DM_MEMORY_EXPERIMENT_FAILED && why);
    assert(dm_memory_hooks_active() && !destroys && !allocator_retains);
}
static void scenario(unsigned which) {
    identities();
    if (which != 17) assert(dm_memory_hooks_open_startup());
    const char *request = DM_SDK_MEMORY_EXPERIMENT_NAME, *why = NULL;
    if (which == 0) request = NULL;
    if (which == 1) request = "0";
    if (which == 2) request = "";
    if (which == 3) request = "1";
    if (which == 4) { module_t m = g_mod[0]; g_mod[0] = g_mod[1]; g_mod[1] = m; }
    if (which == 5) g_mod[1].full_build_id[63] = '1';
    if (which == 6) startup_images[1]->symbols[DM_SA_ADAPTER_SYMBOL_COUNT + 1].st_shndx = 0;
    if (which == 7) startup_images[1]->slots[STARTUP_SYMBOLS - 1] ^= 8;
    if (which == 8) startup_images[1]->rela[STARTUP_SYMBOLS - 1].r_info = 999;
    if (which == 9) assert(dm_memory_hooks_enter_guest());
    if (which == 10) fail_create = 1;
    if (which == 11) fail_config = 1;
    if (which == 13) {
        for (unsigned k = 0; k < 2; ++k) for (unsigned i = 0; i < DM_SA_ADAPTER_SYMBOL_COUNT; ++i) {
            startup_images[k]->symbols[i + 1] = (Elf64_Sym){0};
            startup_images[k]->rela[i].r_info = R_AARCH64_RELATIVE;
        }
    }
    if (which == 14 || which == 15) {
        for (unsigned k = 0; k < 2; ++k) {
            strcpy(startup_images[k]->strings + 7000, which == 14 ?
                "_ZN2nn3mem17StandardAllocatorD1Ev" : "_ZN2nn2os6detail11VammManagerD1Ev");
            startup_images[k]->symbols[1].st_name = 7000;
        }
    }
    if (which == 16) request = DM_MEMORY_EXPERIMENT_NAME;
    if (which == 18) assert(!setenv(DM_MEMORY_RECLAIM_ENV, "1", 1));
    if (which >= 100 && which < 100 + 2 * DM_VAMM_SYMBOL_COUNT) fail_resolution = (int)which - 100;
    int result = dm_memory_experiment_startup(request, &why);
    if (which < 3) {
        assert(result == DM_MEMORY_EXPERIMENT_OFF && !why && !sdk_creates && !full_creates);
    } else {
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY
        if (which == 12 || which == 13 || which == 14) {
            assert(result == DM_MEMORY_EXPERIMENT_ACTIVE && !why && sdk_creates == 1 && sdk_configs == 1);
            assert(sdk_installs == 1 && !full_creates && !full_configs && !full_installs && !destroys);
            assert(resolver_calls == 2 * DM_VAMM_SYMBOL_COUNT);
            active_sdk_calls(); return;
        }
        if (which == 18) assert(!sdk_creates && !full_creates && !sdk_configs && !full_configs && !sdk_installs && !full_installs && !destroys);
        else if (which == 16) assert(full_creates == 1 && full_configs == 1 && full_installs == 1 && destroys == 1 && !sdk_creates);
        else if (which == 10) assert(sdk_creates == 1 && !sdk_configs && !sdk_installs && !destroys);
        else if (which == 11) assert(sdk_creates == 1 && sdk_configs == 1 && !sdk_installs && destroys == 1);
        else if (which >= 6) assert(sdk_creates == 1 && sdk_configs == 1 && sdk_installs == 1 && destroys == 1);
        else assert(!sdk_creates && !sdk_installs && !destroys);
#else
        assert(!sdk_creates && !full_creates && !sdk_installs && !full_installs && !destroys);
#endif
        assert(result == DM_MEMORY_EXPERIMENT_FAILED && why && *why);
        if (which == 18) assert(strstr(why, "DM_MEMORY_RECLAIM=1") && strstr(why, "SDK-only"));
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY
        if (fail_resolution >= 0) assert(resolver_calls == fail_resolution + 1);
#endif
    }
    assert(!dm_memory_hooks_active() && !alias_mapped() && !atomic_load(&constructors) && !allocator_retains);
    preserved_slots();
    for (unsigned k = 0; k < 2; ++k) for (unsigned i = DM_SA_ADAPTER_SYMBOL_COUNT; i < STARTUP_SYMBOLS; ++i) {
        uint64_t expected = original_slots[k][i];
        if (which == 7 && k == 1 && i == STARTUP_SYMBOLS - 1) expected ^= 8;
        assert(startup_images[k]->slots[i] == expected);
    }
}
static void run_scenario(unsigned which) {
    int fds[2]; assert(!pipe(fds));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(fds[0]); assert(dup2(fds[1], STDERR_FILENO) == STDERR_FILENO); close(fds[1]);
        scenario(which); _exit(0);
    }
    close(fds[1]); char log[8192]; size_t used = 0;
    for (;;) {
        assert(used < sizeof log - 1);
        ssize_t n = read(fds[0], log + used, sizeof log - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        assert(n >= 0); if (!n) break; used += (size_t)n;
    }
    log[used] = 0; close(fds[0]);
    int status; assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fputs(log, stderr); fprintf(stderr, "SDK experiment scenario %u failed\n", which);
    }
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!strstr(log, "experimental shared memory managed ") &&
           !strstr(log, "experimental shared memory snapshot "));
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY
    if (which == 12 || which == 13 || which == 14)
        assert(strstr(log, "experimental SDK-only memory " DM_SDK_MEMORY_EXPERIMENT_NAME " active:"));
    else
#endif
        assert(!strstr(log, " active:"));
}
int main(void) {
    alarm(60); struct rlimit limit = {0, 0}; assert(!setrlimit(RLIMIT_CORE, &limit));
    assert(!unsetenv(DM_MEMORY_RECLAIM_ENV));
    startup_images_create();
    for (unsigned k = 0; k < 2; ++k) {
        startup_images[k]->slots[10] = (uintptr_t)sdk_original_allocate;
        startup_images[k]->slots[11] = (uintptr_t)sdk_original_free;
        memcpy(original_slots[k], startup_images[k]->slots, sizeof original_slots[k]);
    }
    for (unsigned i = 0; i <= 18; ++i) run_scenario(i);
    assert(!setenv(DM_MEMORY_RECLAIM_ENV, "0", 1)); run_scenario(12);
    for (unsigned i = 0; i < 2 * DM_VAMM_SYMBOL_COUNT; ++i) run_scenario(100 + i);
    for (unsigned k = 0; k < 2; ++k) assert(!munmap(startup_images[k], startup_image_bytes));
    puts("SDK experiment: real authority separation, original SA, exact requests, native Vamm, rejection cleanup, lifetime: PASS");
    return 0;
}
