// Deterministic observable transcript: compare disabled capability against
// the frozen provider-only core without changing either source tree.
#define main original_fixture_main
#include TEST_FIXTURE
#undef main
static uint32_t seed = 0xa25cf01b;
static uint32_t random_u32(void) {
    uint32_t x = seed; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return seed = x;
}
int main(void) {
    fixture f; fixture_init(&f, 64 * PAGE);
    dm_sa_allocator *a = create(&f, 64 * PAGE, 8 * PAGE);
    struct { void *p; size_t n; unsigned char value; } live[24] = {0};
    for (size_t iter = 0; iter < 900; ++iter) {
        size_t at = random_u32() % 24, request = 1 + random_u32() % (8 * PAGE);
        unsigned op = live[at].p ? 1 + random_u32() % 3 : 0;
        int borrow = live[at].p && iter % 19 == 0;
        if (borrow) set_borrow(&f, live[at].p, 16);
        if (iter % 17 == 0) f.metadata_fail_at = f.metadata_calls + 1;
        if (iter % 23 == 0) f.fail_release_at = f.release_calls + 1;
        if (iter % 29 == 0) f.fail_query_at = f.query_calls + 1;
        void *out = SENTINEL;
        dm_sa_result result;
        if (op == 0) {
            size_t align = (size_t)16 << (random_u32() % 9);
            result = dm_sa_allocate(a, request, align, &out);
            if (result == DM_SA_OK) {
                live[at].p = out; live[at].n = (request + 15) & ~(size_t)15;
                live[at].value = (unsigned char)(1 + at); memset(out, live[at].value, live[at].n);
            } else CHECK(out == SENTINEL);
        } else if (op == 1) {
            result = dm_sa_free(a, live[at].p);
            if (result == DM_SA_OK) memset(&live[at], 0, sizeof live[at]);
        } else if (op == 2) {
            result = dm_sa_reallocate(a, live[at].p, request, &out);
            if (result == DM_SA_OK) {
                size_t next = (request + 15) & ~(size_t)15;
                check_pattern(out, next < live[at].n ? next : live[at].n, live[at].value);
                live[at].p = out; live[at].n = next; memset(out, live[at].value, next);
            } else CHECK(out == SENTINEL);
        } else {
            request = (1 + random_u32() % (live[at].n / 16)) * 16;
            size_t actual = SIZE_MAX;
            result = dm_sa_shrink(a, live[at].p, request, &actual);
            if (result == DM_SA_OK) { CHECK(actual == request); live[at].n = request; }
            else CHECK(actual == SIZE_MAX);
        }
        if (borrow) set_borrow(&f, NULL, 0);
        f.metadata_fail_at = f.fail_release_at = f.fail_query_at = 0;
        if (iter % 31 == 0) OK(dm_sa_trim(a));
        dm_sa_stats s = stats(a);
        size_t count = 0, used = 0;
        for (size_t j = 0; j < 24; ++j) if (live[j].p) {
            CHECK(usable(a, live[j].p) == live[j].n);
            check_pattern(live[j].p, live[j].n, live[j].value);
            ++count; used += live[j].n;
        }
        CHECK(s.allocation_count == count && s.allocated_bytes == used);
        printf("%zu %u %u %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu\n",
            iter, op, (unsigned)result, live[at].p ? (uintptr_t)live[at].p - (uintptr_t)f.base : SIZE_MAX,
            live[at].n, s.backing_bytes, s.allocated_bytes, s.allocation_count, s.span_count,
            s.total_free, s.largest_free, s.metadata_bytes, f.acquire_calls, f.release_calls, f.query_calls);
    }
    OK(dm_sa_destroy(a)); fixture_finish(&f);
    return 0;
}
