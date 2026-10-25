# Experimental Vamm ownership boundary

See [SDK compatibility status](sdk-memory-compatibility.md) for the 19-build inventory,
placement mechanisms and the separately gated Vamm-only transaction prototype.

**Experimental integration; default production installation is disabled.**
The separate [bounded native experiment](bounded-shared-memory-experiment.md)
requires build/runtime opt-in and exact reviewed full identities. No game or SDK
patch is removed; synthetic tests do not claim real-game handoff validation.
This is a complementary boundary to the [StandardAllocator adapter](standard-allocator-adapter.md),
not a replacement for the evidence required to activate either one.

## Why allocation hooks alone are unsafe

Static SDK inspection identified eight public `nn::os` operations:
`InitializeVirtualAddressMemory`, `AllocateAddressRegion`, `AllocateMemory`,
`AllocateMemoryPages`, `FreeAddressRegion`, `FreeMemoryPages`,
`GetVirtualAddressMemoryResourceUsage`, and `IsVirtualAddressMemoryEnabled`.
There are corresponding eight `detail::VammManager` methods and two constructor
aliases: 18 exported names in the inspected surface.

The constructor bootstraps the reader/writer lock and queries alias base/size;
it does not create the reservation tree. `InitializeIfEnabled` separately
constructs that tree through a **direct unexported call**, commits initial node
storage and reserves a large guest node area. `AllocateMemory` also has its own
reservation path; it does not call exported `AllocateAddressRegion`. Page and
free methods consult the same tree. Consequently replacing only allocation or
only public methods would leave competing reservation owners.

Observed SDK callers enter these exported methods through PLTs. That is useful
but does not prove absent game/subsdk inlining, computed calls, object-field
access or a different SDK implementation. No Vamm destructor/finalizer export
was observed; absence of an export is not proof of complete process teardown.
Private inspection evidence remains outside the repository. No offsets or guest
machine code are included in this implementation or its synthetic tests.

## Separate reservation and physical ownership

`hle_vamm.[ch]` is a single **reservation-only** owner over an already real,
fixed native alias domain. It allocates bounded host metadata, not a guest node
area. It must never run beside the original SDK reservation owner.

Upstream's `hle_svc.c` bitmap remains the sole physical-page authority. Raw
`MapPhysicalMemory` callers are not forced to hold a Vamm reservation. An
uncommitted physical interval is not proof that it is unreserved; only the
current reservation owner's ledger supplies reservation facts. The superseded
experimental address-space manager has been removed, not layered underneath
this owner.

Core contracts:

- Exact payload sizes and real pointers; no fictitious giant arena, rounding to
  a smaller success, fragmented contiguous allocation, or mmap outside the
  registered domain.
- Size-dependent absolute alignment (64 KiB below 2 MiB, 2 MiB otherwise), a
  4 KiB guard on each payload side, overflow-safe bounds and descending free-gap
  selection. Adjacent payloads may share the separating guard page. Guards
  are logical reservation separation, **not** independent
  host-page protection promises on 16 KiB hosts.
- Combined reserve/map publishes no output or ledger entry if mapping fails;
  successful mapping has no later fallible metadata step.
- Free decommits the entire recorded payload before removing its record. A
  borrowed MapMemory source causes a returning rejection that retains ownership
  and contents. Host protection failures abort before ledger/accounting
  publication, as upstream requires. Foreign/interior/cross-reservation ranges
  are rejected.
- Native page callbacks hold the Vamm lock, then acquire upstream's single
  `g_map_lock` for source ownership, physical backing and accounting. They must
  not run guest callbacks or reenter Vamm. Production
  result translation belongs at that verified callback boundary; synthetic
  tests do not claim to establish every SDK's invalid-input/abort policy.
- `sys_resource=0` keeps initialize disabled with no page changes. Resource
  usage returns a 16-byte aggregate (AArch64 x0/x1), using kernel system-resource
  values and bounded 512 KiB headroom; these values are not alias reservations.
