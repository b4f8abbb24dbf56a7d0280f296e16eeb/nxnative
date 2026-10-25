// Experimental all-method StandardAllocator boundary. No HLE_FORCE registrations:
// a loader must prove and install the WHOLE method group before managed init.
#pragma once
#include "standard_allocator_core.h"

#define DM_SA_ADAPTER_SYMBOL_COUNT 22u
#define DM_SA_ADAPTER_OBJECT_SIZE 56u

typedef void (*dm_sa_guest_fn)(void);
typedef struct { const char *name; dm_sa_guest_fn function; } dm_sa_adapter_symbol;
typedef enum {
    DM_SA_PROOF_ABI = 1u << 0,
    DM_SA_PROOF_ALL_MODULE_CALLS = 1u << 1,
    DM_SA_PROOF_NO_INLINE_ACCESS = 1u << 2,
    DM_SA_PROOF_CURRENT_MEMORY_OWNER = 1u << 3,
    DM_SA_PROOF_BEHAVIOR = 1u << 4,
    DM_SA_PROOF_ALL = (1u << 5) - 1
} dm_sa_adapter_proof;

typedef struct {
    unsigned proofs;
    // Complete class-symbol census, including aliases and any destructor. Extra,
    // duplicate and missing symbols reject the group, never partial takeover.
    const dm_sa_adapter_symbol *symbols;
    size_t symbol_count;
    void *context;
    // Optional paired native lifetime callbacks for the table's context. A
    // successful prepare retains once, cancel or stopped shutdown releases once.
    // Callback reentry rejects while preparation/cancellation is in flight.
    dm_sa_result (*retain_context)(void *context);
    void (*release_context)(void *context);
    // Required original Allocate(size) shim, preserving hle_os.c's established
    // alloc_big_align behavior for SDK-owned objects. It must not be an adapter
    // hook; the existing shim's managed-only bridge avoids recursive dispatch.
    void *(*forward_allocate)(void *self, size_t size);
    // Native policy for managed Allocate(size) only (normally 16). The host
    // supplies its existing alloc_big_align policy; no game rules live here.
    // Thread-safe native policy only: must not reenter this adapter or change
    // instance lifetime. Runs without registry/core locks, with an in-flight
    // instance reference. Invalid or unrepresentable alignment
    // makes that allocation fail, never fall through to the SDK.
    size_t (*allocation_alignment)(void *self, size_t size);
    // Runs without the registry lock. Other instances may be used; reentry or
    // concurrent lifecycle calls on this initializing self fail with BUSY.
    dm_sa_result (*make_config)(void *context, void *self, size_t capacity,
                               unsigned thread_cache, dm_sa_config *out);
    // Optional lifecycle pair for a successful make_config. The adapter calls
    // this exactly once after a later create/validation failure, or ONLY after
    // successful core destruction. A failed core drain retains the config.
    // Runs outside registry locks while same-instance reentry remains BUSY.
    // make_config failure cleans its own partial resources; release must not
    // fail or reenter this instance (it may operate on unrelated instances).
    void (*release_config)(void *context, void *self, const dm_sa_config *config);
    // Called only for managed Dump; does not inspect the SDK's embedded heap.
    void (*dump)(void *context, const void *self, const dm_sa_stats *stats);
    // Fatal guest contracts (Initialize/Free/Finalize/query errors) never fall
    // through to SDK ownership. The callback must not return; abort follows if
    // it does. Allocation exhaustion instead returns NULL, Shrink failure 0.
    void (*fatal)(void *context, const char *operation, dm_sa_result result);
} dm_sa_adapter_config;

// Prepare copies and validates the entire immutable method table, reserving one
// startup-only lifecycle lease. It does NOT publish lookup or run guest code.
// Production preparation is hard-disabled; proof flags are not evidence.
// Hooks obtained from a prepared table are for relocation only: calling one
// before publish aborts, including original-forwarding/constructor paths.
typedef struct dm_sa_adapter_prepared dm_sa_adapter_prepared;
dm_sa_result dm_sa_adapter_prepare(const dm_sa_adapter_config *config, dm_sa_adapter_prepared **out);
dm_sa_guest_fn dm_sa_adapter_prepared_hook(const dm_sa_adapter_prepared *prepared, size_t index);
// Exclusive startup transaction only. Publish is infallible for the outstanding
// lease (no allocation/callbacks). Cancel frees unowned state; neither accepts
// reuse of a consumed lease. Never restore SDK methods after publication.
void dm_sa_adapter_publish(dm_sa_adapter_prepared *prepared);
void dm_sa_adapter_cancel(dm_sa_adapter_prepared *prepared);
// Fixture convenience only. Production always rejects; loader uses prepare,
// journals actual relocations, then publishes. No asserted binding masks.
dm_sa_result dm_sa_adapter_install(const dm_sa_adapter_config *config);
// Exclusive host teardown with guest execution stopped. Rejects live cores or
// in-flight instance work, keeps lookup intercepted, and prevents every hook
// from using the released context. No reset or reactivation is permitted.
dm_sa_result dm_sa_adapter_shutdown(void);
const char *dm_sa_adapter_symbol_name(size_t index);
dm_sa_guest_fn dm_sa_adapter_lookup(const char *name); // NULL while inactive
int dm_sa_adapter_is_managed(const void *self); // includes finalized tombstones
// hle_os.c's existing Allocate(size) owner uses this fast bridge. A managed
// failure returns 1 and NULL; it MUST NOT escape to the original SDK allocator.
int dm_sa_adapter_allocate_if_managed(void *self, size_t size, size_t alignment, void **out);
// hle_os.c's native policy, shared by its forced entry and future installation.
size_t dm_sa_host_allocation_alignment(void *self, size_t size);
