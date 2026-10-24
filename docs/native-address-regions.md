# Shared native address regions

`dm/host/hle_address_space.[ch]` is a host-owned address-region manager, shared
with the production SVC physical mapping path. It registers the **actual native
alias range** returned by the existing host reservation; it neither invents a
64 GB guest range nor changes a profile's reservation sizes.

## What is active

`hle_svc.c` creates the manager in `DM_AS_EXTERNAL_RESERVATIONS` mode. The SDK
still owns every virtual-address reservation. The manager handles:

- `MapPhysicalMemory` / `UnmapPhysicalMemory` commit tracking and the existing
  profile-controlled application-pool limit;
- `QueryMemory` committed/free runs (committed alias pages now report Heap/RW,
  rather than being incorrectly reported as free);
- `GetInfo` used-memory accounting and `DM_ALIAS_WINDOWS` diagnostics;
- serialization of map/unmap, backend protection changes, heap resizing and pool
  checks, including rollback of accounting when protection fails;
- zero initialization of newly committed logical pages, without overwriting
  already committed bytes in overlapping requests or live host-page siblings.

Repeated or overlapping maps charge only newly committed 4 KiB pages. Repeated
unmaps cannot make the total negative. Host-page permissions are retained while
a neighboring logical page is live, including 16 KiB Apple host pages. A failed
map/decommit does not modify the manager bitmap. Invalid/overflowing or unaligned
physical-map ranges fail with the existing `0xd401`; pool exhaustion remains
`0x10801`. Heap resize now rejects non-2-MiB sizes, checks already committed alias
memory when `mem_map_limit` is enabled, and retains its old size on backend
failure. Disabled `mem_map_limit` keeps its existing accounting policy.

Initialization is `pthread_once`-protected. The complete profile partition must
fit the real reservation and its boundaries must be host-page aligned; all 58
current profiles pass both 4 KiB and 16 KiB checks. Zero-size alias regions are
unsupported (none of the shipped profiles has one).

## Host reservation core

`DM_AS_HOST_RESERVATIONS` is available to host callers that exclusively own their
registered range. It is **not enabled for the production guest alias region**.

Reservations preserve the exact requested size and absolute power-of-two
alignment. First-fit placement scans a bounded, sorted reservation table.
Releasing an exact reservation immediately makes the surrounding free gaps one
gap again; no separate free-list node allocation can fail during coalescing.
Page operations must fit inside one reservation. Release refuses committed
regions with `DM_AS_BUSY`, so a future SDK adapter must successfully decommit
first rather than silently losing ownership.

The configured region count is a hard metadata bound, with distinct
metadata-exhaustion, address-exhaustion, invalid-input, budget and backend errors.
No operation allocates metadata after initialization. Metadata is host memory:
one bitmap bit per guest page (32 KiB per GiB at 4 KiB pages), an optional bit
per host page for exposure history (8 KiB per GiB at 16 KiB host pages), one
two-word record per configured reservation, and a small fixed control block. Production uses
zero reservation records in external mode. Reservations consume no committed
memory budget. External mode rejects reserve/release and does not advertise
uncommitted SDK reservations as host-allocatable free space.

Mutations and queries are serialized by one manager mutex. Reservation search,
insertion and removal are bounded by the configured region count; bitmap
operations and queries scan words, bounded by the registered range. Fresh-zero
initialization checks also scan host-page exposure history; actual zeroing is
linear in newly committed bytes on previously exposed host pages. This is not
a real-time or lock-free allocator. The backend callback runs under that lock
and must leave accessibility unchanged on failure; it must not re-enter the
manager. Destruction requires quiescent callers and refuses live ownership/maps.

## Deliberately not activated

No `nn::os` allocator symbol is redirected. No patch or game profile is changed.
**This does not yet remove the SDK's node area or reduce the required patches.**
It supplies a tested shared backend and reservation core for that next step.

