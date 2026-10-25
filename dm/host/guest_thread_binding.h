#ifndef DM_GUEST_THREAD_BINDING_H
#define DM_GUEST_THREAD_BINDING_H
#include "guest_thread_imports.h"
#include <pthread.h>
#include <string.h>

enum {
#define DM_GUEST_THREAD_SIGNATURE(name, result, parameters) DM_GUEST_IMPORT_##name,
#include "guest_thread_signatures.inc"
#undef DM_GUEST_THREAD_SIGNATURE
    DM_GUEST_THREAD_IMPORT_COUNT,
    DM_GUEST_IMPORT_MALLOC = DM_GUEST_THREAD_IMPORT_COUNT,
    DM_GUEST_IMPORT_FREE,
    DM_GUEST_IMPORT_COUNT
};
typedef struct {
    pthread_mutex_t mutex;
    int attempted;
    dm_guest_thread_imports api;
    const void *owners[DM_GUEST_IMPORT_COUNT];
    const char *error, *symbol;
} dm_guest_thread_binding;
#define DM_GUEST_THREAD_BINDING_INIT { .mutex = PTHREAD_MUTEX_INITIALIZER }

// The resolver preserves the loader's normal ELF lookup order. owner must
// return a stable, process-lifetime module identity or NULL for an unloadable /
// unknown address. malloc/free may be interposed in different permanent modules;
// this captures the existing pair, it does not invent an allocator ABI guarantee.
// A failed bind is cached too. No partial table is ever published or called.
static inline const dm_guest_thread_imports *dm_guest_thread_bind(
    dm_guest_thread_binding *state, void *context,
    void *(*resolve)(void *, const char *), const void *(*owner)(void *, void *)) {
    pthread_mutex_lock(&state->mutex);
    if (!state->attempted) {
        dm_guest_thread_imports candidate = {0};
        const void *owners[DM_GUEST_IMPORT_COUNT] = {0};
        void *address;
#define DM_BIND(field, spelling, index) do { \
    state->symbol = spelling; \
    address = resolve(context, spelling); \
    if (!address) { state->error = "missing symbol"; goto finish; } \
    owners[index] = owner(context, address); \
    if (!owners[index]) { state->error = "symbol has no permanent module owner"; goto finish; } \
    _Static_assert(sizeof candidate.field == sizeof address, "guest function pointer representation"); \
    memcpy(&candidate.field, &address, sizeof address); \
} while (0)
#define DM_GUEST_THREAD_SIGNATURE(name, result, parameters) DM_BIND(name, dm_guest_symbol_##name, DM_GUEST_IMPORT_##name);
#include "guest_thread_signatures.inc"
#undef DM_GUEST_THREAD_SIGNATURE
        for (int i = 1; i < DM_GUEST_THREAD_IMPORT_COUNT; ++i) {
            if (owners[i] != owners[0]) {
                state->error = "thread operations belong to different SDK modules";
                goto finish;
            }
        }
        DM_BIND(allocate, "malloc", DM_GUEST_IMPORT_MALLOC);
        DM_BIND(release, "free", DM_GUEST_IMPORT_FREE);
#undef DM_BIND
        state->api = candidate;
        memcpy(state->owners, owners, sizeof owners);
        state->symbol = NULL;
finish:
        state->attempted = 1;
    }
    const dm_guest_thread_imports *result = state->error ? NULL : &state->api;
    pthread_mutex_unlock(&state->mutex);
    return result;
}

// Separate, lazy table for DM_GUEST_SPAWN_TEST. Normal spawn never resolves
// these optional exports and gains no dependency on the mutex symbol family.
typedef struct {
    pthread_mutex_t mutex;
    int attempted;
    dm_guest_mutex_imports api;
    const void *sdk_owner;
    const char *error, *symbol;
} dm_guest_mutex_binding;
#define DM_GUEST_MUTEX_BINDING_INIT { .mutex = PTHREAD_MUTEX_INITIALIZER }
static inline const dm_guest_mutex_imports *dm_guest_mutex_bind(
    dm_guest_mutex_binding *state, void *context,
    void *(*resolve)(void *, const char *), const void *(*owner)(void *, void *)) {
    pthread_mutex_lock(&state->mutex);
    if (!state->attempted) {
        dm_guest_mutex_imports candidate = {0};
        const void *sdk_owner = NULL;
#define DM_GUEST_MUTEX_SIGNATURE(name, result, parameters) do { \
    state->symbol = dm_guest_symbol_##name; \
    void *address = resolve(context, dm_guest_symbol_##name); \
    if (!address) { state->error = "missing symbol"; goto finish; } \
    const void *identity = owner(context, address); \
    if (!identity) { state->error = "symbol has no permanent module owner"; goto finish; } \
    if (sdk_owner && sdk_owner != identity) { state->error = "mutex operations belong to different SDK modules"; goto finish; } \
    sdk_owner = identity; \
    _Static_assert(sizeof candidate.name == sizeof address, "guest function pointer representation"); \
    memcpy(&candidate.name, &address, sizeof address); \
} while (0);
#include "guest_mutex_signatures.inc"
#undef DM_GUEST_MUTEX_SIGNATURE
        state->api = candidate; state->sdk_owner = sdk_owner; state->symbol = NULL;
finish:
        state->attempted = 1;
    }
    const dm_guest_mutex_imports *result = state->error ? NULL : &state->api;
    pthread_mutex_unlock(&state->mutex);
    return result;
}
#endif
