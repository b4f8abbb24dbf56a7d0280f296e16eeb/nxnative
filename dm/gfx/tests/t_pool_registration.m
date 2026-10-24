// Real NVN Initialize/GetAddress/Finalize over the production Metal pool index.
// Public GPU addresses remain CPU identity; caller-owned storage stays alive.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
void crash_install(void);
uint64_t now_ns(void) { return bp_now(); }
static unsigned checks, failed;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "[RED] %s:%d: %s\n", __FILE__, __LINE__, #expr); failed++; } } while (0)

static int dirty_live(void) {
    int live, peak, cap; nvn_dirty_stat(&live, &peak, &cap); return live;
}
static uint64_t initialize(NVNmemoryPool *pool, void *mem, size_t size) {
    NVNmemoryPoolBuilder b = {0};
    h_nvnMemoryPoolBuilderSetDefaults(&b);
    h_nvnMemoryPoolBuilderSetStorage(&b, mem, size);
    h_nvnMemoryPoolBuilderSetFlags(&b, 0x24);
    CHECK(h_nvnMemoryPoolInitialize(pool, &b));
    CHECK(h_nvnMemoryPoolGetBufferAddress(pool) == (uintptr_t)mem);
    return ((pool_t *)pool)->registration;
}
static void getters(NVNmemoryPool *pool) {
    pool_t *p = (pool_t *)pool;
    NVNbufferBuilder bb = {0}; NVNbuffer b = {0};
    h_nvnBufferBuilderSetDefaults(&bb);
    h_nvnBufferBuilderSetStorage(&bb, pool, 1024, 256);
    CHECK(h_nvnBufferInitialize(&b, &bb));
    CHECK(h_nvnBufferGetAddress(&b) == (uintptr_t)p->mem + 1024);
    CHECK(h_nvnBufferMap(&b) == p->mem + 1024);
    h_nvnBufferFinalize(&b);
    NVNtextureBuilder tb = {0}; NVNtexture t = {0};
    h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetSize2D(&tb, 4, 4);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_R8);
    h_nvnTextureBuilderSetStorage(&tb, pool, 2048);
    CHECK(h_nvnTextureInitialize(&t, &tb));
    CHECK(h_nvnTextureGetTextureAddress(&t) == (uintptr_t)p->mem + 2048);
    h_nvnTextureFinalize(&t);
}
int main(void) { @autoreleasepool {
    crash_install(); mtl_init();
    const size_t pg = (size_t)getpagesize(); unsigned char *storage;
    if (posix_memalign((void **)&storage, pg, 8 * pg)) abort();
    memset(storage, 0, 8 * pg);
    unsigned char *base = storage + 4096;
    const int initial = g_npool, resident = g_pool_res_n, dirty_initial = dirty_live();
    NVNmemoryPool pools[4] = {0}; uint64_t token[4]; uintptr_t view[4];
    token[0] = initialize(&pools[0], base, 2 * pg);
    token[1] = initialize(&pools[1], base, 2 * pg);       // exact duplicate
    token[2] = initialize(&pools[2], base, 3 * pg);       // same start, different extent
    token[3] = initialize(&pools[3], base + 4096, 2 * pg); // partial overlap
    CHECK(g_npool == initial + 4 && dirty_live() == dirty_initial + 4);
    CHECK(g_pool_res_n == resident); // registration/getters keep residency lazy
    for (int k = 0; k < 4; ++k) {
        CHECK(token[k] && (!k || token[k] > token[k - 1]));
        getters(&pools[k]);
        pthread_mutex_lock(&g_mu);
        int i = pool_find_registration(token[k], NULL);
        CHECK(i >= 0);
        view[k] = i >= 0 ? (uintptr_t)(__bridge void *)g_pools[i].buf : 0;
        if (i >= 0) CHECK(g_pools[i].key == ((pool_t *)&pools[k])->mem);
        pthread_mutex_unlock(&g_mu);
    }
    uint64_t next = g_pool_registration_next;
    mtl_pool_add(base, 2 * pg); // the identity-only API used by sparse runs
    CHECK(g_pool_registration_next == next && g_npool == initial + 5);
    mtl_pool_del(base);
    CHECK(g_npool == initial + 4);
    mtl_pool_del(base); // cannot delete an ordinary pool at the same CPU address
    CHECK(g_npool == initial + 4);
    mtl_pool_add(base, 2 * pg); // ordinary retirement must preserve this legacy entry
    const int order[] = {1, 0, 3, 2}; int alive[] = {1, 1, 1, 1};
    for (int n = 0; n < 4; ++n) {
        int victim = order[n];
        h_nvnMemoryPoolFinalize(&pools[victim]); alive[victim] = 0;
        CHECK(((pool_t *)&pools[victim])->registration == 0);
        CHECK(g_npool == initial + 4 - n && dirty_live() == dirty_initial + 3 - n);
        mtl_pool_unregister(token[victim]); // repeated retirement cannot select another owner
        CHECK(g_npool == initial + 4 - n);
        pthread_mutex_lock(&g_mu);
        CHECK(pool_find_registration(0, base) >= 0);
        for (int k = 0; k < 4; ++k) {
            int i = pool_find_registration(token[k], NULL);
            CHECK((i >= 0) == alive[k]);
            if (i >= 0) {
                CHECK(g_pools[i].key == ((pool_t *)&pools[k])->mem);
                CHECK((uintptr_t)(__bridge void *)g_pools[i].buf == view[k]);
            }
        }
        pthread_mutex_unlock(&g_mu);
        for (int k = 0; k < 4; ++k) if (alive[k]) getters(&pools[k]);
    }

    mtl_pool_del(base);
    CHECK(g_npool == initial && dirty_live() == dirty_initial);

    // Translate/capture while registered, then retire only metadata before the
    // encoded copy completes. Caller storage remains alive and is not reused.
    NVNmemoryPool source = {0}, dest = {0};
    uint64_t old = initialize(&source, base, pg);
    initialize(&dest, storage + 6 * pg, pg);
    for (unsigned k = 0; k < 64; ++k) base[k] = (unsigned char)(3 * k + 1);
    void *buffer = NULL; uint64_t off = 0;
    CHECK(mtl_addr((uintptr_t)base, 64, &buffer, &off));
    if (!buffer) abort();
    id<MTLBuffer> captured = (__bridge id<MTLBuffer>)buffer;
    unsigned char *cpu = (unsigned char *)captured.contents + off;
    mtl_sub_begin(); mtl_sub_copy((uintptr_t)base, (uintptr_t)storage + 6 * pg, 64);
    h_nvnMemoryPoolFinalize(&source);
    pthread_mutex_lock(&g_deferred_mu);
    int retired = 0;
    for (NvDeferredRelease *d in g_deferred) retired += d->object == captured && !d->keep_resident;
    CHECK(retired == 1); // exact no-copy view goes through the existing deferred path
    pthread_mutex_unlock(&g_deferred_mu);
    CHECK(memcmp(cpu, base, 64) == 0); // caller-owned allocation is still alive
    uint64_t replacement = initialize(&source, base, pg);
    CHECK(replacement > old);
    pthread_mutex_lock(&g_mu);
    CHECK(pool_find_registration(old, NULL) < 0);
    CHECK(pool_find_registration(replacement, NULL) >= 0);
    pthread_mutex_unlock(&g_mu);
    mtl_pool_unregister(old); // stale token cannot delete the replacement
    CHECK(h_nvnMemoryPoolGetBufferAddress(&source) == (uintptr_t)base);
    int complete = mtl_wait(mtl_sub_commit(), 5000000000ull);
    CHECK(complete); if (!complete) abort();
    CHECK(mtl_last_commit_error() == 0);
    CHECK(memcmp(storage + 6 * pg, base, 64) == 0);
    h_nvnMemoryPoolFinalize(&source); h_nvnMemoryPoolFinalize(&dest);
    CHECK(g_npool == initial && g_pool_res_n == resident && dirty_live() == dirty_initial);
    // Existing retirement needs a following completed submission; do not free
    // borrowed CPU storage while its no-copy views may still be in flight.
    mtl_sub_begin(); complete = mtl_wait(mtl_sub_commit(), 5000000000ull);
    CHECK(complete); if (!complete) abort();
    mtl_drain(); defer_drain(); captured = nil; free(storage);
    printf("pool registration: %u checks, %u failed\n", checks, failed);
    return failed != 0;
} }
