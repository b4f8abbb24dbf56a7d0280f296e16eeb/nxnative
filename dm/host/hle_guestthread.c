// Host-initiated guest threads (host background work that calls the game's own allocator / locks, such as the movie player's, needs a complete SDK thread context).
// Approach: on a guest thread (e.g. before guest_main enters nnMain) call the SDK's nn::os::CreateThread / StartThread directly, with a host function as the entry --
// the thread is created by the SDK, so ThreadType, thread handle and TLS slots are all in place, and the game's malloc / nn::os locks work as usual (the lock tag word holds this thread's guest handle).
// Constraints: must be called on a guest thread (SDK thread creation goes through the SVC gate); not exposed to the game.
// Self-test DM_GUEST_SPAWN_TEST=1: before guest_main enters nnMain, start one thread that does "SDK malloc/free + lock an nn::os mutex and check that the owner is itself", and report the result after WaitThread finishes.
#include "hle.h"
#include "guest_thread_binding.h"
#include <unistd.h>
// A process has one immutable NSO load set before guest code starts. Resolve
// once in normal ELF order, reject unloadable NRO exports, and retain the exact
// SDK backend and allocator pair for every outstanding worker. This remains a
// guest SDK/TLS dependency, not an implementation of Atmosphere/libnx on iOS.
static dm_guest_thread_binding thread_binding = DM_GUEST_THREAD_BINDING_INIT;
static void *thread_import_resolve(void *context, const char *name) {
    (void)context; return guest_sym(name);
}
static const void *thread_import_owner(void *context, void *address) {
    (void)context;
    module_t *module = module_of(address);
    return module && !module->nro && module->loaded ? module : NULL;
}
static const dm_guest_thread_imports *thread_imports(void) {
    const dm_guest_thread_imports *api = dm_guest_thread_bind(&thread_binding, NULL, thread_import_resolve, thread_import_owner);
    if (!api) die("host guest thread: %s (%s)", thread_binding.error, thread_binding.symbol);
    return api;
}
// The handle keeps ThreadType first, but also owns its source allocation. Only
// join consumes it; resident workers intentionally retain it for their lifetime.
// Use the guest's matched C malloc/free family, not host aligned_alloc: the SDK
// borrows these pages with MapMemory, whose sources must have guest backing.
typedef struct {
    _Alignas(16) uint8_t tt[0x200];       // ThreadType (0x1c0 bytes, with headroom)
    void *storage;
    void (*release)(void *);
    dm_guest_WaitThread_fn wait;
    dm_guest_DestroyThread_fn destroy;
    void (*fn)(void *);
    void *arg;
} spawn_owner;
static _Thread_local spawn_owner *current_owner;
void dm_thread_mark_host(void);
uint32_t thr_cur_handle(void);
int dm_guest_map_source_available(const void *source, size_t size);
static void spawn_tramp(void *p) {
    spawn_owner *owner = p;
    dm_thread_mark_host();
    current_owner = owner;
    owner->fn(owner->arg);
    current_owner = NULL;
}
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) {
    if (!thr_cur_handle()) die("host guest thread: creation requires a guest thread");
    // Round the mapped span, then reserve enough extra bytes to align its start.
    // Both full pages and their padding stay inside this one guest allocation.
    if (!fn || !stack_size || stack_size > SIZE_MAX - 0xfff) die("host guest thread: invalid stack size or entry");
    stack_size = (stack_size + 0xfff) & ~(size_t)0xfff;
    const dm_guest_thread_imports *api = thread_imports();
    spawn_owner *owner = calloc(1, sizeof *owner);
    if (!owner) die("host guest thread: allocation failed");
    owner->storage = api->allocate(stack_size + 0xfff);
    owner->release = api->release; owner->wait = api->WaitThread; owner->destroy = api->DestroyThread;
    owner->fn = fn; owner->arg = arg;
    if (!owner->storage) { free(owner); die("host guest thread: guest stack allocation failed"); }
    uintptr_t raw = (uintptr_t)owner->storage;
    uintptr_t aligned = (raw + 0xfff) & ~(uintptr_t)0xfff;
    // This is an ownership/state check, not a host-readability test or a
    // reservation. SDK MapMemory remains the authoritative atomic borrow.
    if (raw > UINTPTR_MAX - 0xfff || !dm_guest_map_source_available((void *)aligned, stack_size)) {
        owner->release(owner->storage); free(owner);
        die("host guest thread: allocator returned an ineligible stack source");
    }
    dm_guest_thread_type *thread = (void *)owner->tt;
    int rc = api->CreateThread(thread, spawn_tramp, owner, (void *)aligned, stack_size, prio);
    if (rc) {
        owner->release(owner->storage); free(owner);
        die("host guest thread: CreateThread failed %#x", rc);
    }
    api->StartThread(thread);
    return owner;
}
void dm_guest_thread_join(void *tt) {
    if (!thr_cur_handle()) die("host guest thread: join requires a guest thread");
    spawn_owner *owner = tt;
    if (owner == current_owner) die("host guest thread: cannot join itself");
    dm_guest_thread_type *thread = (void *)owner->tt;
    owner->wait(thread);
    owner->destroy(thread);           // SDK has exited and released its alias stack
    owner->release(owner->storage);     // free the original base, never the aligned interior
    free(owner);
}

