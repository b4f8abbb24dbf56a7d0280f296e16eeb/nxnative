// Portable dispatch vertical slice. Deliberately NOT globally registered yet.
#include "hle_standard_allocator_adapter.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY
// Kept independent of dm.h so portable adapter fixtures need no game profile.
extern int dm_memory_experiment_authorized(const struct dm_memory_experiment_authorization *, const void *);
#endif

_Static_assert(sizeof(dm_sa_hash) == 24, "Hash is an AArch64 indirect x8 result");
_Static_assert(sizeof(size_t) == 8, "StandardAllocator ABI requires 64-bit size_t");
enum {
    CTOR1, CTOR2, CTOR_BUFFER1, CTOR_BUFFER2, CTOR_CACHE1, CTOR_CACHE2,
    INIT, INIT_CACHE, FINALIZE, ALLOCATE, ALLOCATE_ALIGNED, FREE, REALLOCATE,
    SHRINK, SIZE, TOTAL_FREE, ALLOCATABLE, CLEAR_CACHE, CLEANUP, WALK, DUMP, HASH
};
static const char *const names[DM_SA_ADAPTER_SYMBOL_COUNT] = {
    "_ZN2nn3mem17StandardAllocatorC1Ev",
    "_ZN2nn3mem17StandardAllocatorC2Ev",
    "_ZN2nn3mem17StandardAllocatorC1EPvm",
    "_ZN2nn3mem17StandardAllocatorC2EPvm",
    "_ZN2nn3mem17StandardAllocatorC1EPvmb",
    "_ZN2nn3mem17StandardAllocatorC2EPvmb",
    "_ZN2nn3mem17StandardAllocator10InitializeEPvm",
    "_ZN2nn3mem17StandardAllocator10InitializeEPvmb",
    "_ZN2nn3mem17StandardAllocator8FinalizeEv",
    "_ZN2nn3mem17StandardAllocator8AllocateEm",
    "_ZN2nn3mem17StandardAllocator8AllocateEmm",
    "_ZN2nn3mem17StandardAllocator4FreeEPv",
    "_ZN2nn3mem17StandardAllocator10ReallocateEPvm",
    "_ZN2nn3mem17StandardAllocator6ShrinkEPvm",
    "_ZNK2nn3mem17StandardAllocator9GetSizeOfEPKv",
    "_ZNK2nn3mem17StandardAllocator16GetTotalFreeSizeEv",
    "_ZNK2nn3mem17StandardAllocator18GetAllocatableSizeEv",
    "_ZNK2nn3mem17StandardAllocator16ClearThreadCacheEv",
    "_ZNK2nn3mem17StandardAllocator21CleanUpManagementAreaEv",
    "_ZNK2nn3mem17StandardAllocator19WalkAllocatedBlocksEPFiPvmS2_ES2_",
    "_ZNK2nn3mem17StandardAllocator4DumpEv",
    "_ZNK2nn3mem17StandardAllocator4HashEv"
};

typedef struct instance {
    void *self;
    dm_sa_allocator *allocator;
    dm_sa_config configuration;
    unsigned users;
    uint64_t generation, diagnostic_failures;
    uint64_t page_sequence, page_event_count;
    unsigned page_success_seen;
    int initializing, finalizing;
    struct instance *next;
} instance;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle = PTHREAD_COND_INITIALIZER;
static instance *instances;
static uint64_t next_generation;
static dm_sa_adapter_config settings;
static dm_sa_guest_fn originals[DM_SA_ADAPTER_SYMBOL_COUNT];
static _Atomic int active, stopped;
static int preparing;
struct dm_sa_adapter_prepared {
    dm_sa_adapter_config config;
    dm_sa_guest_fn originals[DM_SA_ADAPTER_SYMBOL_COUNT];
};
static dm_sa_adapter_prepared *pending;
static void require_active(void) {
    if (!atomic_load_explicit(&active, memory_order_acquire) ||
        atomic_load_explicit(&stopped, memory_order_acquire)) abort();
}

