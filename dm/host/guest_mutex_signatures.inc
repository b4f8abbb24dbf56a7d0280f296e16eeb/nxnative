// Optional self-test imports only; ordinary thread creation does not need these.
// C and C++ both use the ABI's bool, not an int-shaped substitute.
DM_GUEST_MUTEX_SIGNATURE(InitializeMutex, void, (dm_guest_mutex_type *, dm_guest_bool, int))
DM_GUEST_MUTEX_SIGNATURE(LockMutex, void, (dm_guest_mutex_type *))
DM_GUEST_MUTEX_SIGNATURE(IsMutexLockedByCurrentThread, dm_guest_bool, (const dm_guest_mutex_type *))
DM_GUEST_MUTEX_SIGNATURE(UnlockMutex, void, (dm_guest_mutex_type *))
