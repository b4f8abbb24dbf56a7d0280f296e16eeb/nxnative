#include "hle_vamm_adapter.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY
// Kept independent of dm.h so portable adapter fixtures need no game profile.
extern int dm_memory_experiment_authorized(const struct dm_memory_experiment_authorization *, const void *);
#endif

_Static_assert(sizeof(uintptr_t) == 8 && sizeof(size_t) == 8, "64-bit Vamm ABI");
_Static_assert(sizeof(dm_vamm_resource_usage) == 16, "ResourceUsage result in x0/x1");
enum { INIT, RESERVE, ALLOCATE, MAP, RELEASE, UNMAP, USAGE, ENABLED,
       CTOR1, CTOR2, DETAIL_INIT, DETAIL_ENABLED, DETAIL_RESERVE, DETAIL_ALLOCATE,
       DETAIL_MAP, DETAIL_RELEASE, DETAIL_UNMAP, DETAIL_USAGE };
static const char *const names[DM_VAMM_SYMBOL_COUNT] = {
    "_ZN2nn2os30InitializeVirtualAddressMemoryEv",
    "_ZN2nn2os21AllocateAddressRegionEPmm",
    "_ZN2nn2os14AllocateMemoryEPmm",
    "_ZN2nn2os19AllocateMemoryPagesEmm",
    "_ZN2nn2os17FreeAddressRegionEm",
    "_ZN2nn2os15FreeMemoryPagesEmm",
    "_ZN2nn2os36GetVirtualAddressMemoryResourceUsageEv",
    "_ZN2nn2os29IsVirtualAddressMemoryEnabledEv",
    "_ZN2nn2os6detail11VammManagerC1Ev",
    "_ZN2nn2os6detail11VammManagerC2Ev",
    "_ZN2nn2os6detail11VammManager19InitializeIfEnabledEv",
    "_ZNK2nn2os6detail11VammManager29IsVirtualAddressMemoryEnabledEv",
    "_ZN2nn2os6detail11VammManager21AllocateAddressRegionEPmm",
    "_ZN2nn2os6detail11VammManager14AllocateMemoryEPmm",
    "_ZN2nn2os6detail11VammManager19AllocateMemoryPagesEmm",
    "_ZN2nn2os6detail11VammManager17FreeAddressRegionEm",
    "_ZN2nn2os6detail11VammManager15FreeMemoryPagesEmm",
    "_ZN2nn2os6detail11VammManager36GetVirtualAddressMemoryResourceUsageEv"
};
static pthread_mutex_t lifecycle = PTHREAD_MUTEX_INITIALIZER;
static dm_vamm_adapter_config settings;
static dm_vamm_guest_fn originals[DM_VAMM_SYMBOL_COUNT];
static dm_vamm *owner;
static void *identity;
static int constructing, preparing;
static _Atomic int active, stopped;
struct dm_vamm_adapter_prepared {
    dm_vamm_adapter_config config;
    dm_vamm_guest_fn originals[DM_VAMM_SYMBOL_COUNT];
    dm_vamm *owner;
};
static dm_vamm_adapter_prepared *pending;
static void require_active(void) {
    if (!atomic_load_explicit(&active, memory_order_acquire) ||
        atomic_load_explicit(&stopped, memory_order_acquire)) abort();
}
static _Noreturn void fail(const char *operation) {
    require_active(); // stopped shutdown may already have released settings.context
    if (settings.fatal) settings.fatal(settings.context, operation);
    abort();
}
static dm_vamm *take(const void *self, const char *operation) {
    require_active();
    pthread_mutex_lock(&lifecycle);
    int valid = owner && identity && !constructing && !stopped && (!self || self == identity);
    dm_vamm *result = owner;
    pthread_mutex_unlock(&lifecycle);
    if (!valid) fail(operation);
    return result;
}
static dm_vamm *detail(const void *self, const char *operation) {
    require_active();
    if (!self) fail(operation);
    return take(self, operation);
}
static void construct(void *self, unsigned index) {
    require_active();
    pthread_mutex_lock(&lifecycle);
    if (!self || identity || stopped || constructing) { pthread_mutex_unlock(&lifecycle); fail("constructor"); }
    constructing = 1;
    pthread_mutex_unlock(&lifecycle);
    // The proven original ctor only bootstraps the lock/domain and NULL owner.
    // NEVER call original InitializeIfEnabled: it constructs the SDK node tree.
    ((void (*)(void *))originals[index])(self);
    pthread_mutex_lock(&lifecycle);
    identity = self; constructing = 0;
    pthread_mutex_unlock(&lifecycle);
}
static void ctor1(void *self) { construct(self, CTOR1); }
static void ctor2(void *self) { construct(self, CTOR2); }
static void init(void) { dm_vamm_initialize(take(NULL, "initialize")); }
static int enabled(void) { return dm_vamm_is_enabled(take(NULL, "enabled")); }
static uint32_t reserve(uintptr_t *out, size_t size) { return dm_vamm_allocate(take(NULL, "reserve"), out, size, 0); }
static uint32_t allocate(uintptr_t *out, size_t size) { return dm_vamm_allocate(take(NULL, "allocate"), out, size, 1); }
static uint32_t map(uintptr_t base, size_t size) { return dm_vamm_pages(take(NULL, "map"), base, size, 1); }
static uint32_t release(uintptr_t base) { return dm_vamm_free(take(NULL, "release"), base); }
static uint32_t unmap(uintptr_t base, size_t size) { return dm_vamm_pages(take(NULL, "unmap"), base, size, 0); }
static dm_vamm_resource_usage usage(void) { return dm_vamm_resources(take(NULL, "usage")); }
static void d_init(void *self) { dm_vamm_initialize(detail(self, "detail initialize")); }
static int d_enabled(const void *self) { return dm_vamm_is_enabled(detail(self, "detail enabled")); }
static uint32_t d_reserve(void *self, uintptr_t *out, size_t size) { return dm_vamm_allocate(detail(self, "detail reserve"), out, size, 0); }
static uint32_t d_allocate(void *self, uintptr_t *out, size_t size) { return dm_vamm_allocate(detail(self, "detail allocate"), out, size, 1); }
static uint32_t d_map(void *self, uintptr_t base, size_t size) { return dm_vamm_pages(detail(self, "detail map"), base, size, 1); }
static uint32_t d_release(void *self, uintptr_t base) { return dm_vamm_free(detail(self, "detail release"), base); }
static uint32_t d_unmap(void *self, uintptr_t base, size_t size) { return dm_vamm_pages(detail(self, "detail unmap"), base, size, 0); }
static dm_vamm_resource_usage d_usage(void *self) { return dm_vamm_resources(detail(self, "detail usage")); }
static const dm_vamm_guest_fn hooks[DM_VAMM_SYMBOL_COUNT] = {
    (dm_vamm_guest_fn)init, (dm_vamm_guest_fn)reserve, (dm_vamm_guest_fn)allocate,
    (dm_vamm_guest_fn)map, (dm_vamm_guest_fn)release, (dm_vamm_guest_fn)unmap,
    (dm_vamm_guest_fn)usage, (dm_vamm_guest_fn)enabled, (dm_vamm_guest_fn)ctor1,
    (dm_vamm_guest_fn)ctor2, (dm_vamm_guest_fn)d_init, (dm_vamm_guest_fn)d_enabled,
    (dm_vamm_guest_fn)d_reserve, (dm_vamm_guest_fn)d_allocate, (dm_vamm_guest_fn)d_map,
    (dm_vamm_guest_fn)d_release, (dm_vamm_guest_fn)d_unmap, (dm_vamm_guest_fn)d_usage
};
const char *dm_vamm_adapter_symbol_name(size_t index) { return index < DM_VAMM_SYMBOL_COUNT ? names[index] : NULL; }
dm_vamm_guest_fn dm_vamm_adapter_lookup(const char *name) {
    if (!name || !atomic_load_explicit(&active, memory_order_acquire)) return NULL;
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i) if (!strcmp(name, names[i])) return hooks[i];
    return NULL;
}
uint32_t dm_vamm_adapter_prepare(const dm_vamm_adapter_config *c, dm_vamm_adapter_prepared **out) {
    if (!out) return DM_VAMM_INVALID;
    *out = NULL;
#if !defined(DM_VAMM_ADAPTER_TESTING) && (!defined(DM_EXPERIMENTAL_SHARED_MEMORY) || !DM_EXPERIMENTAL_SHARED_MEMORY)
    (void)c; return DM_VAMM_INVALID;
#else
    int authorized = 0;
#ifdef DM_VAMM_ADAPTER_TESTING
    authorized = c && c->proofs == DM_VAMM_PROOF_ALL;
#endif
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY
    authorized = c && !c->proofs && dm_memory_experiment_authorized(c->experimental_startup, c->context) && c->shared_owner;
#endif
    if (!authorized ||
        !c->symbols || c->symbol_count != DM_VAMM_SYMBOL_COUNT ||
        (!!c->shared_owner != !!c->retain_owner) || (!!c->shared_owner != !!c->release_owner)) return DM_VAMM_INVALID;
    dm_vamm_guest_fn captured[DM_VAMM_SYMBOL_COUNT] = {0};
    for (size_t k = 0; k < c->symbol_count; ++k) {
        if (!c->symbols[k].name || !c->symbols[k].function) return DM_VAMM_INVALID;
        unsigned i = 0;
        while (i < DM_VAMM_SYMBOL_COUNT && strcmp(c->symbols[k].name, names[i])) ++i;
        if (i == DM_VAMM_SYMBOL_COUNT || captured[i]) return DM_VAMM_INVALID;
        for (unsigned j = 0; j < DM_VAMM_SYMBOL_COUNT; ++j)
            if (c->symbols[k].function == hooks[j]) return DM_VAMM_INVALID;
        captured[i] = c->symbols[k].function;
    }
    pthread_mutex_lock(&lifecycle);
    if (preparing || pending || atomic_load_explicit(&active, memory_order_relaxed)) { pthread_mutex_unlock(&lifecycle); return DM_VAMM_INVALID; }
    preparing = 1;
    pthread_mutex_unlock(&lifecycle);
    dm_vamm_adapter_prepared *p = calloc(1, sizeof *p);
    uint32_t result = p ? DM_VAMM_OK : DM_VAMM_NO_MEMORY;
    if (p) {
        p->config = *c; p->config.symbols = NULL;
        memcpy(p->originals, captured, sizeof captured);
        if (c->shared_owner) {
            result = c->retain_owner(c->context, c->shared_owner);
            if (!result) {
                p->owner = c->shared_owner;
                dm_vamm_stats stats;
                result = dm_vamm_get_stats(p->owner, 0x10000, &stats);
                if (!result && stats.regions) result = DM_VAMM_INVALID;
                if (result) c->release_owner(c->context, p->owner);
            }
        } else result = dm_vamm_create(&p->owner, &c->owner);
    }
    pthread_mutex_lock(&lifecycle);
    preparing = 0;
    if (!result) { pending = p; *out = p; } else free(p);
    pthread_mutex_unlock(&lifecycle); return result;
#endif
}
dm_vamm_guest_fn dm_vamm_adapter_prepared_hook(const dm_vamm_adapter_prepared *p, size_t index) {
    pthread_mutex_lock(&lifecycle);
    dm_vamm_guest_fn hook = p && p == pending && index < DM_VAMM_SYMBOL_COUNT ? hooks[index] : NULL;
    pthread_mutex_unlock(&lifecycle); return hook;
}
void dm_vamm_adapter_publish(dm_vamm_adapter_prepared *p) {
    pthread_mutex_lock(&lifecycle);
    if (!p || p != pending) abort();
    settings = p->config; owner = p->owner;
    memcpy(originals, p->originals, sizeof originals);
    pending = NULL; free(p);
    atomic_store_explicit(&active, 1, memory_order_release);
    pthread_mutex_unlock(&lifecycle);
}
void dm_vamm_adapter_cancel(dm_vamm_adapter_prepared *p) {
    pthread_mutex_lock(&lifecycle);
    if (!p || p != pending) abort();
    pending = NULL; preparing = 1;
    pthread_mutex_unlock(&lifecycle);
    if (p->config.shared_owner) p->config.release_owner(p->config.context, p->owner);
    else if (dm_vamm_destroy(p->owner)) abort();
    free(p);
    pthread_mutex_lock(&lifecycle); preparing = 0; pthread_mutex_unlock(&lifecycle);
}
uint32_t dm_vamm_adapter_install(const dm_vamm_adapter_config *c) {
    dm_vamm_adapter_prepared *p = NULL;
    uint32_t r = dm_vamm_adapter_prepare(c, &p);
    if (!r) dm_vamm_adapter_publish(p);
    return r;
}
uint32_t dm_vamm_adapter_shutdown(void) {
    pthread_mutex_lock(&lifecycle);
    if (settings.experimental_startup) { pthread_mutex_unlock(&lifecycle); return DM_VAMM_INVALID; }
    uint32_t result = DM_VAMM_INVALID;
    dm_vamm *released = NULL;
    if (owner && !constructing) {
        if (settings.shared_owner) {
            dm_vamm_stats stats;
            result = dm_vamm_get_stats(owner, 0x10000, &stats);
            if (!result && stats.regions) result = DM_VAMM_INVALID;
            if (!result) released = owner;
        } else result = dm_vamm_destroy(owner);
    }
    if (!result) { owner = NULL; atomic_store_explicit(&stopped, 1, memory_order_release); }
    pthread_mutex_unlock(&lifecycle);
    if (released) settings.release_owner(settings.context, released);
    return result;
}
