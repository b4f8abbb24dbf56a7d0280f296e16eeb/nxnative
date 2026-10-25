# Bounded native shared-memory experiment

**Default production behavior is unchanged and disabled.** This opt-in is an
experiment, not a general SDK compatibility claim or permission to remove game
or SDK patches. It joins the existing native runtime to the actual startup
loader transaction; it does not use fixture activation macros or `PROOF_ALL`.

This page describes the full 40-hook mode. The separate
[SDK-only mode](bounded-sdk-memory-experiment.md) retains original StandardAllocator
and does not solve its contiguous 12 GiB arena request. Its authority is separate.

## Deliberate opt-in

For a Mac development host:

```sh
make -C dm/host GAME=0100F43008C44000 EXPERIMENTAL_SHARED_MEMORY=1
DM_EXPERIMENTAL_SHARED_MEMORY=za-base-v1 dm/host/dmhost /path/to/rewritten-base main sdk
```

Do not run this command on unreviewed input. Rebuild with
`EXPERIMENTAL_SHARED_MEMORY=0` for production. The build stamp tracks this switch,
so switching back cannot silently reuse an experimental executable. A missing,
empty, or `0` runtime request leaves ownership with the SDK even in an
experimental binary. The separately reviewed `za-base-sdk-v1` value selects
SDK-only mode; any other requested value fails explicitly rather than
silently falling back. iOS packaging does not opt in automatically.

Partial-page reclamation is a separate, **default-off runtime flag**:

```sh
DM_EXPERIMENTAL_SHARED_MEMORY=za-base-v1 DM_MEMORY_RECLAIM=1 \
  dm/host/dmhost /path/to/rewritten-base main sdk
```

An absent `DM_MEMORY_RECLAIM` or exact `0` preserves whole-lease backing; exact
`1` selects the page-capable native factory only after the full-mode contract
passes. Any other value, including an empty string, is invalid. `1` with an
inactive experiment or SDK-only mode fails clearly rather than becoming a
silent no-op. The flag is read once at startup, not a live ownership switch.
It is reusable for future **separately reviewed** full-mode contracts, but it
does not admit another title, version, module set or SDK: only the Z-A base
identities below are currently eligible. All existing cuts and metadata bounds
remain unchanged. Both the experimental build and exact runtime request are
still required; a failed check stops before guest entry.

Only these **full 32-byte NSO build IDs** are eligible:

| Module | Reviewed identity |
| --- | --- |
| `main` | `7222e13ecf6adb3249b5dd5c57fa40b102245fce000000000000000000000000` |
| `sdk` | `df7a3960c74c3a4803f826c5871532523aa9cf2a000000000000000000000000` |

This is the reviewed Z-A base main and SDK 19.3.11, not the separate 2.0.2 main
or another title/SDK. The entire mapped module set must contain exactly these
two **in the reviewed order, `main` then `sdk`**: no reversal, extras, duplicate
names, deferred NROs, unloaded modules or prior loads. Global ELF symbol lookup
is order-sensitive, so a swapped pair is not the same reviewed configuration.
A missing `BUILD_ID_FULL` in old rewritten output fails closed; regenerate it
with the current rewriter. Existing short build IDs remain unchanged for legacy
profile rules. IDs in fixups are trusted build metadata, not authentication of
untrusted or relabelled binaries. No private code bytes or offsets are embedded.

## Startup and lifetime

`load_modules` maps/parses/relocates the complete set and preloads deferred NROs
before it opens the startup window. The experiment then verifies identities,
creates one native runtime (65,536 reservation records maximum), obtains its two
configs, and calls `loader_install_memory_hooks`. The transaction independently
checks the complete real symbol/relocation census and retains both tables. Only
while that call is in progress does an opaque native authorization allow adapter
preparation. No caller-supplied proof mask enables native operation.

The alignment-aware native lease implementation is a hard prerequisite. Its
capability marker certifies exact aligned placement/feasibility support, not that
an arbitrarily large alignment or allocation will fit. Reservations still obey
real domain gaps, guards, bounded metadata and physical-budget constraints.

A prepublication failure cancels preparations, destroys the unused runtime,
prints a diagnostic and stops the requested experiment before guest entry.
Publication occurs before `hlecount_seal`; counted relocation targets stay stable
under the sealed-wrapper fix. There is no late ownership migration or SDK
fallback after publication. Module loading/unloading and host startup/restart
are refused; launch a new process instead.

The runtime is retained for **process lifetime**. Existing host shutdown ends in
`_exit` and does not establish stopped/joined guest users; it must not destroy
this runtime. Both experimental adapter shutdowns therefore refuse, even when
individual managed allocators have finalized. This does not alter the synthetic
exclusive-lifecycle teardown tests or claim orderly guest teardown support.

## Managed behavior and diagnostics

The reviewed observed main/SDK call closure has no public StandardAllocator Walk
caller. Managed Hash uses the core directly. An unexpected managed
`WalkAllocatedBlocks` therefore aborts with a diagnostic **before any callback**;
it never calls the SDK or invokes the synthetic snapshot-walk policy. SDK-owned
non-null-buffer objects still follow their original methods. This restriction
is not a claim of concurrent Walk/free/finalize or pointer-ABA safety.

