#ifndef DM_GUEST_THREAD_IMPORTS_H
#define DM_GUEST_THREAD_IMPORTS_H
#include "guest_thread_abi.h"
// Only this generated file contains the ABI spellings. Runtime and test code
// refer to named descriptors, never duplicate mangled strings.
#define DM_GUEST_THREAD_IMPORT(name, symbol) static const char dm_guest_symbol_##name[] = symbol;
#include "guest_thread_imports.generated.inc"
#undef DM_GUEST_THREAD_IMPORT
typedef struct {
#define DM_GUEST_THREAD_SIGNATURE(name, result, parameters) dm_guest_##name##_fn name;
#include "guest_thread_signatures.inc"
#undef DM_GUEST_THREAD_SIGNATURE
    void *(*allocate)(dm_guest_size);
    void (*release)(void *);
} dm_guest_thread_imports;
typedef struct {
#define DM_GUEST_MUTEX_SIGNATURE(name, result, parameters) dm_guest_##name##_fn name;
#include "guest_mutex_signatures.inc"
#undef DM_GUEST_MUTEX_SIGNATURE
} dm_guest_mutex_imports;
#endif
