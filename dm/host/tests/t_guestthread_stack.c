// Synthetic SDK/allocator callbacks around the production guest-thread helper
// and SVC dispatcher. No game binary, proprietary code or alternate MapMemory.
// Reuse the ownership harness and run its full suite after these lifecycles.
#define main map_memory_suite_main
#include "t_map_memory.c"
#undef main
#include <setjmp.h>

static _Thread_local jmp_buf failed;
static _Thread_local int expect_failure;
static int host_fail, guest_fail, create_fail, host_source, hide_symbols, self_join;
static const char *missing_symbol;
static int host_allocations, host_frees, guest_allocations, guest_frees;
static int creates, starts, waits, destroys, bodies, marked, mutex_lookups, mutex_operations;
static _Thread_local uint32_t current_handle = 1;
static void *raw_storage;
static size_t allocation_size;
static uintptr_t allocation_base;
static int free_phase;
static void *host_calloc(size_t n, size_t size) {
    if (host_fail) return NULL;
    void *p = calloc(n, size); assert(p); ++host_allocations; return p;
}
static void host_free(void *p) { assert(p); ++host_frees; free(p); }
static _Noreturn void guestthread_abort(void) { assert(expect_failure); longjmp(failed, 1); }
#define calloc host_calloc
#define free host_free
#define abort guestthread_abort
static module_t helper_sdk_module = { .loaded = 1 };
static module_t *helper_module_of(const void *p) { (void)p; return &helper_sdk_module; }
#define module_of helper_module_of
#include "../hle_guestthread.c"
#undef module_of
#undef abort
#undef free
#undef calloc

