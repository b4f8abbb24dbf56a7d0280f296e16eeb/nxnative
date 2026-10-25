"""Exercise the actual native loader startup ordering using synthetic host stubs.

Only load_modules is compiled from loader.c: platform mapping, guest parsing,
relocation and the experiment are recorded stubs. No guest code or inputs run.
The transaction itself is covered by the separate host memory-hook fixtures.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
HOST = REPO / 'dm/host'


class StartupOrder(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            raise unittest.SkipTest('a C compiler is needed for the actual load_modules body')
        cls.temp = tempfile.TemporaryDirectory(prefix='dm-experiment-order-')
        cls.root = Path(cls.temp.name)
        loader = (HOST / 'loader.c').read_text()
        start = loader.index('void load_modules(const char *dir, const char **names, int n) {')
        end = loader.index('\n// Plays the role of rtld', start)
        source = cls.root / 'startup.c'
        source.write_text(r'''
#define _POSIX_C_SOURCE 200809L
#include "dm.h"
#include <assert.h>
#include <stdarg.h>
#undef LOG
#undef die
static void record_log(const char *fmt, ...) { (void)fmt; }
static _Noreturn void fail(const char *fmt, ...) {
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
    fputc('\n', stderr); exit(19);
}
#define LOG(...) record_log(__VA_ARGS__)
#define die(...) fail(__VA_ARGS__)
module_t g_mod[MAX_MOD];
int g_nmod;
struct dm_flags g_flag;
static module_t *g_loaded[MAX_MOD];
static int g_nloaded, relocated, preloaded, opened, experiment, sealed, entered;
static unsigned long g_symchk_n, g_symchk_bad;
static const char *scenario;
static size_t test_strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src); assert(n < size); memcpy(dst, src, n + 1); return n;
}
#define strlcpy test_strlcpy
uint64_t now_ns(void) { static uint64_t n; return ++n; }
static void read_layout(module_t *m, const char *dir) {
    assert(!strcmp(dir, "synthetic")); printf("layout:%s\n", m->name);
}
static void create_keys(void) { puts("keys"); }
static void map_image(module_t *m, const char *dir) {
    assert(!strcmp(dir, "synthetic")); printf("map:%s\n", m->name);
    m->base = (uint8_t *)m; m->size = sizeof *m;
}
static void tsd_check(void) { puts("tsd"); }
static void parse_dynamic(module_t *m) { printf("parse:%s\n", m->name); }
static void register_gates(module_t *m, const char *dir) {
    assert(!strcmp(dir, "synthetic")); printf("gates:%s\n", m->name);
}
static void force_init(void) { assert(g_nmod == 2 && g_nloaded == 2); puts("force_init"); }
static void relocate(module_t *m) { ++relocated; printf("relocate:%s\n", m->name); }
static void force_check(void) { assert(relocated == 2); puts("force_check"); }
static void preload_nros(const char *dir) {
    assert(!strcmp(dir, "synthetic") && relocated == 2 && !opened);
    /* A deferred module must be visible to the experiment, even unloaded. */
    module_t *m = &g_mod[g_nmod++]; strcpy(m->name, "deferred");
    m->nro = 1; m->base = (uint8_t *)m; m->size = sizeof *m;
    preloaded = 1; puts("preload");
}
int dm_memory_hooks_open_startup(void) {
    assert(preloaded && !opened && !experiment && !sealed && !entered);
    opened = 1; puts("open"); return 1;
}
int dm_memory_experiment_startup(const char *request, const char **diagnostic) {
    assert(preloaded && opened && !experiment && !sealed && !entered);
    assert(g_nmod == 3 && g_mod[2].nro && !g_mod[2].loaded);
    experiment = 1; printf("experiment:%s\n", request ? request : "off");
    if (!strcmp(scenario, "fail")) { *diagnostic = "synthetic explicit opt-in rejection"; return -1; }
    *diagnostic = NULL; return request ? 1 : 0;
}
void hlecount_seal(void) {
    assert(opened && experiment && !sealed && !entered);
    sealed = 1; puts("seal");
}
int dm_memory_hooks_enter_guest(void) {
    assert(sealed && !entered); entered = 1; puts("guest"); return 1;
}
''' + loader[start:end] + r'''
int main(int argc, char **argv) {
    assert(argc == 2); scenario = argv[1];
    unsetenv("DM_SYMCHECK"); unsetenv("DM_EXPERIMENTAL_SHARED_MEMORY");
    if (!strcmp(scenario, "on") || !strcmp(scenario, "fail"))
        assert(!setenv("DM_EXPERIMENTAL_SHARED_MEMORY", "za-base-v1", 1));
    const char *names[] = {"main", "sdk"};
    if (!strcmp(scenario, "prior")) g_nmod = 1;
    load_modules("synthetic", names, !strcmp(scenario, "invalid") ? 0 : 2);
    assert(!entered); puts("returned");
    if (!strcmp(scenario, "repeat")) {
        /* Even clearing public counts cannot reopen this process's startup. */
        g_nmod = g_nloaded = 0;
        load_modules("synthetic", names, 2);
        abort();
    }
    assert(dm_memory_hooks_enter_guest());
    return 0;
}
''')
        cls.binary = cls.root / 'startup'
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(HOST), '-I' + str(HOST / 'tests/address_space_profile'),
                        str(source), '-o', str(cls.binary)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, 'temp'):
            cls.temp.cleanup()

    def run_case(self, scenario, status=0):
        result = subprocess.run([str(self.binary), scenario], capture_output=True, text=True)
        self.assertEqual(result.returncode, status, result.stderr)
        return result.stdout.splitlines(), result.stderr

    def test_off_path_still_seals_before_separate_guest_entry(self):
        events, _ = self.run_case('off')
        self.assertEqual(events[-6:], ['preload', 'open', 'experiment:off', 'seal', 'returned', 'guest'])
        self.assertLess(events.index('relocate:sdk'), events.index('preload'))

    def test_opt_in_runs_after_complete_preload_and_before_seal(self):
        events, _ = self.run_case('on')
        self.assertEqual(events[-6:], ['preload', 'open', 'experiment:za-base-v1', 'seal', 'returned', 'guest'])
        self.assertEqual([e for e in events if e.startswith('map:')], ['map:main', 'map:sdk'])

    def test_explicit_failure_aborts_before_seal_or_guest_entry(self):
        events, diagnostic = self.run_case('fail', 19)
        self.assertEqual(events[-3:], ['preload', 'open', 'experiment:za-base-v1'])
        self.assertNotIn('seal', events)
        self.assertNotIn('returned', events)
        self.assertNotIn('guest', events)
        self.assertIn('synthetic explicit opt-in rejection', diagnostic)

    def test_repeat_is_rejected_before_any_new_mapping_or_layout(self):
        events, diagnostic = self.run_case('repeat', 19)
        self.assertEqual(events[-1], 'returned')
        self.assertEqual([e for e in events if e.startswith('map:')], ['map:main', 'map:sdk'])
        self.assertEqual([e for e in events if e.startswith('layout:')], ['layout:main', 'layout:sdk'])
        self.assertIn('startup/restart is unsupported', diagnostic)

    def test_existing_modules_or_invalid_count_reject_before_mapping(self):
        for scenario in ('prior', 'invalid'):
            with self.subTest(scenario=scenario):
                events, _ = self.run_case(scenario, 19)
                self.assertEqual(events, [])


class ActivationSourceContract(unittest.TestCase):
    def compile_policy(self, flags):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('a C compiler is needed for the actual activation gate')
        with tempfile.TemporaryDirectory(prefix='dm-experiment-compile-') as temp:
            return subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                   '-I' + str(HOST / 'tests/address_space_profile'),
                                   *flags, '-c', str(HOST / 'hle_memory_experiment.c'),
                                   '-o', str(Path(temp) / 'policy.o')], capture_output=True, text=True)

    def test_actual_compile_rejects_each_fixture_activation_flag(self):
        for flag in ('DM_SA_ADAPTER_TESTING', 'DM_VAMM_ADAPTER_TESTING', 'DM_MEMORY_HOOKS_TESTING'):
            with self.subTest(flag=flag):
                result = self.compile_policy(['-DDM_EXPERIMENTAL_SHARED_MEMORY=1', '-D' + flag])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Native experimental activation must not use fixture activation flags', result.stderr)

    def test_actual_compile_accepts_default_zero_and_explicit_experiment(self):
        for flags in ([], ['-DDM_EXPERIMENTAL_SHARED_MEMORY=0'], ['-DDM_EXPERIMENTAL_SHARED_MEMORY=1']):
            with self.subTest(flags=flags):
                result = self.compile_policy(flags)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_actual_runtime_factory_loader_and_failure_cleanup(self):
        source = (HOST / 'hle_memory_experiment.c').read_text()
        body = source[source.index('int dm_memory_experiment_startup('):]
        create = body.index('create_contract_runtime(selected, &runtime)')
        self.assertLess(body.index('preflight_contract(selected,'), create)
        constructor = source[source.index('static inline dm_sa_result create_contract_runtime('):source.index('int dm_memory_experiment_startup(')]
        self.assertIn('validate_contract(c)', constructor)
        self.assertIn('dm_memory_runtime_create_vamm_only_with_contract(out, c->max_regions, &c->runtime)', constructor)
        self.assertIn('dm_memory_runtime_create_with_contract(out, c->max_regions, &c->runtime)', constructor)
        configs = body.index('dm_memory_runtime_configs(')
        install = body.index('loader_install_memory_hooks(')
        published = body.index('return DM_MEMORY_EXPERIMENT_ACTIVE;')
        cleanup = body.index('dm_memory_runtime_destroy(')
        self.assertLess(create, configs)
        self.assertLess(configs, install)
        self.assertLess(install, published)
        self.assertLess(published, cleanup)
        self.assertRegex(body, r'if\s*\(runtime\s*&&\s*dm_memory_runtime_destroy\(runtime\)\s*!=\s*DM_SA_OK\)')
        self.assertIn('return DM_MEMORY_EXPERIMENT_FAILED;', body[cleanup:])
        self.assertIn('authorization.installing = 0;', body[install:published])

    def test_native_activation_does_not_assert_proof_all_or_use_fixture_gates(self):
        source = (HOST / 'hle_memory_experiment.c').read_text()
        code = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
        self.assertNotRegex(code, r'\bDM_(?:SA|VAMM)_PROOF_ALL\b')
        self.assertNotRegex(code, r'\.proofs\s*=')
        self.assertNotRegex(code, r'#\s*define\s+DM_\w+_TESTING\b')
        # Fixture gates may only occur in the explicit compile-time rejection.
        rejects = re.findall(r'#if[^\n]*DM_\w+_TESTING[^\n]*\n#error[^\n]*\n#endif', code)
        self.assertEqual(len(rejects), 1)
        self.assertNotRegex(code.replace(rejects[0], ''), r'\bDM_\w+_TESTING\b')
        self.assertIn('DM_EXPERIMENTAL_SHARED_MEMORY', code)
        self.assertIn('DM_MEMORY_RUNTIME_ALIGNED_LEASES', code)


if __name__ == '__main__':
    unittest.main()
