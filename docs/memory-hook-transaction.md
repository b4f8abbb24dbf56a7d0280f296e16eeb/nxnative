# Startup memory-hook transaction (production disabled)

This removes the circular requirement to claim installed bindings before an
adapter exposes hook addresses. It does **not** remove patches, enable a profile,
or establish that any shipping game is safe to hand over.

## Real boundary

`loader_install_memory_hooks` uses `loader.c`'s actual `bind_sym` resolver and
mapped `module_t` array. `hle_memory_hooks.c` reads the ELF symbol, RELA, JMPREL
and RELR data used by ordinary relocation. It does not rewrite instructions,
change permissions, invent offsets, or accept a caller-supplied installed mask.

1. After ordinary relocation and NRO discovery, the loader opens one startup
   window. The first `guest_main` entry closes it **before** thread initialization,
   Oodle tool-mode execution, or any SDK entry. `run_module_inits` independently
   checks that no transaction is in flight. Reentry/concurrent install rejects.
2. The transaction requires one defining module for all 22 StandardAllocator and
   18 Vamm names, with ordinary global/default/function definitions. It rejects
   duplicate names or overlapping module images, additional class methods,
   missing/duplicate definitions, any deferred NRO, and unloaded modules.
3. It captures originals from those definitions and prepares both adapters.
   Preparation copies immutable tables and preallocates the Vamm owner. It does
   not invoke a constructor, factory, resource callback, or SDK initializer.
4. Each actual group relocation must be ABS64/GLOB_DAT/JUMP_SLOT with zero
   addend, a valid writable non-code slot, default visibility and a resolvable
   original matching that slot's current value. Unreferenced exported aliases do not
   require fabricated caller relocations; the published table still covers them. Overlapping group/non-group/packed-relative targets, malformed
   tables and unsupported types fail before writes. All real original slot
   values (including the existing forced Allocate policy shim) are journaled.
5. After complete preflight, it writes every staged hook slot. There are no
   further fallible allocations, resolutions, or guest callbacks. It publishes
   both adapter states and then the loader's lookup table, still excluding guest
   execution. Tests inject a failure at this last prepublication point and verify
   reverse-order restoration of every original slot, with both adapters inactive.
6. Once published, ownership cannot be rolled back to the SDK. Late NRO load or
   unload is explicitly refused. Both adapters' production preparation gates
   remain unconditional failures, and no production caller installs the group.

The coordinator is exclusive startup code, not a concurrent patching primitive.
Prepared lease pointers are private to that transaction and are consumed by
publish/cancel; callers must not reuse or concurrently manipulate a lease.
`guest_sym` deliberately still returns original definitions for established HLE
forwarding. The loader's normal relocation resolver gives the published memory
group precedence over forced HLE entries. This is not a promise that arbitrary
private symbol lookup, computed calls or inline object access is intercepted.

`DM_HLECOUNT` currently returns unwrapped targets after counting pages are sealed.
If that differs from a slot's existing wrapper, preflight rejects installation
without modifying it. There is no guessed wrapper unwrapping or special bypass.

## Native lifetime contract, not permission to enable

The adapters now expose explicit lifetime pairs for the native factory, without
a global owner getter. A Vamm config can carry one `shared_owner` and paired
native `retain_owner`/`release_owner` callbacks. Preparation retains that exact
owner (never creates another), checks it has no regions, and cancellation or
stopped shutdown releases the handle without destroying caller-owned storage.
The [native runtime](shared-memory-runtime.md) implements the retain callback
by requiring an owner that has never been initialized/used. Its provider routes
allocation, free and statistics to this same retained owner and rejects use
before bootstrap/init.

StandardAllocator preparation can retain its table context. Cancellation or
stopped shutdown releases it. Each successful `make_config` transfers a complete
configuration to the adapter; `release_config` runs after later validation/core
creation failure, or after successful core destruction. Failed Finalize keeps
both core and configuration for retry. Release callbacks run outside registry
locks while the instance still rejects reentry. A failed factory owns cleanup
of its own partial resources. Shutdown refuses live/in-flight instances, retains
interception, and makes all hooks abort before accessing released context.

The native factory uses these pairs, preserves exact requested logical capacity
and the established alignment policy, provides the exact native domain, and
keeps its shared provider alive until both tables and all configs/leases have
drained. Its native context predicate checks the retained owner's readiness;
it is not a constant-true guest-thread assertion. Configurations that call SDK
originals still need a verified guest-context contract. These lifetime APIs do
not establish target no-prior-owner, caller or ABI proofs.

The startup transaction cannot infer no-prior-owner, private initialization
paths, main/subsdk direct/inline accesses, ABI behavior, shutdown paths, or device
correctness from symbol names. Those remain blockers. NRO support would require
its own proven transactional load/unload design; this bounded version refuses
such processes instead of allowing partial ownership. All existing gameplay,
allocator-arena and SDK-node/alignment cuts remain enabled.

## Tests

```
make -C dm/host/tests memory-hooks
ASAN_OPTIONS=detect_leaks=0 make -C dm/host/tests memory-hooks-sanitize
# Apple host only: actual loader.c / forced-HLE resolver, synthetic ELF images
make -C dm/host/tests GAME=<profile-id> memory-hooks-loader
```

Portable tests use the actual transaction and both adapters: each missing method,
each resolver-failure position, duplicate/overlapping modules, extra methods,
NROs, zero-reference aliases, malformed/overlapping relocations, invalid owner
configuration, cancellation, prepublication rollback, prepared constructor/init
rejection, reentry, table/config retain-release lifetimes and post-entry installation rejection. They run with and
without fixture macros; a complete real-shaped census cannot bypass production.

The Apple fixture additionally drives ordinary `relocate`, the existing forced
Allocate policy, and `loader_install_memory_hooks`, then verifies published
`bind_sym` precedence. It is part of the Apple host aggregate, not claimed as run
on Linux. No proprietary binaries or fixed private offsets are included.
