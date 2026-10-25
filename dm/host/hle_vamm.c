#include "hle_vamm.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE = 0x1000, SMALL_ALIGNMENT = 0x10000, LARGE_ALIGNMENT = 0x200000 };
typedef struct { size_t minimum, alignment; } placement_tier;
// One definition drives actual placement AND realizable free-space queries.
// No policy is assigned to an unreviewed SDK by these tables. The reduced
// legacy policy reproduces only the six existing alignment cuts, not a claim
// that all older bootstrap/layout/result contracts match this owner.
static const placement_tier modern[] = {{PAGE, SMALL_ALIGNMENT}, {LARGE_ALIGNMENT, LARGE_ALIGNMENT}};
static const placement_tier legacy[] = {
    {PAGE, SMALL_ALIGNMENT}, {LARGE_ALIGNMENT, LARGE_ALIGNMENT},
    {0x400000, 0x400000}, {0x2000000, 0x2000000}, {0x40000000, 0x40000000}
};
static const placement_tier reduced[] = {
    {PAGE, SMALL_ALIGNMENT}, {LARGE_ALIGNMENT, LARGE_ALIGNMENT},
    {0x400000, 0x400000}, {0x2000000, 0x2000000}
};
static const placement_tier *placement(dm_vamm_placement policy, size_t *count) {
    switch (policy) {
    case DM_VAMM_PLACEMENT_MODERN: *count = sizeof modern / sizeof *modern; return modern;
    case DM_VAMM_PLACEMENT_LEGACY: *count = sizeof legacy / sizeof *legacy; return legacy;
    case DM_VAMM_PLACEMENT_LEGACY_REDUCED: *count = sizeof reduced / sizeof *reduced; return reduced;
    }
    *count = 0; return NULL;
}
typedef struct { uintptr_t base; size_t size; } region;
struct dm_vamm {
    pthread_mutex_t mutex;
    dm_vamm_config config;
    region *regions;
    size_t count, reserved;
    int initialized;
};
static int power_two(size_t n) { return n && !(n & (n - 1)); }
static size_t lower_bound(const dm_vamm *v, uintptr_t base) {
    size_t lo = 0, hi = v->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v->regions[mid].base < base) lo = mid + 1; else hi = mid;
    }
    return lo;
}
static int contains(const dm_vamm *v, uintptr_t base, size_t size) {
    if (!size || base % PAGE || size % PAGE) return 0;
    size_t i = lower_bound(v, base);
    if (i == v->count || v->regions[i].base != base) { if (!i) return 0; --i; }
    return size <= v->regions[i].size && base - v->regions[i].base <= v->regions[i].size - size;
}
// The two virtual sentinels consume no metadata in guest memory. All arithmetic
// is bounded by the real domain; guard/alignment overflow cannot wrap a success.
static size_t gap(const dm_vamm *v, size_t i, size_t alignment, uintptr_t *base) {
    uintptr_t lo = i ? v->regions[i - 1].base + v->regions[i - 1].size : v->config.base;
    uintptr_t hi = i < v->count ? v->regions[i].base : v->config.base + v->config.size;
    if (hi - lo < 2 * PAGE || lo > UINTPTR_MAX - PAGE) return 0;
    lo += PAGE; hi -= PAGE;
    if (lo > UINTPTR_MAX - (alignment - 1)) return 0;
    lo = (lo + alignment - 1) & ~(uintptr_t)(alignment - 1);
    if (lo > hi) return 0;
    *base = lo;
    return (hi - lo) & ~(size_t)(PAGE - 1);
}
static size_t largest_gap(const dm_vamm *v, size_t i, size_t alignment) {
    uintptr_t ignored; size_t count, largest = 0;
    const placement_tier *tiers = placement(v->config.placement, &count);
    for (size_t j = 0; j < count; ++j) {
        size_t a = alignment > tiers[j].alignment ? alignment : tiers[j].alignment;
        size_t n = gap(v, i, a, &ignored);
        // A candidate must remain inside the same size tier that produced its
        // alignment, or the advertised largest allocation may not actually fit.
        if (j + 1 < count && n >= tiers[j + 1].minimum) n = tiers[j + 1].minimum - PAGE;
        if (n >= tiers[j].minimum && n > largest) largest = n;
    }
    return largest;
}
uint32_t dm_vamm_create(dm_vamm **out, const dm_vamm_config *c) {
    size_t ignored;
    if (!out || !c || !c->base || c->base % PAGE || !c->size || c->size % PAGE ||
        c->size > UINTPTR_MAX - c->base || !c->max_regions ||
        c->max_regions > (SIZE_MAX - sizeof(dm_vamm)) / sizeof(region) ||
        !c->change_pages || !c->resources || !placement(c->placement, &ignored)) return DM_VAMM_INVALID;
    dm_vamm *v = calloc(1, sizeof *v);
    if (!v) return DM_VAMM_NO_MEMORY;
    v->regions = calloc(c->max_regions, sizeof *v->regions);
    if (!v->regions || pthread_mutex_init(&v->mutex, NULL)) {
        free(v->regions); free(v); return DM_VAMM_NO_MEMORY;
    }
    v->config = *c; *out = v; return 0;
}
uint32_t dm_vamm_destroy(dm_vamm *v) {
    if (!v) return DM_VAMM_INVALID;
    pthread_mutex_lock(&v->mutex);
    if (v->count) { pthread_mutex_unlock(&v->mutex); return DM_VAMM_INVALID; }
    pthread_mutex_unlock(&v->mutex);
    pthread_mutex_destroy(&v->mutex); free(v->regions); free(v); return 0;
}
void dm_vamm_initialize(dm_vamm *v) {
    if (!v) return;
    pthread_mutex_lock(&v->mutex);
    if (!v->initialized && v->config.resources(v->config.context).total) v->initialized = 1;
    pthread_mutex_unlock(&v->mutex);
}
int dm_vamm_is_initialized(dm_vamm *v) {
    if (!v) return 0;
    pthread_mutex_lock(&v->mutex);
    int initialized = v->initialized;
    pthread_mutex_unlock(&v->mutex); return initialized;
}
int dm_vamm_is_enabled(dm_vamm *v) {
    if (!v) return 0;
    pthread_mutex_lock(&v->mutex);
    int enabled = v->config.resources(v->config.context).total != 0;
    pthread_mutex_unlock(&v->mutex); return enabled;
}
dm_vamm_resource_usage dm_vamm_resources(dm_vamm *v) {
    if (!v) return (dm_vamm_resource_usage){0, 0};
    pthread_mutex_lock(&v->mutex);
    dm_vamm_resource_usage r = v->config.resources(v->config.context);
    // Observed SDK contract reserves 512 KiB of system-resource headroom.
    // Saturate instead of wrapping pathological callback values.
    r.used = r.used > UINT64_MAX - 0x80000 ? UINT64_MAX : r.used + 0x80000;
    if (r.used > r.total) r.used = r.total;
    pthread_mutex_unlock(&v->mutex); return r;
}
uint32_t dm_vamm_allocate(dm_vamm *v, uintptr_t *out, size_t size, int commit) {
    if (!v) return DM_VAMM_INVALID;
    size_t count, alignment = SMALL_ALIGNMENT;
    const placement_tier *tiers = placement(v->config.placement, &count);
    for (size_t i = 0; i < count && size >= tiers[i].minimum; ++i) alignment = tiers[i].alignment;
    return dm_vamm_allocate_aligned(v, out, size, alignment, commit);
}
uint32_t dm_vamm_allocate_aligned(dm_vamm *v, uintptr_t *out, size_t size,
                                  size_t alignment, int commit) {
    if (!v || !out || !size || size % PAGE || !power_two(alignment) || alignment < PAGE ||
        (commit != 0 && commit != 1)) return DM_VAMM_INVALID;
    pthread_mutex_lock(&v->mutex);
    uint32_t result = DM_VAMM_INVALID;
    if (!v->initialized) goto done;
    if (v->count == v->config.max_regions) { result = DM_VAMM_NO_MEMORY; goto done; }
    result = DM_VAMM_NO_SPACE;
    // Descending free intervals, lowest aligned address within the chosen hole.
    for (size_t k = v->count + 1; k; --k) {
        size_t i = k - 1; uintptr_t base;
        if (gap(v, i, alignment, &base) < size) continue;
        // No guest callback may observe this in-flight candidate. A failed map
        // creates neither reservation nor output; successful map has no later
        // fallible bookkeeping step, so combined allocation rolls back exactly.
        if (commit) {
            result = v->config.change_pages(v->config.context, base, size, 1);
            if (result) goto done;
        }
        memmove(v->regions + i + 1, v->regions + i, (v->count - i) * sizeof(region));
        v->regions[i] = (region){base, size}; ++v->count; v->reserved += size;
        *out = base; result = 0; break;
    }
done:
    pthread_mutex_unlock(&v->mutex); return result;
}
uint32_t dm_vamm_pages(dm_vamm *v, uintptr_t base, size_t size, int commit) {
    if (!v || (commit != 0 && commit != 1)) return DM_VAMM_INVALID;
    pthread_mutex_lock(&v->mutex);
    uint32_t result = DM_VAMM_INVALID;
    if (v->initialized && contains(v, base, size))
        result = v->config.change_pages(v->config.context, base, size, commit);
    pthread_mutex_unlock(&v->mutex); return result;
}
uint32_t dm_vamm_free(dm_vamm *v, uintptr_t base) {
    if (!v) return DM_VAMM_INVALID;
    pthread_mutex_lock(&v->mutex);
    uint32_t result = DM_VAMM_INVALID;
    size_t i = lower_bound(v, base);
    if (!v->initialized || i == v->count || v->regions[i].base != base) goto done;
    result = v->config.change_pages(v->config.context, base, v->regions[i].size, 0);
    if (!result) {
        v->reserved -= v->regions[i].size;
        memmove(v->regions + i, v->regions + i + 1, (v->count - i - 1) * sizeof(region));
        --v->count;
    }
done:
    pthread_mutex_unlock(&v->mutex); return result;
}
static uint32_t get_stats(dm_vamm *v, size_t alignment, int explicit_alignment, dm_vamm_stats *out) {
    if (!v || !out || !power_two(alignment) || (explicit_alignment && alignment < PAGE))
        return DM_VAMM_INVALID;
    pthread_mutex_lock(&v->mutex);
    dm_vamm_stats s = {v->count, v->reserved, sizeof *v + v->config.max_regions * sizeof(region), 0, 0};
    if (v->initialized && v->count < v->config.max_regions) {
        size_t usable_gaps = 0;
        for (size_t i = 0; i <= v->count; ++i) {
            uintptr_t ignored;
            size_t n = explicit_alignment ? gap(v, i, alignment, &ignored) : largest_gap(v, i, alignment);
            s.available_bytes += n; usable_gaps += n != 0;
            if (n > s.largest_region) s.largest_region = n;
        }
        // A metadata slot is needed for each independent lease. Do not claim
        // every hole can be leased when the fixed ledger cannot record them.
        if (usable_gaps > v->config.max_regions - v->count) s.available_bytes = s.largest_region;
    }
    *out = s; pthread_mutex_unlock(&v->mutex); return 0;
}
uint32_t dm_vamm_get_stats(dm_vamm *v, size_t alignment, dm_vamm_stats *out) {
    return get_stats(v, alignment, 0, out);
}
uint32_t dm_vamm_get_aligned_stats(dm_vamm *v, size_t alignment, dm_vamm_stats *out) {
    return get_stats(v, alignment, 1, out);
}
