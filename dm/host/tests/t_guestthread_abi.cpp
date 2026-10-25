// Synthetic C++ guest-shaped exports. This checks the import call boundary on
// the executing architecture, not a Nintendo SDK binary or its TLS/layout.
#include "../guest_thread_imports.h"
#include <cassert>
#include <cstring>
#if defined(DM_REQUIRE_AARCH64) && !defined(__aarch64__)
#error "native guest thread ABI check requires AArch64"
#endif
static int phase, mutex_phase;
static bool recursive_value;
static void *expected_thread, *expected_arg, *expected_stack;
static dm_guest_thread_entry expected_entry;
extern "C" void dm_test_thread_expect(void *thread, dm_guest_thread_entry entry, void *arg, void *stack) {
    phase = 0; expected_thread = thread; expected_entry = entry; expected_arg = arg; expected_stack = stack;
}
namespace nn { namespace os {
int CreateThread(ThreadType *thread, dm_guest_thread_entry entry, void *arg, void *stack, dm_guest_size size, int prio) {
    assert(phase++ == 0 && thread == expected_thread && entry == expected_entry && arg == expected_arg && stack == expected_stack);
    assert(size == 0x123456789abcdef0UL && prio == -13);
    entry(arg); return 0x1203;
}
void StartThread(ThreadType *thread) { assert(phase++ == 1 && thread == expected_thread); }
void WaitThread(ThreadType *thread) { assert(phase++ == 2 && thread == expected_thread); }
void DestroyThread(ThreadType *thread) { assert(phase++ == 3 && thread == expected_thread); }
void InitializeMutex(MutexType *mutex, bool recursive, int level) {
    assert(mutex == expected_thread && level == -7); mutex_phase = 0; recursive_value = recursive;
}
void LockMutex(MutexType *mutex) { assert(mutex == expected_thread && mutex_phase++ == 0); }
bool IsMutexLockedByCurrentThread(const MutexType *mutex) { assert(mutex == expected_thread && mutex_phase++ == 1); return recursive_value; }
void UnlockMutex(MutexType *mutex) { assert(mutex == expected_thread && mutex_phase++ == 2); }
} }
extern "C" void *dm_test_thread_export(const char *name) {
#define DM_GUEST_THREAD_SIGNATURE(api, result, parameters) \
    if (!std::strcmp(name, dm_guest_symbol_##api)) { \
        dm_guest_##api##_fn fn = &nn::os::api; void *address; \
        static_assert(sizeof fn == sizeof address, "function pointer representation"); \
        std::memcpy(&address, &fn, sizeof address); return address; \
    }
#include "../guest_thread_signatures.inc"
#define DM_GUEST_MUTEX_SIGNATURE DM_GUEST_THREAD_SIGNATURE
#include "../guest_mutex_signatures.inc"
#undef DM_GUEST_MUTEX_SIGNATURE
#undef DM_GUEST_THREAD_SIGNATURE
    return nullptr;
}
