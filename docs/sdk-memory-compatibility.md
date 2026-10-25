# SDK memory compatibility: mechanism versus activation

Scope: **40 node-area cuts + 6 alignment cuts in 20 SDK builds (46 entries)** in
[sdk_patches.py](../dm/tools/sdk_patches.py). All existing cuts remain enabled.
Game-specific arena cuts, audio and unrelated fixes are outside this scope.
Source-tested mechanisms are not verified SDK contracts or passed game tests.

## Implemented mechanisms

The native Vamm owner replaces SDK node metadata with a bounded host ledger,
provides exact contiguous guarded reservations, and owns combined allocation/map,
page operations, whole-region free and failure rollback. Its
[adapter](../dm/host/hle_vamm_adapter.c) covers all 18 reviewed public/detail/
constructor symbols. Constructor-captured identity avoids decoding an enclosing
SDK OS-resource layout.

The [placement implementation](../dm/host/hle_vamm.c) now has three immutable
native policies. One table drives allocation and realizable free-space
statistics, including threshold discontinuities and minimum-alignment queries:

| Request size | Modern | Legacy original | Legacy reduced |
|---|---:|---:|---:|
| Below 2 MiB | 64 KiB | 64 KiB | 64 KiB |
| 2 MiB to below 4 MiB | 2 MiB | 2 MiB | 2 MiB |
| 4 MiB to below 32 MiB | 2 MiB | 4 MiB | 4 MiB |
| 32 MiB to below 1 GiB | 2 MiB | 32 MiB | 32 MiB |
| At least 1 GiB | 2 MiB | 1 GiB | 32 MiB |

Legacy policies implement the alignment tiers documented in the existing patch
catalogue. Reduced legacy reproduces the six alignment cuts, not a modern 2 MiB
cap. They do not establish older placement-order, guard, ABI or result equivalence.
**No policy is automatically assigned to an unreviewed SDK.** The native
runtime explicitly keeps modern placement; invalid policy values reject owner
creation. Explicitly aligned native leases remain independent of SDK size tiers.

The [native domain bridge](../dm/host/hle_vamm_native.c) separately supports
explicit **alias-minus-extra** and **raw-alias** preparation. The former queries
GetInfo alias base/size and extra size, then subtracts extra size; the latter
queries only alias base/size and never queries or subtracts extra size. Both
validate raw bounds before publishing a configuration. Existing runtime callers
still request alias-minus-extra and modern placement. Domain policy is not a
version selector, hook permit or implicit placement choice.