static dm_guest_mutex_binding mutex_binding = DM_GUEST_MUTEX_BINDING_INIT;
static const dm_guest_mutex_imports *selftest_mutex_imports(void) {
    (void)thread_imports();
    const dm_guest_mutex_imports *api = dm_guest_mutex_bind(&mutex_binding, NULL, thread_import_resolve, thread_import_owner);
    if (!api) die("host guest thread self-check: %s (%s)", mutex_binding.error, mutex_binding.symbol);
    if (mutex_binding.sdk_owner != thread_binding.owners[DM_GUEST_IMPORT_CreateThread])
        die("host guest thread self-check: mutex and thread SDK modules differ");
    return api;
}
static struct { int malloc_ok, lock_owned, handle; } g_st;
static void selftest_body(void *arg) {
    (void)arg;
    const dm_guest_thread_imports *api = thread_imports();
    void *p = api->allocate(4096); if (p) { memset(p, 0x5a, 4096); api->release(p); g_st.malloc_ok = 1; }
    uint8_t mtx[0x40] __attribute__((aligned(16))) = {0};           // nn::os::MutexType (0x20 bytes, with headroom)
    const dm_guest_mutex_imports *mutex_api = selftest_mutex_imports();
    dm_guest_mutex_type *mutex = (void *)mtx;
    mutex_api->InitializeMutex(mutex, 0, 0);
    mutex_api->LockMutex(mutex);
    g_st.lock_owned = mutex_api->IsMutexLockedByCurrentThread(mutex);
    mutex_api->UnlockMutex(mutex);
    uint32_t thr_cur_handle(void); g_st.handle = (int)thr_cur_handle();
}
static void park_body(void *arg) { (void)arg; for (;;) sleep(3600); }   // resident (=2): simulates the pre-decode thread staying alive for the whole run, occupying the handle
void dm_guest_spawn_selftest(void) {
    const char *e = getenv("DM_GUEST_SPAWN_TEST"); if (!e) return;
    (void)selftest_mutex_imports(); // validate optional family before any self-test allocation or spawn
    if (atoi(e) == 2) { dm_guest_thread_spawn(park_body, NULL, 16, 16 << 10); LOG("host guest thread self-check: start a resident thread (later game thread handles shift up by one)"); }
    void *tt = dm_guest_thread_spawn(selftest_body, NULL, 16, 64 << 10);
    dm_guest_thread_join(tt);
    uint32_t thr_cur_handle(void); int mine = (int)thr_cur_handle();
    int ok = g_st.malloc_ok && g_st.lock_owned && g_st.handle && g_st.handle != mine;
    LOG("%shost guest thread self-check: SDK malloc/free %s; mutex owner is this thread %s; thread handle %#x (main thread %#x)", ok ? "" : "[RED] ",
        g_st.malloc_ok ? "ok" : "broken", g_st.lock_owned ? "yes" : "no", g_st.handle, mine);
}
