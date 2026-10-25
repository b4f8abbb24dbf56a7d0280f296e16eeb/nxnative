// Source-only ELF fixture. SDK-only interception does not imply target approval.
#define _POSIX_C_SOURCE 200809L
#include "hle_memory_hooks.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

enum { SA = DM_SA_ADAPTER_SYMBOL_COUNT, VA = DM_VAMM_SYMBOL_COUNT, N = SA + VA };
typedef struct {
    unsigned char text[256];
    Elf64_Sym symbols[N + 2];
    char strings[8192];
    Elf64_Dyn dyn[7];
    Elf64_Rela rela[N + 1];
    uint64_t slots[N + 2], relr[1];
} image;
static image images[2];
static module_t modules[2];
static uint64_t before[2][N + 2];
static dm_vamm_adapter_config vc;
static int resolve_calls, resolve_fail, rollback, before_publish;
#ifdef DM_VAMM_ONLY_GATE_TEST
static unsigned authorization_calls;
static unsigned sdk_authorization_calls;
int dm_memory_experiment_authorized(const struct dm_memory_experiment_authorization *a, const void *context) {
    (void)a; (void)context; ++authorization_calls;
    // The gate must reject without even consulting a permissive test authority.
    return 1;
}
int dm_memory_experiment_authorized_sdk_only(const struct dm_memory_experiment_authorization *a, const void *context) {
    (void)a; (void)context; ++sdk_authorization_calls;
    return 0; // The permissive full-group test authority cannot grant this mode.
}
#endif
static const char *name_at(unsigned i) {
    return i < SA ? dm_sa_adapter_symbol_name(i) : dm_vamm_adapter_symbol_name(i - SA);
}
static uint32_t pages(void *c, uintptr_t p, size_t n, int commit) {
    (void)c; (void)p; (void)n; (void)commit;
    assert(!"page callback before guest entry"); return 1;
}
static dm_vamm_resource_usage resources(void *c) {
    (void)c; assert(!"resource callback before guest entry"); return (dm_vamm_resource_usage){0};
}
static uint64_t target(unsigned i) { return (uint64_t)(images[1].text + 8 + 4 * i); }
static uint64_t resolver(module_t *m, const Elf64_Sym *s);
static int install(void) { return dm_memory_hooks_install_vamm_only(modules, 2, &vc, resolver); }
static uint64_t resolver(module_t *m, const Elf64_Sym *s) {
    assert(!dm_memory_hooks_active() && !dm_memory_hooks_enter_guest());
    assert(install() == DM_MEMORY_HOOKS_BUSY);
    assert(dm_memory_hooks_install(modules, 2, NULL, &vc, resolver) == DM_MEMORY_HOOKS_BUSY);
    int call = resolve_calls++;
    if (call == resolve_fail) return 0;
    for (unsigned i = SA; i < N; ++i)
        if (!strcmp(m->strtab + s->st_name, name_at(i))) return target(i);
    assert(!"unselected allocator method was resolved"); return 0;
}
static void original_slots(void) {
    assert(!dm_memory_hooks_active());
    for (unsigned k = 0; k < 2; ++k) assert(!memcmp(before[k], images[k].slots, sizeof before[k]));
    for (unsigned i = 0; i < N; ++i)
        assert(!dm_memory_hooks_lookup(name_at(i)) && !dm_sa_adapter_lookup(name_at(i)) && !dm_vamm_adapter_lookup(name_at(i)));
}
int dm_memory_hooks_test_before_publish(void) {
    ++before_publish;
    assert(!dm_memory_hooks_active() && !dm_memory_hooks_enter_guest());
    assert(install() == DM_MEMORY_HOOKS_BUSY);
    for (unsigned k = 0; k < 2; ++k) for (unsigned i = 0; i < N + 2; ++i) {
        if (i < SA || i >= N || ELF64_R_TYPE(images[k].rela[i].r_info) == R_AARCH64_RELATIVE)
            assert(images[k].slots[i] == before[k][i]);
        else assert(images[k].slots[i] != before[k][i]);
    }
    for (unsigned i = 0; i < N; ++i)
        assert(!dm_sa_adapter_lookup(name_at(i)) && !dm_vamm_adapter_lookup(name_at(i)));
    return !rollback;
}
static void setup(void) {
    memset(images, 0, sizeof images); memset(modules, 0, sizeof modules);
    resolve_calls = before_publish = rollback = 0; resolve_fail = -1;
    vc = (dm_vamm_adapter_config){.proofs = DM_VAMM_PROOF_ALL,
        .owner = {.base = 0x10000, .size = 0x100000, .max_regions = 32,
            .change_pages = pages, .resources = resources}};
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
                .r_info = ((uint64_t)(i + 1) << 32) |
                    (i % 3 == 0 ? R_AARCH64_JUMP_SLOT : i % 3 == 1 ? R_AARCH64_GLOB_DAT : R_AARCH64_ABS64)};
            im->slots[i] = target(i);
        }
        im->dyn[0] = (Elf64_Dyn){.d_tag = DT_RELA, .d_un.d_ptr = offsetof(image, rela)};
        im->dyn[1] = (Elf64_Dyn){.d_tag = DT_RELASZ, .d_un.d_val = 20 * sizeof(Elf64_Rela)};
        im->dyn[2] = (Elf64_Dyn){.d_tag = DT_JMPREL, .d_un.d_ptr = offsetof(image, rela) + 20 * sizeof(Elf64_Rela)};
        im->dyn[3] = (Elf64_Dyn){.d_tag = DT_PLTRELSZ, .d_un.d_val = (N - 20) * sizeof(Elf64_Rela)};
        im->dyn[4] = (Elf64_Dyn){.d_tag = DT_RELR, .d_un.d_ptr = offsetof(image, relr)};
        im->dyn[5] = (Elf64_Dyn){.d_tag = DT_RELRSZ, .d_un.d_val = 8};
        im->relr[0] = offsetof(image, slots) + (N + 1) * 8;
        im->slots[N] = 1234; im->slots[N + 1] = 5678;
        memcpy(before[k], im->slots, sizeof before[k]);
    }
}
static void rejected(void) { assert(install() != DM_MEMORY_HOOKS_OK); original_slots(); }
#if defined(DM_MEMORY_HOOKS_TESTING) && \
    (!defined(DM_EXPERIMENTAL_SHARED_MEMORY) || !DM_EXPERIMENTAL_SHARED_MEMORY)
