#include "hle_memory_experiment.h"
#include "hle_memory_hooks.h"
#include "hle_memory_runtime.h"

// Compile-time admission is separate from construction policy. A new descriptor
// requires review of the COMPLETE ordered module closure; a compatible SDK or a
// valid raw/legacy construction contract alone never grants startup authority.
enum { FULL_MEMORY, SDK_MEMORY };
enum { STARTUP_MAX_REGIONS = 65536 };
typedef struct {
    char name[sizeof(((module_t *)0)->name)];
    char full_build_id[65];
} startup_module_contract;
typedef struct {
    char request[64];
    const startup_module_contract *modules;
    size_t module_count, max_regions;
    unsigned mode;
    dm_memory_runtime_contract runtime;
} startup_contract;
static const startup_module_contract za_modules[] = {
    {"main", "7222e13ecf6adb3249b5dd5c57fa40b102245fce000000000000000000000000"},
    {"sdk", "df7a3960c74c3a4803f826c5871532523aa9cf2a000000000000000000000000"}
};
// No SDK15 or other new admission. Both existing request names retain exactly
// their previous ownership mode, domain, placement and metadata bound.
static const startup_contract admitted_contracts[] = {
    {DM_MEMORY_EXPERIMENT_NAME, za_modules, 2, 65536, FULL_MEMORY,
        {DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA, DM_VAMM_PLACEMENT_MODERN}},
    {DM_SDK_MEMORY_EXPERIMENT_NAME, za_modules, 2, 65536, SDK_MEMORY,
        {DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA, DM_VAMM_PLACEMENT_MODERN}}
};
static const char *validate_contract(const startup_contract *c) {
    if (!c || !memchr(c->request, 0, sizeof c->request) || !c->request[0] ||
        !strcmp(c->request, "0") || !c->modules || !c->module_count ||
        c->module_count > MAX_MOD || !c->max_regions || c->max_regions > STARTUP_MAX_REGIONS ||
        (c->mode != FULL_MEMORY && c->mode != SDK_MEMORY))
        return "invalid memory startup descriptor";
    if ((c->runtime.domain != DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA &&
         c->runtime.domain != DM_VAMM_DOMAIN_RAW_ALIAS) ||
        (c->runtime.placement != DM_VAMM_PLACEMENT_MODERN &&
         c->runtime.placement != DM_VAMM_PLACEMENT_LEGACY &&
         c->runtime.placement != DM_VAMM_PLACEMENT_LEGACY_REDUCED))
        return "invalid memory startup construction contract";
    for (size_t k = 0; k < c->module_count; ++k) {
        const startup_module_contract *m = &c->modules[k];
        if (!memchr(m->name, 0, sizeof m->name) || !m->name[0] || m->full_build_id[64])
            return "invalid memory startup module identity";
        for (unsigned i = 0; i < 64; ++i)
            if (!((m->full_build_id[i] >= '0' && m->full_build_id[i] <= '9') ||
                  (m->full_build_id[i] >= 'a' && m->full_build_id[i] <= 'f')))
                return "invalid memory startup full NSO build ID";
        for (size_t j = 0; j < k; ++j)
            if (!strcmp(m->name, c->modules[j].name)) return "duplicate memory startup module name";
    }
    return NULL;
}
static const startup_contract *select_contract(const char *request) {
    if (!request) return NULL;
    const startup_contract *selected = NULL;
    for (size_t k = 0; k < sizeof admitted_contracts / sizeof *admitted_contracts; ++k) {
        const startup_contract *c = &admitted_contracts[k];
        // Malformed or duplicate registry entries fail closed, never first-win.
        if (validate_contract(c)) return NULL;
        if (!strcmp(request, c->request)) {
            if (selected) return NULL;
            selected = c;
        }
    }
    return selected;
}
static const char *preflight_contract(const startup_contract *c,
                                      const module_t *modules, size_t count) {
    const char *why = validate_contract(c);
    if (why) return why;
    if (!modules || count != c->module_count)
        return "requires exactly the reviewed complete module set (no extra or deferred modules)";
    for (size_t k = 0; k < count; ++k) {
        const module_t *m = &modules[k];
        if (!memchr(m->name, 0, sizeof m->name)) return "unterminated module name";
        if (m->nro || !m->loaded || m->loads) return "deferred, unloaded or previously loaded modules are unsupported";
        if (!m->base || !m->size) return "module is not mapped";
        // Global ELF lookup is order-sensitive (including main interposition).
        if (strcmp(m->name, c->modules[k].name)) return "requires reviewed module order (Z-A: main then sdk)";
        const char *expected = c->modules[k].full_build_id;
        if (memcmp(m->full_build_id, expected, 65)) return "missing or unreviewed full NSO build ID (rewrite old outputs)";
        if (memcmp(m->build_id, expected, 32) || m->build_id[32]) return "inconsistent short/full NSO build ID";
    }
    return NULL;
}
const char *dm_memory_experiment_preflight(const module_t *modules, size_t count) {
    // Preserve the existing public eligibility query for the reviewed Z-A set.
    return preflight_contract(select_contract(DM_MEMORY_EXPERIMENT_NAME), modules, count);
}

