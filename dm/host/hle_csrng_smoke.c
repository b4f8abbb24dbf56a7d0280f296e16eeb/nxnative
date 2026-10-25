// One reviewed original-SDK client call chain, never a production hook policy.
// No private bytes/offsets are embedded. HIPC adapters deliberately stay active.
#include "dm.h"
#include "hle_ipc.h"
#include "hle_csrng_smoke.h"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

static void (*smoke_crypto)(void *, size_t);
static int smoke_prepared;
int dm_csrng_smoke_prepare(void) {
    const char *mode = getenv("DM_CSRNG_SMOKE");
    if (!mode) return 0;
    if (strcmp(mode, "1")) die("DM_CSRNG_SMOKE accepts only 1 (unset disables it)");
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    die("DM_CSRNG_SMOKE is a Mac-only diagnostic");
#endif
    const char *conflicts[] = {"DM_WINDOW", "DM_PRELOAD", "DM_OODLE_SERVE", "DM_OODLE_JOB",
        "DM_GUEST_SPAWN_TEST", "DM_TEST_KEYRACE", "DM_PROBE", "DM_SAVE"};
    for (unsigned i = 0; i < sizeof conflicts / sizeof conflicts[0]; ++i)
        if (getenv(conflicts[i])) die("DM_CSRNG_SMOKE requires %s unset", conflicts[i]);
    // Exact supplied pair, not inferred from a profile or SDK major. Normal
    // relocation, instruction rewriting and all existing forced hooks remain.
    if (g_nmod != 2 || strcmp(g_mod[0].name, "main") || strcmp(g_mod[1].name, "sdk") ||
        memcmp(g_mod[0].full_build_id, "7222e13ecf6adb3249b5dd5c57fa40b102245fce000000000000000000000000", 65) ||
        memcmp(g_mod[1].full_build_id, "df7a3960c74c3a4803f826c5871532523aa9cf2a000000000000000000000000", 65))
        die("DM_CSRNG_SMOKE requires the reviewed FULL main7222e13e/sdkdf7a3960 identities in main,sdk order; rewrite old metadata");
    for (unsigned i = 0; i < 2; ++i) {
        if (!g_mod[i].loaded || g_mod[i].nro) die("DM_CSRNG_SMOKE requires loaded static modules");
        if (memcmp(g_mod[i].build_id, g_mod[i].full_build_id, 32) || g_mod[i].build_id[32])
            die("DM_CSRNG_SMOKE short/full identity metadata disagree");
    }
    module_t *sdk = &g_mod[1];
    void *fn = module_sym(sdk, "_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm");
    uintptr_t p = (uintptr_t)fn, base = (uintptr_t)sdk->base;
    if (!fn || !sdk->text_end || sdk->text_end > sdk->size || p < base || p - base >= sdk->text_end || (p & 3))
        die("DM_CSRNG_SMOKE original crypto export is missing or outside SDK text");
    smoke_crypto = (void (*)(void *, size_t))fn; smoke_prepared = 1;
    LOG("CSRNG smoke admitted main=%s sdk=%s; original crypto body, HIPC adapters retained", g_mod[0].full_build_id, sdk->full_build_id);
    return 1;
}
void dm_csrng_smoke_run(void) {
    if (!smoke_prepared || !smoke_crypto || !pthread_getspecific(g_keytlr))
        die("CSRNG smoke requires admission and an existing live message context");
    dm_ipc *ipc = dm_ipc_host();
    if (!ipc) die("CSRNG smoke cannot create the shared IPC owner");
    // uint64_t alignment plus 17 makes each output deliberately unaligned. The
    // largest case crosses a guest 4 KiB boundary regardless of stack position.
    uint64_t storage[(4097 + 40 + 7) / 8];
    uint8_t *bytes = (uint8_t *)storage, *out = bytes + 17;
    const size_t sizes[] = {0, 1, 17, 4097};
    uint64_t total = 0;
    LOG("CSRNG smoke after SDK module initialization; before game startup");
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        memset(storage, 0xa5, sizeof storage);
        uint32_t result = dm_ipc_csrng_probe_begin(ipc, (uintptr_t)out, sizes[i]);
        if (result) die("CSRNG smoke begin failed result=%#x", result);
        smoke_crypto(out, sizes[i]);
        dm_ipc_csrng_probe p;
        result = dm_ipc_csrng_probe_end(ipc, &p);
        if (result) die("CSRNG smoke end failed result=%#x", result);
        unsigned canaries = 1;
        for (size_t j = 0; j < sizeof storage; ++j)
            if ((j < 17 || j >= 17 + sizes[i]) && bytes[j] != 0xa5) canaries = 0;
        LOG("CSRNG smoke call=%u size=%zu requests=%u filled=%llu connect=%u sm=%u transport_errors=%u service_errors=%u mismatches=%u canaries=%u routes=%u,%u,%u,%u,%u,%u",
            i, sizes[i], p.requests, (unsigned long long)p.filled_bytes, p.csrng_connections, p.sm_requests,
            p.transport_errors, p.service_errors, p.mismatches, canaries,
            p.route[0], p.route[1], p.route[2], p.route[3], p.route[4], p.route[5]);
        if (!canaries || p.requests != 1 || p.filled_bytes != sizes[i] || p.sm_requests ||
            p.transport_errors || p.service_errors || p.mismatches ||
            p.csrng_connections != (i == 0) || p.sessions_after != p.sessions_before + (i == 0) ||
            p.route[DM_IPC_PROBE_FORCE_CRYPTO] || p.route[DM_IPC_PROBE_ADAPTER_CONNECT] != (i == 0) ||
            p.route[DM_IPC_PROBE_ADAPTER_SEND] != 1 || p.route[DM_IPC_PROBE_NATIVE_PORT] ||
            p.route[DM_IPC_PROBE_NATIVE_SEND] || p.route[DM_IPC_PROBE_NATIVE_BUFFER])
            die("CSRNG smoke evidence mismatch; not a native-SM/raw-SVC success");
        total += p.filled_bytes;
    }
    smoke_prepared = 0; smoke_crypto = NULL;
    LOG("CSRNG smoke PASS original crypto/client -> shared backend: 4 calls, %llu bytes; native SM/raw-SVC closure NOT tested; exiting before game startup/nnMain",
        (unsigned long long)total);
}
