# Native shared-memory runtime wiring (gated)

`dm/host/hle_memory_runtime.[ch]` connects the native Vamm reservation owner,
the exact-lease StandardAllocator provider, both adapter tables, and the real
upstream bitmap-backed SVC transaction. It is **not enabled for guest execution**: production adapter
preparation still rejects every installation, including complete proof masks.
No SDK/game patch, profile, private structure offset or game binary is changed.

## One explicit owner, one lifetime

`dm_memory_runtime_create` prepares one Vamm over the exact native GetInfo
2/3/28 domain and one provider. Creation neither initializes Vamm nor reserves
or commits guest pages. `dm_memory_runtime_configs` returns fresh adapter
settings containing that same owner handle, native retention callbacks, and
the StandardAllocator configuration factory. It does not supply a symbol
census or fabricate caller/ABI/behavior proofs.

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
The caller drains cores, shuts down both adapters, then destroys the runtime.
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
to it. The provider currently guarantees 64 KiB lease alignment and rejects
larger requested lease alignments rather than claiming unavailable backing;
full guest alignment/behavior compatibility remains an activation prerequisite.

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
Finalize/retry, concurrent cores, and table/config lifetime ordering.

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