static void success_child(void) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        assert(install() == DM_MEMORY_HOOKS_OK && before_publish == 1 && resolve_calls == 2 * VA);
        assert(dm_memory_hooks_active());
        for (unsigned k = 0; k < 2; ++k) for (unsigned i = 0; i < N + 2; ++i) {
            if (i >= SA && i < N) assert(images[k].slots[i] == (uint64_t)dm_memory_hooks_lookup(name_at(i)));
            else assert(images[k].slots[i] == before[k][i]);
        }
        for (unsigned i = 0; i < SA; ++i)
            assert(!dm_memory_hooks_lookup(name_at(i)) && !dm_sa_adapter_lookup(name_at(i)));
        assert(install() != DM_MEMORY_HOOKS_OK);
        assert(dm_memory_hooks_enter_guest() && install() == DM_MEMORY_HOOKS_BUSY);
        assert(!dm_vamm_adapter_shutdown());
        _exit(0);
    }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    original_slots();
}
static void extra_symbol(const char *name) {
    image *im = &images[0];
    strcpy(im->strings + 7000, name);
    im->symbols[N + 1] = (Elf64_Sym){.st_name = 7000, .st_info = 0x12, .st_shndx = 1, .st_value = 8};
    modules[0].nsym++;
    im->rela[N] = (Elf64_Rela){.r_offset = offsetof(image, slots) + N * 8,
        .r_info = ((uint64_t)(N + 1) << 32) | R_AARCH64_ABS64};
    im->dyn[3].d_un.d_val += sizeof(Elf64_Rela);
}
#endif
int main(void) {
    alarm(60); setup(); rejected();
    assert(dm_memory_hooks_open_startup());
#if defined(DM_MEMORY_HOOKS_TESTING) && \
    (!defined(DM_EXPERIMENTAL_SHARED_MEMORY) || !DM_EXPERIMENTAL_SHARED_MEMORY)
    for (unsigned i = SA; i < N; ++i) { setup(); images[1].symbols[i + 1].st_shndx = 0; rejected(); }
    for (unsigned i = 0; i < 2 * VA; ++i) { setup(); resolve_fail = (int)i; rejected(); }
    setup(); vc.owner.size = 1; rejected();
    setup(); modules[0].nro = 1; rejected();
    setup(); images[0].symbols[SA + 1].st_shndx = 1; images[0].symbols[SA + 1].st_value = 8; rejected();
    setup(); extra_symbol("_ZN2nn2os6detail11VammManagerD1Ev"); rejected();
    setup(); images[0].rela[SA].r_addend = 1; rejected();
    setup(); images[0].slots[SA]++; before[0][SA]++; rejected();
    setup(); images[0].rela[SA].r_offset = images[0].rela[SA + 1].r_offset; rejected();
    setup(); images[0].relr[0] = images[0].rela[SA].r_offset; rejected();
    // Selected Vamm writes may not alias known, unknown, or relative untouched slots.
    setup(); images[0].rela[SA].r_offset = images[0].rela[0].r_offset;
    images[0].slots[0] = before[0][0] = target(SA); rejected();
    setup(); extra_symbol("_ZN2nn3mem17StandardAllocatorD1Ev");
    images[0].rela[SA].r_offset = images[0].rela[N].r_offset;
    images[0].slots[N] = before[0][N] = target(SA); rejected();
    setup(); images[0].rela[0].r_info = R_AARCH64_RELATIVE;
    images[0].rela[SA].r_offset = images[0].rela[0].r_offset;
    images[0].slots[0] = before[0][0] = target(SA); rejected();
    setup(); rollback = 1; rejected(); assert(before_publish == 1);
    // A failed staging transaction leaves the same startup window retryable.
    rollback = 0; resolve_calls = before_publish = 0; success_child();
    setup(); success_child();
    // No StandardAllocator definitions or imports are needed by this mode.
    setup();
    for (unsigned k = 0; k < 2; ++k) for (unsigned i = 0; i < SA; ++i) {
        images[k].symbols[i + 1] = (Elf64_Sym){0};
        images[k].rela[i].r_info = R_AARCH64_RELATIVE;
    }
    success_child();
    // New/duplicate allocator methods are outside the selected ownership group.
    setup(); extra_symbol("_ZN2nn3mem17StandardAllocatorD1Ev"); success_child();
    setup(); images[0].symbols[1].st_shndx = 1; images[0].symbols[1].st_value = 8; success_child();
    assert(dm_memory_hooks_enter_guest()); rejected();
    puts("Vamm-only hooks: full census, allocator isolation, aliases, rollback and startup fences passed");
#else
    assert(install() == DM_MEMORY_HOOKS_DISABLED);
    // A nonnull opaque authorization is not an SDK-only activation capability.
    vc.experimental_startup = (const struct dm_memory_experiment_authorization *)&vc;
    assert(install() == DM_MEMORY_HOOKS_DISABLED);
    assert(!resolve_calls && !before_publish); original_slots();
#ifdef DM_VAMM_ONLY_GATE_TEST
    assert(!authorization_calls);
    assert(sdk_authorization_calls);
#endif
    assert(dm_memory_hooks_enter_guest());
    puts("Vamm-only hooks: default/full-group authority cannot enable SDK-only transaction");
#endif
    return 0;
}
