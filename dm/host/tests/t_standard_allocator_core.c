// Portable, synthetic core-contract tests: no Nintendo binaries, profiles or
// guest-ABI assumptions. The small real mmap arena is a test memory owner, not
// an implementation of the production guest owner or its host-page policy.
#define _GNU_SOURCE
#include "standard_allocator_core.h"
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
#define PAGE ((size_t)4096)
#define ARENA (8 * 1024 * 1024u)
#define MAX_ALIGN (1024 * 1024u)
#define MAX_LEASES 512
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "[RED] %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define OK(x) CHECK((x) == DM_SA_OK)
#define SENTINEL ((void *)(uintptr_t)0x123)

typedef struct { dm_sa_span span; int live; } lease;
typedef union { max_align_t align; size_t size; } metadata_header;
typedef struct {
    pthread_mutex_t mutex, metadata_mutex;
    pthread_t owner;
    int locked;
    void *mapping;
    unsigned char *base;
    size_t mapping_size, granule, budget, used, peak, largest_request;
    size_t acquire_calls, release_calls, query_calls, available_calls;
    size_t fail_acquire_at, fail_release_at, fail_query_at;
    size_t metadata_calls, metadata_fail_at, metadata_live, metadata_bytes;
    uintptr_t next_token;
    unsigned char pages[ARENA / PAGE];
    lease leases[MAX_LEASES];
    uintptr_t borrowed_base;
    size_t borrowed_size;
    size_t borrow_on_acquire_at, borrow_on_query_at, acquire_borrow_size;
    uintptr_t acquire_borrow_base; // zero selects the newly acquired span
    void *acquire_protect_base;
    size_t acquire_protect_size;
} fixture;

