// Direct regression coverage of production SVC stack-map ownership. No second
// MapMemory implementation: only unrelated host services and protection fault
// injection are substituted. Native bytes remain copy/move, not shared aliases.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../dm.h"

#ifndef __APPLE__
static void *pthread_get_stackaddr_np(pthread_t thread) { (void)thread; return NULL; }
static size_t pthread_get_stacksize_np(pthread_t thread) { (void)thread; return 0; }
#endif
static size_t host_page(void) {
#ifdef DM_TEST_HOST_PAGE_16K
    assert(getpagesize() <= 0x4000);
    return 0x4000;
#else
    return (size_t)getpagesize();
#endif
}
static int test_getpagesize(void) { return (int)host_page(); }
static void *test_mmap(void *hint, size_t size, int prot, int flags, int fd, off_t off) {
    // A Linux reservation need not be 16 KiB aligned; simulate that host's
    // reservation alignment as well as its protection granularity.
    assert(!hint && fd == -1 && !off);
    size_t pg = host_page();
    void *raw = mmap(NULL, size + pg, prot, flags, fd, off);
    if (raw == MAP_FAILED) return raw;
    uintptr_t lo = (uintptr_t)raw, aligned = (lo + pg - 1) & ~(uintptr_t)(pg - 1);
    if (aligned != lo) assert(!munmap(raw, aligned - lo));
    assert(!munmap((void *)(aligned + size), lo + size + pg - aligned - size));
    return (void *)aligned;
}
static _Atomic int fail_protect;
static _Atomic unsigned protect_calls, write_prepares;
static _Atomic uintptr_t prepared_base;
static _Atomic size_t prepared_size;
static uintptr_t dirty_readonly_base;
static size_t dirty_readonly_size;
static int test_mprotect(void *address, size_t size, int permission) {
    assert(!((uintptr_t)address & (host_page() - 1)));
    assert(!(size & (host_page() - 1)) && size);
    atomic_fetch_add(&protect_calls, 1);
    if (atomic_exchange(&fail_protect, 0)) { errno = EACCES; return -1; }
    return mprotect(address, size, permission);
}
#define getpagesize test_getpagesize
#define mmap test_mmap
static _Noreturn void test_heap_abort(void);
#define abort test_heap_abort
#define mprotect test_mprotect
#include "../hle_svc.c"
#undef mprotect
#undef abort
#undef mmap
#undef getpagesize

module_t g_mod[MAX_MOD];
int g_nmod;
struct dm_flags g_flag;
void dm_guest_write_prepare(void *p, size_t size) {
    atomic_fetch_add(&write_prepares, 1);
    atomic_store(&prepared_base, (uintptr_t)p);
    atomic_store(&prepared_size, size);
    // Model a GPU dirty tracker's read-only host page. Copyback must call the
    // real pre-write boundary before memcpy, even though no alias is created.
    if (dirty_readonly_size && (uintptr_t)p < dirty_readonly_base + dirty_readonly_size &&
        dirty_readonly_base < (uintptr_t)p + size) {
        assert(!mprotect((void *)dirty_readonly_base, dirty_readonly_size, PROT_READ | PROT_WRITE));
        dirty_readonly_size = 0;
    }
}
module_t *module_of(const void *p) {
    uintptr_t address = (uintptr_t)p;
    for (int i = 0; i < g_nmod; ++i) {
        uintptr_t b = (uintptr_t)g_mod[i].base;
        if (address >= b && address - b < g_mod[i].size) return &g_mod[i];
        if (g_mod[i].patch_off < 0) {
            uintptr_t patch = b + g_mod[i].patch_off;
            if (address >= patch && address - patch < g_mod[i].patch_len) return &g_mod[i];
        }
    }
    return NULL;
}
void hlecount_svc(int num) { (void)num; }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void guest_clock_read_wait(void) {}
uint64_t guest_ns(void) { return 0; }
uint64_t dm_entropy64(void) { return 0; }
int thr_limit_used(void) { return 0; }

#include "memory_failure_test.h"

#include "heap_failure_test.h"

enum { PAGE = 0x1000, BAD_SIZE = 0xca01, BAD_ADDRESS = 0xcc01,
       NO_RESOURCE = 0xce01, BAD_STATE = 0xd401, BAD_REGION = 0xdc01 };
