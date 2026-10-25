# Existing-owner StandardAllocator span provider

`hle_standard_allocator_provider.c` is a testable owner-integration slice, **not a default-enabled replacement**. The default StandardAllocator adapter gate remains closed; the [bounded experiment](bounded-shared-memory-experiment.md) is a separate explicit startup opt-in. All existing SDK/game cuts and reservation-ownership safeguards remain intact.

## Ownership and callback contract

The provider obtains real mapped spans through injected current-owner allocate/free operations. An SDK-owned configuration uses captured original `AllocateMemory` / `FreeAddressRegion`; the gated [native runtime](shared-memory-runtime.md) supplies callbacks to its one retained Vamm owner instead. The provider never infers unreserved holes from the physical bitmap. Its host-only ledger records exact base, size, generation token, and acquiring/active/releasing state. The real SDK must preserve a reservation and mapped bytes on any returning free failure. Static analysis of the privately supplied SDK established this for its inspected free path; this does not certify other SDKs.

A provider context may be shared by independent cores and must outlive every core, including one retaining leases after failed finalization. Destruction requires caller quiescence and returns `BUSY` while any lease record remains. The adapter does not silently take ownership of this context.

SDK owner callbacks require a verified existing guest-thread/TLS context. The native runtime instead checks readiness of its retained native owner and invokes no SDK code. Each configuration must supply the appropriate context predicate; callbacks must not re-enter the invoking core or acquire locks in reverse order. The provider does not initialize guest TLS, create guest threads, invoke uploaded code, or resolve module symbols itself. Success violating the exact mapped-span/domain/alignment/disjointness contract is fatal rather than exposing unsafe memory or losing an owner's reservation.

## Lock ordering and races

- Core metadata remains serialized by its per-instance mutex.
- Source reuse, free, resize, and copying use the real SVC `g_map_lock` scope. Logical committed pages are checked against upstream's authoritative bitmap under that same lock; host readability is never ownership evidence.
- The provider drops `g_map_lock` around **every owner callback**, including reservation queries, then reacquires it before returning. No SVC lock spans those callbacks. This permits the original owner to hold its Vamm lock and call SVCs that acquire `g_map_lock` themselves.
- The core revalidates new leases before publication and the old realloc source after any query/acquire guard suspension. A new borrow yields `BUSY` with unchanged output and retains any exact lease that cannot yet be returned.
- Release may race a new source borrow. The real SVC rejects the decommit; original-owner failure and the provider ledger both preserve that lease for retry.
- An original free can complete before its callback reacquires the source scope. The owner may legitimately reissue that address to another core during this gap. The retiring record is unavailable to its old core and is removed only after successful callback return; a distinct generation token identifies the new lease.

Retaining the per-instance mutex depends on the non-reentry/reverse-order callback contract. The synthetic tests satisfy it. Certification of the complete original callback closure and all external callers remains an activation prerequisite, not an assumption derived from passing synthetic tests.

## Real budget, not requested arena

Reservation queries must come from the **current reservation owner**. QueryMemory's uncommitted ranges are insufficient: an uncommitted address can still be reserved. The callback's snapshot is clamped against current real heap, image floor, and committed alias bytes. The allocator's requested 12–62 GiB capacity is never reported as available physical backing.

A thread-local, nesting-safe scope surrounds only the provider's owner allocation transaction (native or original SDK). SVC MapPhysicalMemory atomically applies the conservative application-pool limit under its existing `g_map_lock` in this scope, including owner metadata expansion. A competing commit after the query therefore cannot make this provider overdraw the pool. Ordinary legacy unlimited-profile SVC calls keep their prior policy.

Queries remain snapshots rather than a promise that the SDK reservation topology will stay unchanged. Failed payload acquisition transfers no core lease or output, but original SDK node-metadata growth may legitimately persist. That memory remains SDK-owned and physically accounted in subsequent queries.

Requests above the callback's guaranteed absolute alignment fail before calling the owner. This slice does not hide padded physical mappings, claim unsupported alignment, or return fragmented memory as one contiguous span.

## Optional page capability: provider layer only

An explicitly supplied `change_pages` owner callback exposes an optional
`dm_sa_provider.pages` capability. It inspects runs from the authoritative SVC
commit bitmap and source-borrow registry, reads physical headroom independently
of reservation gaps, and changes aligned pages inside an exact existing lease.
There is no second bitmap, reservation owner, or hidden malloc-page operation.
The capability's granule is its **logical commitment/decommit unit**: COMMITTED
state must be uniform within each granule, while BORROWED boundaries may be
finer and query prefixes may be clipped. It is not merely a minimum callback
alignment or the host protection-page size. The native capability remains
4 KiB on both 4 KiB and 16 KiB hosts. Finer existing commitment states are not
supported by an advertised coarser capability.
The production runtime does **not** supply this callback. Shrink and activation
gates remain unchanged. Sparse commitment statistics, whole-lease draining and
recommit-on-reuse/growth are implemented as staged core layers below. No core
operation initiates decommit of free runs.

Queries run without dropping the source guard and never read payload bytes.
Page changes mark the lease in-flight, drop the guard before the owner callback,
and restore it afterward. Recommit applies the existing strict physical-budget
scope even under legacy-unlimited profiles. A returning failure preserves page
state and accounting; host-protection failures retain the backend's fatal policy.
Decommit rejects borrowed bytes, while recommit preserves previously committed
bytes. Successful recommit can already be borrowed when the guard is restored:
a future core caller must revalidate before exposing or copying any allocation.

