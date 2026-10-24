// Shared native address-region ownership and page accounting. Metadata belongs
// to the host, with a fixed allocation at creation and no allocations in a hot
// operation. This implementation is independent of guest SDK data structures.
#include "hle_address_space.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uintptr_t base; size_t size; } reservation;
struct dm_address_space {
    pthread_mutex_t lock;
    dm_as_config config;
    reservation *regions; // sorted; free intervals are implicit and coalesce
    uint64_t *pages;
    uint64_t *exposed; // optional host-page history; never cleared on decommit
    size_t count, reserved, committed, metadata;
};

static int power_of_two(size_t n) { return n && !(n & (n - 1)); }
static int in_range(const dm_address_space *s, uintptr_t a, size_t n) {
    return n && a >= s->config.base && n <= s->config.size &&
           a - s->config.base <= s->config.size - n;
}
static int page_range(const dm_address_space *s, uintptr_t a, size_t n) {
    return in_range(s, a, n) && !(a % s->config.guest_page_size) && !(n % s->config.guest_page_size);
}
static uint64_t mask_bits(size_t n) { return n == 64 ? UINT64_MAX : (UINT64_C(1) << n) - 1; }
static size_t count_pages(const dm_address_space *s, size_t first, size_t end) {
    size_t count = 0;
    while (first < end) {
        size_t shift = first % 64, n = 64 - shift;
        if (n > end - first) n = end - first;
        count += (size_t)__builtin_popcountll(s->pages[first / 64] & (mask_bits(n) << shift));
        first += n;
    }
    return count;
}
static void set_bitmap(uint64_t *bitmap, size_t first, size_t end, int commit) {
    while (first < end) {
        size_t shift = first % 64, n = 64 - shift;
        if (n > end - first) n = end - first;
        uint64_t mask = mask_bits(n) << shift;
        if (commit) bitmap[first / 64] |= mask;
        else bitmap[first / 64] &= ~mask;
        first += n;
    }
}
static size_t lower_bound(const dm_address_space *s, uintptr_t a) {
    size_t lo = 0, hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->regions[mid].base < a) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
static int owns(const dm_address_space *s, uintptr_t a, size_t n) {
    size_t i = lower_bound(s, a);
    if (i == s->count || s->regions[i].base != a) {
        if (!i) return 0;
        --i;
    }
    return n <= s->regions[i].size && a - s->regions[i].base <= s->regions[i].size - n;
}

dm_as_result dm_as_create(dm_address_space **out, const dm_as_config *c) {
    if (!out || !c || !c->base || !c->size || c->size > UINTPTR_MAX - c->base ||
        !power_of_two(c->guest_page_size) || !power_of_two(c->host_page_size) ||
        c->guest_page_size > c->host_page_size || c->base % c->host_page_size ||
        c->size % c->host_page_size || !c->apply ||
        (c->initially_zero != 0 && c->initially_zero != 1) ||
        (c->initially_zero && !c->initialize) ||
        (c->mode != DM_AS_EXTERNAL_RESERVATIONS && c->mode != DM_AS_HOST_RESERVATIONS) ||
        (c->mode == DM_AS_HOST_RESERVATIONS && !c->max_regions) ||
        c->max_regions > SIZE_MAX / sizeof(reservation)) return DM_AS_INVALID;
    size_t npages = c->size / c->guest_page_size;
    size_t words = npages / 64 + (npages % 64 != 0);
    size_t host_pages = c->size / c->host_page_size;
    size_t exposed_words = c->initially_zero ? host_pages / 64 + (host_pages % 64 != 0) : 0;
    size_t region_bytes = c->max_regions * sizeof(reservation);
    size_t max_words = (SIZE_MAX - sizeof(dm_address_space)) / sizeof(uint64_t);
    if (words > max_words || exposed_words > max_words - words ||
        region_bytes > SIZE_MAX - sizeof(dm_address_space) - (words + exposed_words) * sizeof(uint64_t)) return DM_AS_INVALID;
    dm_address_space *s = calloc(1, sizeof *s);
    if (!s) return DM_AS_NO_MEMORY;
    s->pages = calloc(words, sizeof *s->pages);
    if (exposed_words) s->exposed = calloc(exposed_words, sizeof *s->exposed);
    if (c->max_regions) s->regions = calloc(c->max_regions, sizeof *s->regions);
    if (!s->pages || (exposed_words && !s->exposed) || (c->max_regions && !s->regions) || pthread_mutex_init(&s->lock, NULL)) {
        free(s->regions); free(s->pages); free(s->exposed); free(s); return DM_AS_NO_MEMORY;
    }
    s->config = *c;
    s->metadata = sizeof *s + region_bytes + (words + exposed_words) * sizeof(uint64_t);
    *out = s;
    return DM_AS_OK;
}

