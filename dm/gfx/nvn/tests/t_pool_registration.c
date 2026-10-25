/* Portable registration-identity regression. The token issuer/selector are the
 * production helpers; this tiny registry only models ownership and page-rounded
 * views, not a runtime map or backing-store lease.
 * cc -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
 *   dm/gfx/nvn/tests/t_pool_registration.c -o /tmp/t_pool_registration
 * /tmp/t_pool_registration
 */
#include "../nvn_pool_registration.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned long checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "[RED] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    exit(EXIT_FAILURE); } } while (0)

typedef struct { uint64_t token, size; uintptr_t base, end; unsigned char *cpu; } entry;
typedef struct { entry e[8]; unsigned n; uint64_t next; uintptr_t page; } registry;
static uint64_t add(registry *r, unsigned char *cpu, uint64_t size, int ordinary) {
    entry e = {.cpu = cpu, .size = size};
    CHECK(r->n < sizeof r->e / sizeof *r->e);
    if (ordinary) CHECK(nvn_pool_token_take(&r->next, &e.token));
    e.base = (uintptr_t)cpu & ~(r->page - 1);
    e.end = ((uintptr_t)cpu + size + r->page - 1) & ~(r->page - 1);
    r->e[r->n++] = e;
    return e.token;
}
static int find(const registry *r, uint64_t token, const void *cpu) {
    for (unsigned i = 0; i < r->n; ++i)
        if (nvn_pool_registration_match(r->e[i].token, r->e[i].cpu, token, cpu)) return (int)i;
    return -1;
}
static int remove_registration(registry *r, uint64_t token, const void *cpu) {
    int i = find(r, token, cpu);
    if (i < 0) return 0;
    r->e[i] = r->e[--r->n];
    return 1;
}
static void order(uintptr_t page, unsigned a, unsigned b, unsigned c, unsigned d) {
    _Alignas(16384) unsigned char bytes[32768] = {0};
    registry r = {.next = 1, .page = page};
    uint64_t tokens[4]; uintptr_t bases[4], ends[4];
    const unsigned offsets[] = {4096, 4096, 4096, 8192};
    const unsigned sizes[] = {4096, 4096, 12288, 8192};
    for (unsigned i = 0; i < 4; ++i) {
        tokens[i] = add(&r, bytes + offsets[i], sizes[i], 1);
        CHECK(tokens[i] == i + 1);
        bases[i] = r.e[r.n - 1].base; ends[i] = r.e[r.n - 1].end;
        CHECK(bases[i] <= (uintptr_t)bytes + offsets[i]);
        CHECK(ends[i] >= (uintptr_t)bytes + offsets[i] + sizes[i]);
    }
    if (page == 16384) {
        CHECK(bases[0] == bases[3] && ends[0] == ends[3]);
        CHECK(bytes + offsets[0] + sizes[0] == bytes + offsets[3]);
    } else CHECK(bases[0] != bases[3]);
    CHECK(add(&r, bytes + 4096, 4096, 0) == 0);
    CHECK(r.next == 5); /* Sparse/legacy runs do not consume ordinary identity. */
    CHECK(remove_registration(&r, 0, bytes + 4096));
    CHECK(!remove_registration(&r, 0, bytes + 4096));
    CHECK(r.n == 4); /* Legacy removal cannot consume an ordinary registration. */

    unsigned retire[] = {a, b, c, d}, alive[] = {1, 1, 1, 1};
    for (unsigned step = 0; step < 4; ++step) {
        unsigned victim = retire[step];
        int at = find(&r, tokens[victim], NULL);
        CHECK(at >= 0);
        entry captured = r.e[at];
        CHECK(remove_registration(&r, tokens[victim], NULL));
        CHECK(!remove_registration(&r, tokens[victim], NULL));
        alive[victim] = 0;
        for (unsigned i = 0; i < 4; ++i) {
            at = find(&r, tokens[i], NULL);
            CHECK((at >= 0) == (int)alive[i]);
            if (alive[i]) {
                CHECK(r.e[at].base == bases[i] && r.e[at].end == ends[i]);
                CHECK(r.e[at].cpu == bytes + offsets[i] && r.e[at].size == sizes[i]);
            }
        }
        /* Caller deliberately keeps these bytes alive; this is no free/unmap
         * test and does not claim a storage lease after registration retires. */
        captured.cpu[31] = (unsigned char)(step + 1);
        CHECK(bytes[offsets[victim] + 31] == step + 1);
        uint64_t newer = add(&r, captured.cpu, captured.size, 1);
        CHECK(newer > tokens[3] && find(&r, tokens[victim], NULL) < 0);
        CHECK(!remove_registration(&r, tokens[victim], NULL));
        CHECK(find(&r, newer, NULL) >= 0);
        CHECK(remove_registration(&r, newer, NULL));
    }
    CHECK(r.n == 0);
}
static void exhaustion(void) {
    uint64_t next = UINT64_MAX - 1, token = 0;
    CHECK(nvn_pool_token_take(&next, &token) && token == UINT64_MAX - 1);
    CHECK(nvn_pool_token_take(&next, &token) && token == UINT64_MAX && next == 0);
    token = 17;
    CHECK(!nvn_pool_token_take(&next, &token) && next == 0 && token == 17);
    next = 1;
    CHECK(!nvn_pool_token_take(NULL, &token) && token == 17);
    CHECK(!nvn_pool_token_take(&next, NULL) && next == 1);
    CHECK(!nvn_pool_token_take(NULL, NULL));
    CHECK(!nvn_pool_registration_match(1, NULL, 0, NULL));
    CHECK(!nvn_pool_registration_match(0, NULL, 1, NULL));
    CHECK(nvn_pool_registration_match(UINT64_MAX, NULL, UINT64_MAX, NULL));
}
int main(void) {
    for (uintptr_t page = 4096; page <= 16384; page *= 4)
        for (unsigned a = 0; a < 4; ++a)
            for (unsigned b = 0; b < 4; ++b) if (b != a)
                for (unsigned c = 0; c < 4; ++c) if (c != a && c != b)
                    for (unsigned d = 0; d < 4; ++d) if (d != a && d != b && d != c)
                        order(page, a, b, c, d);
    exhaustion();
    printf("pool_registration: %lu checks passed (24 retirement orders at 4K/16K, "
           "duplicates, rounded overlaps, legacy isolation, stale tokens, exhaustion)\n", checks);
    return EXIT_SUCCESS;
}
