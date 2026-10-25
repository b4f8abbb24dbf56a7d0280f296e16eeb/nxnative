# Experimental Vamm ownership boundary

**Private prototype; production installation is hard-disabled.** No game or SDK
patch is removed, no guest binary is executed, and no live handoff is claimed.
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

The existing `g_alias_space` remains in `DM_AS_EXTERNAL_RESERVATIONS` mode and
continues to own kernel-style physical-page accounting. This is intentionally
not switched to host-reservation mode: raw `MapPhysicalMemory` callers must not
be forced to hold an SDK reservation. An uncommitted physical interval is not
proof that it is unreserved. Only the Vamm ledger supplies reservation facts.
The two managers have different roles, not competing reservation free lists.

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
  borrowed MapMemory source or backend failure retains both ownership and
  physical contents. Foreign/interior/cross-reservation ranges are rejected.
- Native page callbacks hold the Vamm lock, then acquire source guard, physical
  accounting and page-manager locks in that order. They must be atomic on
  failure, must not run guest callbacks, and must not reenter Vamm. Production
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
share one native transaction. It acquires source ownership, physical accounting
and address-manager locks in that order, rejects borrowed source overlap before
calling the backend, and retains the existing atomic page/accounting failure
contract. No guest callbacks or dispatcher reentry occur. Limited profiles
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
success. In particular, `0xd001` and `0xd401` are different results. A native
map backend failure currently returns `0xd401`: its SVC transaction rolls back,
but the nn::os boundary then aborts rather than reporting a recoverable failure.
Unmap with that same result returns `0x803` and keeps the Vamm reservation live.
This is an observed contract for the inspected SDK, not activation proof for
arbitrary SDK versions. There is no guest abort callback under native locks.

The resource callback returns GetInfo 16/17 **unmodified**. The Vamm owner alone
adds and bounds the 512 KiB headroom; no alias reservations or committed bytes
are substituted for system resources. Tests include zero and tiny totals.

## All-or-none dispatch, with no shipping activation

`hle_vamm_adapter.[ch]` includes all 18 signatures, including constructor aliases,
public/detail initialization, the independent combined allocation path and the
16-byte resource return. It validates a complete symbol census, originals,
proof requirements and complete binding mask before publishing a table. Failure
leaves lookup unavailable so a loader can keep the entire SDK group original.
The only original operation invoked after a synthetic handoff is the verified
non-owning bootstrap constructor. The SDK tree initializer is never invoked.
A mismatched detail object is fatal, never a fallback to another owner.

The production install function unconditionally rejects activation even if all
proof flags are supplied. Only `DM_VAMM_ADAPTER_TESTING` enables synthetic
installation. There are no `HLE`/`HLE_FORCE` registrations, loader edits or SVC
ownership changes. The binding mask is an integration assertion tested for
completeness, **not evidence that a live loader has installed those hooks**.
A future loader still needs a startup transaction that installs the complete
group before guest execution and retains original bindings on preflight failure.
Shutdown keeps the group intercepted: it cannot silently restore an SDK owner
into memory the host previously owned.

### Remaining activation blockers

1. Matching complete main/sdk/subsdk evidence for each target, including all
   initialization, direct/indirect calls, inlines, object access and unload paths.
   The current dispatch assumes one process-wide Vamm identity; multiple SDK
   copies or independently constructed managers need explicit owner analysis.
2. Proven original constructor-only bootstrap and a handoff before **any** SDK
   node owner or conflicting reservation is created. Existing live reservations
   are not imported or migrated by this prototype.
3. Verify the native callback/result and exact-domain contracts against every
   supported SDK, then implement the loader's atomic group installation
   transaction and native AArch64 signature validation. The native bridge above
   does not establish a safe owner handoff.
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
disabled initialization. Tests compile the actual SVC manager, reservation owner,
complete dispatcher, existing span provider and StandardAllocator core together.
They verify guards/alignment, fragmentation statistics, rollback, borrowed-source
retention, raw SVC behavior, no node-area commit, metadata exhaustion,
multithreaded allocation/free, complete dispatch and blocked shipping install.

On the current x86 host the AArch64 fixture explicitly skips rather than claiming
native ABI validation. ASan/UBSan passed with `ASAN_OPTIONS=detect_leaks=0` because
LeakSanitizer cannot run under the harness's ptrace; no leak-check pass is claimed.
The expected failure-injection tests log SVC backend errors deliberately.

The native bridge targets additionally run six configurations: 4/16 KiB host
pages with limited/unlimited maps, and disabled/tiny system-resource totals.
They use production native callbacks with the actual SVC manager and provider,
checking exact-domain validation, operation-specific result/abort behavior,
borrowed sources, strict nested budgets/image floor, exhaustion, rollback,
raw SVC compatibility, resource totals and concurrent allocation/free. Expected
fatal-policy cases run in child processes and must terminate with SIGABRT;
these deliberate aborts do not claim target-device SDK execution. The old
synthetic Vamm fixture now shares this checked result translator too.
