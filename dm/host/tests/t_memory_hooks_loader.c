#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
// Apple-only fixture drives loader.c's actual ordinary relocation + forced HLE
// resolver, then its new ownership transaction. No guest instruction is run.
#define main portable_memory_hooks_main
#include "t_memory_hooks.c"
#undef main
#include "../loader.c"

struct dm_flags g_flag;
uint64_t now_ns(void) { return 0; }
void gate_register(uint64_t a, const char *k, int n, uint64_t o) { (void)a; (void)k; (void)n; (void)o; }
void hlecount_seal(void) {}
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
uint64_t resolve_import(module_t *m, const char *name) { (void)m; (void)name; assert(!"unexpected HLE/stub fallback"); return 0; }
int hle_implements(const char *name) { (void)name; return 0; }

int main(void) {
    setup();
    g_nmod = g_nloaded = 2;
    hle_force_ent forced = {.name = name_at(9), .fn = (void *)forward, .why = "synthetic established Allocate policy"};
    unsigned hits = 0; g_force = &forced; g_nforce = 1; g_force_hits = &hits;
    for (unsigned k = 0; k < 2; ++k) {
        g_mod[k] = modules[k]; g_loaded[k] = &g_mod[k]; build_symhash(&g_mod[k]);
        memset(images[k].slots, 0, sizeof images[k].slots);
    }
    for (unsigned k = 0; k < 2; ++k) relocate(&g_mod[k]);
    for (unsigned k = 0; k < 2; ++k) {
        for (unsigned i = 0; i < N; ++i) assert(images[k].slots[i] == target(i));
        memcpy(before[k], images[k].slots, sizeof before[k]);
    }
    assert(hits == 2 && dm_memory_hooks_open_startup());
    // Preflight detects a changed real symbol table without modifying a slot.
    images[1].symbols[1].st_shndx = 0;
    assert(loader_install_memory_hooks(&sc, &vc) != DM_MEMORY_HOOKS_OK); original_slots();
    images[1].symbols[1].st_shndx = 1;
    rollback = 1;
    assert(loader_install_memory_hooks(&sc, &vc) != DM_MEMORY_HOOKS_OK); original_slots();
    rollback = 0;
    assert(loader_install_memory_hooks(&sc, &vc) == DM_MEMORY_HOOKS_OK);
    for (unsigned k = 0; k < 2; ++k) for (unsigned i = 0; i < N; ++i) {
        int other, weak0;
        assert(images[k].slots[i] == (uint64_t)dm_memory_hooks_lookup(name_at(i)));
        assert(bind_sym(&g_mod[k], &images[k].symbols[i + 1], &other, &weak0) == images[k].slots[i]);
    }
    assert(dm_memory_hooks_enter_guest());
    assert(loader_install_memory_hooks(&sc, &vc) == DM_MEMORY_HOOKS_BUSY);
    for (unsigned k = 0; k < 2; ++k) free(g_mod[k].symh);
    puts("memory hooks: actual loader/HLE relocation, rollback and binding precedence passed");
    return 0;
}
