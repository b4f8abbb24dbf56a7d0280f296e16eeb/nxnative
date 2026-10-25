// Synthetic native SVC/HIPC adapter chain using the production memory owner.
// No guest code, TLS construction/retirement, or private input is executed.
#define _GNU_SOURCE
#include "../dm.h"
#include "../hle.h"
#include "../hle_ipc.h"
#include "ipc_wire_fixture.h"
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

static uintptr_t caller_stack;
static size_t caller_stack_size;
static void *test_stack_top(pthread_t t) { (void)t; return (void *)(caller_stack + caller_stack_size); }
static size_t test_stack_size(pthread_t t) { (void)t; return caller_stack_size; }
static int test_pagesize(void) {
#ifdef TEST_PAGE_16K
    assert(getpagesize() <= 0x4000); return 0x4000;
#else
    return getpagesize();
#endif
}
#define pthread_get_stackaddr_np test_stack_top
#define pthread_get_stacksize_np test_stack_size
#define getpagesize test_pagesize
#include "../hle_svc.c"
#undef getpagesize
#undef pthread_get_stacksize_np
#undef pthread_get_stackaddr_np
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_sf.c"

module_t g_mod[MAX_MOD]; int g_nmod;
struct dm_flags g_flag;
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
static unsigned entropy_calls, prepared, other_closes, broad_ue_queries;
static uint8_t next_byte;
void hlecount_svc(int n) { (void)n; }
int svc_thread(int n, uint64_t *r) {
    if (n != 0x16) return 0;
    ++other_closes; r[0] = 0xbad; return 1;
}
int dm_ue_heap_owns(uint64_t a, uint64_t n) { (void)a; (void)n; ++broad_ue_queries; return 1; }
void dm_backtrace(uint64_t p) { (void)p; }
void guest_clock_read_wait(void) {}
uint64_t guest_ns(void) { return 0; }
uint64_t dm_entropy64(void) { return 0; }
int thr_limit_used(void) { return 0; }
module_t *module_of(const void *p) {
    uintptr_t a = (uintptr_t)p;
    for (int i = 0; i < g_nmod; ++i)
        if (a >= (uintptr_t)g_mod[i].base && a - (uintptr_t)g_mod[i].base < g_mod[i].size) return &g_mod[i];
    return NULL;
}
void dm_guest_write_prepare(void *p, size_t n) {
    (void)p; assert(n); ++prepared;
    // Every IPC write is made while the memory owner's lifetime lock is held.
    int r = pthread_mutex_trylock(&g_map_lock);
    assert(r == EBUSY);
}
void dm_entropy_bytes(void *p, size_t n) {
    assert(n); ++entropy_calls;
    for (size_t i = 0; i < n; ++i) ((uint8_t *)p)[i] = next_byte++;
}
static void svc(unsigned n, uint64_t *r) { (void)svc_dispatch((int)n, r, 0); }
static uint64_t svc3(unsigned n, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t r[31] = {a,b,c}; svc(n,r); return r[0];
}
static ipc_test_reply native_request(uint32_t handle, uint8_t *tlr, ipc_test_packet q) {
    memcpy(tlr, q.bytes, sizeof q.bytes);
    assert(!svc3(0x21, handle, 0, 0)); return ipc_test_decode(tlr, DM_IPC_TLS_CAPACITY);
}
static void random_request(uint32_t handle, uint8_t *tlr, void *out, size_t n, uint32_t expected) {
    ipc_test_packet q = ipc_test_random((uintptr_t)out, n);
    memcpy(tlr, q.bytes, sizeof q.bytes);
    uint8_t old[DM_IPC_TLS_CAPACITY]; memcpy(old, tlr, sizeof old);
    unsigned calls = entropy_calls, prep = prepared;
    assert(svc3(0x21, handle, 0, 0) == expected);
    if (expected) { assert(!memcmp(old, tlr, sizeof old)); assert(calls == entropy_calls && prep == prepared); }
    else assert(!ipc_test_decode(tlr, DM_IPC_TLS_CAPACITY).result);
}
int main(void) {
    assert(!pthread_key_create(&g_keytlr, NULL));
    uint8_t *tlr = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(tlr != MAP_FAILED && !pthread_setspecific(g_keytlr, tlr));
    mem_init();
    assert(!svc3(1, 0, 4 * 1024 * 1024, 0));
    strcpy((char *)g_heap, "sm:");
    uint64_t r[31] = {UINT64_C(0xdeadbeef), (uintptr_t)g_heap};
    svc(0x1f, r); assert(!r[0] && dm_ipc_owns_handle((uint32_t)r[1]));
    uint32_t sm = (uint32_t)r[1];
    assert(!native_request(sm, tlr, ipc_test_pointer_size()).result);
    assert(!native_request(sm, tlr, ipc_test_register()).result);
    ipc_test_reply service = native_request(sm, tlr, ipc_test_service("csrng\0\0"));
    assert(!service.result && service.move_handle);
    uint32_t rng = service.move_handle;
    assert(!native_request(rng, tlr, ipc_test_pointer_size()).result);
    memset(g_heap + 0x1000, 0xaa, 0x2000);
    random_request(rng, tlr, g_heap + 0x1001, 4097, 0);
    assert(g_heap[0x1000] == 0xaa && g_heap[0x2002] == 0xaa && entropy_calls == 1);
    random_request(rng, tlr, NULL, 0, 0); assert(entropy_calls == 1);
    // Explicit-buffer native path has kernel alignment rules, unlike h_sf_Send.
    ipc_test_packet q = ipc_test_random((uintptr_t)(g_heap + 0x1001), 9);
    memcpy(g_heap + 0x4000, q.bytes, sizeof q.bytes);
    assert(svc3(0x22, (uintptr_t)g_heap + 0x4001, 4096, rng) == DM_IPC_INVALID_ADDRESS);
    assert(svc3(0x22, (uintptr_t)g_heap + 0x4000, 256, rng) == DM_IPC_INVALID_SIZE);
    assert(!svc3(0x22, (uintptr_t)g_heap + 0x4000, 4096, rng));
    assert(!ipc_test_decode(g_heap + 0x4000, 4096).result);
    memcpy(g_heap + 0x4001, q.bytes, sizeof q.bytes);
    assert(!h_sf_Send(rng, (uint32_t *)(g_heap + 0x4001), 256));
    uint32_t adapter_rng = 0; assert(!h_sf_Connect(&adapter_rng, "csrng"));
    random_request(adapter_rng, tlr, g_heap + 0x1001, 7, 0);
    uint16_t pointer_size = 99; assert(!h_sf_PtrBuf(&pointer_size, rng) && !pointer_size);
    uint32_t duplicate = 0; assert(!h_sf_Dup(&duplicate, rng));
    h_sf_Close(rng); random_request(duplicate, tlr, g_heap + 0x1001, 1, 0);
    assert(svc3(0x16, rng, 0, 0) == DM_IPC_INVALID_HANDLE && !other_closes);
    assert(svc3(0x16, 0x1234, 0, 0) == 0xbad && other_closes == 1);

    // No arbitrary host-RW admission, even when the old UE heuristic says yes.
    uint8_t *outside = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(outside != MAP_FAILED); memset(outside, 0x6a, 0x4000);
    random_request(duplicate, tlr, outside, 16, DM_IPC_INVALID_MEMORY);
    assert(!broad_ue_queries && outside[0] == 0x6a);
    caller_stack = (uintptr_t)outside; caller_stack_size = 0x4000;
    random_request(duplicate, tlr, outside + 16, 16, 0);
    random_request(duplicate, tlr, outside + 0x3ff0, 17, DM_IPC_INVALID_MEMORY);
    caller_stack = caller_stack_size = 0;

    // Real alias bitmap ownership, including a cross-page hole and borrowed
    // source; preparing a write never grants permission to such ranges.
    assert(!svc3(0x2c, (uintptr_t)g_alias, 0x1000, 0));
    random_request(duplicate, tlr, g_alias + 0xff0, 17, DM_IPC_INVALID_MEMORY);
    random_request(duplicate, tlr, g_alias + 16, 16, 0);
    assert(!svc3(4, (uintptr_t)g_stack, (uintptr_t)g_alias, 0x1000));
    random_request(duplicate, tlr, g_alias, 16, DM_IPC_INVALID_MEMORY);
    random_request(duplicate, tlr, g_stack, 16, 0);
    assert(!svc3(5, (uintptr_t)g_stack, (uintptr_t)g_alias, 0x1000));
    random_request(duplicate, tlr, g_stack, 16, DM_IPC_INVALID_MEMORY);
    // Permanent static modules admit only writable data. Text/ro, key seals,
    // patch seals, unloadable NROs and unloaded modules never become IPC output.
    uint8_t *module = mmap(NULL, 0x20000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(module != MAP_FAILED);
    g_mod[0] = (module_t){.base = module, .size = 0x20000, .loaded = 1,
        .text_end = 0x2000, .ro_off = 0x4000, .data_off = 0x8000,
        .key_page_off = 0xc000, .key_page_size = 0x4000, .patch_off = 0x18000};
    g_nmod = 1;
    strcpy((char *)module, "sm:");
    assert(!mprotect(module, 0x8000, PROT_READ));
    assert(!mprotect(module + 0xc000, 0x4000, PROT_READ));
    assert(!mprotect(module + 0x18000, 0x8000, PROT_READ));
    r[1] = (uintptr_t)module; svc(0x1f, r); assert(!r[0]);
    assert(!svc3(0x16, r[1], 0, 0)); // read-only module name is readable
    random_request(duplicate, tlr, module + 0x8000, 16, 0);
    random_request(duplicate, tlr, module + 0x10000, 16, 0);
    random_request(duplicate, tlr, module, 16, DM_IPC_INVALID_MEMORY);
    random_request(duplicate, tlr, module + 0x4000, 16, DM_IPC_INVALID_MEMORY);
    random_request(duplicate, tlr, module + 0xbff0, 17, DM_IPC_INVALID_MEMORY);
    random_request(duplicate, tlr, module + 0xc000, 16, DM_IPC_INVALID_MEMORY);
    random_request(duplicate, tlr, module + 0x17ff0, 17, DM_IPC_INVALID_MEMORY);
    random_request(duplicate, tlr, module + 0x18000, 16, DM_IPC_INVALID_MEMORY);
    random_request(duplicate, tlr, module + 0x1fff0, 17, DM_IPC_INVALID_MEMORY);
    g_mod[0].nro = 1;
    random_request(duplicate, tlr, module + 0x8000, 16, DM_IPC_INVALID_MEMORY);
    g_mod[0].nro = 0; g_mod[0].loaded = 0;
    random_request(duplicate, tlr, module + 0x8000, 16, DM_IPC_INVALID_MEMORY);
    g_mod[0].loaded = 1;
    q = ipc_test_random((uintptr_t)g_heap + 0x1001, 9);
    memcpy(module + 0x8000, q.bytes, sizeof q.bytes);
    assert(!svc3(0x22, (uintptr_t)module + 0x8000, 4096, duplicate));
    assert(svc3(0x22, (uintptr_t)module, 4096, duplicate) == DM_IPC_INVALID_MEMORY);
    assert(svc3(0x22, UINT64_MAX - 0xfff, 4096, duplicate) == DM_IPC_INVALID_MEMORY);
    r[1] = UINT64_MAX; svc(0x1f, r);
    assert(r[0] == DM_IPC_INVALID_MEMORY && r[1] == UINT64_MAX);
    g_nmod = 0; assert(!munmap(module, 0x20000));
    // The TLR is not a whole page of admitted storage.
    random_request(duplicate, tlr, tlr + 0x100, 1, DM_IPC_INVALID_MEMORY);
    random_request(duplicate, tlr, tlr + 16, 1, DM_IPC_INVALID_MEMORY);
    assert(!pthread_setspecific(g_keytlr, NULL));
    assert(svc3(0x21, duplicate, 0, 0) == DM_IPC_INVALID_MEMORY);
    assert(!pthread_setspecific(g_keytlr, tlr));
    q = ipc_test_close(); memcpy(tlr, q.bytes, sizeof q.bytes);
    assert(!svc3(0x21, duplicate, 0, 0));
    assert(svc3(0x21, duplicate, 0, 0) == DM_IPC_SESSION_CLOSED);
    assert(!svc3(0x16, duplicate, 0, 0));
    assert(!svc3(0x16, adapter_rng, 0, 0)); assert(!svc3(0x16, sm, 0, 0));
    assert(!pthread_setspecific(g_keytlr, NULL)); pthread_key_delete(g_keytlr);
    dm_ipc_destroy(g_ipc); free(g_alias_bits); free(g_alias_exposed);
    assert(!munmap(g_as, RESERVE) && !munmap(tlr, 0x4000) && !munmap(outside, 0x4000));
    puts("IPC synthetic native dispatch/adapters and memory-domain checks passed");
}
