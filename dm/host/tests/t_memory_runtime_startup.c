// Synthetic ELF startup -> real hook transaction -> runtime factory -> native
// owner/provider/SVC. Reuse the runtime fixture's portable SVC harness, but never
// its direct adapter installation or replacement factory. Only the original
// constructors are tiny native test trampolines; no private SDK code is run.
#define main memory_runtime_direct_fixture_main
#include "t_memory_runtime.c"
#undef main
#include "../hle_memory_hooks.h"
#include <stddef.h>

enum { STARTUP_SYMBOLS = DM_SA_ADAPTER_SYMBOL_COUNT + DM_VAMM_SYMBOL_COUNT,
       TEXT_SIZE = 0x4000, STUB_SIZE = 32 };
typedef struct {
    unsigned char text[TEXT_SIZE];
    Elf64_Sym symbols[STARTUP_SYMBOLS + 1];
    char strings[8192];
    Elf64_Dyn dyn[7];
    Elf64_Rela rela[STARTUP_SYMBOLS];
    uint64_t slots[STARTUP_SYMBOLS + 1], relr[1];
} startup_image;
static startup_image *startup_images[2];
static module_t startup_modules[2];
static uint64_t original_slots[2][STARTUP_SYMBOLS + 1];
static dm_memory_runtime *startup_runtime;
static dm_sa_adapter_config startup_allocator;
static dm_vamm_adapter_config startup_vamm;
static int resolver_failure = -1, resolver_calls, reject_publication, publications;
static size_t startup_image_bytes;
static const char *startup_name(unsigned i) {
    return i < DM_SA_ADAPTER_SYMBOL_COUNT ? dm_sa_adapter_symbol_name(i) :
        dm_vamm_adapter_symbol_name(i - DM_SA_ADAPTER_SYMBOL_COUNT);
}
static uintptr_t original_target(unsigned i) {
    // Preserve the established forced host Allocate(size) shim on every failure.
    return i == 9 ? (uintptr_t)dm_sa_host_forward_allocate :
        (uintptr_t)startup_images[1]->text + STUB_SIZE * (i + 1);
}
static void trampoline(unsigned char *code, dm_sa_guest_fn target) {
    // A native tail jump retains the fixture constructor's ordinary C ABI. The
    // surrounding bytes are mapped so UBSan's indirect-call type probe is safe.
    uintptr_t address = (uintptr_t)target;
#if defined(__x86_64__)
    const unsigned char jump[] = {0x48, 0xb8}; // movabs rax, target; jmp rax
    memcpy(code, jump, sizeof jump); memcpy(code + 2, &address, 8);
    code[10] = 0xff; code[11] = 0xe0;
#elif defined(__aarch64__)
    const uint32_t jump[] = {0xd503245f, 0x58000070, 0xd61f0200, 0xd503201f};
    // bti c; ldr x16, [pc + 12]; br x16; nop; .quad target
    memcpy(code, jump, sizeof jump); memcpy(code + sizeof jump, &address, 8);
#else
#error "Synthetic executable ELF fixture supports x86-64 and AArch64 hosts"
#endif
}
static void startup_images_create(void) {
    size_t page = (size_t)getpagesize();
    assert(page <= TEXT_SIZE && !(TEXT_SIZE % page));
    startup_image_bytes = (sizeof(startup_image) + page - 1) & ~(page - 1);
    for (unsigned k = 0; k < 2; ++k) {
        startup_image *im = mmap(NULL, startup_image_bytes, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(im != MAP_FAILED); startup_images[k] = im;
    }
    for (unsigned k = 0; k < 2; ++k) {
        startup_image *im = startup_images[k]; module_t *m = &startup_modules[k];
        snprintf(m->name, sizeof m->name, "%s", k ? "sdk" : "main");
        m->base = (uint8_t *)im; m->size = sizeof *im;
        m->text_end = m->ro_off = TEXT_SIZE; m->key_page_off = sizeof *im;
        m->symtab = im->symbols; m->strtab = im->strings;
        m->nsym = STARTUP_SYMBOLS + 1; m->dyn = im->dyn; m->loaded = 1;
        size_t pos = 1;
        for (unsigned i = 0; i < STARTUP_SYMBOLS; ++i) {
            const char *name = startup_name(i);
            assert(pos + strlen(name) + 1 <= sizeof im->strings);
            strcpy(im->strings + pos, name);
            im->symbols[i + 1] = (Elf64_Sym){.st_name = (uint32_t)pos,
                .st_info = 0x12, .st_shndx = k ? 1 : 0,
                .st_value = k ? STUB_SIZE * (i + 1) : 0};
            pos += strlen(name) + 1;
            dm_sa_guest_fn original = impossible_original;
            if (i < 2) original = (dm_sa_guest_fn)construct_allocator;
            if (i == DM_SA_ADAPTER_SYMBOL_COUNT + 8 ||
                i == DM_SA_ADAPTER_SYMBOL_COUNT + 9) original = (dm_sa_guest_fn)bootstrap;
            trampoline(im->text + STUB_SIZE * (i + 1), original);
            im->rela[i] = (Elf64_Rela){.r_offset = offsetof(startup_image, slots) + i * 8,
                .r_info = ((uint64_t)(i + 1) << 32) |
                    (i % 3 == 0 ? R_AARCH64_JUMP_SLOT : i % 3 == 1 ? R_AARCH64_GLOB_DAT : R_AARCH64_ABS64)};
            im->slots[i] = original_target(i);
        }
        im->dyn[0] = (Elf64_Dyn){.d_tag = DT_RELA, .d_un.d_ptr = offsetof(startup_image, rela)};
        im->dyn[1] = (Elf64_Dyn){.d_tag = DT_RELASZ, .d_un.d_val = 20 * sizeof(Elf64_Rela)};
        im->dyn[2] = (Elf64_Dyn){.d_tag = DT_JMPREL,
            .d_un.d_ptr = offsetof(startup_image, rela) + 20 * sizeof(Elf64_Rela)};
        im->dyn[3] = (Elf64_Dyn){.d_tag = DT_PLTRELSZ,
            .d_un.d_val = (STARTUP_SYMBOLS - 20) * sizeof(Elf64_Rela)};
        im->dyn[4] = (Elf64_Dyn){.d_tag = DT_RELR, .d_un.d_ptr = offsetof(startup_image, relr)};
        im->dyn[5] = (Elf64_Dyn){.d_tag = DT_RELRSZ, .d_un.d_val = 8};
        im->relr[0] = offsetof(startup_image, slots) + STARTUP_SYMBOLS * 8;
        im->slots[STARTUP_SYMBOLS] = 0x12345678;
        memcpy(original_slots[k], im->slots, sizeof original_slots[k]);
        __builtin___clear_cache((char *)im->text, (char *)im->text + TEXT_SIZE);
        assert(!mprotect(im->text, TEXT_SIZE, PROT_READ | PROT_EXEC));
    }
}
static void startup_create(void) {
    assert(dm_memory_runtime_create(&startup_runtime, 128) == DM_SA_OK);
    assert(dm_memory_runtime_configs(startup_runtime, &startup_vamm, &startup_allocator) == DM_SA_OK);
    assert(!startup_vamm.proofs && !startup_allocator.proofs);
    assert(startup_vamm.context == startup_runtime && startup_allocator.context == startup_runtime);
    startup_vamm.proofs = DM_VAMM_PROOF_ALL;
    startup_allocator.proofs = DM_SA_PROOF_ALL;
    startup_allocator.fatal = fatal; // diagnostic only; factory/lifetimes stay real
    resolver_failure = -1; resolver_calls = publications = reject_publication = 0;
}
static void unpublished(void) {
    assert(!dm_memory_hooks_active());
    for (unsigned i = 0; i < STARTUP_SYMBOLS; ++i) {
        assert(!dm_memory_hooks_lookup(startup_name(i)));
        assert(!dm_sa_adapter_lookup(startup_name(i)) && !dm_vamm_adapter_lookup(startup_name(i)));
    }
    assert(!dm_vamm_is_initialized(startup_vamm.shared_owner));
    assert(!atomic_load(&constructors) && !atomic_load(&forwards) && !alias_mapped());
}
static void unchanged(void) {
    for (unsigned k = 0; k < 2; ++k)
        assert(!memcmp(original_slots[k], startup_images[k]->slots, sizeof original_slots[k]));
    unpublished();
}
static void both_tables_pinned(void) {
    unpublished();
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    // The actual callbacks reject a second pin: neither check adds a reference.
    assert(startup_allocator.retain_context(startup_runtime) == DM_SA_BUSY);
    assert(startup_vamm.retain_owner(startup_runtime, startup_vamm.shared_owner) == DM_VAMM_INVALID);
    no_config(&startup_allocator);
}
static uint64_t startup_resolve(module_t *module, const Elf64_Sym *symbol) {
    both_tables_pinned();
    assert(!dm_memory_hooks_enter_guest());
    int call_index = resolver_calls++;
    if (call_index == resolver_failure) return 0;
    for (unsigned i = 0; i < STARTUP_SYMBOLS; ++i)
        if (!strcmp(module->strtab + symbol->st_name, startup_name(i))) return original_target(i);
    abort();
}
int dm_memory_hooks_test_before_publish(void) {
    ++publications; both_tables_pinned();
    assert(!dm_memory_hooks_enter_guest());
    for (unsigned k = 0; k < 2; ++k) {
        for (unsigned i = 0; i < STARTUP_SYMBOLS; ++i)
            assert(startup_images[k]->slots[i] && startup_images[k]->slots[i] != original_slots[k][i]);
        assert(startup_images[k]->slots[STARTUP_SYMBOLS] == original_slots[k][STARTUP_SYMBOLS]);
    }
    return !reject_publication;
}
static int startup_install(void) {
    return dm_memory_hooks_install(startup_modules, 2, &startup_allocator, &startup_vamm, startup_resolve);
}
static void startup_cleanup(void) {
    free(g_alias_bits); free(g_alias_exposed);
    assert(!munmap(g_as, RESERVE));
    for (unsigned k = 0; k < 2; ++k) assert(!munmap(startup_images[k], startup_image_bytes));
}

#if defined(DM_SA_ADAPTER_TESTING) && defined(DM_VAMM_ADAPTER_TESTING)
// Guest-visible calls below go through the actual rewritten import/data slots,
// not direct adapter installation, direct provider APIs, or lookup-only dispatch.
#undef S
#undef V
#define S(n, type) ((type)(uintptr_t)startup_images[0]->slots[n])
#define V(n, type) ((type)(uintptr_t)startup_images[0]->slots[DM_SA_ADAPTER_SYMBOL_COUNT + (n)])
static void startup_expect_finalize(void *self, dm_sa_result expected) {
    jmp_buf jump; escape = &jump;
    if (!setjmp(jump)) { FINISH(self); abort(); }
    escape = NULL; assert(fatal_result == expected);
}
static void startup_expect_initialize(void *self, dm_sa_result expected) {
    jmp_buf jump; escape = &jump;
    if (!setjmp(jump)) { INIT(self); abort(); }
    escape = NULL; assert(fatal_result == expected);
}
static void rejected_startup(void) {
    unsigned calls = atomic_load(&protect_calls);
    assert(startup_install() != DM_MEMORY_HOOKS_OK);
    unchanged(); assert(atomic_load(&protect_calls) == calls);
    // All real context/owner pins are canceled; no retained config or lease.
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_OK);
}
static void reverse_table_shutdown(void) {
    // Separate process keeps the parent's one-shot publication intact. With no
    // objects, releasing Vamm first must leave the allocator table's own pin.
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        assert(!dm_vamm_adapter_shutdown());
        assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
        assert(dm_sa_adapter_shutdown() == DM_SA_OK);
        assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_OK);
        startup_cleanup(); _exit(0);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
}
static void stopped_slots(void) {
    // Cached committed slots remain intercepted after runtime free. They must
    // abort before dereferencing the retired runtime or constructor originals.
    for (unsigned test = 0; test < 2; ++test) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            unsigned char self[DM_SA_ADAPTER_OBJECT_SIZE] = {0};
            if (test) V(8, void (*)(void *))(self); else CONSTRUCT(self);
            _exit(7);
        }
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
}
#endif
int main(void) {
    struct rlimit limit = {0, 0}; assert(!setrlimit(RLIMIT_CORE, &limit));
    startup_images_create(); startup_create();
    assert(dm_memory_hooks_open_startup());
#if !defined(DM_SA_ADAPTER_TESTING) || !defined(DM_VAMM_ADAPTER_TESTING)
    assert(startup_install() == DM_MEMORY_HOOKS_DISABLED);
    unchanged(); assert(!resolver_calls && !publications);
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_OK);
    startup_cleanup();
    puts("memory runtime startup production transaction remains disabled: PASS");
