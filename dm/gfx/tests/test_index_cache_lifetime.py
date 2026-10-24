"""Run the real index-cache functions and dirty tracker without Metal.

Only Metal pool registration is represented by its existing epoch increments;
page tracking, slot reuse and index scans are production code. The native
`t_index_cache_lifetime` test also exercises NVN pool/buffer lifetimes and GPU
completion. Run directly; CC/CFLAGS and NVN_MTL_SOURCE may override the compiler,
flags and source (the latter allows checking the same regression on old code).
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

NVN = Path(__file__).resolve().parents[1] / 'nvn'
SOURCE = Path(os.environ.get('NVN_MTL_SOURCE', NVN / 'nvn_mtl.m'))

TIME_HEADER = r'''
#pragma once
#include <stdint.h>
#include <time.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = t->denom = 1; return 0; }
static inline uint64_t mach_absolute_time(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
'''
PRELUDE = r'''
#include "nvn_dirty.c"
#include "nvn_hmap.h"
#include "nvn_obj.h"
#define MLOG(...) LOG(__VA_ARGS__)
static void (*write_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { (void)fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { write_prep = fn; }
void dm_guest_write_prepare(void *p, size_t n) { write_prep(p, n); }
static int pending;
static int pend_hits(uint64_t a, uint64_t n) { (void)a; (void)n; return pending; }
static int scans;
static void count_protect(const void *p, size_t n) { scans++; nvn_dirty_protect(p, n); }
#define nvn_dirty_protect count_protect
'''
CASES = r'''
static int failed, checks;
#define CHECK(c, label) do { checks++; if (!(c)) { fprintf(stderr, "FAIL: %s\n", label); failed++; } } while (0)
// The native test uses the real NVN API; here only the Metal registration calls
// are represented, in their production order relative to dirty registration.
static void add_pool(void *p, size_t n) { atomic_fetch_add(&g_pool_gen, 1); nvn_dirty_add_range(p, n); }
static void retire_pool(void *p) { nvn_dirty_remove_range(p); atomic_fetch_add(&g_pool_gen, 1); }
static void indices(void *p, int size, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t values[] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        if (size == 1) ((uint8_t *)p)[i] = values[i];
        else if (size == 2) ((uint16_t *)p)[i] = values[i];
        else ((uint32_t *)p)[i] = values[i];
    }
}
static void check_type(int type) {
    size_t pg = (size_t)getpagesize(); int size = 1 << type;
    void *mem = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    void *other = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED || other == MAP_FAILED) abort();
    indices(mem, size, 2, 5, 4); add_pool(mem, pg);
    p_draw_idx di = {.ibuf=(uintptr_t)mem, .count=3, .itype=type}; uint32_t lo=0, hi=0;
    CHECK(ix_range(&di, &lo, &hi) && lo == 2 && hi == 5, "initial indices");
    int before = scans;
    CHECK(ix_range(&di, &lo, &hi) && scans == before, "unchanged lifetime reuses cached indices");
    // Adding an unrelated pool does not advance the dirty clock. The epoch
    // guard must precede the now == seen shortcut, not only the span shortcut.
    uint64_t now = nvn_dirty_now(); add_pool(other, pg);
    CHECK(nvn_dirty_now() == now, "pool creation does not advance dirty clock");
    CHECK(ix_range(&di, &lo, &hi) && scans == before + 1, "pool epoch invalidates equal-clock shortcut");
    before = scans;
    CHECK(ix_range(&di, &lo, &hi) && scans == before, "rescan captures current pool epoch");
    retire_pool(other);
    CHECK(ix_range(&di, &lo, &hi), "refresh after unrelated retirement");
    // These are sequential lifetimes, not writes to a still-live GPU resource.
    // Retiring the protected pool advances the dirty clock; its replacement
    // reuses the dirty slot but resets that slot's per-page generations.
    const void *span = nvn_dirty_span(mem, 3 * size); now = nvn_dirty_now();
    retire_pool(mem); indices(mem, size, 10, 20, 30); add_pool(mem, pg);
    CHECK(nvn_dirty_now() != now, "retirement advances dirty clock");
    CHECK(nvn_dirty_span(mem, 3 * size) == span, "replacement reuses dirty slot");
    CHECK(ix_range(&di, &lo, &hi) && lo == 10 && hi == 30, "new lifetime rescans reset dirty slot");
    dm_guest_write_prepare(mem, 3 * size); indices(mem, size, 40, 50, 60);
    CHECK(ix_range(&di, &lo, &hi) && lo == 40 && hi == 60, "live CPU writes still invalidate");
    pending = 1; CHECK(!ix_range(&di, &lo, &hi), "pending GPU write is not scanned"); pending = 0;
    vc_gpu_write((uintptr_t)mem, 3 * size);
    CHECK(!ix_range(&di, &lo, &hi), "GPU-written range stays conservative");
    add_pool(other, pg); retire_pool(other);
    CHECK(!ix_range(&di, &lo, &hi), "unrelated pool churn never clears gpuw");
    retire_pool(mem); indices(mem, size, 70, 80, 90); add_pool(mem, pg);
    CHECK(!ix_range(&di, &lo, &hi), "replacement also preserves gpuw conservatism");
    retire_pool(mem);
    nvn_hm_clear(&g_ir, free); memset(g_vc_rgn, 0, sizeof g_vc_rgn);
    CHECK(!ix_range(&di, &lo, &hi), "untracked indices are not scanned");
    munmap(mem, pg); munmap(other, pg);
}
int main(void) {
    for (int type = 0; type < 3; type++) check_type(type);
    printf("index cache lifetime: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
'''


class IndexCacheLifetime(unittest.TestCase):
    def test_production_index_cache(self):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not shutil.which(compiler[0]):
            self.skipTest('C compiler not installed')
        source = SOURCE.read_text()
        # Exact production declarations and functions, not a copied model.
        epoch = next(line for line in source.splitlines() if line.startswith('static _Atomic uint64_t g_pool_gen ='))
        begin = source.index('enum { VC_RGN =')
        end = source.index('// The vertex ids a direct draw reads:', begin)
        with tempfile.TemporaryDirectory(prefix='index-cache-lifetime-') as tmp:
            tmp = Path(tmp)
            (tmp / 'mach').mkdir()
            (tmp / 'mach/mach_time.h').write_text(TIME_HEADER)
            (tmp / 'game_profile.h').write_text('#define DM_GAME_VCONV_RANGE 1\n')
            code, exe = tmp / 'test.c', tmp / 'test'
            code.write_text(PRELUDE + '\n' + epoch + '\n' + source[begin:end] + CASES)
            flags = shlex.split(os.environ.get('CFLAGS', '-O1 -g'))
            build = subprocess.run(compiler + flags + ['-std=gnu11', '-Wall', '-Wextra', '-Wno-unused-function',
                '-Wno-unused-variable', '-I' + str(tmp), '-I' + str(NVN), '-I' + str(NVN / 'include'),
                str(code), '-pthread', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout, end='')


if __name__ == '__main__':
    unittest.main()
