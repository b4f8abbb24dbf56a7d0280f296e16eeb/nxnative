// Offline compiler input only; never linked into the host or a guest.
#include "../host/guest_thread_abi.h"
#define DM_GUEST_THREAD_SIGNATURE(name, result, parameters) \
    static_assert(__is_same(decltype(&nn::os::name), dm_guest_##name##_fn), "guest import signature drift"); \
    extern "C" { \
    __attribute__((used, section(".dm_import." #name))) \
    dm_guest_##name##_fn dm_anchor_##name = &nn::os::name; \
    }
#include "../host/guest_thread_signatures.inc"
#define DM_GUEST_MUTEX_SIGNATURE DM_GUEST_THREAD_SIGNATURE
#include "../host/guest_mutex_signatures.inc"
#undef DM_GUEST_MUTEX_SIGNATURE
#undef DM_GUEST_THREAD_SIGNATURE