- Empty exclusive host teardown frees metadata. Live reservations prevent
  destruction; a failed free must be retried after its actual blocker clears.

`dm_vamm_get_stats` supplies exact largest legal payload per free interval at
an alignment, plus a conservative sum of these maximal payloads. If remaining
metadata slots cannot represent every hole, the sum conservatively falls back
to the largest hole. This is not a claim of optimal multi-allocation packing.
It is also not physical capacity: the existing span provider clamps it against
real heap/image/alias usage under the SVC source guard. StandardAllocator can
consume this owner through the same trusted current-owner callback interface;
its 62 GiB logical capacity does not reserve 62 GiB.

## Native SVC bridge

`hle_vamm_native.[ch]` now supplies real callbacks instead of relying on the
synthetic fixture to drive the SVC dispatcher. `dm_vamm_native_configure` obtains
its base from GetInfo type 2 and its **exact** size from type 3 minus type 28.
It rejects zero, misaligned, underflowed or wrapping raw domains without
publishing configuration. Type 28 is currently zero in this host, but the
owner does not hard-code that assumption; nonzero deductions are tested.
Preparing configuration does not initialize an owner or transfer reservations.

The page callback and the public MapPhysicalMemory/UnmapPhysicalMemory SVCs
share the upstream bitmap transaction under one `g_map_lock`. Decommit rejects
borrowed-source overlap before changing backing. Recommit may overlap borrowed
sources and extend their committed windows while preserving existing bytes,
retaining upstream's Raging Loop fix. Host protection failures abort before
publishing new accounting, rather than returning a recoverable result.
No guest callbacks or dispatcher reentry occur. Limited profiles
still enforce the application pool; ordinary legacy-unlimited callers remain
unlimited. Only the existing provider's thread-local allocation scope adds the
strict current heap/image/alias budget, atomically at commit.

The callback explicitly implements the independently checked operation-specific
result policy of the inspected SDK, not the old fixture's generic guess:

| SVC result | Map/combined allocation | Unmap/free |
| --- | --- | --- |
| success | success | success |
| `0xce01` (OutOfResource) | `0x1203` | `0x1203` |
| `0xd001` (OutOfMemory) | `0x1003` | `0x1003` |
| `0x10801` (LimitReached) | `0x1003` | `0x1003` |
| `0xd401` (InvalidCurrentMemory) | fatal | `0x803` |
| other nonzero result | fatal | fatal |

Upper result-context bits are masked for error comparisons, not for detecting
success. In particular, `0xd001` and `0xd401` are different results. A returning
invalid-current-memory unmap, such as decommit of a borrowed source, translates
to `0x803` and keeps the Vamm reservation live. Backend mprotect failure is
separate: upstream aborts inside the SVC transaction before result translation
for both map and unmap. The table is an observed contract for the inspected
SDK, not activation proof for arbitrary versions. There is no guest abort
callback under native locks.

The resource callback returns GetInfo 16/17 **unmodified**. The Vamm owner alone
adds and bounds the 512 KiB headroom; no alias reservations or committed bytes
are substituted for system resources. Tests include zero and tiny totals.

## All-or-none dispatch, with no shipping activation

`hle_vamm_adapter.[ch]` includes all 18 signatures, including constructor aliases,
public/detail initialization, the independent combined allocation path and the
16-byte resource return. Preparation validates and copies the complete originals
and preallocates the reservation owner, returning one exclusive cancelable lease.
Lookup remains inactive until publication. Calling a prepared constructor or
initializer aborts before invoking any original or resource callback. Cancellation
destroys only never-published, empty host metadata. Alternatively an explicit
`shared_owner` and native retain/release pair pin the runtime-owned Vamm through
preparation/publication; cancellation or drained shutdown releases that handle
without destroying the runtime owner. Reentrant lifecycle calls from retain or
release callbacks reject rather than deadlock. Publication is infallible for
the outstanding lease and performs no allocation or guest callback.

