// Host-owned native address regions. This is NOT the nn::os ABI: no guest hooks
// may use it until all SDK reservation paths have been proven to share ownership.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct dm_address_space dm_address_space;
typedef enum {
    DM_AS_OK, DM_AS_INVALID, DM_AS_NO_SPACE, DM_AS_METADATA_FULL,
    DM_AS_NOT_FOUND, DM_AS_BUSY, DM_AS_LIMIT, DM_AS_BACKEND_FAILED,
    DM_AS_WRONG_MODE, DM_AS_NO_MEMORY
} dm_as_result;
typedef enum {
    // The guest SDK still owns reservations. Track its SVC commits, but reject
    // host reserve/release: two allocators must never share the same free space.
    DM_AS_EXTERNAL_RESERVATIONS,
    // Only the host owns reservations. Page operations require containment in
    // one live reservation; a future verified SDK adapter can use this mode.
    DM_AS_HOST_RESERVATIONS
} dm_as_mode;

// Called under the manager lock, on host-page aligned ranges in the registered
// native block. Return 0 on success; failure MUST leave host accessibility
// unchanged. Must not re-enter this manager. No callback for a zero-length run.
typedef int (*dm_as_apply_fn)(void *context, uintptr_t base, size_t size, int commit);
// Optional, non-failing initialization of ONLY newly committed logical slices,
// after successful accessibility change and before publishing accounting. Must
// not allocate, re-enter, or change protections; existing live bytes are excluded.
typedef void (*dm_as_initialize_fn)(void *context, uintptr_t base, size_t size);
typedef struct {
    uintptr_t base;
    size_t size, guest_page_size, host_page_size, max_regions;
    dm_as_mode mode;
    dm_as_apply_fn apply;
    void *context;
    dm_as_initialize_fn initialize;
    // Fresh anonymous native reservation: first exposure of a host page is
    // already zero-filled by the OS, so skip eager initialization then only.
    // Any later exposure initializes new logical slices, even slices never
    // individually committed before. Requires initialize; not for reused maps.
    int initially_zero;
} dm_as_config;

typedef struct {
    uintptr_t base;
    size_t size;
    uintptr_t reservation_base;
    size_t reservation_size;
    int reserved, committed;
} dm_as_range;
typedef struct {
    uintptr_t base;
    size_t size, reserved_bytes, committed_bytes, free_bytes, largest_free;
    size_t regions, max_regions, metadata_bytes;
    dm_as_mode mode;
} dm_as_stats;

// Register an already reserved native block: never mmap, invent addresses,
// resize it, or charge guest memory for metadata. Sizes/bases must be aligned
// to host pages; guest pages divide host pages. All output arguments stay
// unchanged on failure. Initialization/destruction require exclusive ownership.
dm_as_result dm_as_create(dm_address_space **out, const dm_as_config *config);
dm_as_result dm_as_destroy(dm_address_space *space); // BUSY if owned/mapped
// Exact size, absolute power-of-two alignment >= guest page size; no rounding,
// silent size reduction, SDK-specific guards or guessed alignment policy.
dm_as_result dm_as_reserve(dm_address_space *space, size_t size, size_t alignment, uintptr_t *out);
// Release an exact reservation only after decommit. BUSY preserves ownership.
dm_as_result dm_as_release(dm_address_space *space, uintptr_t base, size_t size);
// Idempotent page accounting, including partially overlapping commits/unmaps.
// committed_limit bounds total committed bytes, not reserved bytes (SIZE_MAX
// means no limit). Backend errors preserve bitmap, counters and reservations.
dm_as_result dm_as_commit(dm_address_space *space, uintptr_t base, size_t size, size_t committed_limit);
dm_as_result dm_as_decommit(dm_address_space *space, uintptr_t base, size_t size);
// Maximal homogeneous run, clipped to the reservation boundary in host mode
// and to the registered block in external mode. Does not expose metadata.
dm_as_result dm_as_query(dm_address_space *space, uintptr_t address, dm_as_range *out);
dm_as_result dm_as_committed_in(dm_address_space *space, uintptr_t base, size_t size, size_t *out);
dm_as_result dm_as_get_stats(dm_address_space *space, dm_as_stats *out);
