// Single signature source for C function pointers and C++ guest declarations.
// Result keeps the existing helper's 32-bit success/error ABI. These declarations
// describe imports, not an SDK implementation or a definition of ThreadType.
DM_GUEST_THREAD_SIGNATURE(CreateThread, int, (dm_guest_thread_type *, dm_guest_thread_entry, void *, void *, dm_guest_size, int))
DM_GUEST_THREAD_SIGNATURE(StartThread, void, (dm_guest_thread_type *))
DM_GUEST_THREAD_SIGNATURE(WaitThread, void, (dm_guest_thread_type *))
DM_GUEST_THREAD_SIGNATURE(DestroyThread, void, (dm_guest_thread_type *))
