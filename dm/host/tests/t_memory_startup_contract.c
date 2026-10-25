// Synthetic descriptor validation and construction only. Neither this fixture's
// registry nor any private SDK is admitted; native startup activation stays off.
#define main runtime_contract_fixture_main
#include "t_memory_runtime_contract.c"
#undef main
#include "../hle_memory_experiment.c"

static void synthetic(startup_contract *c, startup_module_contract expected[2], module_t modules[2]) {
    *c = admitted_contracts[0];
    strcpy(c->request, "synthetic-not-admitted");
    c->modules = expected; c->max_regions = 17;
    memcpy(expected, za_modules, sizeof za_modules);
    memset(modules, 0, 2 * sizeof *modules);
    for (unsigned k = 0; k < 2; ++k) {
        // Invented exact identities, not activation entries for any real module.
        memset(expected[k].full_build_id, '1' + k, 64);
        strcpy(modules[k].name, expected[k].name);
        memcpy(modules[k].full_build_id, expected[k].full_build_id, 65);
        memcpy(modules[k].build_id, expected[k].full_build_id, 32);
        modules[k].base = (uint8_t *)(uintptr_t)(0x1000 + k * 0x1000);
        modules[k].size = 0x1000; modules[k].loaded = 1;
    }
}
static void rejected(startup_contract *c, const module_t *modules) {
    dm_memory_runtime *out = (void *)1;
    reset(0);
    assert(preflight_contract(c, modules, 2));
    assert(create_contract_runtime(c, &out) == DM_SA_INVALID && out == (void *)1);
    untouched();
}
static void malformed(void) {
    startup_contract c; startup_module_contract expected[2]; module_t modules[2];
#define FRESH() synthetic(&c, expected, modules)
#define BAD(statement) do { FRESH(); statement; rejected(&c, modules); } while (0)
    FRESH(); rejected(NULL, modules);
    BAD(c.mode = 2); BAD(c.mode = (unsigned)-1);
    BAD(c.runtime.domain = (dm_vamm_native_domain_policy)-1);
    BAD(c.runtime.placement = (dm_vamm_placement)3);
    BAD(c.max_regions = 0); BAD(c.max_regions = STARTUP_MAX_REGIONS + 1); BAD(c.max_regions = SIZE_MAX); BAD(c.module_count = 0); BAD(c.module_count = MAX_MOD + 1);
    BAD(c.modules = NULL); BAD(c.request[0] = 0); BAD(strcpy(c.request, "0"));
    BAD(memset(c.request, 'x', sizeof c.request));
    BAD(expected[0].name[0] = 0); BAD(memset(expected[0].name, 'x', sizeof expected[0].name));
    BAD(strcpy(expected[1].name, expected[0].name));
    BAD(expected[0].full_build_id[64] = 'x'); BAD(expected[0].full_build_id[2] = 0);
    BAD(expected[0].full_build_id[2] = 'g'); BAD(expected[0].full_build_id[2] = 'A');
#undef BAD
    FRESH(); reset(0);
    assert(!preflight_contract(&c, modules, 2));
    assert(preflight_contract(&c, NULL, 2));
    assert(preflight_contract(&c, modules, 1)); assert(preflight_contract(&c, modules, 3));
#define BAD_MODULE(statement) do { FRESH(); statement; assert(preflight_contract(&c, modules, 2)); untouched(); } while (0)
    BAD_MODULE(module_t swap = modules[0]; modules[0] = modules[1]; modules[1] = swap);
    BAD_MODULE(modules[1] = modules[0]); BAD_MODULE(strcpy(modules[1].name, "subsdk0"));
    BAD_MODULE(modules[1].nro = 1); BAD_MODULE(modules[1].loaded = 0); BAD_MODULE(modules[0].loads = 1);
    BAD_MODULE(modules[0].base = NULL); BAD_MODULE(modules[0].size = 0);
    BAD_MODULE(memset(modules[1].name, 'x', sizeof modules[1].name));
    BAD_MODULE(modules[1].full_build_id[63] = 'f'); BAD_MODULE(modules[1].full_build_id[64] = 'x');
    BAD_MODULE(modules[1].build_id[0] = 'f'); BAD_MODULE(modules[1].build_id[32] = 'x');
#undef BAD_MODULE
#undef FRESH
}
static void complete_module_set(void) {
    startup_contract c; startup_module_contract expected[3]; module_t modules[3];
    synthetic(&c, expected, modules);
    expected[2] = expected[1]; modules[2] = modules[1];
    strcpy(expected[2].name, "subsdk0"); strcpy(modules[2].name, "subsdk0");
    memset(expected[2].full_build_id, '3', 64);
    memcpy(modules[2].full_build_id, expected[2].full_build_id, 65);
    memcpy(modules[2].build_id, expected[2].full_build_id, 32);
    modules[2].base = (uint8_t *)(uintptr_t)0x3000;
    c.module_count = 3; reset(0);
    assert(!preflight_contract(&c, modules, 3));
    assert(preflight_contract(&c, modules, 2));
    modules[2].nro = 1; assert(preflight_contract(&c, modules, 3));
    modules[2].nro = 0; modules[2].full_build_id[63] = '4';
    assert(preflight_contract(&c, modules, 3));
    assert(!select_contract(c.request)); untouched();
}
static void registry(void) {
    assert(sizeof admitted_contracts / sizeof *admitted_contracts == 2);
    assert(!select_contract(NULL) && !select_contract("") && !select_contract("0"));
    assert(!select_contract("za-base-v1 ") && !select_contract("ZA-BASE-V1"));
    assert(!select_contract("synthetic-not-admitted") && !select_contract("sdk15"));
    for (unsigned mode = FULL_MEMORY; mode <= SDK_MEMORY; ++mode) {
        const startup_contract *c = select_contract(mode == FULL_MEMORY ? DM_MEMORY_EXPERIMENT_NAME : DM_SDK_MEMORY_EXPERIMENT_NAME);
        assert(c == &admitted_contracts[mode] && c->mode == mode && c->max_regions == 65536);
        assert(c->runtime.domain == DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA && c->runtime.placement == DM_VAMM_PLACEMENT_MODERN);
        assert(!dm_memory_experiment_authorized(NULL, c));
        assert(!dm_memory_experiment_authorized_sdk_only(NULL, c));
    }
}
static void construction(void) {
    for (unsigned mode = FULL_MEMORY; mode <= SDK_MEMORY; ++mode)
    for (unsigned domain = DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA; domain <= DM_VAMM_DOMAIN_RAW_ALIAS; ++domain)
    for (unsigned policy = DM_VAMM_PLACEMENT_MODERN; policy <= DM_VAMM_PLACEMENT_LEGACY_REDUCED; ++policy) {
        startup_contract c; startup_module_contract expected[2]; module_t modules[2];
        synthetic(&c, expected, modules);
        c.mode = mode; c.runtime = (dm_memory_runtime_contract){domain, policy};
        reset(domain == DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA);
        assert(!preflight_contract(&c, modules, 2)); untouched();
        dm_memory_runtime *r = NULL;
        assert(create_contract_runtime(&c, &r) == DM_SA_OK && r);
        assert(queries[2] == 1 && queries[3] == 1 && queries[28] == !domain);
        assert(provider_creates == (mode == FULL_MEMORY) && !changes);
        // Construction is not activation: no permit or guest initializer ran.
        assert(!dm_memory_experiment_authorized(NULL, r) && !dm_memory_experiment_authorized_sdk_only(NULL, r));
        dm_vamm_adapter_config v; dm_sa_adapter_config a;
        assert((mode == SDK_MEMORY ? dm_memory_runtime_vamm_config(r, &v) : dm_memory_runtime_configs(r, &v, &a)) == DM_SA_OK);
        assert(!v.experimental_startup && !v.proofs && !dm_vamm_is_initialized(v.shared_owner));
        assert((mode == SDK_MEMORY ? dm_memory_runtime_configs(r, &v, &a) : dm_memory_runtime_vamm_config(r, &v)) == DM_SA_INVALID);
        // Mutating the descriptor after construction cannot change the owner.
        c.runtime = (dm_memory_runtime_contract){(dm_vamm_native_domain_policy)-1, (dm_vamm_placement)-1};
        dm_vamm_initialize(v.shared_owner);
        for (size_t size = 0x1000; size <= 0x40000000; size *= 2) {
            uintptr_t address = 0;
            assert(!dm_vamm_allocate(v.shared_owner, &address, size, 0));
            assert(!(address % alignment(size, policy)));
            assert(!dm_vamm_free(v.shared_owner, address));
        }
        assert(dm_memory_runtime_destroy(r) == DM_SA_OK);
        assert(provider_destroys == provider_creates);
    }
    startup_contract c; startup_module_contract expected[2]; module_t modules[2];
    synthetic(&c, expected, modules); reset(1); fail_provider = 1;
    dm_memory_runtime *out = (void *)1;
    assert(create_contract_runtime(&c, &out) == DM_SA_NO_MEMORY && out == (void *)1);
    assert(provider_creates == 1 && !provider_destroys && !changes);
    reset(1); ++base;
    assert(create_contract_runtime(&c, &out) == DM_SA_INVALID && out == (void *)1);
    --base; assert(!provider_creates && !changes);
}
int main(void) {
    registry(); malformed(); complete_module_set(); construction();
    puts("startup contracts: exact registry, malformed/order/identity rejection, raw/legacy construction, mode separation, cleanup: PASS");
    return 0;
}
