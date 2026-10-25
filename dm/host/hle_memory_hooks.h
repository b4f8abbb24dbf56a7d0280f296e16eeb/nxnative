// Startup-only relocation transaction. This is not a production activation API:
// both adapters retain independent compile-time gates, and no profile opts in.
#pragma once
#include "dm.h"
#include "hle_standard_allocator_adapter.h"
#include "hle_vamm_adapter.h"

enum { DM_MEMORY_HOOKS_OK, DM_MEMORY_HOOKS_INVALID, DM_MEMORY_HOOKS_BUSY,
       DM_MEMORY_HOOKS_DISABLED, DM_MEMORY_HOOKS_NO_MEMORY };
// Called once after all initial modules and deferred NROs have been mapped and
// ordinary relocation has finished. Guest entry permanently closes this window.
int dm_memory_hooks_open_startup(void);
int dm_memory_hooks_enter_guest(void); // false during loading/transaction
int dm_memory_hooks_active(void);
void *dm_memory_hooks_lookup(const char *name);
// Native loader resolver only, never guest code. Returning zero rejects the
// transaction; resolution is checked against each existing relocation slot.
typedef uint64_t (*dm_memory_hook_resolver)(module_t *, const Elf64_Sym *);
// Config symbol arrays are ignored: originals come from the actual module
// census. Both complete groups are required. This bounded version rejects ANY
// deferred NRO, duplicate definition, unknown class method, nonzero hook addend,
// unsupported relocation, or prior guest entry. No late ownership migration.
int dm_memory_hooks_install(module_t *modules, size_t count,
    const dm_sa_adapter_config *allocator, const dm_vamm_adapter_config *vamm,
    dm_memory_hook_resolver resolve);
// Actual loader entrypoint using the loaded process census and bind_sym.
int loader_install_memory_hooks(const dm_sa_adapter_config *, const dm_vamm_adapter_config *);