// Public libnx/Horizon MemoryInfo ABI, including its final reserved word.
// The independently declared layout catches both underwrite and overwrite.
typedef struct {
    uint64_t base, size;
    uint32_t state, attr, perm, ipc, device, padding;
} memory_info;
_Static_assert(sizeof(memory_info) == 40, "MemoryInfo size");
_Static_assert(offsetof(memory_info, padding) == 36, "MemoryInfo padding offset");
static uintptr_t alias_base, stack_base, heap_base;
static uint64_t call(unsigned svc, uint64_t x0, uint64_t x1, uint64_t x2) {
    uint64_t regs[31] = {x0, x1, x2};
    svc_dispatch(svc, regs, 0); return regs[0];
}
static uint64_t info(unsigned id) {
    uint64_t regs[31] = {0, id};
    svc_dispatch(0x29, regs, 0); assert(!regs[0]); return regs[1];
}
static uint64_t pages(int commit, uintptr_t a, size_t n) { return call(commit ? 0x2c : 0x2d, a, n, 0); }
static uint64_t heap(size_t n) { return call(1, 0, n, 0); }
static uint64_t map(uintptr_t d, uintptr_t s, size_t n) { return call(4, d, s, n); }
static uint64_t unmap(uintptr_t d, uintptr_t s, size_t n) { return call(5, d, s, n); }
static memory_info query(uintptr_t address) {
    // Alternate ABI consumers do not have to align the output object. The
    // deliberately unaligned pointer also catches typed stores under UBSan.
    _Alignas(8) uint8_t out[17 + 40 + 16];
    memset(out, 0xa7, sizeof out);
    uint32_t not_an_svc_output = 0xfedcba98;
    uint64_t regs[31] = {(uintptr_t)(out + 17), (uintptr_t)&not_an_svc_output, address};
    svc_dispatch(6, regs, 0);
    assert(!regs[0] && !regs[1]);
    // The SVC returns PageInfo in x1. The SDK wrapper, not the dispatcher,
    // writes the caller's PageInfo pointer.
    assert(not_an_svc_output == 0xfedcba98);
    for (size_t i = 0; i < 17; ++i) assert(out[i] == 0xa7);
    for (size_t i = 17 + 40; i < sizeof out; ++i) assert(out[i] == 0xa7);
    memory_info mi;
    memcpy(&mi, out + 17, sizeof mi);
    assert(!mi.ipc && !mi.device && !mi.padding);
    assert(mi.size && address >= mi.base && address - mi.base < mi.size);
    return mi;
}
static void expect_query(uintptr_t address, uintptr_t base, size_t size, unsigned state, unsigned attr, unsigned perm) {
    memory_info mi = query(address);
    assert(mi.base == base && mi.size == size);
    assert(mi.state == state && mi.attr == attr && mi.perm == perm);
}
static void expect_state(uintptr_t address, unsigned state, unsigned attr, unsigned perm) {
    memory_info mi = query(address);
    assert(mi.state == state && mi.attr == attr && mi.perm == perm);
}
static void expect_prepared(uintptr_t base, size_t size) {
    assert(atomic_load(&prepared_base) == base && atomic_load(&prepared_size) == size);
}

static void invalid_inputs(void) {
    uintptr_t a = alias_base, s = stack_base, h = heap_base;
    assert(!pages(1, a, 4 * PAGE));
    assert(!heap(0x200000));
    for (unsigned svc = 4; svc <= 5; ++svc) {
        assert(call(svc, s + 1, a, PAGE) == BAD_ADDRESS);
        assert(call(svc, s, a + 1, PAGE) == BAD_ADDRESS);
        assert(call(svc, s + 1, a, 0) == BAD_ADDRESS);
        assert(call(svc, s, a, 0) == BAD_SIZE);
        assert(call(svc, s, a, PAGE + 1) == BAD_SIZE);
        assert(call(svc, s, UINTPTR_MAX - PAGE + 1, 2 * PAGE) == BAD_STATE);
        assert(call(svc, UINTPTR_MAX - PAGE + 1, a, 2 * PAGE) == BAD_STATE);
        assert(call(svc, s, a, SIZE_MAX & ~(size_t)(PAGE - 1)) == BAD_STATE);
        assert(call(svc, s - PAGE, a, PAGE) == BAD_REGION);
        assert(call(svc, s + DM_GAME_MEM_STACK - PAGE, a, 2 * PAGE) == BAD_REGION);
        assert(call(svc, a, a, PAGE) == BAD_REGION);
        assert(call(svc, s, 0, PAGE) == BAD_STATE);
    }
    assert(map(s, a + 3 * PAGE, 2 * PAGE) == BAD_STATE); // partly uncommitted
    assert(map(s, a + 4 * PAGE, PAGE) == BAD_STATE);
    assert(map(s, a - PAGE, PAGE) == BAD_STATE);
    assert(map(s, a + DM_GAME_MEM_ALIAS - PAGE, 2 * PAGE) == BAD_STATE);
    assert(map(s, h + 0x200000 - PAGE, 2 * PAGE) == BAD_STATE);
    assert(map(s, h + 0x200000, PAGE) == BAD_STATE);
    assert(map(s, s, PAGE) == BAD_STATE);
    assert(map(s, s + PAGE, PAGE) == BAD_STATE);
    void *host = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(host != MAP_FAILED);
    assert(map(s, (uintptr_t)host, PAGE) == BAD_STATE);
    assert(!munmap(host, 0x4000));
    assert(!g_nmap && alias_mapped() == 4 * PAGE);
    expect_query(a, a, 4 * PAGE, 5, 0, 3);
    assert(!pages(0, a, 4 * PAGE));
    assert(!heap(0));
}

