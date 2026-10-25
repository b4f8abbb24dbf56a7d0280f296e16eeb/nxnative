// The real experimental policy, adapters, transaction, owner/provider and SVC
// backend. Only the loader resolver, guest originals and VM host-page harness
// are synthetic. No DM_*_TESTING activation flags and no private code execute.
#define MEMORY_STARTUP_MAIN memory_startup_fixture_main
#include "t_memory_runtime_startup.c"
#undef MEMORY_STARTUP_MAIN
#include "../hle_memory_experiment.h"

static unsigned creates, destroys, installs;
static int fail_create, fail_configs;
static dm_sa_result track_create(dm_memory_runtime **out, size_t max_regions) {
    ++creates;
    if (fail_create) return DM_SA_NO_MEMORY;
    dm_sa_result result = dm_memory_runtime_create(out, max_regions);
    if (result == DM_SA_OK) startup_runtime = *out;
    return result;
}
static dm_sa_result track_configs(dm_memory_runtime *r, dm_vamm_adapter_config *v, dm_sa_adapter_config *a) {
    if (fail_configs) return DM_SA_INVALID;
    return dm_memory_runtime_configs(r, v, a);
}
static dm_sa_result track_destroy(dm_memory_runtime *r) {
    ++destroys;
    return dm_memory_runtime_destroy(r);
}
int loader_install_memory_hooks(const dm_sa_adapter_config *a, const dm_vamm_adapter_config *v) {
    ++installs;
    assert(!a->proofs && !v->proofs); // native opt-in is NOT synthetic PROOF_ALL
    assert(a->reject_managed_walk && a->dump && a->event && a->fatal && v->fatal);
    assert(dm_memory_experiment_authorized(a->experimental_startup, a->context));
    assert(dm_memory_experiment_authorized(v->experimental_startup, v->context));
    startup_allocator = *a; startup_vamm = *v;
    return dm_memory_hooks_install(g_mod, (size_t)g_nmod, a, v, startup_resolve);
}
int loader_install_vamm_hooks(const dm_vamm_adapter_config *v) {
    (void)v; assert(!"full-group test must not select SDK-only startup"); return DM_MEMORY_HOOKS_INVALID;
}
#ifdef MEMORY_EXPERIMENT_NO_ALIGNED_LEASES
#undef DM_MEMORY_RUNTIME_ALIGNED_LEASES
#endif
#define dm_memory_runtime_create track_create
#define dm_memory_runtime_destroy track_destroy
#define dm_memory_runtime_configs track_configs
#include "../hle_memory_experiment.c"
#undef dm_memory_runtime_create
#undef dm_memory_runtime_destroy
#undef dm_memory_runtime_configs

