// Host-only metadata over real owner-supplied leases. This file never reserves
// alias space, calls an SDK entry point, or installs a guest hook.
#include "standard_allocator_core.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct block {
    size_t offset, size, alignment;
    int live;
    struct block *prev, *next;
} block;
typedef struct span {
    dm_sa_span lease;
    block *blocks;
    size_t live_count, free_bytes;
    struct span *next;
} span;
struct dm_sa_allocator {
    pthread_mutex_t mutex;
    dm_sa_config config;
    span *spans;
    size_t backing, used, count, span_count, metadata;
    int closed;
};

static int power2(size_t n) { return n && !(n & (n - 1)); }
static size_t lesser(size_t a, size_t b) { return a < b ? a : b; }
static int rounded(size_t n, size_t a, size_t *out) {
    if (n > SIZE_MAX - (a - 1)) return 0;
    *out = (n + a - 1) & ~(a - 1); return 1;
}
static void *default_alloc(void *context, size_t n) { (void)context; return malloc(n); }
static void default_free(void *context, void *p) { (void)context; free(p); }
static void *metadata_new(dm_sa_allocator *a, size_t size) {
    if (size > SIZE_MAX - a->metadata) return NULL;
    void *p = a->config.metadata_alloc(a->config.metadata_context, size);
    if (p) { memset(p, 0, size); a->metadata += size; }
    return p;
}
static void metadata_delete(dm_sa_allocator *a, void *p, size_t size) {
    a->metadata -= size; a->config.metadata_free(a->config.metadata_context, p);
}
static void enter(dm_sa_allocator *a) {
    pthread_mutex_lock(&a->mutex); a->config.provider.begin(a->config.provider.context);
}
static void leave(dm_sa_allocator *a) {
    a->config.provider.end(a->config.provider.context); pthread_mutex_unlock(&a->mutex);
}
static int available(dm_sa_allocator *a, const span *s, size_t offset, size_t size) {
    return a->config.provider.available(a->config.provider.context, &s->lease, offset, size);
}
static dm_sa_result raw_page_run(dm_sa_allocator *a, const span *s, size_t offset,
                              size_t maximum, dm_sa_page_run *out) {
    const dm_sa_provider *p = &a->config.provider;
    dm_sa_page_run run;
    dm_sa_result r = p->pages.query_run(p->context, &s->lease, offset, maximum, &run);
    if (r != DM_SA_OK) return r;
    if (!run.size || run.size > maximum ||
        (run.flags & ~(DM_SA_PAGE_COMMITTED | DM_SA_PAGE_BORROWED))) return DM_SA_PROVIDER_FAILED;
    *out = run; return DM_SA_OK;
}
static dm_sa_result page_run(dm_sa_allocator *a, const span *s, size_t offset,
                              size_t maximum, dm_sa_page_run *out) {
    dm_sa_page_run run;
    dm_sa_result r = raw_page_run(a, s, offset, maximum, &run);
    if (r != DM_SA_OK) return r;
    // Providers may return short prefixes. Merge identical adjacent states so
    // a complete free hole is charged in granules, not arbitrary query chunks.
    while (run.size < maximum) {
        dm_sa_page_run next;
        r = raw_page_run(a, s, offset + run.size, maximum - run.size, &next);
        if (r != DM_SA_OK) return r;
        if (next.flags != run.flags) break;
        run.size += next.size;
    }
    *out = run; return DM_SA_OK;
}
static dm_sa_result releasable(dm_sa_allocator *a, const span *s) {
    if (!a->config.provider.pages.granule)
        return available(a, s, 0, s->lease.size) ? DM_SA_OK : DM_SA_BUSY;
    for (size_t offset = 0; offset < s->lease.size;) {
        dm_sa_page_run run;
        dm_sa_result r = page_run(a, s, offset, s->lease.size - offset, &run);
        if (r != DM_SA_OK) return r;
        if (run.flags & DM_SA_PAGE_BORROWED) return DM_SA_BUSY;
        offset += run.size;
    }
    return DM_SA_OK;
}
// Prepare backing only after all fallible allocation metadata is staged. Page
// callbacks may drop the source guard: success is not publication permission.
// A postcommit borrow retains owned pages and returns BUSY without rolling back
// a rounded interval that may also contain pre-existing live boundary bytes.
static dm_sa_result prepare_backing(dm_sa_allocator *a, span *s, size_t offset, size_t size) {
    if (available(a, s, offset, size)) return DM_SA_OK;
    const dm_sa_provider *p = &a->config.provider;
    if (!p->pages.granule) return DM_SA_BUSY;
    size_t end = offset + size;
    for (size_t at = offset; at < end;) {
        dm_sa_page_run run;
        dm_sa_result r = page_run(a, s, at, end - at, &run);
        if (r != DM_SA_OK) return r;
        if (run.flags & DM_SA_PAGE_BORROWED) return DM_SA_BUSY;
        at += run.size;
    }
    size_t granule = p->pages.granule, lo = offset & ~(granule - 1), hi;
    if (!rounded(end, granule, &hi) || hi > s->lease.size) return DM_SA_INVALID;
    size_t missing = 0;
    for (size_t at = lo; at < hi;) {
        dm_sa_page_run run;
        dm_sa_result r = page_run(a, s, at, hi - at, &run);
        if (r != DM_SA_OK) return r;
        if (!(run.flags & DM_SA_PAGE_COMMITTED)) {
            // This scan covers a granule-rounded interval, so a partial hole
            // violates the provider's uniform logical commitment contract.
            // Finer BORROWED boundaries on committed neighbors are valid.
            if ((at | run.size) & (granule - 1)) return DM_SA_PROVIDER_FAILED;
            missing += run.size;
        }
        at += run.size;
    }
    if (missing) {
        size_t budget;
        dm_sa_result r = p->pages.budget(p->context, &budget);
        if (r != DM_SA_OK) return r;
        if (missing > budget) return DM_SA_NO_MEMORY;
        r = p->pages.change(p->context, &s->lease, lo, hi - lo, 1);
        if (r != DM_SA_OK) return r;
    }
    return available(a, s, offset, size) ? DM_SA_OK : DM_SA_BUSY;
}
static void *address(const span *s, const block *b) {
    return (void *)((uintptr_t)s->lease.base + b->offset);
}
static block *find_block(dm_sa_allocator *a, const void *p, span **owner) {
    uintptr_t v = (uintptr_t)p;
    for (span *s = a->spans; s; s = s->next) {
        uintptr_t base = (uintptr_t)s->lease.base;
        if (v < base) break;
        if (v - base >= s->lease.size) continue;
        for (block *b = s->blocks; b; b = b->next)
            if (b->live && b->offset == v - base) { *owner = s; return b; }
        break;
    }
    return NULL;
}
static void insert_after(block *b, block *n) {
    n->prev = b; n->next = b->next;
    if (n->next) n->next->prev = n;
    b->next = n;
}
static block *coalesce(dm_sa_allocator *a, block *b) {
    if (b->prev && !b->prev->live) {
        block *previous = b->prev;
        previous->size += b->size; previous->next = b->next;
        if (b->next) b->next->prev = previous;
        metadata_delete(a, b, sizeof *b); b = previous;
    }
    if (b->next && !b->next->live) {
        block *next = b->next;
        b->size += next->size; b->next = next->next;
        if (next->next) next->next->prev = b;
        metadata_delete(a, next, sizeof *next);
    }
    return b;
}
static void delete_span_metadata(dm_sa_allocator *a, span *s) {
    for (block *b = s->blocks, *next; b; b = next) {
        next = b->next; metadata_delete(a, b, sizeof *b);
    }
    metadata_delete(a, s, sizeof *s);
}
static dm_sa_result release_span(dm_sa_allocator *a, span *s) {
    dm_sa_result ready = releasable(a, s);
    if (ready != DM_SA_OK) return ready;
    dm_sa_result r = a->config.provider.release(a->config.provider.context, &s->lease);
    if (r != DM_SA_OK) return r;
    span **link = &a->spans;
    while (*link != s) link = &(*link)->next;
    *link = s->next; a->backing -= s->lease.size; a->span_count--;
    delete_span_metadata(a, s); return DM_SA_OK;
}

