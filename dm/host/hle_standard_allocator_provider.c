// No parallel allocator over SDK-owned free alias space. All real leases go
// through the current owner's operations; metadata lives only on the host.
#include "hle_standard_allocator_provider.h"
#include <stdlib.h>

typedef struct sdk_lease {
    dm_sa_span span;
    int in_flight;
    struct sdk_lease *next;
} sdk_lease;
enum { LEASE_ACTIVE, LEASE_ACQUIRING, LEASE_RELEASING };
struct dm_sa_sdk_provider {
    dm_sa_sdk_owner owner;
    sdk_lease *leases;
    uintptr_t next_token;
};
static int power2(size_t value) { return value && !(value & (value - 1)); }
static size_t minimum(size_t a, size_t b) { return a < b ? a : b; }
static void begin(void *context) { (void)context; dm_sa_svc_scope_begin(); }
static void end(void *context) { (void)context; dm_sa_svc_scope_end(); }
static sdk_lease *find(dm_sa_sdk_provider *p, const dm_sa_span *span) {
    if (!span) return NULL;
    for (sdk_lease *l = p->leases; l; l = l->next)
        if (l->span.token == span->token && l->span.base == span->base &&
            l->span.size == span->size) return l;
    return NULL;
}
static void erase(dm_sa_sdk_provider *p, sdk_lease *lease) {
    sdk_lease **link = &p->leases;
    while (*link != lease) link = &(*link)->next;
    *link = lease->next; free(lease);
}
static int available(void *context, const dm_sa_span *span, size_t offset, size_t size) {
    dm_sa_sdk_provider *p = context;
    sdk_lease *l = find(p, span);
    if (!l || l->in_flight || !size || offset > span->size || size > span->size - offset)
        return 0;
    return dm_sa_svc_available_locked((uintptr_t)span->base + offset, size);
}
static dm_sa_result acquire(void *context, size_t size, size_t alignment, dm_sa_span *out) {
    dm_sa_sdk_provider *p = context;
    if (!out || !size || (size & 0xfff) || !power2(alignment) || alignment < 0x1000)
        return DM_SA_INVALID;
    if (!p->owner.allocate_memory_aligned && alignment > p->owner.guaranteed_alignment) return DM_SA_NO_MEMORY;
    if (p->next_token == UINTPTR_MAX) return DM_SA_NO_MEMORY;
    sdk_lease *l = calloc(1, sizeof *l);
    if (!l) return DM_SA_NO_MEMORY;
    l->span.size = size; l->span.token = ++p->next_token;
    l->in_flight = LEASE_ACQUIRING; l->next = p->leases; p->leases = l;
    // Both original SDK and native owners hold their Vamm lock and enter SVC
    // page transactions. Keeping the source guard would recursively deadlock.
    dm_sa_svc_scope_end();
    uintptr_t address = 0;
    int guest = p->owner.guest_context_active(p->owner.context);
    uint32_t result = 1;
    if (guest) {
        dm_sa_svc_owner_allocation_begin();
        result = p->owner.allocate_memory_aligned ?
            p->owner.allocate_memory_aligned(p->owner.context, &address, size, alignment) :
            p->owner.allocate_memory(p->owner.context, &address, size);
        dm_sa_svc_owner_allocation_end();
    }
    dm_sa_svc_scope_begin();
    if (result) { erase(p, l); return DM_SA_PROVIDER_FAILED; }
    l->span.base = (void *)address; l->in_flight = 0;
    if ((address & (alignment - 1)) || !dm_sa_svc_backed_locked(address, size)) abort();
    for (sdk_lease *other = p->leases; other; other = other->next) {
        uintptr_t base = (uintptr_t)other->span.base;
        // A successful original free can make its address reusable before its
        // callback regains this scope. Such a retiring record is unavailable to
        // its old core and is erased on return; it is not a second live lease.
        if (other != l && other->in_flight != LEASE_RELEASING && base &&
            address < base + other->span.size && base < address + size)
            abort();
    }
    // A borrow may already have raced the SDK return. The core MUST check
    // available before publication; it retains the entire lease on BUSY.
    *out = l->span;
    return DM_SA_OK;
}
static dm_sa_result release(void *context, const dm_sa_span *span) {
    dm_sa_sdk_provider *p = context;
    sdk_lease *l = find(p, span);
    if (!l) return DM_SA_NOT_FOUND;
    if (l->in_flight || !available(p, span, 0, span->size)) return DM_SA_BUSY;
    l->in_flight = LEASE_RELEASING;
    dm_sa_svc_scope_end();
    int guest = p->owner.guest_context_active(p->owner.context);
    uint32_t result = guest ? p->owner.free_address_region(p->owner.context,
                                                        (uintptr_t)span->base) : 1;
    dm_sa_svc_scope_begin();
    if (result) {
        // Original FreeAddressRegion must leave its reservation intact when
        // UnmapPhysicalMemory fails (including a newly borrowed source). Keep
        // the EXACT token/base/size, not a guessed replacement or partial free.
        l->in_flight = 0;
        return available(p, span, 0, span->size) ? DM_SA_PROVIDER_FAILED : DM_SA_BUSY;
    }
    erase(p, l);
    return DM_SA_OK;
}
static dm_sa_result query(void *context, size_t alignment, dm_sa_provider_stats *out) {
    dm_sa_sdk_provider *p = context;
    if (!out || !power2(alignment) || alignment < 0x1000) return DM_SA_INVALID;
    if (!p->owner.allocate_memory_aligned && alignment > p->owner.guaranteed_alignment) {
        *out = (dm_sa_provider_stats){0, 0}; return DM_SA_OK;
    }
    dm_sa_provider_stats owner;
    dm_sa_svc_scope_end();
    dm_sa_result result = p->owner.guest_context_active(p->owner.context) ?
        p->owner.query_reservations(p->owner.context, alignment, &owner) : DM_SA_PROVIDER_FAILED;
    dm_sa_svc_scope_begin();
    if (result != DM_SA_OK) return result;
    // Reservations may change after the owner snapshot. This is a bounded
    // query, not a promise: acquire still performs the real owner transaction.
    // Never report the caller's multi-GiB requested capacity as physical RAM.
    size_t budget = dm_sa_svc_physical_budget_locked();
    size_t bytes = minimum(owner.available_bytes, budget) & ~(size_t)0xfff;
    *out = (dm_sa_provider_stats){bytes, minimum(owner.largest_span, bytes) & ~(size_t)0xfff};
    return DM_SA_OK;
}
dm_sa_result dm_sa_sdk_provider_create(dm_sa_sdk_provider **out, const dm_sa_sdk_owner *owner) {
    if (!out || !owner || !owner->allocate_memory || !owner->free_address_region ||
        !owner->query_reservations || !owner->guest_context_active ||
        !power2(owner->guaranteed_alignment) || owner->guaranteed_alignment < 0x1000)
        return DM_SA_INVALID;
    dm_sa_sdk_provider *p = calloc(1, sizeof *p);
    if (!p) return DM_SA_NO_MEMORY;
    p->owner = *owner; *out = p; return DM_SA_OK;
}
dm_sa_provider dm_sa_sdk_provider_interface(dm_sa_sdk_provider *p) {
    if (!p) return (dm_sa_provider){0};
    return (dm_sa_provider){p, 0x1000, begin, end, acquire, release, available, query};
}
dm_sa_result dm_sa_sdk_provider_destroy(dm_sa_sdk_provider *p) {
    if (!p) return DM_SA_INVALID;
    dm_sa_svc_scope_begin();
    int busy = p->leases != NULL;
    dm_sa_svc_scope_end();
    if (busy) return DM_SA_BUSY;
    free(p); return DM_SA_OK;
}