static void alias_lifecycle(void) {
    uintptr_t a = alias_base, d = stack_base + PAGE, src = a + 4 * PAGE;
    assert(!pages(1, a, 16 * PAGE));
    memset((void *)a, 0x39, 16 * PAGE);
    uint64_t used = info(I_UsedMem);
    unsigned prepares = atomic_load(&write_prepares);
    assert(!map(d, src, 2 * PAGE));
    assert(atomic_load(&write_prepares) == prepares + 1);
    uintptr_t page_base = d & ~(uintptr_t)(host_page() - 1);
    size_t page_size = ((d + 2 * PAGE + host_page() - 1) & ~(uintptr_t)(host_page() - 1)) - page_base;
    expect_prepared(page_base, page_size);
    assert(info(I_UsedMem) == used); // transfer ownership, not a second pool charge
    expect_query(src, src, 2 * PAGE, 5, 1, 0);
    expect_query(src + 2 * PAGE - 1, src, 2 * PAGE, 5, 1, 0);
    expect_query(a + PAGE, a, 4 * PAGE, 5, 0, 3);
    expect_query(a + 7 * PAGE, a + 6 * PAGE, 10 * PAGE, 5, 0, 3);
    expect_query(d, d, 2 * PAGE, 0xb, 0, 3);
    for (size_t i = 0; i < 2 * PAGE; ++i) assert(((uint8_t *)d)[i] == 0x39);
    memset((void *)d, 0x72, 2 * PAGE);
    // The native implementation is explicitly copy/move, not true aliasing.
    // Logical no-permission ownership does not make the retained source slice
    // inaccessible on a shared host page, nor make destination writes visible.
    assert(*(volatile uint8_t *)src == 0x39);
    assert(map(d, a + 10 * PAGE, PAGE) == BAD_STATE);
    assert(map(d + PAGE, a + 10 * PAGE, 2 * PAGE) == BAD_STATE);
    assert(map(stack_base + 0x10000, src, PAGE) == BAD_STATE);
    assert(map(stack_base + 0x10000, src - PAGE, 2 * PAGE) == BAD_STATE);
    assert(map(stack_base + 0x10000, src + PAGE, 2 * PAGE) == BAD_STATE);
    assert(map(stack_base + 0x10000, d, PAGE) == BAD_STATE);
    assert(unmap(d, src, PAGE) == BAD_STATE);
    assert(unmap(d + PAGE, src + PAGE, PAGE) == BAD_STATE);
    assert(unmap(d, src + PAGE, 2 * PAGE) == BAD_STATE);
    assert(unmap(d, src, 3 * PAGE) == BAD_STATE);
    assert(unmap(d + 0x10000, src, 2 * PAGE) == BAD_STATE);
    expect_query(src, src, 2 * PAGE, 5, 1, 0);
    assert(*(volatile uint8_t *)src == 0x39);
    assert(pages(0, src, PAGE) == BAD_STATE);
    assert(pages(0, src - PAGE, 2 * PAGE) == BAD_STATE);
    assert(pages(0, src + PAGE, 2 * PAGE) == BAD_STATE);
    assert(pages(0, a, 16 * PAGE) == BAD_STATE);
    // Upstream game evidence: recommit across a borrowed source is a no-op
    // for live bytes, including when an SDK extends its committed window.
    assert(!pages(1, src, PAGE));
    assert(!pages(1, src - PAGE, 2 * PAGE));
    assert(!pages(1, src + PAGE, 2 * PAGE));
    assert(!pages(1, a, 16 * PAGE));
    assert(*(volatile uint8_t *)src == 0x39 && *(volatile uint8_t *)d == 0x72);
    // Existing permission/attribute stubs must not claim to modify a source
    // while the stack mapping owns it. Unrelated behavior remains unchanged.
    for (unsigned svc = 2; svc <= 3; ++svc) {
        assert(call(svc, src, PAGE, 0) == BAD_STATE);
        assert(call(svc, src - PAGE, 2 * PAGE, 3) == BAD_STATE);
        assert(call(svc, src + PAGE, 2 * PAGE, 3) == BAD_STATE);
        assert(!call(svc, a, PAGE, 3));
    }
    expect_query(src, src, 2 * PAGE, 5, 1, 0);
    assert(alias_mapped() == 16 * PAGE && info(I_UsedMem) == used);
    // Unrelated logical siblings can decommit/recommit without losing the
    // owned source. This also exercises 4 KiB slices of a 16 KiB host page.
    assert(!pages(0, src - PAGE, PAGE));
    assert(!pages(1, src - PAGE, PAGE));
    assert(*(volatile uint8_t *)src == 0x39);
    assert(!map(d + 2 * PAGE, a + 8 * PAGE, PAGE));
    expect_query(a + 6 * PAGE, a + 6 * PAGE, 2 * PAGE, 5, 0, 3);
    expect_query(a + 8 * PAGE, a + 8 * PAGE, PAGE, 5, 1, 0);
    expect_query(a + 9 * PAGE, a + 9 * PAGE, 7 * PAGE, 5, 0, 3);
    memset((void *)(d + 2 * PAGE), 0x8b, PAGE);
    dirty_readonly_base = src & ~(uintptr_t)(host_page() - 1);
    dirty_readonly_size = host_page();
    assert(!mprotect((void *)dirty_readonly_base, dirty_readonly_size, PROT_READ));
    prepares = atomic_load(&write_prepares);
    assert(!unmap(d, src, 2 * PAGE));
    assert(!dirty_readonly_size && atomic_load(&write_prepares) == prepares + 1);
    expect_prepared(src, 2 * PAGE);
    for (size_t i = 0; i < 2 * PAGE; ++i) assert(((uint8_t *)src)[i] == 0x72);
    expect_query(src, a, 8 * PAGE, 5, 0, 3);
    expect_state(d, 0, 0, 0);
    expect_query(d + 2 * PAGE, d + 2 * PAGE, PAGE, 0xb, 0, 3);
    assert(*(volatile uint8_t *)(d + 2 * PAGE) == 0x8b);
    assert(unmap(d, src, 2 * PAGE) == BAD_STATE);
    assert(!unmap(d + 2 * PAGE, a + 8 * PAGE, PAGE));
    assert(*(volatile uint8_t *)(a + 8 * PAGE) == 0x8b);
    expect_query(src, a, 16 * PAGE, 5, 0, 3);
    assert(!call(2, src, PAGE, 3) && !call(3, src, PAGE, 0));
    assert(!pages(0, a, 16 * PAGE) && !g_nmap);
}

