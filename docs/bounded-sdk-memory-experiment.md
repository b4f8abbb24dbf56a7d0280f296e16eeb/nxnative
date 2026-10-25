# Bounded native SDK-only memory experiment

**Default-off, source-tested, not a successful Z-A boot claim.** This mode
replaces only the SDK Vamm reservation owner. It leaves the original
StandardAllocator, CentralHeap, cache paths, object layout, allocation behavior
and existing profile-alignment shim in place. All SDK/game patches remain enabled.

## Exact scope and important limitation

The eligible complete module set and full NSO IDs are exactly the reviewed
`main` then `sdk` pair listed in the [full-group experiment](bounded-shared-memory-experiment.md#deliberate-opt-in).
No new SDK identity, title, update, NRO or module ordering is authorized. The
supplied SDK 20.5.8 is not enabled by matching SDK-side semantics.

The retained original allocator in this base main requests **one contiguous
12 GiB arena**. If the real alias domain is smaller, SDK-only mode must return
no-space with the output and ownership unchanged. It never caps a successful
reservation, splits it, or substitutes a StandardAllocator core. The original
SDK will then take its initialization-failure path. This mode isolates SDK-node
support; it is **not a substitute for the full 40-hook mode's large-arena work**.
Do not describe successful hook installation as game startup or gameplay success.

## Separate opt-in

A development host is compiled with the existing explicit build switch:

```sh
make -C dm/host GAME=0100F43008C44000 EXPERIMENTAL_SHARED_MEMORY=1
DM_EXPERIMENTAL_SHARED_MEMORY=za-base-sdk-v1 dm/host/dmhost /path/to/rewritten-base main sdk
```

This command is for the exact reviewed pair and SDK-focused diagnostics, not a
recommendation to replace the existing gameplay candidate. Missing, empty or `0`
runtime values retain the SDK owner. `za-base-v1` still selects the original full
group. Unknown values, mismatched IDs, extra/deferred modules and repeats fail
closed. No game profile or packaging default enables either mode.

## Evidence for retained allocator paths

Static inspection of supplied SDK 19.3.11 traced original StandardAllocator,
CentralHeap, cached-heap and platform-memory helpers. Reserve/free/map/unmap and
enabled queries reach the intercepted public Vamm imports. The expanded
allocator/helper scan found no direct Vamm body/interior entry, address formation
for Vamm ownership storage or access through the resource-manager pointer.
Cached-heap indirect dispatch resolves to allocator-local methods; query branches
are local tables and Walk is a caller callback. No additional Vamm owner was
found in these retained ordinary allocation paths.

This is bounded static evidence for this exact supplied SDK and reviewed main,
not universal proof against arbitrary computed pointers or a gameplay result.
Private bytes, offsets and disassembly are not included in source/tests/CI.

The actual SDK/main symbol and relocation census was also checked against the
coordinator's predicates. Unrelated service exports accepting a
`StandardAllocator*` exposed a real false-positive in the old substring check.
The corrected check recognizes the mangled owning class, including cv/ref
qualifiers, while continuing to reject genuine unknown members. It does not
whitelist those service names. Zero-reference constructor/public aliases are
valid exports, not missing relocation coverage.

## Native authority and lifetime

After exact preflight, startup creates one owner-only runtime (65,536 reservation
records) and no SA provider. `loader_install_vamm_hooks` uses the actual loader
resolver and commits the complete 18-name Vamm group with the same journal,
relocation-alias and startup fences as full mode. All existing SA slots remain
unchanged, including the established profile-alignment forwarding shim.

Only the live opaque SDK-only permit authorizes this transaction. It does **not**
authorize `dm_sa_adapter_prepare` or the full 40-hook transaction. Conversely,
the full-group permit cannot select SDK-only mode. After the installation call,
both authorization predicates reject the expired permit. No proof bit or SDK
hash flag substitutes for this authority.

Failed prepublication creation/configuration/resolution cancels retained state
and destroys the unused owner. Successful publication is process-lifetime;
shutdown does not restore SDK ownership or free a live context. Logs explicitly
say `experimental SDK-only memory`, identify retained StandardAllocator/profile
behavior, and warn about the unchanged 12 GiB startup request. Full-mode logs
retain their existing `experimental shared memory` identity.

## Source-only validation

```sh
make -C dm/host/tests sdk-memory-experiment memory-runtime-owner-only
ASAN_OPTIONS=detect_leaks=0 make -C dm/host/tests sdk-memory-experiment-sanitize memory-runtime-owner-only-sanitize
```

The real-authorization fixture uses no adapter/hook fixture activation macros.
It covers default-off/explicit-zero, exact IDs/order, missing/unknown members,
forged/wrong-context/expired and cross-mode authority, all selected resolver
failure positions, retained-owner cleanup, original callable SA dispatch, and
honest 12 GiB no-space failure. Original SA calls are synthetic host functions:
no Nintendo allocator implementation is executed. Enabled tests cover native
and simulated 16 KiB page behavior. ASan/UBSan with leak detection disabled is
not an LSan pass. Apple builds, original SDK execution and gameplay remain
separate validation requirements.
