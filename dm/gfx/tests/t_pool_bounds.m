/* Exercise the production pool index, both cache levels and public outputs.
 * Synthetic resident entries avoid device allocation; no GPU work is submitted. */
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static unsigned checks, failed;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "[RED] %s:%d: %s\n", __FILE__, __LINE__, #expr); failed++; } } while (0)

static void rejected(uint64_t addr, uint64_t len) {
    void *buf = &checks;
    uint64_t off = UINT64_MAX, gpu = UINT64_MAX;
    CHECK(!mtl_addr(addr, len, &buf, &off));
    CHECK(buf == NULL && off == 0);
    buf = &checks; off = UINT64_MAX;
    CHECK(!mtl_gpu_addr(addr, len, &buf, &off, &gpu));
    CHECK(buf == NULL && off == 0 && gpu == 0);
}

int main(void) { @autoreleasepool {
    /* Empty cache entries must never resolve address zero or a wrapped end. */
    t_pc.gen = atomic_load(&g_pool_gen);
    CHECK(pc_get(0, 0) == NULL);
    CHECK(pc_get(UINT64_MAX, 1) == NULL);
    rejected(0, 0);
    rejected(UINT64_MAX, 1);

    /* Buffers are only returned as opaque markers: res=1 avoids Metal calls. */
    id<MTLBuffer> marker = (id<MTLBuffer>)[NSObject new];
    g_npool = g_cap = 3; g_pidx_ok = 0; g_plast = -1;
    g_pools = calloc((size_t)g_cap, sizeof *g_pools);
    if (!g_pools) abort();
    g_pools[0] = (pool_ent){0x1003, 0x3000, 0x1000, marker, 0x100000, NULL, 1};
    g_pools[1] = (pool_ent){0x2000, 0x2800, 0x2000, marker, 0x200000, NULL, 1};
    g_pools[2] = (pool_ent){0x8000, 0x9000, 0x8000, marker, 0x300000, NULL, 1};
    CHECK(pool_find(0x8000, 0x1000) == 2); /* sorted index */
    CHECK(pool_find(0x8fff, 1) == 2);      /* last-hit path */
    g_plast = -1;
    CHECK(pool_find(0x2500, 0x800) == 0);  /* overlap fallback */
    CHECK(pool_find(UINT64_MAX - 7, 16) == -1);
    g_plast = -1;
    CHECK(pool_find(UINT64_MAX - 7, 16) == -1);

    void *buf; uint64_t off, gpu;
    CHECK(mtl_gpu_addr(0x1003, 1, &buf, &off, &gpu));
    CHECK(buf == (__bridge void *)marker && off == 3 && gpu == 0x100003);
    CHECK(pc_get(0x1003, 1) != NULL);      /* direct cache */
    memset(t_pc.e, 0, sizeof t_pc.e);
    CHECK(pc_get(0x1003, 1) != NULL);      /* recent cache repopulates direct */
    CHECK(t_pc.e[pc_slot(0x1003)].lo == 0x1003);
    rejected(UINT64_MAX - 7, 16);
    rejected(0x1003, UINT64_MAX);
    rejected(0x1003, 0);
    rejected(0x1002, 1);
    rejected(0x3000, 1);
    rejected(0x3000, 0);
    CHECK(mtl_addr(0x2fff, 1, &buf, &off) && off == 0x1fff);
    CHECK(!mtl_addr(0x1003, 1, NULL, &off));
    CHECK(!mtl_addr(0x1003, 1, &buf, NULL));
    CHECK(!mtl_gpu_addr(0x1003, 1, &buf, &off, NULL));

    /* CopyBufferToBuffer records zero sizes; like fill/update it is a no-op. */
    uint64_t epoch = g_mdx_epoch;
    mtl_sub_copy(0, UINT64_MAX, 0);
    CHECK(g_mdx_epoch == epoch);
    free(g_pidx); free(g_pools);
    printf("t_pool_bounds: %u checks, %u failed\n", checks, failed);
    return failed != 0;
} }