static void wrong_mapping_pair(void) {
    uintptr_t src_a = alias_base, src_b = alias_base + 2 * PAGE;
    uintptr_t dst_a = stack_base + 0x100000, dst_b = stack_base + 0x104000;
    assert(!pages(1, alias_base, 4 * PAGE));
    memset((void *)src_a, 0x14, 2 * PAGE);
    memset((void *)src_b, 0x28, 2 * PAGE);
    assert(!map(dst_a, src_a, 2 * PAGE));
    assert(!map(dst_b, src_b, 2 * PAGE));
    memset((void *)dst_a, 0x33, 2 * PAGE);
    memset((void *)dst_b, 0x66, 2 * PAGE);
    unsigned prepares = atomic_load(&write_prepares), protects = atomic_load(&protect_calls);
    uint64_t used = info(I_UsedMem);
    // Both ranges have valid states and exact live ownership, but they do not
    // name the same backing. Upstream returns InvalidCurrentMemory for both.
    assert(unmap(dst_a, src_b, 2 * PAGE) == BAD_STATE);
    assert(unmap(dst_b, src_a, 2 * PAGE) == BAD_STATE);
    // Interior/partial requests still fail the exact live-mapping state check.
    assert(unmap(dst_a + PAGE, src_b + PAGE, PAGE) == BAD_STATE);
    assert(unmap(dst_a, src_b, PAGE) == BAD_STATE);
    assert(unmap(dst_a, src_b + PAGE, 2 * PAGE) == BAD_STATE);
    assert(g_nmap == 2 && info(I_UsedMem) == used);
    assert(atomic_load(&write_prepares) == prepares && atomic_load(&protect_calls) == protects);
    expect_query(src_a, src_a, 2 * PAGE, 5, 1, 0);
    expect_query(src_b, src_b, 2 * PAGE, 5, 1, 0);
    expect_query(dst_a, dst_a, 2 * PAGE, 0xb, 0, 3);
    expect_query(dst_b, dst_b, 2 * PAGE, 0xb, 0, 3);
    for (size_t i = 0; i < 2 * PAGE; ++i) {
        assert(((uint8_t *)src_a)[i] == 0x14 && ((uint8_t *)src_b)[i] == 0x28);
        assert(((uint8_t *)dst_a)[i] == 0x33 && ((uint8_t *)dst_b)[i] == 0x66);
    }
    assert(!unmap(dst_a, src_a, 2 * PAGE));
    expect_query(src_b, src_b, 2 * PAGE, 5, 1, 0);
    assert(!unmap(dst_b, src_b, 2 * PAGE));
    for (size_t i = 0; i < 2 * PAGE; ++i) {
        assert(((uint8_t *)src_a)[i] == 0x33 && ((uint8_t *)src_b)[i] == 0x66);
    }
    expect_query(src_a, src_a, 4 * PAGE, 5, 0, 3);
    assert(!g_nmap && !pages(0, alias_base, 4 * PAGE));
}

