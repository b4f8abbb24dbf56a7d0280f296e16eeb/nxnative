// Synthetic exports only. Exercises the production binding boundary, not SDK
// TLS/layout or the ARM64 calling convention; native CI also checks signatures.
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "../guest_thread_binding.h"

static int create(dm_guest_thread_type *thread, dm_guest_thread_entry entry, void *arg, void *stack, dm_guest_size size, int prio) {
    (void)thread; (void)entry; (void)arg; (void)stack; (void)size; (void)prio; return 0;
}
static void thread_op(dm_guest_thread_type *thread) { (void)thread; }
static void *allocate(dm_guest_size n) { (void)n; return NULL; }
static void release(void *p) { (void)p; }
static const char *const symbols[] = {
#define DM_GUEST_THREAD_SIGNATURE(name, result, parameters) dm_guest_symbol_##name,
#include "../guest_thread_signatures.inc"
#undef DM_GUEST_THREAD_SIGNATURE
    "malloc", "free"
};
static void initialize_mutex(dm_guest_mutex_type *p, dm_guest_bool recursive, int level) { (void)p; (void)recursive; (void)level; }
static void mutex_op(dm_guest_mutex_type *p) { (void)p; }
static dm_guest_bool mutex_locked(const dm_guest_mutex_type *p) { (void)p; return 1; }
static const char *const mutex_symbols[] = {
#define DM_GUEST_MUTEX_SIGNATURE(name, result, parameters) dm_guest_symbol_##name,
#include "../guest_mutex_signatures.inc"
#undef DM_GUEST_MUTEX_SIGNATURE
};
enum { MUTEX_IMPORT_COUNT = sizeof mutex_symbols / sizeof *mutex_symbols };
typedef struct {
    dm_guest_thread_binding binding;
    dm_guest_mutex_binding mutex_binding;
    int mutex_mode;
    void *mutex_addresses[MUTEX_IMPORT_COUNT];
    _Atomic int mutex_calls[MUTEX_IMPORT_COUNT];
    void *addresses[DM_GUEST_IMPORT_COUNT];
    int missing, transient, mixed, hidden;
    int sdk_identity, allocator_identity, interposed_free_identity;
    pthread_mutex_t gate;
    pthread_cond_t condition;
    int ready, go;
    _Atomic int calls[DM_GUEST_IMPORT_COUNT];
} fixture;
static void init(fixture *f) {
    memset(f, 0, sizeof *f); assert(!pthread_mutex_init(&f->binding.mutex, NULL));
    assert(!pthread_mutex_init(&f->mutex_binding.mutex, NULL));
    assert(!pthread_mutex_init(&f->gate, NULL));
    assert(!pthread_cond_init(&f->condition, NULL));
    for (int i = 0; i < DM_GUEST_IMPORT_COUNT; ++i) atomic_init(&f->calls[i], 0);
    for (int i = 0; i < MUTEX_IMPORT_COUNT; ++i) atomic_init(&f->mutex_calls[i], 0);
    f->mutex_addresses[0] = (void *)initialize_mutex;
    f->mutex_addresses[1] = (void *)mutex_op;
    f->mutex_addresses[2] = (void *)mutex_locked;
    f->mutex_addresses[3] = (void *)mutex_op;
    f->missing = f->transient = -1;
    // Separate thread-op addresses matter for owner lookup; synthetic unused
    // stand-ins can share them because their lifetime/module are identical.
    f->addresses[DM_GUEST_IMPORT_CreateThread] = (void *)create;
    f->addresses[DM_GUEST_IMPORT_StartThread] = (void *)thread_op;
    f->addresses[DM_GUEST_IMPORT_WaitThread] = (void *)thread_op;
    f->addresses[DM_GUEST_IMPORT_DestroyThread] = (void *)thread_op;
    f->addresses[DM_GUEST_IMPORT_MALLOC] = (void *)allocate;
    f->addresses[DM_GUEST_IMPORT_FREE] = (void *)release;
}
static void *resolve(void *context, const char *name) {
    fixture *f = context;
    if (f->mutex_mode) {
        for (int i = 0; i < MUTEX_IMPORT_COUNT; ++i) if (!strcmp(name, mutex_symbols[i])) {
            atomic_fetch_add(&f->mutex_calls[i], 1);
            return f->hidden || i == f->missing ? NULL : f->mutex_addresses[i];
        }
        assert(0); return NULL;
    }
    for (int i = 0; i < DM_GUEST_IMPORT_COUNT; ++i) if (!strcmp(name, symbols[i])) {
        atomic_fetch_add(&f->calls[i], 1);
        return f->hidden || i == f->missing ? NULL : f->addresses[i];
    }
    assert(0); return NULL;
}
static const void *owner(void *context, void *p) {
    fixture *f = context;
    if (f->mutex_mode) {
        if (f->transient >= 0 && p == f->mutex_addresses[f->transient]) return NULL;
        if (f->mixed && p != (void *)initialize_mutex) return &f->allocator_identity;
        return &f->sdk_identity;
    }
    if (f->transient >= 0 && p == f->addresses[f->transient]) return NULL;
    if (p == (void *)allocate) return &f->allocator_identity;
    if (p == (void *)release) return &f->interposed_free_identity;
    if (f->mixed && p == (void *)thread_op) return &f->allocator_identity;
    return &f->sdk_identity;
}
static const dm_guest_thread_imports *bind(fixture *f) { return dm_guest_thread_bind(&f->binding, f, resolve, owner); }
static void *worker(void *context) {
    fixture *f = context;
    pthread_mutex_lock(&f->gate); ++f->ready; pthread_cond_broadcast(&f->condition);
    while (!f->go) pthread_cond_wait(&f->condition, &f->gate);
    pthread_mutex_unlock(&f->gate);
    for (int i = 0; i < 256; ++i) {
        if (f->mutex_mode) {
            const dm_guest_mutex_imports *api = dm_guest_mutex_bind(&f->mutex_binding, f, resolve, owner);
            if (f->missing >= 0 || f->transient >= 0 || f->mixed) assert(!api);
            else assert(api && api->InitializeMutex == initialize_mutex && api->LockMutex == mutex_op && api->IsMutexLockedByCurrentThread == mutex_locked && api->UnlockMutex == mutex_op);
            continue;
        }
        const dm_guest_thread_imports *api = bind(f);
        if (f->missing >= 0 || f->transient >= 0 || f->mixed) assert(!api);
        else assert(api && api->CreateThread == create && api->StartThread == thread_op && api->WaitThread == thread_op && api->DestroyThread == thread_op && api->allocate == allocate && api->release == release);
    }
    return NULL;
}
static void concurrent(fixture *f) {
    pthread_t threads[24];
    for (unsigned i = 0; i < 24; ++i) assert(!pthread_create(&threads[i], NULL, worker, f));
    pthread_mutex_lock(&f->gate);
    while (f->ready != 24) pthread_cond_wait(&f->condition, &f->gate);
    f->go = 1; pthread_cond_broadcast(&f->condition); pthread_mutex_unlock(&f->gate);
    for (unsigned i = 0; i < 24; ++i) assert(!pthread_join(threads[i], NULL));
    for (int i = 0; i < DM_GUEST_IMPORT_COUNT; ++i) assert(atomic_load(&f->calls[i]) <= 1);
    for (int i = 0; i < MUTEX_IMPORT_COUNT; ++i) assert(atomic_load(&f->mutex_calls[i]) <= 1);
}
static void finish(fixture *f) {
    assert(!pthread_mutex_destroy(&f->binding.mutex));
    assert(!pthread_mutex_destroy(&f->mutex_binding.mutex));
    assert(!pthread_mutex_destroy(&f->gate));
    assert(!pthread_cond_destroy(&f->condition));
}
int main(void) {
    fixture f; init(&f); concurrent(&f);
    assert(!f.binding.error && !f.mutex_binding.attempted);
    for (int i = 0; i < MUTEX_IMPORT_COUNT; ++i) assert(!atomic_load(&f.mutex_calls[i]));
    // Captures actual permanent identities; allocator interposition is retained.
    assert(f.binding.owners[DM_GUEST_IMPORT_MALLOC] == &f.allocator_identity);
    assert(f.binding.owners[DM_GUEST_IMPORT_FREE] == &f.interposed_free_identity);
    for (int i = 0; i < DM_GUEST_IMPORT_COUNT; ++i) assert(atomic_load(&f.calls[i]) == 1);
    f.hidden = 1; assert(bind(&f)); // resolved callbacks outlive symbol lookup changes
    finish(&f);
    for (int i = 0; i < DM_GUEST_IMPORT_COUNT; ++i) {
        init(&f); f.missing = i; concurrent(&f);
        assert(f.binding.error && !f.binding.api.CreateThread && !f.binding.api.allocate);
        f.missing = -1; assert(!bind(&f)); // failure is also immutable
        finish(&f);
        init(&f); f.transient = i; concurrent(&f);
        assert(f.binding.error && !f.binding.api.CreateThread && !f.binding.api.allocate);
        finish(&f);
    }
    init(&f); f.mixed = 1; concurrent(&f); assert(!bind(&f));
    finish(&f);
    init(&f); f.mutex_mode = 1; concurrent(&f);
    assert(!f.mutex_binding.error && !f.binding.attempted);
    for (int i = 0; i < MUTEX_IMPORT_COUNT; ++i) assert(atomic_load(&f.mutex_calls[i]) == 1);
    f.hidden = 1; assert(dm_guest_mutex_bind(&f.mutex_binding, &f, resolve, owner)); finish(&f);
    for (int i = 0; i < MUTEX_IMPORT_COUNT; ++i) {
        init(&f); f.mutex_mode = 1; f.missing = i; concurrent(&f);
        assert(f.mutex_binding.error && !f.mutex_binding.api.InitializeMutex);
        f.missing = -1; assert(!dm_guest_mutex_bind(&f.mutex_binding, &f, resolve, owner)); finish(&f);
        init(&f); f.mutex_mode = 1; f.transient = i; concurrent(&f);
        assert(f.mutex_binding.error && !f.mutex_binding.api.InitializeMutex); finish(&f);
    }
    init(&f); f.mutex_mode = f.mixed = 1; concurrent(&f); assert(f.mutex_binding.error); finish(&f);
    puts("guest thread imports: concurrent one-time binding, complete/partial/missing families, permanent owners and captured interposition passed");
}
