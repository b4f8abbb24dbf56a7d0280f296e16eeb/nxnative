// Shared fault-injection check for the SVC ownership fixtures. Include after
// hle_svc.c with abort substituted by test_heap_abort. Only a real SetHeapSize
// fatal path with every protected state invariant intact may pass.
#include <signal.h>
#include <sys/wait.h>

static uint64_t heap_failure_regs[31];
static const uint64_t heap_failure_sentinel = UINT64_C(0xabcdeffe12345678);
static size_t heap_failure_size, heap_failure_previous, heap_failure_alias;
static size_t heap_failure_prepares, heap_failure_live_size;
static int heap_failure_maps, heap_failure_expected;
static unsigned char heap_failure_map_bytes[sizeof g_map];
static unsigned char heap_failure_live_bytes[0x4000];
static const void *heap_failure_live;
static volatile sig_atomic_t heap_failure_validated;

static void heap_failure_signal(int sig) {
    _exit(sig == SIGABRT && heap_failure_validated ? 77 : 78);
}
static _Noreturn void test_heap_abort(void) {
    if (heap_failure_expected) {
        if (errno != EACCES || atomic_load(&fail_protect) || atomic_load(&g_heap_cur) != heap_failure_previous ||
            heap_failure_regs[0] != heap_failure_sentinel ||
            heap_failure_regs[1] != heap_failure_size ||
            alias_mapped() != heap_failure_alias || g_nmap != heap_failure_maps ||
            memcmp(g_map, heap_failure_map_bytes, sizeof g_map) ||
            write_prepares != heap_failure_prepares ||
            memcmp(heap_failure_live, heap_failure_live_bytes, heap_failure_live_size) ||
            !dm_guest_heap_readable((uint64_t)g_heap, heap_failure_previous) ||
            dm_guest_heap_readable((uint64_t)g_heap, heap_failure_previous + 1)) _exit(78);
        heap_failure_validated = 1;
    }
    abort();
}
static void expect_heap_failure(size_t size, const void *live, size_t live_size) {
    assert(live_size && live_size <= sizeof heap_failure_live_bytes);
    heap_failure_size = size; heap_failure_previous = atomic_load(&g_heap_cur);
    heap_failure_alias = alias_mapped(); heap_failure_maps = g_nmap;
    heap_failure_prepares = write_prepares;
    heap_failure_live = live; heap_failure_live_size = live_size;
    memcpy(heap_failure_map_bytes, g_map, sizeof g_map);
    memcpy(heap_failure_live_bytes, live, live_size);
    memset(heap_failure_regs, 0, sizeof heap_failure_regs);
    heap_failure_regs[0] = heap_failure_sentinel; heap_failure_regs[1] = size;
    fflush(NULL);
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        signal(SIGABRT, heap_failure_signal); alarm(5);
        heap_failure_expected = 1; atomic_store(&fail_protect, 1);
        svc_dispatch(1, heap_failure_regs, 0);
        _exit(79); // Returning to the guest after backend failure is forbidden.
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
}
