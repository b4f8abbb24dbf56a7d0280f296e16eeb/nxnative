// Small real-VM test of ordinary dirty-range ownership. No Metal/device/game
// files are used. Including the implementation also lets the test verify exact
// live owners and hold the writer mutex across an actual CPU page fault.
#include "../nvn/nvn_dirty.c"
#include <signal.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static unsigned checks;
static uint64_t next_token = 1;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "[RED] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    exit(EXIT_FAILURE); } } while (0)

static uint64_t add(const void *mem, size_t size) {
    uint64_t token = 0;
    CHECK(nvn_pool_token_take(&next_token, &token));
    nvn_dirty_add_registration(token, mem, size);
    return token;
}
static range_t *owner(uint64_t token) {
    for (int i = 0; i < atomic_load(&g_nr); ++i)
        if (live_at(i) && rg(i)->registration == token) return rg(i);
    return NULL;
}
static int live_count(void) {
    int live, used; uint64_t adds;
    nvn_dirty_slot_stats(&live, &used, &adds);
    CHECK(live == atomic_load(&g_live));
    return live;
}
static uint8_t *map_pages(size_t n) {
    void *p = mmap(NULL, n * (size_t)getpagesize(), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(p != MAP_FAILED);
    return p;
}
static void check_write(uint8_t *mem, size_t size, const void *kept_span) {
    CHECK(nvn_dirty_tracked(mem, size));
    nvn_dirty_protect(mem, size);
    uint64_t stamp = nvn_dirty_now(), faults, preps;
    nvn_dirty_stats(&faults, &preps);
    CHECK(!nvn_dirty_since_span(kept_span, mem, size, stamp));
    // If the fault path starts taking the pool-registration mutex, this hangs
    // and the test's alarm terminates it instead of leaving CI stuck.
    pthread_mutex_lock(&g_slot_mu);
    ((volatile uint8_t *)mem)[size / 2] = 0x5a;
    pthread_mutex_unlock(&g_slot_mu);
    CHECK(mem[size / 2] == 0x5a);
    CHECK(atomic_load(&g_faults) > faults);
    CHECK(nvn_dirty_since_span(kept_span, mem, size, stamp));
}

static void order(int rounded, unsigned a, unsigned b, unsigned c, unsigned d) {
    size_t pg = (size_t)getpagesize();
    uint8_t *mem = map_pages(4);
    const size_t offset[] = {rounded ? 4096 : 0, rounded ? 4096 : 0,
                             rounded ? 4096 : 0, rounded ? 8192 : pg};
    const size_t size[] = {rounded ? 4096 : 2 * pg, rounded ? 4096 : 2 * pg,
                           rounded ? 12288 : 3 * pg, rounded ? 8192 : 3 * pg};
    uint64_t token[4]; range_t *slot[4]; const void *span[4];
    unsigned alive[] = {1, 1, 1, 1}, retire[] = {a, b, c, d};
    for (unsigned i = 0; i < 4; ++i) {
        token[i] = add(mem + offset[i], size[i]); slot[i] = owner(token[i]);
        CHECK(slot[i] != NULL);
        span[i] = nvn_dirty_span(mem + offset[i], size[i]);
    }
    CHECK(live_count() == 4);
    // Neither legacy pointer removal nor an invalid ordinary token can retire
    // a live ordinary range, even when its CPU start is exactly equal.
    nvn_dirty_remove_range(mem);
    nvn_dirty_remove_registration(0);
    nvn_dirty_add_registration(0, mem, pg);
    CHECK(live_count() == 4);
    for (unsigned i = 0; i < 4; ++i) check_write(mem + offset[i], size[i], span[i]);

    for (unsigned step = 0; step < 4; ++step) {
        unsigned victim = retire[step];
        nvn_dirty_remove_registration(token[victim]); alive[victim] = 0;
        CHECK(owner(token[victim]) == NULL);
        CHECK(live_count() == 3 - (int)step);
        // A delayed/double finalize must not retire a same-start survivor.
        nvn_dirty_remove_registration(token[victim]);
        CHECK(live_count() == 3 - (int)step);
        for (unsigned i = 0; i < 4; ++i) {
            CHECK((owner(token[i]) != NULL) == (int)alive[i]);
            if (!alive[i]) continue;
            CHECK(owner(token[i]) == slot[i]);
            CHECK(slot[i]->mem == (uintptr_t)(mem + offset[i]));
            CHECK(slot[i]->base == ((uintptr_t)(mem + offset[i]) & ~(pg - 1)));
            CHECK(slot[i]->end == (((uintptr_t)(mem + offset[i]) + size[i] + pg - 1) & ~(pg - 1)));
            // A kept span can name a retired different owner; its fallback must
            // find the survivor. Re-arm before testing the next CPU write.
            check_write(mem + offset[i], size[i], span[i]);
        }
        for (unsigned page = 0; page < 4; ++page) {
            int tracked = 0;
            for (unsigned i = 0; i < 4; ++i)
                tracked |= alive[i] && (uintptr_t)(mem + page * pg) >= slot[i]->base &&
                           (uintptr_t)(mem + (page + 1) * pg) <= slot[i]->end;
            CHECK(nvn_dirty_tracked(mem + page * pg, pg) == tracked);
        }
    }
    CHECK(live_count() == 0);
    // Final retirement restores real VM permissions, not only table metadata.
    uint64_t faults = atomic_load(&g_faults);
    for (unsigned i = 0; i < 4; ++i) ((volatile uint8_t *)mem)[i * pg] = 0x36;
    CHECK(atomic_load(&g_faults) == faults);
    CHECK(munmap(mem, 4 * pg) == 0);
}

static void overlap(unsigned victim) {
    size_t pg = (size_t)getpagesize();
    uint8_t *mem = map_pages(5);
    uint64_t token[] = {add(mem, 3 * pg), add(mem + 2 * pg, 3 * pg)};
    range_t *r[] = {owner(token[0]), owner(token[1])};
    nvn_dirty_protect(mem, 3 * pg);
    nvn_dirty_protect(mem + 2 * pg, 3 * pg);
    uint64_t stamp = nvn_dirty_now(), faults = atomic_load(&g_faults);
    pthread_mutex_lock(&g_slot_mu);
    ((volatile uint8_t *)mem)[2 * pg + 17] = 0x74;
    pthread_mutex_unlock(&g_slot_mu);
    CHECK(atomic_load(&g_faults) == faults + 2);
    CHECK(atomic_load(&r[0]->gen[2]) > stamp && atomic_load(&r[1]->gen[0]) > stamp);
    CHECK(atomic_load(&r[0]->state[2]) == PG_NONE && atomic_load(&r[1]->state[0]) == PG_NONE);
    nvn_dirty_protect(mem, 3 * pg);
    nvn_dirty_protect(mem + 2 * pg, 3 * pg);
    stamp = nvn_dirty_now();
    nvn_dirty_remove_registration(token[victim]);
    unsigned kept = 1 - victim, shared = kept ? 0 : 2, exclusive = kept ? 2 : 0;
    CHECK(owner(token[kept]) == r[kept]);
    CHECK(atomic_load(&r[kept]->gen[shared]) > stamp);
    CHECK(atomic_load(&r[kept]->state[shared]) == PG_NONE);
    CHECK(atomic_load(&r[kept]->state[exclusive]) == PG_PROT);
    CHECK(atomic_load(&r[kept]->gen[exclusive]) <= stamp);
    CHECK(nvn_dirty_since_span(r[kept], mem + kept * 2 * pg, 3 * pg, stamp));
    // Host pre-write still releases the survivor's untouched exclusive page.
    uint8_t *tail = mem + (kept * 2 + exclusive) * pg;
    dm_guest_write_prepare(tail, 1); tail[0] = 0x31;
    CHECK(atomic_load(&r[kept]->gen[exclusive]) > stamp);
    check_write(mem + kept * 2 * pg, 3 * pg, r[kept]);
    nvn_dirty_remove_registration(token[kept]);
    CHECK(live_count() == 0);
    CHECK(munmap(mem, 5 * pg) == 0);
}

static void legacy_and_reuse(void) {
    size_t pg = (size_t)getpagesize();
    uint8_t *mem = map_pages(3);
    uint64_t old = add(mem, pg);
    const void *stale = nvn_dirty_span(mem, pg);
    nvn_dirty_add_range(mem, 2 * pg);
    CHECK(live_count() == 2);
    nvn_dirty_remove_range(mem);
    CHECK(live_count() == 1 && owner(old) == stale);
    CHECK(!nvn_dirty_tracked(mem + pg, pg));
    nvn_dirty_add_range(mem, 2 * pg);
    nvn_dirty_remove_registration(old);
    CHECK(live_count() == 1 && nvn_dirty_tracked(mem + pg, pg));
    check_write(mem, 2 * pg, stale);
    nvn_dirty_remove_range(mem);
    CHECK(live_count() == 0);

    uint64_t newer = add(mem, 3 * pg);
    CHECK(newer != old && owner(newer) == stale); // Reused slot, not reused token.
    nvn_dirty_remove_registration(old);
    nvn_dirty_remove_range(mem);
    CHECK(live_count() == 1 && owner(newer) == stale);
    check_write(mem, 3 * pg, stale);
    nvn_dirty_remove_registration(newer);
    CHECK(live_count() == 0);
    // An inactive kept span must conservatively report dirty when no owner lives.
    CHECK(nvn_dirty_since_span(stale, mem, pg, nvn_dirty_now()));

    newer = add(mem + 2 * pg, pg); // Same slot, different coverage.
    CHECK(owner(newer) == stale);
    CHECK(nvn_dirty_since_span(stale, mem, pg, nvn_dirty_now()));
    check_write(mem + 2 * pg, pg, stale);
    nvn_dirty_remove_registration(newer);
    CHECK(munmap(mem, 3 * pg) == 0);
}

int main(void) {
    crash_install(); alarm(20);
    for (unsigned a = 0; a < 4; ++a)
        for (unsigned b = 0; b < 4; ++b) if (b != a)
            for (unsigned c = 0; c < 4; ++c) if (c != a && c != b)
                for (unsigned d = 0; d < 4; ++d) if (d != a && d != b && d != c)
                    { order(0, a, b, c, d); order(1, a, b, c, d); }
    overlap(0); overlap(1); legacy_and_reuse();
    CHECK(live_count() == 0);
    alarm(0);
    printf("t_dirty_registration: %u checks passed (24 retirement orders, "
           "exact owners, 4K-aligned rounded ranges, overlap generations, real VM faults with slot mutex held)\n", checks);
    return 0;
}