Successful managed null-buffer Initialize now emits its exact requested capacity
and a best-effort initial snapshot automatically; it does not depend on the game
calling Dump. Allocation/Reallocate failures emit the same ownership/budget
snapshot for failure counts 1, 2, 3, 4, 8, 16, ... per object generation, not every
retry. Zero-size allocation is not logged as a failure. Finalize reports either
success or retained ownership on failure. A successful Finalize never queries
the destroyed core. No initialization-success event is emitted on failure.

These optional native observers are installed only by the experiment; default
production has no observer and performs no extra diagnostic stats queries.
Snapshots are taken while initialization/finalization or an in-flight reference
protects the core; logging occurs after native locks/references are released.
Self is only an identity in the copied event, not a dereferenceable pointer.
Events may interleave across threads, so match object identity and generation.
Query failure prints `snapshot unavailable` and never changes the operation or
ownership. Budgets are instantaneous summaries, not a promise that a subsequent
high-alignment request fits.

Managed Dump still reports nominal capacity, actual leased backing, live
bytes/blocks, span count, real free/largest budgets, native metadata and closed
state. Existing Dump/event snapshots append `reserved` (the outstanding lease
extent, also reported as `backing`) and authoritative `committed` bytes. The
one-time activation policy line names the contract, full/SDK-only mode and
reclaim on/off. A successful flag request is not evidence that a game has
actually shrunk or trimmed any allocation; compare the commitment snapshots
with the exercised operations. Returning pages preserves the virtual reservation
and does not promise an immediate physical RSS drop. Fatal
contracts identify the operation/result and explicitly retain ownership. Ordinary
allocation exhaustion still returns null without SDK fallback.

### Reclaim evidence (only `DM_MEMORY_RECLAIM=1`)

Every managed Initialize emits a zero cumulative page line with
`route=unexercised`. This means enabled but not yet exercised, **not** a pass or a
failure. No allocations are forced for diagnostics. Shrink/Trim/reuse/growth
activity emits copied summaries automatically, even if the game never calls
Dump: the first four changed observations, powers of two thereafter, and the
first successful decommit and recommit route. Dump and Finalize also report
cumulative counters. Reclaim off adds no page sampler or page log lines.

`reclaim_calls`/`recommit_calls` count actual optional provider calls; `*_ok`
counts returning success. `*_requested_range_bytes` and
`*_successful_range_bytes` count **callback interval coverage**, not newly freed
or newly committed physical bytes. A rounded recommit can include an already
committed live boundary; a concurrent raw SVC can make a call idempotent. The
line explicitly says `ranges_not_physical_deltas`. Use authoritative `reserved`
and `committed` snapshots for current state; no immediate RSS reduction or
successful gameplay is implied by either counters or activation.

`borrowed_reclaim_runs` counts observed borrowed runs skipped during cleanup;
`borrowed_recommit` counts a borrowed preparation interval. `budget_denials`
counts definite insufficient-headroom recommit prechecks, **not** every failed
allocation or the deliberate backed-first search. `change_no_memory` and
`change_busy` separately record provider callback results without guessing their
cause. `page_failures` records operational page query/contract/other callback
failures; diagnostic stats queries do not increment it. `publication_busy`
records unavailable payload/source revalidation, which retains new owned pages
rather than rolling them back unsafely. Best-effort cleanup failure still cannot
turn a successful logical shrink into failure. `route=attempted` has calls but
no successful callback; `skipped` has observations but no callback; `observed`
means at least one callback succeeded, with the two directions shown separately.

Counters and sequence saturate at `UINT64_MAX`. They are updated under the core
mutex, kept only in a fixed-size optional host-metadata tail, and copied without
provider callbacks for sampling. There is no per-operation allocation or IO in
the core. Extra stats scans happen only for emitted samples. An in-flight or
exclusive lifecycle reference protects every copy; notification contains no
live-core pointer and runs after all locks/references are released. Finalize
captures its final counters before destroying the core. Sequence orders copied
state, **not console delivery** across threads; match self/generation/sequence.
At sequence saturation periodic sampling stops, but the first successful route
still forces a sample; Dump/Finalize continue to copy the saturated totals. The observer must not reenter the adapter or call guest code.

## Source-only verification

```sh
make -C dm/host/tests memory-experiment standard-allocator-events
ASAN_OPTIONS=detect_leaks=0 make -C dm/host/tests memory-experiment-sanitize standard-allocator-events-sanitize
make -C dm/host/tests standard-allocator-events-tsan memory-runtime-reclaim-tsan
make -C dm/host/tests memory-runtime-reclaim standard-allocator-provider
ASAN_OPTIONS=detect_leaks=0 make -C dm/host/tests memory-runtime-reclaim-sanitize standard-allocator-provider-sanitize
python3 -m unittest discover -s dm/tools/tests -p 'test_full_build_identity.py'
python3 -m unittest discover -s dm/tools/tests -p 'test_memory_experiment_startup_order.py'
```