static _Noreturn void fail(const char *operation, dm_sa_result result) {
    if (settings.fatal) settings.fatal(settings.context, operation, result);
    abort();
}
static instance *find(const void *self) {
    for (instance *i = instances; i; i = i->next) if (i->self == self) return i;
    return NULL;
}
static instance *take(const void *self, const char *operation) {
    require_active();
    pthread_mutex_lock(&mutex);
    instance *i = find(self);
    if (i && (i->initializing || !i->allocator || i->finalizing)) {
        dm_sa_result result = i->initializing ? DM_SA_BUSY : DM_SA_CLOSED;
        pthread_mutex_unlock(&mutex);
        fail(operation, result);
    }
    if (i) i->users++;
    pthread_mutex_unlock(&mutex);
    return i;
}
static void drop(instance *i) {
    pthread_mutex_lock(&mutex);
    if (!--i->users) pthread_cond_broadcast(&idle);
    pthread_mutex_unlock(&mutex);
}
// Snapshot while an in-flight reference or exclusive lifecycle protects the
// core, then notify only after releasing it. Never touch the instance in notify.
static dm_sa_adapter_event event_snapshot(instance *i, dm_sa_allocator *allocator,
                                          dm_sa_adapter_event_kind kind,
                                          dm_sa_result result, size_t size, size_t alignment) {
    dm_sa_adapter_event event = {.kind = kind, .self = i->self,
        .generation = i->generation, .requested_size = size, .alignment = alignment,
        .result = result, .stats_result = DM_SA_CLOSED};
    if (allocator) {
        event.stats_result = dm_sa_get_stats(allocator, &event.stats);
        if (settings.page_events) {
            if (event.stats_result == DM_SA_OK) { event.pages = event.stats.pages; event.pages_valid = 1; }
            else event.pages_valid = dm_sa_get_page_diagnostics(allocator, &event.pages) == DM_SA_OK;
        }
    }
    return event;
}
static int failure_snapshot(instance *i, dm_sa_adapter_event_kind kind,
                            dm_sa_result result, size_t size, size_t alignment,
                            dm_sa_adapter_event *out) {
    if (!settings.event || result == DM_SA_OK) return 0;
    pthread_mutex_lock(&mutex);
    uint64_t count = i->diagnostic_failures;
    if (count != UINT64_MAX) count = ++i->diagnostic_failures;
    pthread_mutex_unlock(&mutex);
    if (count > 4 && (count & (count - 1))) return 0;
    *out = event_snapshot(i, i->allocator, kind, result, size, alignment);
    out->failure_count = count;
    return 1;
}
static int page_snapshot(instance *i, dm_sa_result result, dm_sa_adapter_event *out) {
    if (!settings.event || !settings.page_events) return 0;
    dm_sa_page_diagnostics pages;
    if (dm_sa_get_page_diagnostics(i->allocator, &pages) != DM_SA_OK) return 0;
    pthread_mutex_lock(&mutex);
    unsigned success = (pages.reclaim_successes ? 1u : 0u) | (pages.recommit_successes ? 2u : 0u);
    int first_success = (success & ~i->page_success_seen) != 0;
    if (pages.sequence < i->page_sequence || (pages.sequence == i->page_sequence &&
        !(pages.sequence == UINT64_MAX && first_success))) { pthread_mutex_unlock(&mutex); return 0; }
    i->page_sequence = pages.sequence;
    uint64_t count = i->page_event_count;
    if (count != UINT64_MAX) count = ++i->page_event_count;
    i->page_success_seen |= success;
    pthread_mutex_unlock(&mutex);
    if (!first_success && count > 4 && (count & (count - 1))) return 0;
    *out = event_snapshot(i, i->allocator, DM_SA_EVENT_PAGE_ACTIVITY, result, 0, 0);
    out->page_event_count = count;
    // The authoritative stats scan may be newer; keep its counters if valid.
    // Neither sequence nor sample order promises console order across threads.
    return 1;
}
static void event_notify(const dm_sa_adapter_event *event) {
    settings.event(settings.context, event);
}
int dm_sa_adapter_is_managed(const void *self) {
    if (!atomic_load_explicit(&active, memory_order_acquire)) return 0;
    pthread_mutex_lock(&mutex);
    int yes = find(self) != NULL;
    pthread_mutex_unlock(&mutex);
    return yes;
}
const char *dm_sa_adapter_symbol_name(size_t index) {
    return index < DM_SA_ADAPTER_SYMBOL_COUNT ? names[index] : NULL;
}
#define ORIGINAL(n, type) ((type)originals[n])

