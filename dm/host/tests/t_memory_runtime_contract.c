// Explicit runtime construction -> real native domain decoder and Vamm owner.
// Integer domains and provider lifetime stubs; no SDK input or activation.
#include "../hle_memory_runtime.h"
#include "../hle_standard_allocator_provider.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE = 0x1000 };
static uint64_t base = 0x12345000, extent = UINT64_C(3) << 30, extra = 0x8000000;
static unsigned queries[32], changes, provider_creates, provider_destroys;
static int allow_extra, fail_provider;
static unsigned char provider_token;
static dm_sa_sdk_owner captured_owner;
static dm_memory_runtime_contract *mutate_on_query;
uint64_t dm_vamm_svc_info(unsigned id) {
    assert(id < 32); ++queries[id];
    switch (id) {
    case 2:
        if (mutate_on_query) {
            *mutate_on_query = (dm_memory_runtime_contract){
                (dm_vamm_native_domain_policy)-1, (dm_vamm_placement)-1};
            mutate_on_query = NULL;
        }
        return base;
    case 3: return extent;
    case 28: assert(allow_extra); return extra;
    case 16: return 0x100000;
    case 17: return 0;
    default: abort();
    }
}
uint32_t dm_vamm_svc_change_pages(uintptr_t address, size_t size, int commit) {
    assert(address >= base && size && (commit == 0 || commit == 1));
    ++changes; return 0;
}
dm_sa_result dm_sa_sdk_provider_create(dm_sa_sdk_provider **out, const dm_sa_sdk_owner *owner) {
    ++provider_creates;
    assert(owner && owner->context && owner->allocate_memory && owner->free_address_region &&
           owner->query_reservations && owner->guest_context_active && owner->allocate_memory_aligned);
    captured_owner = *owner;
    if (fail_provider) return DM_SA_NO_MEMORY;
    *out = (dm_sa_sdk_provider *)&provider_token; return DM_SA_OK;
}
dm_sa_result dm_sa_sdk_provider_destroy(dm_sa_sdk_provider *provider) {
    assert(provider == (dm_sa_sdk_provider *)&provider_token);
    ++provider_destroys; return DM_SA_OK;
}
dm_sa_provider dm_sa_sdk_provider_interface(dm_sa_sdk_provider *provider) {
    (void)provider; abort();
}
size_t dm_sa_host_allocation_alignment(void *self, size_t size) {
    (void)self; (void)size; abort();
}
void *dm_sa_host_forward_allocate(void *self, size_t size) {
    (void)self; (void)size; abort();
}
typedef dm_sa_result (*create_fn)(dm_memory_runtime **, size_t, const dm_memory_runtime_contract *);
static const create_fn create[] = {dm_memory_runtime_create_vamm_only_with_contract,
                                  dm_memory_runtime_create_with_contract};
