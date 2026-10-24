# Exact ordinary pool registration and retirement

Ordinary `MemoryPoolInitialize` obtains a unique, nonzero Metal registration
token and uses that same token for dirty tracking. `MemoryPoolFinalize` retires
those exact entries rather than the first one with the same CPU start. Duplicate
storage, same-start different-size pools and page-rounded overlaps remain
separate registrations. Tokens are issued under the existing Metal registry
mutex and never recycled; exhaustion stops instead of wrapping.

The legacy `mtl_pool_add` / `mtl_pool_del` and `nvn_dirty_add_range` /
`nvn_dirty_remove_range` APIs keep their pointer-keyed behavior in a separate
zero-token domain. Legacy removal cannot consume an ordinary entry, and ordinary
removal cannot consume a legacy entry. Zero-token ordinary add/remove calls do
nothing. Sparse mappings and their pre-supplied-storage path stay unchanged.

Public pool, buffer and texture addresses remain CPU identity. Their getters,
the address resolver, bounds arithmetic, cache invalidation and deferred Metal
release are unchanged. No shadow VA, backing allocation or storage lease is
introduced.

## Tests

Portable token/selector test (no Apple SDK):

```sh
make -C dm/gfx/tests check_t_pool_registration_identity
```

It checks all 24 retirement orders under modeled 4 KB and 16 KB page rounding,
including identical pools, same-start different extents, distinct CPU starts
sharing one rounded extent, legacy isolation, stale tokens and issuer exhaustion.
It exercises the production issuer/selector with a small registry fixture, not
Metal's index or a second runtime address map.

Native VM and Metal tests:

```sh
make -C dm/gfx/tests GAME=010020D01AD24000 \
  t_dirty_registration t_dirty_race t_dirty_slots t_pool_registration
dm/gfx/tests/t_dirty_registration
dm/gfx/tests/t_dirty_race
dm/gfx/tests/t_dirty_slots
MTL_DEBUG_LAYER=1 dm/gfx/tests/t_pool_registration
```

The profile supplies build constants, not game files. `t_dirty_registration`
uses anonymous VM and production crash/pre-write dispatch, holding at most five
host pages. It checks all 24 retirement orders with both host-page and 4 KB
aligned ranges, exact surviving tokens, repeated retirement, legacy isolation,
slot reuse, restored protection, overlapping write generations and fresh writes
through surviving registrations. Writes while the registry mutex is held ensure
the fault path does not acquire it; an alarm bounds a deadlock regression.

`t_pool_registration` uses real NVN Initialize/GetAddress/Finalize and the Metal
registry. It checks exact surviving Metal views, dirty live counts, legacy
isolation, unchanged public addresses, lazy residency and stale token removal.
An encoded copy captures a no-copy view before registration retires; caller-owned
storage is deliberately kept alive until the existing deferred-release path has
completed. It does not test freeing or remapping that storage while in flight.

Local Linux results: portable test **4,641 checks**, also with ASan/UBSan (leak
checking disabled because this executor runs under ptrace); dirty registration
**6,945 checks**, normally and with UBSan; existing dirty-race test passed. The VM
runs used temporary Linux clock/signal shims, not Apple's crash implementation.
An additional temporary fixture grouped protection at 16 KB over aligned Linux
mappings and passed normally and with UBSan; this is not native Apple VM testing.
A mutation restoring same-CPU-start ordinary retirement failed the exact-owner
assertion. Native Mac/Metal tests and game/device validation remain unrun here;
these Linux results are not an Apple validation claim.

## Unchanged lifecycle and coherence limits

- Dirty span handles point into stable table slots, but do not carry registration
  generations. Reusing a slot for the same bytes resets its page generations;
  an old cache stamp is not proof of content identity after retire/recreate.
  Global `now == seen` shortcuts and concurrent slot reuse have no new reader-pin
  protocol. This change fixes retirement selection, not stale dirty-cache history.
- Fault/pre-write/protection code is unchanged. It releases overlapping live
  ranges and stamps protected pages it releases; protection still selects one
  containing range rather than a canonical shared-backing owner. Tests re-arm
  survivors and do not prove universal alias coherence.
- Metal TLS epochs and deferred buffer release do not own caller CPU storage.
  There is no new allocator/remap synchronization or backing-storage lease.
- `fl_drop` remains global and address-based. Finalizing an overlapping pool can
  remove flushed coverage another pool uses. Exact dirty retirement does not
  fix or change that separate ownership behavior.
