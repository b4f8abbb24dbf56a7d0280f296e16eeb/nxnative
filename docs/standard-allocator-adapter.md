# StandardAllocator symbol boundary (experimental)

**Shipping activation is hard-disabled.** `dm_sa_adapter_install` rejects even
complete proof flags unless this translation unit is built with the synthetic
fixture-only `DM_SA_ADAPTER_TESTING` macro. There is no production installer or
new `HLE_FORCE` registration. No game profile, SDK rewrite or game patch changes.

## Implemented vertical slice

`hle_standard_allocator_adapter.[ch]` owns the observed 22-symbol method group:
six C1/C2 constructor aliases, both Initialize overloads, Finalize, both Allocate
overloads, Free, Reallocate, Shrink, GetSizeOf, both free-space queries, cache and
management cleanup, WalkAllocatedBlocks, Dump and Hash. The supplied SDK exports
no StandardAllocator destructor; an extra or missing destructor/method rejects
this census rather than claiming an unknown ABI is covered.

Synthetic activation requires every symbol, every binding bit, nonrecursive
original function addresses, an original Allocate(size) forwarding callback, a native managed-alignment policy callback,
and explicit ABI/caller/inline/current-owner/behavior proof bits. All validation
precedes publication. These bits are requirements for future integration, not a
substitute for evidence. A real loader transaction must finish before any guest
execution and must cover every relocation, dynamically loaded module and direct
or inline path; the current code does not install such a transaction.

A side table holds the managed instance and its real provider-backed core.
Initialization publishes a provisional initializing state, then invokes its
configuration factory and creates the core without holding the registry lock.
Other instances remain usable from the factory. Same-instance calls, constructors,
reinitialization and Finalize reject that in-flight state with BUSY rather than
reentering the SDK or deadlocking. Failed first initialization removes only its
provisional record; failed reinitialization keeps its finalized tombstone. Only
null-buffer initialization becomes managed. Provided-buffer and other SDK-owned
objects call their captured original methods unchanged. Once managed, failures
never fall back to the SDK's embedded CentralHeap. Finalize waits for in-flight
operations, releases core ownership and retains a finalized tombstone until an
explicit constructor/reinitialization, preventing stale operations from escaping
to the SDK. The guest object remains 56 bytes; no host metadata is stored in its
embedded heap. Initialized/cache bytes are set within that observed extent.

The existing `hle_os.c` Allocate(size) registration is still the sole forced
entry. Its managed-only bridge cannot turn an allocation failure into an SDK
allocation. Its explicit managed alignment input preserves the existing `alloc_big_align`
policy instead of bypassing it. For unmanaged objects, original forwarding stays
unchanged. Future adapter installation supplies `forward_allocate` plus the same
thread-safe, nonreentrant native `allocation_alignment` policy for managed
Allocate(size). That policy is
not used for the explicit-alignment overload or SDK-owned objects; malformed or
unrepresentable managed alignment fails without an SDK fallback. The portable
adapter does not duplicate game rules.

## ABI and behavior evidence

Static analysis of the supplied SDK20.5.8 established:

- `self` uses x0. Initialize's cache bool is the low bit of w3; **null buffer**, not
  that bool, chooses virtual backing. Initialize is not a recoverable bool API.
- Shrink's observed result is a usable allocation size in x0, not bool.
- Hash writes a 24-byte aggregate through x8: block count, summed usable bytes,
  and the first eight SHA-1 digest bytes. Its input is ascending pointer/usable
  size pairs, each encoded as a little-endian 64-bit word; not allocation data.
- The walk callback continues for **nonzero** and stops for **zero**. The SDK
  drops its heap mutex around callbacks. Its observed w0 status reaches the
  StandardAllocator boundary (0 on success/early stop, 2 if empty, 22 for a null
  callback); the public C++ return declaration was not independently available.
- StandardAllocator passes CentralHeap flags=0. That disables the optional
  malloc-zero sentinel. Both central and cached `Allocate(0)` return NULL;
  `GetSizeOf(NULL)` is 0 and `Free(NULL)` does nothing. `Reallocate(NULL,0)` is
  NULL. These create no allocation and do not change Hash/walk state. The
  adapter translates these rules without changing the generic core's distinct
  minimum-16 zero-allocation policy.
- Nonnull `Reallocate(p,0)` frees and returns NULL. High-bit sizes are rejected
  before rounding. Default alignment is 16; nonzero powers of two below 16 can
  be serviced by 16-aligned backing. Zero or non-power-of-two alignment returns
  NULL. Virtual capacities must be nonzero and page-multiple; the adapter also
  conservatively rejects high-bit capacities.
- Shrink(p,0) does **not** free. SDK small blocks retain their size class and
  large blocks retain at least one 4KiB page. The adapter currently shrinks to a
  real minimum-16 core block and returns its actual usable size. This is an
  intentional allocation-policy difference, **not exact SDK size-class parity**.

No SDK instructions or extracted binaries are included in source or fixtures.
The public method names and behavioral descriptions are interoperability facts.

## Remaining activation blockers

The 56-byte layout and all observed SDK-internal relocated calls are evidence
for one SDK, not for game mains, other SDKs, computed calls, inline getters,
embedded CentralHeap use, or destructor paths. Matching target main/SDK/subsdk
coverage and device validation are still required. A zeroed embedded heap and
unallocated SDK TLS slot must never be exposed to inline/direct SDK access.

The managed implementation uses no per-thread caches. It reports real provider
budget plus reusable real backing, not unreserved virtual capacity. SDK class
rounding, minimum arena overhead, finalization edge cases, exact diagnostics,
maintenance return declarations, cached behavior and allocation-failure behavior
are not all proven equivalent. Core realloc preserves original alignment while
observed SDK move paths may use 8-byte alignment.

Walk collects a stable internal snapshot and invokes guest callbacks with no
allocator/registry lock. Before each callback it skips known-freed blocks and
stops if the instance was finalized/reinitialized. Reentrant queries, freeing a
later block and finalization are covered by fixtures. Concurrent same-address
free/reallocation (ABA), newly added blocks and SDK iteration semantics under
mutation remain unproven; a snapshot does not pin guest pointer lifetimes.

A provider must lease real spans through the current SDK reservation owner or
use demonstrably disjoint backing, while synchronizing source-borrow checks.
The adapter does not authorize an independent alias-space owner. Existing Vamm
ownership fences and all arena/SDK workarounds remain in place.

## Reproducible synthetic checks

From `dm/host/tests`:

```
make standard-allocator-adapter standard-allocator-gate standard-allocator-abi
make standard-allocator-adapter-sanitize
```

The adapter fixture covers all-method forwarding and ownership, constructor
aliases, complete-group rejection, existing alignment-policy forwarding and managed alignment, callback-safe initialization, concurrent lifecycle rejection and factory-failure cleanup, guarded
failure/no SDK escape, initialization bounds, zero/resize/query/hash behavior,
walk stop/reentry/lifetime cases, and concurrent allocation/reallocation/free.
The gate fixture builds without the testing macro and verifies that production
remains inactive even with a complete advertised census and all proof flags.

The ABI fixture is original handwritten assembly and runs only on native
AArch64, checking x0/self plus x8/24-byte aggregate return. Other architectures
explicitly print `SKIP`; ordinary x86 aggregate tests are not an AArch64 result.
On the current x86_64 executor the adapter normal, ASan+UBSan and TSan checks
pass, but live AArch64/macOS/iOS execution is not available. LeakSanitizer is
blocked by this executor's ptrace runtime; `ASAN_OPTIONS=detect_leaks=0` permits
the address/undefined checks and is not a leak-clean claim.