static void reset(int modern) {
    memset(queries, 0, sizeof queries); changes = provider_creates = provider_destroys = 0;
    allow_extra = modern; fail_provider = 0; mutate_on_query = NULL;
}
static void untouched(void) {
    for (unsigned i = 0; i < 32; ++i) assert(!queries[i]);
    assert(!changes && !provider_creates && !provider_destroys);
}
static size_t alignment(size_t size, dm_vamm_placement placement) {
    if (size < 0x200000) return 0x10000;
    if (placement == DM_VAMM_PLACEMENT_MODERN || size < 0x400000) return 0x200000;
    if (size < 0x2000000) return 0x400000;
    if (placement == DM_VAMM_PLACEMENT_LEGACY && size >= 0x40000000) return 0x40000000;
    return 0x2000000;
}
static void invalid_contracts(void) {
    dm_memory_runtime_contract bad[] = {
        {(dm_vamm_native_domain_policy)-1, DM_VAMM_PLACEMENT_MODERN},
        {(dm_vamm_native_domain_policy)2, DM_VAMM_PLACEMENT_LEGACY},
        {DM_VAMM_DOMAIN_RAW_ALIAS, (dm_vamm_placement)-1},
        {DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA, (dm_vamm_placement)3}
    };
    for (unsigned full = 0; full < 2; ++full) {
        dm_memory_runtime *r = (void *)1;
        reset(0);
        for (unsigned i = 0; i < sizeof bad / sizeof *bad; ++i)
            assert(create[full](&r, 17, &bad[i]) == DM_SA_INVALID && r == (void *)1);
        dm_memory_runtime_contract good = {DM_VAMM_DOMAIN_RAW_ALIAS, DM_VAMM_PLACEMENT_LEGACY};
        assert(create[full](&r, 17, NULL) == DM_SA_INVALID && r == (void *)1);
        assert(create[full](NULL, 17, &good) == DM_SA_INVALID);
        assert(create[full](&r, 0, &good) == DM_SA_INVALID && r == (void *)1);
        untouched();
        // A rejected native domain must not allocate provider state or publish out.
        ++base;
        assert(create[full](&r, 17, &good) == DM_SA_INVALID && r == (void *)1);
        --base;
        assert(queries[2] == 1 && queries[3] == 1 && !queries[28]);
        assert(!changes && !provider_creates && !provider_destroys);
    }
}
static void exercise(unsigned full, dm_vamm_native_domain_policy domain,
                     dm_vamm_placement placement, int default_wrapper) {
    reset(domain == DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA);
    dm_memory_runtime_contract contract = {domain, placement};
    dm_memory_runtime *r = NULL;
    if (!default_wrapper) mutate_on_query = &contract;
    assert((default_wrapper ? (full ? dm_memory_runtime_create(&r, 17) :
                                     dm_memory_runtime_create_vamm_only(&r, 17)) :
            create[full](&r, 17, &contract)) == DM_SA_OK);
    // Query-time mutation proves selection was snapshotted before the backend
    // call; later mutation/lifetime cannot change the already-created owner.
    assert(!mutate_on_query);
    contract = (dm_memory_runtime_contract){(dm_vamm_native_domain_policy)-1, (dm_vamm_placement)-1};
    for (unsigned i = 0; i < 32; ++i)
        assert(queries[i] == (unsigned)(i == 2 || i == 3 || (allow_extra && i == 28)));
    assert(!changes && provider_creates == full && !provider_destroys);
    dm_vamm_adapter_config v;
    dm_sa_adapter_config a;
    dm_vamm_adapter_config rejected_v, before_v;
    dm_sa_adapter_config rejected_a, before_a;
    memset(&rejected_v, 0xa5, sizeof rejected_v); memcpy(&before_v, &rejected_v, sizeof before_v);
    memset(&rejected_a, 0x5a, sizeof rejected_a); memcpy(&before_a, &rejected_a, sizeof before_a);
    if (full) {
        assert(dm_memory_runtime_vamm_config(r, &rejected_v) == DM_SA_INVALID);
        assert(!memcmp(&rejected_v, &before_v, sizeof before_v));
        assert(dm_memory_runtime_configs(r, &v, &a) == DM_SA_OK);
        assert(a.context == r && !a.proofs && !a.experimental_startup && !a.symbols);
        assert(a.allocation_alignment == dm_sa_host_allocation_alignment &&
               a.forward_allocate == dm_sa_host_forward_allocate);
        assert(a.retain_context(r) == DM_SA_OK);
    } else {
        assert(dm_memory_runtime_configs(r, &rejected_v, &rejected_a) == DM_SA_INVALID);
        assert(!memcmp(&rejected_v, &before_v, sizeof before_v) &&
               !memcmp(&rejected_a, &before_a, sizeof before_a));
        assert(dm_memory_runtime_vamm_config(r, &v) == DM_SA_OK);
    }
    assert(!v.owner.base && !v.owner.size && !v.owner.max_regions && !v.owner.context &&
           !v.owner.change_pages && !v.owner.resources && v.owner.placement == DM_VAMM_PLACEMENT_MODERN);
    assert(v.context == r && v.shared_owner && !v.proofs && !v.experimental_startup && !v.symbols);
    assert(!dm_vamm_is_initialized(v.shared_owner));
    assert(!v.retain_owner(r, v.shared_owner));
    assert(dm_memory_runtime_destroy(r) == DM_SA_BUSY);
    dm_vamm_initialize(v.shared_owner);
    dm_vamm_stats stats;
    assert(!dm_vamm_get_aligned_stats(v.shared_owner, PAGE, &stats));
    assert(stats.largest_region == extent - (allow_extra ? extra : 0) - 2 * PAGE);
    const size_t sizes[] = {PAGE, 0x1ff000, 0x200000, 0x3ff000, 0x400000,
                           0x1fff000, 0x2000000, 0x3ffff000, 0x40000000};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        uintptr_t address = 123;
        size_t align = alignment(sizes[i], placement);
        assert(!dm_vamm_allocate(v.shared_owner, &address, sizes[i], 0));
        assert(address == ((base + PAGE + align - 1) & ~(uintptr_t)(align - 1)));
        assert(!dm_vamm_get_stats(v.shared_owner, PAGE, &stats) && stats.regions == 1);
        assert(stats.reserved_bytes == sizes[i]);
        assert(!dm_vamm_free(v.shared_owner, address));
    }
    // The runtime provider must use the explicit-alignment callback and its
    // matching statistics, not the SDK's selected size-dependent tier.
    if (full) {
        assert(captured_owner.guest_context_active(r));
        dm_sa_provider_stats provider_stats;
        assert(captured_owner.query_reservations(r, 0x10000, &provider_stats) == DM_SA_OK);
        assert(!dm_vamm_get_aligned_stats(v.shared_owner, 0x10000, &stats));
        assert(provider_stats.available_bytes == stats.available_bytes &&
               provider_stats.largest_span == stats.largest_region);
        uintptr_t lease = 0;
        assert(!captured_owner.allocate_memory_aligned(r, &lease, 0x400000, 0x10000));
        assert(lease == ((base + PAGE + 0xffff) & ~(uintptr_t)0xffff));
        assert(!captured_owner.free_address_region(r, lease));
        assert(!captured_owner.allocate_memory_aligned(r, &lease, PAGE, 0x4000000));
        assert(!(lease & 0x3ffffff) && !captured_owner.free_address_region(r, lease));
    }
    // Explicit native alignment stays independent of the selected SDK tiers.
    uintptr_t address = 123;
    assert(!dm_vamm_allocate_aligned(v.shared_owner, &address, 0x400000, PAGE, 0));
    assert(address == base + PAGE && !dm_vamm_free(v.shared_owner, address));
    assert(dm_vamm_allocate(v.shared_owner, &address, (size_t)4 << 30, 0) == DM_VAMM_NO_SPACE);
    assert(address == base + PAGE); // failed reservation never changes output
    if (full) a.release_context(r);
    v.release_owner(r, v.shared_owner);
    assert(dm_memory_runtime_destroy(r) == DM_SA_OK);
    assert(provider_destroys == full);
}
int main(void) {
    invalid_contracts();
    for (unsigned full = 0; full < 2; ++full) {
        for (unsigned domain = 0; domain < 2; ++domain)
            for (unsigned placement = 0; placement < 3; ++placement)
                exercise(full, (dm_vamm_native_domain_policy)domain, (dm_vamm_placement)placement, 0);
        exercise(full, DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA, DM_VAMM_PLACEMENT_MODERN, 1);
    }
    reset(0); fail_provider = 1;
    dm_memory_runtime_contract contract = {DM_VAMM_DOMAIN_RAW_ALIAS, DM_VAMM_PLACEMENT_LEGACY_REDUCED};
    dm_memory_runtime *r = (void *)1;
    assert(create[1](&r, 17, &contract) == DM_SA_NO_MEMORY && r == (void *)1);
    assert(provider_creates == 1 && !provider_destroys && !changes);
    // The failed provider never publishes/retains an owner; a fresh retry works.
    fail_provider = 0;
    assert(create[1](&r, 17, &contract) == DM_SA_OK);
    assert(dm_memory_runtime_destroy(r) == DM_SA_OK);
    puts("runtime domain/placement contracts: 12 explicit combinations, modern wrappers, immutable owner, rejection/rollback: PASS");
    return 0;
}