`loader.c` redirects named symbol relocations, not arbitrary internal branches,
relative pointers, or addresses obtained through `module_sym()`. Hooking only
`AllocateAddressRegion` would leave two independent allocators handing out the
same address space. An environment switch is not sufficient evidence to enable
that unsafe mixture; no such switch is provided.

Before guest activation, establish for each supported SDK/call-path family:

1. Verified binary ABIs and coverage of initialization, allocate-region,
   combined allocate/commit, free-region, allocate/free-pages, resource queries,
   internal direct calls and dynamically loaded NRO callers.
2. One authoritative reservation owner, including all initialization and teardown
   paths. Public symbol presence alone is not call-coverage proof.
3. Compatibility rules for guard gaps, size-dependent alignment, error mapping,
   combined-operation rollback, and free-region decommit behavior. The host core
   deliberately does not guess or silently relax SDK policy.
4. Device/game regression evidence before selectively retiring any old patch.

The [Atmosphere public API](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/include/stratosphere/os/os_virtual_address_memory_api.hpp)
and its manager were inspected only as interface/behavior references, not copied.
They are not proof of every Nintendo SDK binary's ABI or internal call graph.

## Native mapping limitations

Commit accounting describes logical guest pages, not host RSS. A live 4 KiB
neighbor keeps the encompassing 16 KiB host page readable/writable, even if
another 4 KiB slice is logically decommitted. Retained bytes can still be read
through that uncommitted slice until reuse; native host pages cannot provide
4 KiB permission isolation. This is explicitly tested, not presented as full
Horizon memory-isolation fidelity.

Production recommit **does zero** each formerly uncommitted logical slice, after
`mprotect` succeeds and before bitmap/accounting publication. Fresh anonymous
host pages are already zero-filled by the OS: production explicitly enables
`initially_zero` only for its new anonymous reservation and skips initialization
on each host page's first exposure. A bounded exposure-history bitmap is never
cleared by decommit. Subsequent newly committed slices are cleared, including
logical slices never previously committed if their host page was ever writable.
This keeps multi-GiB first-map startup probes lazy rather than touching every
page resident. Reused caller buffers must not enable this option. It preserves live
siblings and repeated/overlapping maps. The existing dirty-tracker pre-write hook
is notified after a successful protection change for the entire host-page run
that was opened, conservatively invalidating neighboring tracked data too.
Failed protection does not initialize bytes or notify dirty tracking. The
[reference kernel](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libmesosphere/source/kern_k_page_table_base.cpp)
uses the process's fill policy for newly allocated physical pages (normally
zero); this host does not implement its debug-fill policy.

## MapMemory source ownership

The production SVC dispatcher also tracks the source of each `MapMemory` stack.
Mapping borrows one nonempty, 4 KiB-aligned source interval; a second mapping
cannot borrow any part of it until an **exact** destination/source/size
`UnmapMemory` copies the stack contents back. The source must be wholly committed
alias memory, an opened heap interval, or writable data/BSS in a loaded static
module. Free logical slices are rejected even when their host page is accessible.
Code, read-only data, executable patch areas and stacks are not eligible sources.
The loader's read-only key-table page and 16 KiB-rounded text/patch seals also
split nominal CodeData query runs; no source may include these nonwritable bytes.
Dynamic NRO module sources are deliberately unsupported: the existing loader
restores their data on reload outside the memory locks. Supporting that lifetime
requires loader integration, not merely recognizing a writable module address.

`QueryMemory` preserves the source's underlying Heap/CodeData state, reports
attribute bit 0 (`Locked` / `IsBorrowed`) and no user permission, and splits the
surrounding unlocked runs at source ownership boundaries. This is **not** the
distinct permission-lock attribute (bit 4). Exact unmap restores the ordinary
RW source run and reports the destination as free. Its public `MemoryInfo`
output is exactly 40 bytes, including zero IPC/device counts and reserved word;
PageInfo is returned in register x1 by this raw SVC dispatcher.