dm_as_result dm_as_destroy(dm_address_space *s) {
    if (!s) return DM_AS_INVALID;
    pthread_mutex_lock(&s->lock);
    if (s->count || s->committed) { pthread_mutex_unlock(&s->lock); return DM_AS_BUSY; }
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_destroy(&s->lock);
    free(s->regions); free(s->pages); free(s->exposed); free(s);
    return DM_AS_OK;
}

dm_as_result dm_as_reserve(dm_address_space *s, size_t n, size_t alignment, uintptr_t *out) {
    if (!s || !out || !n || n % s->config.guest_page_size || !power_of_two(alignment) ||
        alignment < s->config.guest_page_size) return DM_AS_INVALID;
    pthread_mutex_lock(&s->lock);
    dm_as_result result = DM_AS_NO_SPACE;
    if (s->config.mode != DM_AS_HOST_RESERVATIONS) { result = DM_AS_WRONG_MODE; goto done; }
    if (s->count == s->config.max_regions) { result = DM_AS_METADATA_FULL; goto done; }
    uintptr_t start = s->config.base, end = start + s->config.size;
    for (size_t i = 0; i <= s->count; ++i) {
        uintptr_t stop = i < s->count ? s->regions[i].base : end;
        if (start <= UINTPTR_MAX - (alignment - 1)) {
            uintptr_t aligned = (start + alignment - 1) & ~(uintptr_t)(alignment - 1);
            if (aligned <= stop && n <= stop - aligned) {
                memmove(&s->regions[i + 1], &s->regions[i], (s->count - i) * sizeof *s->regions);
                s->regions[i] = (reservation){aligned, n};
                ++s->count; s->reserved += n;
                *out = aligned;
                result = DM_AS_OK;
                break;
            }
        }
        if (i < s->count) start = s->regions[i].base + s->regions[i].size;
    }
done:
    pthread_mutex_unlock(&s->lock);
    return result;
}

dm_as_result dm_as_release(dm_address_space *s, uintptr_t a, size_t n) {
    if (!s || !page_range(s, a, n)) return DM_AS_INVALID;
    pthread_mutex_lock(&s->lock);
    dm_as_result result = DM_AS_OK;
    if (s->config.mode != DM_AS_HOST_RESERVATIONS) { result = DM_AS_WRONG_MODE; goto done; }
    size_t i = lower_bound(s, a);
    if (i == s->count || s->regions[i].base != a || s->regions[i].size != n) { result = DM_AS_NOT_FOUND; goto done; }
    size_t first = (a - s->config.base) / s->config.guest_page_size;
    if (count_pages(s, first, first + n / s->config.guest_page_size)) { result = DM_AS_BUSY; goto done; }
    memmove(&s->regions[i], &s->regions[i + 1], (s->count - i - 1) * sizeof *s->regions);
    --s->count; s->reserved -= n;
done:
    pthread_mutex_unlock(&s->lock);
    return result;
}

