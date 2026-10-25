// Runtime factory -> actual adapter -> native owner -> provider -> real SVC.
// Only constructor/forwarding guest code and unrelated platform services are
// synthetic. No SDK code, private offsets or proprietary inputs are used.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../dm.h"
#include "../hle_standard_allocator_provider.h"
#include "../hle_vamm_adapter.h"
#include "../hle_vamm_native.h"
#include "../hle_memory_runtime.h"
#include <setjmp.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

#ifndef __APPLE__
static void *pthread_get_stackaddr_np(pthread_t thread) { (void)thread; return NULL; }
static size_t pthread_get_stacksize_np(pthread_t thread) { (void)thread; return 0; }
#endif
static size_t host_page(void) {
#ifdef DM_TEST_HOST_PAGE_16K
    assert(getpagesize() <= 0x4000); return 0x4000;
#else
    return (size_t)getpagesize();
#endif
}
static int test_getpagesize(void) { return (int)host_page(); }
static void *test_mmap(void *hint, size_t size, int prot, int flags, int fd, off_t off) {
    assert(!hint && fd == -1 && !off);
    size_t pg = host_page();
    void *raw = mmap(NULL, size + pg, prot, flags, fd, off);
    if (raw == MAP_FAILED) return raw;
    uintptr_t lo = (uintptr_t)raw, a = (lo + pg - 1) & ~(uintptr_t)(pg - 1);
    if (lo != a) assert(!munmap(raw, a - lo));
    assert(!munmap((void *)(a + size), lo + pg - a)); return (void *)a;
}
static _Atomic int fail_protect;
static _Atomic unsigned protect_calls;
static int test_mprotect(void *address, size_t size, int permission) {
    atomic_fetch_add(&protect_calls, 1);
    assert(!((uintptr_t)address & (host_page() - 1)) && !(size & (host_page() - 1)));
    if (atomic_exchange(&fail_protect, 0)) { errno = EACCES; return -1; }
    return mprotect(address, size, permission);
}
#ifdef DM_TEST_SYS_RESOURCE
#undef DM_GAME_SYS_RESOURCE
#define DM_GAME_SYS_RESOURCE DM_TEST_SYS_RESOURCE
#endif
#define getpagesize test_getpagesize
#define mmap test_mmap
#define mprotect test_mprotect
#include "../hle_svc.c"
#undef mprotect
#undef mmap
#undef getpagesize