#else
    // Missing census and failed second-table preparation never publish or leak
    // the first table's real runtime context pin.
    startup_images[1]->symbols[1].st_shndx = 0;
    rejected_startup(); startup_images[1]->symbols[1].st_shndx = 1;
    startup_create(); startup_vamm.proofs = 0; rejected_startup();
    // Every resolver position fails while both real tables are pinned. Include
    // the final position, where a complete earlier module is already journaled.
    for (unsigned i = 0; i < 2 * STARTUP_SYMBOLS; ++i) {
        startup_create(); resolver_failure = (int)i;
        rejected_startup(); assert(resolver_calls == (int)i + 1 && !publications);
    }
    startup_create(); reject_publication = 1;
    rejected_startup(); assert(publications == 1);

    startup_create();
    unsigned calls = atomic_load(&protect_calls);
    assert(startup_install() == DM_MEMORY_HOOKS_OK && publications == 1);
    assert(dm_memory_hooks_active() && atomic_load(&protect_calls) == calls);
    assert(!atomic_load(&constructors) && !alias_mapped());
    for (unsigned k = 0; k < 2; ++k) for (unsigned i = 0; i < STARTUP_SYMBOLS; ++i)
        assert(startup_images[k]->slots[i] == (uintptr_t)dm_memory_hooks_lookup(startup_name(i)));
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    reverse_table_shutdown();
    assert(dm_memory_hooks_enter_guest());
    assert(startup_install() == DM_MEMORY_HOOKS_BUSY);

    unsigned char manager[64] = {0};
    unsigned char self[DM_SA_ADAPTER_OBJECT_SIZE] = {0};
    no_config(&startup_allocator);
    V(8, void (*)(void *))(manager);
    assert(atomic_load(&constructors) == 1);
    no_config(&startup_allocator); // original constructor is not initialization
    V(10, void (*)(void *))(manager);
    assert(dm_vamm_is_initialized(startup_vamm.shared_owner) == !!info(16));
    assert(atomic_load(&protect_calls) == calls && !alias_mapped());
    CONSTRUCT(self); assert(atomic_load(&constructors) == 2);
    assert(V(7, int (*)(void))() == !!info(16));
    if (!info(16)) {
        startup_expect_initialize(self, DM_SA_INVALID);
        assert(!dm_sa_adapter_is_managed(self));
    } else {
        INIT(self);
        assert(self[0] && dm_sa_adapter_is_managed(self) && !alias_mapped());
        assert(TOTAL(self) <= APP_POOL - DM_GAME_MEM_USED && TOTAL(self) < ((size_t)62 << 30));
        assert(dm_sa_adapter_shutdown() == DM_SA_BUSY); // even zero-backing config pins
        assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
        assert(!ALLOC(self, 0));
        ALIGNED_RUNTIME_TESTS(self, startup_vamm.shared_owner, &startup_allocator);
        void *pointer = ALLOC(self, SMALL); assert(pointer && !((uintptr_t)pointer % startup_allocator.allocation_alignment(self, SMALL)));
        memset(pointer, 0x71, SMALL);
        query_state((uintptr_t)pointer, 5, 0, 3);
        dm_vamm_stats stats;
        assert(!dm_vamm_get_stats(startup_vamm.shared_owner, SMALL, &stats) && stats.regions == 1);
        uintptr_t direct;
        assert(!V(2, uint32_t (*)(uintptr_t *, size_t))(&direct, SMALL));
        assert((uintptr_t)pointer + SMALL <= direct || direct + SMALL <= (uintptr_t)pointer);
        assert(alias_mapped() == 2 * SMALL);
        memset((void *)direct, 0x42, SMALL);
        assert(!V(4, uint32_t (*)(uintptr_t))(direct));
        assert(alias_mapped() == SMALL);

        // Exhaust the real physical budget while retaining a live exact lease.
        // Failure must not release it or fall through to the SDK, even under
        // the legacy unlimited direct-SVC policy.
        assert(!call(1, 0, APP_POOL - DM_GAME_MEM_USED - SMALL, 0));
        assert(!ALLOC(self, SMALL) && alias_mapped() == SMALL);
        assert(*(unsigned char *)pointer == 0x71);
        assert(!dm_vamm_get_stats(startup_vamm.shared_owner, SMALL, &stats) && stats.regions == 1);
        assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
        assert(!call(1, 0, 0, 0));

        uintptr_t stack = info(I_StackAddr);
        assert(!call(4, stack, (uintptr_t)pointer, PAGE));
        *(unsigned char *)stack = 0x36;
        startup_expect_finalize(self, DM_SA_BUSY);
        assert(self[0] && dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
        assert(dm_sa_adapter_shutdown() == DM_SA_BUSY && dm_vamm_adapter_shutdown() == DM_VAMM_INVALID);
        assert(!dm_vamm_get_stats(startup_vamm.shared_owner, SMALL, &stats) && stats.regions == 1);
        query_state((uintptr_t)pointer, 5, 1, 0);
        assert(!call(5, stack, (uintptr_t)pointer, PAGE));
        assert(*(unsigned char *)pointer == 0x36);
        FREE(self, pointer); assert(!alias_mapped());

        pointer = ALLOC(self, SMALL); assert(pointer); memset(pointer, 0x62, SMALL);
        // Real host protection failures retain the bitmap/borrow state and
        // abort, per upstream policy; the isolated child verifies that state.
        EXPECT_MEMORY_PROTECTION_FAILURE((FINISH(self), 0), pointer, SMALL);
        assert(self[0] && alias_mapped() == SMALL && *(unsigned char *)pointer == 0x62);
        assert(!dm_vamm_get_stats(startup_vamm.shared_owner, SMALL, &stats) && stats.regions == 1);
        assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
        assert(dm_sa_adapter_shutdown() == DM_SA_BUSY && dm_vamm_adapter_shutdown() == DM_VAMM_INVALID);
        FINISH(self); assert(!self[0] && !alias_mapped());
        assert(!dm_vamm_get_stats(startup_vamm.shared_owner, SMALL, &stats) && !stats.regions);
        INIT(self); pointer = ALLOC(self, SMALL); assert(pointer);
        FREE(self, pointer); FINISH(self);
    }
    assert(!alias_mapped() && !g_nmap && !atomic_load(&forwards));
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    assert(dm_sa_adapter_shutdown() == DM_SA_OK);
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY); // Vamm table alone
    assert(!dm_vamm_adapter_shutdown());
    assert(dm_memory_runtime_destroy(startup_runtime) == DM_SA_OK);
    assert(dm_memory_hooks_active()); // never restore original slots after publication
    for (unsigned k = 0; k < 2; ++k) for (unsigned i = 0; i < STARTUP_SYMBOLS; ++i)
        assert(startup_images[k]->slots[i] == (uintptr_t)dm_memory_hooks_lookup(startup_name(i)));
    stopped_slots(); startup_cleanup();
    puts("memory runtime ELF startup transaction/native ownership/teardown: PASS");
#endif
    return 0;
}