struct dm_memory_experiment_authorization { dm_memory_runtime *runtime; int installing, mode; };
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY && defined(DM_MEMORY_RUNTIME_ALIGNED_LEASES)
static struct dm_memory_experiment_authorization authorization;
#endif
static int startup_seen;
int dm_memory_experiment_authorized(const struct dm_memory_experiment_authorization *a,
                                    const void *context) {
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY && defined(DM_MEMORY_RUNTIME_ALIGNED_LEASES)
    return a == &authorization && authorization.installing && authorization.mode == FULL_MEMORY &&
        context && context == authorization.runtime;
#else
    (void)a; (void)context; return 0;
#endif
}
int dm_memory_experiment_authorized_sdk_only(const struct dm_memory_experiment_authorization *a,
                                             const void *context) {
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY && defined(DM_MEMORY_RUNTIME_ALIGNED_LEASES)
    return a == &authorization && authorization.installing && authorization.mode == SDK_MEMORY &&
        context && context == authorization.runtime;
#else
    (void)a; (void)context; return 0;
#endif
}

#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY && defined(DM_MEMORY_RUNTIME_ALIGNED_LEASES)
#if defined(DM_SA_ADAPTER_TESTING) || defined(DM_VAMM_ADAPTER_TESTING) || defined(DM_MEMORY_HOOKS_TESTING)
#error "Native experimental activation must not use fixture activation flags"
#endif
static void allocator_dump(void *context, const void *self, const dm_sa_stats *s) {
    (void)context;
    LOG("experimental shared memory Dump self=%p capacity=%zu backing=%zu live=%zu blocks=%zu spans=%zu free=%zu largest=%zu metadata=%zu closed=%d",
        self, s->capacity, s->backing_bytes, s->allocated_bytes, s->allocation_count,
        s->span_count, s->total_free, s->largest_free, s->metadata_bytes, s->closed);
}
static void allocator_event(void *context, const dm_sa_adapter_event *e) {
    (void)context;
    const char *operation = e->kind == DM_SA_EVENT_INITIALIZED ? "Initialize success" :
        e->kind == DM_SA_EVENT_ALLOCATE_FAILED ? "Allocate failure" :
        e->kind == DM_SA_EVENT_REALLOCATE_FAILED ? "Reallocate failure" :
        e->result == DM_SA_OK ? "Finalize success" : "Finalize retained";
    LOG("experimental shared memory managed %s self=%p generation=%llu requested=%zu alignment=%zu result=%d failure_count=%llu",
        operation, e->self, (unsigned long long)e->generation, e->requested_size,
        e->alignment, e->result, (unsigned long long)e->failure_count);
    if (e->stats_result == DM_SA_OK)
        LOG("experimental shared memory snapshot self=%p generation=%llu capacity=%zu backing=%zu live=%zu blocks=%zu spans=%zu free=%zu largest=%zu metadata=%zu closed=%d",
            e->self, (unsigned long long)e->generation, e->stats.capacity,
            e->stats.backing_bytes, e->stats.allocated_bytes, e->stats.allocation_count,
            e->stats.span_count, e->stats.total_free, e->stats.largest_free,
            e->stats.metadata_bytes, e->stats.closed);
    else if (e->kind != DM_SA_EVENT_FINALIZED || e->result != DM_SA_OK)
        LOG("experimental shared memory snapshot unavailable self=%p generation=%llu result=%d",
            e->self, (unsigned long long)e->generation, e->stats_result);
}
static void allocator_fatal(void *context, const char *operation, dm_sa_result result) {
    (void)context;
    die("[RED] experimental shared memory StandardAllocator %s failed (%d); SDK ownership will not be restored", operation, result);
}
static void vamm_fatal(void *context, const char *operation) {
    (void)context;
    die("[RED] experimental shared memory Vamm %s failed; SDK ownership will not be restored", operation);
}
static void sdk_vamm_fatal(void *context, const char *operation) {
    (void)context;
    die("[RED] experimental SDK-only memory Vamm %s failed; SDK ownership will not be restored", operation);
}
#endif

