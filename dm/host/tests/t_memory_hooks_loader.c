#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
// Apple-only fixture drives loader.c's actual ordinary relocation + forced HLE
// resolver and real counting stubs, then its new ownership transaction. No guest
// instruction is run: the executable counter probe calls only a host function.
#define main portable_memory_hooks_main
#include "t_memory_hooks.c"
#undef main
#include "../loader.c"
#include "../hle_count.c"
#include <mach/mach.h>
#include <mach/mach_vm.h>

struct dm_flags g_flag;
uint64_t now_ns(void) { return 0; }
void gate_register(uint64_t a, const char *k, int n, uint64_t o) { (void)a; (void)k; (void)n; (void)o; }
uint64_t resolve_import(module_t *m, const char *name) { (void)m; (void)name; assert(!"unexpected HLE/stub fallback"); return 0; }
int hle_implements(const char *name) { (void)name; return 0; }

static unsigned char sealed_code[HC_MAX * 48];
static uint64_t echo(uint64_t value) { return value ^ UINT64_C(0xfedcba9876543210); }
static uint64_t other_echo(uint64_t value) { return value; }
static void code_protection(vm_prot_t expected) {
    mach_vm_address_t address = (mach_vm_address_t)g_hcode;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    assert(mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
        (vm_region_info_t)&info, &count, &object) == KERN_SUCCESS);
    assert(address <= (mach_vm_address_t)g_hcode &&
        (mach_vm_address_t)g_hcode + sizeof sealed_code <= address + size);
    assert(info.protection == expected);
    if (object != MACH_PORT_NULL) assert(mach_port_deallocate(mach_task_self(), object) == KERN_SUCCESS);
}
static void code_unchanged(void) {
    assert(g_hsealed && g_nhc == 2);
    assert(!memcmp(sealed_code, g_hcode, sizeof sealed_code));
    code_protection(VM_PROT_READ | VM_PROT_EXECUTE);
}
int main(int argc, char **argv) {
    assert(argc == 1 || argc == 2 || (argc == 3 && !strcmp(argv[2], "--empty")));
    int counted = argc >= 2;
    if (counted) assert(!setenv("DM_HLECOUNT", argv[1], 1));
    else assert(!unsetenv("DM_HLECOUNT"));
    if (argc == 3) {
        hlecount_seal();
        assert(g_hsealed && !g_hcode && !g_nhc);
        assert(hlecount_wrap("HLE", "late_echo", (void *)echo) == (void *)echo);
        hlecount_flush();
        assert(!g_hcode && !g_nhc);
        puts("memory hooks: sealing an empty counter pool refuses later code generation");
        return 0;
    }
    setup();
    g_nmod = g_nloaded = 2;
    hle_force_ent forced = {.name = name_at(9), .fn = (void *)forward, .why = "synthetic established Allocate policy"};
    unsigned hits = 0; g_force = &forced; g_nforce = 1; g_force_hits = &hits;
    for (unsigned k = 0; k < 2; ++k) {
        g_mod[k] = modules[k]; g_loaded[k] = &g_mod[k]; build_symhash(&g_mod[k]);
        memset(images[k].slots, 0, sizeof images[k].slots);
    }
    for (unsigned k = 0; k < 2; ++k) relocate(&g_mod[k]);
    void *forced_target = hlecount_wrap("FORCE", name_at(9), (void *)forward);
    void *probe = hlecount_wrap("HLE", "fixture_echo", (void *)echo);
    assert((forced_target != (void *)forward) == counted);
    assert((probe != (void *)echo) == counted);
    for (unsigned k = 0; k < 2; ++k) {
        for (unsigned i = 0; i < N; ++i)
            assert(images[k].slots[i] == (i == 9 ? (uint64_t)forced_target : target(i)));
        memcpy(before[k], images[k].slots, sizeof before[k]);
    }
    assert(hits == 2 && dm_memory_hooks_open_startup());
    // Match load_modules: ordinary relocation opens startup, then seals counters
    // before a caller can request a memory-hook transaction.
    if (counted) code_protection(VM_PROT_READ | VM_PROT_WRITE);
    hlecount_seal();
    if (counted) {
        memcpy(sealed_code, g_hcode, sizeof sealed_code);
        code_unchanged();
        char same_name[] = "fixture_echo";
        assert(hlecount_wrap("HLE", same_name, (void *)echo) == probe);
        assert(hlecount_wrap("FORCE", name_at(9), (void *)forward) == forced_target);
        assert(((uint64_t (*)(uint64_t))probe)(123) == echo(123));
        assert(((uint64_t (*)(uint64_t))hlecount_wrap("HLE", same_name, (void *)echo))(456) == echo(456));
        assert(atomic_load(&g_hc[1].n) == 2);
        // A changed name or implementation is not a cache hit. Neither may
        // create a stub or reopen executable pages after sealing.
        assert(hlecount_wrap("HLE", "late_echo", (void *)echo) == (void *)echo);
        assert(hlecount_wrap("HLE", same_name, (void *)other_echo) == (void *)other_echo);
        assert(!hlecount_wrap("HLE", same_name, NULL));
        code_unchanged();
    }
    // Preflight detects a changed real symbol table without modifying a slot.
    images[1].symbols[1].st_shndx = 0;
    assert(loader_install_memory_hooks(&sc, &vc) != DM_MEMORY_HOOKS_OK); original_slots();
    images[1].symbols[1].st_shndx = 1;
    rollback = 1;
    assert(loader_install_memory_hooks(&sc, &vc) != DM_MEMORY_HOOKS_OK); original_slots();
    assert(before_publish == 1); // really reached writes and rollback, not an early wrapper mismatch
    rollback = 0;
    assert(loader_install_memory_hooks(&sc, &vc) == DM_MEMORY_HOOKS_OK);
    for (unsigned k = 0; k < 2; ++k) for (unsigned i = 0; i < N; ++i) {
        int other, weak0;
        assert(images[k].slots[i] == (uint64_t)dm_memory_hooks_lookup(name_at(i)));
        assert(bind_sym(&g_mod[k], &images[k].symbols[i + 1], &other, &weak0) == images[k].slots[i]);
    }
    assert(dm_memory_hooks_enter_guest());
    assert(loader_install_memory_hooks(&sc, &vc) == DM_MEMORY_HOOKS_BUSY);
    if (counted) {
        code_unchanged();
        hlecount_flush();
        FILE *report = fopen(argv[1], "r"); assert(report);
        char line[1024], expected[1024];
        assert(fgets(line, sizeof line, report) && !strcmp(line, "kind\tname\tcount\n"));
        snprintf(expected, sizeof expected, "FORCE\t%s\t0\n", name_at(9));
        assert(fgets(line, sizeof line, report) && !strcmp(line, expected));
        assert(fgets(line, sizeof line, report) && !strcmp(line, "HLE\tfixture_echo\t2\n"));
        assert(!fgets(line, sizeof line, report) && feof(report));
        assert(!fclose(report));
    } else assert(!g_hcode && !g_nhc);
    for (unsigned k = 0; k < 2; ++k) free(g_mod[k].symh);
    printf("memory hooks: actual loader/HLE relocation, sealed counters (%s), rollback and binding precedence passed\n", counted ? "enabled" : "disabled");
    return 0;
}
