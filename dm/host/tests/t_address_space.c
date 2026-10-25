// Production address-region core tests, independent of profiles and Apple SDKs.
// Every tested address belongs to a real mmap reservation. A 16 KiB backend
// exercises Apple's host-page sharing even on Linux with 4 KiB native pages.
#define _GNU_SOURCE
#include "hle_address_space.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#define P ((size_t)4096)
#define H ((size_t)16384)
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); } } while (0)
#define OK(expr) CHECK((expr) == DM_AS_OK)

typedef struct {
    void *mapping;
    size_t mapping_size, size;
    uintptr_t base;
    dm_address_space *space;
    size_t calls, last_size, initialized_bytes;
    uintptr_t last_base;
    int last_commit, fail_next;
    unsigned char *host_accessible;
} fixture;

static uintptr_t align_up(uintptr_t n, size_t alignment) {
    return (n + alignment - 1) & ~(uintptr_t)(alignment - 1);
}
static int apply(void *context, uintptr_t base, size_t size, int commit) {
    fixture *f = context;
    CHECK(base >= f->base && size <= f->size && base - f->base <= f->size - size);
    CHECK(!(base % H) && !(size % H) && size);
    f->calls++;
    f->last_base = base;
    f->last_size = size;
    f->last_commit = commit;
    if (f->fail_next) { f->fail_next = 0; errno = ENOMEM; return -1; }
    if (mprotect((void *)base, size, commit ? PROT_READ | PROT_WRITE : PROT_NONE)) return -1;
    memset(f->host_accessible + (base - f->base) / H, commit != 0, size / H);
    return 0;
}
static void initialize(void *context, uintptr_t base, size_t size) {
    fixture *f = context;
    f->initialized_bytes += size;
    memset((void *)base, 0, size);
}
static void setup(fixture *f, size_t size, size_t max_regions, dm_as_mode mode) {
    memset(f, 0, sizeof *f);
    CHECK(size && !(size % H));
    CHECK((size_t)sysconf(_SC_PAGESIZE) <= H && !(H % (size_t)sysconf(_SC_PAGESIZE)));
    f->mapping_size = size + H;
    f->mapping = mmap(NULL, f->mapping_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(f->mapping != MAP_FAILED);
    f->base = align_up((uintptr_t)f->mapping, H);
    f->size = size;
    f->host_accessible = calloc(size / H, 1);
    CHECK(f->host_accessible);
    dm_as_config config = { f->base, size, P, H, max_regions, mode, apply, f, initialize, 0 };
    OK(dm_as_create(&f->space, &config));
}
static dm_as_stats stats(fixture *f) {
    dm_as_stats s;
    memset(&s, 0, sizeof s);
    OK(dm_as_get_stats(f->space, &s));
    CHECK(s.base == f->base && s.size == f->size);
    return s;
}
static void finish(fixture *f) {
    CHECK(stats(f).committed_bytes == 0);
    OK(dm_as_destroy(f->space));
    CHECK(munmap(f->mapping, f->mapping_size) == 0);
    free(f->host_accessible);
}
static dm_as_range query(fixture *f, uintptr_t address) {
    dm_as_range range;
    memset(&range, 0, sizeof range);
    OK(dm_as_query(f->space, address, &range));
    CHECK(range.base <= address && address - range.base < range.size);
    CHECK(range.base >= f->base && range.size <= f->size && range.base - f->base <= f->size - range.size);
    return range;
}
static void test_validation(void) {
    fixture f;
    setup(&f, 64 * P, 8, DM_AS_HOST_RESERVATIONS);
    dm_as_config c = {f.base, f.size, P, H, 8, DM_AS_HOST_RESERVATIONS, apply, &f, initialize, 0};
    dm_address_space *sentinel = (dm_address_space *)(uintptr_t)1;
    CHECK(dm_as_create(NULL, &c) == DM_AS_INVALID);
    CHECK(dm_as_create(&sentinel, NULL) == DM_AS_INVALID);
    CHECK(sentinel == (dm_address_space *)(uintptr_t)1);
    dm_as_config invalid[] = {
        {0, f.size, P, H, 8, DM_AS_HOST_RESERVATIONS, apply, &f, initialize, 0},
        {f.base + P, f.size, P, H, 8, DM_AS_HOST_RESERVATIONS, apply, &f, initialize, 0},
        {f.base, 0, P, H, 8, DM_AS_HOST_RESERVATIONS, apply, &f, initialize, 0},
        {f.base, f.size - P, P, H, 8, DM_AS_HOST_RESERVATIONS, apply, &f, initialize, 0},
        {f.base, f.size, 3 * P, H, 8, DM_AS_HOST_RESERVATIONS, apply, &f, initialize, 0},
        {f.base, f.size, 2 * H, H, 8, DM_AS_HOST_RESERVATIONS, apply, &f, initialize, 0},
        {UINTPTR_MAX & ~(uintptr_t)(H - 1), H, P, H, 8, DM_AS_HOST_RESERVATIONS, apply, &f, initialize, 0},
        {f.base, f.size, P, H, 8, (dm_as_mode)42, apply, &f, initialize, 0}
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        CHECK(dm_as_create(&sentinel, &invalid[i]) == DM_AS_INVALID);
        CHECK(sentinel == (dm_address_space *)(uintptr_t)1);
    }
    uintptr_t out = 17;
    CHECK(dm_as_reserve(f.space, 0, P, &out) == DM_AS_INVALID);
    CHECK(dm_as_reserve(f.space, P + 1, P, &out) == DM_AS_INVALID);
    CHECK(dm_as_reserve(f.space, P, 0, &out) == DM_AS_INVALID);
    CHECK(dm_as_reserve(f.space, P, P / 2, &out) == DM_AS_INVALID);
    CHECK(dm_as_reserve(f.space, P, 3 * P, &out) == DM_AS_INVALID);
    CHECK(dm_as_reserve(f.space, P, P, NULL) == DM_AS_INVALID);
    CHECK(out == 17);
    CHECK(dm_as_reserve(f.space, f.size + P, P, &out) == DM_AS_NO_SPACE);
    CHECK(out == 17);
    CHECK(dm_as_commit(f.space, f.base, P, SIZE_MAX) != DM_AS_OK); // no reservation
    CHECK(dm_as_decommit(f.space, f.base, P) != DM_AS_OK);
    CHECK(dm_as_commit(f.space, f.base, 0, SIZE_MAX) == DM_AS_INVALID);
    CHECK(dm_as_commit(f.space, f.base + 1, P, SIZE_MAX) == DM_AS_INVALID);
    CHECK(dm_as_commit(f.space, f.base, P + 1, SIZE_MAX) == DM_AS_INVALID);
    CHECK(dm_as_commit(f.space, f.base - P, P, SIZE_MAX) == DM_AS_INVALID);
    CHECK(dm_as_commit(f.space, f.base + f.size, P, SIZE_MAX) == DM_AS_INVALID);
    CHECK(dm_as_commit(f.space, UINTPTR_MAX - P + 1, 2 * P, SIZE_MAX) == DM_AS_INVALID);
    dm_as_range range, before;
    memset(&range, 0x5a, sizeof range); before = range;
    CHECK(dm_as_query(f.space, f.base - 1, &range) == DM_AS_INVALID);
    CHECK(!memcmp(&range, &before, sizeof range));
    CHECK(dm_as_query(f.space, f.base + f.size, &range) == DM_AS_INVALID);
    CHECK(dm_as_query(f.space, f.base, NULL) == DM_AS_INVALID);
    size_t count = 123;
    CHECK(dm_as_committed_in(f.space, f.base + f.size, P, &count) == DM_AS_INVALID);
    CHECK(count == 123);
    CHECK(f.calls == 0);
    finish(&f);
}
static void test_reservation_and_commit(void) {
    fixture f;
    setup(&f, 256 * P, 16, DM_AS_HOST_RESERVATIONS);
    uintptr_t a;
    OK(dm_as_reserve(f.space, 3 * P, 16 * P, &a));
    CHECK(a == align_up(f.base, 16 * P));
    CHECK(a >= f.base && a + 3 * P <= f.base + f.size);
    dm_as_stats s = stats(&f);
    CHECK(s.regions == 1 && s.reserved_bytes == 3 * P && s.committed_bytes == 0);
    CHECK(s.free_bytes == f.size - 3 * P && s.metadata_bytes < f.size);
    CHECK(f.calls == 0); // reservations do not commit memory or call the backend
    dm_as_range r = query(&f, a + 123);
    CHECK(r.base == a && r.size == 3 * P && r.reserved && !r.committed);
    CHECK(r.reservation_base == a && r.reservation_size == 3 * P);
    CHECK(dm_as_destroy(f.space) == DM_AS_BUSY);
    CHECK(dm_as_commit(f.space, a, 3 * P, 2 * P) == DM_AS_LIMIT);
    CHECK(f.calls == 0 && stats(&f).committed_bytes == 0);
    OK(dm_as_commit(f.space, a, 2 * P, 3 * P));
    memset((void *)a, 0x6d, 2 * P);
    OK(dm_as_commit(f.space, a + P, 2 * P, 3 * P));
    CHECK(*(unsigned char *)a == 0x6d); // overlapping commit must retain contents
    CHECK(stats(&f).committed_bytes == 3 * P);
    OK(dm_as_commit(f.space, a, 3 * P, 3 * P));
    CHECK(stats(&f).committed_bytes == 3 * P);
    size_t count = 0;
    OK(dm_as_committed_in(f.space, a + P, 2 * P, &count)); CHECK(count == 2 * P);
    r = query(&f, a + P + 1);
    CHECK(r.base == a && r.size == 3 * P && r.reserved && r.committed);
    CHECK(dm_as_release(f.space, a, 3 * P) == DM_AS_BUSY);
    OK(dm_as_decommit(f.space, a + P, P));
    CHECK(stats(&f).committed_bytes == 2 * P);
    CHECK(*(unsigned char *)a == 0x6d);
    r = query(&f, a + P);
    CHECK(r.base == a + P && r.size == P && !r.committed && r.reserved);
    CHECK(query(&f, a).size == P && query(&f, a + 2 * P).size == P);
    CHECK(dm_as_release(f.space, a, 2 * P) != DM_AS_OK);
    CHECK(dm_as_release(f.space, a + P, 2 * P) != DM_AS_OK);
    OK(dm_as_decommit(f.space, a, 3 * P));
    OK(dm_as_decommit(f.space, a, 3 * P));
    CHECK(stats(&f).committed_bytes == 0);
    OK(dm_as_release(f.space, a, 3 * P));
    CHECK(dm_as_release(f.space, a, 3 * P) != DM_AS_OK);
    s = stats(&f);
    CHECK(s.regions == 0 && s.free_bytes == f.size && s.largest_free == f.size);
    uintptr_t whole;
    OK(dm_as_reserve(f.space, f.size, P, &whole)); CHECK(whole == f.base);
    CHECK(stats(&f).reserved_bytes == f.size && stats(&f).committed_bytes == 0);
    OK(dm_as_release(f.space, whole, f.size));
    finish(&f);
}
static void test_fragmentation_and_metadata(void) {
    fixture f;
    setup(&f, 32 * P, 8, DM_AS_HOST_RESERVATIONS);
    uintptr_t a[8], combined;
    for (size_t i = 0; i < 8; i++) {
        OK(dm_as_reserve(f.space, 4 * P, P, &a[i])); CHECK(a[i] == f.base + i * 4 * P);
    }
    for (size_t i = 0; i < 8; i += 2) OK(dm_as_release(f.space, a[i], 4 * P));
    CHECK(stats(&f).free_bytes == 16 * P && stats(&f).largest_free == 4 * P);
    combined = 17;
    CHECK(dm_as_reserve(f.space, 8 * P, P, &combined) == DM_AS_NO_SPACE); CHECK(combined == 17);
    OK(dm_as_release(f.space, a[1], 4 * P));
    CHECK(stats(&f).largest_free == 12 * P);
    OK(dm_as_reserve(f.space, 12 * P, P, &combined)); CHECK(combined == f.base);
    OK(dm_as_release(f.space, combined, 12 * P));
    for (size_t i = 3; i < 8; i += 2) OK(dm_as_release(f.space, a[i], 4 * P));
    CHECK(stats(&f).largest_free == f.size);
    finish(&f);

    setup(&f, 64 * P, 2, DM_AS_HOST_RESERVATIONS);
    size_t bytes = stats(&f).metadata_bytes;
    OK(dm_as_reserve(f.space, P, P, &a[0])); OK(dm_as_reserve(f.space, P, P, &a[1]));
    combined = 17;
    CHECK(dm_as_reserve(f.space, P, P, &combined) == DM_AS_METADATA_FULL); CHECK(combined == 17);
    CHECK(stats(&f).free_bytes == f.size - 2 * P && stats(&f).metadata_bytes == bytes);
    OK(dm_as_release(f.space, a[0], P));
    OK(dm_as_reserve(f.space, P, P, &combined)); CHECK(combined == a[0]);
    OK(dm_as_release(f.space, combined, P)); OK(dm_as_release(f.space, a[1], P));
    CHECK(stats(&f).metadata_bytes == bytes);
    finish(&f);
}
static void test_failures_and_host_neighbors(void) {
    fixture f;
    setup(&f, 16 * P, 8, DM_AS_HOST_RESERVATIONS);
    uintptr_t a[4];
    for (size_t i = 0; i < 4; i++) OK(dm_as_reserve(f.space, P, P, &a[i]));
    CHECK(a[0] == f.base && a[3] == f.base + 3 * P);
    f.fail_next = 1;
    CHECK(dm_as_commit(f.space, a[0], P, SIZE_MAX) == DM_AS_BACKEND_FAILED);
    CHECK(stats(&f).committed_bytes == 0 && !query(&f, a[0]).committed);
    OK(dm_as_commit(f.space, a[0], P, SIZE_MAX));
    *(unsigned char *)a[0] = 0x6a;
    OK(dm_as_commit(f.space, a[3], P, SIZE_MAX));
    *(unsigned char *)a[3] = 0xb7;
    CHECK(stats(&f).committed_bytes == 2 * P);
    CHECK(dm_as_commit(f.space, a[1], P, 2 * P) == DM_AS_LIMIT);
    CHECK(!query(&f, a[1]).committed);
    // A range may not cross separate reservations, even if both are live.
    CHECK(dm_as_commit(f.space, a[0], 2 * P, SIZE_MAX) != DM_AS_OK);
    OK(dm_as_decommit(f.space, a[0], P));
    CHECK(f.host_accessible[0] && *(unsigned char *)a[3] == 0xb7);
    CHECK(stats(&f).committed_bytes == P);
    // Logical decommit cannot revoke one 4 KiB slice of a live 16 KiB host
    // page. Recommit initializes only the formerly free logical slice, without
    // touching the live sibling; failure must not clear retained bytes.
    CHECK(*(unsigned char *)a[0] == 0x6a && !query(&f, a[0]).committed);
    f.fail_next = 1;
    CHECK(dm_as_commit(f.space, a[0], P, SIZE_MAX) == DM_AS_BACKEND_FAILED);
    CHECK(*(unsigned char *)a[0] == 0x6a && !query(&f, a[0]).committed);
    OK(dm_as_commit(f.space, a[0], P, SIZE_MAX));
    CHECK(*(unsigned char *)a[0] == 0 && *(unsigned char *)a[3] == 0xb7);
    OK(dm_as_decommit(f.space, a[0], P));
    f.fail_next = 1;
    CHECK(dm_as_decommit(f.space, a[3], P) == DM_AS_BACKEND_FAILED);
    CHECK(stats(&f).committed_bytes == P && query(&f, a[3]).committed);
    CHECK(f.host_accessible[0] && *(unsigned char *)a[3] == 0xb7);
    CHECK(dm_as_release(f.space, a[3], P) == DM_AS_BUSY);
    OK(dm_as_decommit(f.space, a[3], P));
    CHECK(!f.host_accessible[0]);
    CHECK(f.last_base == f.base && f.last_size == H && !f.last_commit);
    for (size_t i = 0; i < 4; i++) OK(dm_as_release(f.space, a[i], P));
    finish(&f);
}
static void test_lazy_native_zero(void) {
    fixture f;
    setup(&f, 4 * H, 0, DM_AS_EXTERNAL_RESERVATIONS);
    OK(dm_as_destroy(f.space));
    dm_as_config c = {f.base, f.size, P, H, 0, DM_AS_EXTERNAL_RESERVATIONS, apply, &f, initialize, 1};
    OK(dm_as_create(&f.space, &c));
    OK(dm_as_commit(f.space, f.base, P, SIZE_MAX));
    CHECK(f.initialized_bytes == 0 && *(unsigned char *)f.base == 0);
    *(unsigned char *)f.base = 0x6a;
    // Simulate a native write through an uncommitted 4K sibling. Its first
    // logical commit is not a first host-page exposure and must clear it.
    *(unsigned char *)(f.base + P) = 0xbc;
    OK(dm_as_commit(f.space, f.base + P, P, SIZE_MAX));
    CHECK(f.initialized_bytes == P && *(unsigned char *)(f.base + P) == 0);
    CHECK(*(unsigned char *)f.base == 0x6a);
    OK(dm_as_commit(f.space, f.base + H, H, SIZE_MAX));
    CHECK(f.initialized_bytes == P); // no eager memset of fresh native pages
    *(unsigned char *)(f.base + H) = 0x31;
    OK(dm_as_decommit(f.space, f.base + H, H));
    OK(dm_as_commit(f.space, f.base + H, H, SIZE_MAX));
    CHECK(f.initialized_bytes == P + H && *(unsigned char *)(f.base + H) == 0);
    f.fail_next = 1;
    CHECK(dm_as_commit(f.space, f.base + 2 * H, H, SIZE_MAX) == DM_AS_BACKEND_FAILED);
    CHECK(f.initialized_bytes == P + H);
    OK(dm_as_commit(f.space, f.base + 2 * H, H, SIZE_MAX));
    CHECK(f.initialized_bytes == P + H); // failed apply did not consume virgin history
    OK(dm_as_decommit(f.space, f.base, f.size));
    finish(&f);
}
static void test_external_reservations(void) {
    fixture f;
    setup(&f, 256 * P, 8, DM_AS_EXTERNAL_RESERVATIONS);
    uintptr_t out = 17;
    CHECK(dm_as_reserve(f.space, P, P, &out) == DM_AS_WRONG_MODE && out == 17);
    CHECK(dm_as_release(f.space, f.base, P) == DM_AS_WRONG_MODE);
    CHECK(stats(&f).mode == DM_AS_EXTERNAL_RESERVATIONS);
    // Spans bitmap word boundaries (page 63/64), with overlapping retries.
    OK(dm_as_commit(f.space, f.base + 62 * P, 5 * P, SIZE_MAX));
    OK(dm_as_commit(f.space, f.base + 64 * P, 4 * P, SIZE_MAX));
    CHECK(stats(&f).committed_bytes == 6 * P);
    dm_as_range r = query(&f, f.base + 64 * P + 23);
    CHECK(r.base == f.base + 62 * P && r.size == 6 * P && r.committed && !r.reserved);
    CHECK(query(&f, f.base).size == 62 * P);
    r = query(&f, f.base + 68 * P);
    CHECK(r.base == f.base + 68 * P && r.size == f.size - 68 * P && !r.committed);
    CHECK(dm_as_destroy(f.space) == DM_AS_BUSY);
    OK(dm_as_decommit(f.space, f.base + 63 * P, 3 * P));
    CHECK(stats(&f).committed_bytes == 3 * P);
    OK(dm_as_decommit(f.space, f.base, f.size));
    OK(dm_as_decommit(f.space, f.base, f.size));
    CHECK(stats(&f).committed_bytes == 0 && stats(&f).regions == 0);
    finish(&f);
}

static void test_multi_gib_lazy_commit(void) {
    if (SIZE_MAX <= UINT32_MAX) return;
    fixture f;
    size_t size = (size_t)3 << 30;
    setup(&f, size, 0, DM_AS_EXTERNAL_RESERVATIONS);
    OK(dm_as_destroy(f.space));
    dm_as_config c = {f.base, f.size, P, H, 0, DM_AS_EXTERNAL_RESERVATIONS, apply, &f, initialize, 1};
    OK(dm_as_create(&f.space, &c));
    OK(dm_as_commit(f.space, f.base, size, SIZE_MAX));
    CHECK(f.initialized_bytes == 0 && stats(&f).committed_bytes == size);
    // Large startup probes remain lazy, but any reused slice gets initialized.
    OK(dm_as_decommit(f.space, f.base, size));
    OK(dm_as_commit(f.space, f.base + size - P, P, SIZE_MAX));
    CHECK(f.initialized_bytes == P && *(unsigned char *)(f.base + size - P) == 0);
    OK(dm_as_decommit(f.space, f.base, size));
    finish(&f);
}

static uint32_t random32(uint32_t *state) {
    uint32_t x = *state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *state = x;
}
static void test_randomized_page_model(void) {
    enum { NPAGES = 256, STEPS = 12000 };
    fixture f;
    setup(&f, NPAGES * P, 8, DM_AS_EXTERNAL_RESERVATIONS);
    unsigned char committed[NPAGES] = {0};
    uint32_t rng = 0x6e786173;
    size_t total = 0;
    for (size_t step = 0; step < STEPS; step++) {
        size_t first = random32(&rng) % NPAGES;
        size_t count = 1 + random32(&rng) % (NPAGES - first);
        int map = random32(&rng) & 1;
        size_t old = 0;
        for (size_t i = first; i < first + count; i++) old += committed[i];
        size_t proposed = map ? total + count - old : total - old;
        size_t limit = (random32(&rng) % (NPAGES + 1)) * P;
        dm_as_result result = map ? dm_as_commit(f.space, f.base + first * P, count * P, limit)
                                  : dm_as_decommit(f.space, f.base + first * P, count * P);
        if (map && proposed != total && proposed * P > limit) CHECK(result == DM_AS_LIMIT);
        else {
            CHECK(result == DM_AS_OK);
            memset(committed + first, map, count);
            total = proposed;
        }
        CHECK(stats(&f).committed_bytes == total * P);
        size_t measured;
        OK(dm_as_committed_in(f.space, f.base, f.size, &measured)); CHECK(measured == total * P);
        size_t at = random32(&rng) % NPAGES, lo = at, hi = at + 1;
        while (lo && committed[lo - 1] == committed[at]) lo--;
        while (hi < NPAGES && committed[hi] == committed[at]) hi++;
        dm_as_range q = query(&f, f.base + at * P + 31);
        CHECK(q.base == f.base + lo * P && q.size == (hi - lo) * P && q.committed == committed[at]);
        // Every logically live page must still have a writable host page.
        for (size_t i = 0; i < NPAGES; i += 4) {
            int any = committed[i] || committed[i + 1] || committed[i + 2] || committed[i + 3];
            CHECK(f.host_accessible[i / 4] == any);
        }
    }
    OK(dm_as_decommit(f.space, f.base, f.size));
    finish(&f);
}

static void test_randomized_reservation_model(void) {
    enum { NPAGES = 128, NSLOTS = 16, STEPS = 10000 };
    fixture f;
    setup(&f, NPAGES * P, NSLOTS, DM_AS_HOST_RESERVATIONS);
    int owner[NPAGES] = {0};
    struct { uintptr_t base; size_t size; } slots[NSLOTS] = {{0}};
    uint32_t rng = 0x1a34bca8;
    size_t count = 0, used = 0, metadata = stats(&f).metadata_bytes;
    for (size_t step = 0; step < STEPS; step++) {
        size_t slot = random32(&rng) % NSLOTS;
        if (slots[slot].size && (random32(&rng) & 1)) {
            OK(dm_as_release(f.space, slots[slot].base, slots[slot].size));
            size_t first = (slots[slot].base - f.base) / P;
            for (size_t j = first; j < first + slots[slot].size / P; j++) {
                CHECK(owner[j] == (int)slot + 1); owner[j] = 0;
            }
            used -= slots[slot].size; count--; slots[slot].size = 0;
        } else {
            size_t n = 1 + random32(&rng) % 24, alignment = P << (random32(&rng) % 6);
            uintptr_t expected = 0, actual = 17;
            for (size_t first = 0; first + n <= NPAGES; first++) {
                if ((f.base + first * P) % alignment) continue;
                size_t j = 0; while (j < n && !owner[first + j]) j++;
                if (j == n) { expected = f.base + first * P; break; }
            }
            dm_as_result result = dm_as_reserve(f.space, n * P, alignment, &actual);
            if (count == NSLOTS) CHECK(result == DM_AS_METADATA_FULL && actual == 17);
            else if (!expected) CHECK(result == DM_AS_NO_SPACE && actual == 17);
            else {
                CHECK(result == DM_AS_OK && actual == expected);
                for (slot = 0; slot < NSLOTS && slots[slot].size; slot++) {}
                CHECK(slot < NSLOTS);
                slots[slot].base = actual; slots[slot].size = n * P;
                size_t first = (actual - f.base) / P;
                for (size_t j = first; j < first + n; j++) { CHECK(!owner[j]); owner[j] = (int)slot + 1; }
                count++; used += n * P;
            }
        }
        size_t largest = 0, gap = 0;
        for (size_t i = 0; i < NPAGES; i++) {
            gap = owner[i] ? 0 : gap + P;
            if (gap > largest) largest = gap;
        }
        dm_as_stats st = stats(&f);
        CHECK(st.regions == count && st.reserved_bytes == used && !st.committed_bytes);
        CHECK(st.free_bytes == f.size - used && st.largest_free == largest && st.metadata_bytes == metadata);
        size_t at = random32(&rng) % NPAGES, lo = at, hi = at + 1;
        while (lo && owner[lo - 1] == owner[at]) lo--;
        while (hi < NPAGES && owner[hi] == owner[at]) hi++;
        dm_as_range q = query(&f, f.base + at * P + 7);
        CHECK(q.base == f.base + lo * P && q.size == (hi - lo) * P && q.reserved == (owner[at] != 0));
        CHECK(!q.committed);
        if (owner[at]) {
            CHECK(q.reservation_base == slots[owner[at] - 1].base);
            CHECK(q.reservation_size == slots[owner[at] - 1].size);
        }
    }
    for (size_t i = 0; i < NSLOTS; i++) if (slots[i].size) OK(dm_as_release(f.space, slots[i].base, slots[i].size));
    CHECK(stats(&f).largest_free == f.size && !f.calls);
    finish(&f);
}

typedef struct { fixture *f; unsigned id; } worker_arg;
static void *worker(void *arg) {
    worker_arg *w = arg;
    fixture *f = w->f;
    uint32_t rng = 0x91ae3b2d ^ w->id;
    for (size_t i = 0; i < 1200; i++) {
        size_t size = (1 + random32(&rng) % 8) * P;
        size_t alignment = P << (random32(&rng) % 5);
        uintptr_t a;
        OK(dm_as_reserve(f->space, size, alignment, &a));
        CHECK(!(a % alignment));
        OK(dm_as_commit(f->space, a, size, f->size));
        memset((void *)a, (int)(w->id + 1), size);
        dm_as_range r = query(f, a + size - 1);
        CHECK(r.reserved && r.committed && r.reservation_base == a && r.reservation_size == size);
        CHECK(*(unsigned char *)a == w->id + 1 && *(unsigned char *)(a + size - 1) == w->id + 1);
        OK(dm_as_decommit(f->space, a, size));
        OK(dm_as_release(f->space, a, size));
    }
    return NULL;
}
static void test_concurrent_allocators(void) {
    enum { NTHREADS = 8 };
    fixture f;
    setup(&f, 4096 * P, 64, DM_AS_HOST_RESERVATIONS);
    pthread_t threads[NTHREADS];
    worker_arg args[NTHREADS];
    for (unsigned i = 0; i < NTHREADS; i++) {
        args[i] = (worker_arg){&f, i};
        CHECK(!pthread_create(&threads[i], NULL, worker, &args[i]));
    }
    for (unsigned i = 0; i < NTHREADS; i++) CHECK(!pthread_join(threads[i], NULL));
    dm_as_stats s = stats(&f);
    CHECK(!s.regions && !s.reserved_bytes && !s.committed_bytes);
    CHECK(s.free_bytes == f.size && s.largest_free == f.size);
    finish(&f);
}
int main(void) {
    test_validation();
    test_reservation_and_commit();
    test_fragmentation_and_metadata();
    test_failures_and_host_neighbors();
    test_lazy_native_zero();
    test_multi_gib_lazy_commit();
    test_external_reservations();
    test_randomized_page_model();
    test_randomized_reservation_model();
    test_concurrent_allocators();
    puts("address-space: validation, native reserve/reuse/coalescing/alignment, bounded metadata, commit/rollback, 16K neighbors, model stress and concurrency passed");
    return 0;
}
