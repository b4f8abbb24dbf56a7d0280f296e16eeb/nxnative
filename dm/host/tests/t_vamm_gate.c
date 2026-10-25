#include "hle_vamm_adapter.h"
#include <assert.h>
#include <stdio.h>
static void never(void) { assert(0); }
static uint32_t pages(void *context, uintptr_t base, size_t size, int commit) {
    (void)context; (void)base; (void)size; (void)commit; assert(0); return 1;
}
static dm_vamm_resource_usage resources(void *context) {
    (void)context; assert(0); return (dm_vamm_resource_usage){0, 0};
}
int main(void) {
    dm_vamm_symbol symbols[DM_VAMM_SYMBOL_COUNT];
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i)
        symbols[i] = (dm_vamm_symbol){dm_vamm_adapter_symbol_name(i), never};
    dm_vamm_adapter_config c = {.proofs = DM_VAMM_PROOF_ALL, .installed_bindings = DM_VAMM_ALL_BINDINGS,
        .symbols = symbols, .symbol_count = DM_VAMM_SYMBOL_COUNT,
        .owner = {.base = 0x10000, .size = 0x100000, .max_regions = 32, .change_pages = pages, .resources = resources}};
    assert(dm_vamm_adapter_install(&c) == DM_VAMM_INVALID);
    for (unsigned i = 0; i < DM_VAMM_SYMBOL_COUNT; ++i) assert(!dm_vamm_adapter_lookup(symbols[i].name));
    assert(dm_vamm_adapter_shutdown() == DM_VAMM_INVALID);
    puts("vamm: production gate remains closed"); return 0;
}
