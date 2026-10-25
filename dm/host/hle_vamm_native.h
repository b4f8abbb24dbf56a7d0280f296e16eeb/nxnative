// Native backing for the experimental Vamm owner. This only prepares a
// configuration: it does not install hooks, transfer ownership or enable gates.
#pragma once
#include "hle_vamm.h"

// Exact GetInfo domain: base = type 2, size = type 3 minus type 28. Validate
// every raw boundary before subtraction/narrowing; failure preserves outputs.
uint32_t dm_vamm_native_decode_domain(uint64_t alias_base, uint64_t alias_size,
                                      uint64_t extra_size, uintptr_t *base, size_t *size);
// Query the real SVC domain and provide native page/raw-resource callbacks.
// The caller still must prove exclusive reservation ownership before create.
uint32_t dm_vamm_native_configure(dm_vamm_config *out, size_t max_regions);

// Operation-specific nn::os translation. Unknown results abort, never become
// success or a fabricated recoverable failure. commit must be exactly 0 or 1.
uint32_t dm_vamm_native_svc_result(uint32_t result, int commit);

// Internal SVC boundary shared with the public dispatcher. No guest callbacks.
// change_pages acquires upstream g_map_lock itself; never call it while
// holding that source/backing/accounting scope. Host protection failure is fatal.
uint32_t dm_vamm_svc_change_pages(uintptr_t base, size_t size, int commit);
// Only GetInfo types 2, 3, 28, 16 and 17 are accepted; resource values are raw.
uint64_t dm_vamm_svc_info(unsigned id);
