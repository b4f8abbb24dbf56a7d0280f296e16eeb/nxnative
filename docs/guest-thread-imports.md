# Typed guest thread imports

`dm_guest_thread_spawn` still creates a real **guest SDK thread**. The guest's
`CreateThread`/`StartThread` establish its SDK `ThreadType`, handle, TLS and lock
ownership; `WaitThread`/`DestroyThread` retain their existing teardown roles.
This is not an Atmosphere/libnx thread runtime linked into the iOS host.

## Source and generated names

`dm/host/guest_thread_signatures.inc` and the optional self-test
`guest_mutex_signatures.inc` are the single signature sources for their families.
They produce
both the host's typed C pointers and readable `nn::os` C++ declarations in
`guest_thread_abi.h`. The offline `dm/tools/guest_thread_imports.cpp` takes typed
addresses of these declarations, with compile assertions against the pointer
types. The compiler, not a handwritten encoder, generates all eight ABI names.

The checked-in `guest_thread_imports.generated.inc` is the only authored-code
location containing those eight mangled spellings. Normal player builds consume
it as C; they need neither a C++ runtime nor an extra generation tool. Maintainers
regenerate/check with Python's standard library and Clang:

```sh
python3 dm/tools/generate_guest_thread_imports.py
python3 dm/tools/generate_guest_thread_imports.py --check
```

The default generator explicitly targets `aarch64-none-elf` and verifies ELF64,
AArch64 and one unadjusted ABS64 relocation per declaration. This avoids Apple's
Mach-O leading-underscore convention and accidental x86/LLP64 generation. Hosts
with only libclang may use `--libclang /path/to/libclang.so`; this verifies the
AArch64 ELF target and uses the public Clang frontend's mangling API, **not** the
ELF object check. CI must still pass the default compiler/object path.

The generated header includes a digest of all declaration inputs. A stale header
fails `--check`, including changes to return types that C++ symbol mangling does
not encode. Changes to the single signature source change C and C++ types together.

## Binding and lifetime contract

The first valid spawn performs one mutex-protected binding before allocating
anything or calling the SDK. It uses the loader's existing global ELF lookup
order for each export, never a preferred-module fallback. All four thread exports
must identify one coherent permanent SDK module. Every resolved address, including
`malloc` and `free`, must belong to a loaded, non-NRO module. Missing/partial,
unknown/unloadable-owner, and mixed-SDK tables fail without publishing callbacks.
Success and failure are cached for the process's immutable NSO load set.

The allocator pair is captured exactly as global lookup resolves it: legitimate
interposition can place `malloc` and `free` in different permanent modules. Module
identity alone does not prove allocator compatibility; the existing supported
contract is that these guest exports form a matching allocation family. The
binding does not attempt to infer a different allocator family.

The mutex self-test has a separate lazy binding. Only `DM_GUEST_SPAWN_TEST`
resolves `InitializeMutex`, `LockMutex`, `IsMutexLockedByCurrentThread` and
`UnlockMutex`, before the self-test allocates or creates anything. Ordinary spawn
never looks up these optional symbols. The self-test requires the mutex exports
to share the thread SDK backend, preserves C/C++ `bool` for the recursive argument
and ownership result, and contains no handwritten mangled names or raw casts.

Each worker retains its original free/wait/destroy callbacks through join. Its
unaligned guest stack allocation, alignment padding, MapMemory eligibility
preflight, and guest SDK's authoritative borrow are unchanged. The original
allocation is released only after wait and destroy; resident workers retain it.
Permanent NSOs remain mapped for process lifetime. This binder intentionally does
not support guest runtime/module-set replacement or unloadable NRO thread backends.

## Verification and limits

- `make -C dm/host/tests guest-thread-imports` races 24 first users and checks
  cached success/failure, every missing export, transient/mixed owners, captured
  allocator interposition, and lookup count (once, never once per spawn).
- `guest-thread-stack` retains the real SVC MapMemory ownership/lifecycle suite,
  tests every missing export before allocation, every missing optional mutex export before the self-test
  starts, ordinary-spawn independence from mutex exports, and simulated 16 KiB pages.
- `guest-thread-abi` calls synthetic C++ exports through the production typed C
  binding, checking arguments, callback, return value, true/false mutex ABI and operation order.
- `dm/tools/tests/test_guest_thread_imports.py` checks reproducibility, signature
  changes, stale-file failure and rejection of non-AArch64 object formats.
- The CI-only handoff requires an Apple-arm64 runner, AArch64 ELF regeneration
  and the synthetic C/C++ boundary with `DM_REQUIRE_AARCH64`; workflow changes
  are deliberately excluded from this feature patch:

  ```sh
  test "$(uname -m)" = arm64
  python3 dm/tools/generate_guest_thread_imports.py --check --cxx "$(xcrun --find clang++)"
  make -C dm/host/tests -B CC=clang CXX=clang++ GUEST_ABI_FLAGS=-DDM_REQUIRE_AARCH64 guest-thread-abi guest-thread-imports guest-thread-imports-sanitize guest-thread-imports-tsan
  ```

A host x86 test is not ARM64 execution evidence. Even native synthetic tests do
not validate Nintendo SDK return representation, the retained `ThreadType` buffer
size, TLS initialization, or actual guest lock behavior. Those remain the existing
helper's SDK assumptions. The next integration checkpoint is passing native CI
and exercising `DM_GUEST_SPAWN_TEST=1` plus a real resident movie worker on the
user's authorized guest inputs; no private binaries are needed for these unit tests.
