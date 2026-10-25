// Existing-owner exact leases for the StandardAllocator core. This does NOT
// install hooks, reserve alias addresses itself, or enable the adapter gate.
#pragma once
#include "standard_allocator_core.h"

typedef struct dm_sa_sdk_provider dm_sa_sdk_provider;
typedef struct {
    void *context;
    // Existing owner operations, not intercepted allocator entry points. SDK
    // integrations capture ORIGINAL operations; native owners supply their own
    // verified equivalents. Zero means success. AllocateMemory returns exactly size
    // mapped bytes; FreeAddressRegion releases that exact complete reservation.
    // A returning failure MUST preserve the reservation and mapped bytes.
    uint32_t (*allocate_memory)(void *context, uintptr_t *out, size_t size);
    uint32_t (*free_address_region)(void *context, uintptr_t base);
    // Real current-owner reservation-tree snapshot, NOT QueryMemory's holes:
    // uncommitted does not imply unreserved. No implementation is invented for
    // SDKs without a verified snapshot boundary. Physical budget is clamped
    // separately against the real SVC accounting after this callback returns.
    dm_sa_result (*query_reservations)(void *context, size_t alignment,
                                       dm_sa_provider_stats *out);
    // SDK owner: verify the correct guest thread/TLS/gate context. Native owner:
    // verify its retained, initialized capability without creating guest TLS.
    // No provider callback creates guest TLS or runs an SDK on a host worker.
    int (*guest_context_active)(void *context);
    // Guaranteed absolute alignment of EVERY successful original allocation.
    // Without allocate_memory_aligned, requests above this fail without calling
    // the owner: no hidden padding or fabricated larger contiguous region.
    size_t guaranteed_alignment;
    // Optional verified owner operation, used for EVERY lease when present.
    // Returns exactly size mapped bytes at the explicit absolute alignment;
    // no padded mapping/interior pointer. query_reservations must use this same
    // alignment policy. Original SDK owners leave this NULL unless proven.
    uint32_t (*allocate_memory_aligned)(void *context, uintptr_t *out, size_t size,
                                        size_t alignment);
    // Optional verified owner page operation. No SDK original is inferred or
    // installed. Preserve the containing reservation and every committed byte
    // outside the interval. Returning failure changes no pages/accounting;
    // commit preserves already committed bytes and initializes logical holes.
    // The provider applies strict physical-budget scope to every commit.
    dm_sa_result (*change_pages)(void *context, uintptr_t base, size_t size, int commit);
} dm_sa_sdk_owner;

// Callbacks are trusted, verified integration boundaries, not plugin hooks.
// The per-instance core mutex stays held; their complete call closure must not
// re-enter that core or take a reverse-ordered lock. g_map_lock and
// provider metadata locks are NEVER held while an owner callback runs. Ledger
// transitions serialize under the SVC source guard, with in-flight records.
// An owner success violating the exact lease/domain/alignment contract aborts
// rather than publishing an inaccessible pointer or forgetting a reservation.
// Create/destroy require exclusive lifecycle ownership. Destroy refuses while
// a core still owns a lease. Callers retain the provider until all cores drain.
dm_sa_result dm_sa_sdk_provider_create(dm_sa_sdk_provider **out,
                                        const dm_sa_sdk_owner *owner);
dm_sa_provider dm_sa_sdk_provider_interface(dm_sa_sdk_provider *provider);
dm_sa_result dm_sa_sdk_provider_destroy(dm_sa_sdk_provider *provider);

// Internal SVC ownership bridge. The scope is the existing MapMemory source
// lock; it does not establish SDK reservation ownership. Only the provider's
// exact successful-lease ledger does that. _locked helpers require the scope.
// This one scope protects borrowed sources, physical backing and accounting.
void dm_sa_svc_scope_begin(void);
void dm_sa_svc_scope_end(void);
int dm_sa_svc_alias_range_locked(uintptr_t base, size_t size);
int dm_sa_svc_backed_locked(uintptr_t base, size_t size);
int dm_sa_svc_available_locked(uintptr_t base, size_t size);
// Ownership-independent bitmap/borrow inspection; the provider validates its
// exact lease first. No payload access; clipped to the supplied byte interval.
dm_sa_result dm_sa_svc_page_run_locked(uintptr_t base, size_t maximum, dm_sa_page_run *out);
int dm_sa_svc_unborrowed_locked(uintptr_t base, size_t size);
size_t dm_sa_svc_physical_budget_locked(void);
// Thread-local, nesting-safe strict physical budget for ONLY an original owner
// allocation transaction. SVC applies the limit atomically at commit, including
// owner metadata growth; ordinary legacy unlimited-map callers are unchanged.
void dm_sa_svc_owner_allocation_begin(void);
void dm_sa_svc_owner_allocation_end(void);
