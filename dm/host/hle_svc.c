// Horizon kernel system calls (triggered by the SDK runtime once the SDK is loaded). Only what the P1 boot chain needs is implemented: memory layout + heap + query + debug output + timing.
// Register convention: inputs w1.. / outputs w0=Result, x1=value (GetInfo / SetHeapSize), consistent with the public Horizon ABI.
#include "dm.h"
#include "hle_address_space.h"
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <stdatomic.h>

// Reserve one whole block of guest address space, partitioned the Horizon way (address values are the real addresses returned by the host's mmap).
// It only takes address space, not memory.
// Size 5 GB: the game needs the first 1 GB (alias region, stacks) + one whole heap block (application memory pool of 3285 MiB, see APP_POOL below),
// about 4.2 GB in total; on iOS without the "extended virtual addressing" entitlement a single reservation can be at most 6 GB
// (read on a real device: 6 GB succeeds, 7 GB fails), so it cannot be made any wider --
// with an 8 GB reservation, builds from developer accounts that cannot sign that entitlement (free accounts) would exit at this step.
#define RESERVE DM_GAME_MEM_RESERVE   // SMBW: the heap needs only 1.47 GB, so the block stays at 5 GB (the total proven on a real device; a single 6 GB block can be reserved, but Metal etc. still need address space)
static uint8_t *g_as;              // ASLR region base = the whole block
static uint8_t *g_heap; static _Atomic size_t g_heap_cur; static size_t g_heap_cap;
// The address range lies entirely within the currently opened (readable and writable) part of the guest heap: such reads need no kernel trap to check, memcpy directly (used where guest memory is read every tick, such as diag.c tk_rd)
int dm_guest_heap_readable(uint64_t a, size_t n) { uint64_t b = (uint64_t)(uintptr_t)g_heap; size_t cur = g_heap_cur; return g_heap && a >= b && n <= cur && a - b <= cur - n; }
static uint8_t *g_alias; static size_t g_alias_cap;
static dm_address_space *g_alias_space;
// Serializes heap changes with physical maps and their application-pool checks.
// Lock order: g_map_lock (source ownership), g_phys_mu, address-space lock.
static pthread_mutex_t g_phys_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_stack; static size_t g_stack_cap;
// The address lies in the guest address block (every guest thread stack is here: the game's own heap or the stack region); the big-stack entry (hle_bigstack.c) uses it to decide whether to switch stacks
int dm_guest_as_contains(uintptr_t a) { uintptr_t b = (uintptr_t)g_as; return g_as && a >= b && a - b < RESERVE; }

static void *alias_windows_thread(void *arg);
// The manager supplies host-page aligned ranges. Revoke access before advising
// discard: a failed mprotect must not discard live data or change accounting.
static int alias_apply(void *context, uintptr_t base, size_t size, int commit) {
    (void)context;
    if (mprotect((void *)base, size, commit ? PROT_READ | PROT_WRITE : PROT_NONE)) return -1;
    // Opening a host page may also release GPU dirty-tracker protection on a
    // live neighbor. Notify it only after mprotect succeeds, for the full range
    // whose protection changed; clearing below still touches only logical holes.
    if (commit) dm_guest_write_prepare((void *)base, size);
    else (void)madvise((void *)base, size, MADV_FREE);
    return 0;
}
static void alias_initialize(void *context, uintptr_t base, size_t size) {
    (void)context;
    memset((void *)base, 0, size);
}
static size_t alias_mapped(void) {
    dm_as_stats stats;
    if (dm_as_get_stats(g_alias_space, &stats) != DM_AS_OK) die("alias address-space statistics");
    return stats.committed_bytes;
}
static void mem_init_once(void) {
    size_t pg = (size_t)getpagesize();
    if (RESERVE < 0x10000000 || DM_GAME_MEM_ALIAS > RESERVE - 0x10000000 ||
        DM_GAME_MEM_STACK > RESERVE - 0x10000000 - DM_GAME_MEM_ALIAS ||
        ((RESERVE | DM_GAME_MEM_ALIAS | DM_GAME_MEM_STACK | 0x10000000) & (pg - 1)))
        die("guest address-space profile has invalid native boundaries");
    g_as = mmap(NULL, RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_as == MAP_FAILED) die("reserve guest AS");
    // Fixed partition (all inside the reserved block): alias 2.875 GB, stack 256 MB, heap the remaining 1.625 GB.
    // In newer SDKs the AddressRegionManager always takes 0x3fe00000 (~1 GB) at the start of the alias region as its node area (sdk 0x134728) before handing out regions; with a 512 MB alias region the very first AllocateAddressRegion fails (0x1803).
    // SMBW also defines a 1792000 KB (1.75 GB) VirtualAddressMemory in HeapDef (System/HeapSize): it is subtracted from the memory pool before the heap is opened (SetHeapSize asks for only 0x5be00000),
    //   and resource loading then MapPhysicalMemory's into the alias region after the node area on demand. With a 1.25 GB alias region only about 256 MB was left there, and 3 MB-class layout archives (WipeCourseIn / WipeWarp)
    //   got no address, were skipped, and crashed on a null pointer when used (main +0x373e04). So alias = node area + 1.75 GB + slack; the heap needs only 1.47 GB, and 2.625 GB is plenty.
    // The alias and stack sizes come from the game profile (mem_alias / mem_stack), the heap takes the rest; Mario: alias 0xB8000000, stack 0x10000000 → stack at +0xC8000000, heap at +0xD8000000
    g_alias = g_as + 0x10000000; g_alias_cap = DM_GAME_MEM_ALIAS;
    g_stack = g_alias + g_alias_cap; g_stack_cap = DM_GAME_MEM_STACK;
    g_heap  = g_stack + g_stack_cap; g_heap_cap  = RESERVE - (size_t)(g_heap - g_as);
    // The SDK retains EVERY reservation path and its existing patches. Host
    // reservations are rejected in this mode; only known SVC commit paths share
    // the manager. Do not enable a second allocator through partial symbol hooks.
    dm_as_config config = { .base = (uintptr_t)g_alias, .size = g_alias_cap,
        .guest_page_size = 0x1000, .host_page_size = pg, .max_regions = 0,
        .mode = DM_AS_EXTERNAL_RESERVATIONS, .apply = alias_apply, .initialize = alias_initialize,
        .initially_zero = 1 }; // a slice of the fresh anonymous PROT_NONE mmap
    if (dm_as_create(&g_alias_space, &config) != DM_AS_OK) die("alias address-space manager allocation");
    LOG("guest AS %p +%#llx  heap %p alias %p stack %p", g_as, (unsigned long long)RESERVE, g_heap, g_alias, g_stack);
    const char *aw = getenv("DM_ALIAS_WINDOWS");
    if (aw && atoi(aw) > 0) { pthread_t th; if (!pthread_create(&th, NULL, alias_windows_thread, (void *)(uintptr_t)atoi(aw))) pthread_detach(th); }
}
static void mem_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, mem_init_once);
}

