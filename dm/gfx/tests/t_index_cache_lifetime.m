// Real NVN buffer/pool lifetimes at one CPU address, with GPU reads completed
// before Finalize. The cache must not carry the old indices into the new pool.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
void crash_install(void);
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static void pool_init(NVNmemoryPool *pool, void *mem, size_t size) {
    NVNmemoryPoolBuilder b = {0}; h_nvnMemoryPoolBuilderSetDefaults(&b);
    h_nvnMemoryPoolBuilderSetStorage(&b, mem, size); h_nvnMemoryPoolBuilderSetFlags(&b, 0x24);
    if (!h_nvnMemoryPoolInitialize(pool, &b)) abort();
}
static void buffer_init(NVNbuffer *buffer, NVNmemoryPool *pool) {
    NVNbufferBuilder b = {0}; h_nvnBufferBuilderSetDefaults(&b);
    h_nvnBufferBuilderSetStorage(&b, pool, 0, 16);
    if (!h_nvnBufferInitialize(buffer, &b)) abort();
}
static void finish(void) {
    int done = mtl_wait(mtl_sub_commit(), 5000000000ull);
    CHECK(done, "GPU submission completed before resource retirement");
    CHECK(!mtl_last_commit_error(), "no GPU error");
    if (!done || mtl_last_commit_error()) abort();
}
static void retire(NVNbuffer *buffer, NVNmemoryPool *pool) {
    h_nvnBufferFinalize(buffer); h_nvnMemoryPoolFinalize(pool);
    // Releases are fenced to the next submission, even after a completed read.
    mtl_sub_begin(); finish(); defer_drain();
}
static void indices(void *mem, int size, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t values[] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        if (size == 1) ((uint8_t *)mem)[i] = values[i];
        else if (size == 2) ((uint16_t *)mem)[i] = values[i];
        else ((uint32_t *)mem)[i] = values[i];
    }
}
static void gpu_read(uint64_t address, int size, uint32_t a, uint32_t b, uint32_t c) {
    mtl_sub_begin();
    id<MTLBuffer> out = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; res_add(out);
    void *raw = NULL; uint64_t off = 0;
    if (!mtl_addr(address, 16, &raw, &off)) abort();
    enc_b_copy_buffer(blit(), (__bridge id<MTLBuffer>)raw, off, out, 0, 16); finish();
    uint8_t want[16] = {0}; uint32_t values[] = {a, b, c};
    for (int i = 0; i < 3; i++) memcpy(want + i * size, &values[i], size);
    CHECK(!memcmp(out.contents, want, 3 * size), "GPU reads current lifetime's indices");
    defer_obj_release(out);
}
static void check_type(int type) {
    size_t pg = (size_t)getpagesize(); int size = 1 << type;
    void *mem = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    void *other = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED || other == MAP_FAILED) abort();
    NVNmemoryPool pool, unrelated; NVNbuffer buffer;
    indices(mem, size, 2, 5, 4); pool_init(&pool, mem, pg); buffer_init(&buffer, &pool);
    p_draw_idx di = {.ibuf=h_nvnBufferGetAddress(&buffer), .count=3, .itype=type}; uint32_t lo=0, hi=0;
    CHECK(ix_range(&di, &lo, &hi) && lo == 2 && hi == 5, "initial cached range");
    CHECK(ix_range(&di, &lo, &hi) && lo == 2 && hi == 5, "unchanged cached range");
    gpu_read(di.ibuf, size, 2, 5, 4);
    const void *span = nvn_dirty_span(mem, 3 * size);
    // No submission still uses the old buffer. Finalize both guest objects,
    // then complete deferred Metal releases before reusing the storage.
    retire(&buffer, &pool);
    indices(mem, size, 10, 20, 30); pool_init(&pool, mem, pg); buffer_init(&buffer, &pool);
    CHECK(h_nvnBufferGetAddress(&buffer) == di.ibuf, "new buffer reuses CPU address");
    CHECK(nvn_dirty_span(mem, 3 * size) == span, "new pool reuses dirty slot");
    CHECK(ix_range(&di, &lo, &hi) && lo == 10 && hi == 30, "new lifetime does not reuse old min/max");
    gpu_read(di.ibuf, size, 10, 20, 30);
    dm_guest_write_prepare(mem, 3 * size); indices(mem, size, 40, 50, 60);
    CHECK(ix_range(&di, &lo, &hi) && lo == 40 && hi == 60, "live CPU write refreshes min/max");
    mtl_sub_begin(); mtl_sub_fill(di.ibuf, 16, 0);
    CHECK(!ix_range(&di, &lo, &hi), "pending GPU write is not scanned"); finish();
    mtl_sub_begin();
    CHECK(!pend_hits(di.ibuf, 16), "next submission has no pending write");
    CHECK(!ix_range(&di, &lo, &hi), "completed GPU write remains conservative");
    pool_init(&unrelated, other, pg); h_nvnMemoryPoolFinalize(&unrelated);
    CHECK(!ix_range(&di, &lo, &hi), "unrelated pool churn never clears gpuw"); finish();
    retire(&buffer, &pool);
    indices(mem, size, 70, 80, 90); pool_init(&pool, mem, pg); buffer_init(&buffer, &pool);
    CHECK(!ix_range(&di, &lo, &hi), "even replacement preserves gpuw conservatism");
    retire(&buffer, &pool);
    nvn_hm_clear(&g_ir, free); memset(g_vc_rgn, 0, sizeof g_vc_rgn);
    munmap(mem, pg); munmap(other, pg);
}
int main(void) { @autoreleasepool {
    crash_install(); mtl_init();
    for (int type = 0; type < 3; type++) check_type(type);
    printf("index cache lifetime: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
