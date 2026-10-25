# Opt-in 2D → six-face cube compatibility candidate

This fork-only experiment replaces the zero cube for a narrow class of mismatched
scalar bindings. It repeats the **resolved 2D view's pixels** on all six faces.
That preserves content, but does **not** establish the intended environment-map
semantics or prove the cause of a particular rendering defect. Default behavior
is unchanged. Enable explicitly with `DM_NVN_CUBE_COPY=1`; unset it or set `0` to
restore the zero fallback. No shader rebuild or translation-chain change is needed.

## Scope and safety

- Only scalar `TEX` / `TEXU` float cube or cube-array bindings. Mixed dynamic handle
  arrays retain zero for incompatible unselected entries.
- Single-layer, square, single-sample **private** color sources, at most 256×256;
  no buffer-backed, sparse, framebuffer-only, integer, depth or stencil sources.
  Multi-layer arrays with fewer than six layers, non-square images and oversized
  images keep the zero fallback. A source view can select a mip range and one
  layer of a larger parent.
- The destination is six-layer private storage with the **same pixel format and
  visible mip count**; its cube view keeps the source swizzle. Copy coordinates
  are relative to the resolved source view, not its parent. Each face receives
  every full mip, including compressed 2×2 and 1×1 tail mips.
- This is an **allocation cache, not a persistent content cache**. Existing
  `tex_by_id` dirty/upload invalidation resolves the current source. All six faces
  are refreshed before every consuming draw/dispatch; duplicate scalar uses of
  the same source/type within one consumer share that refresh. This deliberately
  avoids assuming that an unchanged Metal object means unchanged GPU-written
  content. It can split render passes and add copy bandwidth.
- Lazy and load-action clears land before the refresh. Uploads are flushed onto
  the Metal 4 queue before the replay copy. Existing copy/encoder barriers order
  earlier writes, the copy, and the consumer. No CPU texture readback is added to
  production.
- Only explicitly supported draw, transform-feedback and compute binding scopes
  can use a prepared cube. Geometry helper, fused and deferred-reordered consumers
  retain zero. An indirect multi-draw cannot take its binding-reuse fast path
  after binding this fallback. If the source changes between preparation and
  binding, it also retains zero rather than using an old snapshot.
- At most **16 entries / 16 MiB** of charged cube allocations. The charge is the
  larger of Metal's allocation estimate and actual `allocatedSize`. The six-face
  allocation costs approximately six times one visible source image, plus Metal
  alignment/padding: a 32×32, six-mip BC7 source has 1,392 bytes of logical blocks,
  or 8,352 bytes replicated, but its actual allocation can be larger.
- Cache entries hold their cube storage/view strongly and the source weakly.
  Each copy separately retains the source through the submission's deferred
  lifetime. Storage enters the existing residency set before GPU use. Entries
  used by incomplete submissions or the current consumer cannot be evicted.
  Saturation returns zero, without allocating an unbounded retired list or waiting
  for the GPU. Completed entries are removed from residency when evicted.

`CUBE_COPY` logs report the first 16 refreshes and every 600th thereafter, with
texture ID, shape, format, visible mips, parent offsets, total allocations and
charged bytes. The first 16 refusals are logged too. No image pixels are logged.

## Verification

Portable policy and wiring checks (no Apple SDK or game files):

```sh
python3 -m unittest -v dm/gfx/tests/test_cube_copy.py
make -C dm/gfx/tests GAME=010015100B514000 t_cube_copy_policy
./dm/gfx/tests/t_cube_copy_policy
ASAN_OPTIONS=detect_leaks=0 \
  NVN_CUBE_COPY_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  python3 -m unittest -v dm/gfx/tests/test_cube_copy.py
```

The sanitizer command disables LeakSanitizer for ptrace-hosted Linux executors;
**it is not a leak test**. Portable checks exercise the production admission,
overflow/budget and fence policy, plus narrow wiring checks. They cannot prove
Metal API acceptance, GPU synchronization or rendering correctness. Native
coverage still does not include a complete production draw/dispatch stream with
image writes or actual pool finalization; keep those runtime
trace checks distinct from the direct-copy fixture.

