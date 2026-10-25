// Synthetic SVC boundary: prove domain selection/query behavior independently
// of the host's current extra-size value. No SDK bytes, profiles or activation.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../hle_vamm_native.h"

static uint64_t alias_base, alias_size, extra_size;
static unsigned queries[32], page_calls;
static int allow_extra;
uint64_t dm_vamm_svc_info(unsigned id) {
    assert(id < 32); ++queries[id];
    switch (id) {
    case 2: return alias_base;
    case 3: return alias_size;
    case 28: assert(allow_extra); return extra_size;
    case 16: return 0x800000;
    case 17: return 0x4000;
    default: assert(0); return 0;
    }
}
uint32_t dm_vamm_svc_change_pages(uintptr_t base, size_t size, int commit) {
    assert(base == 0x200000 && size == 0x1000 && (commit == 0 || commit == 1));
    ++page_calls; return 0;
}
static void reset(uint64_t base, uint64_t size, uint64_t extra, int modern) {
    alias_base = base; alias_size = size; extra_size = extra; allow_extra = modern;
    memset(queries, 0, sizeof queries); page_calls = 0;
}
static void queried(int configured, int modern) {
    for (unsigned i = 0; i < 32; ++i)
        assert(queries[i] == (unsigned)(configured &&
            (i == 2 || i == 3 || (modern && i == 28))));
    assert(!page_calls);
}
static void check(uint64_t base, uint64_t size, uint64_t extra,
                  dm_vamm_native_domain_policy policy, uint32_t expected) {
    int modern = policy == DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA;
    reset(base, size, extra, modern);
    dm_vamm_config c, before; memset(&c, 0xa5, sizeof c); memcpy(&before, &c, sizeof c);
    assert(dm_vamm_native_configure_domain(&c, 17, policy) == expected);
    queried(1, modern);
    if (expected) {
        assert(!memcmp(&before, &c, sizeof c)); return;
    }
    assert(c.base == base && c.size == size - (modern ? extra : 0));
    assert(c.max_regions == 17 && !c.context && c.change_pages && c.resources);
    assert(c.placement == DM_VAMM_PLACEMENT_MODERN); // independent from domain
    assert(!c.change_pages(c.context, 0x200000, 0x1000, 1));
    assert(!c.change_pages(c.context, 0x200000, 0x1000, 0) && page_calls == 2);
    dm_vamm_resource_usage r = c.resources(c.context);
    assert(r.total == 0x800000 && r.used == 0x4000); // raw, no owner headroom
    assert(queries[16] == 1 && queries[17] == 1);
}
static void invalid_arguments(void) {
    const dm_vamm_native_domain_policy policies[] = {
        DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA, DM_VAMM_DOMAIN_RAW_ALIAS,
        (dm_vamm_native_domain_policy)-1, (dm_vamm_native_domain_policy)2
    };
    for (unsigned i = 0; i < sizeof policies / sizeof policies[0]; ++i) {
        dm_vamm_config c, before; memset(&c, 0xa5, sizeof c); memcpy(&before, &c, sizeof c);
        reset(0x100000, 0x200000, 0x4000, 0);
        assert(dm_vamm_native_configure_domain(NULL, 17, policies[i]) == DM_VAMM_INVALID);
        assert(dm_vamm_native_configure_domain(&c, 0, policies[i]) == DM_VAMM_INVALID);
        if (i >= 2)
            assert(dm_vamm_native_configure_domain(&c, 17, policies[i]) == DM_VAMM_INVALID);
        queried(0, 0); assert(!memcmp(&before, &c, sizeof c));
    }
}
int main(void) {
    invalid_arguments();
    const uint64_t extras[] = {0, 0x4000, 0x1ff000, 0x200000, 0x201000, 1, UINT64_MAX};
    for (unsigned i = 0; i < sizeof extras / sizeof extras[0]; ++i) {
        // Raw mode succeeds even if querying extra size would be unavailable,
        // malformed, or remove the entire region. The query itself is trapped.
        check(0x100000, 0x200000, extras[i], DM_VAMM_DOMAIN_RAW_ALIAS, DM_VAMM_OK);
        check(0x100000, 0x200000, extras[i], DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA,
              i < 3 ? DM_VAMM_OK : DM_VAMM_INVALID);
    }
    const uint64_t invalid[][2] = {
        {0, 0x200000}, {0x100001, 0x200000}, {0x100000, 0}, {0x100000, 0x200001},
        {UINT64_MAX & ~UINT64_C(0xfff), 0x1000},
        {0x1000, UINT64_MAX & ~UINT64_C(0xfff)}
    };
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        for (unsigned p = 0; p <= 1; ++p)
            check(invalid[i][0], invalid[i][1], 0x1000,
                  (dm_vamm_native_domain_policy)p, DM_VAMM_INVALID);
    // Minimal nonempty raw domain, and high valid endpoint, remain exact.
    check(0x1000, 0x1000, UINT64_MAX, DM_VAMM_DOMAIN_RAW_ALIAS, DM_VAMM_OK);
    if (UINTPTR_MAX == UINT64_MAX && SIZE_MAX == UINT64_MAX) {
        check((UINT64_MAX & ~UINT64_C(0xfff)) - 0x1000, 0x1000, 1,
              DM_VAMM_DOMAIN_RAW_ALIAS, DM_VAMM_OK);
    }
    // Existing callers retain the explicit modern contract, including failure.
    reset(0x100000, 0x200000, 0x4000, 1);
    dm_vamm_config c; assert(!dm_vamm_native_configure(&c, 17));
    queried(1, 1); assert(c.base == 0x100000 && c.size == 0x1fc000);
    assert(c.placement == DM_VAMM_PLACEMENT_MODERN);
    reset(0x100000, 0x200000, 0x200000, 1);
    dm_vamm_config before; memcpy(&before, &c, sizeof c);
    assert(dm_vamm_native_configure(&c, 17) == DM_VAMM_INVALID);
    queried(1, 1); assert(!memcmp(&before, &c, sizeof c));
    puts("native Vamm domain policies: PASS");
    return 0;
}