static void heap_lifecycle_and_rollback(void) {
    uintptr_t h = heap_base, d = stack_base + 0x20000, src = h + PAGE;
    assert(!heap(0x400000));
    memset((void *)src, 0x53, PAGE);
    assert(!map(d, src, PAGE));
    assert(!unmap(d, src, PAGE));
    memset((void *)d, 0x6a, PAGE); // remains host-accessible, logically free
    unsigned prepares = atomic_load(&write_prepares), protects = atomic_load(&protect_calls);
    EXPECT_MEMORY_PROTECTION_FAILURE(map(d, src, PAGE), (void *)src, PAGE);
    assert(atomic_load(&write_prepares) == prepares && atomic_load(&protect_calls) == protects);
    assert(*(volatile uint8_t *)d == 0x6a && *(volatile uint8_t *)src == 0x53);
    expect_query(src, h, 0x400000, 5, 0, 3);
    expect_state(d, 0, 0, 0);
    assert(!g_nmap);
    assert(!map(d, src, PAGE));
    expect_query(h, h, PAGE, 5, 0, 3);
    expect_query(src, src, PAGE, 5, 1, 0);
    expect_query(src + PAGE, src + PAGE, 0x400000 - 2 * PAGE, 5, 0, 3);
    assert(!heap(0x200000)); // shrink after the owned source is safe
    assert(!heap(0x600000)); // growth leaves ownership intact
    assert(heap(0) == BAD_STATE && atomic_load(&g_heap_cur) == 0x600000);
    expect_query(src, src, PAGE, 5, 1, 0);
    expect_heap_failure(0x400000, (const void *)src, PAGE);
    assert(atomic_load(&g_heap_cur) == 0x600000);
    expect_query(src, src, PAGE, 5, 1, 0);
    *(volatile uint8_t *)d = 0x90;
    assert(!unmap(d, src, PAGE) && *(volatile uint8_t *)src == 0x90);
    assert(!heap(0));
}