The only original operation invoked after a synthetic handoff is the verified
non-owning bootstrap constructor. The SDK tree initializer is never invoked.
A mismatched detail object is fatal, never a fallback to another owner.

Default production preparation rejects activation even if all proof flags
are supplied. `DM_VAMM_ADAPTER_TESTING` enables synthetic preparation; the bounded
native experiment requires a separate opaque startup authorization. There
are no new `HLE`/`HLE_FORCE` registrations. The old asserted binding mask is removed:
the [bounded loader transaction](memory-hook-transaction.md) derives its actual
census and journal from the mapped modules, replaces both complete groups before
guest execution, and retains original slot values on failure. It rejects NROs,
late installation, duplicate owners and unsupported relocation shapes. It does
not prove the absence of private/direct/inline accesses or a prior owner, and no
default production startup path supplies an installation configuration. Shutdown keeps
the group intercepted: it cannot restore an SDK owner into previously host-owned
memory.

### Remaining activation blockers

1. Matching complete main/sdk/subsdk evidence for each target, including all
   initialization, direct/indirect calls, inlines, object access and unload paths.
   The current dispatch assumes one process-wide Vamm identity; multiple SDK
   copies or independently constructed managers need explicit owner analysis.
2. Proven original constructor-only bootstrap and a handoff before **any** SDK
   node owner or conflicting reservation is created. Existing live reservations
   are not imported or migrated by this prototype.
3. Verify native callback/result and exact-domain contracts against each supported
   SDK, plus the bounded loader transaction and all target-specific call paths.
   The native bridge and startup transaction do not alone establish complete
   owner-handoff compatibility.
4. Real target-device, unpatched initialization and exhaustion/rollback tests,
   concurrency/teardown, GPU pointer registration, rendering and gameplay.

Only after those proofs and validation may the node-area/alignment cuts be
considered for removal. Large-arena per-game cuts additionally require the
complete StandardAllocator replacement: Vamm cannot truthfully satisfy a huge
single contiguous region inside a smaller native alias domain.

## Reproducible synthetic checks

```sh
make -C dm/host/tests vamm vamm-gate vamm-abi
make -C dm/host/tests vamm-sanitize
make -C dm/host/tests vamm-tsan
make -C dm/host/tests vamm-native vamm-native-sanitize vamm-native-tsan
```

The normal, ASan/UBSan and TSan targets each execute 12 combinations: 4/16 KiB
host pages, limited/unlimited physical profiles, constructor aliases C1/C2 and
disabled initialization. Tests compile the actual upstream SVC backend, reservation owner,
complete dispatcher, existing span provider and StandardAllocator core together.
They verify guards/alignment, fragmentation statistics, rollback, borrowed-source
retention, raw SVC behavior, no node-area commit, metadata exhaustion,
multithreaded allocation/free, complete dispatch and blocked shipping install.

On x86 the AArch64 fixture explicitly skips rather than claiming native ABI
validation. When ptrace prevents LeakSanitizer, `ASAN_OPTIONS=detect_leaks=0`
permits ASan/UBSan checks without claiming a leak-check pass. Results must name
the tested rebased commit. Expected fatal-policy injections run in child
processes; a process abort is not treated as a recoverable backend result.

The native bridge targets additionally run six configurations: 4/16 KiB host
pages with limited/unlimited maps, and disabled/tiny system-resource totals.
They use production native callbacks with the actual upstream SVC backend and provider,
checking exact-domain validation, operation-specific result/abort behavior,
borrowed sources, strict nested budgets/image floor, exhaustion, rollback,
raw SVC compatibility, resource totals and concurrent allocation/free. Expected
fatal-policy cases run in child processes and must terminate with SIGABRT;
these deliberate aborts do not claim target-device SDK execution. The old
synthetic Vamm fixture now shares this checked result translator too.
