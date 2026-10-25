// Experimental reservation-only owner. NOT a second owner alongside the SDK:
// callers must suppress every SDK reservation initializer and operation first.
// Physical pages remain owned/accounted by the external-mode SVC manager.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct dm_vamm dm_vamm;
typedef enum {
    DM_VAMM_OK = 0, DM_VAMM_INVALID = 0xe03, DM_VAMM_NO_MEMORY = 0x1003,
    DM_VAMM_NO_SPACE = 0x1803
} dm_vamm_result;
typedef struct { uint64_t total, used; } dm_vamm_resource_usage;
typedef struct {
    uintptr_t base;
    size_t size, max_regions;
    void *context;
    // Native callbacks only; no guest callbacks and no reentry. Called while
    // holding the owner lock: owner -> source guard -> physical -> page manager.
    // Return nn::os-domain results; failure MUST leave pages/accounting intact.
    uint32_t (*change_pages)(void *context, uintptr_t base, size_t size, int commit);
    // Raw kernel system-resource values (NOT alias reserved/committed bytes).
    dm_vamm_resource_usage (*resources)(void *context);
} dm_vamm_config;
typedef struct {
    size_t regions, reserved_bytes, metadata_bytes;
    // One maximal legal payload per current free interval, after guards and
    // size-dependent alignment. Sum is conservative if holes could be split.
    // Not a physical budget; provider must clamp separately under its guard.
    size_t available_bytes, largest_region;
} dm_vamm_stats;

// Exclusive lifecycle; no mmap, guest metadata, node-area reservation, or page
// changes at create/init. Real already-reserved domain required; size is exact.
uint32_t dm_vamm_create(dm_vamm **out, const dm_vamm_config *config);
uint32_t dm_vamm_destroy(dm_vamm *owner); // rejects any live reservation
void dm_vamm_initialize(dm_vamm *owner); // idempotent, no-op if resource total=0
int dm_vamm_is_enabled(dm_vamm *owner);
// Monotonic lifecycle observation for an explicitly held owner capability.
// Unlike is_enabled (raw system-resource availability), false means no init.
int dm_vamm_is_initialized(dm_vamm *owner);
dm_vamm_resource_usage dm_vamm_resources(dm_vamm *owner);
// SDK-style absolute alignment: 64 KiB below 2 MiB, 2 MiB otherwise; one
// 4 KiB guard on EACH side (neighbors may share their separating guard page).
// No rounding or smaller success.
uint32_t dm_vamm_allocate(dm_vamm *owner, uintptr_t *out, size_t size, int commit);
uint32_t dm_vamm_pages(dm_vamm *owner, uintptr_t base, size_t size, int commit);
// Decommit exact entire payload before removing its record. Any backend error
// (including borrowed-source rejection) retains reservation and page ownership.
uint32_t dm_vamm_free(dm_vamm *owner, uintptr_t base);
uint32_t dm_vamm_get_stats(dm_vamm *owner, size_t alignment, dm_vamm_stats *out);