static void forget_finalized(void *self) {
    require_active();
    pthread_mutex_lock(&mutex);
    instance **p = &instances;
    while (*p && (*p)->self != self) p = &(*p)->next;
    if (*p) {
        instance *i = *p;
        if (i->initializing || i->allocator || i->users || i->finalizing) {
            pthread_mutex_unlock(&mutex);
            fail("constructor/Initialize", DM_SA_BUSY);
        }
        *p = i->next;
        free(i);
    }
    pthread_mutex_unlock(&mutex);
}
static void initialize(void *self, void *buffer, size_t capacity, unsigned cache, int overload) {
    require_active();
    if (buffer) {
        forget_finalized(self);
        if (overload == INIT) ORIGINAL(INIT, void (*)(void *, void *, size_t))(self, buffer, capacity);
        else ORIGINAL(INIT_CACHE, void (*)(void *, void *, size_t, unsigned))(self, buffer, capacity, cache);
        return;
    }
    if (!self || !capacity || (capacity & 4095u) || capacity > INT64_MAX)
        fail("Initialize", DM_SA_INVALID);
    pthread_mutex_lock(&mutex);
    instance *i = find(self);
    if (i && (i->initializing || i->allocator || i->users || i->finalizing)) {
        pthread_mutex_unlock(&mutex);
        fail("Initialize", DM_SA_BUSY);
    }
    if (((unsigned char *)self)[0] || next_generation == UINT64_MAX) {
        pthread_mutex_unlock(&mutex);
        fail("Initialize", DM_SA_INVALID);
    }
    int fresh = !i;
    if (!i) i = calloc(1, sizeof *i);
    if (!i) { pthread_mutex_unlock(&mutex); fail("Initialize", DM_SA_NO_MEMORY); }
    i->self = self;
    i->initializing = 1;
    i->generation = ++next_generation;
    i->diagnostic_failures = 0;
    i->page_sequence = i->page_event_count = 0; i->page_success_seen = 0;
    if (fresh) { i->next = instances; instances = i; }
    pthread_mutex_unlock(&mutex);
    // Factory/provider creation may call other allocator instances. Publish an
    // initializing owner first so same-instance reentry/lifecycle races fail
    // without escaping to SDK or reusing this identity while the lock is absent.
    dm_sa_config config = {0};
    dm_sa_allocator *allocator = NULL;
    dm_sa_result r = settings.make_config(settings.context, self, capacity, cache & 1u, &config);
    int configured = r == DM_SA_OK;
    // Requested capacity is never silently shrunk, nor represented as a fake
    // reservation. It bounds lazily acquired real backing in the core.
    if (r == DM_SA_OK && config.capacity != capacity) r = DM_SA_INVALID;
    if (r == DM_SA_OK) r = dm_sa_create(&allocator, &config);
    if (r != DM_SA_OK && configured && settings.release_config)
        settings.release_config(settings.context, self, &config);
    dm_sa_adapter_event event;
    int notify = settings.event && r == DM_SA_OK;
    if (notify) event = event_snapshot(i, allocator, DM_SA_EVENT_INITIALIZED, r, capacity, 0);
    pthread_mutex_lock(&mutex);
    i->initializing = 0;
    if (r == DM_SA_OK) {
        i->allocator = allocator; i->configuration = config;
        ((unsigned char *)self)[0] = 1;
        ((unsigned char *)self)[1] = (unsigned char)(cache & 1u);
    } else if (fresh) {
        instance **link = &instances;
        while (*link != i) link = &(*link)->next;
        *link = i->next;
        free(i);
    }
    // A failed reinitialization retains the prior finalized tombstone. Only a
    // failed first initialization removes its provisional ownership record.
    pthread_mutex_unlock(&mutex);
    if (r != DM_SA_OK) fail("Initialize", r);
    if (notify) event_notify(&event);
}
static void ctor1(void *self) { forget_finalized(self); ORIGINAL(CTOR1, void (*)(void *))(self); }
static void ctor2(void *self) { forget_finalized(self); ORIGINAL(CTOR2, void (*)(void *))(self); }
static void ctor_buffer(void *self, void *buffer, size_t capacity, int which) {
    forget_finalized(self);
    if (buffer) ORIGINAL(which, void (*)(void *, void *, size_t))(self, buffer, capacity);
    else { ORIGINAL(CTOR1, void (*)(void *))(self); initialize(self, NULL, capacity, 0, INIT); }
}
static void ctor_buffer1(void *s, void *b, size_t n) { ctor_buffer(s, b, n, CTOR_BUFFER1); }
static void ctor_buffer2(void *s, void *b, size_t n) { ctor_buffer(s, b, n, CTOR_BUFFER2); }
static void ctor_cache(void *self, void *buffer, size_t capacity, unsigned cache, int which) {
    forget_finalized(self);
    if (buffer) ORIGINAL(which, void (*)(void *, void *, size_t, unsigned))(self, buffer, capacity, cache);
    else { ORIGINAL(CTOR1, void (*)(void *))(self); initialize(self, NULL, capacity, cache, INIT_CACHE); }
}
static void ctor_cache1(void *s, void *b, size_t n, unsigned c) { ctor_cache(s,b,n,c,CTOR_CACHE1); }
static void ctor_cache2(void *s, void *b, size_t n, unsigned c) { ctor_cache(s,b,n,c,CTOR_CACHE2); }
static void init(void *s, void *b, size_t n) { initialize(s,b,n,0,INIT); }
static void init_cache(void *s, void *b, size_t n, unsigned c) { initialize(s,b,n,c,INIT_CACHE); }
static void finalize(void *self) {
    require_active();
    pthread_mutex_lock(&mutex);
    instance *i = find(self);
    if (!i) { pthread_mutex_unlock(&mutex); ORIGINAL(FINALIZE, void (*)(void *))(self); return; }
    if (i->initializing || !i->allocator || i->finalizing) {
        dm_sa_result result = i->initializing ? DM_SA_BUSY : DM_SA_CLOSED;
        pthread_mutex_unlock(&mutex); fail("Finalize", result);
    }
    i->finalizing = 1;
    while (i->users) pthread_cond_wait(&idle, &mutex);
    pthread_mutex_unlock(&mutex);
    dm_sa_page_diagnostics final_pages = {0};
    int have_pages = settings.event && settings.page_events &&
        dm_sa_get_page_diagnostics(i->allocator, &final_pages) == DM_SA_OK;
    dm_sa_result r = dm_sa_destroy(i->allocator);
    dm_sa_adapter_event event;
    int notify = settings.event != NULL;
    // On success the core is already gone. On failure finalizing still guards
    // its retained ownership, so a best-effort snapshot is safe before unlock.
    if (notify) event = event_snapshot(i, r == DM_SA_OK ? NULL : i->allocator,
        DM_SA_EVENT_FINALIZED, r, i->configuration.capacity, 0);
    if (notify && r == DM_SA_OK && have_pages) { event.pages = final_pages; event.pages_valid = 1; }
    if (r == DM_SA_OK && settings.release_config)
        settings.release_config(settings.context, self, &i->configuration);
    pthread_mutex_lock(&mutex);
    if (r == DM_SA_OK) { i->allocator = NULL; memset(&i->configuration, 0, sizeof i->configuration); ((unsigned char *)self)[0] = 0; }
    i->finalizing = 0;
    pthread_mutex_unlock(&mutex);
    if (notify) event_notify(&event);
    if (r != DM_SA_OK) fail("Finalize", r);
    // Keep an ownership tombstone until explicit constructor/reinitialization.
    // A stale method cannot suddenly reach an uninitialized SDK CentralHeap.
}
static void *managed_allocate(instance *i, size_t size, size_t alignment) {
    void *out = NULL;
    dm_sa_result r = DM_SA_INVALID;
    // The observed SDK accepts nonzero sub-16 powers of two; backing remains at least
    // 16-aligned. Reject >INT64_MAX sizes before rounding, as the SDK does.
    // StandardAllocator passes flags=0 to CentralHeap, disabling its optional
    // malloc(0) sentinel. Both cached and central zero paths return NULL and
    // contribute no live block to Hash/walk, unlike the generic core API.
    if (!size) { drop(i); return NULL; }
    if (size <= INT64_MAX && alignment && alignment <= INT64_MAX && !(alignment & (alignment - 1)))
        r = dm_sa_allocate(i->allocator, size, alignment < 16 ? 16 : alignment, &out);
    dm_sa_adapter_event event;
    int notify = failure_snapshot(i, DM_SA_EVENT_ALLOCATE_FAILED, r, size, alignment, &event);
    dm_sa_adapter_event pages;
    int notify_pages = page_snapshot(i, r, &pages);
    drop(i);
    if (notify) event_notify(&event);
    if (notify_pages) event_notify(&pages);
    if (r != DM_SA_OK) return NULL;
    return out;
}
int dm_sa_adapter_allocate_if_managed(void *self, size_t size, size_t alignment, void **out) {
    if (!atomic_load_explicit(&active, memory_order_acquire)) return 0;
    instance *i = take(self, "Allocate");
    if (!i) return 0;
    *out = managed_allocate(i, size, alignment);
    return 1;
}
static void *allocate(void *self, size_t size) {
    instance *i = take(self, "Allocate");
    if (i) return managed_allocate(i, size, settings.allocation_alignment(self, size));
    return settings.forward_allocate(self, size);
}
static void *allocate_aligned(void *self, size_t size, size_t alignment) {
    instance *i = take(self, "Allocate(aligned)");
    if (i) return managed_allocate(i, size, alignment);
    return ORIGINAL(ALLOCATE_ALIGNED, void *(*)(void *, size_t, size_t))(self, size, alignment);
}
static void release_block(void *self, void *pointer) {
    instance *i = take(self, "Free");
    if (!i) { ORIGINAL(FREE, void (*)(void *, void *))(self, pointer); return; }
    dm_sa_result r = dm_sa_free(i->allocator, pointer);
    drop(i);
    if (r != DM_SA_OK) fail("Free", r);
}
static void *reallocate(void *self, void *pointer, size_t size) {
    instance *i = take(self, "Reallocate");
    if (!i) return ORIGINAL(REALLOCATE, void *(*)(void *, void *, size_t))(self,pointer,size);
    void *out = NULL;
    dm_sa_result r = !pointer && !size ? DM_SA_OK : size <= INT64_MAX ? dm_sa_reallocate(i->allocator, pointer, size, &out) : DM_SA_INVALID;
    dm_sa_adapter_event event;
    int notify = failure_snapshot(i, DM_SA_EVENT_REALLOCATE_FAILED, r, size, 0, &event);
    dm_sa_adapter_event pages;
    int notify_pages = page_snapshot(i, r, &pages);
    drop(i);
    if (notify) event_notify(&event);
    if (notify_pages) event_notify(&pages);
    return r == DM_SA_OK ? out : NULL;
}
static size_t shrink(void *self, void *pointer, size_t size) {
    instance *i = take(self, "Shrink");
    if (!i) return ORIGINAL(SHRINK, size_t (*)(void *, void *, size_t))(self,pointer,size);
    size_t out = 0;
    // SDK Shrink(p,0) keeps a live minimum-sized allocation, unlike Reallocate
    // (p,0). Core policy uses a 16-byte minimum rather than SDK size classes.
    dm_sa_result r = size <= INT64_MAX ? dm_sa_shrink(i->allocator, pointer, size ? size : 1, &out) : DM_SA_INVALID;
    dm_sa_adapter_event event;
    int notify = page_snapshot(i, r, &event);
    drop(i);
    if (notify) event_notify(&event);
    return r == DM_SA_OK ? out : 0;
}
static size_t get_size(const void *self, const void *pointer) {
    instance *i = take(self, "GetSizeOf");
    if (!i) return ORIGINAL(SIZE, size_t (*)(const void *, const void *))(self,pointer);
    size_t out = 0;
    dm_sa_result r = pointer ? dm_sa_get_size(i->allocator, pointer, &out) : DM_SA_OK;
    drop(i);
    if (r != DM_SA_OK) fail("GetSizeOf", r);
    return out;
}
static size_t stat_size(const void *self, int which) {
    instance *i = take(self, names[which]);
    if (!i) return ORIGINAL(which, size_t (*)(const void *))(self);
    dm_sa_stats stats;
    dm_sa_result r = dm_sa_get_stats(i->allocator, &stats);
    drop(i);
    if (r != DM_SA_OK) fail(names[which], r);
    return which == TOTAL_FREE ? stats.total_free : stats.largest_free;
}
static size_t total_free(const void *self) { return stat_size(self, TOTAL_FREE); }
static size_t allocatable(const void *self) { return stat_size(self, ALLOCATABLE); }
static void maintenance(const void *self, int which) {
    instance *i = take(self, names[which]);
    if (!i) { ORIGINAL(which, void (*)(const void *))(self); return; }
    // Managed mode has no thread-local cache, including when the SDK bool was
    // true. Wholly free real spans are released through their original owner.
    dm_sa_result r = dm_sa_trim(i->allocator);
    dm_sa_adapter_event event;
    int notify = page_snapshot(i, r, &event);
    drop(i);
    if (notify) event_notify(&event);
    if (r != DM_SA_OK) fail(names[which], r);
}
static void clear_cache(const void *self) { maintenance(self, CLEAR_CACHE); }
static void cleanup(const void *self) { maintenance(self, CLEANUP); }

