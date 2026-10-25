# Native shared-memory runtime wiring (gated)

`dm/host/hle_memory_runtime.[ch]` connects the native Vamm reservation owner,
the exact-lease StandardAllocator provider, both adapter tables, and the real
upstream bitmap-backed SVC transaction. Default production adapter preparation **remains disabled**, including complete
proof masks. A separate [bounded native experiment](bounded-shared-memory-experiment.md)
requires both build/runtime opt-in and the exact reviewed full main+SDK identities.
No SDK/game patch, profile, private structure offset or game binary is changed.

## One explicit owner, one lifetime

`dm_memory_runtime_create` prepares one Vamm over the exact native GetInfo
2/3/28 domain and one provider. Creation neither initializes Vamm nor reserves
or commits guest pages. `dm_memory_runtime_configs` returns fresh adapter
settings containing that same owner handle, native retention callbacks, and
the StandardAllocator configuration factory. It does not supply a symbol
census or fabricate caller/ABI/behavior proofs.

### Explicit immutable construction contracts

`dm_memory_runtime_create_with_contract` and
`dm_memory_runtime_create_vamm_only_with_contract` compose the independently
implemented native domain and SDK placement mechanisms. The contract contains
only `domain` (alias-minus-extra or raw alias) and `placement` (modern, legacy
original or legacy reduced). Both selectors are copied once and validated
before native GetInfo queries; the owner copies the resulting configuration.
No caller-owned contract pointer or mutable policy is retained. Invalid/null
contracts, null output and zero record capacity reject before querying the
backend. Every construction failure preserves the output pointer and unwinds
any unused owner/provider. Existing metadata-capacity bounds remain enforced
by owner creation; they are not a new zero-query guarantee for every count.

Both existing constructors remain explicit **alias-minus-extra + modern**
wrappers, including all current experimental startup callers. Full and
owner-only runtimes keep their separate configuration/lifetime capabilities.
Selecting mechanisms neither authorizes a hook transaction nor establishes a
reviewed SDK contract. No allowlist, permit, environment mode, profile or patch
changes accompany these APIs.

The selected placement governs SDK-facing reservations. Managed allocator
leases still use exact caller alignment through `dm_vamm_allocate_aligned`,
paired with `dm_vamm_get_aligned_stats`; SDK size tiers must not silently alter
either native lease placement or its reported feasibility. A large lazy
StandardAllocator capacity remains a limit rather than a contiguous arena.

Source-only checks:

```sh
make -C dm/host/tests memory-runtime-contract
ASAN_OPTIONS=detect_leaks=0 make -C dm/host/tests memory-runtime-contract-sanitize
```

The contract fixture covers both runtime types across all six domain/placement
combinations, modern wrappers, invalid-input ordering, caller-contract mutation,
provider failure cleanup and explicit aligned-provider callbacks/statistics.
The mixed fixture runs all six combinations with real native owner/provider/SVC
code at native and simulated 16 KiB host-page sizes. It combines a 40 GiB lazy
managed allocator with an independently reserved sparse range committed at
both ends, shared physical-budget exhaustion, unchanged-output no-space,
provided-buffer child forwarding and borrowed-source free failure/retention.
Guest originals are synthetic; these are not Paper Mario/Absolum ABI or gameplay
tests, and no SDK is newly enabled.

The Vamm adapter retains the explicit owner capability, rather than using a
global getter or creating another reservation manager. Retention rejects an
initialized, reserved or already-retained owner. Only the adapter's verified
bootstrap and Initialize paths initialize the host owner; the SDK reservation
initializer is never called. The StandardAllocator factory rejects use before
both table retentions and host-owner initialization, or when system resources
are disabled. Callers must not directly initialize or mutate the owner carried
in adapter settings; it is an adapter capability, not a parallel allocator API.

Each successful factory result records its exact object identity and requested
capacity. The adapter releases that registration after failed core creation or
successful core destruction, **not** after failed Finalize. Both pending/active
adapter tables separately retain the runtime context, even with no live cores.
Cancellation releases these pins without guest calls or mappings. Shutdown
keeps hooks intercepted and prohibits use of released contexts; it never
reactivates SDK ownership.

Runtime destruction requires stopped guest execution and exclusive lifecycle
ownership. It refuses table pins, factory registrations (including zero-backing
cores), provider leases and native reservations. A returning decommit rejection,
such as a borrowed MapMemory source, cannot destroy the provider or forget its
owner. Host protection failure instead follows upstream's fatal policy.
Synthetic/exclusive-lifecycle callers drain cores, shut down both adapters, then
destroy the runtime. The bounded native experiment does not have that teardown
boundary: it retains ownership until process exit and refuses adapter shutdown.
An initialization/census/relocation failure before publication instead cancels
both prepared tables before destruction.