static void identities(void) {
    static const char *ids[] = {
        "7222e13ecf6adb3249b5dd5c57fa40b102245fce000000000000000000000000",
        "df7a3960c74c3a4803f826c5871532523aa9cf2a000000000000000000000000"};
    for (unsigned k = 0; k < 2; ++k) {
        g_mod[k] = startup_modules[k];
        memcpy(g_mod[k].build_id, ids[k], 32); g_mod[k].build_id[32] = 0;
        strcpy(g_mod[k].full_build_id, ids[k]);
    }
    g_nmod = 2;
}
static void unreviewed_sdk(void) {
    // Same inspected SDK-side contracts do not authorize a different whole
    // game closure. In particular, SDK20.5.8 is not another experiment target.
    const char *id = "e01950c58e17b080c90494784ea1dd29e9dbd486000000000000000000000000";
    strcpy(g_mod[1].full_build_id, id);
    memcpy(g_mod[1].build_id, id, 32); g_mod[1].build_id[32] = 0;
}
static void preflight(void) {
    identities();
    assert(!dm_memory_experiment_preflight(g_mod, 2));
    unreviewed_sdk(); assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
    assert(dm_memory_experiment_preflight(NULL, 2));
    assert(dm_memory_experiment_preflight(g_mod, 1));
    assert(dm_memory_experiment_preflight(g_mod, 3));
    module_t saved = g_mod[0]; g_mod[0] = g_mod[1]; g_mod[1] = saved;
    const char *order_error = dm_memory_experiment_preflight(g_mod, 2);
    assert(order_error && strstr(order_error, "main then sdk")); identities();
    g_mod[1] = g_mod[0]; assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
    strcpy(g_mod[1].name, "subsdk0"); assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
    g_mod[1].nro = 1; assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
    g_mod[1].loaded = 0; assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
    g_mod[0].loads = 1; assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
    g_mod[1].base = NULL; assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
    g_mod[1].size = 0; assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
    memset(g_mod[1].name, 'a', sizeof g_mod[1].name);
    assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
    for (unsigned k = 0; k < 2; ++k) {
        // A different tail with the identical old 16-byte identity MUST fail.
        g_mod[k].full_build_id[63] = '1'; assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
        g_mod[k].full_build_id[0] = 0; assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
        g_mod[k].full_build_id[64] = 'a'; assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
        g_mod[k].build_id[0] = '0'; assert(dm_memory_experiment_preflight(g_mod, 2)); identities();
    }
}
// Capture the real experiment logger. Read before waiting so even a verbose
// child cannot deadlock on a full pipe. Expected fatal paths run only in forks.
static int child_log(void (*work)(void *), void *context, char *log, size_t capacity) {
    int fds[2]; assert(capacity > 1 && !pipe(fds));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(fds[0]); assert(dup2(fds[1], STDERR_FILENO) == STDERR_FILENO); close(fds[1]);
        work(context); _exit(0);
    }
    close(fds[1]); size_t used = 0;
    for (;;) {
        assert(used < capacity - 1);
        ssize_t n = read(fds[0], log + used, capacity - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        assert(n >= 0);
        if (!n) break;
        used += (size_t)n;
    }
    log[used] = 0; close(fds[0]);
    int status; assert(waitpid(child, &status, 0) == child);
    return status;
}
static void initialize_before_owner(void *unused) {
    (void)unused;
    unsigned char self[DM_SA_ADAPTER_OBJECT_SIZE] = {0}; CONSTRUCT(self);
    S(7, void (*)(void *, void *, size_t, unsigned))(self, NULL, (size_t)12 << 30, 1);
}
static void finalize_borrowed(void *self) { FINISH(self); }
static uint64_t experiment_svc(unsigned svc, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t regs[31] = {a, b, c}; svc_dispatch(svc, regs, 0); return regs[0];
}
static uintptr_t experiment_stack(void) {
    uint64_t regs[31] = {0, I_StackAddr}; svc_dispatch(0x29, regs, 0);
    assert(!regs[0]); return regs[1];
}
static void assert_abort(int status, const char *log) {
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) fputs(log, stderr);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
static size_t occurrences(const char *log, const char *text) {
    size_t count = 0, length = strlen(text);
    for (const char *p = log; (p = strstr(p, text)); p += length) ++count;
    return count;
}
static void assert_lifecycle_log(const char *log) {
    const char *line = strstr(log, "experimental shared memory managed Initialize success ");
    assert(line && occurrences(log, "managed Initialize success ") == 1);
    void *self; unsigned long long generation, failures;
    size_t requested, alignment; int result;
    assert(sscanf(line, "experimental shared memory managed Initialize success self=%p generation=%llu requested=%zu alignment=%zu result=%d failure_count=%llu",
        &self, &generation, &requested, &alignment, &result, &failures) == 6);
    assert(self && generation && requested == ((size_t)12 << 30) && !alignment && result == DM_SA_OK && !failures);
    // This is the first snapshot, immediately following successful Initialize:
    // nominal 12 GiB was preserved, but no backing was acquired and free/largest
    // come from the bounded physical/native budget rather than that capacity.
    line = strstr(line, "experimental shared memory snapshot "); assert(line);
    void *snapshot_self; unsigned long long snapshot_generation;
    size_t capacity, backing, live, blocks, spans, available, largest, metadata; int closed;
    assert(sscanf(line, "experimental shared memory snapshot self=%p generation=%llu capacity=%zu backing=%zu live=%zu blocks=%zu spans=%zu free=%zu largest=%zu metadata=%zu closed=%d",
        &snapshot_self, &snapshot_generation, &capacity, &backing, &live, &blocks, &spans,
        &available, &largest, &metadata, &closed) == 11);
    assert(snapshot_self == self && snapshot_generation == generation && capacity == requested);
    assert(!backing && !live && !blocks && !spans && !closed && metadata);
    assert(available && available <= APP_POOL - DM_GAME_MEM_USED && available < capacity);
    assert(largest && largest <= available);
    // Zero-size Allocate and Reallocate(NULL,0) are not failures. Eighteen real
    // budget failures produce only 1,2,3,4,8,16 across Allocate/Reallocate.
    static const unsigned counts[] = {1, 2, 3, 4, 8, 16};
    const char *scan = log;
    for (unsigned i = 0; i < sizeof counts / sizeof *counts; ++i) {
        scan = strstr(scan, " failure_count="); assert(scan);
        // Skip the one initialization event's zero counter.
        if (!i) { scan = strstr(scan + 1, " failure_count="); assert(scan); }
        assert(sscanf(scan, " failure_count=%llu", &failures) == 1 && failures == counts[i]);
        scan++;
    }
    assert(occurrences(log, "managed Allocate failure ") == 2);
    assert(occurrences(log, "managed Reallocate failure ") == 4);
    assert(occurrences(log, "experimental shared memory snapshot ") == 7);
    line = strstr(log, "experimental shared memory managed Finalize success "); assert(line);
    assert(sscanf(line, "experimental shared memory managed Finalize success self=%p generation=%llu requested=%zu alignment=%zu result=%d failure_count=%llu",
        &snapshot_self, &snapshot_generation, &requested, &alignment, &result, &failures) == 6);
    assert(snapshot_self == self && snapshot_generation == generation && requested == capacity && !alignment && result == DM_SA_OK && !failures);
    assert(occurrences(log, "managed Finalize success ") == 1 && !strstr(log, "managed Finalize retained "));
}
static int unexpected_callback(void *p, size_t n, void *context) {
    (void)p; (void)n; (void)context;
    _exit(88); // Anything but SIGABRT proves the forbidden callback ran.
}
static void active_calls(void) {
    assert(!dm_memory_experiment_authorized(startup_allocator.experimental_startup, startup_runtime));
    assert(dm_memory_hooks_enter_guest()); // startup completed, guest may now enter
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    unsigned char manager[64] = {0}, self[DM_SA_ADAPTER_OBJECT_SIZE] = {0};
    V(8, void (*)(void *))(manager);
    char rejected[8192];
    int rejected_status = child_log(initialize_before_owner, NULL, rejected, sizeof rejected);
    assert_abort(rejected_status, rejected);
    assert(strstr(rejected, "StandardAllocator Initialize failed"));
    assert(!strstr(rejected, "managed Initialize success") && !strstr(rejected, "experimental shared memory snapshot"));
    V(10, void (*)(void *))(manager);
    CONSTRUCT(self);
    S(7, void (*)(void *, void *, size_t, unsigned))(self, NULL, (size_t)12 << 30, 1);
    assert(self[0] && dm_sa_adapter_is_managed(self) && !alias_mapped());
    assert(!ALLOC(self, 0));
    assert(!S(12, void *(*)(void *, void *, size_t))(self, NULL, 0));
    void *p = S(10, void *(*)(void *, size_t, size_t))(self, SMALL, 0x400000);
    assert(p && !((uintptr_t)p & (0x400000 - 1)) && alias_mapped() == SMALL);
    memset(p, 0x37, SMALL);
    for (unsigned i = 1; i <= 18; ++i) {
        void *failed = i & 1 ? ALLOC(self, APP_POOL) :
            S(12, void *(*)(void *, void *, size_t))(self, p, APP_POOL);
        assert(!failed && alias_mapped() == SMALL);
        assert(S(14, size_t (*)(const void *, const void *))(self, p) == SMALL);
        assert(*(unsigned char *)p == 0x37 && ((unsigned char *)p)[SMALL - 1] == 0x37);
    }
    // A retained Finalize event must precede the fatal contract diagnostic;
    // no success event or SDK handoff is permitted while the lease is borrowed.
    uintptr_t stack = experiment_stack();
    assert(!experiment_svc(4, stack, (uintptr_t)p, PAGE));
    rejected_status = child_log(finalize_borrowed, self, rejected, sizeof rejected);
    assert_abort(rejected_status, rejected);
    const char *retained = strstr(rejected, "managed Finalize retained ");
    const char *fatal_line = strstr(rejected, "StandardAllocator Finalize failed");
    assert(retained && fatal_line && retained < fatal_line);
    const char *retained_result = strstr(retained, " result="); int retained_code = -1;
    assert(retained_result && sscanf(retained_result, " result=%d", &retained_code) == 1 && retained_code == DM_SA_BUSY);
    assert(strstr(retained, " failure_count=0") && strstr(retained, "experimental shared memory snapshot "));
    assert(!strstr(rejected, "managed Finalize success "));
    assert(self[0] && dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    assert(!experiment_svc(5, stack, (uintptr_t)p, PAGE));
    assert(alias_mapped() == SMALL && *(unsigned char *)p == 0x37);
    dm_sa_hash h = S(21, dm_sa_hash (*)(const void *))(self);
    assert(h.block_count == 1 && h.allocated_bytes == SMALL);
    S(20, void (*)(const void *))(self); // meaningful native budget diagnostic
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        S(19, int (*)(const void *, dm_sa_walk_fn, void *))(self, unexpected_callback, NULL);
        _exit(89);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(*(unsigned char *)p == 0x37);
    FREE(self, p); FINISH(self);
    assert(!alias_mapped() && !atomic_load(&forwards));
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    assert(dm_sa_adapter_shutdown() == DM_SA_BUSY);
    assert(dm_vamm_adapter_shutdown() == DM_VAMM_INVALID);
    assert(dm_memory_hooks_active()); // process-lifetime owner, never SDK restore
    const char *why = NULL;
    assert(dm_memory_experiment_startup("0", &why) == DM_MEMORY_EXPERIMENT_FAILED && why);
    assert(dm_memory_hooks_active() && !destroys);
}
static void scenario(unsigned which) {
    identities();
    if (which != 17) assert(dm_memory_hooks_open_startup());
    const char *why = NULL, *request = DM_MEMORY_EXPERIMENT_NAME;
    if (which == 0) request = NULL;
    if (which == 1) request = "0";
    if (which == 2) request = "";
    if (which == 3) request = "1";
    if (which == 4) g_mod[0].full_build_id[63] = '1';
    if (which == 5) g_nmod = 1;
    if (which == 6) g_nmod = 3;
    if (which == 7) g_mod[1].nro = 1;
    if (which == 8) g_mod[1] = g_mod[0];
    if (which == 9) g_mod[1].full_build_id[0] = 0;
    if (which == 10) startup_images[1]->slots[STARTUP_SYMBOLS - 1] ^= 8;
    if (which == 11) startup_images[1]->rela[STARTUP_SYMBOLS - 1].r_info = 999;
    if (which == 12) startup_images[1]->symbols[1].st_shndx = 0;
    if (which == 13) assert(dm_memory_hooks_enter_guest());
    if (which == 15) fail_create = 1;
    if (which == 16) fail_configs = 1;
    if (which == 18) { module_t saved = g_mod[0]; g_mod[0] = g_mod[1]; g_mod[1] = saved; }
    if (which == 19) unreviewed_sdk();
    int result = dm_memory_experiment_startup(request, &why);
    if (which < 3) {
        assert(result == DM_MEMORY_EXPERIMENT_OFF && !why && !creates && !installs && !destroys);
    } else {
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY && defined(DM_MEMORY_RUNTIME_ALIGNED_LEASES)
        if (which == 14) {
            assert(result == DM_MEMORY_EXPERIMENT_ACTIVE && !why && creates == 1 && installs == 1 && !destroys);
            active_calls(); return;
        }
        if (which == 19) assert(!creates && !installs && !destroys && why && strstr(why, "full NSO build ID"));
        else if (which == 18) assert(!creates && !installs && !destroys && why && strstr(why, "main then sdk"));
        else if (which == 15) assert(creates == 1 && !installs && !destroys);
        else if (which == 16) assert(creates == 1 && !installs && destroys == 1);
        else if (which >= 10) assert(creates == 1 && installs == 1 && destroys == 1);
        else assert(!creates && !installs && !destroys);
#else
        assert(!creates && !installs && !destroys);
#endif
        assert(result == DM_MEMORY_EXPERIMENT_FAILED && why && *why);
    }
    assert(!dm_memory_hooks_active() && !alias_mapped() && !atomic_load(&constructors));
    for (unsigned k = 0; k < 2; ++k) for (unsigned i = 0; i < STARTUP_SYMBOLS; ++i) {
        uint64_t expected = original_slots[k][i];
        if (which == 10 && k == 1 && i == STARTUP_SYMBOLS - 1) expected ^= 8;
        assert(startup_images[k]->slots[i] == expected);
    }
}
static void run_scenario(void *which) { scenario(*(unsigned *)which); }
int main(void) {
    struct rlimit limit = {0, 0}; assert(!setrlimit(RLIMIT_CORE, &limit));
    startup_images_create(); preflight();
    for (unsigned i = 0; i <= 19; ++i) {
        char log[32768];
        int status = child_log(run_scenario, &i, log, sizeof log);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) fputs(log, stderr);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY && defined(DM_MEMORY_RUNTIME_ALIGNED_LEASES)
        if (i == 14) assert_lifecycle_log(log);
        else
#endif
            assert(!strstr(log, "experimental shared memory managed ") &&
                   !strstr(log, "experimental shared memory snapshot "));
    }
    for (unsigned k = 0; k < 2; ++k) assert(!munmap(startup_images[k], startup_image_bytes));
    puts("bounded experiment: identity, off/on, prepublication cleanup, lifecycle/budget/failure diagnostics, managed Hash/Dump/Walk policy: PASS");
    return 0;
}