typedef struct { void *pointer; size_t size; } pair;
typedef struct { pair *items; size_t count, capacity; int failed; } snapshot;
static int collect(void *pointer, size_t size, void *context) {
    snapshot *s = context;
    if (s->count == s->capacity) {
        size_t cap = s->capacity ? s->capacity * 2 : 32;
        if (cap < s->capacity || cap > SIZE_MAX / sizeof(pair)) { s->failed = 1; return 1; }
        pair *p = realloc(s->items, cap * sizeof *p);
        if (!p) { s->failed = 1; return 1; }
        s->items = p; s->capacity = cap;
    }
    s->items[s->count++] = (pair){pointer, size};
    return 0; // core convention; NOT the SDK callback convention
}
static int walk(const void *self, dm_sa_walk_fn callback, void *context) {
    instance *i = take(self, "WalkAllocatedBlocks");
    if (!i) return ORIGINAL(WALK, int (*)(const void *, dm_sa_walk_fn, void *))(self,callback,context);
    if (settings.reject_managed_walk) {
        drop(i); fail("WalkAllocatedBlocks is outside the reviewed experiment closure", DM_SA_INVALID);
    }
    if (!callback) { drop(i); return 22; }
    snapshot s = {0};
    uint64_t generation = i->generation;
    int callback_result = 0;
    dm_sa_result r = dm_sa_walk(i->allocator, collect, &s, &callback_result);
    drop(i);
    // SDK unlocks across callback invocation; snapshot also permits callbacks
    // to query/allocate/free/finalize without holding either host mutex. This
    // does NOT claim the SDK's behavior under concurrent mutation is proven.
    if (r != DM_SA_OK || s.failed) { free(s.items); return 12; }
    int result = s.count ? 0 : 2;
    for (size_t n = 0; n < s.count; n++) {
        // A previous callback may free a later snapshot entry or finalize the
        // whole instance. Do not pass an already-known stale block onward.
        pthread_mutex_lock(&mutex);
        instance *current = find(self);
        if (!current || !current->allocator || current->finalizing || current->generation != generation) {
            pthread_mutex_unlock(&mutex);
            break;
        }
        current->users++;
        pthread_mutex_unlock(&mutex);
        size_t live_size = 0;
        r = dm_sa_get_size(current->allocator, s.items[n].pointer, &live_size);
        drop(current);
        if (r == DM_SA_NOT_FOUND || r == DM_SA_INVALID) continue;
        if (r != DM_SA_OK) { result = 12; break; }
        if (!callback(s.items[n].pointer, live_size, context)) break;
    }
    free(s.items);
    return result;
}
static void dump(const void *self) {
    instance *i = take(self, "Dump");
    if (!i) { ORIGINAL(DUMP, void (*)(const void *))(self); return; }
    dm_sa_stats stats;
    dm_sa_result r = dm_sa_get_stats(i->allocator, &stats);
    drop(i);
    if (r != DM_SA_OK) fail("Dump", r);
    if (settings.dump) settings.dump(settings.context, self, &stats);
}
static dm_sa_hash hash(const void *self) {
    instance *i = take(self, "Hash");
    if (!i) return ORIGINAL(HASH, dm_sa_hash (*)(const void *))(self);
    dm_sa_hash out;
    dm_sa_result r = dm_sa_get_hash(i->allocator, &out);
    drop(i);
    if (r != DM_SA_OK) fail("Hash", r);
    return out; // AAPCS64 >16-byte aggregate uses the caller's x8 result address
}
static const dm_sa_guest_fn hooks[DM_SA_ADAPTER_SYMBOL_COUNT] = {
    (dm_sa_guest_fn)ctor1, (dm_sa_guest_fn)ctor2,
    (dm_sa_guest_fn)ctor_buffer1, (dm_sa_guest_fn)ctor_buffer2,
    (dm_sa_guest_fn)ctor_cache1, (dm_sa_guest_fn)ctor_cache2,
    (dm_sa_guest_fn)init, (dm_sa_guest_fn)init_cache, (dm_sa_guest_fn)finalize,
    (dm_sa_guest_fn)allocate, (dm_sa_guest_fn)allocate_aligned,
    (dm_sa_guest_fn)release_block, (dm_sa_guest_fn)reallocate, (dm_sa_guest_fn)shrink,
    (dm_sa_guest_fn)get_size, (dm_sa_guest_fn)total_free, (dm_sa_guest_fn)allocatable,
    (dm_sa_guest_fn)clear_cache, (dm_sa_guest_fn)cleanup, (dm_sa_guest_fn)walk,
    (dm_sa_guest_fn)dump, (dm_sa_guest_fn)hash
};
dm_sa_guest_fn dm_sa_adapter_lookup(const char *name) {
    if (!name || !atomic_load_explicit(&active, memory_order_acquire)) return NULL;
    for (size_t n = 0; n < DM_SA_ADAPTER_SYMBOL_COUNT; n++) if (!strcmp(name, names[n])) return hooks[n];
    return NULL;
}
dm_sa_result dm_sa_adapter_prepare(const dm_sa_adapter_config *config, dm_sa_adapter_prepared **out) {
    if (!out) return DM_SA_INVALID;
    *out = NULL;
#if !defined(DM_SA_ADAPTER_TESTING) && (!defined(DM_EXPERIMENTAL_SHARED_MEMORY) || !DM_EXPERIMENTAL_SHARED_MEMORY)
    (void)config;
    return DM_SA_INVALID; // No global production activation on incomplete evidence.
#else
    int authorized = 0;
#ifdef DM_SA_ADAPTER_TESTING
    authorized = config && config->proofs == DM_SA_PROOF_ALL;
#endif
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY
    authorized = config && !config->proofs && dm_memory_experiment_authorized(config->experimental_startup, config->context) && config->reject_managed_walk;
#endif
    if (!authorized || !config->make_config || !config->forward_allocate || !config->allocation_alignment ||
        !config->symbols || config->symbol_count != DM_SA_ADAPTER_SYMBOL_COUNT ||
        (!!config->retain_context != !!config->release_context)) return DM_SA_INVALID;
    for (size_t h = 0; h < DM_SA_ADAPTER_SYMBOL_COUNT; h++)
        if ((dm_sa_guest_fn)config->forward_allocate == hooks[h]) return DM_SA_INVALID;
    dm_sa_guest_fn resolved[DM_SA_ADAPTER_SYMBOL_COUNT] = {0};
    for (size_t n = 0; n < config->symbol_count; n++) {
        const dm_sa_adapter_symbol *s = &config->symbols[n];
        if (!s->name || !s->function) return DM_SA_INVALID;
        size_t index;
        for (index = 0; index < DM_SA_ADAPTER_SYMBOL_COUNT; index++) if (!strcmp(s->name,names[index])) break;
        if (index == DM_SA_ADAPTER_SYMBOL_COUNT || resolved[index]) return DM_SA_INVALID;
        for (size_t h = 0; h < DM_SA_ADAPTER_SYMBOL_COUNT; h++)
            if (s->function == hooks[h]) return DM_SA_INVALID; // no recursive original
        resolved[index] = s->function;
    }
    pthread_mutex_lock(&mutex);
    if (preparing || pending || atomic_load_explicit(&active, memory_order_relaxed)) {
        pthread_mutex_unlock(&mutex); return DM_SA_BUSY;
    }
    preparing = 1;
    pthread_mutex_unlock(&mutex);
    dm_sa_adapter_prepared *p = malloc(sizeof *p);
    dm_sa_result result = p ? DM_SA_OK : DM_SA_NO_MEMORY;
    if (p) {
        p->config = *config;
        p->config.symbols = NULL; // no borrowed caller census after preparation
        memcpy(p->originals, resolved, sizeof resolved);
        if (p->config.retain_context) result = p->config.retain_context(p->config.context);
    }
    pthread_mutex_lock(&mutex);
    preparing = 0;
    if (result == DM_SA_OK) { pending = p; *out = p; } else free(p);
    pthread_mutex_unlock(&mutex);
    return result;
#endif
}
dm_sa_guest_fn dm_sa_adapter_prepared_hook(const dm_sa_adapter_prepared *p, size_t index) {
    pthread_mutex_lock(&mutex);
    dm_sa_guest_fn hook = p && p == pending && index < DM_SA_ADAPTER_SYMBOL_COUNT ? hooks[index] : NULL;
    pthread_mutex_unlock(&mutex); return hook;
}
void dm_sa_adapter_publish(dm_sa_adapter_prepared *p) {
    pthread_mutex_lock(&mutex);
    if (!p || p != pending) abort();
    settings = p->config; memcpy(originals, p->originals, sizeof originals);
    pending = NULL; free(p);
    atomic_store_explicit(&active, 1, memory_order_release);
    pthread_mutex_unlock(&mutex);
}
void dm_sa_adapter_cancel(dm_sa_adapter_prepared *p) {
    pthread_mutex_lock(&mutex);
    if (!p || p != pending) abort();
    pending = NULL; preparing = 1;
    pthread_mutex_unlock(&mutex);
    if (p->config.release_context) p->config.release_context(p->config.context);
    free(p);
    pthread_mutex_lock(&mutex); preparing = 0; pthread_mutex_unlock(&mutex);
}
dm_sa_result dm_sa_adapter_shutdown(void) {
    pthread_mutex_lock(&mutex);
    // The bounded native policy has no stopped-guest teardown boundary. Its
    // opaque authorization persists in the immutable table until process exit.
    if (settings.experimental_startup) { pthread_mutex_unlock(&mutex); return DM_SA_BUSY; }
    if (!atomic_load_explicit(&active, memory_order_relaxed) || atomic_load_explicit(&stopped, memory_order_relaxed)) {
        pthread_mutex_unlock(&mutex); return DM_SA_INVALID;
    }
    for (instance *i = instances; i; i = i->next) if (i->allocator || i->users || i->initializing || i->finalizing) {
        pthread_mutex_unlock(&mutex); return DM_SA_BUSY;
    }
    atomic_store_explicit(&stopped, 1, memory_order_release);
    while (instances) { instance *i = instances; instances = i->next; free(i); }
    pthread_mutex_unlock(&mutex);
    if (settings.release_context) settings.release_context(settings.context);
    return DM_SA_OK;
}
dm_sa_result dm_sa_adapter_install(const dm_sa_adapter_config *config) {
    dm_sa_adapter_prepared *p = NULL;
    dm_sa_result r = dm_sa_adapter_prepare(config, &p);
    if (r == DM_SA_OK) dm_sa_adapter_publish(p);
    return r;
}
