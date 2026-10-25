// Owner-only construction boundary. Real runtime and Vamm source, synthetic
// integer address domain. No guest callback, SDK input or mapped storage.
// Provider stubs count every creation/destruction and trap any interface use.
#include "../hle_memory_runtime.h"
#include "../hle_standard_allocator_provider.h"
#include "../hle_vamm_native.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE = 0x1000, SMALL = 0x10000 };
static unsigned provider_creates, provider_destroys, changes;
static int fail_configure, fail_provider;
static uint32_t page_failure;
static unsigned char provider_token;
static uint32_t pages(void *context, uintptr_t base, size_t size, int commit) {
    assert(!context && base && size && (commit == 0 || commit == 1));
    ++changes; return page_failure;
}
static dm_vamm_resource_usage resources(void *context) {
    assert(!context); return (dm_vamm_resource_usage){0x100000, 0};
}
uint32_t dm_vamm_native_configure(dm_vamm_config *out, size_t max_regions) {
    if (fail_configure) return DM_VAMM_INVALID;
    *out = (dm_vamm_config){.base = 0x100000, .size = 0x1000000,
        .max_regions = max_regions, .change_pages = pages, .resources = resources};
    return DM_VAMM_OK;
}
dm_sa_result dm_sa_sdk_provider_create(dm_sa_sdk_provider **out, const dm_sa_sdk_owner *owner) {
    ++provider_creates;
    assert(owner && owner->context && owner->allocate_memory && owner->free_address_region &&
           owner->query_reservations && owner->guest_context_active && owner->allocate_memory_aligned);
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
static void no_full_config(dm_memory_runtime *r) {
    dm_vamm_adapter_config v, old_v; memset(&v, 0xa5, sizeof v); old_v = v;
    dm_sa_adapter_config a, old_a; memset(&a, 0x5a, sizeof a); old_a = a;
    assert(dm_memory_runtime_configs(r, &v, &a) == DM_SA_INVALID);
    assert(!memcmp(&v, &old_v, sizeof v) && !memcmp(&a, &old_a, sizeof a));
}
int main(void) {
    dm_memory_runtime *r = (void *)1;
    assert(dm_memory_runtime_create_vamm_only(NULL, 8) == DM_SA_INVALID);
    assert(dm_memory_runtime_create_vamm_only(&r, 0) == DM_SA_INVALID && r == (void *)1);
    assert(dm_memory_runtime_create_vamm_only(&r, SIZE_MAX) == DM_SA_INVALID && r == (void *)1);
    fail_configure = 1;
    assert(dm_memory_runtime_create_vamm_only(&r, 8) == DM_SA_INVALID && r == (void *)1);
    fail_configure = 0; fail_provider = 1;
    // Provider failure is irrelevant: owner-only must never attempt to make it.
    assert(dm_memory_runtime_create_vamm_only(&r, 8) == DM_SA_OK);
    assert(!provider_creates && !provider_destroys && !changes);
    no_full_config(r); no_full_config(NULL);
    dm_vamm_adapter_config v, old_v; memset(&v, 0xa5, sizeof v); old_v = v;
    assert(dm_memory_runtime_vamm_config(NULL, &v) == DM_SA_INVALID && !memcmp(&v, &old_v, sizeof v));
    assert(dm_memory_runtime_vamm_config(r, NULL) == DM_SA_INVALID);
    assert(dm_memory_runtime_vamm_config(r, &v) == DM_SA_OK);
    assert(v.context == r && v.shared_owner && v.retain_owner && v.release_owner);
    assert(!v.experimental_startup && !v.proofs && !v.symbols && !v.symbol_count && !v.fatal);
    assert(!v.owner.base && !v.owner.size && !v.owner.max_regions && !v.owner.context &&
           !v.owner.change_pages && !v.owner.resources && v.owner.placement == DM_VAMM_PLACEMENT_MODERN);
    assert(!v.retain_owner(v.context, v.shared_owner));
    assert(v.retain_owner(v.context, v.shared_owner) == DM_VAMM_INVALID);
    assert(dm_memory_runtime_destroy(r) == DM_SA_BUSY);
    v.release_owner(v.context, v.shared_owner);
    // Cancel before initialization permits a fresh preparation of the same owner.
    assert(!v.retain_owner(v.context, v.shared_owner));
    dm_vamm_initialize(v.shared_owner);
    assert(!changes && dm_vamm_is_initialized(v.shared_owner));
    uintptr_t out = 123;
    page_failure = DM_VAMM_NO_MEMORY;
    assert(dm_vamm_allocate(v.shared_owner, &out, SMALL, 1) == DM_VAMM_NO_MEMORY && out == 123);
    page_failure = 0;
    assert(!dm_vamm_allocate(v.shared_owner, &out, SMALL, 1));
    v.release_owner(v.context, v.shared_owner);
    assert(dm_memory_runtime_destroy(r) == DM_SA_BUSY);
    page_failure = 0x803;
    assert(dm_vamm_free(v.shared_owner, out) == 0x803 && dm_memory_runtime_destroy(r) == DM_SA_BUSY);
    page_failure = 0;
    assert(!dm_vamm_free(v.shared_owner, out));
    assert(v.retain_owner(v.context, v.shared_owner) == DM_VAMM_INVALID);
    assert(dm_memory_runtime_destroy(r) == DM_SA_OK);
    assert(!provider_creates && !provider_destroys);
    // Fresh unused owner-only teardown takes no provider destruction path.
    assert(dm_memory_runtime_create_vamm_only(&r, 8) == DM_SA_OK);
    assert(dm_memory_runtime_destroy(r) == DM_SA_OK && !provider_destroys);
    // Existing shared-runtime behavior remains distinct and unchanged.
    r = (void *)1;
    assert(dm_memory_runtime_create(&r, 8) == DM_SA_NO_MEMORY && r == (void *)1);
    fail_provider = 0;
    assert(dm_memory_runtime_create(&r, 8) == DM_SA_OK && provider_creates == 2);
    memset(&v, 0xa5, sizeof v); old_v = v;
    assert(dm_memory_runtime_vamm_config(r, &v) == DM_SA_INVALID && !memcmp(&v, &old_v, sizeof v));
    dm_sa_adapter_config a;
    assert(dm_memory_runtime_configs(r, &v, &a) == DM_SA_OK);
    assert(a.make_config && a.release_config && a.retain_context && a.release_context);
    assert(dm_memory_runtime_destroy(r) == DM_SA_OK && provider_destroys == 1);
    puts("Vamm-only runtime: no provider creation/use, isolated configs, retained/live ownership, rollback: PASS");
    return 0;
}