static void module_sources(void) {
    size_t n = 0x30000;
    uint8_t *raw = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(raw != MAP_FAILED);
    // Real module images and their protection changes are 16 KiB aligned even
    // when this test runs on a host whose own pages are only 4 KiB.
    uint8_t *b = (uint8_t *)(((uintptr_t)raw + 0x7fff) & ~(uintptr_t)0x3fff);
    uintptr_t d = stack_base + 0x30000;
    for (int negative = 0; negative <= 1; ++negative) {
        g_mod[0] = (module_t){ .base = b, .size = 0x18000, .text_end = 0x2000,
            .ro_off = 0x2000, .data_off = 0x4000, .patch_off = negative ? -0x4000 : 0x16000,
            .patch_len = negative ? 0x4000 : 0x2000, .file_image_len = 0x10000, .loaded = 1,
            .key_page_off = 0x8000, .key_page_size = 0x4000 };
        g_nmod = 1;
        uintptr_t src = (uintptr_t)b + 0x5000, end = (uintptr_t)b + (negative ? 0x18000 : 0x14000);
        memset(b - 0x4000, 0x61, 0x1c000); // synthetic bytes, never real keys
        assert(!mprotect(b, 0x4000, PROT_READ));
        assert(!mprotect(b + 0x8000, 0x4000, PROT_READ));
        if (negative) assert(!mprotect(b - 0x4000, 0x4000, PROT_READ));
        else assert(!mprotect(b + 0x14000, 0x4000, PROT_READ));
        assert(map(d, (uintptr_t)b, PAGE) == BAD_STATE);
        assert(map(d, (uintptr_t)b + 0x2000, PAGE) == BAD_STATE);
        assert(map(d, (uintptr_t)b + 0x3000, 2 * PAGE) == BAD_STATE);
        assert(map(d, end - PAGE, 2 * PAGE) == BAD_STATE);
        assert(map(d, (uintptr_t)b + g_mod[0].patch_off, PAGE) == BAD_STATE);
        // A real loader key page is read-only inside otherwise writable data.
        assert(map(d, (uintptr_t)b + 0x8000, PAGE) == BAD_STATE);
        assert(map(d, (uintptr_t)b + 0x7000, 2 * PAGE) == BAD_STATE);
        assert(map(d, (uintptr_t)b + 0xb000, 2 * PAGE) == BAD_STATE);
        expect_query((uintptr_t)b + 0x9000, (uintptr_t)b + 0x8000, 0x4000, 4, 0, 1);
        assert(!map(d, src, PAGE));
        expect_query(src, src, PAGE, 4, 1, 0);
        expect_query(src - PAGE, (uintptr_t)b + 0x4000, PAGE, 4, 0, 3);
        expect_query(src + PAGE, src + PAGE, 2 * PAGE, 4, 0, 3);
        assert(map(d + 0x4000, src, PAGE) == BAD_STATE);
        *(volatile uint8_t *)d = 0x82;
        assert(!unmap(d, src, PAGE) && *(volatile uint8_t *)src == 0x82);
        expect_query(src, (uintptr_t)b + 0x4000, 0x4000, 4, 0, 3);
        // The writable BSS after the key page remains eligible.
        uintptr_t bss = (uintptr_t)b + 0xd000;
        assert(!map(d, bss, PAGE));
        expect_query(bss, bss, PAGE, 4, 1, 0);
        expect_query(bss + PAGE, bss + PAGE, end - bss - PAGE, 4, 0, 3);
        assert(!unmap(d, bss, PAGE));
        // Logical data can start in the loader's rounded-up text host page.
        // It must not be copied back to that physically read-only tail.
        g_mod[0].text_end = 0x2800; g_mod[0].ro_off = g_mod[0].data_off = 0x3000;
        assert(map(d, (uintptr_t)b + 0x3000, PAGE) == BAD_STATE);
        assert(map(d, (uintptr_t)b + 0x3000, 2 * PAGE) == BAD_STATE);
        expect_state((uintptr_t)b + 0x3000, 4, 0, 1);
        assert(!map(d, (uintptr_t)b + 0x4000, PAGE));
        assert(!unmap(d, (uintptr_t)b + 0x4000, PAGE));
        if (!negative) {
            assert(map(d, (uintptr_t)b + 0x15000, PAGE) == BAD_STATE);
            expect_state((uintptr_t)b + 0x15000, 4, 0, 1);
        }
        // Missing ro/data offsets use text_end, but the host text tail still
        // cannot be borrowed even though ordinary data after it can be.
        g_mod[0].text_end = 0x2000; g_mod[0].ro_off = g_mod[0].data_off = 0;
        assert(map(d, (uintptr_t)b + 0x2000, PAGE) == BAD_STATE);
        assert(!map(d, (uintptr_t)b + 0x4000, PAGE));
        expect_query((uintptr_t)b + 0x4000, (uintptr_t)b + 0x4000, PAGE, 4, 1, 0);
        assert(!unmap(d, (uintptr_t)b + 0x4000, PAGE));
        // Dynamic NRO reload restores bytes outside the memory ownership
        // locks. Until that lifetime is integrated these sources are rejected,
        // whether currently loaded or not, rather than promising a fake pin.
        g_mod[0].nro = 1;
        assert(map(d, src, PAGE) == BAD_STATE);
        expect_state(src, 4, 0, 3);
        g_mod[0].loaded = 0;
        assert(map(d, src, PAGE) == BAD_STATE);
        g_mod[0].nro = 0;
        assert(map(d, src, PAGE) == BAD_STATE);
        g_nmod = 0;
        assert(!mprotect(b - 0x4000, 0x1c000, PROT_READ | PROT_WRITE));
    }
    memset(g_mod, 0, sizeof g_mod);
    assert(!munmap(raw, n));
}

