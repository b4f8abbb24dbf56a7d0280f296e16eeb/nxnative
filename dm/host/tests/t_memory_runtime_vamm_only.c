// Independent SDK-only integration fixture: execute committed Vamm imports over
// the real runtime owner/SVC while StandardAllocator imports stay original.
// Synthetic original allocator calls model dispatch, not a proprietary SDK ABI.
// Deliberately built WITHOUT DM_SA_ADAPTER_TESTING: SA activation cannot help.
#define MEMORY_STARTUP_MAIN unused_full_group_startup_main
#define dm_memory_hooks_test_before_publish unused_full_group_before_publish
#include "t_memory_runtime_startup.c"
#undef dm_memory_hooks_test_before_publish
#undef MEMORY_STARTUP_MAIN
#undef S
#undef V
#define S(n, type) ((type)(uintptr_t)startup_images[0]->slots[n])
#define V(n, type) ((type)(uintptr_t)startup_images[0]->slots[DM_SA_ADAPTER_SYMBOL_COUNT + (n)])

static unsigned original_allocations, original_frees;
static void *original_sdk_allocate(void *self, size_t size, size_t alignment) {
    assert(self && alignment == SMALL); ++original_allocations;
    uintptr_t pointer = 0;
    return V(2, uint32_t (*)(uintptr_t *, size_t))(&pointer, size) ? NULL : (void *)pointer;
}
static void original_sdk_free(void *self, void *pointer) {
    assert(self && pointer); ++original_frees;
    assert(!V(4, uint32_t (*)(uintptr_t))((uintptr_t)pointer));
}
static void original_allocator_slots(void) {
    for (unsigned k = 0; k < 2; ++k) {
        for (unsigned i = 0; i < DM_SA_ADAPTER_SYMBOL_COUNT; ++i) {
            assert(startup_images[k]->slots[i] == original_slots[k][i]);
            assert(!dm_memory_hooks_lookup(startup_name(i)) && !dm_sa_adapter_lookup(startup_name(i)));
        }
        assert(startup_images[k]->slots[STARTUP_SYMBOLS] == original_slots[k][STARTUP_SYMBOLS]);
    }
}
static void only_owner_pinned(void) {
    unpublished(); original_allocator_slots();
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    assert(startup_vamm.retain_owner(startup_runtime, startup_vamm.shared_owner) == DM_VAMM_INVALID);
    // Unlike the full-group path, the allocator table context was never pinned.
    assert(startup_allocator.retain_context(startup_runtime) == DM_SA_OK);
    startup_allocator.release_context(startup_runtime);
    no_config(&startup_allocator);
}
static uint64_t sdk_only_resolve(module_t *module, const Elf64_Sym *symbol) {
    only_owner_pinned(); assert(!dm_memory_hooks_enter_guest());
    int call_index = resolver_calls++;
    if (call_index == resolver_failure) return 0;
    for (unsigned i = DM_SA_ADAPTER_SYMBOL_COUNT; i < STARTUP_SYMBOLS; ++i)
        if (!strcmp(module->strtab + symbol->st_name, startup_name(i))) return original_target(i);
    assert(!"StandardAllocator originals must not be resolved/captured"); return 0;
}
int dm_memory_hooks_test_before_publish(void) {
    ++publications; only_owner_pinned(); assert(!dm_memory_hooks_enter_guest());
    for (unsigned k = 0; k < 2; ++k)
        for (unsigned i = DM_SA_ADAPTER_SYMBOL_COUNT; i < STARTUP_SYMBOLS; ++i)
            assert(startup_images[k]->slots[i] && startup_images[k]->slots[i] != original_slots[k][i]);
    return !reject_publication;
}
static int sdk_only_install(void) {
    return dm_memory_hooks_install_vamm_only(startup_modules, 2, &startup_vamm, sdk_only_resolve);
}
#if defined(DM_MEMORY_HOOKS_TESTING) && defined(DM_VAMM_ADAPTER_TESTING)
static uint64_t sdk_svc(unsigned svc, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t regs[31] = {a, b, c}; svc_dispatch(svc, regs, 0); return regs[0];
}
static uint64_t sdk_info(unsigned id) {
    uint64_t regs[31] = {0, id}; svc_dispatch(0x29, regs, 0); assert(!regs[0]); return regs[1];
}
static void sdk_rejected(void) {
    unsigned calls = atomic_load(&protect_calls);
    assert(sdk_only_install() != DM_MEMORY_HOOKS_OK);
    unchanged(); original_allocator_slots();
    assert(atomic_load(&protect_calls) == calls);
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_OK);
}
static void stopped_vamm_slot(void) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) { V(7, int (*)(void))(); _exit(7); }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
static void ctor2_alias(void) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        unsigned char manager[64] = {0};
        V(9, void (*)(void *))(manager);
        V(0, void (*)(void))();
        assert(atomic_load(&constructors) == 1);
        assert(dm_vamm_is_initialized(startup_vamm.shared_owner) == !!sdk_info(16));
        assert(!dm_vamm_adapter_shutdown());
        assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_OK);
        startup_cleanup(); _exit(0);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
}
#endif
int main(void) {
    struct rlimit limit = {0, 0}; assert(!setrlimit(RLIMIT_CORE, &limit));
    alarm(60); startup_images_create();
    // Callable original allocator imports are part of the ordinary relocated
    // image. These stay byte-for-byte unchanged, as does Allocate(size)'s shim.
    for (unsigned k = 0; k < 2; ++k) {
        startup_images[k]->slots[10] = (uintptr_t)original_sdk_allocate;
        startup_images[k]->slots[11] = (uintptr_t)original_sdk_free;
        memcpy(original_slots[k], startup_images[k]->slots, sizeof original_slots[k]);
    }
    startup_create(); assert(dm_memory_hooks_open_startup());
#if !defined(DM_MEMORY_HOOKS_TESTING) || !defined(DM_VAMM_ADAPTER_TESTING)
    assert(sdk_only_install() == DM_MEMORY_HOOKS_DISABLED);
    unchanged(); original_allocator_slots(); assert(!resolver_calls && !publications);
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_OK);
    startup_cleanup(); puts("Vamm-only runtime: production remains disabled: PASS");