// Application memory pool = the 3285 MB the Switch (4 GB model) gives applications. At startup (main+0x1b66160) the game sets the whole total reported by QueryMemoryInfo as the heap, retrying with 2 MB less on failure,
// and hands the whole block to its own StandardAllocator (main+0x32137a0) -- so heap size = pool − image footprint, it has only as much as the host reports.
// Previously 3 GB was reported here and SetHeapSize never failed: the heap was about 150 MB smaller than on the real device, at the run entry peak the main heap had only about 18 MB left, and under high local load a timing shift made a 128 KB request return null,
// which the guest writes to without a null check (CRASH main+0x1b36138, re-verified after merge with inv_tap).
#define APP_POOL DM_GAME_APP_POOL   // game profile app_pool (Mario 0xCD500000)
static size_t image_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) if (!g_mod[i].nro) n += g_mod[i].size;   // an NRO lives in memory the game allocated (its image and bss buffers)
    return (n + 0x1fffff) & ~(size_t)0x1fffff; }
// Giant bss (modules with the patch area before the base, patch_nso.py FAR): the image bytes beyond the file part. Used memory was reported as "heap + 64 MB", where the 64 MB covers ordinary modules' code and data (Mario's two modules are about 74 MB together,
// and the heap it opens = pool − 64 MB − 1.75 GB virtual address memory − 1 MB = 0x5be00000, as on hardware); Metroid Dread's main has about 1.8 GB of bss, the engine's static heap, which the Switch counts as used;
// without it the game asks for pool − 64 MB = 3.15 GB of heap
static size_t big_bss_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) if (g_mod[i].patch_off < 0) n += g_mod[i].size - g_mod[i].file_image_len; return n; }

// For memory accounting (memstat.m): the range of the whole guest address space block (base = NULL when not initialized)
void svc_guest_range(uint8_t **base, size_t *size) { *base = g_as; *size = RESERVE; }

enum { I_CoreMask=0, I_PriorityMask=1, I_AliasAddr=2, I_AliasSize=3, I_HeapAddr=4, I_HeapSize=5,
       I_TotalMem=6, I_UsedMem=7, I_DebuggerAttached=8, I_ResourceLimit=9, I_IdleTick=10, I_RandomEntropy=11,
       I_AslrAddr=12, I_AslrSize=13, I_StackAddr=14, I_StackSize=15, I_SysResTotal=16, I_SysResUsed=17,
       I_ProgramId=18, I_InitProcIdRange=19, I_UserExcAddr=20, I_TotalNonSys=21, I_UsedNonSys=22, I_IsApplication=23, I_FreeThreadCount=24 };

static uint8_t g_seen[256];

