// Real ELF symbol/RELA/JMPREL/RELR shapes, no guest executable or private input.
#define _POSIX_C_SOURCE 200809L
#include "hle_memory_hooks.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

enum { N = DM_SA_ADAPTER_SYMBOL_COUNT + DM_VAMM_SYMBOL_COUNT };
typedef struct {
    unsigned char text[256];
    Elf64_Sym symbols[N + 2];
    char strings[8192];
    Elf64_Dyn dyn[7];
    Elf64_Rela rela[N + 1];
    uint64_t slots[N + 1], relr[1];
} image;
static image images[2];
static module_t modules[2];
static uint64_t before[2][N + 1];
static dm_sa_adapter_config sc;
static dm_vamm_adapter_config vc;
static int resolve_calls, resolve_fail = -1, rollback, before_publish;
static const char *name_at(unsigned i) {
    return i < DM_SA_ADAPTER_SYMBOL_COUNT ? dm_sa_adapter_symbol_name(i) :
        dm_vamm_adapter_symbol_name(i - DM_SA_ADAPTER_SYMBOL_COUNT);
}
static void never(void) { assert(!"guest/original executed"); }
static uint32_t pages(void *c, uintptr_t p, size_t n, int commit) {
    (void)c; (void)p; (void)n; (void)commit; never(); return 1;
}
static dm_vamm_resource_usage resources(void *c) { (void)c; never(); return (dm_vamm_resource_usage){0}; }
static void *forward(void *self, size_t size) { (void)self; (void)size; never(); return NULL; }
static size_t alignment(void *self, size_t size) { (void)self; (void)size; never(); return 16; }
static dm_sa_result config(void *c, void *s, size_t n, unsigned cache, dm_sa_config *out) {
    (void)c; (void)s; (void)n; (void)cache; (void)out; never(); return DM_SA_INVALID;
}
static uint64_t target(unsigned i) {
    // Existing forced Allocate(size) shim must survive every failed transaction.
    return i == 9 ? (uint64_t)forward : (uint64_t)(images[1].text + 8 + i * 4);
}
static uint64_t resolver(module_t *m, const Elf64_Sym *s) {
    assert(!dm_memory_hooks_active());
    assert(!dm_memory_hooks_enter_guest());
    assert(dm_memory_hooks_install(modules, 2, &sc, &vc, resolver) == DM_MEMORY_HOOKS_BUSY);
    int call = resolve_calls++;
    if (call == resolve_fail) return 0;
    for (unsigned i = 0; i < N; ++i) if (!strcmp(m->strtab + s->st_name, name_at(i))) return target(i);
    return 0;
}
static void original_slots(void) {
    for (unsigned k = 0; k < 2; ++k) assert(!memcmp(before[k], images[k].slots, sizeof before[k]));
    assert(!dm_memory_hooks_active());
    for (unsigned i = 0; i < N; ++i) {
        assert(!dm_memory_hooks_lookup(name_at(i)));
        assert(!dm_sa_adapter_lookup(name_at(i)) && !dm_vamm_adapter_lookup(name_at(i)));
    }
}
int dm_memory_hooks_test_before_publish(void) {
    ++before_publish;
    assert(!dm_memory_hooks_active() && !dm_memory_hooks_enter_guest());
    assert(dm_memory_hooks_install(modules, 2, &sc, &vc, resolver) == DM_MEMORY_HOOKS_BUSY);
    for (unsigned k = 0; k < 2; ++k) for (unsigned i = 0; i < N; ++i) {
        if (ELF64_R_TYPE(images[k].rela[i].r_info) == R_AARCH64_RELATIVE)
            assert(images[k].slots[i] == before[k][i]);
        else assert(images[k].slots[i] != before[k][i]);
        assert(!dm_sa_adapter_lookup(name_at(i)) && !dm_vamm_adapter_lookup(name_at(i)));
    }
    return !rollback;
}
static void setup(void) {
    memset(images, 0, sizeof images); memset(modules, 0, sizeof modules);
    resolve_calls = before_publish = 0; resolve_fail = -1; rollback = 0;
    sc = (dm_sa_adapter_config){.proofs = DM_SA_PROOF_ALL,
        .forward_allocate = forward, .allocation_alignment = alignment, .make_config = config};
    vc = (dm_vamm_adapter_config){.proofs = DM_VAMM_PROOF_ALL,
        .owner = {.base = 0x10000, .size = 0x100000, .max_regions = 32, .change_pages = pages, .resources = resources}};
    for (unsigned k = 0; k < 2; ++k) {
        image *im = &images[k]; module_t *m = &modules[k];
        snprintf(m->name, sizeof m->name, "%s", k ? "sdk" : "main");
        m->base = (uint8_t *)im; m->size = sizeof *im; m->text_end = m->ro_off = sizeof im->text;
        m->key_page_off = sizeof *im; m->symtab = im->symbols; m->strtab = im->strings;
        m->nsym = N + 1; m->dyn = im->dyn; m->loaded = 1;
        size_t pos = 1;
        for (unsigned i = 0; i < N; ++i) {
            strcpy(im->strings + pos, name_at(i));
            im->symbols[i + 1] = (Elf64_Sym){.st_name = (uint32_t)pos, .st_info = 0x12,
                .st_shndx = k ? 1 : 0, .st_value = k ? 8 + i * 4 : 0};
            pos += strlen(name_at(i)) + 1;
            im->rela[i] = (Elf64_Rela){.r_offset = offsetof(image, slots) + i * 8,
                .r_info = ((uint64_t)(i + 1) << 32) | (i % 3 == 0 ? R_AARCH64_JUMP_SLOT : i % 3 == 1 ? R_AARCH64_GLOB_DAT : R_AARCH64_ABS64)};
            im->slots[i] = target(i);
        }
        im->dyn[0] = (Elf64_Dyn){.d_tag = DT_RELA, .d_un.d_ptr = offsetof(image, rela)};
        im->dyn[1] = (Elf64_Dyn){.d_tag = DT_RELASZ, .d_un.d_val = 20 * sizeof(Elf64_Rela)};
        im->dyn[2] = (Elf64_Dyn){.d_tag = DT_JMPREL, .d_un.d_ptr = offsetof(image, rela) + 20 * sizeof(Elf64_Rela)};
        im->dyn[3] = (Elf64_Dyn){.d_tag = DT_PLTRELSZ, .d_un.d_val = (N - 20) * sizeof(Elf64_Rela)};
        im->dyn[4] = (Elf64_Dyn){.d_tag = DT_RELR, .d_un.d_ptr = offsetof(image, relr)};
        im->dyn[5] = (Elf64_Dyn){.d_tag = DT_RELRSZ, .d_un.d_val = 8};
        im->relr[0] = offsetof(image, slots) + N * 8;
        im->slots[N] = 1234;
        memcpy(before[k], im->slots, sizeof before[k]);
    }
}
static int install(void) { return dm_memory_hooks_install(modules, 2, &sc, &vc, resolver); }
static void rejected(void) { assert(install() != DM_MEMORY_HOOKS_OK); original_slots(); }
#ifdef DM_MEMORY_HOOKS_TESTING
static unsigned sa_refs, va_refs;
static int reject_retain;
static dm_sa_result context_retain(void *context) {
    (void)context; dm_sa_adapter_prepared *other = NULL;
    assert(dm_sa_adapter_prepare(&sc, &other) == DM_SA_BUSY && !other);
    if (reject_retain) return DM_SA_PROVIDER_FAILED;
    ++sa_refs; return DM_SA_OK;
}
static void context_release(void *context) {
    (void)context; dm_sa_adapter_prepared *other = NULL;
    assert(dm_sa_adapter_prepare(&sc, &other) == DM_SA_BUSY && !other);
    assert(sa_refs == 1); --sa_refs;
}
static uint32_t owner_retain(void *context, dm_vamm *owner) {
    (void)context; assert(owner == vc.shared_owner); dm_vamm_adapter_prepared *other = NULL;
    assert(dm_vamm_adapter_prepare(&vc, &other) == DM_VAMM_INVALID && !other);
    if (reject_retain) return DM_VAMM_INVALID;
    ++va_refs; return DM_VAMM_OK;
}
static void owner_release(void *context, dm_vamm *owner) {
    (void)context; assert(owner == vc.shared_owner); dm_vamm_adapter_prepared *other = NULL;
    assert(dm_vamm_adapter_prepare(&vc, &other) == DM_VAMM_INVALID && !other);
    assert(va_refs == 1); --va_refs;
}
typedef struct { dm_vamm *owner; unsigned marker; } stopped_context;
static uint32_t stopped_retain(void *context, dm_vamm *owner) {
    stopped_context *c = context; assert(c->marker == 0x55aa && c->owner == owner); return 0;
}
static void stopped_release(void *context, dm_vamm *owner) {
    stopped_context *c = context; assert(c->marker == 0x55aa && c->owner == owner);
    memset(c, 0, sizeof *c); free(c);
}
static void stopped_fatal(void *context, const char *operation) {
    // Must never run: read the retired context so ASan also detects regression.
    volatile unsigned marker = ((stopped_context *)context)->marker;
    (void)marker; (void)operation; _exit(88);
}
static void stopped_vamm_hooks(void) {
    // Each process owns a fresh publication; no test resets production state.
    for (unsigned test = 0; test < 5; ++test) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            stopped_context *context = malloc(sizeof *context); assert(context);
            *context = (stopped_context){.marker = 0x55aa};
            assert(!dm_vamm_create(&context->owner, &vc.owner));
            dm_vamm_adapter_config config = vc;
            config.context = context; config.shared_owner = context->owner;
            config.retain_owner = stopped_retain; config.release_owner = stopped_release;
            config.fatal = stopped_fatal;
            dm_vamm_adapter_prepared *prepared = NULL;
            assert(!dm_vamm_adapter_prepare(&config, &prepared));
            unsigned index = test == 0 ? 0 : test == 1 ? 8 : test == 4 ? 6 : 10;
            dm_vamm_guest_fn cached = dm_vamm_adapter_prepared_hook(prepared, index);
            dm_vamm *retained_owner = context->owner;
            dm_vamm_adapter_publish(prepared);
            assert(!dm_vamm_adapter_shutdown()); // poisons and frees context
            assert(!dm_vamm_destroy(retained_owner));
            unsigned char self[80] = {0};
            if (test == 0) ((void (*)(void))cached)();
            else if (test == 4) (void)((dm_vamm_resource_usage (*)(void))cached)();
            else ((void (*)(void *))cached)(test == 2 ? NULL : self);
            _exit(89);
        }
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
}
static void prepare_lifecycle(void) {
    dm_sa_adapter_symbol ss[DM_SA_ADAPTER_SYMBOL_COUNT]; dm_vamm_symbol vs[DM_VAMM_SYMBOL_COUNT];
    for (unsigned i = 0; i < DM_SA_ADAPTER_SYMBOL_COUNT; ++i) ss[i] = (dm_sa_adapter_symbol){name_at(i), never};
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i) vs[i] = (dm_vamm_symbol){dm_vamm_adapter_symbol_name(i), never};
    sc.symbols = ss; sc.symbol_count = DM_SA_ADAPTER_SYMBOL_COUNT;
    vc.symbols = vs; vc.symbol_count = DM_VAMM_SYMBOL_COUNT;
    dm_sa_adapter_prepared *sa = NULL, *sa2 = NULL; dm_vamm_adapter_prepared *va = NULL, *va2 = NULL;
    stopped_vamm_hooks();
    sc.retain_context = context_retain;
    assert(dm_sa_adapter_prepare(&sc, &sa) == DM_SA_INVALID && !sa);
    sc.release_context = context_release;
    assert(dm_vamm_create(&vc.shared_owner, &vc.owner) == DM_VAMM_OK);
    assert(dm_vamm_adapter_prepare(&vc, &va) == DM_VAMM_INVALID && !va);
    vc.retain_owner = owner_retain; vc.release_owner = owner_release;
    reject_retain = 1;
    assert(dm_sa_adapter_prepare(&sc, &sa) == DM_SA_PROVIDER_FAILED && !sa);
    assert(dm_vamm_adapter_prepare(&vc, &va) == DM_VAMM_INVALID && !va);
    assert(!sa_refs && !va_refs); reject_retain = 0;
    assert(dm_sa_adapter_prepare(&sc, &sa) == DM_SA_OK && sa);
    assert(dm_vamm_adapter_prepare(&vc, &va) == DM_VAMM_OK && va);
    assert(dm_sa_adapter_prepare(&sc, &sa2) == DM_SA_BUSY && !sa2);
    assert(dm_vamm_adapter_prepare(&vc, &va2) != DM_VAMM_OK && !va2);
    assert(dm_vamm_adapter_shutdown() != DM_VAMM_OK);
    original_slots();
    // Retained relocation hook pointers must reject ctor/init before publication.
    for (unsigned i = 0; i < 4; ++i) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) {
            unsigned char self[80] = {0};
            if (i == 0) ((void (*)(void *))dm_sa_adapter_prepared_hook(sa, 0))(self);
            if (i == 1) ((void (*)(void *, void *, size_t))dm_sa_adapter_prepared_hook(sa, 6))(self, NULL, 4096);
            if (i == 2) ((void (*)(void *))dm_vamm_adapter_prepared_hook(va, 8))(self);
            if (i == 3) ((void (*)(void))dm_vamm_adapter_prepared_hook(va, 0))();
            _exit(5);
        }
        int status; assert(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
    // Publication and stopped shutdown consume table-level context/owner refs
    // exactly once without destroying the caller-owned shared Vamm.
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        dm_sa_adapter_publish(sa); dm_vamm_adapter_publish(va);
        assert(sa_refs == 1 && va_refs == 1);
        assert(dm_sa_adapter_shutdown() == DM_SA_OK && !sa_refs);
        assert(dm_vamm_adapter_shutdown() == DM_VAMM_OK && !va_refs);
        dm_vamm_stats stats; assert(!dm_vamm_get_stats(vc.shared_owner, 0x10000, &stats));
        assert(!dm_vamm_destroy(vc.shared_owner));
        assert(dm_sa_adapter_lookup(name_at(0)) && dm_vamm_adapter_lookup(dm_vamm_adapter_symbol_name(0)));
        _exit(0);
    }
    int child_status; assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && !WEXITSTATUS(child_status));
    // Caller-owned census/config mutations cannot alter a prepared hook table.
    memset(ss, 0, sizeof ss); memset(vs, 0, sizeof vs);
    assert(dm_sa_adapter_prepared_hook(sa, 0) && dm_vamm_adapter_prepared_hook(va, 0));
    assert(!dm_sa_adapter_prepared_hook(sa, DM_SA_ADAPTER_SYMBOL_COUNT));
    // Keep valid config arrays for reentry checks in cancellation callbacks.
    for (unsigned i = 0; i < DM_SA_ADAPTER_SYMBOL_COUNT; ++i) ss[i] = (dm_sa_adapter_symbol){name_at(i), never};
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i) vs[i] = (dm_vamm_symbol){dm_vamm_adapter_symbol_name(i), never};
    dm_sa_adapter_cancel(sa); dm_vamm_adapter_cancel(va);
    assert(!sa_refs && !va_refs);
    dm_vamm_stats stats; assert(!dm_vamm_get_stats(vc.shared_owner, 0x10000, &stats));
    assert(!dm_vamm_destroy(vc.shared_owner));
    original_slots();
}
#endif
int main(void) {
    setup(); assert(!dm_memory_hooks_enter_guest()); rejected();
    assert(dm_memory_hooks_open_startup()); assert(!dm_memory_hooks_open_startup());
#ifdef DM_MEMORY_HOOKS_TESTING
    prepare_lifecycle();
    for (unsigned i = 0; i < N; ++i) { setup(); images[1].symbols[i + 1].st_shndx = 0; rejected(); }
    for (unsigned i = 0; i < 2 * N; ++i) { setup(); resolve_fail = (int)i; rejected(); }
    setup(); modules[0].nro = 1; rejected();
    setup(); modules[0].loaded = 0; rejected();
    setup(); strcpy(modules[0].name, "sdk"); rejected();
    setup(); modules[0] = modules[1]; rejected();
    setup(); images[0].symbols[1].st_shndx = 1; images[0].symbols[1].st_value = 8; rejected();
    setup(); images[1].symbols[N + 1] = images[1].symbols[1]; modules[1].nsym++; rejected();
    setup(); strcpy(images[1].strings + 7000, "_ZN2nn3mem17StandardAllocatorD1Ev");
    images[1].symbols[N + 1] = images[1].symbols[1]; images[1].symbols[N + 1].st_name = 7000; modules[1].nsym++; rejected();
    setup(); images[0].rela[2].r_addend = 1; rejected();
    setup(); images[0].rela[2].r_offset = 8; rejected();
    setup(); images[0].rela[2].r_offset = sizeof(image); rejected();
    setup(); images[0].rela[2].r_info = (UINT64_C(1) << 32) | 99; rejected();
    setup(); images[0].rela[2].r_info = ((uint64_t)(N + 1) << 32) | R_AARCH64_ABS64; rejected();
    setup(); images[0].rela[2].r_offset = images[0].rela[1].r_offset; rejected();
    setup(); images[0].relr[0] = images[0].rela[0].r_offset; rejected();
    setup(); images[0].dyn[1].d_un.d_val++; rejected();
    setup(); images[0].slots[2]++; before[0][2]++; rejected();
    setup(); images[0].symbols[2].st_info = STB_LOCAL << 4; rejected();
    setup(); images[0].symbols[2].st_name = UINT32_MAX; rejected();
    setup(); vc.owner.size = 1; rejected(); // SA preparation is canceled if Vamm fails.
    setup(); rollback = 1; rejected(); assert(before_publish == 1); // written slots restored in reverse order.
    setup(); pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { assert(dm_memory_hooks_enter_guest()); rejected(); _exit(0); }
    int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    // A defined constructor alias with no relocation callers is not an
    // installation gap. Exact observed slots are covered, the table is complete.
    for (unsigned unused = 0; unused < N; ++unused) {
        setup(); pid = fork(); assert(pid >= 0);
        if (!pid) {
            images[0].rela[unused].r_info = images[1].rela[unused].r_info = R_AARCH64_RELATIVE;
            assert(install() == DM_MEMORY_HOOKS_OK);
            assert(dm_memory_hooks_lookup(name_at(unused)));
            assert(images[0].slots[unused] == before[0][unused] && images[1].slots[unused] == before[1][unused]);
            _exit(0);
        }
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    setup(); assert(install() == DM_MEMORY_HOOKS_OK && before_publish == 1);
    assert(dm_memory_hooks_active());
    for (unsigned k = 0; k < 2; ++k) for (unsigned i = 0; i < N; ++i)
        assert(images[k].slots[i] == (uint64_t)dm_memory_hooks_lookup(name_at(i)));
    assert(install() != DM_MEMORY_HOOKS_OK);
    assert(dm_memory_hooks_enter_guest() && dm_memory_hooks_enter_guest());
    assert(install() == DM_MEMORY_HOOKS_BUSY);
    puts("memory hooks: actual relocation prepare/rollback/publication/startup fences passed");
#else
    assert(install() == DM_MEMORY_HOOKS_DISABLED); original_slots();
    assert(!resolve_calls && !before_publish);
    assert(dm_memory_hooks_enter_guest());
    puts("memory hooks: complete actual module census cannot bypass production gate");
#endif
    return 0;
}