## Native callbacks, real budgets

All provider callbacks installed by this runtime are native Vamm operations.
They do not call SDK originals, manufacture guest TLS, or run SDK code on host
workers. The provider's existing context predicate now checks the retained,
initialized native owner; it is deliberately not a permissive guest-TLS check.
The only guest callbacks remaining in this integration are the adapter's
separately verified bootstrap/forwarding paths, outside native locks.

The existing provider drops its SVC source guard before owner calls and restores
it before publishing or reusing a lease. Owner callbacks run outside the SVC
scope. Native page operations hold the Vamm owner lock, then acquire upstream's
single `g_map_lock` for source ownership, the physical bitmap and accounting.
There is no separate page manager or physical lock. No native owner operation
calls back into the allocator or runtime; each core serializes its own metadata.

A multi-GiB StandardAllocator capacity is an exact lazy upper bound, not a
reservation or physical-memory claim. Reservation statistics come from the
actual Vamm ledger, including guards, alignment and metadata exhaustion; the
provider clamps them against real guarded physical budget. Its thread-local
strict allocation scope also enforces that budget atomically when a legacy
profile permits unlimited ordinary mappings. System-resource telemetry still
comes from the native raw GetInfo values with Vamm's existing headroom rule.

`hle_standard_allocator_policy.c` shares the existing `alloc_big_align` policy
between managed and original size-only Allocate dispatch. This native policy
has no SDK symbol lookup or TLS dependency. `hle_os.c` exports an original-only
forwarder for SDK-owned objects; managed allocation failure cannot fall through
to it. The native provider supplies an explicit-alignment owner operation:
`dm_vamm_allocate_aligned` leases exactly the requested page-granular payload
at any legal power-of-two alignment that fits a real guarded interval. It does
not pad a mapping, return an interior pointer, shrink a request, or combine
fragmented holes. `dm_vamm_get_aligned_stats` uses the same placement rule before
physical-budget clamping. The SDK-facing `dm_vamm_allocate` retains its original
size-dependent alignment (64 KiB below 2 MiB, 2 MiB otherwise); native leases
use their explicit requested alignment instead. Original-SDK providers without
that verified callback remain limited to their truthful fixed guarantee.
Full guest behavior/call-coverage compatibility remains an activation prerequisite.

## Verification

```sh
make -C dm/host/tests memory-runtime
make -C dm/host/tests memory-runtime-sanitize
make -C dm/host/tests memory-runtime-tsan
```

The native harness uses real mmap-backed SVC storage, the runtime factory,
provider, Vamm, both adapters, and the actual host alignment policy. Only guest
bootstrap/original forwarding and unrelated platform services are synthetic.
It tests native and simulated 16 KiB host pages, both map-limit policies, disabled
system resources, the unconditional production gate, pending cancellation,
pre-initialization rejection, core-create rollback, lazy 62 GiB capacity,
reservation sharing, physical exhaustion, borrowed-source retention, failed
Finalize/retry, concurrent cores, and table/config lifetime ordering. Cold
128 KiB, 2 MiB, 8 MiB and 16 MiB alignments cover small/large payloads, exact
backing and guards, impossible requests, moving reallocation with preserved
alignment/data, borrowed-source rejection, and realizable largest-span statistics.
Direct Vamm tests additionally cover fragmented intervals, address-limit overflow,
metadata exhaustion and unchanged SDK-default alignment.

The normal suite is included in the existing host `run` target. Sanitizer
variants retain the same page and budget matrix. On ptrace-restricted Linux,
`ASAN_OPTIONS=detect_leaks=0` is necessary for ASan/UBSan; that is not a leak-check
pass. Portable 16 KiB simulation does not replace Apple hardware validation,
and these tests do not establish the missing SDK/game call-coverage proofs.

## Synthetic startup-to-teardown integration

`make -C dm/host/tests memory-runtime-startup` uses a synthetic ELF module census
and real runtime configuration, loader transaction and committed relocation slots.
Constructor entry points are tiny native test trampolines, not proprietary SDK
code. The fixture covers a live allocator lease through native ownership, physical
budget exhaustion, borrowed-source retention, fatal protection failure, shutdown
and cached post-shutdown Vamm hooks. Six normal variants include limited/unlimited
budgets, 4/16 KiB page behavior, disabled resources and the production gate.
`memory-runtime-startup-sanitize` adds five ASan/UBSan variants. This is synthetic
startup coverage, not proof that any title's complete SDK caller set is intercepted.

