// Compatibility adapters for SDK HIPC client symbols. The same bounded SM,
// CSRNG and existing olsc:u backend also answers native SVC requests. Retaining
// these adapters is intentional until original-SDK call closure is verified.
// HIPC/CMIF protocol references: libnx sf/{hipc,cmif}.h and services/{sm,csrng}.c.
#include "hle.h"
#include "hle_ipc.h"

// These compatibility entry points stay installed until native SDK closure is
// verified. They now share one session owner with the raw SVC path; no handles
// or service-specific response policies are manufactured in this adapter.
HLE_FORCE("_ZN2nn2sf4hipc31InitializeHipcServiceResolutionEv", "service resolution through the shared host IPC backend", Result, h_sf_InitRes, void) { return RES_OK; }
HLE_FORCE("_ZN2nn2sf4hipc29FinalizeHipcServiceResolutionEv", "service resolution compatibility adapter", void, h_sf_FinRes, void) {}
HLE_FORCE("_ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc", "connect through the shared host IPC session owner", Result, h_sf_Connect,
          uint32_t *out, const char *name) {
    dm_ipc_host_csrng_probe_route(DM_IPC_PROBE_ADAPTER_CONNECT);
    if (!out || !name) return DM_IPC_INVALID_MEMORY;
    uint8_t service[8] = {0};
    for (unsigned i = 0; i < sizeof service && name[i]; ++i) service[i] = (uint8_t)name[i];
    dm_ipc *ipc = dm_ipc_host();
    return ipc ? dm_ipc_connect_service(ipc, service, out) : DM_IPC_OUT_OF_HANDLES;
}
HLE_FORCE("_ZN2nn2sf4hipc24CloseClientSessionHandleENS_3svc6HandleE", "close a shared host IPC session", void, h_sf_Close, uint32_t h) {
    uint32_t result = dm_ipc_close(dm_ipc_host(), h);
    if (result) LOG("[RED] sf: CloseClientSessionHandle on %#x failed %#x", h, result);
}
HLE_FORCE("_ZN2nn2sf4hipc6client22QueryPointerBufferSizeEPtNS_3svc6HandleE", "shared host IPC pointer-buffer size", Result, h_sf_PtrBuf, uint16_t *out, uint32_t h) {
    return dm_ipc_pointer_size(dm_ipc_host(), h, out);
}
HLE_FORCE("_ZN2nn2sf4hipc6client22ConvertSessionToDomainEPNS0_4cmif18CmifDomainObjectIdENS_3svc6HandleE", "shared host IPC domain conversion", Result, h_sf_ToDomain,
          uint32_t *out, uint32_t h) {
    return dm_ipc_convert_domain(dm_ipc_host(), h, out);
}
HLE_FORCE("_ZN2nn2sf4hipc6client16DuplicateSessionEPNS_3svc6HandleES4_", "duplicate a shared host IPC session", Result, h_sf_Dup, uint32_t *out, uint32_t h) {
    return dm_ipc_clone(dm_ipc_host(), h, out);
}
HLE_FORCE("_ZN2nn2sf4hipc6client18DuplicateSessionExEPNS_3svc6HandleES4_j", "duplicate a shared host IPC session (tag does not affect identity)", Result, h_sf_DupEx,
          uint32_t *out, uint32_t h, unsigned tag) { (void)tag; return h_sf_Dup(out, h); }
HLE_FORCE("_ZN2nn2sf4hipc15SendSyncRequestENS_3svc6HandleEPvm", "dispatch through the checked shared host IPC backend", Result, h_sf_Send,
          uint32_t h, uint32_t *m, uint64_t size) {
    dm_ipc_host_csrng_probe_route(DM_IPC_PROBE_ADAPTER_SEND);
    if (size > SIZE_MAX) return DM_IPC_INVALID_SIZE;
    uint32_t result = dm_ipc_host_send(h, m, (size_t)size);
    static int trace = -1; if (trace < 0) trace = getenv("DM_SF_TRACE") && atoi(getenv("DM_SF_TRACE"));
    if (trace || result) LOG("sf: SendSyncRequest %#x capacity %#llx result %#x", h, (unsigned long long)size, result);
    return result;
}
