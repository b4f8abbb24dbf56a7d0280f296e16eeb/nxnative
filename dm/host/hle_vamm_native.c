#include "hle_vamm_native.h"
#include <stdio.h>
#include <stdlib.h>

uint32_t dm_vamm_native_decode_domain(uint64_t alias_base, uint64_t alias_size,
                                      uint64_t extra_size, uintptr_t *base, size_t *size) {
    if (!base || !size || !alias_base || !alias_size ||
        ((alias_base | alias_size | extra_size) & 0xfff) ||
        alias_base > UINTPTR_MAX || alias_size > SIZE_MAX ||
        alias_size > UINTPTR_MAX - alias_base || extra_size >= alias_size)
        return DM_VAMM_INVALID;
    *base = (uintptr_t)alias_base;
    *size = (size_t)(alias_size - extra_size);
    return DM_VAMM_OK;
}

uint32_t dm_vamm_native_svc_result(uint32_t result, int commit) {
    if (commit == 0 || commit == 1) {
        if (!result) return DM_VAMM_OK;
        // Verified map (combined/pages) and unmap (free/pages) translations.
        // 0xd001 is OutOfMemory; 0xd401 is InvalidCurrentMemory, NOT the same
        // error. The latter is recoverable only for unmap, fatal for map.
        // Like nn::Result comparisons, ignore the upper result context bits.
        switch (result & 0x3fffff) {
        case 0xce01: return 0x1203; // OutOfResource
        case 0xd001: case 0x10801: return DM_VAMM_NO_MEMORY;
        case 0xd401: if (!commit) return 0x803; break; // InvalidCurrentMemoryState
        }
    }
    fprintf(stderr, "[RED] Vamm native %s unexpected SVC result %#x\n",
            commit ? "map" : "unmap", result);
    abort();
}

static uint32_t native_pages(void *context, uintptr_t base, size_t size, int commit) {
    (void)context;
    return dm_vamm_native_svc_result(dm_vamm_svc_change_pages(base, size, commit), commit);
}
static dm_vamm_resource_usage native_resources(void *context) {
    (void)context;
    // The owner adds the bounded 512 KiB headroom exactly once, not here.
    return (dm_vamm_resource_usage){dm_vamm_svc_info(16), dm_vamm_svc_info(17)};
}
uint32_t dm_vamm_native_configure_domain(dm_vamm_config *out, size_t max_regions,
                                         dm_vamm_native_domain_policy policy) {
    if (!out || !max_regions || (policy != DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA &&
                                policy != DM_VAMM_DOMAIN_RAW_ALIAS))
        return DM_VAMM_INVALID;
    dm_vamm_config c = {.max_regions = max_regions,
        .change_pages = native_pages, .resources = native_resources,
        // The reviewed runtime remains modern. Legacy placement mechanisms
        // must not imply ABI/bootstrap eligibility or enable another SDK.
        .placement = DM_VAMM_PLACEMENT_MODERN};
    uint64_t alias_base = dm_vamm_svc_info(2), alias_size = dm_vamm_svc_info(3);
    // Legacy raw-alias callers must not even query the extra-size service.
    uint64_t extra_size = policy == DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA ?
        dm_vamm_svc_info(28) : 0;
    uint32_t result = dm_vamm_native_decode_domain(alias_base, alias_size,
                                                  extra_size, &c.base, &c.size);
    if (!result) *out = c;
    return result;
}
uint32_t dm_vamm_native_configure(dm_vamm_config *out, size_t max_regions) {
    return dm_vamm_native_configure_domain(out, max_regions,
                                            DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA);
}
