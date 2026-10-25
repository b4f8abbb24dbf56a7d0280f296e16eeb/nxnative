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
The production runtime does **not** supply this callback. Core Shrink, Free,
Trim, reuse, allocation statistics and activation gates remain unchanged; the
core currently validates only the capability's complete shape.

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
recommit-on-reuse, accounting integration and transactional Shrink policy still
require a separate implementation/review. This establishes no SDK policy parity,
new admission, gameplay result or permission to remove an existing cut.

## Verification and remaining boundary

`make -C dm/host/tests standard-allocator-provider` runs the actual production bitmap-backed SVC dispatcher, provider, and allocator core with a synthetic owner. Variants cover 4 KiB and simulated 16 KiB host pages, plus limited and legacy-unlimited profiles. Sanitizer and thread-sanitizer targets use the same matrix. The fixture contains no proprietary code or binary input.

Coverage includes callback-to-SVC lock reacquisition, real query/accounting, logical acquire and release failure retention, guest-context refusal, source-borrow races during acquire/release/query, retained lease retry, owner metadata growth on payload failure, a competing commit after a stale budget snapshot, unchanged ordinary unlimited-profile policy, and multiple cores sharing the provider.

The original-SDK-owner route still needs a verified reservation snapshot implementation, original-function binding and guest-context wiring. The gated native runtime supplies an explicit alternative owner, but production activation still needs complete ABI, module, inline-access, and behavior coverage; stronger-alignment behavior if required; and device/gameplay validation. Other ownership domains and lifetime assumptions must also be certified before claiming equivalence. Host mprotect failures retain upstream's fail-fast policy rather than becoming retryable provider errors. Passing this fixture does not authorize removing any cut or installing partial hooks.