// Registry of MapMemory stacks and their exclusively borrowed source intervals.
// Physical alias pages live in g_alias_space; QueryMemory consults both.
// g_map_lock is held over a whole lookup + check + change (and over QueryMemory's reads): map_find's index is only valid under it, since map_del moves the last entry into the freed slot.
// The lock used to cover only the add and the delete, so two threads unmapping at once (stacks of exiting threads) could delete through a stale index and drop another live mapping
#define MAX_MAP 4096
static struct { uint64_t base, size, src; uint32_t state; } g_map[MAX_MAP];
static int g_nmap;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static int map_find(uint64_t a, uint64_t n) {       // returns a 1-based index, 0 = no overlap; caller holds g_map_lock
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1;
    return 0;
}
static void map_add(uint64_t b, uint64_t n, uint64_t src, uint32_t st) {   // caller holds g_map_lock
    if (g_nmap >= MAX_MAP) die("mapping table full");
    g_map[g_nmap++] = (typeof(g_map[0])){b, n, src, st};
}
static void map_del(int i) { g_map[i] = g_map[--g_nmap]; }   // caller holds g_map_lock

// All mapping intervals are nonempty and nonwrapping before reaching these
// helpers. Integer subtraction also keeps malformed guest pointers out of C
// pointer arithmetic. Public MapMemory / UnmapMemory checks share their order.
static int memory_contains(uint64_t base, uint64_t capacity, uint64_t addr, uint64_t size) {
    return addr >= base && size <= capacity && addr - base <= capacity - size;
}
static uint32_t map_arguments(uint64_t dst, uint64_t src, uint64_t size) {
    if ((dst | src) & 0xfff) return 0xCC01;  // InvalidAddress
    if (!size || (size & 0xfff)) return 0xCA01;  // InvalidSize
    if (size > UINT64_MAX - src || size > UINT64_MAX - dst) return 0xD401;
    // Modules are separate native mappings outside the reserved guest block.
    if (!memory_contains((uintptr_t)g_as, RESERVE, src, size)) {
        module_t *m = module_of((void *)(uintptr_t)src);
        if (!m || !memory_contains((uintptr_t)m->base, m->size, src, size)) return 0xD401;
    }
    return memory_contains((uintptr_t)g_stack, g_stack_cap, dst, size) ? 0 : 0xDC01;
}
static int map_source_busy(uint64_t addr, uint64_t size) { // g_map_lock held
    for (int i = 0; i < g_nmap; ++i)
        if (addr < g_map[i].src + g_map[i].size && g_map[i].src < addr + size) return 1;
    return 0;
}
// Clip a module-data query to one host protection run. Loader metadata is
// immutable after startup; its text/patch seals use fixed 16 KiB boundaries on
// both Mac and iOS, even when this portable test process has 4 KiB host pages.
static void module_readonly_run(uint64_t off, uint64_t start, uint64_t end,
                                uint64_t *lo, uint64_t *hi, uint64_t *perm) {
    if (start >= end) return;
    if (off < start) { if (start < *hi) *hi = start; }
    else if (off >= end) { if (end > *lo) *lo = end; }
    else {
        if (start > *lo) *lo = start;
        if (end < *hi) *hi = end;
        *perm &= ~UINT64_C(2); // host RX/R pages cannot be writable CodeData
    }
}
static void module_data_query(const module_t *m, uint64_t off, uint64_t *lo, uint64_t *hi, uint64_t *perm) {
    uint64_t text = m->text_end <= UINT64_MAX - 0x3fff ? (m->text_end + 0x3fff) & ~UINT64_C(0x3fff) : m->size;
    module_readonly_run(off, 0, text, lo, hi, perm);
    if (m->patch_off > 0)
        module_readonly_run(off, (uint64_t)m->patch_off & ~UINT64_C(0x3fff), m->size, lo, hi, perm);
    if (m->key_page_size && m->key_page_off <= m->size && m->key_page_size <= m->size - m->key_page_off)
        module_readonly_run(off, m->key_page_off, m->key_page_off + m->key_page_size, lo, hi, perm);
}
// Source permissions are logical, not inferred from host readability: a free
// 4 KiB alias slice can still share an accessible 16 KiB host page. Only the
// native states with CanAlias and user RW are accepted (Heap and CodeData).
// Caller holds g_map_lock -> g_phys_mu, including throughout the subsequent copy.
static int map_source_backed(uint64_t src, uint64_t size) {
    module_t *m = module_of((void *)(uintptr_t)src);
    // NROs are separate permanent native mappings, not the guest image/buffer
    // passed to LoadModule. This relies on the current loader lifetime/layout.
    if (m && m->nro) return 0;
    if (memory_contains((uintptr_t)g_heap, atomic_load(&g_heap_cur), src, size)) return 1;
    if (memory_contains((uintptr_t)g_alias, g_alias_cap, src, size)) {
        size_t committed = 0;
        return dm_as_committed_in(g_alias_space, src, size, &committed) == DM_AS_OK && committed == size;
    }
    // No fallback to module metadata for a hole inside the reserved block.
    if (memory_contains((uintptr_t)g_as, RESERVE, src, size)) return 0;
    // Static module backing is permanent. NRO reload restores pristine data
    // outside these locks; reject it until that separate lifetime is integrated.
    if (!m || m->nro || !m->loaded || !memory_contains((uintptr_t)m->base, m->size, src, size)) return 0;
    uint64_t ro = m->ro_off ? m->ro_off : m->text_end;
    uint64_t data = m->data_off ? m->data_off : ro;
    uint64_t end = m->patch_off > 0 ? (uint64_t)m->patch_off : m->size;
    uint64_t off = src - (uintptr_t)m->base;
    if (ro > data || data > end || end > m->size || !memory_contains(data, end - data, off, size)) return 0;
    uint64_t perm = 3;
    module_data_query(m, off, &data, &end, &perm);
    return perm == 3 && memory_contains(data, end - data, off, size);
}
// Overlay borrowed source ownership on the existing state run. Individual
// ownership boundaries stay distinct; deleting an owner naturally rejoins the
// surrounding unlocked run. No per-4-KiB hardware protection is implied.
static void map_source_query(uint64_t addr, uint64_t *base, uint64_t *size, uint32_t *attr, uint64_t *perm) {
    uint64_t lo = *base, remaining = *size;
    for (int i = 0; i < g_nmap; ++i) {
        uint64_t src = g_map[i].src, end = src + g_map[i].size;
        uint64_t lower = lo, upper_distance = remaining;
        if (addr >= src && addr < end) {
            if (src > lower) lower = src;
            if (end - lo < upper_distance) upper_distance = end - lo;
            *attr = 1;  // Locked / IsBorrowed, NOT PermissionLocked (bit 4)
            *perm = 0;
        } else if (end <= addr && end > lower) lower = end;
        else if (src > addr && src - lo < upper_distance) upper_distance = src - lo;
        remaining = upper_distance - (lower - lo);
        lo = lower;
    }
    *base = lo; *size = remaining;
}

