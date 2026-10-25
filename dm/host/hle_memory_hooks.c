#include "hle_memory_hooks.h"
#include <stdatomic.h>

// Only a single startup writer may own the window. Reentry rejects rather than
// deadlocking; even a resolver callback cannot open guest entry while staging.
enum { COLD, READY, INSTALLING, RUNNING };
static _Atomic int phase, published;
int dm_memory_hooks_open_startup(void) {
    int expected = COLD;
    return atomic_compare_exchange_strong(&phase, &expected, READY);
}
int dm_memory_hooks_enter_guest(void) {
    int expected = READY;
    return atomic_compare_exchange_strong(&phase, &expected, RUNNING) || expected == RUNNING;
}
int dm_memory_hooks_active(void) { return atomic_load_explicit(&published, memory_order_acquire); }
void *dm_memory_hooks_lookup(const char *name) {
    if (!dm_memory_hooks_active()) return NULL;
    dm_sa_guest_fn sa = dm_sa_adapter_lookup(name);
    return sa ? (void *)sa : (void *)dm_vamm_adapter_lookup(name);
}
enum { TOTAL = DM_SA_ADAPTER_SYMBOL_COUNT + DM_VAMM_SYMBOL_COUNT };
static const char *name_at(size_t i) {
    return i < DM_SA_ADAPTER_SYMBOL_COUNT ? dm_sa_adapter_symbol_name(i) :
        dm_vamm_adapter_symbol_name(i - DM_SA_ADAPTER_SYMBOL_COUNT);
}
static int index_of(const char *name) {
    if (!name) return -1;
    for (unsigned i = 0; i < TOTAL; ++i) if (!strcmp(name, name_at(i))) return (int)i;
    return -1;
}
static int owned_member(const char *name, const char *owner_name) {
    // Itanium nested names encode the owning class before the member, whereas
    // unrelated functions may mention this class only in a parameter type.
    // Do not reject those service exports as new allocator/manager methods.
    if (strncmp(name, "_ZN", 3)) return 0;
    name += 3;
    while (*name && strchr("rVKRO", *name)) ++name; // cv/ref qualifiers
    return !strncmp(name, owner_name, strlen(owner_name));
}
static int inside(const module_t *m, uint64_t offset, size_t size) {
    return offset <= m->size && size <= m->size - offset;
}
static int pointer_inside(const module_t *m, const void *p, size_t size) {
    return (uintptr_t)p >= (uintptr_t)m->base &&
        inside(m, (uintptr_t)p - (uintptr_t)m->base, size);
}
static const char *symbol_name(const module_t *m, const Elf64_Sym *s) {
    size_t start = (uintptr_t)m->strtab - (uintptr_t)m->base;
    if (start >= m->size || s->st_name >= m->size - start) return NULL;
    const char *name = m->strtab + s->st_name;
    return memchr(name, 0, m->size - start - s->st_name) ? name : NULL;
}
typedef struct { uint64_t *slot, before, after; } write_entry;
typedef struct { const Elf64_Rela *tab[2]; size_t count[2]; const uint64_t *relr; size_t nrelr; } relocations;
static int relocation_tables(module_t *m, relocations *r) {
    uint64_t offset[3] = {0}, size[3] = {0}; unsigned seen = 0;
    if (!pointer_inside(m, m->dyn, sizeof *m->dyn)) return 0;
    for (const Elf64_Dyn *d = m->dyn;; ++d) {
        if (!pointer_inside(m, d, sizeof *d)) return 0;
        if (!d->d_tag) break;
        int key = -1;
        switch (d->d_tag) {
        case DT_RELA: key = 0; break; case DT_RELASZ: key = 1; break;
        case DT_JMPREL: key = 2; break; case DT_PLTRELSZ: key = 3; break;
        case DT_RELR: key = 4; break; case DT_RELRSZ: key = 5; break;
        }
        if (key < 0) continue;
        if (seen & (1u << key)) return 0;
        seen |= 1u << key;
        if (key & 1) size[key / 2] = d->d_un.d_val; else offset[key / 2] = d->d_un.d_ptr;
    }
    for (unsigned i = 0; i < 3; ++i) {
        size_t unit = i == 2 ? sizeof(uint64_t) : sizeof(Elf64_Rela);
        if (size[i] % unit || offset[i] % 8 || !inside(m, offset[i], size[i])) return 0;
        if (i == 2) { r->relr = (const uint64_t *)(m->base + offset[i]); r->nrelr = size[i] / unit; }
        else { r->tab[i] = (const Elf64_Rela *)(m->base + offset[i]); r->count[i] = size[i] / unit; }
    }
    return 1;
}
static int writable_slot(const module_t *m, uint64_t offset) {
    // Mapped text, patch instructions and the host key page are never writable
    // transaction targets. The loader's ro/data image is copy-on-write on iOS.
    return !(offset % 8) && offset >= m->ro_off && inside(m, offset, 8) &&
        (!m->key_page_off || offset + 8 <= m->key_page_off) &&
        (m->patch_off <= 0 || offset + 8 <= (uint64_t)m->patch_off);
}
static int overlaps(const write_entry *journal, size_t count, const uint64_t *slot) {
    for (size_t i = 0; i < count; ++i) if (journal[i].slot == slot) return 1;
    return 0;
}
static int relative_slots_clear(module_t *m, const relocations *r, const write_entry *journal, size_t count) {
    uint64_t next = 0; int started = 0;
    for (size_t i = 0; i < r->nrelr; ++i) {
        uint64_t bits = r->relr[i];
        if (!(bits & 1)) {
            if (!writable_slot(m, bits) || overlaps(journal, count, (uint64_t *)(m->base + bits))) return 0;
            next = bits + 8; started = 1;
        } else {
            if (!started || next > UINT64_MAX - 63 * 8) return 0;
            for (unsigned b = 0; b < 63; ++b) if (bits & (UINT64_C(1) << (b + 1))) {
                uint64_t at = next + b * 8;
                if (!writable_slot(m, at) || overlaps(journal, count, (uint64_t *)(m->base + at))) return 0;
            }
            next += 63 * 8;
        }
    }
    return 1;
}
int dm_memory_hooks_install(module_t *modules, size_t count,
    const dm_sa_adapter_config *allocator, const dm_vamm_adapter_config *vamm,
    dm_memory_hook_resolver resolve) {
    int expected = READY;
    if (!atomic_compare_exchange_strong(&phase, &expected, INSTALLING)) return DM_MEMORY_HOOKS_BUSY;
    int result = DM_MEMORY_HOOKS_INVALID;
    dm_sa_adapter_prepared *sa = NULL; dm_vamm_adapter_prepared *va = NULL;
    write_entry *journal = NULL; size_t writes = 0, capacity = 0;
    relocations tables[MAX_MOD];
    dm_sa_adapter_symbol sas[DM_SA_ADAPTER_SYMBOL_COUNT] = {0};
    dm_vamm_symbol vas[DM_VAMM_SYMBOL_COUNT] = {0};
    module_t *definition_owner = NULL;
    if (dm_memory_hooks_active() || !modules || !count || count > MAX_MOD || !allocator || !vamm || !resolve) goto done;
    for (size_t k = 0; k < count; ++k) {
        module_t *m = &modules[k];
        if (!m->base || m->nro || !m->loaded || !m->size || m->size > UINTPTR_MAX - (uintptr_t)m->base ||
            m->text_end > m->ro_off || m->ro_off > m->size ||
            m->nsym > m->size / sizeof(Elf64_Sym) || !pointer_inside(m, m->symtab, m->nsym * sizeof(Elf64_Sym)) ||
            !pointer_inside(m, m->strtab, 1) || !memchr(m->name, 0, sizeof m->name) || !relocation_tables(m, &tables[k])) goto done;
        for (size_t j = 0; j < k; ++j) {
            module_t *n = &modules[j];
            if (!strcmp(m->name, n->name) || ((uintptr_t)m->base < (uintptr_t)n->base + n->size &&
                (uintptr_t)n->base < (uintptr_t)m->base + m->size)) goto done;
        }
        for (unsigned i = 1; i < m->nsym; ++i) {
            const Elf64_Sym *s = &m->symtab[i]; const char *name = symbol_name(m, s);
            if (!name) goto done;
            int index = index_of(name);
            if (index < 0) {
                if (owned_member(name, "2nn3mem17StandardAllocator") ||
                    owned_member(name, "2nn2os6detail11VammManager")) goto done;
                continue;
            }
            if (!s->st_shndx) continue;
            if (ELF64_ST_BIND(s->st_info) != 1 || (s->st_info & 15) != 2 ||
                ELF64_ST_VISIBILITY(s->st_other) != STV_DEFAULT || !s->st_value || s->st_value >= m->text_end) goto done;
            if (definition_owner && definition_owner != m) goto done;
            definition_owner = m;
            if (index < (int)DM_SA_ADAPTER_SYMBOL_COUNT) {
                if (sas[index].function) goto done;
                sas[index] = (dm_sa_adapter_symbol){name_at(index), (dm_sa_guest_fn)(m->base + s->st_value)};
            } else {
                index -= DM_SA_ADAPTER_SYMBOL_COUNT;
                if (vas[index].function) goto done;
                vas[index] = (dm_vamm_symbol){dm_vamm_adapter_symbol_name(index), (dm_vamm_guest_fn)(m->base + s->st_value)};
            }
        }
        for (unsigned t = 0; t < 2; ++t) {
            if (tables[k].count[t] > SIZE_MAX / sizeof(write_entry) - capacity) goto done;
            capacity += tables[k].count[t];
        }
    }
    // Resolution failures and missing definitions are not HLE/stub fallbacks.
    for (unsigned i = 0; i < DM_SA_ADAPTER_SYMBOL_COUNT; ++i) if (!sas[i].function) goto done;
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i) if (!vas[i].function) goto done;
    dm_sa_adapter_config sc = *allocator; sc.symbols = sas; sc.symbol_count = DM_SA_ADAPTER_SYMBOL_COUNT;
    dm_vamm_adapter_config vc = *vamm; vc.symbols = vas; vc.symbol_count = DM_VAMM_SYMBOL_COUNT;
    if (dm_sa_adapter_prepare(&sc, &sa) != DM_SA_OK || dm_vamm_adapter_prepare(&vc, &va)) {
        result = DM_MEMORY_HOOKS_DISABLED; goto done;
    }
    journal = calloc(capacity ? capacity : 1, sizeof *journal);
    if (!journal) { result = DM_MEMORY_HOOKS_NO_MEMORY; goto done; }
    for (size_t k = 0; k < count; ++k) {
        module_t *m = &modules[k]; relocations *rl = &tables[k];
        for (unsigned t = 0; t < 2; ++t) for (size_t i = 0; i < rl->count[t]; ++i) {
            const Elf64_Rela *r = &rl->tab[t][i]; unsigned type = ELF64_R_TYPE(r->r_info);
            if (!writable_slot(m, r->r_offset)) goto done;
            if (type == R_AARCH64_RELATIVE) continue;
            if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT) goto done;
            if (ELF64_R_SYM(r->r_info) >= m->nsym) goto done;
            const Elf64_Sym *s = &m->symtab[ELF64_R_SYM(r->r_info)]; const char *name = symbol_name(m, s);
            if (!name) goto done;
            int index = index_of(name);
            if (index < 0) continue;
            if (r->r_addend || ELF64_ST_BIND(s->st_info) != 1 || ELF64_ST_VISIBILITY(s->st_other) != STV_DEFAULT) goto done;
            uint64_t *slot = (uint64_t *)(m->base + r->r_offset), original = resolve(m, s);
            if (!original || *slot != original || overlaps(journal, writes, slot)) goto done;
            uint64_t hook = index < (int)DM_SA_ADAPTER_SYMBOL_COUNT ? (uint64_t)dm_sa_adapter_prepared_hook(sa, index) :
                (uint64_t)dm_vamm_adapter_prepared_hook(va, index - DM_SA_ADAPTER_SYMBOL_COUNT);
            if (!hook) goto done;
            journal[writes++] = (write_entry){slot, original, hook};
        }
    }
    // Check aliases with every non-group RELA and packed-relative slot too.
    for (size_t k = 0; k < count; ++k) {
        module_t *m = &modules[k]; relocations *rl = &tables[k];
        if (!relative_slots_clear(m, rl, journal, writes)) goto done;
        for (unsigned t = 0; t < 2; ++t) for (size_t i = 0; i < rl->count[t]; ++i) {
            const Elf64_Rela *r = &rl->tab[t][i];
            if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE &&
                index_of(symbol_name(m, &m->symtab[ELF64_R_SYM(r->r_info)])) >= 0) continue;
            if (overlaps(journal, writes, (uint64_t *)(m->base + r->r_offset))) goto done;
        }
    }
    // From here no allocation, resolver or guest callback can run. The startup
    // window excludes execution while these ordinary writable slots are staged.
    for (size_t i = 0; i < writes; ++i) *journal[i].slot = journal[i].after;
#ifdef DM_MEMORY_HOOKS_TESTING
    extern int dm_memory_hooks_test_before_publish(void);
    if (!dm_memory_hooks_test_before_publish()) {
        for (size_t i = writes; i; --i) *journal[i - 1].slot = journal[i - 1].before;
        goto done;
    }
#endif
    dm_vamm_adapter_publish(va); va = NULL;
    dm_sa_adapter_publish(sa); sa = NULL;
    atomic_store_explicit(&published, 1, memory_order_release);
    result = DM_MEMORY_HOOKS_OK;
done:
    if (va) dm_vamm_adapter_cancel(va);
    if (sa) dm_sa_adapter_cancel(sa);
    free(journal);
    atomic_store_explicit(&phase, READY, memory_order_release);
    return result;
}