dm_sa_result dm_sa_create(dm_sa_allocator **out, const dm_sa_config *config) {
    if (!out || !config) return DM_SA_INVALID;
    const dm_sa_provider *p = &config->provider;
    const dm_sa_page_provider *pages = &p->pages;
    if (!power2(p->granule) || p->granule < 16 || !config->capacity ||
        !p->begin || !p->end || !p->acquire || !p->release || !p->available || !p->query ||
        (!!config->metadata_alloc != !!config->metadata_free)) return DM_SA_INVALID;
    if (pages->granule || pages->query_run || pages->budget || pages->change) {
        if (!power2(pages->granule) || pages->granule < 16 ||
            pages->granule > p->granule || !pages->query_run || !pages->budget ||
            !pages->change) return DM_SA_INVALID;
    }
    size_t pool;
    if (!rounded(config->pool_size ? config->pool_size : 65536, p->granule, &pool)) return DM_SA_INVALID;
    dm_sa_config c = *config;
    c.pool_size = pool;
    if (!c.metadata_alloc) { c.metadata_alloc = default_alloc; c.metadata_free = default_free; }
    dm_sa_allocator *a = c.metadata_alloc(c.metadata_context, sizeof *a);
    if (!a) return DM_SA_NO_MEMORY;
    memset(a, 0, sizeof *a); a->config = c; a->metadata = sizeof *a;
    if (pthread_mutex_init(&a->mutex, NULL)) {
        c.metadata_free(c.metadata_context, a); return DM_SA_NO_MEMORY;
    }
    *out = a; return DM_SA_OK;
}