The portable matrix uses synthetic ELF originals/resolution with the real
experimental authorization, transaction, adapters, runtime, native provider and
SVC backend. It covers opt-in off/on, build-disabled and alignment-prerequisite
failure, exact/mismatched/missing/extra/duplicate/deferred/swapped-order modules, canceled
runtime destruction, unchanged slots on rejection, 12 GiB lazy initialization,
a cold 4 MiB-aligned allocation, managed Hash/Dump and Walk rejection before
callback. Both host-page settings include sanitizer runs. Separate source tests
compile the actual `load_modules` body against recording native stubs to check
ordering, failure before sealing, and restart rejection before mapping. They do
not execute a real game or validate the Apple loader's platform dependencies.

Apple/AArch64 ABI and actual loader/counting-stub tests, unpatched guest startup
and its allocation probe, guest callback/thread-local paths, real 16 KiB VM/GPU
interactions and gameplay remain required before production enablement or any
patch removal. Static observed closure is deliberately not universal proof of
all computed or inline call paths. Private input is never a CI fixture.

## Real-game handoff signals

Keep all existing game/SDK/UI patches and use the user's reviewed base inputs
in the ordered pair `main sdk` shown above;
do not substitute the update main or edit metadata to bypass an eligibility
failure. Capture the complete host log from launch (Mac stderr, or the iOS
app's `Documents/log.txt`) so startup rejection is not mistaken for a later
allocation failure. Successful publication prints:

```text
experimental shared memory za-base-v1 active: exact main+sdk, process lifetime, managed Walk rejected
```

That activation line confirms publication only, not guest initialization,
rendering or gameplay. The expected evidence of the original 12 GiB request is
an automatic pair of lines (addresses, generation, budgets and metadata vary):

```text
experimental shared memory managed Initialize success self=... generation=... requested=12884901888 alignment=0 result=0 failure_count=0
experimental shared memory snapshot self=... generation=... capacity=12884901888 backing=0 live=0 blocks=0 spans=0 free=... largest=... metadata=... closed=0
```

This proves successful interception of that capacity with initially lazy
backing; it does not prove the startup probe, later allocations or gameplay.
`free`/`largest` must be real budget values, not the nominal capacity fabricated
as physically available. If the snapshot is unavailable, retain that line and
do not infer a budget from activation alone. Later diagnostic lines are:

```text
experimental shared memory managed Allocate failure self=... generation=... requested=... alignment=... result=... failure_count=...
experimental shared memory managed Reallocate failure self=... generation=... requested=... alignment=0 result=... failure_count=...
experimental shared memory managed Finalize success self=... generation=... requested=... alignment=0 result=0 failure_count=0
experimental shared memory managed Finalize retained self=... generation=... requested=... alignment=0 result=... failure_count=0
```

Failure snapshots describe retained ownership before the fatal policy, if any.
Keep the first fatal/rejection, automatic Initialize and failure snapshots, and
surrounding startup log. Test a new process each time; an in-process stop/restart
is intentionally unsupported. No binary/disassembly upload is required.

### Verified Mac memory sampling flags

`dm/host/memstat.m` reads `DM_MEMSTAT=<TSV path>` at process construction and
samples approximately once per second. It records `phys_footprint`,
`metal_alloc`, internal/compressed/graphics/headroom and per-class
virtual/resident/charged MiB. The `available` column is iOS-only (zero on Mac).
`DM_MEMSTAT_DUMP=<integer seconds>` additionally writes `<TSV path>.regions` at
that sample. These are read-only diagnostics, not allocator budget controls.
Use writable absolute paths and unique output names per run, for example:

```sh
DM_EXPERIMENTAL_SHARED_MEMORY=za-base-v1 \
DM_MEMSTAT=/tmp/za-native-on.tsv DM_MEMSTAT_DUMP=30 \
dm/host/dmhost /path/to/rewritten-base main sdk 2>/tmp/za-native-on.log

DM_EXPERIMENTAL_SHARED_MEMORY=0 \
DM_MEMSTAT=/tmp/za-native-off.tsv DM_MEMSTAT_DUMP=30 \
dm/host/dmhost /path/to/rewritten-base main sdk 2>/tmp/za-native-off.log
```

The reviewed base main requests 12 GiB and has no matching arena-size cut.
**Opt-in off may fail during SDK initialization in the smaller alias region.**
Record that baseline failure versus experimental startup; compare identical
menu/gameplay/transition routes only if both paths reach gameplay. Do not add a
temporary game patch or edit an identity to force an A/B result. Launching alone
is not the completion criterion: review the initialization/budget evidence,
then the user's real gameplay and transition results.

On iOS, automatic experimental lifecycle logs are in `Documents/log.txt` after
`boot_env` loads env.txt. However, the current memory sampler starts in a C
constructor **before** `main.m` reads env.txt. Therefore adding `DM_MEMSTAT` only
to env.txt does not currently start that sampler; no iOS TSV is promised by
these instructions. A launch-time environment supplied before constructors is
a different path and remains unverified here.
