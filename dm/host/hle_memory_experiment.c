#include "hle_memory_experiment.h"
#include "hle_memory_hooks.h"
#include "hle_memory_runtime.h"

// These are full NSO identities, not title-ID guesses or private code offsets.
// The reviewed closure is exactly Z-A base main + SDK 19.3.11; updates, subsdks,
// deferred NROs and other SDKs require a new review. Existing cuts stay intact.
static const char reviewed_main[] = "7222e13ecf6adb3249b5dd5c57fa40b102245fce000000000000000000000000";
static const char reviewed_sdk[] = "df7a3960c74c3a4803f826c5871532523aa9cf2a000000000000000000000000";
_Static_assert(sizeof reviewed_main == 65 && sizeof reviewed_sdk == 65, "full NSO identities");

const char *dm_memory_experiment_preflight(const module_t *modules, size_t count) {
    if (!modules || count != 2) return "requires exactly the reviewed main and sdk (no extra or deferred modules)";
    for (size_t k = 0; k < count; ++k) {
        const module_t *m = &modules[k];
        if (!memchr(m->name, 0, sizeof m->name)) return "unterminated module name";
        if (m->nro || !m->loaded || m->loads) return "deferred, unloaded or previously loaded modules are unsupported";
        if (!m->base || !m->size) return "module is not mapped";
        // Global ELF lookup is order-sensitive (including main interposition).
        // This is experiment eligibility, not a generic loader restriction.
        if (strcmp(m->name, k ? "sdk" : "main")) return "requires reviewed module order: main then sdk";
        const char *expected = k ? reviewed_sdk : reviewed_main;
        if (memcmp(m->full_build_id, expected, 65)) return "missing or unreviewed full NSO build ID (rewrite old outputs)";
        if (memcmp(m->build_id, expected, 32) || m->build_id[32]) return "inconsistent short/full NSO build ID";
    }
    return NULL;
}

enum { FULL_MEMORY, SDK_MEMORY };
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

int dm_memory_experiment_startup(const char *request, const char **diagnostic) {
    if (diagnostic) *diagnostic = NULL;
    const char *why = NULL;
    int sdk_only = request && !strcmp(request, DM_SDK_MEMORY_EXPERIMENT_NAME);
    int repeated = startup_seen; startup_seen = 1;
    if (repeated) why = "startup/restart is unsupported; start a new host process";
    else if (!request || !*request || !strcmp(request, "0")) return DM_MEMORY_EXPERIMENT_OFF;
    else if (strcmp(request, DM_MEMORY_EXPERIMENT_NAME) && !sdk_only)
        why = "expected DM_EXPERIMENTAL_SHARED_MEMORY=" DM_MEMORY_EXPERIMENT_NAME " or " DM_SDK_MEMORY_EXPERIMENT_NAME;
#if !defined(DM_EXPERIMENTAL_SHARED_MEMORY) || !DM_EXPERIMENTAL_SHARED_MEMORY
    else why = "host was built without EXPERIMENTAL_SHARED_MEMORY=1";
#elif !defined(DM_MEMORY_RUNTIME_ALIGNED_LEASES)
    else why = "alignment-aware native leases are required; activation remains disabled";
#else
    else why = dm_memory_experiment_preflight(g_mod, (size_t)g_nmod);
    if (!why) {
        // Bounded host metadata, allocated before any guest entry. This limits
        // live reservations, not the allocator's nominal virtual capacity.
        dm_memory_runtime *runtime = NULL;
        dm_sa_adapter_config allocator = {0};
        dm_vamm_adapter_config vamm = {0};
        if ((sdk_only ? dm_memory_runtime_create_vamm_only(&runtime, 65536) :
                        dm_memory_runtime_create(&runtime, 65536)) != DM_SA_OK)
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