// Horizon's public MemoryInfo is 0x28 bytes, including the final reserved word.
// A local object + memcpy supports an unaligned output and initializes every
// field, without depending on the caller's old padding/refcount values.
typedef struct {
    uint64_t base, size;
    uint32_t state, attr, perm, ipc_refcount, device_refcount, reserved;
} svc_memory_info;
_Static_assert(sizeof(svc_memory_info) == 0x28, "MemoryInfo ABI size");
_Static_assert(offsetof(svc_memory_info, state) == 0x10 && offsetof(svc_memory_info, attr) == 0x14 &&
               offsetof(svc_memory_info, perm) == 0x18 && offsetof(svc_memory_info, reserved) == 0x24,
               "MemoryInfo ABI field offsets");

// Diagnostic DM_ALIAS_WINDOWS=<window MB>: every 30 s, the mapped memory of each window of that size in the alias region (windows aligned to the size in absolute addresses, counted from the page
// bitmap; windows with nothing mapped are left out). For sizing an allocator that reserves fixed, aligned windows and commits pages from each window's start: Unreal's FMallocBinned3 takes
// 74 pools of 512 MB (Octopath Traveler 0, main+0x9f748)
static void *alias_windows_thread(void *arg) {
    uint64_t win = (uint64_t)(uintptr_t)arg << 20, a0 = (uint64_t)(uintptr_t)g_alias, a1 = a0 + g_alias_cap;
    for (;;) {
        sleep(30);
        char line[8192] = {0}; size_t o = 0; uint64_t tot = 0;
        for (uint64_t w = a0 & ~(win - 1); w < a1 && o < sizeof line - 64; w += win) {
            uintptr_t lo = w > a0 ? w : a0, hi = win < a1 - w ? w + win : a1;
            size_t n = 0;
            if (dm_as_committed_in(g_alias_space, lo, hi - lo, &n) != DM_AS_OK) continue;
            if (n) { o += snprintf(line + o, sizeof line - o, " +%#llx:%.1f", (unsigned long long)(w - (a0 & ~(win - 1))), n / 1048576.0); tot += n; }
        }
        LOG("[mem] alias windows of %llu MB (window offset:MB mapped), %.1f MB in all:%s", (unsigned long long)(win >> 20), tot / 1048576.0, line);
    }
    return NULL;
}

