// Built WITHOUT DM_SA_ADAPTER_TESTING. Proof bits alone cannot activate an
// unsupported guest ABI, even when the advertised symbol group is complete.
#include "hle_standard_allocator_adapter.h"
#include <assert.h>
#include <stdio.h>
static void original(void) { assert(!"production gate called an original"); }
static void *forward_allocate(void *s,size_t n) { (void)s;(void)n; assert(!"production gate forwarded"); return NULL; }
static size_t allocation_alignment(void *s,size_t n) { (void)s;(void)n; assert(!"production gate called policy"); return 16; }
static dm_sa_result make_config(void *c,void *s,size_t n,unsigned cache,dm_sa_config *out) {
    (void)c;(void)s;(void)n;(void)cache;(void)out;
    assert(!"production gate called provider"); return DM_SA_INVALID;
}
int main(void) {
    dm_sa_adapter_symbol symbols[DM_SA_ADAPTER_SYMBOL_COUNT];
    for(size_t n=0;n<DM_SA_ADAPTER_SYMBOL_COUNT;n++) symbols[n]=(dm_sa_adapter_symbol){dm_sa_adapter_symbol_name(n),original};
    dm_sa_adapter_config c={.proofs=DM_SA_PROOF_ALL,.symbols=symbols,
        .symbol_count=DM_SA_ADAPTER_SYMBOL_COUNT,.installed_bindings=DM_SA_ADAPTER_ALL_BINDINGS,.forward_allocate=forward_allocate,.allocation_alignment=allocation_alignment,.make_config=make_config};
    assert(dm_sa_adapter_install(&c)==DM_SA_INVALID);
    unsigned char self[56]={0}; void *out=self;
    for(size_t n=0;n<DM_SA_ADAPTER_SYMBOL_COUNT;n++) assert(!dm_sa_adapter_lookup(symbols[n].name));
    assert(!dm_sa_adapter_is_managed(self));
    assert(!dm_sa_adapter_allocate_if_managed(self,16,16,&out) && out==self);
    puts("StandardAllocator production activation remains closed");
}
