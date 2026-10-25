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

enum { DM_SA_PAGE_COMMITTED = 1, DM_SA_PAGE_BORROWED = 2 };
typedef struct {
    size_t size; // nonzero prefix of the requested interval, in bytes
    unsigned flags; // DM_SA_PAGE_*; describes every byte in this prefix
} dm_sa_page_run;
typedef struct {
    // Logical commitment/decommit unit, NOT host protection-page alignment.
    // COMMITTED must be uniform throughout each granule of every lease;
    // BORROWED boundaries may be finer. Query prefixes may still be clipped.
    // Zero disables the entire optional capability. Native SVC uses 4 KiB
    // even on a 16 KiB host. Finer commitment states are unsupported.
    size_t granule;
    // Same context/scope as the lease provider. Query callbacks never suspend
    // that scope, dereference payload bytes, or infer backing from mprotect.
    // query_run returns a bounded committed/borrowed-state run inside the exact
    // lease. budget is physical commit headroom, NOT free reservation space.
    // Outputs remain unchanged on failure.
    dm_sa_result (*query_run)(void *context, const dm_sa_span *span,
        size_t offset, size_t maximum, dm_sa_page_run *out);
    dm_sa_result (*budget)(void *context, size_t *out);
    // The caller owns suballocation lifetimes: decommit only complete granules
    // containing no live bytes. The provider additionally rejects borrowed
    // bytes. Commit preserves already committed bytes, including borrowed ones.
    // Both operations retain the exact lease/token/reservation; a returning
    // failure preserves all pre-call pages/accounting. Idempotent operations
    // succeed. This callback may suspend the source guard like acquire: callers
    // MUST revalidate allocation and realloc-source availability on return.
    dm_sa_result (*change)(void *context, const dm_sa_span *span,
        size_t offset, size_t size, int commit);
} dm_sa_page_provider;

typedef struct {
    void *context;
    size_t granule; // power of two >= 16; acquire sizes are exact multiples
    // Lock order: allocator mutex -> provider ownership scope -> lower owner
    // locks. All callbacks below run inside this scope and must not re-enter
    // this allocator. A disjoint provider may use no-op begin/end callbacks.
    // MapMemory/source-borrow changes MUST use the same scope (no TOCTOU).
    // The core mutex remains held. acquire/release/query/pages.change may suspend
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
    // Nonzero only when the entire byte interval is owned by this lease, fully
    // backed and not borrowed/pinned by another consumer. Used before reuse,
    // free, resize and memcpy. Page-capable release separately checks ownership
    // and borrows so holes do not prevent teardown. Must not dereference
    // the interval. A provider must not infer access from host page protection.
    int (*available)(void *context, const dm_sa_span *span, size_t offset, size_t size);
    dm_sa_result (*query)(void *context, size_t alignment, dm_sa_provider_stats *out);
    // All-zero for whole-lease/malloc providers. Partial capabilities are
    // invalid. Statistics, lease draining and allocation reuse/growth consume
    // this capability. Successful shrink and live-span trim also best-effort
    // decommit complete unborrowed free granules (an optional native policy).
    dm_sa_page_provider pages;
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
    // backing_bytes is the exact outstanding lease extent/capacity charge,
    // including reserved holes; committed_bytes is actual logical commitment.
    size_t capacity, backing_bytes, allocated_bytes, allocation_count, span_count;
    // Page-capable configurations include budgeted recommit potential; these
    // are snapshots, not a promise against concurrent physical-budget changes.
    size_t total_free, largest_free; // backed free bytes + once-budgeted recommit/new leases
    size_t metadata_bytes;
    int closed;
    size_t committed_bytes;
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
// With optional page backing, reuse/growth stages metadata before recommit and
// revalidates both the candidate and old realloc source afterward. A postcommit
// borrow returns BUSY with unchanged output/live allocation; newly committed
// pages stay owned free backing, visible in committed_bytes, for safe retry.
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
// Shrink/realloc-shrink publish logical metadata first; optional page cleanup
// cannot turn that success into failure. Live/borrowed boundary granules stay
// committed. Free has no eager partial-page policy; dead spans use full release.
dm_sa_result dm_sa_shrink(dm_sa_allocator *allocator, void *pointer, size_t size, size_t *out);
dm_sa_result dm_sa_get_size(dm_sa_allocator *allocator, const void *pointer, size_t *out);
dm_sa_result dm_sa_get_stats(dm_sa_allocator *allocator, dm_sa_stats *out);
// No per-thread caches: trim releases any wholly free spans retained after a
// provider release failure and reports the first whole-span release error.
// Optional free-page reclamation in live spans is best effort; its errors are
// ignored. Live bytes and borrowed ranges are not disturbed.
dm_sa_result dm_sa_trim(dm_sa_allocator *allocator);
// Stable ascending-address enumeration while holding the allocator mutex.
// Callback: zero continues; nonzero stops. It must not re-enter this allocator
// or mutate provider ownership; an ABI adapter must verify its SDK convention.
typedef int (*dm_sa_walk_fn)(void *pointer, size_t size, void *context);
dm_sa_result dm_sa_walk(dm_sa_allocator *allocator, dm_sa_walk_fn callback, void *context, int *callback_result);
// Hash ascending pointer/usable-size pairs as two little-endian uint64_t words.
// This explicit core format does not assert unverified SDK ordering equivalence.
dm_sa_result dm_sa_get_hash(dm_sa_allocator *allocator, dm_sa_hash *out);