int svc_thread(int num, uint64_t *regs);   // hle_thread.c: thread and synchronization kind

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr) {
    { void hlecount_svc(int); hlecount_svc(num); }   // DM_HLECOUNT: svc counts by number
    mem_init();
    if (svc_thread(num, regs)) return regs[0];
    switch (num) {
    case 0x01: {  // SetHeapSize(out_addr*, size)  → x1 = heap base
        size_t size = regs[1];
        if (size > g_heap_cap || (size & 0x1fffff)) { LOG("SetHeapSize invalid: req %#zx, cap %#zx", size, g_heap_cap); regs[0] = 0xD401; return 0; }
        pthread_mutex_lock(&g_map_lock);
        pthread_mutex_lock(&g_phys_mu);
        size_t previous = atomic_load(&g_heap_cur);
        if (size < previous && map_source_busy((uintptr_t)g_heap + size, previous - size)) {
            pthread_mutex_unlock(&g_phys_mu); pthread_mutex_unlock(&g_map_lock);
            regs[0] = 0xD401; return 0;
        }
        size_t image = image_bytes(), bss = big_bss_bytes();
        // Keep the image-footprint check. With mem_map_limit, later heap growth
        // must also respect the same total reported by UsedMem and used by maps.
        uint64_t charged = (uint64_t)DM_GAME_MEM_USED + bss;
        if (DM_GAME_MEM_MAP_LIMIT) charged += alias_mapped();
        if (image > APP_POOL || size > APP_POOL - image ||
            (DM_GAME_MEM_MAP_LIMIT && (charged > APP_POOL || size > APP_POOL - charged))) {
            static int once; if (!once++) LOG("[mem] SetHeapSize %#zx + modules %#zx exceeds app pool %#llx (used memory reported %#llx + bss %#zx)", size, image - bss,
                                              (unsigned long long)APP_POOL, (unsigned long long)DM_GAME_MEM_USED, bss);
            pthread_mutex_unlock(&g_phys_mu);
            pthread_mutex_unlock(&g_map_lock);
            regs[0] = 0x10801; return 0;
        }
        int rc = 0;
        if (size > previous) rc = mprotect(g_heap + previous, size - previous, PROT_READ | PROT_WRITE);
        else if (size < previous) rc = mprotect(g_heap + size, previous - size, PROT_NONE);
        if (g_flag.svc) LOG("SetHeapSize %#zx -> base %p mprotect=%d errno=%d", size, g_heap, rc, errno);
        if (rc) { pthread_mutex_unlock(&g_phys_mu); pthread_mutex_unlock(&g_map_lock); regs[0] = 0xD401; return 0; }
        if (!g_flag.svc && size > previous) LOG("[mem] heap %.2f GB (heap region %.2f GB)", size / 1073741824.0, g_heap_cap / 1073741824.0);
        atomic_store(&g_heap_cur, size);
        pthread_mutex_unlock(&g_phys_mu);
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = 0; regs[1] = (uint64_t)g_heap; return 0;
    }
    case 0x02: case 0x03: { // SetMemoryPermission / SetMemoryAttribute
        // The general SVCs remain legacy no-ops, but cannot claim a successful
        // mutation of a source borrowed by MapMemory. Do not touch host pages.
        uint64_t addr = regs[0], size = regs[1];
        pthread_mutex_lock(&g_map_lock);
        int busy = size > UINT64_MAX - addr || (size && map_source_busy(addr, size));
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = busy ? 0xD401 : 0; return 0;
    }
    case 0x04: {  // MapMemory(dst, src, size): when creating a thread the SDK moves the stack on the heap into the stack region
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        uint32_t result = map_arguments(dst, src, size);
        if (result) { regs[0] = result; return 0; }
        pthread_mutex_lock(&g_map_lock);
        pthread_mutex_lock(&g_phys_mu);
        if (map_find(dst, size) || map_source_busy(src, size) || !map_source_backed(src, size)) result = 0xD401;
        else if (g_nmap == MAX_MAP) result = 0xCE01;  // OutOfResource; no backend change
        if (!result) {
            // Native copy/move architecture, NOT two coherent aliases. Horizon
            // removes user access to the borrowed source; here it stays host
            // accessible because a host page can contain live 4 KiB siblings.
            size_t mask = (size_t)getpagesize() - 1;
            uint64_t lo = dst & ~(uint64_t)mask, hi = (dst + size + mask) & ~(uint64_t)mask;
            if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) result = 0xD401;
            else {
                // Opening a whole host page also unprotects neighboring GPU
                // dirty-tracked slices, so notify for that full host-page run.
                dm_guest_write_prepare((void *)lo, hi - lo);
                memcpy((void *)dst, (void *)src, size);
                map_add(dst, size, src, 0x0B /*Stack*/);
            }
        }
        pthread_mutex_unlock(&g_phys_mu);
        pthread_mutex_unlock(&g_map_lock);
        if (result) { regs[0] = result; return 0; }
        if (g_flag.svc) LOG("MapMemory %#llx <- %#llx +%#llx", dst, src, size);
        regs[0] = 0; return 0;
    }
    case 0x05: {  // UnmapMemory(dst, src, size): contents are moved back to the source region; a 16K page may be shared with an adjacent mapping, permissions are not revoked
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        uint32_t result = map_arguments(dst, src, size);
        if (result) { regs[0] = result; return 0; }
        pthread_mutex_lock(&g_map_lock);
        pthread_mutex_lock(&g_phys_mu);
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size) result = 0xD401;
        else if (g_map[k - 1].src != src) {
            result = 0xD401;
            // Both exact ranges can have valid mapped/borrowed state yet refer
            // to different backing. Distinguish that from an invalid state.
            for (int i = 0; i < g_nmap; ++i)
                if (g_map[i].src == src && g_map[i].size == size) { result = 0xDC01; break; }
        }
        if (result) {
            pthread_mutex_unlock(&g_phys_mu); pthread_mutex_unlock(&g_map_lock); regs[0] = result; return 0; }
        // Source backing cannot have shrunk/decommitted while this entry lives.
        // Unlock logically only after copyback, with dirty tracking informed.
        dm_guest_write_prepare((void *)src, size);
        memcpy((void *)src, (void *)dst, size);
        map_del(k - 1);
        pthread_mutex_unlock(&g_phys_mu);
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = 0; return 0;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* out, PageInfo* pout, addr) → x1 = pageinfo
        uint64_t mi = regs[0], addr = regs[2];
        // MemoryInfo: base(0) size(8) state(0x10) attr(0x14) perm(0x18) ...
        uint8_t *a = (uint8_t *)addr;
        uint64_t base, size, state, perm;
        uint32_t attr = 0;
        int k;
        pthread_mutex_lock(&g_map_lock);
        pthread_mutex_lock(&g_phys_mu);
        size_t heap_size = atomic_load(&g_heap_cur);
        uintptr_t alias = (uintptr_t)g_alias, alias_end = alias + g_alias_cap;
        int in_alias = addr >= alias && addr - alias < g_alias_cap;
        dm_as_range ar = {0};
        if (in_alias && dm_as_query(g_alias_space, addr, &ar) != DM_AS_OK) die("alias address query");
        if (a >= g_heap && a < g_heap + heap_size) { base=(uint64_t)g_heap; size=heap_size; state=5/*Heap*/; perm=3; }
        else if ((k = map_find(addr, 1))) { base=g_map[k-1].base; size=g_map[k-1].size; state=g_map[k-1].state; perm=3; }
        else if (in_alias && ar.committed) { base=ar.base; size=ar.size; state=5/*Heap*/; perm=3; }
        else if (a >= g_as && a < g_as + RESERVE) {   // free: up to the adjacent mapping/heap boundary
            uint64_t lo = (uint64_t)g_as, hi = (uint64_t)g_as + RESERVE;
            if (a >= g_heap) lo = (uint64_t)g_heap + heap_size;
            else if (hi > (uint64_t)g_heap) hi = (uint64_t)g_heap;
            // A free run may cross an alias partition boundary, but never a
            // committed alias page. Querying an edge finds the nearest map when
            // the requested address lies before/after the alias region itself.
            if (in_alias) {
                if (ar.base > alias && ar.base > lo) lo = ar.base;
                if (ar.base + ar.size < alias_end && ar.base + ar.size < hi) hi = ar.base + ar.size;
            } else if (addr < alias) {
                if (dm_as_query(g_alias_space, alias, &ar) != DM_AS_OK) die("alias lower-edge query");
                uintptr_t edge = ar.committed ? alias : ar.base + ar.size;
                if ((ar.committed || edge < alias_end) && edge < hi) hi = edge;
            } else {
                if (dm_as_query(g_alias_space, alias_end - 1, &ar) != DM_AS_OK) die("alias upper-edge query");
                uintptr_t edge = ar.committed ? alias_end : ar.base;
                if ((ar.committed || edge > alias) && edge > lo) lo = edge;
            }
            for (int i = 0; i < g_nmap; i++) {
                uint64_t e = g_map[i].base + g_map[i].size;
                if (e <= addr && e > lo) lo = e;
                if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
            }
            base=lo; size=hi-lo; state=0; perm=0; }
        else if (module_of(a)) {
            // Loaded module: report the regions in Horizon's layout -- code [0, ro) Code+RX, read-only [ro, data) Code+R, data and bss [data, patch area) CodeData+RW.
            // The SDK's C++ exception unwinding (nn::ro::detail::GetExceptionInfo) walks back from the PC over "Code+RX" regions to find the module start, forward over Code then CodeData to find its end, then reads MOD0 at the module start for .eh_frame_hdr.
            // Previously "the 4 KiB page around it, Code+RX" was reported: the backward walk ran on into other memory, found no MOD0 and aborted (Engage's IL2CPP throws a C++ exception for every managed exception).
            // The rewriter's patch area is reported as CodeData+RX: modules sit back to back in the host, and an executable patch area reported as Code+RX would merge the previous module into the backward walk
            // Modules with the patch area before the base (patch_off < 0): the patch area is reported alone as CodeData+RX, and data and bss run to the end of the image
            module_t *mm = module_of(a); uint64_t off = (uint64_t)(a - mm->base), mb = (uint64_t)mm->base;
            uint64_t ro = mm->ro_off ? mm->ro_off : mm->text_end, da = mm->data_off ? mm->data_off : ro, pa = mm->patch_off > 0 ? (uint64_t)mm->patch_off : mm->size;
            if (a < mm->base)  { base = mb + mm->patch_off; size = mm->patch_len; state = 4/*CodeData*/; perm = 5; }
            else if (off < ro) { base = mb;      size = ro;            state = 3/*Code*/;     perm = 5; }
            else if (off < da) { base = mb + ro; size = da - ro;       state = 3/*Code*/;     perm = 1; }
            else if (off < pa) { base = mb + da; size = pa - da;       state = 4/*CodeData*/; perm = 3; }
            else               { base = mb + pa; size = mm->size - pa; state = 4/*CodeData*/; perm = 5; }
            if (state == 4 && perm == 3) {
                // Key-table pages and rounded text/patch seals are not writable,
                // even when they lie inside the nominal data/BSS section.
                uint64_t lo = base - mb, hi = lo + size;
                module_data_query(mm, off, &lo, &hi, &perm);
                base = mb + lo; size = hi - lo;
            }
        }
        else {
            // The address lies in "the current host thread's stack": report the whole stack (state 0xB = stack). When the SDK initializes the main thread record it queries here with the sp at that moment and takes the reported range as the main thread stack;
            // later nn::os::GetCurrentStackInfo asserts that sp is still inside that range and aborts if not. Previously every address outside the guest address space was reported as "the 4 KiB page containing it",
            // so the main thread stack was recorded as one page. Whether the sp at init and a later sp share a page depends on the absolute position of the host stack top, which moves with the total length of environment variables / launch arguments (a different environment occasionally aborts at startup).
            uint64_t top = (uint64_t)pthread_get_stackaddr_np(pthread_self()), ssz = pthread_get_stacksize_np(pthread_self());
            if (addr < top && addr >= top - ssz) { base = top - ssz; size = ssz; state = 0xB/*Stack*/; perm = 3; }
            // Other host memory: report "free, no permissions" (the 4 KiB page around it) as Horizon does for unmapped addresses. It used to report Code+RX, and GetExceptionInfo walking back from main merged on through host memory down to 0
            else { base=addr & ~0xFFFull; size=0x1000; state=0/*Free*/; perm=0; } }
        map_source_query(addr, &base, &size, &attr, &perm);
        pthread_mutex_unlock(&g_phys_mu);
        pthread_mutex_unlock(&g_map_lock);
        if (mi) {
            svc_memory_info out = {base, size, (uint32_t)state, attr, (uint32_t)perm, 0, 0, 0};
            memcpy((void *)(uintptr_t)mi, &out, sizeof out);
        }
        regs[0] = 0; regs[1] = 0; return 0;
    }
    case 0x2C: case 0x2D: {  // MapPhysicalMemory / UnmapPhysicalMemory(addr, size): only allowed inside the alias region (used by the SDK's VammManager)
        uintptr_t a = (uintptr_t)regs[0]; size_t size = regs[1];
        uintptr_t alias = (uintptr_t)g_alias;
        // Subtraction after lower-bound/size checks avoids wrapping a+size.
        if (!size || (a & 0xfff) || (size & 0xfff) || a < alias ||
            size > g_alias_cap || a - alias > g_alias_cap - size) {
            LOG("SVC %#x invalid alias range %p+%#zx", num, (void *)a, size); regs[0] = 0xD401; return 0;
        }
        // Profile mem_map_limit is unchanged: only enabled profiles charge alias
        // commits to the application pool. Repeated/overlapping maps charge only
        // newly committed pages. All map/unmap/backend/accounting steps serialize.
        pthread_mutex_lock(&g_map_lock);
        pthread_mutex_lock(&g_phys_mu);
        // Neither recommit nor decommit may modify pages borrowed by a stack.
        // Reject the entire request before the manager/backend sees it.
        if (map_source_busy(a, size)) {
            pthread_mutex_unlock(&g_phys_mu); pthread_mutex_unlock(&g_map_lock);
            regs[0] = 0xD401; return 0;
        }
        size_t before = alias_mapped(), limit = SIZE_MAX;
        if (DM_GAME_MEM_MAP_LIMIT) {
            uint64_t used = (uint64_t)atomic_load(&g_heap_cur) + DM_GAME_MEM_USED + big_bss_bytes();
            limit = used < APP_POOL ? (size_t)(APP_POOL - used) : 0;
        }
        dm_as_result result = num == 0x2C ? dm_as_commit(g_alias_space, a, size, limit)
                                         : dm_as_decommit(g_alias_space, a, size);
        size_t after = alias_mapped();
        pthread_mutex_unlock(&g_phys_mu);
        pthread_mutex_unlock(&g_map_lock);
        if (result != DM_AS_OK) {
            if (result == DM_AS_LIMIT) {
                static _Atomic int nref; if (atomic_fetch_add(&nref, 1) < 8) LOG("[mem] MapPhysicalMemory +%#zx refused: %.2f GB mapped, the application pool is full (ResultLimitReached, the SDK frees cached pages and retries)",
                                                                       size, before / 1073741824.0);
                regs[0] = 0x10801;
            } else {
                LOG("[RED] SVC %#x alias mapping failed %p+%#zx (manager=%d errno=%d)", num, (void *)a, size, result, errno);
                regs[0] = 0xD401;
            }
            return 0;
        }
        if (g_flag.svc) LOG("%sPhysicalMemory %p+%#zx", num == 0x2C ? "Map" : "Unmap", (void *)a, size);
        // High-water address and actual physical use remain separate diagnostics.
        if (num == 0x2C) { static _Atomic uint64_t hw; uint64_t end = a - alias + size, old = atomic_load(&hw);
            while (end > old && !atomic_compare_exchange_weak(&hw, &old, end)) {}
            if (end > old && end >> 28 != old >> 28) LOG("[mem] alias region mapped up to +%.2f GB (alias region %.2f GB)", end / 1073741824.0, g_alias_cap / 1073741824.0); }
        if (before >> 28 != after >> 28) LOG("[mem] alias region mapped %.2f GB (application pool %.2f GB)", after / 1073741824.0, APP_POOL / 1073741824.0);
        regs[0] = 0; return 0;
    }
    case 0x26: {  // Break(reason, ptr, size): SDK fatal abort
        uint32_t res = (regs[1] && regs[2] >= 4) ? *(uint32_t *)regs[1] : 0;
        module_t *mm = module_of((void *)regs[30]);
        static int nb;
        LOG("Break#%d reason=%#llx Result=%#x svc at %s+%#llx", ++nb, regs[0], res, mm ? mm->name : "?", orig_addr);
        dm_backtrace(regs[29]);
        if (regs[0] & 0x80000000ull) return 0;   // notification only, can continue
        _exit(42);
    }
    case 0x1E: guest_clock_read_wait(); regs[0] = 0; regs[1] = guest_ns() * 12 / 625; return 0;   // GetSystemTick
    case 0x27: {  // OutputDebugString(str, len)
        if (g_flag.dbg) fprintf(stderr, "[guest] %.*s\n", (int)regs[1], (char *)regs[0]);
        regs[0] = 0; return 0;
    }
    case 0x29: {  // GetInfo(out*, id, handle, subid) → x1 = value
        uint64_t id = regs[1], v = 0;
        switch (id) {
        case I_CoreMask: v = 7; break;
        case I_PriorityMask: v = 0; break;
        case I_AliasAddr: v = (uint64_t)g_alias; break;   case I_AliasSize: v = g_alias_cap; break;
        case I_HeapAddr:  v = (uint64_t)g_heap;  break;   case I_HeapSize:  v = g_heap_cap;  break;
        case I_AslrAddr:  v = (uint64_t)g_as;    break;   case I_AslrSize:  v = RESERVE;     break;
        case I_StackAddr: v = (uint64_t)g_stack; break;   case I_StackSize: v = g_stack_cap; break;
        case I_TotalMem: case I_TotalNonSys: v = APP_POOL; break;
        case I_UsedMem: case I_UsedNonSys:
            pthread_mutex_lock(&g_phys_mu);
            v = atomic_load(&g_heap_cur) + DM_GAME_MEM_USED + big_bss_bytes();
            if (DM_GAME_MEM_MAP_LIMIT) v += alias_mapped();
            pthread_mutex_unlock(&g_phys_mu);
            break;
        // the system resource the NPDM declares (profile sys_resource; 32 MB without the key). The SDK's IsVirtualAddressMemoryEnabled is "total != 0": Big Brain Academy's nninitStartup
        // asks StandardAllocator for 60 GB of virtual address memory when it is, and sets a heap only when it is not (its NPDM declares none)
        case I_SysResTotal: v = DM_GAME_SYS_RESOURCE; break; case I_SysResUsed: v = DM_GAME_SYS_RESOURCE ? 0x400000 : 0; break;
        case I_DebuggerAttached: v = 0; break;
        case I_RandomEntropy: { uint64_t dm_entropy64(void); v = dm_entropy64(); break; }   // a deterministic stream when DM_DETCLOCK is on (hle_os.c)
        case I_ProgramId: { static uint64_t pid; if (!pid) { const char *e = getenv("DM_PROGRAM_ID"); pid = e ? strtoull(e, NULL, 16) : DM_GAME_PROGRAM_ID; } v = pid; break; }   // default from the game profile's program_id; DM_PROGRAM_ID=hex overrides
        case I_IsApplication: v = 1; break;
        // nn::os::GetFreeThreadCount (SDK 11+): the thread resource limit less the threads alive, as the kernel answers it (Atmosphère kern_svc_info.cpp). pm gives an application a limit of
        // 96 threads (pm_spec.cpp BaseApplicationThreads; a boost raises it to 256). Monster Train 2's Unity 2022.3 player creates a thread only when this is at least 1: the 0 it got here
        // left its main thread waiting at boot for a thread it never created (main+0x3e1ac4)
        case I_FreeThreadCount: { int thr_limit_used(void); int u = thr_limit_used(); v = u < 96 ? 96 - u : 0; break; }
        case I_UserExcAddr: { static _Alignas(0x1000) uint8_t ctx[0x1000]; v = (uint64_t)ctx; break; }   // user exception context area: newer SDKs' SetDyingMessageRegion writes to +0x1c0 (0 would write through a null pointer)
        default: v = 0; break;
        }
        regs[0] = 0; regs[1] = v; return 0;
    }
    default:
        if (!g_seen[num & 0xff]) { g_seen[num & 0xff] = 1; LOG("SVC %#x x0=%#llx x1=%#llx x2=%#llx (placeholder returns success)", num, regs[0], regs[1], regs[2]); }
        regs[0] = 0; return 0;
    }
}