// Construction grants no permit and publishes no hooks. This helper is kept
// private: production reaches it only through exact registry selection/preflight.
// Default-off builds have no caller; Clang diagnoses even unused static inline functions.
__attribute__((unused))
static inline dm_sa_result create_contract_runtime(const startup_contract *c, dm_memory_runtime **out) {
    if (validate_contract(c)) return DM_SA_INVALID;
    return c->mode == SDK_MEMORY ?
        dm_memory_runtime_create_vamm_only_with_contract(out, c->max_regions, &c->runtime) :
        dm_memory_runtime_create_with_contract(out, c->max_regions, &c->runtime);
}

int dm_memory_experiment_startup(const char *request, const char **diagnostic) {
    if (diagnostic) *diagnostic = NULL;
    const char *why = NULL;
    const startup_contract *selected = select_contract(request);
    int repeated = startup_seen; startup_seen = 1;
    if (repeated) why = "startup/restart is unsupported; start a new host process";
    else if (!request || !*request || !strcmp(request, "0")) return DM_MEMORY_EXPERIMENT_OFF;
    else if (!selected)
        why = "expected DM_EXPERIMENTAL_SHARED_MEMORY=" DM_MEMORY_EXPERIMENT_NAME " or " DM_SDK_MEMORY_EXPERIMENT_NAME;
#if !defined(DM_EXPERIMENTAL_SHARED_MEMORY) || !DM_EXPERIMENTAL_SHARED_MEMORY
    else why = "host was built without EXPERIMENTAL_SHARED_MEMORY=1";
#elif !defined(DM_MEMORY_RUNTIME_ALIGNED_LEASES)
    else why = "alignment-aware native leases are required; activation remains disabled";
#else
    else why = preflight_contract(selected, g_mod, (size_t)g_nmod);
    if (!why) {
        int sdk_only = selected->mode == SDK_MEMORY;
        // Bounded host metadata, allocated before any guest entry. This limits
        // live reservations, not the allocator's nominal virtual capacity.
        dm_memory_runtime *runtime = NULL;
        dm_sa_adapter_config allocator = {0};
        dm_vamm_adapter_config vamm = {0};
        if (create_contract_runtime(selected, &runtime) != DM_SA_OK)
            why = "cannot create native memory owner";
        else if ((sdk_only ? dm_memory_runtime_vamm_config(runtime, &vamm) :
                             dm_memory_runtime_configs(runtime, &vamm, &allocator)) != DM_SA_OK)
            why = "cannot configure native memory owner";
        else {
            authorization.runtime = runtime;
            authorization.mode = sdk_only ? SDK_MEMORY : FULL_MEMORY;
            vamm.experimental_startup = &authorization;
            // No PROOF_ALL: static closure evidence is deliberately narrower
            // than general behavior/concurrency/teardown compatibility.
            if (!sdk_only) {
                allocator.experimental_startup = &authorization;
                allocator.reject_managed_walk = 1;
                allocator.dump = allocator_dump; allocator.event = allocator_event; allocator.fatal = allocator_fatal;
            }
            vamm.fatal = sdk_only ? sdk_vamm_fatal : vamm_fatal;
            authorization.installing = 1;
            int result = sdk_only ? loader_install_vamm_hooks(&vamm) :
                                   loader_install_memory_hooks(&allocator, &vamm);
            authorization.installing = 0;
            if (result == DM_MEMORY_HOOKS_OK) {
                // Process lifetime only. dm_host_shutdown's _exit is not an
                // exclusive guest teardown boundary. Never release/reactivate.
                if (sdk_only)
                    LOG("experimental SDK-only memory %s active: exact main+sdk, process lifetime, original StandardAllocator and profile alignment retained; original 12 GiB startup arena remains unchanged and may exceed native alias domain", DM_SDK_MEMORY_EXPERIMENT_NAME);
                else
                    LOG("experimental shared memory %s active: exact main+sdk, process lifetime, managed Walk rejected", DM_MEMORY_EXPERIMENT_NAME);
                return DM_MEMORY_EXPERIMENT_ACTIVE;
            }
            LOG("[RED] experimental %s memory relocation transaction rejected (%d)", sdk_only ? "SDK-only" : "shared", result);
            why = "loader rejected the complete ownership transaction";
        }
        authorization.runtime = NULL;
        // Transaction failure cancels both preparations before returning. No
        // reservation/config can exist before guest entry; failure to destroy
        // is a lifecycle bug, not permission to resume under the SDK.
        if (runtime && dm_memory_runtime_destroy(runtime) != DM_SA_OK)
            die("[RED] experimental %s memory prepublication cleanup failed", sdk_only ? "SDK-only" : "shared");
    }
#endif
    if (diagnostic) *diagnostic = why;
    return DM_MEMORY_EXPERIMENT_FAILED;
}
