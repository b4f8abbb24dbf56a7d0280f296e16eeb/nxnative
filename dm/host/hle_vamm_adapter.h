// Complete synthetic Vamm dispatch group. No HLE registrations. Production
// installation is hard-disabled until all modules' call/inline paths are proven.
#pragma once
#include "hle_vamm.h"
#define DM_VAMM_SYMBOL_COUNT 18u
typedef void (*dm_vamm_guest_fn)(void);
typedef struct { const char *name; dm_vamm_guest_fn function; } dm_vamm_symbol;
enum {
    DM_VAMM_PROOF_ABI = 1u << 0,
    DM_VAMM_PROOF_ALL_CALLS_AND_INLINE = 1u << 1,
    DM_VAMM_PROOF_BOOTSTRAP_ONLY_CONSTRUCTOR = 1u << 2,
    DM_VAMM_PROOF_NO_PRIOR_OWNER = 1u << 3,
    DM_VAMM_PROOF_TEARDOWN = 1u << 4,
    DM_VAMM_PROOF_BEHAVIOR = 1u << 5,
    DM_VAMM_PROOF_ALL = (1u << 6) - 1
};
typedef struct {
    unsigned proofs;
    const dm_vamm_symbol *symbols; // complete original symbol census, aliases included
    size_t symbol_count;
    dm_vamm_config owner;
    // Optional explicitly retained runtime-owned Vamm, instead of creating a
    // second owner from owner above. All three fields or none are required.
    // retain must verify an unused, empty exact-domain owner and keep it alive;
    // it runs outside lifecycle locks with preparation marked in-flight, so
    // reentry rejects. Callbacks are native only and must never run guest code.
    // Cancel/shutdown release the handle; the adapter never destroys it.
    dm_vamm *shared_owner;
    uint32_t (*retain_owner)(void *context, dm_vamm *owner);
    void (*release_owner)(void *context, dm_vamm *owner);
    void *context;
    void (*fatal)(void *context, const char *operation); // must not return
} dm_vamm_adapter_config;
// Prepare copies and validates the entire immutable method table, reserving one
// startup-only lifecycle lease. It does NOT publish lookup or run guest code.
// Production preparation is hard-disabled; proof flags are not evidence.
// Hooks obtained from a prepared table are for relocation only: calling one
// before publish aborts, including original-forwarding/constructor paths.
typedef struct dm_vamm_adapter_prepared dm_vamm_adapter_prepared;
uint32_t dm_vamm_adapter_prepare(const dm_vamm_adapter_config *config, dm_vamm_adapter_prepared **out);
dm_vamm_guest_fn dm_vamm_adapter_prepared_hook(const dm_vamm_adapter_prepared *prepared, size_t index);
// Exclusive startup transaction only. Publish is infallible for the outstanding
// lease (no allocation/callbacks). Cancel frees unowned state; neither accepts
// reuse of a consumed lease. Never restore SDK methods after publication.
void dm_vamm_adapter_publish(dm_vamm_adapter_prepared *prepared);
void dm_vamm_adapter_cancel(dm_vamm_adapter_prepared *prepared);
// Fixture convenience only. Production always rejects; loader uses prepare,
// journals actual relocations, then publishes. No asserted binding masks.
uint32_t dm_vamm_adapter_install(const dm_vamm_adapter_config *config);
const char *dm_vamm_adapter_symbol_name(size_t index);
dm_vamm_guest_fn dm_vamm_adapter_lookup(const char *name);
// No SDK shutdown export was observed. This host-only drain check is called
// with guest threads stopped, before unload; refuses to discard live ownership.
// Does not reactivate the SDK or permit owner changes after installation.
uint32_t dm_vamm_adapter_shutdown(void);
