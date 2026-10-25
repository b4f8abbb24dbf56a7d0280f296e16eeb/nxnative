#include "hle_memory_runtime.h"
#include "hle_standard_allocator_provider.h"
#include "hle_vamm_native.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct runtime_client {
    void *self;
    size_t capacity;
    struct runtime_client *next;
} runtime_client;
struct dm_memory_runtime {
    pthread_mutex_t mutex;
    dm_vamm *owner;
    dm_sa_sdk_provider *provider;
    runtime_client *clients;
    unsigned vamm_refs, allocator_refs;
};

// This is deliberately not a guest-TLS shim. Every callback installed here is
// native, has no route to an SDK original, and never creates/uses guest TLS.
// No native mutex is held across ANY guest callback, including diagnostics.
// Provider drops the SVC source guard before entering this closure; order is
// runtime -> owner -> upstream SVC memory scope, never the reverse.
static int native_owner_active(void *context) {
    dm_memory_runtime *r = context;
    pthread_mutex_lock(&r->mutex);
    int active = r->vamm_refs && dm_vamm_is_initialized(r->owner) &&
                 dm_vamm_is_enabled(r->owner);
    pthread_mutex_unlock(&r->mutex);
    return active;
}
static uint32_t owner_allocate(void *context, uintptr_t *out, size_t size) {
    dm_memory_runtime *r = context;
    return dm_vamm_allocate(r->owner, out, size, 1);
}
static uint32_t owner_allocate_aligned(void *context, uintptr_t *out, size_t size, size_t alignment) {
    dm_memory_runtime *r = context;
    return dm_vamm_allocate_aligned(r->owner, out, size, alignment, 1);
}
static uint32_t owner_free(void *context, uintptr_t base) {
    dm_memory_runtime *r = context;
    return dm_vamm_free(r->owner, base);
}
static dm_sa_result owner_pages(void *context, uintptr_t base, size_t size, int commit) {
    dm_memory_runtime *r = context;
    uint32_t result = dm_vamm_pages(r->owner, base, size, commit);
    return !result ? DM_SA_OK : result == DM_VAMM_NO_MEMORY ? DM_SA_NO_MEMORY :
        result == 0x803 ? DM_SA_BUSY : DM_SA_PROVIDER_FAILED;
}
static dm_sa_result owner_stats(void *context, size_t alignment, dm_sa_provider_stats *out) {
    dm_memory_runtime *r = context;
    dm_vamm_stats s;
    if (dm_vamm_get_aligned_stats(r->owner, alignment, &s)) return DM_SA_PROVIDER_FAILED;
    // Real reservation gaps, guards, explicit alignment and metadata capacity. The
    // shared provider independently clamps these against guarded physical RAM.
    *out = (dm_sa_provider_stats){s.available_bytes, s.largest_region};
    return DM_SA_OK;
}
static uint32_t retain_owner(void *context, dm_vamm *owner) {
    dm_memory_runtime *r = context;
    pthread_mutex_lock(&r->mutex);
    dm_vamm_stats s;
    uint32_t result = DM_VAMM_INVALID;
    // The explicit capability must name our never-initialized, unused owner;
    // no global getter, second owner or handoff from an active SDK is allowed.
    if (owner == r->owner && !r->vamm_refs && !r->clients &&
        !dm_vamm_is_initialized(owner) && !dm_vamm_get_stats(owner, 0x10000, &s) && !s.regions) {
        r->vamm_refs = 1; result = DM_VAMM_OK;
    }
    pthread_mutex_unlock(&r->mutex);
    return result;
}
static void release_owner(void *context, dm_vamm *owner) {
    dm_memory_runtime *r = context;
    pthread_mutex_lock(&r->mutex);
    if (owner != r->owner || r->vamm_refs != 1) abort();
    r->vamm_refs = 0;
    pthread_mutex_unlock(&r->mutex);
}
static dm_sa_result retain_context(void *context) {
    dm_memory_runtime *r = context;
    pthread_mutex_lock(&r->mutex);
    dm_sa_result result = r->allocator_refs ? DM_SA_BUSY : DM_SA_OK;
    if (!r->allocator_refs) r->allocator_refs = 1;
    pthread_mutex_unlock(&r->mutex);
    return result;
}
static void release_context(void *context) {
    dm_memory_runtime *r = context;
    pthread_mutex_lock(&r->mutex);
    if (r->allocator_refs != 1 || r->clients) abort();
    r->allocator_refs = 0;
    pthread_mutex_unlock(&r->mutex);
}
static dm_sa_result make_config(void *context, void *self, size_t capacity,
                                unsigned thread_cache, dm_sa_config *out) {
    dm_memory_runtime *r = context;
    if (!self || !out || !capacity || capacity > INT64_MAX || (capacity & 0xfff) || thread_cache > 1)
        return DM_SA_INVALID;
    runtime_client *client = calloc(1, sizeof *client);
    if (!client) return DM_SA_NO_MEMORY;
    pthread_mutex_lock(&r->mutex);
    dm_sa_result result = DM_SA_INVALID;
    // Only adapter bootstrap + its observed initializer can make this owner
    // usable. Zero system-resource domains remain disabled, without SDK init.
    if (!r->vamm_refs || !r->allocator_refs || !dm_vamm_is_initialized(r->owner) ||
        !dm_vamm_is_enabled(r->owner)) goto done;
    for (runtime_client *p = r->clients; p; p = p->next)
        if (p->self == self) { result = DM_SA_BUSY; goto done; }
    client->self = self; client->capacity = capacity;
    client->next = r->clients; r->clients = client;
    *out = (dm_sa_config){.provider = dm_sa_sdk_provider_interface(r->provider),
                          .capacity = capacity};
    client = NULL; result = DM_SA_OK;
done:
    pthread_mutex_unlock(&r->mutex);
    free(client);
    return result;
}
static void release_config(void *context, void *self, const dm_sa_config *config) {
    dm_memory_runtime *r = context;
    pthread_mutex_lock(&r->mutex);
    runtime_client **link = &r->clients;
    while (*link && (*link)->self != self) link = &(*link)->next;
    if (!*link || !config || config->provider.context != r->provider ||
        config->capacity != (*link)->capacity) abort();
    runtime_client *client = *link; *link = client->next;
    pthread_mutex_unlock(&r->mutex);
    free(client);
}