## Owner-only runtime

`dm_memory_runtime_create_vamm_only` creates only the native Vamm owner, with no
StandardAllocator provider or guest callbacks. `dm_memory_runtime_vamm_config`
returns only that owner's lifetime configuration. Full-mode `configs` rejects an
owner-only runtime without changing outputs; the owner-only config API rejects
a full-mode runtime. Destruction still rejects retained/live ownership and skips
the absent provider. `memory-runtime-owner-only` and its sanitizer target verify
that provider creation/interface/guest-forwarding functions are never invoked.

The [bounded SDK-only experiment](bounded-sdk-memory-experiment.md) uses these
APIs and its own native startup permit. `sdk-memory-experiment` exercises the real
production authorization without fixture activation macros, including full/SDK
permit separation, original SA bindings and exact oversized-request failure.

## SDK-only operational integration fixture

`make -C dm/host/tests memory-runtime-vamm-only` independently exercises the
fixture-authorized Vamm transaction without `DM_SA_ADAPTER_TESTING`. The six variants
cover 4/16 KiB page behavior, limited/unlimited direct-SVC mapping policy, disabled
system resources and the production gate. `memory-runtime-vamm-only-sanitize`
runs the five enabled variants under ASan/UBSan; on restricted Linux use
`ASAN_OPTIONS=detect_leaks=0` (not a leak-check pass).

The synthetic ELF images preserve every original StandardAllocator slot and
never prepare, capture or retain an allocator adapter table. Calls through those
unchanged slots exercise a synthetic original constructor, the fixture's
original-only size-allocation shim, and synthetic original allocate/free methods
that call the committed Vamm imports. These originals model dispatch only: they
are not Nintendo SDK implementations or evidence of game allocator compatibility.
All 18 Vamm wrapper paths execute over the existing runtime's one retained native
owner and actual bitmap-backed SVC storage; no second reservation manager or
replacement StandardAllocator core is used. Both constructor aliases are covered
in separate processes because a live manager identity cannot be replaced.

The same test verifies cancellation at all 36 selected resolver positions,
prepublication rollback, original-slot preservation, owner-only retention,
borrowed-source failure/retry, no SA factory activation after Vamm initialization,
stopped cached-hook rejection and native runtime destruction without an SA-table
shutdown. This establishes a working source-only transaction path, not native
loader activation by itself. Native SDK-only installation is a separate default-off
bounded path and rejects the existing full-group experiment permit. No SDK/game identity is
newly authorized and every SDK/game patch remains in place. Real SDK caller,
bootstrap, ABI, compatibility and Apple-device evidence remain necessary.


## Fail-closed startup contract selection

`hle_memory_experiment.c` now selects an immutable compile-time descriptor by
exact request name. Each descriptor binds the complete ordered module names and
64-hex-character NSO identities to one ownership mode, a bounded reservation
ledger, and an explicit domain/placement construction contract. Validation
rejects malformed identities, duplicate names, unsupported modes/policies,
missing/extra/reordered/unloaded/deferred modules, and inconsistent short/full
identities before construction or native GetInfo queries. The descriptor is not
a profile override or a caller-issued authorization capability.

The only admitted requests remain `za-base-v1` and `za-base-sdk-v1`, with the
same exact Z-A main plus SDK pair, 65,536-record bound, alias-minus-extra domain,
and modern placement. Full versus owner-only runtime construction now calls the
explicit-contract constructors; the existing independent permit and relocation
transaction gates remain. SDK15 and every other unreviewed whole-process closure
remain rejected. No SDK/game patch is removed and no new gameplay proof follows.

`make -C dm/host/tests memory-startup-contract` exercises synthetic raw-alias and
both legacy placement contracts through that same construction helper, including
mode separation, immutable selection, malformed descriptor rejection, and
constructor failures. The fixture compiles with native experimental activation
disabled and never issues a startup permit. The existing `memory-experiment` and
`sdk-memory-experiment` suites now assert the exact explicit constructor contract
and continue covering startup failure cleanup, transaction rollback, original SA
preservation and mode-scoped authority. Add `-sanitize` to each target for the
ASan/UBSan variants. These tests execute no private SDK and admit no new title.
