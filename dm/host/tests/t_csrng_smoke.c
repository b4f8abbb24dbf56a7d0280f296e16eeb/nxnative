// Synthetic original-function callback: no SDK code or extracted input.
#define _GNU_SOURCE
#include "../dm.h"
#include "../hle_ipc.h"
#include "ipc_wire_fixture.h"
#include <assert.h>
#include <setjmp.h>
static jmp_buf failure;
static int expect_abort;
static _Noreturn void smoke_abort(void) {
    assert(expect_abort); longjmp(failure, 1);
}
#define abort smoke_abort
#undef LOG
#define LOG(...) do { fprintf(stderr, "[synthetic smoke fixture] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#include "../hle_csrng_smoke.c"
#undef abort

module_t g_mod[MAX_MOD]; int g_nmod;
pthread_key_t g_keytlr;
static dm_ipc *test_ipc;
static unsigned called, resolves, mode;
static uint32_t session;
static size_t observed[4];
static uintptr_t output;
static size_t output_size;
static int validate(void *c, uint64_t a, uint64_t n) {
    (void)c; return (a == output || (!n && !a)) && n == output_size;
}
static void fill(void *c, uint64_t a, size_t n) {
    (void)c; if (n) memset((void *)(uintptr_t)a, 0x5a, n);
}
__attribute__((aligned(16))) static void original_stub(void *out, size_t size) {
    assert(called < 4); observed[called++] = size;
    if (mode == 1) return; // returning alone is not backend evidence
    if (!session) {
        dm_ipc_csrng_probe_route(test_ipc, DM_IPC_PROBE_ADAPTER_CONNECT);
        assert(!dm_ipc_connect_service(test_ipc, (const uint8_t *)"csrng\0\0", &session));
    }
    output = (uintptr_t)out; output_size = size;
    ipc_test_packet q = ipc_test_request(6, 0, 0, mode == 7 && !size ? 0 : output, size, 1, NULL, 0);
    ipc_test_put32(q.bytes + q.cmif + 12, 0x12345678);
    dm_ipc_memory memory = {NULL, validate, fill, NULL};
    dm_ipc_csrng_probe_route(test_ipc, DM_IPC_PROBE_ADAPTER_SEND);
    assert(!dm_ipc_dispatch(test_ipc, session, q.bytes, sizeof q.bytes, &memory));
    ipc_test_reply reply = ipc_test_decode(q.bytes, sizeof q.bytes);
    assert(!reply.result && reply.version == 1 && reply.token == 0x12345678);
    if (mode == 2) ((uint8_t *)out)[-1] = 0; // canary damage must fail
    if (mode == 3) dm_ipc_csrng_probe_route(test_ipc, DM_IPC_PROBE_FORCE_CRYPTO);
    if (mode == 4) dm_ipc_csrng_probe_route(test_ipc, DM_IPC_PROBE_NATIVE_SEND);
}
void *module_sym(module_t *m, const char *n) {
    assert(m == &g_mod[1]);
    assert(!strcmp(n, "_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm"));
    ++resolves;
    if (mode == 5) return NULL;
    if (mode == 6) return g_mod[0].base;
    return (void *)original_stub;
}
dm_ipc *dm_ipc_host(void) { return test_ipc; }
static void reset(void) {
    if (test_ipc) dm_ipc_destroy(test_ipc);
    test_ipc = dm_ipc_create(64); assert(test_ipc);
    session = called = resolves = mode = 0;
    smoke_prepared = 0; smoke_crypto = NULL;
    memset(g_mod, 0, sizeof g_mod); g_nmod = 2;
    strcpy(g_mod[0].name, "main"); strcpy(g_mod[1].name, "sdk");
    strcpy(g_mod[0].full_build_id, "7222e13ecf6adb3249b5dd5c57fa40b102245fce000000000000000000000000");
    strcpy(g_mod[1].full_build_id, "df7a3960c74c3a4803f826c5871532523aa9cf2a000000000000000000000000");
    for (unsigned i = 0; i < 2; ++i) {
        memcpy(g_mod[i].build_id, g_mod[i].full_build_id, 32);
        g_mod[i].loaded = 1; g_mod[i].text_end = g_mod[i].size = 0x1000;
    }
    g_mod[0].base = (uint8_t *)(uintptr_t)0x10000;
    g_mod[1].base = (uint8_t *)((uintptr_t)original_stub & ~(uintptr_t)0xfff);
    assert(!pthread_setspecific(g_keytlr, observed));
}
#define FAIL(statement) do { expect_abort = 1; if (!setjmp(failure)) { statement; assert(!"expected failure"); } expect_abort = 0; } while (0)
int main(void) {
    assert(!pthread_key_create(&g_keytlr, NULL));
    const char *env[] = {"DM_CSRNG_SMOKE", "DM_WINDOW", "DM_PRELOAD", "DM_OODLE_SERVE", "DM_OODLE_JOB", "DM_GUEST_SPAWN_TEST", "DM_TEST_KEYRACE", "DM_PROBE", "DM_SAVE"};
    for (unsigned i = 0; i < sizeof env / sizeof env[0]; ++i) unsetenv(env[i]);
    reset(); g_nmod = 0; assert(!dm_csrng_smoke_prepare() && !resolves && !called);
    setenv("DM_CSRNG_SMOKE", "0", 1); FAIL(dm_csrng_smoke_prepare());
    setenv("DM_CSRNG_SMOKE", "1", 1);
    for (volatile unsigned i = 1; i < sizeof env / sizeof env[0]; ++i) {
        reset(); setenv(env[i], "0", 1); FAIL(dm_csrng_smoke_prepare()); unsetenv(env[i]); assert(!resolves);
    }
    reset(); g_nmod = 3; FAIL(dm_csrng_smoke_prepare()); assert(!resolves);
    reset(); strcpy(g_mod[0].build_id, "b1f12fd919eae86ab8a978317677e64b"); FAIL(dm_csrng_smoke_prepare()); assert(!resolves);
    reset(); g_mod[1].build_id[0] = '0'; FAIL(dm_csrng_smoke_prepare()); assert(!resolves);
    for (volatile unsigned k = 0; k < 2; ++k) {
        reset(); g_mod[k].full_build_id[63] = '1'; FAIL(dm_csrng_smoke_prepare()); assert(!resolves);
        reset(); g_mod[k].full_build_id[0] = 0; FAIL(dm_csrng_smoke_prepare()); assert(!resolves);
        reset(); g_mod[k].full_build_id[32] = 0; FAIL(dm_csrng_smoke_prepare()); assert(!resolves);
        reset(); g_mod[k].full_build_id[64] = 'a'; FAIL(dm_csrng_smoke_prepare()); assert(!resolves);
    }
    reset(); strcpy(g_mod[0].name, "sdk"); FAIL(dm_csrng_smoke_prepare());
    reset(); g_mod[1].nro = 1; FAIL(dm_csrng_smoke_prepare());
    reset(); g_mod[1].loaded = 0; FAIL(dm_csrng_smoke_prepare());
    reset(); mode = 5; FAIL(dm_csrng_smoke_prepare());
    reset(); mode = 6; FAIL(dm_csrng_smoke_prepare());
    reset(); g_mod[1].text_end = 0; FAIL(dm_csrng_smoke_prepare());
    reset(); FAIL(dm_csrng_smoke_run()); assert(!called);
    reset(); assert(dm_csrng_smoke_prepare()); assert(!pthread_setspecific(g_keytlr, NULL)); FAIL(dm_csrng_smoke_run()); assert(!called);
    for (volatile unsigned bad = 1; bad <= 4; ++bad) {
        reset(); mode = bad; assert(dm_csrng_smoke_prepare()); FAIL(dm_csrng_smoke_run()); assert(called == 1);
    }
    reset(); assert(dm_csrng_smoke_prepare()); dm_csrng_smoke_run();
    assert(called == 4 && resolves == 1 && observed[0] == 0 && observed[1] == 1 && observed[2] == 17 && observed[3] == 4097);
    assert(!smoke_prepared && !smoke_crypto);
    reset(); mode = 7; assert(dm_csrng_smoke_prepare()); dm_csrng_smoke_run(); assert(called == 4);
    dm_ipc_destroy(test_ipc); pthread_key_delete(g_keytlr);
    puts("CSRNG smoke synthetic admission/driver evidence tests passed");
}