// The fake allocation has deliberately unaligned ends, both filled with
// sentinels: all mapped pages must lie inside it, not overlap a neighbor.
static void *guest_allocate(size_t size) {
    assert(current_handle);
    if (guest_fail || size >= 0x20000) return NULL;
    raw_storage = host_source ? malloc(size) : (void *)(allocation_base + 16);
    assert(raw_storage); allocation_size = size;
    memset(raw_storage, 0x5a, size); ++guest_allocations;
    return raw_storage;
}
static void guest_release(void *p) {
    assert(current_handle && p == raw_storage);
    assert(!g_nmap && destroys == free_phase);
    if (!host_source) {
        uintptr_t begin = (uintptr_t)p, aligned = (begin + 0xfff) & ~(uintptr_t)0xfff;
        size_t span = allocation_size - 0xfff;
        for (uintptr_t q = begin; q < aligned; ++q) assert(*(uint8_t *)q == 0x5a);
        for (uintptr_t q = aligned + span; q < begin + allocation_size; ++q) assert(*(uint8_t *)q == 0x5a);
        assert(dm_guest_map_source_available((void *)aligned, span));
    } else free(p);
    ++guest_frees; raw_storage = NULL;
}
typedef struct {
    pthread_t thread;
    void (*entry)(void *);
    void *arg, *source;
    size_t size;
    int joined;
} fake_thread;
static int guest_create(dm_guest_thread_type *tt, void (*entry)(void *), void *arg, void *source, size_t size, int prio) {
    assert(current_handle && !((uintptr_t)tt & 15) && size == 0x2000 && prio == 16);
    assert(dm_guest_map_source_available(source, size));
    assert((uintptr_t)source >= (uintptr_t)raw_storage);
    assert(size <= allocation_size && (uintptr_t)source - (uintptr_t)raw_storage <= allocation_size - size);
    ++creates;
    assert(!map(stack_base, (uintptr_t)source, size));
    assert(!dm_guest_map_source_available(source, size));
    expect_state((uintptr_t)source, 5, 1, 0);
    if (create_fail) { assert(!unmap(stack_base, (uintptr_t)source, size)); return 0x1203; }
    *(fake_thread *)tt = (fake_thread){ .entry = entry, .arg = arg, .source = source, .size = size };
    return 0;
}
static void *fake_run(void *p) {
    fake_thread *tt = (void *)p; current_handle = 2;
    tt->entry(tt->arg); return NULL;
}
static void guest_start(dm_guest_thread_type *p) {
    fake_thread *tt = (void *)p; ++starts;
    assert(!pthread_create(&tt->thread, NULL, fake_run, tt));
}
static void guest_wait(dm_guest_thread_type *p) {
    fake_thread *tt = (void *)p; ++waits;
    assert(!pthread_join(tt->thread, NULL));
    assert(!unmap(stack_base, (uintptr_t)tt->source, tt->size));
    tt->joined = 1;
    assert(!guest_frees && !host_frees); // wait may unmap; storage must survive destroy
}
static void guest_destroy(dm_guest_thread_type *p) {
    fake_thread *tt = (void *)p;
    assert(tt->joined && !guest_frees && !host_frees);
    ++destroys;
}
static void guest_mutex_initialize(dm_guest_mutex_type *p, dm_guest_bool recursive, int level) {
    (void)p; (void)recursive; (void)level; ++mutex_operations;
}
static void guest_mutex_operation(dm_guest_mutex_type *p) { (void)p; ++mutex_operations; }
static dm_guest_bool guest_mutex_locked(const dm_guest_mutex_type *p) { (void)p; ++mutex_operations; return 1; }
void *guest_sym(const char *name) {
    if (hide_symbols || (missing_symbol && !strcmp(name, missing_symbol))) return NULL;
    if (!strcmp(name, "malloc")) return guest_allocate;
    if (!strcmp(name, "free")) return guest_release;
    if (!strcmp(name, dm_guest_symbol_CreateThread)) return guest_create;
    if (!strcmp(name, dm_guest_symbol_StartThread)) return guest_start;
    if (!strcmp(name, dm_guest_symbol_WaitThread)) return guest_wait;
    if (!strcmp(name, dm_guest_symbol_DestroyThread)) return guest_destroy;
    if (!strcmp(name, dm_guest_symbol_InitializeMutex)) { ++mutex_lookups; return guest_mutex_initialize; }
    if (!strcmp(name, dm_guest_symbol_LockMutex)) { ++mutex_lookups; return guest_mutex_operation; }
    if (!strcmp(name, dm_guest_symbol_IsMutexLockedByCurrentThread)) { ++mutex_lookups; return guest_mutex_locked; }
    if (!strcmp(name, dm_guest_symbol_UnlockMutex)) { ++mutex_lookups; return guest_mutex_operation; }
    return NULL;
}
uint32_t thr_cur_handle(void) { return current_handle; }
void dm_thread_mark_host(void) { assert(current_handle == 2); marked = 1; }
static void body(void *p) {
    assert(p == &bodies && marked && current_handle == 2);
    if (self_join) {
        expect_failure = 1;
        if (!setjmp(failed)) { dm_guest_thread_join(current_owner); assert(!"self-join must fail"); }
        expect_failure = 0;
        assert(!destroys && !guest_frees && !host_frees);
    }
    ++bodies; memset((void *)stack_base, 0x6b, 0x2000);
}
static void reset(void) {
    assert(!raw_storage && host_allocations == host_frees && guest_allocations == guest_frees && !g_nmap);
    expect_failure = host_fail = guest_fail = create_fail = host_source = hide_symbols = self_join = 0;
    missing_symbol = NULL;
    thread_binding.attempted = 0; thread_binding.error = thread_binding.symbol = NULL;
    memset(&thread_binding.api, 0, sizeof thread_binding.api);
    memset(thread_binding.owners, 0, sizeof thread_binding.owners);
    mutex_binding.attempted = 0; mutex_binding.error = mutex_binding.symbol = NULL;
    memset(&mutex_binding.api, 0, sizeof mutex_binding.api); mutex_binding.sdk_owner = NULL;
    mutex_lookups = mutex_operations = 0;
    host_allocations = host_frees = guest_allocations = guest_frees = 0;
    creates = starts = waits = destroys = bodies = marked = free_phase = 0;
    current_handle = 1;
}
static void success(void) {
    free_phase = 1;
    void *handle = dm_guest_thread_spawn(body, &bodies, 16, 0x1001);
    assert(handle && !guest_frees && !host_frees);
    assert(!mutex_binding.attempted && !mutex_lookups && !mutex_operations);
    hide_symbols = 1; // teardown must retain the originally paired functions
    dm_guest_thread_join(handle);
    assert(creates == 1 && starts == 1 && waits == 1 && destroys == 1 && bodies == 1 && marked);
    assert(host_allocations == 1 && host_frees == 1 && guest_allocations == 1 && guest_frees == 1);
}
static void failure(size_t size) {
    expect_failure = 1;
    if (!setjmp(failed)) { dm_guest_thread_spawn(body, &bodies, 16, size); assert(!"expected fatal failure"); }
    assert(!starts && !waits && !destroys && !bodies && !marked && !g_nmap);
    assert(host_allocations == host_frees && guest_allocations == guest_frees);
}
static void optional_selftest_failure(void) {
    expect_failure = 1;
    if (!setjmp(failed)) { dm_guest_spawn_selftest(); assert(!"missing optional self-test import"); }
    assert(!host_allocations && !guest_allocations && !creates && !starts && !mutex_operations);
}
static void optional_selftest_failures(void) {
    const char *symbols[] = {
#define DM_GUEST_MUTEX_SIGNATURE(name, result, parameters) dm_guest_symbol_##name,
#include "../guest_mutex_signatures.inc"
#undef DM_GUEST_MUTEX_SIGNATURE
    };
    assert(!setenv("DM_GUEST_SPAWN_TEST", "1", 1));
    for (unsigned i = 0; i < sizeof symbols / sizeof *symbols; ++i) {
        reset(); missing_symbol = symbols[i]; optional_selftest_failure();
    }
    assert(!unsetenv("DM_GUEST_SPAWN_TEST"));
}
static void source_preflight(void) {
    assert(!dm_guest_map_source_available((void *)heap_base, 0));
    assert(!dm_guest_map_source_available((void *)(heap_base + 1), PAGE));
    assert(!dm_guest_map_source_available((void *)heap_base, PAGE + 1));
    assert(!dm_guest_map_source_available((void *)(UINTPTR_MAX - PAGE + 1), 2 * PAGE));
    assert(!dm_guest_map_source_available((void *)alias_base, PAGE));
    assert(!dm_guest_map_source_available((void *)stack_base, PAGE));
    assert(!dm_guest_map_source_available((void *)(heap_base + 0x200000), PAGE));
}
int main(void) {
    alias_base = info(I_AliasAddr); stack_base = info(I_StackAddr); heap_base = info(I_HeapAddr);
    assert(!heap(0x200000)); allocation_base = heap_base;
    source_preflight();
    for (int i = 0; i < 8; ++i) { reset(); success(); } // close/reopen reuses the same source and destination
    reset(); self_join = 1; success();
    const char *symbols[] = {
#define DM_GUEST_THREAD_SIGNATURE(name, result, parameters) dm_guest_symbol_##name,
#include "../guest_thread_signatures.inc"
#undef DM_GUEST_THREAD_SIGNATURE
        "malloc", "free"
    };
    for (unsigned i = 0; i < sizeof symbols / sizeof *symbols; ++i) {
        reset(); missing_symbol = symbols[i]; failure(0x1001);
        assert(!host_allocations && !guest_allocations && !creates);
    }
    optional_selftest_failures();
    reset(); host_fail = 1; failure(0x1001); assert(!guest_allocations && !creates);
    reset(); guest_fail = 1; failure(0x1001); assert(!guest_allocations && !creates);
    reset(); host_source = 1; failure(0x1001); assert(guest_frees == 1 && !creates);
    reset(); create_fail = 1; failure(0x1001); assert(creates == 1 && guest_frees == 1);
    reset(); failure(0); assert(!host_allocations && !guest_allocations && !creates);
    reset(); failure(SIZE_MAX); assert(!host_allocations && !guest_allocations && !creates);
    reset(); failure(SIZE_MAX - 0xfff); assert(host_allocations == 1 && !guest_allocations && !creates);
    reset(); current_handle = 0; failure(0x1001); assert(!host_allocations && !guest_allocations && !creates);
    reset(); assert(!heap(0));
    assert(!pages(1, alias_base, 0x10000)); allocation_base = alias_base;
    success(); reset(); assert(!pages(0, alias_base, 0x10000));
    puts("production guest-thread stack ownership/lifecycle tests passed");
    return map_memory_suite_main();
}