// pthread_barrier is absent on Apple. This reusable gate keeps both contestants
// at the same boundary without sleeping or timing-sensitive result assertions.
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; unsigned waiting, epoch, count; } gate;
static void gate_init(gate *g, unsigned count) {
    *g = (gate){ .count = count };
    assert(!pthread_mutex_init(&g->mu, NULL) && !pthread_cond_init(&g->cv, NULL));
}
static void gate_wait(gate *g) {
    assert(!pthread_mutex_lock(&g->mu));
    unsigned epoch = g->epoch;
    if (++g->waiting == g->count) { g->waiting = 0; ++g->epoch; assert(!pthread_cond_broadcast(&g->cv)); }
    else while (epoch == g->epoch) assert(!pthread_cond_wait(&g->cv, &g->mu));
    assert(!pthread_mutex_unlock(&g->mu));
}
static void gate_destroy(gate *g) { assert(!pthread_cond_destroy(&g->cv) && !pthread_mutex_destroy(&g->mu)); }
typedef struct { gate *start; unsigned op; uintptr_t d, s; size_t size; uint64_t result; } contestant;
static void *compete(void *argument) {
    contestant *c = argument;
    gate_wait(c->start);
    c->result = c->op == 4 ? map(c->d, c->s, c->size) :
                c->op == 1 ? heap(c->size) : pages(0, c->s, c->size);
    return NULL;
}
static void two_way_race(contestant *a, contestant *b) {
    pthread_t threads[2]; gate start; gate_init(&start, 2); a->start = b->start = &start;
    assert(!pthread_create(&threads[0], NULL, compete, a));
    assert(!pthread_create(&threads[1], NULL, compete, b));
    assert(!pthread_join(threads[0], NULL) && !pthread_join(threads[1], NULL));
    gate_destroy(&start);
    assert((a->result == 0 && b->result == BAD_STATE) || (a->result == BAD_STATE && b->result == 0));
}
static void ownership_races(void) {
    uintptr_t a = alias_base, d = stack_base + 0x40000;
    for (unsigned i = 0; i < 40; ++i) {
        assert(!pages(1, a, 2 * PAGE));
        contestant first = { .op = 4, .d = d, .s = a, .size = PAGE };
        contestant second = { .op = 4, .d = d + 0x4000, .s = a, .size = PAGE };
        two_way_race(&first, &second);
        assert(!unmap(first.result ? second.d : first.d, a, PAGE));
        // Different sources competing for one destination must also have one
        // owner; a loser must not leave its source logically borrowed.
        second.d = d; second.s = a + PAGE;
        two_way_race(&first, &second);
        uintptr_t winner = first.result ? second.s : first.s;
        uintptr_t loser = first.result ? first.s : second.s;
        expect_state(winner, 5, 1, 0); expect_state(loser, 5, 0, 3);
        assert(!unmap(d, winner, PAGE));
        first.result = second.result = 0;
        second.op = 0x2d; second.s = a; second.size = PAGE;
        two_way_race(&first, &second);
        if (!first.result) { expect_state(a, 5, 1, 0); assert(!unmap(d, a, PAGE)); }
        else expect_state(a, 0, 0, 0);
        assert(!pages(0, a, 2 * PAGE));
        assert(!heap(0x400000));
        first.s = heap_base + 0x300000;
        second.op = 1; second.size = 0x200000;
        two_way_race(&first, &second);
        if (!first.result) { expect_state(first.s, 5, 1, 0); assert(!unmap(d, first.s, PAGE)); }
        else expect_state(first.s, 0, 0, 0);
        assert(!heap(0));
    }
}

