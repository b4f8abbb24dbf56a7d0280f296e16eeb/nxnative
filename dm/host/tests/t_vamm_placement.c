// Source-only placement contracts. Domains are synthetic integers: callbacks
// never map/dereference them, and no SDK input, offset or instruction is used.
#include "../hle_vamm.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { PAGE = 0x1000, K64 = 0x10000, M2 = 0x200000, M4 = 0x400000,
       M32 = 0x2000000, G1 = 0x40000000 };
typedef struct { unsigned calls; uint32_t failure; uintptr_t base; size_t size; int commit; } fixture;
static uint32_t pages(void *context, uintptr_t base, size_t size, int commit) {
    fixture *f = context; ++f->calls; f->base = base; f->size = size; f->commit = commit;
    return f->failure;
}
static dm_vamm_resource_usage resources(void *context) {
    (void)context; return (dm_vamm_resource_usage){0x100000, 0};
}
static dm_vamm_config config(fixture *f, dm_vamm_placement policy) {
    return (dm_vamm_config){.base = (uintptr_t)4 * G1 + 3 * PAGE,
        .size = (size_t)8 * G1, .max_regions = 16, .context = f,
        .change_pages = pages, .resources = resources, .placement = policy};
}
// Independent piecewise reference, intentionally not the implementation table.
static size_t alignment(dm_vamm_placement policy, size_t size) {
    if (size < M2) return K64;
    if (policy == DM_VAMM_PLACEMENT_MODERN || size < M4) return M2;
    if (size < M32) return M4;
    if (policy == DM_VAMM_PLACEMENT_LEGACY_REDUCED || size < G1) return M32;
    return G1;
}
static uintptr_t start(uintptr_t base, size_t a) { return (base + PAGE + a - 1) & ~(uintptr_t)(a - 1); }
static dm_vamm *create(dm_vamm_config *c) {
    dm_vamm *v = NULL; assert(!dm_vamm_create(&v, c) && v); dm_vamm_initialize(v); return v;
}
static dm_vamm_stats stats(dm_vamm *v, size_t a) {
    dm_vamm_stats s; assert(!dm_vamm_get_stats(v, a, &s)); return s;
}
static void boundaries(void) {
    static const size_t sizes[] = {PAGE, M2 - PAGE, M2, M2 + PAGE, M4 - PAGE,
        M4, M4 + PAGE, M32 - PAGE, M32, M32 + PAGE, G1 - PAGE, G1, G1 + PAGE, (size_t)2 * G1};
    for (unsigned p = DM_VAMM_PLACEMENT_MODERN; p <= DM_VAMM_PLACEMENT_LEGACY_REDUCED; ++p) {
        fixture f = {0}; dm_vamm_config c = config(&f, p); dm_vamm *v = create(&c);
        // Configuration is copied, not a live mutable placement switch.
        c.placement = (dm_vamm_placement)99;
        for (size_t k = 0; k < sizeof sizes / sizeof *sizes; ++k) {
            size_t n = sizes[k], a = alignment(p, n); uintptr_t out = 123;
            unsigned before = f.calls;
            assert(!dm_vamm_allocate(v, &out, n, 0));
            assert(out == start(c.base, a) && f.calls == before);
            assert(stats(v, K64).reserved_bytes == n);
            assert(!dm_vamm_free(v, out));
            assert(f.calls == before + 1 && f.base == out && f.size == n && !f.commit);
            dm_vamm_stats saved = stats(v, K64);
            f.failure = DM_VAMM_NO_MEMORY; out = 123;
            assert(dm_vamm_allocate(v, &out, n, 1) == DM_VAMM_NO_MEMORY && out == 123);
            dm_vamm_stats after = stats(v, K64);
            assert(!memcmp(&saved, &after, sizeof saved));
            f.failure = 0;
            assert(!dm_vamm_allocate(v, &out, n, 1));
            assert(f.base == out && out == start(c.base, a) && f.size == n && f.commit);
            f.failure = 0x803;
            assert(dm_vamm_free(v, out) == 0x803 && stats(v, K64).regions == 1);
            f.failure = 0; assert(!dm_vamm_free(v, out));
        }
        // Explicit native leases are independent of SDK tiers in every family.
        uintptr_t out; assert(!dm_vamm_allocate_aligned(v, &out, M32, PAGE, 0));
        assert(out == c.base + PAGE && out % M32);
        assert(!dm_vamm_free(v, out)); assert(!dm_vamm_destroy(v));
    }
}
static size_t brute_gap(dm_vamm_placement p, uintptr_t lo, uintptr_t hi, size_t minimum_alignment) {
    size_t best = 0;
    for (size_t n = PAGE; n <= hi - lo; n += PAGE) {
        size_t a = alignment(p, n); if (minimum_alignment > a) a = minimum_alignment;
        uintptr_t first = start(lo, a);
        if (first <= hi && hi - first >= PAGE && n <= hi - first - PAGE) best = n;
    }
    return best;
}
static void realizable_stats(void) {
    static const size_t extents[] = {M2 + K64, M4 + K64, M32 + K64, G1 + M32};
    static const size_t constraints[] = {PAGE, K64, M2, M4, M32, G1};
    for (unsigned p = DM_VAMM_PLACEMENT_MODERN; p <= DM_VAMM_PLACEMENT_LEGACY_REDUCED; ++p)
    for (size_t k = 0; k < sizeof extents / sizeof *extents; ++k)
    for (size_t q = 0; q < sizeof constraints / sizeof *constraints; ++q) {
        fixture f = {0}; dm_vamm_config c = config(&f, p); c.size = extents[k];
        dm_vamm *v = create(&c); size_t constraint = constraints[q];
        size_t expected = brute_gap(p, c.base, c.base + c.size, constraint);
        dm_vamm_stats s = stats(v, constraint);
        assert(s.largest_region == expected && s.available_bytes == expected);
        uintptr_t out = 123;
        size_t a = alignment(p, expected); if (constraint > a) a = constraint;
        if (expected) {
            assert(!dm_vamm_allocate_aligned(v, &out, expected, a, 0));
            assert(!dm_vamm_free(v, out));
        }
        size_t too_large = expected + PAGE; a = alignment(p, too_large); if (constraint > a) a = constraint;
        out = 123;
        assert(dm_vamm_allocate_aligned(v, &out, too_large, a, 0) == DM_VAMM_NO_SPACE && out == 123);
        assert(!dm_vamm_destroy(v));
    }
}
static void fragmented_stats(void) {
    for (unsigned p = DM_VAMM_PLACEMENT_MODERN; p <= DM_VAMM_PLACEMENT_LEGACY_REDUCED; ++p) {
        fixture f = {0}; dm_vamm_config c = config(&f, p); c.size = (size_t)3 * G1 + M32;
        dm_vamm *v = create(&c); uintptr_t a, b, d;
        assert(!dm_vamm_allocate(v, &a, M4, 0));
        assert(!dm_vamm_allocate(v, &b, M32, 0));
        assert(!dm_vamm_allocate(v, &d, G1, 0));
        assert(!dm_vamm_free(v, b));
        size_t holes[] = {brute_gap(p, c.base, a, K64), brute_gap(p, a + M4, d, K64),
            brute_gap(p, d + G1, c.base + c.size, K64)};
        size_t sum = 0, largest = 0;
        for (size_t i = 0; i < 3; ++i) { sum += holes[i]; if (holes[i] > largest) largest = holes[i]; }
        dm_vamm_stats s = stats(v, K64); assert(s.available_bytes == sum && s.largest_region == largest);
        assert(!dm_vamm_allocate(v, &b, largest, 0)); assert(!dm_vamm_free(v, b));
        assert(!dm_vamm_free(v, a)); assert(!dm_vamm_free(v, d)); assert(!dm_vamm_destroy(v));
    }
}
static void invalid_and_overflow(void) {
    fixture f = {0}; dm_vamm_config c = config(&f, DM_VAMM_PLACEMENT_MODERN);
    dm_vamm *v = NULL; c.placement = (dm_vamm_placement)99;
    assert(dm_vamm_create(&v, &c) == DM_VAMM_INVALID && !v && !f.calls);
    c.placement = (dm_vamm_placement)-1;
    assert(dm_vamm_create(&v, &c) == DM_VAMM_INVALID && !v && !f.calls);
    for (unsigned p = DM_VAMM_PLACEMENT_MODERN; p <= DM_VAMM_PLACEMENT_LEGACY_REDUCED; ++p) {
        c.placement = p; c.base = UINTPTR_MAX - M32 + 1; c.size = M32 - PAGE;
        v = create(&c); uintptr_t out = 123;
        assert(!stats(v, G1).largest_region);
        assert(dm_vamm_allocate(v, &out, G1, 0) == DM_VAMM_NO_SPACE && out == 123);
        assert(!dm_vamm_destroy(v));
    }
}
int main(void) {
    _Static_assert(sizeof(uintptr_t) == 8, "64-bit synthetic address domain");
    boundaries(); realizable_stats(); fragmented_stats(); invalid_and_overflow();
    puts("Vamm placement families: tier boundaries, realizable/fragmented stats, rollback, overflow: PASS");
    return 0;
}
