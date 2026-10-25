// Native ownership wiring, not an activation switch. The loader must
// prepare/commit the complete independently authorized ownership transaction.
#pragma once
#include "hle_standard_allocator_adapter.h"
#include "hle_vamm_adapter.h"
#include "hle_vamm_native.h"

// This runtime uses alignment-aware exact leases and matching feasibility
// statistics. It does not promise that any particular alignment/size fits the
// current guarded address domain or physical budget, and does not enable hooks.
#define DM_MEMORY_RUNTIME_ALIGNED_LEASES 1

typedef struct dm_memory_runtime dm_memory_runtime;
// Independent construction mechanisms, NOT SDK eligibility or activation.
// The values are copied into the owner before publication and cannot change
// during its lifetime. No pointer to this caller-owned contract is retained.
typedef struct {
    dm_vamm_native_domain_policy domain;
    dm_vamm_placement placement;
} dm_memory_runtime_contract;
// Explicit-contract counterparts for separately reviewed callers. Invalid
// contracts reject before native GetInfo queries; every failure preserves out.
// Construction creates no hooks/authority and does not initialize the owner.
dm_sa_result dm_memory_runtime_create_with_contract(dm_memory_runtime **out,
    size_t max_regions, const dm_memory_runtime_contract *contract);
dm_sa_result dm_memory_runtime_create_vamm_only_with_contract(dm_memory_runtime **out,
    size_t max_regions, const dm_memory_runtime_contract *contract);
// Explicit full-mode construction with native partial-page backing. The bounded
// startup policy selects it only after its independent full-mode preflight and
// explicit reclaim flag. This grants no hook/game authorization. Existing constructors
// remain whole-lease-backed; there is deliberately no SDK-only counterpart.
dm_sa_result dm_memory_runtime_create_page_capable_with_contract(dm_memory_runtime **out,
    size_t max_regions, const dm_memory_runtime_contract *contract);
// Preallocates one native Vamm owner and its exact-lease provider. Does not
// initialize Vamm, call guest code, reserve guest addresses or publish hooks.
// Exclusive lifecycle; caller must not create an SDK owner over the same range.
// Existing entry points always select ALIAS_MINUS_EXTRA + MODERN placement.
dm_sa_result dm_memory_runtime_create(dm_memory_runtime **out, size_t max_regions);
// SDK-only alternative: preallocates ONLY the native Vamm owner. It creates no
// StandardAllocator provider, factory, client or guest-forwarding callback.
// It does not establish any SDK/main eligibility or publish/initialize hooks.
dm_sa_result dm_memory_runtime_create_vamm_only(dm_memory_runtime **out, size_t max_regions);
// Fill fresh adapter settings with this runtime's explicit shared owner,
// lifetime callbacks, lazy config factory and the existing host alignment shim.
// Census/proofs/transaction evidence are intentionally zero. The caller fills
// them independently; default production adapter preparation stays disabled.
// The bounded experiment supplies a startup-only authorization, not proof bits.
// Keep context and all lifetime/factory/policy fields unchanged. The runtime
// must outlive both prepared/active adapter tables and every successful config.
dm_sa_result dm_memory_runtime_configs(dm_memory_runtime *runtime,
                                      dm_vamm_adapter_config *vamm,
                                      dm_sa_adapter_config *allocator);
// Owner-only counterpart; accepts only create_vamm_only runtimes. Supplies
// Vamm ownership/lifetime fields, with authorization and symbols left empty.
// Full configs rejects an owner-only runtime. Both APIs leave outputs unchanged
// on failure; neither retains the returned config until adapter preparation.
dm_sa_result dm_memory_runtime_vamm_config(dm_memory_runtime *runtime,
                                         dm_vamm_adapter_config *vamm);
// Refuses retained adapter tables, configs (even zero-backing cores), native
// reservations or provider leases. Failure preserves the whole runtime. Call
// only with guest execution stopped and exclusive lifecycle ownership.
dm_sa_result dm_memory_runtime_destroy(dm_memory_runtime *runtime);

// Original-only hle_os shim; never performs managed dispatch. Invoked only for
// SDK-owned objects already in the valid guest call context. Full-group mode
// forwards non-NULL-buffer objects; SDK-only also retains original virtual mode.
void *dm_sa_host_forward_allocate(void *self, size_t size);