// Split free run only after all required host metadata exists. No writes to
// guest bytes, provider ownership, or allocation accounting occur on failure.
static dm_sa_result take_free(dm_sa_allocator *a, span *s, block *b,
                              size_t offset, size_t size, size_t alignment, void **out) {
    size_t before = offset - b->offset, after = b->size - before - size;
    block *allocated = before ? metadata_new(a, sizeof *allocated) : b;
    if (!allocated) return DM_SA_NO_MEMORY;
    block *tail = after ? metadata_new(a, sizeof *tail) : NULL;
    if (after && !tail) {
        if (before) metadata_delete(a, allocated, sizeof *allocated);
        return DM_SA_NO_MEMORY;
    }
    dm_sa_result prepared = a->config.provider.pages.granule ?
        prepare_backing(a, s, offset, size) : DM_SA_OK;
    if (prepared != DM_SA_OK) {
        if (tail) metadata_delete(a, tail, sizeof *tail);
        if (before) metadata_delete(a, allocated, sizeof *allocated);
        return prepared;
    }
    if (before) { b->size = before; insert_after(b, allocated); }
    allocated->offset = offset; allocated->size = size;
    allocated->alignment = alignment; allocated->live = 1;
    if (tail) { tail->offset = offset + size; tail->size = after; insert_after(allocated, tail); }
    a->used += size; a->count++; s->live_count++; s->free_bytes -= size;
    *out = address(s, allocated); return DM_SA_OK;
}
static int window_candidate(const span *s, size_t left, size_t right,
                              size_t size, size_t alignment, size_t *out) {
    uintptr_t base = (uintptr_t)s->lease.base + left;
    if (base > UINTPTR_MAX - (alignment - 1)) return 0;
    size_t skip = (size_t)((-(uintptr_t)base) & (alignment - 1));
    if (skip > right - left || size > right - left - skip) return 0;
    *out = left + skip; return 1;
}
// Find an aligned subinterval, not merely the first address of a mixed run.
// Budget zero is the backed-first pass. Missing-page steps preserve alignment
// opportunities while the left cursor advances; cached left runs avoid scanning
// the same bitmap suffix for each step. No payload or metadata is changed here.
static dm_sa_result sparse_candidate(dm_sa_allocator *a, span *s, block *b,
                                       size_t size, size_t alignment, size_t budget, size_t *out) {
    size_t left = b->offset, right = left, limit = left + b->size, cost = 0;
    size_t left_end = left, granule = a->config.provider.pages.granule;
    unsigned left_flags = 0;
    while (right < limit) {
        dm_sa_page_run run;
        dm_sa_result r = page_run(a, s, right, limit - right, &run);
        if (r != DM_SA_OK) return r;
        size_t end = right + run.size;
        if (run.flags & DM_SA_PAGE_BORROWED) {
            left = right = left_end = end; cost = 0; continue;
        }
        if (run.flags & DM_SA_PAGE_COMMITTED) {
            right = end;
            if (window_candidate(s, left, right, size, alignment, out)) return DM_SA_OK;
            continue;
        }
        if ((right | run.size) & (granule - 1)) return DM_SA_PROVIDER_FAILED;
        if (!budget) { left = right = left_end = end; cost = 0; continue; }
        while (right < end) {
            right += granule; cost += granule;
            while (cost > budget) {
                if (left == left_end) {
                    dm_sa_page_run old;
                    r = page_run(a, s, left, limit - left, &old);
                    if (r != DM_SA_OK) return r;
                    left_end = left + old.size; left_flags = old.flags;
                }
                size_t remove = left_flags & DM_SA_PAGE_COMMITTED ? left_end - left :
                    lesser(left_end - left, cost - budget);
                if (!(left_flags & DM_SA_PAGE_COMMITTED)) cost -= remove;
                left += remove;
            }
            if (window_candidate(s, left, right, size, alignment, out)) return DM_SA_OK;
        }
    }
    return DM_SA_NOT_FOUND;
}
static dm_sa_result allocate_locked(dm_sa_allocator *a, size_t size, size_t alignment, void **out) {
    if (a->closed) return DM_SA_CLOSED;
    if (!power2(alignment) || alignment < 16) return DM_SA_INVALID;
    size_t n;
    if (!rounded(size ? size : 1, 16, &n) || n > a->config.capacity) return DM_SA_NO_MEMORY;
    const dm_sa_provider *p = &a->config.provider;
    if (p->pages.granule) {
        for (unsigned pass = 0; pass < 2; ++pass) {
            size_t budget = 0;
            if (pass) {
                dm_sa_result r = p->pages.budget(p->context, &budget);
                if (r != DM_SA_OK) return r;
                budget &= ~(p->pages.granule - 1);
                if (!budget) break;
            }
            for (span *s = a->spans; s; s = s->next) {
                if (s->free_bytes < n) continue;
                for (block *b = s->blocks; b; b = b->next) {
                    if (b->live || b->size < n) continue;
                    size_t offset = 0;
                    dm_sa_result r = sparse_candidate(a, s, b, n, alignment, budget, &offset);
                    if (r == DM_SA_OK) return take_free(a, s, b, offset, n, alignment, out);
                    if (r != DM_SA_NOT_FOUND) return r;
                }
            }
        }
    } else for (span *s = a->spans; s; s = s->next) {
      if (s->free_bytes < n) continue; // do not scan every block of full pools
      for (block *b = s->blocks; b; b = b->next) {
        if (b->live) continue;
        uintptr_t base = (uintptr_t)s->lease.base + b->offset;
        if (base > UINTPTR_MAX - (alignment - 1)) continue;
        size_t skip = (size_t)((-(uintptr_t)base) & (alignment - 1));
        if (skip > b->size || n > b->size - skip) continue;
        if (!available(a, s, b->offset + skip, n)) continue;
        return take_free(a, s, b, b->offset + skip, n, alignment, out);
      }
    }
    size_t minimum;
    if (!rounded(n, p->granule, &minimum)) return DM_SA_NO_MEMORY;
    size_t lease_alignment = alignment > p->granule ? alignment : p->granule;
    dm_sa_provider_stats ps;
    dm_sa_result r = p->query(p->context, lease_alignment, &ps);
    if (r != DM_SA_OK) return r;
    size_t limit = lesser(a->config.capacity - a->backing, lesser(ps.available_bytes, ps.largest_span));
    limit &= ~(p->granule - 1);
    if (minimum > limit) return DM_SA_NO_MEMORY;
    size_t wanted = n < a->config.pool_size ? a->config.pool_size : minimum;
    wanted = lesser(wanted, limit);
    span *s = metadata_new(a, sizeof *s);
    if (!s) return DM_SA_NO_MEMORY;
    s->blocks = metadata_new(a, sizeof *s->blocks);
    if (!s->blocks) { metadata_delete(a, s, sizeof *s); return DM_SA_NO_MEMORY; }
    block *tail = wanted > n ? metadata_new(a, sizeof *tail) : NULL;
    if (wanted > n && !tail) { delete_span_metadata(a, s); return DM_SA_NO_MEMORY; }
    r = p->acquire(p->context, wanted, lease_alignment, &s->lease);
    if (r != DM_SA_OK) {
        if (tail) metadata_delete(a, tail, sizeof *tail);
        delete_span_metadata(a, s); return r;
    }
    // acquire's exact size/alignment/disjointness contract is mandatory. The
    // provider owns validation of native/guest address domains and accounting.
    span **link = &a->spans;
    while (*link && (uintptr_t)(*link)->lease.base < (uintptr_t)s->lease.base) link = &(*link)->next;
    s->next = *link; *link = s;
    a->backing += wanted; a->span_count++;
    block *b = s->blocks;
    // An SDK lease call may have dropped the source guard while its SVC used
    // the same guard. A new MapMemory borrow can then race the acquisition.
    // Keep the exact lease, but never expose borrowed bytes as an allocation.
    if (!available(a, s, 0, n)) {
        b->size = wanted; s->free_bytes = wanted;
        if (tail) metadata_delete(a, tail, sizeof *tail);
        (void)release_span(a, s); // usually BUSY; safe rollback if owner permits
        return DM_SA_BUSY;
    }
    b->size = n; b->alignment = alignment; b->live = 1;
    if (tail) { tail->offset = n; tail->size = wanted - n; insert_after(b, tail); }
    s->live_count = 1; s->free_bytes = wanted - n;
    a->used += n; a->count++;
    *out = s->lease.base; return DM_SA_OK;
}
dm_sa_result dm_sa_allocate(dm_sa_allocator *a, size_t size, size_t alignment, void **out) {
    if (!a || !out) return DM_SA_INVALID;
    enter(a); dm_sa_result r = allocate_locked(a, size, alignment, out); leave(a); return r;
}
static void free_block(dm_sa_allocator *a, span *s, block *b) {
    a->used -= b->size; a->count--; s->live_count--; s->free_bytes += b->size;
    b->live = 0; b->alignment = 0; coalesce(a, b);
    if (!s->live_count) (void)release_span(a, s); // failed release stays owned
}
static dm_sa_result free_locked(dm_sa_allocator *a, void *pointer) {
    if (a->closed) return DM_SA_CLOSED;
    if (!pointer) return DM_SA_OK;
    span *s = NULL; block *b = find_block(a, pointer, &s);
    if (!b) return DM_SA_NOT_FOUND;
    if (!available(a, s, b->offset, b->size)) return DM_SA_BUSY;
    free_block(a, s, b); return DM_SA_OK;
}
dm_sa_result dm_sa_free(dm_sa_allocator *a, void *pointer) {
    if (!a) return DM_SA_INVALID;
    enter(a); dm_sa_result r = free_locked(a, pointer); leave(a); return r;
}
static dm_sa_result shrink_locked(dm_sa_allocator *a, span *s, block *b, size_t n) {
    if (n > b->size) return DM_SA_INVALID;
    if (!available(a, s, b->offset, b->size)) return DM_SA_BUSY;
    if (n == b->size) return DM_SA_OK;
    size_t released = b->size - n;
    block *tail = b->next;
    if (tail && !tail->live) { tail->offset -= released; tail->size += released; }
    else {
        tail = metadata_new(a, sizeof *tail);
        if (!tail) return DM_SA_NO_MEMORY;
        tail->offset = b->offset + n; tail->size = released; insert_after(b, tail);
    }
    b->size = n; a->used -= released; s->free_bytes += released; return DM_SA_OK;
}
dm_sa_result dm_sa_shrink(dm_sa_allocator *a, void *pointer, size_t size, size_t *out) {
    if (!a || !pointer || !out || !size) return DM_SA_INVALID;
    size_t n;
    if (!rounded(size, 16, &n)) return DM_SA_INVALID;
    enter(a);
    span *s = NULL; block *b = a->closed ? NULL : find_block(a, pointer, &s);
    dm_sa_result r = a->closed ? DM_SA_CLOSED : !b ? DM_SA_NOT_FOUND : shrink_locked(a, s, b, n);
    if (r == DM_SA_OK) *out = n;
    leave(a); return r;
}
dm_sa_result dm_sa_reallocate(dm_sa_allocator *a, void *pointer, size_t size, void **out) {
    if (!a || !out) return DM_SA_INVALID;
    enter(a);
    dm_sa_result r;
    if (a->closed) r = DM_SA_CLOSED;
    else if (!pointer) r = allocate_locked(a, size, 16, out);
    else if (!size) { r = free_locked(a, pointer); if (r == DM_SA_OK) *out = NULL; }
    else {
        span *s = NULL; block *b = find_block(a, pointer, &s);
        size_t n;
        if (!b) r = DM_SA_NOT_FOUND;
        else if (!rounded(size, 16, &n)) r = DM_SA_NO_MEMORY;
        else if (!available(a, s, b->offset, b->size)) r = DM_SA_BUSY;
        else if (n <= b->size) { r = shrink_locked(a, s, b, n); if (r == DM_SA_OK) *out = pointer; }
        else {
            block *tail = b->next; size_t extra = n - b->size;
            int grow = 0;
            dm_sa_result growth = DM_SA_OK;
            if (tail && !tail->live && extra <= tail->size) {
                if (a->config.provider.pages.granule) {
                    growth = prepare_backing(a, s, tail->offset, extra);
                    // Committing the tail can suspend the source guard. The
                    // already-live prefix can become borrowed independently.
                    if (!available(a, s, b->offset, b->size) ||
                        (growth == DM_SA_OK && !available(a, s, b->offset, n))) growth = DM_SA_BUSY;
                    grow = growth == DM_SA_OK;
                } else grow = available(a, s, tail->offset, extra);
            }
            if (grow) {
                tail->offset += extra; tail->size -= extra; b->size = n; a->used += extra; s->free_bytes -= extra;
                if (!tail->size) { b->next = tail->next; if (tail->next) tail->next->prev = b; metadata_delete(a, tail, sizeof *tail); }
                *out = pointer; r = DM_SA_OK;
            } else if (growth != DM_SA_OK && growth != DM_SA_NO_MEMORY) r = growth;
            else {
                void *replacement;
                r = allocate_locked(a, n, b->alignment, &replacement);
                if (r == DM_SA_OK) {
                    // Acquiring a new SDK lease can suspend the source guard.
                    // The old allocation may have become borrowed meanwhile;
                    // never memcpy from it based on the pre-acquisition check.
                    if (!available(a, s, b->offset, b->size)) {
                        span *replacement_owner = NULL;
                        block *replacement_block = find_block(a, replacement, &replacement_owner);
                        free_block(a, replacement_owner, replacement_block);
                        r = DM_SA_BUSY; // original and caller output unchanged
                    } else {
                        memcpy(replacement, pointer, b->size);
                        free_block(a, s, b); *out = replacement;
                    }
                }
            }
        }
    }
    leave(a); return r;
}
dm_sa_result dm_sa_get_size(dm_sa_allocator *a, const void *pointer, size_t *out) {
    if (!a || !pointer || !out) return DM_SA_INVALID;
    enter(a); span *s = NULL; block *b = a->closed ? NULL : find_block(a, pointer, &s);
    dm_sa_result r = a->closed ? DM_SA_CLOSED : !b ? DM_SA_NOT_FOUND : DM_SA_OK;
    if (r == DM_SA_OK) *out = b->size;
    leave(a); return r;
}
// A two-cursor window over authoritative runs, not a second residency ledger.
// Whole free granules may be holes; live block boundary granules stay backed.
// Each missing granule consumes physical headroom once. Borrowed runs are hard
// barriers; already-backed free runs require no additional physical budget.
static dm_sa_result sparse_free_stats(dm_sa_allocator *a, const span *s, const block *b,
                                       size_t budget, size_t *backed, size_t *holes,
                                       size_t *largest) {
    size_t left = b->offset, right = left, limit = left + b->size, cost = 0;
    size_t granule = a->config.provider.pages.granule;
    while (right < limit) {
        dm_sa_page_run run;
        dm_sa_result r = page_run(a, s, right, limit - right, &run);
        if (r != DM_SA_OK) return r;
        size_t end = right + run.size;
        if (run.flags & DM_SA_PAGE_BORROWED) {
            left = right = end; cost = 0; continue;
        }
        if (run.flags & DM_SA_PAGE_COMMITTED) *backed += run.size;
        else {
            // A hole split by a live boundary would violate the page-provider
            // lifetime contract. Never advertise that malformed run as usable.
            if ((right | run.size) & (granule - 1)) return DM_SA_PROVIDER_FAILED;
            *holes += run.size;
            size_t prefix = right - left + lesser(run.size, budget - cost);
            if (prefix > *largest) *largest = prefix;
            cost += run.size;
        }
        right = end;
        while (cost > budget) {
            dm_sa_page_run old;
            r = page_run(a, s, left, right - left, &old);
            if (r != DM_SA_OK) return r;
            size_t remove = old.flags & DM_SA_PAGE_COMMITTED ? old.size : lesser(old.size, cost - budget);
            if (!(old.flags & DM_SA_PAGE_COMMITTED)) cost -= remove;
            left += remove;
        }
        if (right - left > *largest) *largest = right - left;
    }
    return DM_SA_OK;
}
static dm_sa_result committed_stats(dm_sa_allocator *a, size_t *out) {
    size_t committed = 0;
    for (span *s = a->spans; s; s = s->next)
        for (size_t offset = 0; offset < s->lease.size;) {
            dm_sa_page_run run;
            dm_sa_result r = page_run(a, s, offset, s->lease.size - offset, &run);
            if (r != DM_SA_OK) return r;
            if (run.flags & DM_SA_PAGE_COMMITTED) committed += run.size;
            offset += run.size;
        }
    *out = committed; return DM_SA_OK;
}
dm_sa_result dm_sa_get_stats(dm_sa_allocator *a, dm_sa_stats *out) {
    if (!a || !out) return DM_SA_INVALID;
    enter(a);
    dm_sa_stats st = { .capacity = a->config.capacity, .backing_bytes = a->backing,
        .allocated_bytes = a->used, .allocation_count = a->count, .span_count = a->span_count,
        .metadata_bytes = a->metadata, .closed = a->closed, .committed_bytes = a->backing };
    dm_sa_result r = DM_SA_OK;
    const dm_sa_provider *p = &a->config.provider;
    if (p->pages.granule) r = committed_stats(a, &st.committed_bytes);
    if (!a->closed && r == DM_SA_OK) {
        dm_sa_provider_stats ps;
        r = p->query(p->context, p->granule, &ps);
        if (r == DM_SA_OK) {
            size_t cap = a->config.capacity - a->backing;
            size_t extra = lesser(cap, ps.available_bytes) & ~(p->granule - 1);
            if (p->pages.granule) {
                size_t budget, backed = 0, holes = 0;
                r = p->pages.budget(p->context, &budget);
                if (r == DM_SA_OK) {
                    budget &= ~(p->pages.granule - 1);
                    extra = lesser(extra, budget) & ~(p->granule - 1);
                    st.largest_free = lesser(extra, ps.largest_span) & ~(p->granule - 1);
                    for (span *s = a->spans; s && r == DM_SA_OK; s = s->next)
                        for (block *b = s->blocks; b && r == DM_SA_OK; b = b->next)
                            if (!b->live) r = sparse_free_stats(a, s, b, budget, &backed, &holes, &st.largest_free);
                    st.total_free = backed + lesser(budget, holes + extra);
                }
            } else {
                st.total_free = extra;
                st.largest_free = lesser(extra, ps.largest_span) & ~(p->granule - 1);
                for (span *s = a->spans; s; s = s->next) for (block *b = s->blocks; b; b = b->next)
                    if (!b->live && available(a, s, b->offset, b->size)) {
                        st.total_free += b->size;
                        if (b->size > st.largest_free) st.largest_free = b->size;
                    }
            }
        }
    }
    if (r == DM_SA_OK) *out = st;
    leave(a); return r;
}
dm_sa_result dm_sa_trim(dm_sa_allocator *a) {
    if (!a) return DM_SA_INVALID;
    enter(a); dm_sa_result r = a->closed ? DM_SA_CLOSED : DM_SA_OK;
    if (!a->closed) for (span *s = a->spans, *next; s; s = next) {
        next = s->next;
        if (!s->live_count) { dm_sa_result one = release_span(a, s); if (r == DM_SA_OK) r = one; }
    }
    leave(a); return r;
}
dm_sa_result dm_sa_finalize(dm_sa_allocator *a) {
    if (!a) return DM_SA_INVALID;
    enter(a);
    if (!a->closed) {
        for (span *s = a->spans; s; s = s->next) {
            dm_sa_result ready = releasable(a, s);
            if (ready != DM_SA_OK) { leave(a); return ready; }
        }
        a->closed = 1; a->used = 0; a->count = 0;
        for (span *s = a->spans; s; s = s->next) s->live_count = 0;
    }
    dm_sa_result r = DM_SA_OK;
    for (span *s = a->spans, *next; s; s = next) {
        next = s->next; dm_sa_result one = release_span(a, s); if (r == DM_SA_OK) r = one;
    }
    leave(a); return r;
}
dm_sa_result dm_sa_destroy(dm_sa_allocator *a) {
    if (!a) return DM_SA_INVALID;
    dm_sa_result r = dm_sa_finalize(a);
    if (r != DM_SA_OK) return r;
    dm_sa_config c = a->config;
    pthread_mutex_destroy(&a->mutex); c.metadata_free(c.metadata_context, a); return DM_SA_OK;
}
dm_sa_result dm_sa_walk(dm_sa_allocator *a, dm_sa_walk_fn callback, void *context, int *callback_result) {
    if (!a || !callback || !callback_result) return DM_SA_INVALID;
    enter(a);
    if (a->closed) { leave(a); return DM_SA_CLOSED; }
    int result = 0;
    for (span *s = a->spans; s && !result; s = s->next)
        for (block *b = s->blocks; b && !result; b = b->next)
            if (b->live) result = callback(address(s, b), b->size, context);
    *callback_result = result; leave(a); return DM_SA_OK;
}