static size_t run_end(const dm_address_space *s, size_t p, size_t hi, int committed);
static void initialize_run(dm_address_space *s, size_t first, size_t end) {
    size_t gp = s->config.guest_page_size;
    if (!s->exposed) {
        s->config.initialize(s->config.context, s->config.base + first * gp, (end - first) * gp);
        return;
    }
    // Only never-exposed native host pages retain the OS's lazy zero-fill
    // guarantee. A first logical commit beside a live sibling is NOT virgin:
    // that whole host page has already been writable. Coalesce initialization
    // across adjacent exposed host pages; no byte of an existing commit is hit.
    size_t ratio = s->config.host_page_size / gp, clear_first = first;
    for (size_t p = first; p < end;) {
        size_t host_page = p / ratio, next = (host_page + 1) * ratio;
        if (next > end) next = end;
        if (!((s->exposed[host_page / 64] >> (host_page % 64)) & 1)) {
            if (clear_first < p) s->config.initialize(s->config.context, s->config.base + clear_first * gp, (p - clear_first) * gp);
            clear_first = next;
        }
        p = next;
    }
    if (clear_first < end) s->config.initialize(s->config.context, s->config.base + clear_first * gp, (end - clear_first) * gp);
}
static dm_as_result change_pages(dm_address_space *s, uintptr_t a, size_t n, size_t limit, int commit) {
    if (!s || !page_range(s, a, n)) return DM_AS_INVALID;
    pthread_mutex_lock(&s->lock);
    dm_as_result result = DM_AS_OK;
    if (s->config.mode == DM_AS_HOST_RESERVATIONS && !owns(s, a, n)) { result = DM_AS_NOT_FOUND; goto done; }
    size_t gp = s->config.guest_page_size, hp = s->config.host_page_size;
    size_t first = (a - s->config.base) / gp, end = first + n / gp;
    size_t old = count_pages(s, first, end) * gp;
    size_t delta = commit ? n - old : old;
    // A repeated commit consumes no additional capacity, even if a caller has
    // since lowered its budget below existing committed memory.
    if (commit && delta && (s->committed > limit || delta > limit - s->committed)) { result = DM_AS_LIMIT; goto done; }
    if (!delta) goto done;
    uintptr_t lo = a & ~(uintptr_t)(hp - 1), hi = (a + n + hp - 1) & ~(uintptr_t)(hp - 1);
    // The registered block is host-page aligned and checked for overflow;
    // a+n <= block end, so rounding outward cannot escape it or overflow.
    // For decommit, retain edge host pages if any adjacent logical page is
    // still live. The remaining releasable host pages form one contiguous run.
    if (!commit) {
        if (count_pages(s, (lo - s->config.base) / gp, first)) lo += hp;
        if (count_pages(s, end, (hi - s->config.base) / gp)) hi -= hp;
    }
    if (hi > lo && s->config.apply(s->config.context, lo, hi - lo, commit)) { result = DM_AS_BACKEND_FAILED; goto done; }
    // Nothing fallible follows the protection change. Initialize only holes in
    // the old bitmap: overlapping/repeated commits must preserve live contents,
    // including neighbors sharing a larger host page. Ownership and accounting
    // remain locked until all newly committed bytes are initialized.
    if (commit && s->config.initialize) {
        for (size_t p = first; p < end;) {
            int was_committed = (int)((s->pages[p / 64] >> (p % 64)) & 1);
            size_t next = run_end(s, p, end, was_committed);
            if (!was_committed) initialize_run(s, p, next);
            p = next;
        }
    }
    if (commit && s->exposed) set_bitmap(s->exposed, (lo - s->config.base) / hp, (hi - s->config.base) / hp, 1);
    set_bitmap(s->pages, first, end, commit);
    if (commit) s->committed += delta;
    else s->committed -= delta;
done:
    pthread_mutex_unlock(&s->lock);
    return result;
}

dm_as_result dm_as_commit(dm_address_space *s, uintptr_t a, size_t n, size_t limit) { return change_pages(s, a, n, limit, 1); }
dm_as_result dm_as_decommit(dm_address_space *s, uintptr_t a, size_t n) { return change_pages(s, a, n, 0, 0); }