#else
    // Real retained owner must unwind at every resolver failure and after the
    // final staged write. SA preparation is neither enabled nor necessary.
    for (unsigned i = 0; i < 2 * DM_VAMM_SYMBOL_COUNT; ++i) {
        resolver_failure = (int)i; sdk_rejected();
        assert(resolver_calls == (int)i + 1 && !publications); startup_create();
    }
    reject_publication = 1; sdk_rejected(); assert(publications == 1);
    startup_create(); unsigned calls = atomic_load(&protect_calls);
    assert(sdk_only_install() == DM_MEMORY_HOOKS_OK && publications == 1);
    assert(resolver_calls == 2 * DM_VAMM_SYMBOL_COUNT && dm_memory_hooks_active() == 1);
    assert(atomic_load(&protect_calls) == calls && !atomic_load(&constructors));
    original_allocator_slots(); no_config(&startup_allocator);
    assert(dm_memory_hooks_enter_guest() && sdk_only_install() == DM_MEMORY_HOOKS_BUSY);
    ctor2_alias();
    unsigned char manager[64] = {0}, self[DM_SA_ADAPTER_OBJECT_SIZE] = {0};
    V(8, void (*)(void *))(manager); V(10, void (*)(void *))(manager);
    assert(atomic_load(&constructors) == 1);
    assert(V(7, int (*)(void))() == !!sdk_info(16));
    assert(V(11, int (*)(const void *))(manager) == !!sdk_info(16));
    dm_vamm_resource_usage global = V(6, dm_vamm_resource_usage (*)(void))();
    dm_vamm_resource_usage detail = V(17, dm_vamm_resource_usage (*)(void *))(manager);
    assert(global.total == detail.total && global.used == detail.used);
    CONSTRUCT(self); assert(atomic_load(&constructors) == 2 && !dm_sa_adapter_is_managed(self));
    assert(!ALLOC(self, SMALL) && atomic_load(&forwards) == 1); // unchanged forced shim
    no_config(&startup_allocator); // Vamm init does NOT enable an SA factory/table
    void *pointer = S(10, void *(*)(void *,size_t,size_t))(self, SMALL, SMALL);
    assert(original_allocations == 1 && !dm_sa_adapter_is_managed(self));
    if (!sdk_info(16)) {
        assert(!pointer && !alias_mapped());
    } else {
        assert(pointer && !((uintptr_t)pointer & (SMALL - 1)) && alias_mapped() == SMALL);
        memset(pointer, 0x71, SMALL);
        dm_vamm_stats stats;
        assert(!dm_vamm_get_stats(startup_vamm.shared_owner, SMALL, &stats) && stats.regions == 1);
        uintptr_t reserved = 0;
        assert(!V(12, uint32_t (*)(void *,uintptr_t *,size_t))(manager, &reserved, SMALL));
        assert(alias_mapped() == SMALL);
        assert(!V(14, uint32_t (*)(void *,uintptr_t,size_t))(manager, reserved, SMALL));
        memset((void *)reserved, 0x42, SMALL); assert(alias_mapped() == 2 * SMALL);
        assert(!V(16, uint32_t (*)(void *,uintptr_t,size_t))(manager, reserved, SMALL));
        assert(!V(15, uint32_t (*)(void *,uintptr_t))(manager, reserved));
        assert(!V(1, uint32_t (*)(uintptr_t *,size_t))(&reserved, SMALL));
        assert(!V(3, uint32_t (*)(uintptr_t,size_t))(reserved, SMALL));
        assert(!V(5, uint32_t (*)(uintptr_t,size_t))(reserved, SMALL));
        assert(!V(4, uint32_t (*)(uintptr_t))(reserved));
        assert(!V(13, uint32_t (*)(void *,uintptr_t *,size_t))(manager, &reserved, SMALL));
        assert(!V(15, uint32_t (*)(void *,uintptr_t))(manager, reserved));
        assert(alias_mapped() == SMALL && *(unsigned char *)pointer == 0x71);
        // A borrowed original-SDK allocation stays owned on failed decommit.
        uintptr_t stack = sdk_info(I_StackAddr);
        assert(!sdk_svc(4, stack, (uintptr_t)pointer, PAGE));
        *(unsigned char *)stack = 0x36;
        assert(V(4, uint32_t (*)(uintptr_t))((uintptr_t)pointer) == 0x803);
        assert(alias_mapped() == SMALL && dm_vamm_adapter_shutdown() == DM_VAMM_INVALID);
        assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
        assert(!sdk_svc(5, stack, (uintptr_t)pointer, PAGE) && *(unsigned char *)pointer == 0x36);
        FREE(self, pointer); assert(original_frees == 1 && !alias_mapped());
        assert(!dm_vamm_get_stats(startup_vamm.shared_owner, SMALL, &stats) && !stats.regions);
    }
    original_allocator_slots();
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    assert(!dm_vamm_adapter_shutdown()); // no SA shutdown needed or active table
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_OK);
    assert(dm_memory_hooks_active() == 1); original_allocator_slots();
    stopped_vamm_slot(); startup_cleanup();
    puts("Vamm-only runtime: committed wrappers/native owner/original SA/lifetime/rollback: PASS");
#endif
    return 0;
}