Both physical commit and decommit reject an entire request that intersects a
borrowed source, without touching other pages in that request. Heap shrink
similarly refuses to remove a borrowed tail; disjoint changes remain allowed.
The legacy permission/attribute SVC stubs reject mutations intersecting borrowed
sources; general permission/attribute changes remain unimplemented.
Map/unmap, query, physical operations and heap resizing observe the lock order
`g_map_lock` → `g_phys_mu` → address-manager lock. Source ownership publishes
only after destination protection and copying succeed. A protection failure
returns an error without copying, acquiring ownership or notifying dirty
tracking. Full mapping metadata returns `OutOfResource` rather than aborting.

Map/unmap address misalignment returns `0xcc01`; zero/misaligned size returns
`0xca01`; overflow, invalid source state, conflicts and unmatched unmaps return
`0xd401`; a destination outside the stack region or two valid exact ranges
belonging to different mapping pairs return `0xdc01`. Partial unmap remains
unsupported and returns `0xd401`, even within a valid mapping. Validation
precedes host memory access. These contracts were independently implemented
using the [reference SVC validation](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libmesosphere/source/svc/kern_svc_memory.cpp),
[MapMemory state transitions](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libmesosphere/source/kern_k_page_table_base.cpp)
and [public MemoryInfo definition](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h)
as behavioral/interface references; no reference implementation was copied.

This remains the native **copy/move** implementation, not coherent aliases.
Source and destination do not share live writes: the stack copy is authoritative
until copyback. Source permission removal is logical only; host access remains
possible, and an unmapped stack's host pages are retained so that adjacent live
4 KiB stacks on a 16 KiB host page are not revoked. Dirty tracking is notified
for the full destination host-page run on map and the source bytes on copyback.
No claim of per-4-KiB hardware isolation, partial unmap, general permission-SVC
support or NRO lifecycle ownership is made. Existing game patches/profiles and
the SDK's address-region reservation ownership are unchanged.

The generic core delegates initialization to an optional, non-failing callback;
without it, the core makes **no zero-fill promise**. That callback must not
allocate, re-enter the manager or change page protections. It sees only old-zero
bitmap runs, runs under the manager lock, and is followed by no fallible step.
The native block must remain owned by the caller, with no foreign unmaps or
remaps. Apple runtime and GPU dirty-tracking integration still need device
validation; the portable SVC harness verifies the pre-write notification boundary.

## Verification

Profile-free production tests run on Linux or macOS, without game binaries:

```sh
make -C dm/host/tests address-space address-space-svc
make -C dm/host/tests address-space-sanitize address-space-svc-sanitize
make -C dm/host/tests address-space-tsan address-space-svc-tsan
make -C dm/host/tests map-memory map-memory-sanitize map-memory-tsan
python3 -m unittest dm.tools.tests.test_address_regions
```

The tests reserve real native `mmap` blocks. They cover exact-size/aligned
placement, fragmentation, coalescing, metadata bounds, reserve/commit separation,
overlaps, injected protection failures, pool limits, queries, 16 KiB page sharing,
22,000 deterministic model steps, a 3 GiB lazy-commit/one-page-reuse test and
concurrent operations. The SVC harness
includes the real dispatcher/backend and tests both `mem_map_limit` policies,
concurrent initialization, native read/write, heap rollback, and free-range
queries outside the alias boundaries. The normal targets are also in the
existing host `run` suite.

The MapMemory SVC harness covers lifecycle/copyback, source/destination overlap,
invalid/overflowing inputs, exact 40-byte query writes with unaligned guard
buffers, source-state splits, readonly module seals, bounded metadata exhaustion
and reuse, dirty-prewrite notifications, protection failure, backing-decommit and
heap-shrink protection, and concurrent ownership/query/decommit/shrink races.
It runs native and simulated 16 KiB host pages with both pool-accounting policies;
the simulation does not substitute for Apple hardware protection/device tests.

For environments where LeakSanitizer cannot use ptrace, only leak detection can
be disabled with `ASAN_OPTIONS=detect_leaks=0`; this is not a leak-check pass.
These focused tests do not substitute for full Apple host/iOS builds or game and
device validation.