// Minimal streaming SHA-1 for allocation metadata diagnostics. SHA-1 is not
// used for authentication/security. Input bytes are generated locally; no guest
// allocation contents or SDK implementation is read by this computation.
typedef struct { uint32_t h[5]; uint64_t bytes; unsigned char data[64]; size_t used; } sha1;
static uint32_t rol(uint32_t v, unsigned n) { return (v << n) | (v >> (32 - n)); }
static void sha1_block(sha1 *s) {
    uint32_t w[80];
    for (unsigned i = 0; i < 16; i++) w[i] = (uint32_t)s->data[4*i] << 24 |
        (uint32_t)s->data[4*i+1] << 16 | (uint32_t)s->data[4*i+2] << 8 | s->data[4*i+3];
    for (unsigned i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (unsigned i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_put(sha1 *s, unsigned char v) {
    s->data[s->used++] = v; s->bytes++;
    if (s->used == 64) { sha1_block(s); s->used = 0; }
}
static void sha1_word(sha1 *s, uint64_t v) {
    for (unsigned i = 0; i < 8; i++) { sha1_put(s, (unsigned char)v); v >>= 8; }
}
dm_sa_result dm_sa_get_hash(dm_sa_allocator *a, dm_sa_hash *out) {
    if (!a || !out) return DM_SA_INVALID;
    enter(a);
    if (a->closed) { leave(a); return DM_SA_CLOSED; }
    sha1 hash = { .h = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 } };
    dm_sa_hash result = { .block_count = a->count, .allocated_bytes = a->used };
    for (span *s = a->spans; s; s = s->next) for (block *b = s->blocks; b; b = b->next)
        if (b->live) { sha1_word(&hash, (uint64_t)(uintptr_t)address(s, b)); sha1_word(&hash, b->size); }
    uint64_t bits = hash.bytes * 8;
    sha1_put(&hash, 0x80);
    while (hash.used != 56) sha1_put(&hash, 0);
    for (int i = 7; i >= 0; i--) sha1_put(&hash, (unsigned char)(bits >> (i * 8)));
    for (unsigned i = 0; i < 8; i++) result.digest[i] = (unsigned char)(hash.h[i / 4] >> (24 - (i % 4) * 8));
    *out = result; leave(a); return DM_SA_OK;
}
