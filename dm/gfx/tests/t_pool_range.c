/* Portable checks for the arithmetic used by the live Metal pool index. */
#include "../nvn/nvn_pool_range.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "[RED] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    exit(EXIT_FAILURE); } } while (0)

static void spans(void) {
    const uint64_t v[] = {0, 1, 2, 15, 16, 17, 4095, 4096, 4097,
        UINT64_C(1) << 32, UINT64_C(1) << 63, UINT64_MAX - 16, UINT64_MAX - 1, UINT64_MAX};
    const size_t n = sizeof v / sizeof *v;
    for (size_t l = 0; l < n; ++l)
        for (size_t h = 0; h < n; ++h)
            for (size_t a = 0; a < n; ++a)
                for (size_t k = 0; k < n; ++k) {
                    /* Independent checked-end reference: addition cannot wrap. */
                    int expected = v[l] < v[h] && v[k] != 0 && v[a] >= v[l] &&
                        v[k] <= UINT64_MAX - v[a] && v[a] + v[k] <= v[h];
                    CHECK(nvn_pool_span(v[l], v[h], v[a], v[k]) == expected);
                }
    CHECK(nvn_pool_span(100, 200, 100, 100));
    CHECK(nvn_pool_span(100, 200, 199, 1));
    CHECK(!nvn_pool_span(100, 200, 200, 0));
    CHECK(!nvn_pool_span(0, 0, 0, 0));
    CHECK(!nvn_pool_span(100, 200, UINT64_MAX - 7, 16));
}

static void rejected(uintptr_t lo, uint64_t size, uintptr_t page) {
    uintptr_t hi = 11, base = 22, end = 33;
    CHECK(!nvn_pool_page_range(lo, size, page, &hi, &base, &end));
    CHECK(hi == 11 && base == 22 && end == 33);
}

static void pages(void) {
    for (uintptr_t page = 1; page <= 65536; page *= 2) {
        const uintptr_t last = UINTPTR_MAX & ~(page - 1);
        uintptr_t hi, base, end;
        CHECK(nvn_pool_page_range(page + 1, page, page, &hi, &base, &end));
        CHECK(hi == 2 * page + 1 && base == ((page + 1) & ~(page - 1)));
        CHECK(end == ((hi + page - 1) & ~(page - 1)));
        CHECK(nvn_pool_page_range(last - page, page, page, &hi, &base, &end));
        CHECK(hi == last && base == last - page && end == last);
        rejected(last, page, page);          /* size addition or end rounding wraps */
        rejected(UINTPTR_MAX - 3, 4, page); /* size addition wraps */
        if (page > 1) rejected(last - 1, 2, page); /* only page rounding wraps */
    }
    rejected(0, 1, 4096);
    rejected(4096, 0, 4096);
    rejected(4096, 1, 0);
    rejected(4096, 1, 3);
    rejected(4096, UINT64_MAX, 4096);
}

int main(void) {
    spans(); pages();
    printf("t_pool_range: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