module_t g_mod[MAX_MOD];
int g_nmod;
struct dm_flags g_flag;
void dm_guest_write_prepare(void *p, size_t size) { (void)p; (void)size; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void hlecount_svc(int num) { (void)num; }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void guest_clock_read_wait(void) {}
uint64_t guest_ns(void) { return 0; }
uint64_t dm_entropy64(void) { return 0; }
int thr_limit_used(void) { return 0; }

#include "memory_failure_test.h"

#if defined(DM_SA_ADAPTER_TESTING) && defined(DM_VAMM_ADAPTER_TESTING)
static uint64_t call(unsigned svc, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t regs[31] = {a, b, c}; svc_dispatch(svc, regs, 0); return regs[0];
}
static uint64_t info(unsigned id) {
    uint64_t regs[31] = {0, id}; svc_dispatch(0x29, regs, 0); assert(!regs[0]); return regs[1];
}
static void query_state(uintptr_t address, unsigned state, unsigned attr, unsigned permission) {
    svc_memory_info mi;
    assert(!call(6, (uintptr_t)&mi, 0, address));
    assert(mi.state == state && mi.attr == attr && mi.perm == permission);
}

#endif
enum { PAGE = 0x1000, SMALL = 0x10000, LARGE = 0x200000 };
static _Atomic unsigned constructors, forwards;
static void impossible_original(void) { abort(); }
static void bootstrap(void *self) { assert(self); atomic_fetch_add(&constructors, 1); }
static void construct_allocator(void *self) {
    memset(self, 0, DM_SA_ADAPTER_OBJECT_SIZE); atomic_fetch_add(&constructors, 1);
}
void *dm_sa_host_forward_allocate(void *self, size_t size) {
    (void)self; (void)size; atomic_fetch_add(&forwards, 1); return NULL;
}
#define V(n, type) ((type)dm_vamm_adapter_lookup(dm_vamm_adapter_symbol_name(n)))
#define S(n, type) ((type)dm_sa_adapter_lookup(dm_sa_adapter_symbol_name(n)))
#define CONSTRUCT(s) S(0, void (*)(void *))(s)
#define INIT(s) S(6, void (*)(void *,void *,size_t))(s,NULL,(size_t)62<<30)
#define FINISH(s) S(8, void (*)(void *))(s)
#define ALLOC(s,n) S(9, void *(*)(void *,size_t))(s,n)
#define FREE(s,p) S(11, void (*)(void *,void *))(s,p)
#define TOTAL(s) S(15, size_t (*)(const void *))(s)
static _Thread_local jmp_buf *escape;
static _Thread_local dm_sa_result fatal_result;
static void fatal(void *context, const char *operation, dm_sa_result result) {
    assert(context && operation);
    if (!escape) abort();
    fatal_result = result; longjmp(*escape, 1);
}
#if defined(DM_SA_ADAPTER_TESTING) && defined(DM_VAMM_ADAPTER_TESTING)
static dm_sa_result (*real_factory)(void *,void *,size_t,unsigned,dm_sa_config *);
static int fail_core_create;
static dm_sa_result factory(void *context, void *self, size_t n, unsigned cache, dm_sa_config *out) {
    dm_sa_result result = real_factory(context, self, n, cache, out);
    if (result == DM_SA_OK && fail_core_create) { fail_core_create = 0; out->provider.granule = 3; }
    return result;
}
#endif
static void census(dm_vamm_adapter_config *v, dm_sa_adapter_config *a,
                   dm_vamm_symbol *vs, dm_sa_adapter_symbol *as) {
    for (size_t i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i)
        vs[i] = (dm_vamm_symbol){dm_vamm_adapter_symbol_name(i), impossible_original};
    vs[8].function = vs[9].function = (dm_vamm_guest_fn)bootstrap;
    v->proofs = DM_VAMM_PROOF_ALL; v->symbols = vs; v->symbol_count = DM_VAMM_SYMBOL_COUNT;
    for (size_t i = 0; i < DM_SA_ADAPTER_SYMBOL_COUNT; ++i)
        as[i] = (dm_sa_adapter_symbol){dm_sa_adapter_symbol_name(i), impossible_original};
    as[0].function = as[1].function = (dm_sa_guest_fn)construct_allocator;
    a->proofs = DM_SA_PROOF_ALL; a->symbols = as; a->symbol_count = DM_SA_ADAPTER_SYMBOL_COUNT;
    a->fatal = fatal;
}
#if defined(DM_SA_ADAPTER_TESTING) && defined(DM_VAMM_ADAPTER_TESTING)
static void expect_finalize(void *self, dm_sa_result expected) {
    jmp_buf jump; escape = &jump;
    if (!setjmp(jump)) { FINISH(self); abort(); }
    escape = NULL; assert(fatal_result == expected);
}
static void expect_initialize(void *self, dm_sa_result expected) {
    jmp_buf jump; escape = &jump;
    if (!setjmp(jump)) { INIT(self); abort(); }
    escape = NULL; assert(fatal_result == expected);
}
static void *worker(void *unused) {
    (void)unused;
    unsigned char self[DM_SA_ADAPTER_OBJECT_SIZE] = {0};
    CONSTRUCT(self); INIT(self);
    for (unsigned i = 0; i < 100; ++i) {
        void *pointer = ALLOC(self, 64 + i); assert(pointer);
        memset(pointer, (int)i, 64 + i); FREE(self, pointer);
    }
    FINISH(self); return NULL;
}
#endif
static void no_config(dm_sa_adapter_config *a) {
    unsigned char self[DM_SA_ADAPTER_OBJECT_SIZE] = {0};
    dm_sa_config c, before; memset(&c, 0xa5, sizeof c); before = c;
    assert(a->make_config(a->context,self,(size_t)62<<30,0,&c) == DM_SA_INVALID);
    assert(!memcmp(&c,&before,sizeof c));
}
int main(void) {
    dm_memory_runtime *runtime = (void *)1;
    assert(dm_memory_runtime_create(&runtime,0) == DM_SA_INVALID && runtime == (void *)1);
    assert(dm_memory_runtime_create(NULL,32) == DM_SA_INVALID);
    assert(dm_memory_runtime_create(&runtime,SIZE_MAX) == DM_SA_INVALID && runtime == (void *)1);
    assert(dm_memory_runtime_create(&runtime,128) == DM_SA_OK);
    dm_vamm_adapter_config v;
    dm_sa_adapter_config a;
    assert(dm_memory_runtime_configs(runtime,&v,&a) == DM_SA_OK);
    assert(v.shared_owner && v.context == runtime && a.context == runtime);
    assert(!v.proofs && !a.proofs && !alias_mapped());
    assert(a.allocation_alignment == dm_sa_host_allocation_alignment);
    assert(a.forward_allocate == dm_sa_host_forward_allocate);
    assert(a.allocation_alignment(NULL,1) == 16);
    assert(a.allocation_alignment(NULL,SMALL) == (DM_GAME_ALLOC_BIG_ALIGN ? SMALL : 16));
    no_config(&a); // neither table retained, no initialization side effects
    dm_vamm_symbol vs[DM_VAMM_SYMBOL_COUNT];
    dm_sa_adapter_symbol as[DM_SA_ADAPTER_SYMBOL_COUNT];
    census(&v,&a,vs,as);
#if !defined(DM_SA_ADAPTER_TESTING) || !defined(DM_VAMM_ADAPTER_TESTING)
    assert(dm_vamm_adapter_install(&v) == DM_VAMM_INVALID);
    assert(dm_sa_adapter_install(&a) == DM_SA_INVALID);
    assert(!dm_vamm_adapter_lookup(vs[0].name) && !dm_sa_adapter_lookup(as[0].name));
    assert(!alias_mapped() && !atomic_load(&constructors));
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_OK);
    puts("memory runtime production activation remains disabled: PASS");
    return 0;
#else
    // Pending preparation retains both owners; cancellation releases everything
    // without guest calls, initialization, page mapping or global publication.
    dm_vamm_adapter_prepared *vp;
    dm_sa_adapter_prepared *ap;
    assert(!dm_vamm_adapter_prepare(&v,&vp));
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_BUSY);
    assert(dm_sa_adapter_prepare(&a,&ap) == DM_SA_OK);
    no_config(&a); // even both preparations do not initialize the owner
    assert(!atomic_load(&constructors) && !alias_mapped());
    dm_sa_adapter_cancel(ap); dm_vamm_adapter_cancel(vp);
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_OK);

    // An already-initialized owner cannot be presented as a fresh takeover.
    assert(dm_memory_runtime_create(&runtime,128) == DM_SA_OK);
    assert(dm_memory_runtime_configs(runtime,&v,&a) == DM_SA_OK);
    if (info(16)) {
        dm_vamm_initialize(v.shared_owner);
        census(&v,&a,vs,as);
        assert(dm_vamm_adapter_prepare(&v,&vp) == DM_VAMM_INVALID);
    }
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_OK);

    assert(dm_memory_runtime_create(&runtime,128) == DM_SA_OK);
    assert(dm_memory_runtime_configs(runtime,&v,&a) == DM_SA_OK);
    census(&v,&a,vs,as);
    real_factory = a.make_config; a.make_config = factory;
    assert(!dm_vamm_adapter_install(&v));
    assert(dm_sa_adapter_install(&a) == DM_SA_OK);
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_BUSY);
    no_config(&a);
    unsigned char manager[64] = {0};
    V(8,void (*)(void *))(manager);
    no_config(&a); // constructor alone must not enable reservations
    unsigned calls = atomic_load(&protect_calls);
    V(0,void (*)(void))();
    assert(atomic_load(&protect_calls) == calls && !alias_mapped());
    assert(V(7,int (*)(void))() == !!info(16));
    unsigned char self[DM_SA_ADAPTER_OBJECT_SIZE] = {0};
    CONSTRUCT(self);
    if (!info(16)) {
        no_config(&a); expect_initialize(self,DM_SA_INVALID);
        assert(!dm_sa_adapter_is_managed(self));
        assert(dm_sa_adapter_shutdown() == DM_SA_OK);
        assert(dm_memory_runtime_destroy(runtime) == DM_SA_BUSY);
        assert(!dm_vamm_adapter_shutdown());
        assert(dm_memory_runtime_destroy(runtime) == DM_SA_OK);
        puts("memory runtime disabled-system-resource path: PASS"); return 0;
    }
    // A successful factory followed by failed core creation must release its
    // exact self registration. Retrying same self proves no stale client leaks.
    fail_core_create = 1; expect_initialize(self,DM_SA_INVALID);
    assert(!self[0] && !dm_sa_adapter_is_managed(self));
    INIT(self);
    assert(!alias_mapped() && self[0]); // 62 GiB is a cap, not committed memory
    assert(TOTAL(self) <= APP_POOL - DM_GAME_MEM_USED && TOTAL(self) < ((size_t)62<<30));
    assert(dm_sa_adapter_shutdown() == DM_SA_BUSY); // zero-backing core still owns context
    assert(!ALLOC(self,0));
    void *pointer = ALLOC(self,SMALL); assert(pointer && !((uintptr_t)pointer % SMALL));
    memset(pointer,0x71,SMALL);
    uintptr_t direct;
    assert(!V(1,uint32_t (*)(uintptr_t *,size_t))(&direct,SMALL));
    assert((uintptr_t)pointer + SMALL <= direct || direct + SMALL <= (uintptr_t)pointer);
    assert(!V(4,uint32_t (*)(uintptr_t))(direct));

    uintptr_t stack = info(I_StackAddr);
    assert(!call(4,stack,(uintptr_t)pointer,PAGE));
    *(unsigned char *)stack = 0x36;
    expect_finalize(self,DM_SA_BUSY);
    assert(self[0] && dm_memory_runtime_destroy(runtime) == DM_SA_BUSY);
    assert(dm_sa_adapter_shutdown() == DM_SA_BUSY && dm_vamm_adapter_shutdown() == DM_VAMM_INVALID);
    query_state((uintptr_t)pointer,5,1,0);
    assert(!call(5,stack,(uintptr_t)pointer,PAGE));
    assert(*(unsigned char *)pointer == 0x36);
    FREE(self,pointer); assert(!alias_mapped());

    pointer = ALLOC(self,SMALL); assert(pointer); memset(pointer,0x62,SMALL);
    EXPECT_MEMORY_PROTECTION_FAILURE((FINISH(self), 0), pointer, SMALL);
    assert(self[0] && alias_mapped() == SMALL && *(unsigned char *)pointer == 0x62);
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_BUSY);
    FINISH(self); assert(!self[0] && !alias_mapped());
    INIT(self);
    assert(!call(1,0,APP_POOL-DM_GAME_MEM_USED-LARGE,0));
    assert(TOTAL(self) <= LARGE);
    assert(!ALLOC(self,3*LARGE/2) && !alias_mapped()); // strict even unlimited legacy policy
    pointer = ALLOC(self,LARGE/2); assert(pointer && alias_mapped() == LARGE/2);
    assert(TOTAL(self) <= LARGE/2);
    FREE(self,pointer); FINISH(self);
    assert(!call(1,0,0,0));

    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i],NULL,worker,NULL));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i],NULL));
    assert(!alias_mapped() && !g_nmap && !atomic_load(&forwards));
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_BUSY);
    assert(dm_sa_adapter_shutdown() == DM_SA_OK);
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_BUSY);
    assert(!dm_vamm_adapter_shutdown());
    assert(dm_memory_runtime_destroy(runtime) == DM_SA_OK);
    free(g_alias_bits); free(g_alias_exposed);
    assert(!munmap(g_as,RESERVE));
    puts("memory runtime native owner/provider/adapter integration: PASS");
    return 0;
#endif
}