typedef struct { uintptr_t src, dst; unsigned id; } worker_arg;
static _Atomic unsigned workers_done;
static void *lifecycle_worker(void *argument) {
    worker_arg *w = argument;
    for (unsigned i = 0; i < 150; ++i) {
        uint32_t value = (w->id << 16) | i;
        *(volatile uint32_t *)w->src = value;
        assert(!map(w->dst, w->src, PAGE));
        assert(*(volatile uint32_t *)w->dst == value);
        expect_query(w->src, w->src, PAGE, 5, 1, 0);
        assert(pages(0, w->src, PAGE) == BAD_STATE);
        *(volatile uint32_t *)w->dst = ~value;
        assert(!unmap(w->dst, w->src, PAGE));
        assert(*(volatile uint32_t *)w->src == ~value);
        expect_state(w->src, 5, 0, 3);
    }
    atomic_fetch_add(&workers_done, 1);
    return NULL;
}
static void *query_worker(void *argument) {
    worker_arg *workers = argument;
    do {
        for (unsigned i = 0; i < 8; ++i) {
            memory_info mi = query(workers[i].src);
            assert(mi.state == 5 && ((mi.attr == 1 && mi.perm == 0) || (!mi.attr && mi.perm == 3)));
        }
    } while (atomic_load(&workers_done) < 8);
    return NULL;
}
static void concurrent_lifecycles(void) {
    worker_arg workers[8]; pthread_t threads[8], observer;
    assert(!pages(1, alias_base, 8 * PAGE));
    for (unsigned i = 0; i < 8; ++i) workers[i] = (worker_arg){ alias_base + i * PAGE, stack_base + 0x80000 + i * PAGE, i };
    atomic_store(&workers_done, 0);
    assert(!pthread_create(&observer, NULL, query_worker, workers));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&threads[i], NULL, lifecycle_worker, &workers[i]));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_join(observer, NULL));
    assert(!g_nmap);
    expect_query(alias_base, alias_base, 8 * PAGE, 5, 0, 3);
    assert(!pages(0, alias_base, 8 * PAGE));
}

static void metadata_exhaustion(void) {
    size_t count = MAX_MAP, size = (count + 1) * PAGE;
    assert(size < DM_GAME_MEM_STACK && size < DM_GAME_MEM_ALIAS);
    assert(!pages(1, alias_base, size));
    for (size_t i = 0; i <= count; ++i) *(uint32_t *)(alias_base + i * PAGE) = (uint32_t)i;
    for (size_t i = 0; i < count; ++i) {
        assert(!map(stack_base + i * PAGE, alias_base + i * PAGE, PAGE));
        assert(*(uint32_t *)(stack_base + i * PAGE) == i);
    }
    uintptr_t extra_src = alias_base + count * PAGE, extra_dst = stack_base + count * PAGE;
    uint64_t used = info(I_UsedMem);
    unsigned prepares = atomic_load(&write_prepares), protects = atomic_load(&protect_calls);
    assert(map(extra_dst, extra_src, PAGE) == NO_RESOURCE);
    assert(atomic_load(&write_prepares) == prepares && atomic_load(&protect_calls) == protects);
    assert(info(I_UsedMem) == used && g_nmap == MAX_MAP);
    expect_query(extra_src, extra_src, PAGE, 5, 0, 3);
    expect_state(extra_dst, 0, 0, 0);
    // Releasing one record makes that metadata immediately reusable; all
    // unrelated records survive the last-entry swap in the bounded registry.
    size_t released = count / 2;
    assert(!unmap(stack_base + released * PAGE, alias_base + released * PAGE, PAGE));
    assert(!map(extra_dst, extra_src, PAGE) && g_nmap == MAX_MAP);
    expect_query(alias_base + released * PAGE, alias_base + released * PAGE, PAGE, 5, 0, 3);
    expect_query(extra_src, extra_src, PAGE, 5, 1, 0);
    for (size_t i = 0; i < count; ++i) if (i != released) {
        assert(!unmap(stack_base + i * PAGE, alias_base + i * PAGE, PAGE));
        assert(*(uint32_t *)(alias_base + i * PAGE) == i);
    }
    assert(!unmap(extra_dst, extra_src, PAGE));
    assert(!g_nmap && !pages(0, alias_base, size));
}

int main(void) {
    alias_base = info(I_AliasAddr); stack_base = info(I_StackAddr); heap_base = info(I_HeapAddr);
    assert(!((uintptr_t)g_as & (host_page() - 1)));
    invalid_inputs();
    alias_lifecycle();
    wrong_mapping_pair();
    heap_lifecycle_and_rollback();
    module_sources();
    ownership_races();
    concurrent_lifecycles();
    metadata_exhaustion();
    assert(!g_nmap && !alias_mapped() && !atomic_load(&g_heap_cur));
    assert(info(I_UsedMem) == DM_GAME_MEM_USED);
    free(g_alias_bits); free(g_alias_exposed);
    assert(!munmap(g_as, RESERVE));
    printf("production MapMemory ownership tests passed (mem_map_limit=%d, host page=%zu)\n", DM_GAME_MEM_MAP_LIMIT, host_page());
    return 0;
}