static dm_sa_result create_runtime(dm_memory_runtime **out, size_t max_regions,
                                   int with_allocator, int with_pages,
                                   const dm_memory_runtime_contract *contract) {
    if (!out || !max_regions || !contract) return DM_SA_INVALID;
    dm_memory_runtime_contract selected = *contract;
    // Validate both independent selectors before GetInfo can initialize the
    // native memory backend. A constructor contract grants no hook authority.
    if ((selected.domain != DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA &&
         selected.domain != DM_VAMM_DOMAIN_RAW_ALIAS) ||
        (selected.placement != DM_VAMM_PLACEMENT_MODERN &&
         selected.placement != DM_VAMM_PLACEMENT_LEGACY &&
         selected.placement != DM_VAMM_PLACEMENT_LEGACY_REDUCED)) return DM_SA_INVALID;
    dm_vamm_config config;
    if (dm_vamm_native_configure_domain(&config, max_regions, selected.domain)) return DM_SA_INVALID;
    config.placement = selected.placement;
    dm_memory_runtime *r = calloc(1, sizeof *r);
    if (!r) return DM_SA_NO_MEMORY;
    if (pthread_mutex_init(&r->mutex, NULL)) { free(r); return DM_SA_NO_MEMORY; }
    uint32_t result = dm_vamm_create(&r->owner, &config);
    dm_sa_result status = result == DM_VAMM_NO_MEMORY ? DM_SA_NO_MEMORY : DM_SA_INVALID;
    if (result) goto fail;
    if (with_allocator) {
        dm_sa_sdk_owner owner = {.context = r, .allocate_memory = owner_allocate,
            .free_address_region = owner_free, .query_reservations = owner_stats,
            .guest_context_active = native_owner_active, .guaranteed_alignment = 0x10000,
            .allocate_memory_aligned = owner_allocate_aligned,
            .change_pages = with_pages ? owner_pages : NULL};
        status = dm_sa_sdk_provider_create(&r->provider, &owner);
        if (status != DM_SA_OK) { if (dm_vamm_destroy(r->owner)) abort(); goto fail; }
    }
    *out = r; return DM_SA_OK;
fail:
    pthread_mutex_destroy(&r->mutex); free(r); return status;
}
dm_sa_result dm_memory_runtime_create_with_contract(dm_memory_runtime **out,
    size_t max_regions, const dm_memory_runtime_contract *contract) {
    return create_runtime(out, max_regions, 1, 0, contract);
}
dm_sa_result dm_memory_runtime_create_page_capable_with_contract(dm_memory_runtime **out,
    size_t max_regions, const dm_memory_runtime_contract *contract) {
    return create_runtime(out, max_regions, 1, 1, contract);
}
dm_sa_result dm_memory_runtime_create_vamm_only_with_contract(dm_memory_runtime **out,
    size_t max_regions, const dm_memory_runtime_contract *contract) {
    return create_runtime(out, max_regions, 0, 0, contract);
}
static const dm_memory_runtime_contract modern_contract = {
    DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA, DM_VAMM_PLACEMENT_MODERN
};
dm_sa_result dm_memory_runtime_create(dm_memory_runtime **out, size_t max_regions) {
    return dm_memory_runtime_create_with_contract(out, max_regions, &modern_contract);
}
dm_sa_result dm_memory_runtime_create_vamm_only(dm_memory_runtime **out, size_t max_regions) {
    return dm_memory_runtime_create_vamm_only_with_contract(out, max_regions, &modern_contract);
}
static dm_vamm_adapter_config vamm_config(dm_memory_runtime *r) {
    return (dm_vamm_adapter_config){.context = r, .shared_owner = r->owner,
        .retain_owner = retain_owner, .release_owner = release_owner};
}
dm_sa_result dm_memory_runtime_configs(dm_memory_runtime *r, dm_vamm_adapter_config *v,
                                      dm_sa_adapter_config *a) {
    if (!r || !v || !a || !r->provider) return DM_SA_INVALID;
    *v = vamm_config(r);
    *a = (dm_sa_adapter_config){.context = r, .make_config = make_config,
        .release_config = release_config, .retain_context = retain_context,
        .release_context = release_context, .allocation_alignment = dm_sa_host_allocation_alignment,
        .forward_allocate = dm_sa_host_forward_allocate};
    return DM_SA_OK;
}
dm_sa_result dm_memory_runtime_vamm_config(dm_memory_runtime *r, dm_vamm_adapter_config *v) {
    if (!r || !v || r->provider) return DM_SA_INVALID;
    *v = vamm_config(r);
    return DM_SA_OK;
}
dm_sa_result dm_memory_runtime_destroy(dm_memory_runtime *r) {
    if (!r) return DM_SA_INVALID;
    // Lifecycle is exclusive: no new table preparation/config after this point.
    pthread_mutex_lock(&r->mutex);
    int busy = r->vamm_refs || r->allocator_refs || r->clients;
    pthread_mutex_unlock(&r->mutex);
    if (busy) return DM_SA_BUSY;
    dm_vamm_stats stats;
    if (dm_vamm_get_stats(r->owner, 0x10000, &stats)) return DM_SA_PROVIDER_FAILED;
    if (stats.regions) return DM_SA_BUSY;
    if (r->provider) {
        dm_sa_result result = dm_sa_sdk_provider_destroy(r->provider);
        if (result != DM_SA_OK) return result;
    }
    // No remaining caller, config, lease or native reservation can exist here.
    if (dm_vamm_destroy(r->owner)) abort();
    pthread_mutex_destroy(&r->mutex); free(r); return DM_SA_OK;
}