On a Metal 4-capable Apple-silicon Mac with current Xcode:

```sh
make -C dm/gfx/tests GAME=010015100B514000 t_cube_copy
MTL_DEBUG_LAYER=1 DM_RES_CHECK=1 dm/gfx/tests/t_cube_copy --disabled
MTL_DEBUG_LAYER=1 DM_RES_CHECK=1 DM_NVN_ENC_THREAD=0 dm/gfx/tests/t_cube_copy
MTL_DEBUG_LAYER=1 DM_RES_CHECK=1 DM_NVN_ENC_THREAD=1 dm/gfx/tests/t_cube_copy
# Or both fixtures plus policy:
make -C dm/gfx/tests GAME=010015100B514000 check_t_cube_copy
```

The native fixture includes the production implementation and generates all its
own data and MSL. It checks default-off zero behavior, RGBA8 and linear BC7
(format 152) / sRGB BC7 (format 153) full-mip copies
to six faces, nonzero mip/layer source views, same-consumer deduplication,
in-place GPU mutation followed by refresh, allocation reuse, pending clear
ordering, explicit binding-scope rejection, integer-format refusal, six-direction
cube sampling with a nontrivial channel swizzle, cross-submission refresh, slot
and actual charged-byte budget refusal, completed-fence eviction, descriptor-ID
and caller-object recycling, source replacement, and unregistering sources before
in-flight submissions finish. The two encoding-thread settings run as separate
processes. Its byte comparisons read only synthetic known test outputs;
this does not reopen any separate texture-readback investigation. BC7 raw-block
copy tests are not a test of environment-map meaning. Native checks must fail,
not silently skip, if BC7 support is unavailable. Inspect validation stderr as
well as the exit code.

The existing fork CI can **compile** this fixture and the Mac/iOS production
sources without private assets. Virtual Mac runners without Metal 4 cannot
execute the fixture. Do not describe a compile-only job as a GPU pass.

### Controlled runtime test

1. Use a separate checkout of this branch. Record `git rev-parse HEAD`, the local
   diff (if any), host build configuration, OS/GPU and shader-chain stamp. Do not
   replace unrelated GPU, memory, IPC or smoke-test branches.
2. Build the host using the existing profile and existing privately held shader
   libraries. Run the same route once with the flag unset, then with
   `DM_NVN_CUBE_COPY=1` and `DM_RES_CHECK=1`. On iOS these values can be placed in
   the app's existing `Documents/env.txt` mechanism.
3. Capture logs / the user's existing runtime trace, **not screenshots**. Verify
   that the affected scalar binding changes from zero to `CUBE_COPY`, with the
   expected source format, 32×32 size and six mips where applicable. Confirm
   source/view replacement or in-place updates cause the next consumer's refresh.
4. Check for Metal validation errors, nonresident-resource reports, GPU command
   failures, unexpected zero refusals, pass/copy cost, and stable cache bytes at
   or below 16 MiB across scene changes. If the target binding is never prepared,
   report which eligibility/reordering case retained zero; do not call it fixed.
5. Restore the unset/`0` baseline after the trial. A successful candidate trace
   proves that this fallback was exercised, not that it caused or solved another
   missing-material symptom.

## Apple API basis

Apple's [texture-view rules](https://developer.apple.com/documentation/metal/mtltexture/newtextureviewwithpixelformat:texturetype:levels:slices:)
do not allow a one-layer 2D texture to become a cube by reinterpretation; a cube
view requires six faces. [Texture-region copies](https://developer.apple.com/documentation/metal/mtlblitcommandencoder/copy(from:sourceslice:sourcelevel:sourceorigin:sourcesize:to:destinationslice:destinationlevel:destinationorigin:))
require matching source/destination format and sample count, block-aligned
compressed origins, and full-mip extents clamped to image edges for tail blocks.
[Metal 4's core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api)
does not retain resources for the application; allocation residency and object
lifetime both need explicit management. These rules support this copy mechanism,
not the inferred semantics of a game's mismatched sampler binding.