// Find the run of one bitmap value containing page p, word-at-a-time. Bounds
// are page indices; comparisons to differing bits avoid scanning every page.
static size_t run_start(const dm_address_space *s, size_t p, size_t lo, int committed) {
    size_t end = p;
    while (end > lo) {
        size_t first = (end - 1) & ~(size_t)63;
        if (first < lo) first = lo;
        size_t n = end - first, shift = first % 64;
        uint64_t different = committed ? ~s->pages[first / 64] : s->pages[first / 64];
        different &= mask_bits(n) << shift;
        if (different) return (first & ~(size_t)63) + 64 - (size_t)__builtin_clzll(different);
        end = first;
    }
    return lo;
}
static size_t run_end(const dm_address_space *s, size_t p, size_t hi, int committed) {
    size_t first = p + 1;
    while (first < hi) {
        size_t n = 64 - first % 64;
        if (n > hi - first) n = hi - first;
        uint64_t different = committed ? ~s->pages[first / 64] : s->pages[first / 64];
        different &= mask_bits(n) << (first % 64);
        if (different) return (first & ~(size_t)63) + (size_t)__builtin_ctzll(different);
        first += n;
    }
    return hi;
}

dm_as_result dm_as_query(dm_address_space *s, uintptr_t a, dm_as_range *out) {
    if (!s || !out || !in_range(s, a, 1)) return DM_AS_INVALID;
    pthread_mutex_lock(&s->lock);
    uintptr_t lo = s->config.base, hi = lo + s->config.size;
    dm_as_range range = {0};
    if (s->config.mode == DM_AS_HOST_RESERVATIONS) {
        size_t i = lower_bound(s, a);
        if (i < s->count && s->regions[i].base == a) {
            range.reserved = 1; lo = s->regions[i].base; hi = lo + s->regions[i].size;
        } else if (i && a - s->regions[i - 1].base < s->regions[i - 1].size) {
            range.reserved = 1; lo = s->regions[i - 1].base; hi = lo + s->regions[i - 1].size;
        } else {
            if (i) lo = s->regions[i - 1].base + s->regions[i - 1].size;
            if (i < s->count) hi = s->regions[i].base;
        }
        if (range.reserved) { range.reservation_base = lo; range.reservation_size = hi - lo; }
    }
    size_t gp = s->config.guest_page_size, p = (a - s->config.base) / gp;
    range.committed = (int)((s->pages[p / 64] >> (p % 64)) & 1);
    size_t first = run_start(s, p, (lo - s->config.base) / gp, range.committed);
    size_t end = run_end(s, p, (hi - s->config.base) / gp, range.committed);
    range.base = s->config.base + first * gp; range.size = (end - first) * gp;
    *out = range;
    pthread_mutex_unlock(&s->lock);
    return DM_AS_OK;
}

dm_as_result dm_as_committed_in(dm_address_space *s, uintptr_t a, size_t n, size_t *out) {
    if (!s || !out || !page_range(s, a, n)) return DM_AS_INVALID;
    pthread_mutex_lock(&s->lock);
    size_t first = (a - s->config.base) / s->config.guest_page_size;
    *out = count_pages(s, first, first + n / s->config.guest_page_size) * s->config.guest_page_size;
    pthread_mutex_unlock(&s->lock);
    return DM_AS_OK;
}

dm_as_result dm_as_get_stats(dm_address_space *s, dm_as_stats *out) {
    if (!s || !out) return DM_AS_INVALID;
    pthread_mutex_lock(&s->lock);
    dm_as_stats stats = {.base = s->config.base, .size = s->config.size,
        .reserved_bytes = s->reserved, .committed_bytes = s->committed,
        .regions = s->count, .max_regions = s->config.max_regions,
        .metadata_bytes = s->metadata, .mode = s->config.mode};
    // External reservations are deliberately opaque: do not advertise the
    // SDK's uncommitted reservations as space available to a second allocator.
    if (s->config.mode == DM_AS_HOST_RESERVATIONS) {
        stats.free_bytes = s->config.size - s->reserved;
        uintptr_t end = s->config.base;
        for (size_t i = 0; i < s->count; ++i) {
            size_t gap = s->regions[i].base - end;
            if (gap > stats.largest_free) stats.largest_free = gap;
            end = s->regions[i].base + s->regions[i].size;
        }
        size_t gap = s->config.base + s->config.size - end;
        if (gap > stats.largest_free) stats.largest_free = gap;
    }
    *out = stats;
    pthread_mutex_unlock(&s->lock);
    return DM_AS_OK;
}
