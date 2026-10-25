// Instance-owned allocator core, NOT a guest ABI or a replacement reservation
// owner. A verified adapter must supply leases from the current memory owner.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct dm_sa_allocator dm_sa_allocator;
typedef enum {
    DM_SA_OK, DM_SA_INVALID, DM_SA_NO_MEMORY, DM_SA_NOT_FOUND,
    DM_SA_BUSY, DM_SA_PROVIDER_FAILED, DM_SA_CLOSED
} dm_sa_result;

typedef struct {
    void *base;
    size_t size;
    uintptr_t token; // opaque to the core; identifies the owner's exact lease
} dm_sa_span;
typedef struct {
    size_t available_bytes; // physically budgeted, currently leasable backing
    size_t largest_span;    // largest contiguous lease at the requested alignment
} dm_sa_provider_stats;

typedef struct {
    void *context;
    size_t granule; // power of two >= 16; acquire sizes are exact multiples
    // Lock order: allocator mutex -> provider ownership scope -> lower owner
    // locks. All callbacks below run inside this scope and must not re-enter
    // this allocator. A disjoint provider may use no-op begin/end callbacks.
    // MapMemory/source-borrow changes MUST use the same scope (no TOCTOU).
    // The core mutex remains held. acquire/release/query may temporarily suspend
    // their external source guard to call the current owner (whose SVC takes
    // that guard), but must restore it before returning and serialize lease
    // metadata, e.g. with protected in-flight states. Do not hold a provider lock
    // across owner calls unless their entire closure and reverse ordering are
    // proven. Owner calls must not re-enter this allocator. Other callbacks keep
    // the source guard held throughout. The core revalidates a new lease before
    // publication and an old realloc source after acquisition/query callbacks;
    // release failure keeps ownership.
    void (*begin)(void *context);
    void (*end)(void *context);
    // Lease real, contiguous, writable memory, exactly size bytes with absolute
    // power-of-two alignment. Success transfers exclusive suballocation rights
    // to the core. Failure transfers no payload lease and leaves core accounting
    // and *out unchanged. The current owner may grow its own internal metadata
    // during a failed acquisition; that backing must remain owned/accounted by
    // the current owner and be reflected in subsequent provider budget queries.
    dm_sa_result (*acquire)(void *context, size_t size, size_t alignment, dm_sa_span *out);
    // Release the exact lease. Failure MUST leave backing and ownership intact.
    dm_sa_result (*release)(void *context, const dm_sa_span *span);
    // Nonzero only when the entire byte interval is owned by this lease and is
    // not borrowed/pinned by another consumer. Used before reuse, free, resize,
    // memcpy and release, not just before page decommit. Must not dereference
    // the interval. A provider must not infer access from host page protection.
    int (*available)(void *context, const dm_sa_span *span, size_t offset, size_t size);
    dm_sa_result (*query)(void *context, size_t alignment, dm_sa_provider_stats *out);
} dm_sa_provider;

typedef struct {
    dm_sa_provider provider;
    size_t capacity; // upper bound on actual leased bytes, NOT a reservation
    size_t pool_size; // 0 = 64 KiB, rounded up to provider granule
    // Optional non-reentrant host-metadata allocation pair, useful for failure
    // injection. Neither metadata nor the allocator object lives in guest RAM.
    void *metadata_context;
    void *(*metadata_alloc)(void *context, size_t size);
    void (*metadata_free)(void *context, void *pointer);
} dm_sa_config;

typedef struct {
    size_t capacity, backing_bytes, allocated_bytes, allocation_count, span_count;
    size_t total_free, largest_free; // backed free runs + real provider budget
    size_t metadata_bytes;
    int closed;
} dm_sa_stats;
typedef struct {
    uint64_t block_count, allocated_bytes;
    unsigned char digest[8]; // first eight bytes of SHA-1; NOT allocation contents
} dm_sa_hash;

// Creation performs no backing acquisition, including for 12-62 GiB capacities.
// Output parameters remain unchanged on failure, except walk's callback_result
// when a callback deliberately terminates enumeration. If a newly acquired
// lease is borrowed during acquisition, Allocate returns BUSY without exposing
// a pointer and retains that exact lease as free-but-guarded backing for retry.
// This unavoidable rollback deferral is visible in backing_bytes, not leaked
// ownership. trim/finalize release it once the source borrow ends.
// Lifecycle destruction
// requires exclusive caller ownership; ordinary operations are thread-safe.
dm_sa_result dm_sa_create(dm_sa_allocator **out, const dm_sa_config *config);
// Finalize invalidates all allocation lifetimes only after every lease passes
// the borrow guard. BUSY before that point leaves the allocator usable. Once
// draining starts it is closed: release failures retain exact leases for retry,
// never publish them as free or forget them. Destroy retries finalize and frees
// metadata only on complete success. No operation implicitly falls back to SDK.
dm_sa_result dm_sa_finalize(dm_sa_allocator *allocator);
dm_sa_result dm_sa_destroy(dm_sa_allocator *allocator);
// Zero-byte allocation obtains a unique minimum 16-byte block. Alignment must
// be a power of two >= 16. Usable size rounds to 16; pointer must be the exact
// start of this instance's live allocation (foreign/interior pointers fail).
dm_sa_result dm_sa_allocate(dm_sa_allocator *allocator, size_t size, size_t alignment, void **out);
dm_sa_result dm_sa_free(dm_sa_allocator *allocator, void *pointer); // NULL is OK
dm_sa_result dm_sa_reallocate(dm_sa_allocator *allocator, void *pointer, size_t size, void **out);
dm_sa_result dm_sa_shrink(dm_sa_allocator *allocator, void *pointer, size_t size, size_t *out);
dm_sa_result dm_sa_get_size(dm_sa_allocator *allocator, const void *pointer, size_t *out);
dm_sa_result dm_sa_get_stats(dm_sa_allocator *allocator, dm_sa_stats *out);
// No per-thread caches: trim releases any wholly free spans retained after a
// provider release failure. Live bytes and borrowed ranges are not disturbed.
dm_sa_result dm_sa_trim(dm_sa_allocator *allocator);
// Stable ascending-address enumeration while holding the allocator mutex.
// Callback: zero continues; nonzero stops. It must not re-enter this allocator
// or mutate provider ownership; an ABI adapter must verify its SDK convention.
typedef int (*dm_sa_walk_fn)(void *pointer, size_t size, void *context);
dm_sa_result dm_sa_walk(dm_sa_allocator *allocator, dm_sa_walk_fn callback, void *context, int *callback_result);
// Hash ascending pointer/usable-size pairs as two little-endian uint64_t words.
// This explicit core format does not assert unverified SDK ordering equivalence.
dm_sa_result dm_sa_get_hash(dm_sa_allocator *allocator, dm_sa_hash *out);
