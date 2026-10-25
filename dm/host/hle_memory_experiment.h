// Bounded, opt-in startup policy. This is not general SDK compatibility proof.
#pragma once
#include "dm.h"

#define DM_MEMORY_EXPERIMENT_NAME "za-base-v1"
#define DM_SDK_MEMORY_EXPERIMENT_NAME "za-base-sdk-v1"
enum { DM_MEMORY_EXPERIMENT_FAILED = -1, DM_MEMORY_EXPERIMENT_OFF = 0,
       DM_MEMORY_EXPERIMENT_ACTIVE = 1 };
// Pure eligibility check over the COMPLETE mapped set, including deferred NROs.
// NULL means eligible; otherwise a static diagnostic. No guest calls or writes.
const char *dm_memory_experiment_preflight(const module_t *modules, size_t count);
// Called once by load_modules after opening the transaction window and before
// sealing HLECOUNT/guest entry. NULL, empty or "0" requests the production path.
// Failure is fatal to an explicitly requested experiment, never SDK fallback.
int dm_memory_experiment_startup(const char *request, const char **diagnostic);

// Opaque, short-lived native startup authorization, NOT asserted proof bits.
// Only the policy above can issue it, and only during its loader transaction.
// This first predicate authorizes the full-group mode only, never SDK-only.
struct dm_memory_experiment_authorization;
int dm_memory_experiment_authorized(const struct dm_memory_experiment_authorization *,
                                    const void *runtime_context);
// Separate authority: only Vamm may use this permit. In particular it never
// authorizes StandardAllocator preparation or the complete 40-hook group.
int dm_memory_experiment_authorized_sdk_only(const struct dm_memory_experiment_authorization *,
                                             const void *runtime_context);
