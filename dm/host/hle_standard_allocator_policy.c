// Native policy shared by managed and original StandardAllocator dispatch.
// No SDK symbol lookup, guest TLS, registry or allocator lock is needed here.
#include "dm.h"
#include "hle_standard_allocator_adapter.h"
size_t dm_sa_host_allocation_alignment(void *self, size_t size) {
    (void)self;
    return DM_GAME_ALLOC_BIG_ALIGN && size >= 0x10000 ? 0x10000 : 16;
}