The [runtime construction contract](shared-memory-runtime.md#explicit-immutable-construction-contracts)
now composes those domain and placement mechanisms for both full and owner-only
runtimes. The contract is copied/validated before backend queries and fixed for
the owner's lifetime. Existing default constructors remain alias-minus-extra plus modern placement.
[Startup descriptor selection](shared-memory-runtime.md#fail-closed-startup-contract-selection)
now explicitly supplies those same policies for the only admitted Z-A requests. This fills native construction
wiring, not the separate per-SDK ABI/ownership eligibility gap.

A separate [Vamm-only transaction](../dm/host/hle_memory_hooks.c)
intercepts the complete 18-symbol group while leaving all StandardAllocator
methods and existing bindings unchanged. Census, relocation journal, alias,
rollback and startup fences remain. This separates SDK-node ownership from the
40-hook Vamm plus StandardAllocator replacement. The [bounded native SDK-only
mode](bounded-sdk-memory-experiment.md) now connects this transaction to an
owner-only runtime and its own startup authorization. It remains default-off;
the full-group permit cannot authorize it, and its permit cannot prepare SA.
Only the same exact reviewed main + SDK 19.3.11 pair is eligible.

The existing [bounded experiment](bounded-shared-memory-experiment.md) remains
default-off and accepts only its exact full-ID Z-A base main + SDK 19.3.11
closure. No new full-ID target, hash flag or profile was added.

## Two supplied modern SDKs

SDK 19.3.11 and 20.5.8 have the same reviewed 18-symbol surface, bootstrap and
node-initialization ownership, modern alignment/guards, result translations,
resource return and allocation/free contracts. Their public wrappers place the
manager differently within the enclosing OS-resource object. Capturing the
constructor's actual `self` already handles that difference: no private offsets
or new layout adapter are needed. Relocated SVC/data references likewise do not
require build-specific native code.

This establishes SDK-side reuse only. **20.5.8 remains unauthorized** without
reviewed main and other module closure. Nearby versions are not inferred from
version numbers. SDK 9.6 is a structural reference, **not one of these 20 builds**.

## Legacy structural reference, not added coverage

The supplied SDK 9.6 uses the raw-alias domain rather than modern
alias-minus-extra. Its inspected paths match the legacy-original alignment
tiers, operation-specific result translations and resource-return ABI. This
supports an explicit domain mechanism, not selecting it for unreviewed SDKs.
The 20-build matrix and all activation eligibility below remain unchanged.

That reference also exports a weak reserved-region helper and weak
AddressRegionManager constructor in addition to the familiar 18 global exports.
The nonowning forwarded Vamm constructor uses the helper; only the intercepted
initializer calls the owner constructor in the inspected SDK paths. Therefore
**twenty hooks are not established as necessary**, and the extra weak exports
must not simply be appended to a global-only hook group. Future legacy
eligibility requires an explicit helper/owner-constructor census, binding and
call-closure review, including other modules and any direct/computed access.
The current 18-name census alone does not establish that closure. No extra hook
or permissive weak-binding rule is introduced here.

## Complete code-status matrix

`N` = node-area cut; `A` = alignment cut. IDs are repository inventory keys, not
execution authorization. Shared SDKs do not prove each game executes Vamm.

- **E:** reviewed modern contract; exact bounded Z-A base closure eligible only
  with explicit opt-in. Gameplay pending.
- **S:** reviewed modern SDK contract; no authorized whole-game closure.
- **L:** documented legacy alignment; placement mechanism/statistics implemented
  and source-tested. Complete older ABI/bootstrap/results unreviewed, not enabled.
- **U:** complete SDK contract unreviewed; potential adapter differences unknown,
  not merely an untested game and not a supported version.

| SDK build key | SDK | Cuts | Reference titles | Status |
|---|---|---|---|---|
| `56ec24747ec9e7f19b3efb062483a19d` | 12.3.7 | 2N | Fire Emblem Engage | L |
| `311deab1f062b504f3d646a49ddcd4e8` | 17.5.3 | 2N | Ace Attorney Investigations; Romance of the Three Kingdoms 8 | L |
| `12f161039300861f1054371f0e36b8c9` | 16.2.4 | 2N | Brain Memory Training Academy; Unicorn Overlord | U |
| `9c73fb73f529fedc9ffefa28246711cc` | 13.3.7 | 2N | Patrick's Parabox | U |
| `3644a6760469451cabc99f00c540acc2` | 13.3.5 | 2N | AI: The Somnium Files - nirvanA Initiative | L |
| `df7a3960c74c3a4803f826c587153252` | 19.3.11 | 2N | Pokemon Legends: Z-A | E |
| `e01950c58e17b080c90494784ea1dd29` | 20.5.8 | 2N | Hollow Knight: Silksong | S |
| `574284b40c06cf1821a44857939083be` | 20.5.22 | 2N | Breath of the Wild; Tears of the Kingdom | U |
| `8bfebccd6f5c1392597b364b9ab22a8d` | 20.5.19 | 2N | BALL x PIT | U |
| `6075151627016eef437c1312f2e3ed56` | 18.3.5 | 2N | Super Mario Galaxy 1/2; Monster Train 2 | L |
| `f118ae1c2e7728471aaf1a0d58484968` | 18.3.2 | 2N+A | Absolum; Balatro | L |
| `8618895246b420329b77746b960a66ab` | 15.3.2 | 2N+A | Paper Mario: The Thousand-Year Door; Celeste | L |
| `0023a1b06130cd795f7bc4613772af0a` | 20.5.17 | 2N | Final Fantasy Resonance; Steins;Gate Re:Boot | U |
| `145dde94fb574acc9e09780a11995bd3` | 19.3.1 | 2N | Dragon Quest VII Reimagined | U |
| `fcf93a41b08c5193a1410d262602dc62` | 7.3.2 | 2N+A | Dragon Quest XI S; Pokemon Sword; Raging Loop; Luigi's Mansion 3; Super Mario Odyssey | L |
| `5fd4f531c1195e9e5510c9e73f22cd39` | 8.2.1 | 2N+A | Super Smash Bros. Ultimate; Catherine: Full Body | L |
| `d9add76fc264863d85fbbd1d70076c76` | 10.4.1 | 2N+A | Advance Wars 1+2; Great Ace Attorney Chronicles | L |
| `30a1e87b52f5a2ac9879685cbfb93d7c` | 10.4.8 | 2N+A | Pikmin 4 | L |
| `7bba185a35ee9b4d0af66a4df910a823` | 19.3.13 | 2N | Octopath Traveler 0 | U |
| `5bcf525f24eb28658a8d67ac27800d9b` | 12.3.2 | 2N | Octopath Traveler II | U |

Two SDKs have reviewed modern contracts; **18 complete SDK contracts are still
unreviewed**, including the ten with documented legacy alignment. No alignment
cut does not imply modern semantics: some node-only older builds document
stronger request-size alignment too.

## Still code work, versus evidence and testing

1. **Implemented, not gameplay-validated:** distinct Vamm-only native startup,
   exact complete-module eligibility, mode-scoped authorization and owner-only
   runtime. Original SA/profile behavior remains intact. This exact base main
   still requests a contiguous 12 GiB arena: SDK-only mode must return no-space
   when that cannot fit, rather than acting as the full-group arena replacement.
2. **Evidence-gated adapter work:** exact API/alias census, detail and aggregate
   return ABI, constructor forwarding safety, direct/computed/inlined caller
   closure, GetInfo domain/resource contract, result/abort translations, guards,
   placement and rollback for the 18 unreviewed SDKs. Differences found will need
   concrete adapters; an alignment table or build-ID label is not that evidence.
3. **Evidence-gated policy selection:** determine where original legacy alignment
   must remain versus the existing reviewed 32 MiB relaxation. Both mechanisms
   exist, as do raw-alias and alias-minus-extra domain mechanisms; their exact
   combination still needs per-contract evidence. No unreviewed SDK is selected
   or enabled.
4. **Validation, not missing modern code:** Apple compilation/native AArch64 ABI,
   original SDK startup and actual gameplay. Source-only success establishes
   none of these and does not authorize removing a cut.

Minimum next input: **one raw unmodified `sdk` from Paper Mario: The
Thousand-Year Door 1.0.1, SDK 15.3.2**, inventory prefix
`8618895246b420329b77746b960a66ab`. A Celeste SDK with the same verified full
identity also suffices. Inspect SDK contracts first; matching game main/subsdk
closure comes later, before activation. No keys or credentials are needed.

## Reproducible source-only checks

```sh
make -C dm/host/tests vamm-placement vamm vamm-native vamm-native-domain vamm-gate memory-hooks memory-hooks-vamm-only memory-runtime-vamm-only memory-experiment sdk-memory-experiment memory-runtime-owner-only
ASAN_OPTIONS=detect_leaks=0 make -C dm/host/tests vamm-placement-sanitize vamm-sanitize vamm-native-sanitize vamm-native-domain-sanitize memory-hooks-sanitize memory-hooks-vamm-only-sanitize memory-runtime-vamm-only-sanitize memory-experiment-sanitize sdk-memory-experiment-sanitize memory-runtime-owner-only-sanitize
```

Placement tests use synthetic integer domains and independent exhaustive
page-step feasibility checks. Other fixtures use native ownership/SVC code and
synthetic ELF/SDK originals, including two synthetic enclosing layouts and
explicit rejection of SDK 20.5.8 by the unchanged exact-target gate. Private
inputs, derived bytes, offsets and disassembly are not in source, tests or CI.
The synthetic domain fixture traps any extra-size query in raw-alias mode and
checks nonzero/malformed extra sizes, boundary failures and unchanged outputs;
the native integration fixture also checks raw-domain configuration against the
real SVC backend without selecting it for current runtime startup.

The [SDK-only operational integration fixture](shared-memory-runtime.md#sdk-only-operational-integration-fixture) additionally executes all 18 wrappers over
the real runtime owner/SVC while synthetic original allocator imports remain
callable and unchanged. It does not execute a Nintendo StandardAllocator.
