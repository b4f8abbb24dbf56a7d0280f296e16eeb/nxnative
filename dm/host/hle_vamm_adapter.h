// Complete synthetic Vamm dispatch group. No HLE registrations. Production
// installation is hard-disabled until all modules' call/inline paths are proven.
#pragma once
#include "hle_vamm.h"
#define DM_VAMM_SYMBOL_COUNT 18u
#define DM_VAMM_ALL_BINDINGS ((UINT64_C(1) << DM_VAMM_SYMBOL_COUNT) - 1)
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
    uint64_t installed_bindings;
    const dm_vamm_symbol *symbols; // complete original symbol census, aliases included
    size_t symbol_count;
    dm_vamm_config owner;
    void *context;
    void (*fatal)(void *context, const char *operation); // must not return
} dm_vamm_adapter_config;
// Startup only, before constructor/guest execution. Validate/preallocate all
// host state before publishing any hook. Failure returns with lookup()==NULL:
// loader retains original SDK for the WHOLE group, not a mixture of owners.
// DM_VAMM_ADAPTER_TESTING permits ONLY synthetic callback tests to opt in.
uint32_t dm_vamm_adapter_install(const dm_vamm_adapter_config *config);
const char *dm_vamm_adapter_symbol_name(size_t index);
dm_vamm_guest_fn dm_vamm_adapter_lookup(const char *name);
// No SDK shutdown export was observed. This host-only drain check is called
// with guest threads stopped, before unload; refuses to discard live ownership.
// Does not reactivate the SDK or permit owner changes after installation.
uint32_t dm_vamm_adapter_shutdown(void);