The caller must prove that decommitted pages contain no live allocations; the
provider owns lease/budget/borrow validation, not block lifetime metadata. Direct
tests therefore operate on leases not published to a core. Full release of a
page-capable lease checks ownership and source borrows independently of backing,
so holes cannot strand its reservation. Failed release retains its exact token
and sparse commitment for retry. Providers without the callback remain entirely
whole-lease-backed, including malloc fixtures and existing original-SDK routes.

Synthetic provider tests cover page-run clipping and bitmap-word boundaries,
generation/range validation, preserved live neighbors on simulated 16 KiB hosts,
idempotent accounting, failure retention, in-flight serialization, borrow races,
zero-reservation-gap recommit, stale-budget rejection and release through holes.
The native Vamm fixture also exercises the capability through `dm_vamm_pages`
and the real SVC backend without wiring it into production. Core reclamation,
runtime wiring and transactional Shrink policy still
require a separate implementation/review. This establishes no SDK policy parity,
new admission, gameplay result or permission to remove an existing cut.

### Sparse accounting and whole-lease draining

`backing_bytes` remains the exact outstanding lease extent and capacity charge;
`committed_bytes` counts logical commitment from authoritative page runs, including
borrowed backing. A sparse lease therefore retains its full reservation charge
without falsely reporting every reserved page as committed. Closed allocators
retain both figures until failed whole-lease releases succeed on retry.

Free-space statistics exclude borrowed intervals. Backed free bytes consume no
new physical headroom; holes and additional leases share the same headroom once.
With backed free bytes `C`, free holes `D`, additional lease extent `V` and physical
headroom `P`, total free is `C + min(P, D + V)`. Largest free uses a two-cursor
scan of authoritative runs to bound a contiguous candidate by its missing-page
cost; borrowed runs are barriers. No persistent residency ledger is introduced.
Query failures leave the entire caller output unchanged.

Page-capable free-space statistics describe budgeted recommit potential, not a
promise against a competing physical commit after a snapshot. Production
callbacks stay absent. Tests create holes only through exact captured leases
and only in known wholly free granules; no automatic Shrink/Trim reclamation is
enabled.

Sparse release and finalize preflight validate lease ownership and borrowed
ranges independently of commitment. Missing pages are not a reason for `BUSY`.
Finalize still checks every lease before closing or releasing any one of them;
a later release failure retains the exact sparse lease for retry. Providers
without page capabilities retain the original control flow, with committed
bytes equal to their fully backed lease extent.

### Recommit before allocation publication

Page-capable allocation first searches already-committed, unborrowed intervals,
including later aligned portions of mixed free blocks. A second pass searches
contiguous intervals whose missing-page cost fits current physical headroom;
an unaffordable leading hole does not hide a later feasible candidate. The
bounded run cursor retains no persistent bitmap. Alignment opportunities are
checked at missing-granule boundaries, with cached left-run boundaries rather
than repeated whole-bitmap scans.

All split metadata is staged before recommit. The core charges only missing
logical pages in the page-rounded candidate, not existing committed boundary
pages. The provider still enforces the actual budget atomically in its owner
transaction. Failure before commitment leaves allocation metadata, caller
output and page state unchanged. Successful recommit preserves existing live
boundary bytes and initializes only holes.

The source guard can be suspended during that transaction. Before publishing a
new block or enlarging an in-place realloc, the core rechecks candidate backing
and borrowed state; growth additionally rechecks the old live prefix. A moving
realloc retains its existing old-source revalidation before copying. If a borrow
wins after recommit, `BUSY` leaves the old allocation and caller output intact;
newly committed pages remain owned free backing, reflected in `committed_bytes`.
There is no unsafe compensating decommit of a rounded range that may contain
pre-existing live bytes. Whole-lease release/finalize can later drain it safely.

Synthetic cases cover both split-metadata failures before the page callback,
owner rejection, exact missing-only budgets across live page boundaries,
zero-budget selection of a later backed run, protected-old-prefix growth races,
postcommit target borrows and retained-page retry. Providers without page
capabilities retain their original allocation/growth path. Runtime opt-in,
automatic reclamation policy and gameplay validation remain separate work.
A synthetic coarser-capability fixture also rejects unsupported finer holes in
stats, allocation and in-place growth without changing pages, outputs or the
old allocation. A separate valid case keeps commitment uniform while borrowing
a finer boundary neighbor; rounded recommit preserves that borrow and its bytes.

## Verification and remaining boundary

`make -C dm/host/tests standard-allocator-provider` runs the actual production bitmap-backed SVC dispatcher, provider, and allocator core with a synthetic owner. Variants cover 4 KiB and simulated 16 KiB host pages, plus limited and legacy-unlimited profiles. Sanitizer and thread-sanitizer targets use the same matrix. The fixture contains no proprietary code or binary input.

Coverage includes callback-to-SVC lock reacquisition, real query/accounting, logical acquire and release failure retention, guest-context refusal, source-borrow races during acquire/release/query, retained lease retry, owner metadata growth on payload failure, a competing commit after a stale budget snapshot, unchanged ordinary unlimited-profile policy, and multiple cores sharing the provider.

The original-SDK-owner route still needs a verified reservation snapshot implementation, original-function binding and guest-context wiring. The gated native runtime supplies an explicit alternative owner, but production activation still needs complete ABI, module, inline-access, and behavior coverage; stronger-alignment behavior if required; and device/gameplay validation. Other ownership domains and lifetime assumptions must also be certified before claiming equivalence. Host mprotect failures retain upstream's fail-fast policy rather than becoming retryable provider errors. Passing this fixture does not authorize removing any cut or installing partial hooks.
