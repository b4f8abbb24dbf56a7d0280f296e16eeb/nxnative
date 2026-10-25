# Native memory backend and gated ownership

The physical-memory authority is the upstream `hle_svc.c` bitmap backend:
`g_alias_bits` records committed 4 KiB pages, `g_alias_exposed` records host-page
exposure, and `g_map` records stack destinations and borrowed sources. One
`g_map_lock` serializes validation, protection/copying, publication, heap changes,
budget checks and memory-query snapshots. The SDK still owns reservations in
production. Profiles, application-pool policy and existing patches are unchanged.

## Rebase boundary

This integration is based on upstream `b7dfba4` (2026-10-25), including `cae5ec2`
("Keep guest memory queries, budgets and stack ownership consistent"). Those
upstream fixes are retained, not reintroduced as a competing implementation:

- Committed/free alias runs and borrowed-source state in `QueryMemory`, including
  the complete 40-byte MemoryInfo output and module protection boundaries.
- Consistent used-memory and heap/alias budget snapshots; one-time initialization.
- Lazy first exposure of fresh anonymous pages, zeroing only logical holes on
  reused host pages, and preservation of live 4 KiB neighbors on 16 KiB hosts.
- Recommit across borrowed stack sources, including extending a committed window
  around them. It preserves all already committed bytes. Only **decommit** is
  rejected when it intersects a source needed for eventual stack copyback. This
  preserves the upstream Raging Loop compatibility fix.
- Fail-fast host protection failures for heap, stack mapping and physical pages.
  They abort before publishing new accounting/ownership or returning guest
  success; they are not recoverable allocation errors.
- The exact heap-probe diagnostic formats, the memory regression/profile tests,
  and upstream guest-malloc-backed helper stacks used by Tears of the Kingdom.

The prior experimental `hle_address_space.[ch]` and its standalone tests were
removed. Its physical ledger duplicated the upstream bitmap, and its independent
reservation allocator is unnecessary beside the gated `hle_vamm` owner. There
is no `g_phys_mu` or second physical-manager lock in the integration.

## What the feature adds

`dm_vamm_svc_change_pages` extracts the existing physical SVC transaction so both
raw SVC callers and the native Vamm callback use the **same** validation, bitmap,
protection, initialization and accounting path. `dm_vamm_svc_info` exposes the
same raw alias-domain and system-resource values as GetInfo. It does not invent
an address range or transfer reservations.

The StandardAllocator provider adds narrow scopes around `g_map_lock` to inspect
real backing, borrowed sources and physical budget. It drops that scope around
owner callbacks, which may re-enter the SVC path. Its nesting-safe, thread-local
allocation scope enforces a conservative image/heap/alias budget at the actual
commit, even if the ordinary legacy profile permits unlimited physical maps.
Ordinary SVC policy and the SDK's ownership remain unchanged.

`hle_vamm.[ch]` is the optional **reservation-only** owner. The
[shared-memory runtime](shared-memory-runtime.md) connects that explicit owner,
the provider and retained adapter lifetimes; the
[startup hook transaction](memory-hook-transaction.md) journals complete method
groups before guest execution. Both production adapter gates remain closed.
No production startup path installs them, and no SDK node-area/alignment or
per-game arena cut is removed.

The host guest-thread helper retains upstream's matched guest malloc/free
ownership and frees only after SDK Wait + Destroy/copyback. Additional lifecycle
checks cover invalid sizes/entry, guest context, source preflight, allocation or
CreateThread failure cleanup, cached matching callbacks, and self-join rejection.
The source preflight does not reserve memory: MapMemory remains the authoritative
atomic borrow.

## Native limits

Logical committed bytes are not RSS. A decommitted 4 KiB slice can remain host
accessible while a live sibling keeps their 16 KiB host page open. Recommit
clears reused logical holes, not live pages; first exposure remains lazy.
Dirty tracking is notified after successful protection changes for the complete
opened host-page run, and before source copyback.

MapMemory remains a **copy/move**, not coherent aliases. The destination copy is
authoritative until exact destination/source/size unmap copies it back. Borrowed
sources retain their underlying Heap/CodeData state but report Locked/IsBorrowed
and no user permission. Source protection is logical, not independent 4 KiB
hardware isolation. Heap shrink and permission/attribute stubs cannot invalidate
a borrowed source. Partial unmap, general permission changes and dynamic NRO
source lifetime integration remain unsupported.

A physical free run says nothing about reservation ownership. The optional Vamm
owner must never coexist with an active SDK reservation owner. Activation still
requires complete main/SDK/subsdk caller and ABI evidence, a proven no-prior-owner
handoff, direct/inline/private-path coverage, and target-device gameplay,
concurrency, GPU and teardown validation. Synthetic tests alone cannot justify
removing any patch or enabling a partial hook group.

## Regression map

| Boundary | Fixture / check |
| --- | --- |
| Upstream query, bitmap, lazy exposure, borrowed-source recommit and fail-fast policy | `t_svc_memory.c` (`memory`, `memory-tsan`; `memory-dirty` on Apple) |
| Heap-probe diagnostic compatibility | `test_memory_profile.py` (`memory-profile`) |
| Heap transition and pre-abort state | `t_heap_size.c` (`heap_size`, `heap-size-sanitize`) |
| Additional portable SVC range/accounting and stack-ownership regressions | `address-space-svc`, `map-memory` and sanitizer variants |
| All shipped profile boundaries | `dm.tools.tests.test_address_regions` |
| Provider budgets, source races and exact lease retention | `standard-allocator-provider` and sanitizer variants |
| Reservation-only owner and native SVC bridge | `vamm`, `vamm-native` and sanitizer variants |
| Guest helper lifetime/copyback | `guest-thread-stack` and sanitizer variants |
| Runtime lifetimes and complete loader transaction | `memory-runtime`, `memory-hooks`, `memory-runtime-startup`; Apple `memory-hooks-loader` |

These checks use synthetic inputs and real mmap-backed storage, not proprietary
game binaries. Simulated 16 KiB pages do not replace Apple hardware testing.
Normal/sanitizer results must be reported for the actual rebased commit; earlier
branch results are not proof of this integration. Where ptrace blocks
LeakSanitizer, `ASAN_OPTIONS=detect_leaks=0` permits address/undefined checks but
is not a leak-clean claim. Upstream game smoke-test evidence is upstream's;
this integration does not claim new game, rendering, IPA or iOS-device validation.
