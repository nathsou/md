// Network services, as on a Switch with no connection: the network interface manager (nifm) says there is none and
// turns every connection request down, so games take their offline paths.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

#define RESULT_NIFM_NOT_CONNECTED MAKE_RESULT(110, 300)

static uint32_t event_handle(bool signaled) {
    uint32_t w, r;
    KEvent *e = kernel_create_event_pair(&w, &r);
    if (signaled) {
        pthread_mutex_lock(&g_klock);
        kernel_signal(e);
        pthread_mutex_unlock(&g_klock);
    }
    return r;
}

// ------------------------------------------------------------------ nifm: IRequest
typedef struct { bool submitted; } NifmRequest;

static void nifm_request(Service *s, IpcReq *req, IpcResp *resp) {
    NifmRequest *r = s->state;
    switch (req->cmd) {
    case 0:  // GetRequestState: 1 not submitted / invalid (never accepted: no network)
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    case 1:  // GetResult
        resp->result = r->submitted ? RESULT_NIFM_NOT_CONNECTED : 0;
        break;
    case 2:  // GetSystemEventReadableHandles: the request's state changed (it has: it was turned down)
        resp->copy_handles[resp->ncopy++] = event_handle(true);
        resp->copy_handles[resp->ncopy++] = event_handle(true);
        break;
    case 4:  // Submit
        r->submitted = true;
        break;
    default:  // Cancel, SetRequirement*, SetConnectionConfirmationOption, ...
        break;
    }
}

// ------------------------------------------------------------------ nifm: IGeneralService
static void nifm_general(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: {  // GetClientId (into the output buffer)
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 4) *(uint32_t *)(uintptr_t)b->addr = 1;
        break;
    }
    case 4:  // CreateRequest
        ipc_return_object(resp, service_new("nifm:IRequest", nifm_request, calloc(1, sizeof(NifmRequest))));
        break;
    case 5:   // GetCurrentNetworkProfile
    case 12:  // GetCurrentIpAddress
    case 15:  // GetCurrentIpConfigInfo
    case 18:  // GetInternetConnectionStatus
        resp->result = RESULT_NIFM_NOT_CONNECTED;
        break;
    case 21: case 22: case 23: case 24:  // Is{Wireless,Ethernet}CommunicationEnabled, IsAny{Internet,Foreground}RequestAccepted
        OUT(uint8_t, 0) = 0;
        resp_size(resp, 1);
        break;
    default:
        LOG("[nifm] IGeneralService cmd %u (ok)\n", req->cmd);
        resp_size(resp, 0x10);
    }
}

static void nifm_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 4 || req->cmd == 5)  // CreateGeneralServiceOld / CreateGeneralService
        ipc_return_object(resp, service_new("nifm:IGeneralService", nifm_general, NULL));
    else LOG("[nifm] cmd %u (ok)\n", req->cmd);
}
SERVICE("nifm:u", nifm_u) { nifm_root(s, req, resp); }
SERVICE("nifm:a", nifm_a) { nifm_root(s, req, resp); }
SERVICE("nifm:s", nifm_s) { nifm_root(s, req, resp); }

// ------------------------------------------------------------------ friends (offline: no friends, nothing to report)
static void friend_notification(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = event_handle(false); break;  // GetEvent
    case 2: resp->result = MAKE_RESULT(121, 304); break;                     // Pop: no notification
    default: break;                                                          // Clear
    }
}

static void friend_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = event_handle(true); break;  // GetCompletionEvent
    case 10100: case 10101: case 10400: case 10500: case 20100: case 20101: case 20102: case 20103: case 20110:
        OUT(uint32_t, 0) = 0;  // the friend / blocked / profile lists and counts: empty
        resp_size(resp, 8);
        break;
    default:  // presence, play history, online play sessions...: accepted
        resp_size(resp, 0x10);
    }
}

static void friend_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("friend:IFriendService", friend_service, NULL));
    else if (req->cmd == 1) ipc_return_object(resp, service_new("friend:INotificationService", friend_notification, NULL));
    else LOG("[friend] cmd %u (ok)\n", req->cmd);
}
SERVICE("friend:u", friend_u) { friend_root(s, req, resp); }
SERVICE("friend:a", friend_a) { friend_root(s, req, resp); }
SERVICE("friend:v", friend_v) { friend_root(s, req, resp); }

// ------------------------------------------------------------------ bcat (news / delivery cache: nothing delivered)
#define RESULT_BCAT_NOT_FOUND MAKE_RESULT(122, 2)

static void bcat_progress(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = event_handle(true); break;  // GetEvent: done at once
    case 1: {  // GetImpl: the progress struct (0x200): status 9 (done), result: the sync failed (no network)
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 0x10) {
            memset((void *)(uintptr_t)b->addr, 0, b->size);
            *(uint32_t *)(uintptr_t)b->addr = 9;
            *(uint32_t *)(uintptr_t)(b->addr + 4) = RESULT_NIFM_NOT_CONNECTED;
        }
        break;
    }
    default: break;
    }
}

static void bcat_storage_child(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->result = RESULT_BCAT_NOT_FOUND; break;  // Open: no such directory / file
    default: OUT(uint64_t, 0) = 0; resp_size(resp, 8); break;  // Read / GetSize / GetCount / GetDigest: nothing
    }
}

static void bcat_storage(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("bcat:IDeliveryCacheFileService", bcat_storage_child, NULL)); break;
    case 1: ipc_return_object(resp, service_new("bcat:IDeliveryCacheDirectoryService", bcat_storage_child, NULL)); break;
    case 10: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // EnumerateDeliveryCacheDirectory: none
    default: break;
    }
}

static void bcat_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 10100: case 10101:  // RequestSyncDeliveryCache(WithDirectoryName)
        ipc_return_object(resp, service_new("bcat:IDeliveryCacheProgressService", bcat_progress, NULL));
        break;
    default: break;
    }
}

static void bcat_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("bcat:IBcatService", bcat_service, NULL));
    else if (req->cmd == 1 || req->cmd == 2)
        ipc_return_object(resp, service_new("bcat:IDeliveryCacheStorageService", bcat_storage, NULL));
    else LOG("[bcat] cmd %u (ok)\n", req->cmd);
}
SERVICE("bcat:u", bcat_u) { bcat_root(s, req, resp); }
SERVICE("bcat:a", bcat_a) { bcat_root(s, req, resp); }
SERVICE("bcat:m", bcat_m) { bcat_root(s, req, resp); }
SERVICE("bcat:s", bcat_s) { bcat_root(s, req, resp); }

// ------------------------------------------------------------------ play reports: accepted, dropped
static void prepo(Service *s, IpcReq *req, IpcResp *resp) { (void)s, (void)req; resp_size(resp, 0x10); }
SERVICE("prepo:u", prepo_u) { prepo(s, req, resp); }
SERVICE("prepo:a", prepo_a) { prepo(s, req, resp); }
SERVICE("prepo:a2", prepo_a2) { prepo(s, req, resp); }
SERVICE("prepo:s", prepo_s) { prepo(s, req, resp); }
