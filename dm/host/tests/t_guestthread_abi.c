#include <assert.h>
#include <stdio.h>
#include "../guest_thread_binding.h"
#if defined(DM_REQUIRE_AARCH64) && !defined(__aarch64__)
#error "native guest thread ABI check requires AArch64"
#endif
void dm_test_thread_expect(void *, dm_guest_thread_entry, void *, void *);
void *dm_test_thread_export(const char *);
static int calls, identity;
static void *allocate(dm_guest_size size) { assert(size == 4096); return &identity; }
static void release(void *p) { assert(p == &identity); ++calls; }
static void entry(void *p) { assert(p == &calls); ++calls; }
static const void *owner(void *context, void *p) { (void)context; assert(p); return &identity; }
static void *resolve(void *context, const char *name) {
    (void)context;
    if (!strcmp(name, "malloc")) return (void *)allocate;
    if (!strcmp(name, "free")) return (void *)release;
    return dm_test_thread_export(name);
}
int main(void) {
    dm_guest_thread_binding binding = DM_GUEST_THREAD_BINDING_INIT;
    const dm_guest_thread_imports *api = dm_guest_thread_bind(&binding, NULL, resolve, owner);
    assert(api);
    int thread_storage, stack_storage;
    dm_guest_thread_type *thread = (void *)&thread_storage;
    dm_test_thread_expect(thread, entry, &calls, &stack_storage);
    assert(api->CreateThread(thread, entry, &calls, &stack_storage, 0x123456789abcdef0UL, -13) == 0x1203);
    api->StartThread(thread); api->WaitThread(thread); api->DestroyThread(thread);
    api->release(api->allocate(4096)); assert(calls == 2);
    dm_guest_mutex_binding mutex_binding = DM_GUEST_MUTEX_BINDING_INIT;
    const dm_guest_mutex_imports *mutex_api = dm_guest_mutex_bind(&mutex_binding, NULL, resolve, owner);
    assert(mutex_api);
    dm_guest_mutex_type *mutex = (void *)thread;
    for (int recursive = 0; recursive < 2; ++recursive) {
        mutex_api->InitializeMutex(mutex, (_Bool)recursive, -7); mutex_api->LockMutex(mutex);
        assert(mutex_api->IsMutexLockedByCurrentThread(mutex) == (_Bool)recursive);
        mutex_api->UnlockMutex(mutex);
    }
    assert(!pthread_mutex_destroy(&mutex_binding.mutex));
    assert(!pthread_mutex_destroy(&binding.mutex));
    puts("C binding to typed C++ guest-shaped exports passed (synthetic ABI, no SDK/TLS validation)");
}
