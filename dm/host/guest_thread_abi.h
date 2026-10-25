#ifndef DM_GUEST_THREAD_ABI_H
#define DM_GUEST_THREAD_ABI_H
// No SDK, host standard-library headers, or host C++ runtime are linked here.
// The generator targets AArch64 ELF/LP64, not Apple's Mach-O symbol spelling.
#ifdef __cplusplus
namespace nn { namespace os { struct ThreadType; struct MutexType; } }
typedef nn::os::ThreadType dm_guest_thread_type;
typedef nn::os::MutexType dm_guest_mutex_type;
typedef bool dm_guest_bool;
#else
typedef struct dm_guest_thread_type dm_guest_thread_type;
typedef struct dm_guest_mutex_type dm_guest_mutex_type;
typedef _Bool dm_guest_bool;
#endif
typedef unsigned long dm_guest_size;
typedef void (*dm_guest_thread_entry)(void *);
#define DM_GUEST_THREAD_SIGNATURE(name, result, parameters) typedef result (*dm_guest_##name##_fn) parameters;
#include "guest_thread_signatures.inc"
#define DM_GUEST_MUTEX_SIGNATURE DM_GUEST_THREAD_SIGNATURE
#include "guest_mutex_signatures.inc"
#undef DM_GUEST_MUTEX_SIGNATURE
#undef DM_GUEST_THREAD_SIGNATURE
#ifdef __cplusplus
static_assert(sizeof(dm_guest_size) == 8 && sizeof(void *) == 8 && sizeof(int) == 4 && sizeof(dm_guest_bool) == 1, "guest thread ABI requires LP64");
namespace nn { namespace os {
#define DM_GUEST_THREAD_SIGNATURE(name, result, parameters) result name parameters;
#include "guest_thread_signatures.inc"
#define DM_GUEST_MUTEX_SIGNATURE DM_GUEST_THREAD_SIGNATURE
#include "guest_mutex_signatures.inc"
#undef DM_GUEST_MUTEX_SIGNATURE
#undef DM_GUEST_THREAD_SIGNATURE
} }
#else
_Static_assert(sizeof(dm_guest_size) == 8 && sizeof(void *) == 8 && sizeof(int) == 4 && sizeof(dm_guest_bool) == 1, "guest thread ABI requires LP64");
#endif
#endif