static uintptr_t align_up(uintptr_t n, size_t a) {
    CHECK(a && !(a & (a - 1)) && n <= UINTPTR_MAX - (a - 1));
    return (n + a - 1) & ~(uintptr_t)(a - 1);
}
static int overlaps(uintptr_t a, size_t an, uintptr_t b, size_t bn) {
    return an && bn && a < b + bn && b < a + an;
}
static void provider_begin(void *context) {
    fixture *f = context;
    CHECK(!pthread_mutex_lock(&f->mutex));
    CHECK(!f->locked);
    f->locked = 1;
    f->owner = pthread_self();
}
static void scope(fixture *f) {
    CHECK(f->locked && pthread_equal(f->owner, pthread_self()));
}
static void provider_end(void *context) {
    fixture *f = context;
    scope(f);
    f->locked = 0;
    CHECK(!pthread_mutex_unlock(&f->mutex));
}
static lease *find_lease(fixture *f, const dm_sa_span *s) {
    for (size_t i = 0; i < MAX_LEASES; i++)
        if (f->leases[i].live && f->leases[i].span.token == s->token &&
            f->leases[i].span.base == s->base && f->leases[i].span.size == s->size)
            return &f->leases[i];
    return NULL;
}
static int provider_available(void *context, const dm_sa_span *s, size_t offset, size_t size) {
    fixture *f = context;
    scope(f);
    f->available_calls++;
    CHECK(find_lease(f, s));
    CHECK(offset <= s->size && size <= s->size - offset);
    return !overlaps((uintptr_t)s->base + offset, size, f->borrowed_base, f->borrowed_size);
}
static dm_sa_result provider_acquire(void *context, size_t size, size_t alignment, dm_sa_span *out) {
    fixture *f = context;
    scope(f);
    CHECK(size && !(size % f->granule));
    CHECK(alignment >= 16 && !(alignment & (alignment - 1)));
    CHECK(out);
    f->acquire_calls++;
    if (size > f->largest_request) f->largest_request = size;
    if (f->acquire_calls == f->fail_acquire_at) return DM_SA_PROVIDER_FAILED;
    if (size > f->budget - f->used || size > ARENA || alignment > MAX_ALIGN) return DM_SA_NO_MEMORY;
    size_t n = size / PAGE;
    for (size_t p = 0; p + n <= ARENA / PAGE; p++) {
        uintptr_t address = (uintptr_t)f->base + p * PAGE;
        if (address % alignment) continue;
        size_t j;
        for (j = 0; j < n && !f->pages[p + j]; j++) {}
        if (j != n) { p += j; continue; }
        for (size_t i = 0; i < MAX_LEASES; i++) {
            if (f->leases[i].live) continue;
            dm_sa_span s = {(void *)address, size, ++f->next_token};
            f->leases[i] = (lease){s, 1};
            memset(f->pages + p, 1, n);
            f->used += size;
            if (f->used > f->peak) f->peak = f->used;
            if (f->acquire_calls == f->borrow_on_acquire_at) {
                f->borrowed_base = f->acquire_borrow_base ? f->acquire_borrow_base : address;
                f->borrowed_size = f->acquire_borrow_size;
                if (f->acquire_protect_base) CHECK(!mprotect(f->acquire_protect_base, f->acquire_protect_size, PROT_NONE));
            }
            *out = s;
            return DM_SA_OK;
        }
        return DM_SA_NO_MEMORY;
    }
    return DM_SA_NO_MEMORY;
}
static dm_sa_result provider_release(void *context, const dm_sa_span *s) {
    fixture *f = context;
    scope(f);
    lease *l = find_lease(f, s);
    CHECK(l);
    CHECK(provider_available(context, s, 0, s->size));
    f->release_calls++;
    if (f->release_calls == f->fail_release_at) return DM_SA_PROVIDER_FAILED;
    size_t p = ((uintptr_t)s->base - (uintptr_t)f->base) / PAGE;
    for (size_t j = 0; j < s->size / PAGE; j++) CHECK(f->pages[p + j]);
    memset(f->pages + p, 0, s->size / PAGE);
    f->used -= s->size;
    l->live = 0;
    return DM_SA_OK;
}
static dm_sa_result provider_query(void *context, size_t alignment, dm_sa_provider_stats *out) {
    fixture *f = context;
    scope(f);
    CHECK(alignment && !(alignment & (alignment - 1)));
    f->query_calls++;
    if (f->query_calls == f->fail_query_at) return DM_SA_PROVIDER_FAILED;
    if (f->query_calls == f->borrow_on_query_at) {
        CHECK(f->acquire_borrow_base);
        f->borrowed_base = f->acquire_borrow_base;
        f->borrowed_size = f->acquire_borrow_size;
        if (f->acquire_protect_base) CHECK(!mprotect(f->acquire_protect_base, f->acquire_protect_size, PROT_NONE));
    }
    size_t largest = 0;
    for (size_t p = 0; p < ARENA / PAGE;) {
        if (f->pages[p]) { p++; continue; }
        size_t end = p;
        while (end < ARENA / PAGE && !f->pages[end]) end++;
        uintptr_t start = align_up((uintptr_t)f->base + p * PAGE, alignment);
        uintptr_t limit = (uintptr_t)f->base + end * PAGE;
        if (start < limit && limit - start > largest) largest = limit - start;
        p = end;
    }
    size_t available = f->budget - f->used;
    *out = (dm_sa_provider_stats){available, largest < available ? largest : available};
    return DM_SA_OK;
}
static void *metadata_alloc(void *context, size_t size) {
    fixture *f = context;
    CHECK(!pthread_mutex_lock(&f->metadata_mutex));
    f->metadata_calls++;
    if (f->metadata_calls == f->metadata_fail_at) {
        CHECK(!pthread_mutex_unlock(&f->metadata_mutex));
        return NULL;
    }
    CHECK(size <= SIZE_MAX - sizeof(metadata_header));
    metadata_header *h = malloc(sizeof *h + size);
    CHECK(h);
    h->size = size;
    f->metadata_live++;
    f->metadata_bytes += size;
    CHECK(!pthread_mutex_unlock(&f->metadata_mutex));
    return h + 1;
}
static void metadata_free(void *context, void *pointer) {
    fixture *f = context;
    if (!pointer) return;
    metadata_header *h = (metadata_header *)pointer - 1;
    CHECK(!pthread_mutex_lock(&f->metadata_mutex));
    CHECK(f->metadata_live && f->metadata_bytes >= h->size);
    f->metadata_live--;
    f->metadata_bytes -= h->size;
    CHECK(!pthread_mutex_unlock(&f->metadata_mutex));
    free(h);
}
static void fixture_init(fixture *f, size_t budget) {
    memset(f, 0, sizeof *f);
    CHECK(budget <= ARENA && !(budget % PAGE));
    CHECK(!pthread_mutex_init(&f->mutex, NULL));
    CHECK(!pthread_mutex_init(&f->metadata_mutex, NULL));
    f->mapping_size = ARENA + MAX_ALIGN;
    f->mapping = mmap(NULL, f->mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(f->mapping != MAP_FAILED);
    f->base = (void *)align_up((uintptr_t)f->mapping, MAX_ALIGN);
    f->budget = budget;
    f->granule = PAGE;
}
static dm_sa_config config(fixture *f, size_t capacity, size_t pool_size) {
    dm_sa_config c = {0};
    c.provider = (dm_sa_provider){.context = f, .granule = f->granule,
        .begin = provider_begin, .end = provider_end, .acquire = provider_acquire,
        .release = provider_release, .available = provider_available, .query = provider_query};
    c.capacity = capacity;
    c.pool_size = pool_size;
    c.metadata_context = f;
    c.metadata_alloc = metadata_alloc;
    c.metadata_free = metadata_free;
    return c;
}
static dm_sa_allocator *create(fixture *f, size_t capacity, size_t pool_size) {
    dm_sa_allocator *a = NULL;
    dm_sa_config c = config(f, capacity, pool_size);
    OK(dm_sa_create(&a, &c));
    CHECK(a);
    return a;
}
static void fixture_finish(fixture *f) {
    CHECK(!f->used && !f->metadata_live && !f->metadata_bytes && !f->locked);
    for (size_t i = 0; i < MAX_LEASES; i++) CHECK(!f->leases[i].live);
    CHECK(!munmap(f->mapping, f->mapping_size));
    CHECK(!pthread_mutex_destroy(&f->mutex));
    CHECK(!pthread_mutex_destroy(&f->metadata_mutex));
}
static dm_sa_stats stats(dm_sa_allocator *a) {
    dm_sa_stats s;
    memset(&s, 0, sizeof s);
    OK(dm_sa_get_stats(a, &s));
    dm_sa_page_diagnostics pages, zero = {0};
    OK(dm_sa_get_page_diagnostics(a, &pages));
    CHECK(!memcmp(&pages, &zero, sizeof zero) && !memcmp(&s.pages, &zero, sizeof zero));
    CHECK(s.allocated_bytes <= s.backing_bytes && s.backing_bytes <= s.capacity);
    CHECK(s.largest_free <= s.total_free && s.total_free <= s.capacity - s.allocated_bytes);
    return s;
}
static size_t usable(dm_sa_allocator *a, void *p) {
    size_t size = SIZE_MAX;
    OK(dm_sa_get_size(a, p, &size));
    return size;
}
static void *allocate(dm_sa_allocator *a, size_t size, size_t alignment) {
    void *p = NULL;
    OK(dm_sa_allocate(a, size, alignment, &p));
    CHECK(p && !((uintptr_t)p % alignment));
    CHECK(usable(a, p) == (size ? (size + 15) & ~(size_t)15 : 16));
    return p;
}
static void check_pattern(const void *p, size_t size, unsigned char pattern) {
    const unsigned char *bytes = p;
    for (size_t i = 0; i < size; i++) CHECK(bytes[i] == pattern);
}
static void set_borrow(fixture *f, void *p, size_t size) {
    provider_begin(f);
    f->borrowed_base = (uintptr_t)p;
    f->borrowed_size = size;
    provider_end(f);
}
static void test_demand(void) {
    const size_t gib = (size_t)1024 * 1024 * 1024;
    const size_t capacities[] = {12 * gib, 40 * gib, 62 * gib};
    CHECK(sizeof(size_t) >= 8);
    for (size_t c = 0; c < sizeof capacities / sizeof capacities[0]; c++) {
        fixture f;
        fixture_init(&f, 128 * PAGE);
        dm_sa_allocator *a = create(&f, capacities[c], 0);
        CHECK(!f.acquire_calls && !f.release_calls && !f.used);
        CHECK(f.metadata_bytes <= PAGE); // logical capacity must not scale host metadata either
        dm_sa_stats s = stats(a);
        CHECK(s.capacity == capacities[c] && !s.backing_bytes && !s.allocation_count);
        CHECK(s.total_free <= f.budget && s.largest_free <= f.budget);
        void *p[1024];
        for (size_t i = 0; i < 1024; i++) {
            p[i] = allocate(a, 1, 16);
            memset(p[i], (int)(i & 255), 16);
        }
        s = stats(a);
        CHECK(s.allocated_bytes == 16 * 1024 && s.allocation_count == 1024);
        CHECK(f.acquire_calls < 16 && f.largest_request <= 64 * 1024 && f.peak <= 128 * 1024);
        CHECK(s.backing_bytes == f.used && s.total_free <= f.budget - s.allocated_bytes);
        for (size_t i = 0; i < 1024; i++) {
            check_pattern(p[i], 16, (unsigned char)i);
            OK(dm_sa_free(a, p[i]));
        }
        OK(dm_sa_destroy(a));
        fixture_finish(&f);
    }
}
static void test_validation(void) {
    fixture f;
    fixture_init(&f, ARENA);
    dm_sa_config c = config(&f, ARENA, 0);
    dm_sa_allocator *sentinel = SENTINEL;
    CHECK(dm_sa_create(NULL, &c) == DM_SA_INVALID);
    CHECK(dm_sa_create(&sentinel, NULL) == DM_SA_INVALID && (void *)sentinel == SENTINEL);
    dm_sa_config bad = c;
    bad.provider.granule = 3 * PAGE;
    CHECK(dm_sa_create(&sentinel, &bad) == DM_SA_INVALID);
    bad = c; bad.provider.available = NULL;
    CHECK(dm_sa_create(&sentinel, &bad) == DM_SA_INVALID);
    bad = c; bad.provider.pages.granule = PAGE;
    CHECK(dm_sa_create(&sentinel, &bad) == DM_SA_INVALID);
    bad = c; bad.metadata_free = NULL;
    CHECK(dm_sa_create(&sentinel, &bad) == DM_SA_INVALID);
    bad = c; bad.pool_size = SIZE_MAX;
    CHECK(dm_sa_create(&sentinel, &bad) == DM_SA_INVALID);
    CHECK((void *)sentinel == SENTINEL && !f.acquire_calls);
    dm_sa_allocator *a = create(&f, ARENA, PAGE);
    void *out = SENTINEL;
    const size_t bad_align[] = {0, 1, 8, 15, 24, 48};
    for (size_t i = 0; i < sizeof bad_align / sizeof bad_align[0]; i++) {
        CHECK(dm_sa_allocate(a, 5, bad_align[i], &out) == DM_SA_INVALID);
        CHECK(out == SENTINEL);
    }
    CHECK(dm_sa_allocate(a, SIZE_MAX, 16, &out) != DM_SA_OK && out == SENTINEL);
    CHECK(dm_sa_allocate(a, SIZE_MAX - 15, 16, &out) != DM_SA_OK && out == SENTINEL);
    CHECK(dm_sa_allocate(a, 16, 16, NULL) == DM_SA_INVALID);
    CHECK(dm_sa_allocate(a, 16, (size_t)1 << 63, &out) != DM_SA_OK && out == SENTINEL);
    CHECK(dm_sa_allocate(NULL, 16, 16, &out) == DM_SA_INVALID);
    void *p[10];
    const size_t sizes[] = {0, 1, 15, 16, 17, 31, 4095, 4096, 4097, 70001};
    for (size_t i = 0; i < 10; i++) {
        p[i] = allocate(a, sizes[i], (size_t)16 << i);
        memset(p[i], (int)i, usable(a, p[i]));
        for (size_t j = 0; j < i; j++)
            CHECK(!overlaps((uintptr_t)p[i], usable(a, p[i]), (uintptr_t)p[j], usable(a, p[j])));
    }
    void *zero1 = allocate(a, 0, 16), *zero2 = allocate(a, 0, 16);
    CHECK(zero1 != zero2);
    OK(dm_sa_free(a, zero1)); OK(dm_sa_free(a, zero2));
    void *large_align = allocate(a, 31, MAX_ALIGN);
    memset(large_align, 0x6b, 32);
    OK(dm_sa_free(a, large_align));
    int foreign = 0;
    size_t n = 12345;
    CHECK(dm_sa_get_size(a, &foreign, &n) == DM_SA_NOT_FOUND && n == 12345);
    CHECK(dm_sa_get_size(a, (char *)p[4] + 1, &n) == DM_SA_NOT_FOUND && n == 12345);
    CHECK(dm_sa_free(a, (char *)p[4] + 16) == DM_SA_NOT_FOUND);
    CHECK(dm_sa_free(a, &foreign) == DM_SA_NOT_FOUND);
    OK(dm_sa_free(a, NULL));
    for (size_t i = 0; i < 10; i++) {
        OK(dm_sa_free(a, p[i]));
        CHECK(dm_sa_get_size(a, p[i], &n) == DM_SA_NOT_FOUND && n == 12345);
        CHECK(dm_sa_free(a, p[i]) == DM_SA_NOT_FOUND);
    }
    OK(dm_sa_destroy(a));
    fixture_finish(&f);
}
static void test_budget(void) {
    fixture f;
    fixture_init(&f, 3 * PAGE);
    dm_sa_allocator *a = create(&f, (size_t)62 << 30, PAGE);
    CHECK(stats(a).total_free <= 3 * PAGE);
    void *p = allocate(a, 2 * PAGE, 16);
    CHECK(stats(a).total_free <= PAGE && stats(a).largest_free <= PAGE);
    void *out = SENTINEL;
    CHECK(dm_sa_allocate(a, PAGE + 1, 16, &out) != DM_SA_OK && out == SENTINEL);
    CHECK(stats(a).allocation_count == 1 && usable(a, p) == 2 * PAGE);
    void *q = allocate(a, PAGE, PAGE);
    CHECK(!stats(a).total_free && !stats(a).largest_free);
    CHECK(dm_sa_allocate(a, 1, 16, &out) != DM_SA_OK && out == SENTINEL);
    OK(dm_sa_free(a, q));
    OK(dm_sa_free(a, p));
    OK(dm_sa_destroy(a));
    fixture_finish(&f);

    fixture_init(&f, ARENA);
    a = create(&f, 2 * PAGE, PAGE);
    p = allocate(a, 2 * PAGE, 16);
    CHECK(!stats(a).total_free && !stats(a).largest_free);
    CHECK(dm_sa_allocate(a, 1, 16, &out) != DM_SA_OK && out == SENTINEL);
    OK(dm_sa_free(a, p));
    OK(dm_sa_destroy(a));
    fixture_finish(&f);
}
static void test_acquire_failure(void) {
    fixture f;
    fixture_init(&f, ARENA);
    dm_sa_allocator *a = create(&f, ARENA, PAGE);
    void *p = allocate(a, PAGE, 16);
    memset(p, 0xa7, PAGE);
    dm_sa_stats before = stats(a);
    size_t live = f.metadata_live;
    f.fail_acquire_at = f.acquire_calls + 1;
    void *out = SENTINEL;
    CHECK(dm_sa_allocate(a, 2 * PAGE, 16, &out) != DM_SA_OK && out == SENTINEL);
    dm_sa_stats after = stats(a);
    CHECK(before.allocated_bytes == after.allocated_bytes && before.backing_bytes == after.backing_bytes);
    CHECK(before.allocation_count == after.allocation_count && live == f.metadata_live);
    check_pattern(p, PAGE, 0xa7);
    f.fail_query_at = f.query_calls + 1;
    dm_sa_stats unchanged, actual;
    memset(&unchanged, 0x5a, sizeof unchanged);
    actual = unchanged;
    CHECK(dm_sa_get_stats(a, &actual) == DM_SA_PROVIDER_FAILED);
    CHECK(!memcmp(&actual, &unchanged, sizeof actual));
    OK(dm_sa_free(a, p));
    OK(dm_sa_destroy(a));
    fixture_finish(&f);
}
static void test_metadata_failure(void) {
    size_t failures = 0;
    for (size_t at = 1; at <= 12; at++) {
        fixture f;
        fixture_init(&f, ARENA);
        f.metadata_fail_at = at;
        dm_sa_config c = config(&f, ARENA, PAGE);
        dm_sa_allocator *a = SENTINEL;
        dm_sa_result r = dm_sa_create(&a, &c);
        if (r != DM_SA_OK) {
            CHECK(r == DM_SA_NO_MEMORY && (void *)a == SENTINEL);
            failures++;
        } else {
            void *p = SENTINEL;
            r = dm_sa_allocate(a, 47, 256, &p);
            if (r != DM_SA_OK) {
                CHECK(r == DM_SA_NO_MEMORY && p == SENTINEL);
                CHECK(!stats(a).allocation_count && !f.used);
                failures++;
            } else {
                memset(p, 0x4d, 47);
                void *q = SENTINEL;
                r = dm_sa_allocate(a, 71, 1024, &q);
                if (r != DM_SA_OK) {
                    CHECK(r == DM_SA_NO_MEMORY && q == SENTINEL);
                    CHECK(stats(a).allocation_count == 1);
                    failures++;
                } else OK(dm_sa_free(a, q));
                check_pattern(p, 47, 0x4d);
                OK(dm_sa_free(a, p));
            }
            f.metadata_fail_at = 0;
            OK(dm_sa_destroy(a));
        }
        fixture_finish(&f);
    }
    CHECK(failures >= 3);
}
static void test_release_failure(void) {
    fixture f;
    fixture_init(&f, ARENA);
    dm_sa_allocator *a = create(&f, ARENA, PAGE);
    void *p = allocate(a, PAGE, PAGE);
    f.fail_release_at = f.release_calls + 1;
    OK(dm_sa_free(a, p)); // logical lifetime ends; failed lease release is retained
    CHECK(f.used == PAGE && f.release_calls == 1);
    dm_sa_stats s = stats(a);
    CHECK(!s.allocation_count && !s.allocated_bytes && s.backing_bytes == PAGE && s.span_count == 1);
    size_t out = 789;
    CHECK(dm_sa_get_size(a, p, &out) == DM_SA_NOT_FOUND && out == 789);
    CHECK(dm_sa_free(a, p) == DM_SA_NOT_FOUND);
    f.fail_release_at = f.release_calls + 1;
    CHECK(dm_sa_trim(a) == DM_SA_PROVIDER_FAILED && f.used == PAGE);
    OK(dm_sa_trim(a));
    CHECK(!f.used && !stats(a).backing_bytes);
    OK(dm_sa_destroy(a));
    fixture_finish(&f);
}
static void test_provider_granules(void) {
    const size_t granules[] = {4 * PAGE, 16 * PAGE};
    for (size_t g = 0; g < 2; g++) {
        fixture f;
        fixture_init(&f, 64 * PAGE);
        f.granule = granules[g];
        dm_sa_allocator *a = create(&f, 128 * PAGE, PAGE + 1);
        void *p = allocate(a, 32, 16), *q = allocate(a, PAGE, PAGE);
        CHECK(f.acquire_calls == 1 && f.used == f.granule);
        memset(p, 0x83, 32); memset(q, 0x97, PAGE);
        set_borrow(&f, (unsigned char *)p + 3, 1);
        CHECK(dm_sa_free(a, p) == DM_SA_BUSY);
        OK(dm_sa_free(a, q)); // different sub-range inside the same provider granule
        CHECK(dm_sa_finalize(a) == DM_SA_BUSY && !f.release_calls);
        check_pattern(p, 32, 0x83);
        set_borrow(&f, NULL, 0);
        OK(dm_sa_free(a, p));
        CHECK(!f.used && f.release_calls == 1);
        OK(dm_sa_destroy(a)); fixture_finish(&f);
    }
}
static void test_reallocate_release_failure(void) {
    fixture f;
    fixture_init(&f, ARENA);
    dm_sa_allocator *a = create(&f, ARENA, PAGE);
    void *p = allocate(a, PAGE, PAGE), *out = SENTINEL;
    memset(p, 0x54, PAGE);
    f.fail_release_at = f.release_calls + 1;
    OK(dm_sa_reallocate(a, p, 2 * PAGE, &out));
    CHECK(out != p && !((uintptr_t)out % PAGE));
    check_pattern(out, PAGE, 0x54);
    dm_sa_stats s = stats(a);
    CHECK(s.allocation_count == 1 && s.allocated_bytes == 2 * PAGE && s.backing_bytes == 3 * PAGE);
    size_t size = 777;
    CHECK(dm_sa_get_size(a, p, &size) == DM_SA_NOT_FOUND && size == 777);
    CHECK(dm_sa_free(a, p) == DM_SA_NOT_FOUND);
    OK(dm_sa_trim(a));
    CHECK(stats(a).backing_bytes == 2 * PAGE);
    OK(dm_sa_free(a, out)); OK(dm_sa_destroy(a)); fixture_finish(&f);
}
static void test_reallocate_shrink(void) {
    fixture f;
    fixture_init(&f, ARENA);
    dm_sa_allocator *a = create(&f, ARENA, PAGE);
    void *p = SENTINEL;
    OK(dm_sa_reallocate(a, NULL, 0, &p));
    CHECK(p && usable(a, p) == 16);
    void *q = NULL;
    OK(dm_sa_reallocate(a, NULL, 41, &q));
    CHECK(q && q != p && usable(a, q) == 48);
    memset(q, 0xd3, 48);
    void *result = SENTINEL;
    OK(dm_sa_reallocate(a, q, 320, &result));
    CHECK(usable(a, result) == 320);
    check_pattern(result, 48, 0xd3);
    q = result;
    memset(q, 0x71, 320);
    size_t before = stats(a).allocated_bytes, size_out = 777;
    OK(dm_sa_shrink(a, q, 79, &size_out));
    CHECK(size_out == 80 && usable(a, q) == 80);
    CHECK(stats(a).allocated_bytes == before - 240);
    check_pattern(q, 80, 0x71);
    void *slack = allocate(a, 240, 16);
    CHECK(slack == (unsigned char *)q + 80); // released slack is genuinely reusable
    memset(slack, 0x29, 240);
    size_out = 777;
    CHECK(dm_sa_shrink(a, q, 81, &size_out) == DM_SA_INVALID && size_out == 777);
    CHECK(dm_sa_shrink(a, q, 0, &size_out) == DM_SA_INVALID && size_out == 777);
    result = SENTINEL;
    CHECK(dm_sa_reallocate(a, q, SIZE_MAX, &result) != DM_SA_OK && result == SENTINEL);
    CHECK(usable(a, q) == 80);
    check_pattern(q, 80, 0x71);
    OK(dm_sa_reallocate(a, q, 0, &result));
    CHECK(!result);
    CHECK(dm_sa_get_size(a, q, &size_out) == DM_SA_NOT_FOUND);
    OK(dm_sa_free(a, p));
    OK(dm_sa_free(a, slack));
    OK(dm_sa_destroy(a));
    fixture_finish(&f);

    // A forced move fails at each possible metadata step without invalidating
    // the old span. A separate acquire failure covers the provider boundary.
    for (size_t at = 1; at <= 5; at++) {
        fixture_init(&f, ARENA);
        a = create(&f, ARENA, PAGE);
        p = allocate(a, PAGE, PAGE);
        memset(p, 0xb6, PAGE);
        before = stats(a).allocated_bytes;
        if (at <= 4) f.metadata_fail_at = f.metadata_calls + at;
        else f.fail_acquire_at = f.acquire_calls + 1;
        result = SENTINEL;
        dm_sa_result r = dm_sa_reallocate(a, p, 3 * PAGE + 1, &result);
        if (r != DM_SA_OK) {
            CHECK(result == SENTINEL && usable(a, p) == PAGE);
            CHECK(stats(a).allocation_count == 1 && stats(a).allocated_bytes == before);
            check_pattern(p, PAGE, 0xb6);
            OK(dm_sa_free(a, p));
        } else {
            CHECK(!((uintptr_t)result % PAGE));
            check_pattern(result, PAGE, 0xb6);
            OK(dm_sa_free(a, result));
        }
        f.metadata_fail_at = 0;
        OK(dm_sa_destroy(a));
        fixture_finish(&f);
    }
    fixture_init(&f, ARENA);
    a = create(&f, ARENA, PAGE);
    p = allocate(a, 64, 16);
    q = allocate(a, 64, 16);
    memset(p, 0xc1, 64);
    f.metadata_fail_at = f.metadata_calls + 1;
    size_out = 777;
    CHECK(dm_sa_shrink(a, p, 16, &size_out) == DM_SA_NO_MEMORY && size_out == 777);
    CHECK(usable(a, p) == 64);
    check_pattern(p, 64, 0xc1);
    OK(dm_sa_free(a, p));
    OK(dm_sa_free(a, q));
    OK(dm_sa_destroy(a));
    fixture_finish(&f);
}
static void test_borrow(void) {
    fixture f;
    fixture_init(&f, 4 * PAGE);
    dm_sa_allocator *a = create(&f, ARENA, PAGE);
    void *p = allocate(a, 64, 16);
    void *q = allocate(a, 64, 16);
    void *r = allocate(a, 64, 16);
    memset(p, 0x39, 64);
    set_borrow(&f, (unsigned char *)p + 32, 16);
    dm_sa_stats before = stats(a);
    size_t n = 777;
    void *out = SENTINEL;
    CHECK(dm_sa_free(a, p) == DM_SA_BUSY);
    CHECK(dm_sa_shrink(a, p, 16, &n) == DM_SA_BUSY && n == 777);
    CHECK(dm_sa_reallocate(a, p, 128, &out) == DM_SA_BUSY && out == SENTINEL);
    CHECK(dm_sa_reallocate(a, p, 0, &out) == DM_SA_BUSY && out == SENTINEL);
    CHECK(dm_sa_finalize(a) == DM_SA_BUSY);
    CHECK(dm_sa_destroy(a) == DM_SA_BUSY);
    CHECK(!stats(a).closed && stats(a).allocated_bytes == before.allocated_bytes);
    CHECK(usable(a, p) == 64);
    check_pattern(p, 64, 0x39);
    set_borrow(&f, NULL, 0);
    OK(dm_sa_free(a, q));
    set_borrow(&f, q, 64); // free but now externally borrowed: sub-page reuse is forbidden
    size_t guards = f.available_calls;
    void *s = allocate(a, 48, 16);
    CHECK(!overlaps((uintptr_t)s, 48, (uintptr_t)q, 64) && f.available_calls > guards);
    OK(dm_sa_free(a, p)); // unrelated same-page allocation may be freed
    OK(dm_sa_free(a, r));
    OK(dm_sa_free(a, s));
    CHECK(f.used == PAGE && !stats(a).allocation_count);
    CHECK(dm_sa_trim(a) == DM_SA_BUSY);
    CHECK(dm_sa_finalize(a) == DM_SA_BUSY && !stats(a).closed);
    out = allocate(a, 64, 16);
    CHECK(!overlaps((uintptr_t)out, 64, (uintptr_t)q, 64));
    OK(dm_sa_free(a, out));
    set_borrow(&f, NULL, 0);
    OK(dm_sa_trim(a));
    CHECK(!f.used);
    OK(dm_sa_destroy(a));
    fixture_finish(&f);
}
static void check_walk_hash(dm_sa_allocator *a, size_t count, size_t bytes);
static void test_borrow_no_touch(void) {
    fixture f;
    fixture_init(&f, ARENA);
    size_t native_page = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(native_page >= PAGE && !(native_page % PAGE) && native_page <= MAX_ALIGN);
    f.granule = native_page;
    dm_sa_allocator *a = create(&f, ARENA, native_page);
    void *p = allocate(a, native_page, native_page);
    memset(p, 0x65, native_page);
    set_borrow(&f, p, native_page);
    CHECK(!mprotect(p, native_page, PROT_NONE));
    // Borrow checks must happen before any read/write, including realloc copy.
    // Metadata-only queries remain safe even while borrowed backing is unreadable.
    void *out = SENTINEL;
    size_t size = 123;
    CHECK(dm_sa_free(a, p) == DM_SA_BUSY);
    CHECK(dm_sa_reallocate(a, p, 2 * native_page, &out) == DM_SA_BUSY && out == SENTINEL);
    CHECK(dm_sa_reallocate(a, p, 0, &out) == DM_SA_BUSY && out == SENTINEL);
    CHECK(dm_sa_shrink(a, p, 16, &size) == DM_SA_BUSY && size == 123);
    CHECK(dm_sa_finalize(a) == DM_SA_BUSY);
    CHECK(usable(a, p) == native_page);
    check_walk_hash(a, 1, native_page);
    CHECK(!mprotect(p, native_page, PROT_READ | PROT_WRITE));
    check_pattern(p, native_page, 0x65);
    set_borrow(&f, NULL, 0);
    OK(dm_sa_free(a, p)); OK(dm_sa_destroy(a)); fixture_finish(&f);
}
static void test_acquire_borrow_race(void) {
    fixture f;
    fixture_init(&f, 2 * PAGE);
    dm_sa_allocator *a = create(&f, ARENA, PAGE);
    f.borrow_on_acquire_at = 1; f.acquire_borrow_size = 16;
    void *out = SENTINEL;
    CHECK(dm_sa_allocate(a, 128, 16, &out) == DM_SA_BUSY && out == SENTINEL);
    dm_sa_stats s = stats(a);
    CHECK(!s.allocation_count && !s.allocated_bytes && s.backing_bytes == PAGE && s.span_count == 1);
    CHECK(f.used == PAGE && !f.release_calls);
    CHECK(dm_sa_trim(a) == DM_SA_BUSY && dm_sa_finalize(a) == DM_SA_BUSY);
    set_borrow(&f, NULL, 0);
    OK(dm_sa_trim(a));
    CHECK(!f.used);
    out = allocate(a, 128, 16);
    OK(dm_sa_free(a, out)); OK(dm_sa_destroy(a)); fixture_finish(&f);
}
static void test_reallocate_acquire_borrow_race(void) {
    // Also fail replacement release: rollback may keep owned backing but must
    // never manufacture an extra live allocation or discard the old source.
    for (int query_race = 0; query_race < 2; query_race++)
    for (int fail_release = 0; fail_release < 2; fail_release++) {
        fixture f;
        fixture_init(&f, ARENA);
        size_t native_page = (size_t)sysconf(_SC_PAGESIZE);
        CHECK(native_page >= PAGE && !(native_page % PAGE) && native_page <= MAX_ALIGN);
        f.granule = native_page;
        dm_sa_allocator *a = create(&f, ARENA, native_page);
        void *p = allocate(a, native_page, native_page), *out = SENTINEL;
        memset(p, 0x8a, native_page);
        if (query_race) f.borrow_on_query_at = f.query_calls + 1;
        else f.borrow_on_acquire_at = f.acquire_calls + 1;
        f.acquire_borrow_base = (uintptr_t)p + 17; f.acquire_borrow_size = 1;
        f.acquire_protect_base = p; f.acquire_protect_size = native_page;
        if (fail_release) f.fail_release_at = f.release_calls + 1;
        // Querying/acquiring the replacement temporarily suspends the guard. The
        // old source becomes borrowed AND unreadable after initial validation.
        CHECK(dm_sa_reallocate(a, p, 2 * native_page, &out) == DM_SA_BUSY && out == SENTINEL);
        CHECK(usable(a, p) == native_page);
        CHECK(!mprotect(p, native_page, PROT_READ | PROT_WRITE));
        check_pattern(p, native_page, 0x8a);
        dm_sa_stats s = stats(a);
        CHECK(s.allocation_count == 1 && s.allocated_bytes == native_page);
        CHECK(s.backing_bytes == (fail_release ? 3 : 1) * native_page);
        CHECK(dm_sa_free(a, p) == DM_SA_BUSY);
        set_borrow(&f, NULL, 0);
        OK(dm_sa_free(a, p)); OK(dm_sa_destroy(a)); fixture_finish(&f);
    }
}
static void test_default_metadata(void) {
    fixture f;
    fixture_init(&f, ARENA);
    dm_sa_config c = config(&f, ARENA, PAGE);
    c.metadata_alloc = NULL; c.metadata_free = NULL;
    dm_sa_allocator *a = NULL;
    OK(dm_sa_create(&a, &c));
    void *p = allocate(a, 73, 16);
    CHECK(stats(a).metadata_bytes && !f.metadata_live);
    OK(dm_sa_free(a, p)); OK(dm_sa_destroy(a)); fixture_finish(&f);
}
static void test_finalize(void) {
    fixture f;
    fixture_init(&f, ARENA);
    dm_sa_allocator *a = create(&f, ARENA, PAGE);
    void *p[3];
    for (size_t i = 0; i < 3; i++) p[i] = allocate(a, 2 * PAGE, PAGE);
    set_borrow(&f, (unsigned char *)p[1] + 17, 1);
    CHECK(dm_sa_finalize(a) == DM_SA_BUSY && !f.release_calls);
    CHECK(stats(a).allocation_count == 3 && !stats(a).closed);
    set_borrow(&f, NULL, 0);
    f.fail_release_at = f.release_calls + 1;
    CHECK(dm_sa_finalize(a) == DM_SA_PROVIDER_FAILED);
    CHECK(f.release_calls == 3 && f.used == 2 * PAGE);
    dm_sa_stats s = stats(a);
    CHECK(s.closed && !s.allocated_bytes && !s.allocation_count && !s.total_free && !s.largest_free);
    CHECK(s.backing_bytes == 2 * PAGE && s.span_count == 1);
    void *out = SENTINEL;
    size_t n = 123;
    CHECK(dm_sa_allocate(a, 1, 16, &out) == DM_SA_CLOSED && out == SENTINEL);
    CHECK(dm_sa_reallocate(a, p[0], 16, &out) == DM_SA_CLOSED && out == SENTINEL);
    CHECK(dm_sa_get_size(a, p[0], &n) == DM_SA_CLOSED && n == 123);
    CHECK(dm_sa_shrink(a, p[0], 16, &n) == DM_SA_CLOSED && n == 123);
    CHECK(dm_sa_free(a, p[0]) == DM_SA_CLOSED);
    CHECK(dm_sa_trim(a) == DM_SA_CLOSED);
    OK(dm_sa_finalize(a));
    CHECK(!f.used && !stats(a).span_count);
    OK(dm_sa_finalize(a));
    OK(dm_sa_destroy(a));

    a = create(&f, ARENA, PAGE);
    (void)allocate(a, PAGE, PAGE);
    f.fail_release_at = f.release_calls + 1;
    CHECK(dm_sa_destroy(a) == DM_SA_PROVIDER_FAILED);
    CHECK(f.used == PAGE && stats(a).closed && f.metadata_live);
    OK(dm_sa_destroy(a));
    fixture_finish(&f);
}

// Independent one-shot SHA-1 reference for this core's documented byte stream.
// The core uses a streaming implementation; known NIST vectors also validate
// this reference. SHA-1 here is only an allocation-layout diagnostic.
static uint32_t rotate(uint32_t x, unsigned n) { return x << n | x >> (32 - n); }
static void reference_sha1(const unsigned char *input, size_t size, unsigned char digest[20]) {
    size_t padded = (size + 9 + 63) & ~(size_t)63;
    unsigned char *bytes = calloc(1, padded);
    CHECK(bytes);
    if (size) memcpy(bytes, input, size);
    bytes[size] = 0x80;
    for (unsigned j = 0; j < 8; j++) bytes[padded - 1 - j] = (unsigned char)(((uint64_t)size * 8) >> (8 * j));
    uint32_t h[] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    for (size_t offset = 0; offset < padded; offset += 64) {
        uint32_t words[80];
        for (unsigned j = 0; j < 16; j++) {
            words[j] = 0;
            for (unsigned k = 0; k < 4; k++) words[j] = (words[j] << 8) | bytes[offset + 4 * j + k];
        }
        for (unsigned j = 16; j < 80; j++) words[j] = rotate(words[j-3] ^ words[j-8] ^ words[j-14] ^ words[j-16], 1);
        uint32_t v[5];
        memcpy(v, h, sizeof v);
        for (unsigned j = 0; j < 80; j++) {
            uint32_t boolean, constant;
            switch (j / 20) {
            case 0: boolean = v[3] ^ (v[1] & (v[2] ^ v[3])); constant = 0x5a827999u; break;
            case 1: boolean = v[1] ^ v[2] ^ v[3]; constant = 0x6ed9eba1u; break;
            case 2: boolean = (v[1] & v[2]) | (v[3] & (v[1] | v[2])); constant = 0x8f1bbcdcu; break;
            default: boolean = v[1] ^ v[2] ^ v[3]; constant = 0xca62c1d6u; break;
            }
            uint32_t next = rotate(v[0], 5) + boolean + v[4] + constant + words[j];
            v[4] = v[3]; v[3] = v[2]; v[2] = rotate(v[1], 30); v[1] = v[0]; v[0] = next;
        }
        for (unsigned j = 0; j < 5; j++) h[j] += v[j];
    }
    for (unsigned j = 0; j < 20; j++) digest[j] = (unsigned char)(h[j / 4] >> (24 - 8 * (j % 4)));
    free(bytes);
}
typedef struct {
    uintptr_t previous;
    size_t count, bytes, stop_at;
    unsigned char encoded[128 * 16];
    dm_sa_allocator *other;
} walk_state;
static int walk_record(void *pointer, size_t size, void *context) {
    walk_state *w = context;
    CHECK((uintptr_t)pointer > w->previous && size >= 16 && !(size % 16));
    CHECK(w->count < 128);
    w->previous = (uintptr_t)pointer;
    for (unsigned j = 0; j < 8; j++) {
        w->encoded[w->count * 16 + j] = (unsigned char)((uint64_t)(uintptr_t)pointer >> (8 * j));
        w->encoded[w->count * 16 + 8 + j] = (unsigned char)((uint64_t)size >> (8 * j));
    }
    w->count++;
    w->bytes += size;
    if (w->other) { // same-instance reentry is forbidden by this low-level API
        void *p = allocate(w->other, 19, 16);
        OK(dm_sa_free(w->other, p));
    }
    return w->stop_at && w->count == w->stop_at ? -17 : 0;
}
static void check_walk_hash(dm_sa_allocator *a, size_t count, size_t bytes) {
    walk_state w = {0};
    int stop = 777;
    OK(dm_sa_walk(a, walk_record, &w, &stop));
    CHECK(!stop && w.count == count && w.bytes == bytes);
    dm_sa_hash h;
    OK(dm_sa_get_hash(a, &h));
    CHECK(h.block_count == count && h.allocated_bytes == bytes);
    unsigned char reference[20];
    reference_sha1(w.encoded, w.count * 16, reference);
    CHECK(!memcmp(h.digest, reference, 8));
}
static void test_walk_hash(void) {
    unsigned char digest[20];
    static const unsigned char abc[20] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    reference_sha1((const unsigned char *)"abc", 3, digest);
    CHECK(!memcmp(digest, abc, 20));
    fixture f, other;
    fixture_init(&f, ARENA);
    fixture_init(&other, ARENA);
    dm_sa_allocator *a = create(&f, ARENA, PAGE), *b = create(&other, ARENA, PAGE);
    check_walk_hash(a, 0, 0);
    void *p[9];
    size_t total = 0;
    for (size_t i = 0; i < 9; i++) {
        p[i] = allocate(a, i * 121 + 1, (size_t)16 << (i % 6));
        total += usable(a, p[i]);
    }
    OK(dm_sa_free(a, p[3]));
    total -= (3 * 121 + 16) & ~(size_t)15;
    check_walk_hash(a, 8, total);
    dm_sa_hash first, second;
    OK(dm_sa_get_hash(a, &first));
    for (size_t i = 0; i < 9; i++) if (i != 3) memset(p[i], 0xc7, usable(a, p[i]));
    OK(dm_sa_get_hash(a, &second));
    CHECK(!memcmp(&first, &second, sizeof first)); // contents are NOT hashed
    walk_state w = {.stop_at = 3, .other = b};
    int stop = 0;
    OK(dm_sa_walk(a, walk_record, &w, &stop));
    CHECK(w.count == 3 && stop == -17 && !stats(b).allocation_count);
    for (size_t i = 0; i < 9; i++) if (i != 3) OK(dm_sa_free(a, p[i]));
    check_walk_hash(a, 0, 0);
    OK(dm_sa_destroy(a));
    OK(dm_sa_destroy(b));
    fixture_finish(&f);
    fixture_finish(&other);
}
typedef struct { void *p; size_t size, alignment; unsigned char pattern; } model_slot;
static uint64_t random_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return *state = x;
}
static void check_model(fixture *f, dm_sa_allocator *a, model_slot slots[64], int with_hash) {
    size_t count = 0, bytes = 0;
    for (size_t i = 0; i < 64; i++) {
        if (!slots[i].p) continue;
        count++; bytes += slots[i].size;
        CHECK(usable(a, slots[i].p) == slots[i].size);
        CHECK(!((uintptr_t)slots[i].p % slots[i].alignment));
        check_pattern(slots[i].p, slots[i].size, slots[i].pattern);
        for (size_t j = 0; j < i; j++) if (slots[j].p)
            CHECK(!overlaps((uintptr_t)slots[i].p, slots[i].size, (uintptr_t)slots[j].p, slots[j].size));
    }
    dm_sa_stats s = stats(a);
    CHECK(s.allocation_count == count && s.allocated_bytes == bytes && s.backing_bytes == f->used);
    CHECK(s.total_free <= f->budget - bytes && s.metadata_bytes == f->metadata_bytes);
    if (with_hash) check_walk_hash(a, count, bytes);
}
static void test_randomized(void) {
    const uint64_t seeds[] = {1, UINT64_C(0x8af53109c6d720be), UINT64_C(0xfedcba9876543210)};
    for (size_t seed = 0; seed < 3; seed++) {
        fixture f;
        fixture_init(&f, 256 * PAGE);
        dm_sa_allocator *a = create(&f, ARENA, 4 * PAGE);
        model_slot slots[64] = {{0}};
        uint64_t rng = seeds[seed];
        for (size_t step = 0; step < 6000; step++) {
            uint64_t value = random_next(&rng);
            model_slot *s = &slots[value % 64];
            unsigned op = (unsigned)(value >> 8) % 5;
            if (s->p) check_pattern(s->p, s->size, s->pattern);
            if (step % 7 == 0) f.metadata_fail_at = f.metadata_calls + 1 + (value >> 16) % 3;
            if (step % 17 == 0) f.fail_acquire_at = f.acquire_calls + 1;
            if (!s->p) {
                size_t request = (value >> 22) % 4097;
                size_t alignment = (size_t)16 << ((value >> 40) % 9);
                void *out = SENTINEL;
                dm_sa_result r = dm_sa_allocate(a, request, alignment, &out);
                if (r == DM_SA_OK) {
                    *s = (model_slot){out, request ? (request + 15) & ~(size_t)15 : 16, alignment, (unsigned char)value};
                    memset(s->p, s->pattern, s->size);
                } else CHECK((r == DM_SA_NO_MEMORY || r == DM_SA_PROVIDER_FAILED) && out == SENTINEL);
            } else if (op == 0) {
                void *old = s->p;
                OK(dm_sa_free(a, old));
                size_t n = 321;
                CHECK(dm_sa_get_size(a, old, &n) == DM_SA_NOT_FOUND && n == 321);
                *s = (model_slot){0};
            } else if (op == 1 || op == 2) {
                size_t request = (value >> 22) % 8193;
                void *out = SENTINEL, *old = s->p;
                size_t old_size = s->size;
                dm_sa_result r = dm_sa_reallocate(a, old, request, &out);
                if (r == DM_SA_OK) {
                    if (!request) { CHECK(!out); *s = (model_slot){0}; }
                    else {
                        size_t size = (request + 15) & ~(size_t)15;
                        check_pattern(out, old_size < size ? old_size : size, s->pattern);
                        s->p = out; s->size = size;
                        memset(out, s->pattern, size);
                    }
                    if (out != old) {
                        size_t n = 321;
                        CHECK(dm_sa_get_size(a, old, &n) == DM_SA_NOT_FOUND && n == 321);
                    }
                } else {
                    CHECK((r == DM_SA_NO_MEMORY || r == DM_SA_PROVIDER_FAILED) && out == SENTINEL);
                    CHECK(usable(a, old) == old_size);
                }
            } else if (op == 3) {
                size_t request = 1 + (value >> 22) % s->size, out = 321;
                dm_sa_result r = dm_sa_shrink(a, s->p, request, &out);
                if (r == DM_SA_OK) {
                    CHECK(out == ((request + 15) & ~(size_t)15));
                    s->size = out;
                } else CHECK(r == DM_SA_NO_MEMORY && out == 321);
            } else {
                size_t out = 321;
                CHECK(dm_sa_get_size(a, (unsigned char *)s->p + 1, &out) == DM_SA_NOT_FOUND && out == 321);
                CHECK(dm_sa_free(a, (unsigned char *)s->p + 1) == DM_SA_NOT_FOUND);
            }
            if (s->p) {
                check_pattern(s->p, s->size, s->pattern);
                for (size_t j = 0; j < 64; j++) if (&slots[j] != s && slots[j].p)
                    CHECK(!overlaps((uintptr_t)s->p, s->size, (uintptr_t)slots[j].p, slots[j].size));
            }
            f.metadata_fail_at = f.fail_acquire_at = 0;
            if (step % 23 == 0) check_model(&f, a, slots, step % 97 == 0);
        }
        check_model(&f, a, slots, 1);
        for (size_t i = 0; i < 64; i++) if (slots[i].p) OK(dm_sa_free(a, slots[i].p));
        OK(dm_sa_destroy(a));
        fixture_finish(&f);
    }
}
#define WORKERS 8
#define BATCH 4
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned waiting, generation;
    dm_sa_allocator *allocators[2];
    model_slot stable[WORKERS][BATCH];
} thread_shared;
typedef struct { thread_shared *shared; unsigned index; } thread_job;
static void barrier(thread_shared *s) {
    CHECK(!pthread_mutex_lock(&s->mutex));
    unsigned generation = s->generation;
    if (++s->waiting == WORKERS) {
        s->waiting = 0; s->generation++;
        CHECK(!pthread_cond_broadcast(&s->condition));
    } else while (generation == s->generation) CHECK(!pthread_cond_wait(&s->condition, &s->mutex));
    CHECK(!pthread_mutex_unlock(&s->mutex));
}
static void *thread_worker(void *context) {
    thread_job *j = context;
    thread_shared *shared = j->shared;
    dm_sa_allocator *a = shared->allocators[j->index % 2];
    model_slot local[BATCH] = {{0}};
    uint64_t rng = 73 + j->index;
    for (size_t step = 0; step < 2000; step++) {
        for (size_t k = 0; k < BATCH; k++) {
            model_slot *s = &local[k];
            if (s->p) { check_pattern(s->p, s->size, s->pattern); OK(dm_sa_free(a, s->p)); }
            size_t size = 16 + random_next(&rng) % 2048;
            s->p = allocate(a, size, (size_t)16 << (random_next(&rng) % 6));
            s->size = usable(a, s->p);
            s->pattern = (unsigned char)(17 + 13 * j->index + k);
            memset(s->p, s->pattern, s->size);
            if (step % 3 == 0) {
                void *out = NULL;
                OK(dm_sa_reallocate(a, s->p, 4097, &out));
                check_pattern(out, s->size, s->pattern);
                s->p = out; s->size = usable(a, out);
                memset(out, s->pattern, s->size);
            }
        }
    }
    memcpy(shared->stable[j->index], local, sizeof local);
    barrier(shared);
    for (size_t t = 0; t < WORKERS; t++) for (size_t k = 0; k < BATCH; k++) {
        model_slot *s = &shared->stable[t][k];
        check_pattern(s->p, s->size, s->pattern);
        for (size_t other = 0; other < WORKERS; other++) for (size_t n = 0; n < BATCH; n++) {
            if (other == t && n == k) continue;
            model_slot *v = &shared->stable[other][n];
            CHECK(!overlaps((uintptr_t)s->p, s->size, (uintptr_t)v->p, v->size));
        }
    }
    barrier(shared);
    for (size_t k = 0; k < BATCH; k++) OK(dm_sa_free(a, local[k].p));
    return NULL;
}
static void test_threads_instances(void) {
    fixture f;
    fixture_init(&f, ARENA);
    dm_sa_allocator *a = create(&f, ARENA, 16 * PAGE), *b = create(&f, ARENA, 16 * PAGE);
    void *p = allocate(a, 127, 64), *q = allocate(b, 127, 64);
    size_t n = 777;
    CHECK(dm_sa_get_size(b, p, &n) == DM_SA_NOT_FOUND && n == 777);
    CHECK(dm_sa_get_size(a, q, &n) == DM_SA_NOT_FOUND && n == 777);
    CHECK(dm_sa_free(b, p) == DM_SA_NOT_FOUND && dm_sa_free(a, q) == DM_SA_NOT_FOUND);
    void *out = SENTINEL;
    CHECK(dm_sa_reallocate(b, p, 15, &out) == DM_SA_NOT_FOUND && out == SENTINEL);
    CHECK(dm_sa_shrink(b, p, 15, &n) == DM_SA_NOT_FOUND && n == 777);
    OK(dm_sa_free(a, p)); OK(dm_sa_free(b, q));
    thread_shared shared = {.allocators = {a, b}};
    CHECK(!pthread_mutex_init(&shared.mutex, NULL));
    CHECK(!pthread_cond_init(&shared.condition, NULL));
    pthread_t threads[WORKERS];
    thread_job jobs[WORKERS];
    for (unsigned i = 0; i < WORKERS; i++) {
        jobs[i] = (thread_job){&shared, i};
        CHECK(!pthread_create(&threads[i], NULL, thread_worker, &jobs[i]));
    }
    for (unsigned i = 0; i < WORKERS; i++) CHECK(!pthread_join(threads[i], NULL));
    CHECK(!stats(a).allocation_count && !stats(b).allocation_count);
    CHECK(!pthread_cond_destroy(&shared.condition));
    CHECK(!pthread_mutex_destroy(&shared.mutex));
    OK(dm_sa_destroy(a)); OK(dm_sa_destroy(b));
    fixture_finish(&f);
}
int main(int argc, char **argv) {
    const struct { const char *name; void (*run)(void); } tests[] = {
        {"demand", test_demand}, {"validation", test_validation},
        {"budget", test_budget}, {"acquire_failure", test_acquire_failure},
        {"metadata_failure", test_metadata_failure}, {"release_failure", test_release_failure},
        {"reallocate_shrink", test_reallocate_shrink}, {"borrow", test_borrow},
        {"provider_granules", test_provider_granules}, {"reallocate_release_failure", test_reallocate_release_failure},
        {"borrow_no_touch", test_borrow_no_touch}, {"default_metadata", test_default_metadata},
        {"acquire_borrow_race", test_acquire_borrow_race}, {"reallocate_acquire_borrow_race", test_reallocate_acquire_borrow_race},
        {"finalize", test_finalize}, {"walk_hash", test_walk_hash},
        {"randomized", test_randomized}, {"threads_instances", test_threads_instances}
    };
    size_t ran = 0;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        if (argc > 1 && strcmp(argv[1], tests[i].name)) continue;
        tests[i].run(); ran++;
        printf("standard allocator core: %s OK\n", tests[i].name);
        fflush(stdout);
    }
    CHECK(ran);
    puts("standard allocator core: all selected tests passed");
    return 0;
}
