// Fatal host-protection policy: assert that the exact native memory state is
// unchanged at SIGABRT. Run before concurrent workers; child changes are isolated.
#include <signal.h>
#include <sys/wait.h>

static struct {
    size_t heap, mapped, bits_size, exposed_size, live_size;
    int maps;
    const void *live;
    unsigned char map_bytes[sizeof g_map];
    unsigned char *bits, *exposed, *live_bytes;
} memory_failure;
static inline void memory_failure_signal(int signal_number) {
    if (signal_number != SIGABRT) _exit(81);
    // Signal/sanitizer trampolines may change errno; consuming the injected
    // protection failure proves this reached the fatal backend path.
    if (atomic_load(&fail_protect)) _exit(83);
    if (atomic_load(&g_heap_cur) != memory_failure.heap) _exit(84);
    if (alias_mapped() != memory_failure.mapped) _exit(85);
    if (g_nmap != memory_failure.maps || memcmp(g_map, memory_failure.map_bytes, sizeof g_map)) _exit(86);
    if (memcmp((const void *)g_alias_bits, memory_failure.bits, memory_failure.bits_size)) _exit(87);
    if (memcmp(g_alias_exposed, memory_failure.exposed, memory_failure.exposed_size)) _exit(88);
    if (memory_failure.live_size && memcmp(memory_failure.live, memory_failure.live_bytes,
                                          memory_failure.live_size)) _exit(89);
    _exit(77);
}
static inline pid_t memory_failure_begin(const void *live, size_t size) {
    memory_failure.heap = atomic_load(&g_heap_cur); memory_failure.mapped = alias_mapped();
    memory_failure.maps = g_nmap; memory_failure.live = live; memory_failure.live_size = size;
    memory_failure.bits_size = ((g_alias_cap >> 18) + 1) * sizeof *g_alias_bits;
#ifdef DM_TEST_HOST_PAGE_16K
    size_t native_page = 0x4000;
#else
    size_t native_page = (size_t)getpagesize();
#endif
    memory_failure.exposed_size = (g_alias_cap / native_page / 64 + 1) * sizeof *g_alias_exposed;
    memory_failure.bits = malloc(memory_failure.bits_size);
    memory_failure.exposed = malloc(memory_failure.exposed_size);
    memory_failure.live_bytes = size ? malloc(size) : NULL;
    assert(memory_failure.bits && memory_failure.exposed && (!size || memory_failure.live_bytes));
    memcpy(memory_failure.map_bytes, g_map, sizeof g_map);
    memcpy(memory_failure.bits, (const void *)g_alias_bits, memory_failure.bits_size);
    memcpy(memory_failure.exposed, g_alias_exposed, memory_failure.exposed_size);
    if (size) memcpy(memory_failure.live_bytes, live, size);
    fflush(NULL); pid_t child = fork(); assert(child >= 0);
    if (!child) {
        signal(SIGABRT, memory_failure_signal); alarm(5);
        errno = 0; atomic_store(&fail_protect, 1);
    }
    return child;
}
static inline void memory_failure_end(pid_t child) {
    int status; assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 77)
        fprintf(stderr, "host-protection failure did not preserve state and abort (status=%d)\n", status);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
    free(memory_failure.bits); free(memory_failure.exposed); free(memory_failure.live_bytes);
}
#define EXPECT_MEMORY_PROTECTION_FAILURE(action, live, size) do { \
    pid_t child_ = memory_failure_begin((live), (size)); \
    if (!child_) { (void)(action); _exit(79); } \
    memory_failure_end(child_); \
} while (0)
