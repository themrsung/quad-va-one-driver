// Broker: a LaunchDaemon that owns the shared audio ring and hands it to the driver and the app.
//
// The driver runs sandboxed inside Apple's Core Audio driver service and may only reach mach
// services it declares (AudioServerPlugIn_MachServices), and the app can't vend one the system
// domain sees; so neither can hand memory to the other directly. This does it for them. Only the
// driver's host process and our own app may connect, so no one else can read system audio here.
#include "../shared/AADShared.h"
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <os/log.h>
#include <stdio.h>
#include <string.h>
#include <xpc/xpc.h>

#ifndef AAD_TEAM_ID
#error "build with -DAAD_TEAM_ID=\"...\""
#endif

static const char *kPeerRequirement =
    "(anchor apple and (identifier \"com.apple.audio.Core-Audio-Driver-Service\""
    " or identifier \"com.apple.audio.Core-Audio-Driver-Service.helper\""
    " or identifier \"com.apple.audio.coreaudiod\"))"
    " or (anchor apple generic and certificate leaf[subject.OU] = \"" AAD_TEAM_ID "\""
    " and identifier \"me.sjun.AnalogAudio\")";

int main(void) {
    os_log_t log = os_log_create("me.sjun.AnalogAudio", "broker");
    vm_size_t size = round_page(sizeof(AADShared));
    vm_address_t addr = 0;
    if (vm_allocate(mach_task_self(), &addr, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        os_log_error(log, "vm_allocate failed");
        return 1;
    }
    AADShared *shared = (AADShared *)addr;
    memset(shared, 0, size);
    shared->magic = AAD_SHARED_MAGIC;
    shared->version = AAD_SHARED_VERSION;
    shared->frames = AAD_SHARED_FRAMES;
    shared->channels = AAD_SHARED_CHANNELS;

    xpc_connection_t listener = xpc_connection_create_mach_service(AAD_BROKER_SERVICE, NULL, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        xpc_connection_t conn = (xpc_connection_t)peer;
        if (xpc_connection_set_peer_code_signing_requirement(conn, kPeerRequirement) != 0) {
            os_log_error(log, "couldn't set the peer requirement; refusing");
            xpc_connection_cancel(conn);
            return;
        }
        xpc_connection_set_event_handler(conn, ^(xpc_object_t msg) {
            if (xpc_get_type(msg) == XPC_TYPE_ERROR) {
                if (msg == XPC_ERROR_PEER_CODE_SIGNING_REQUIREMENT)
                    os_log_error(log, "refused a peer (pid %d): code signature doesn't match", xpc_connection_get_pid(conn));
                return;
            }
            if (xpc_get_type(msg) != XPC_TYPE_DICTIONARY) return;
            xpc_object_t reply = xpc_dictionary_create_reply(msg);
            if (!reply) return;
            xpc_object_t shm = xpc_shmem_create(shared, size);
            xpc_dictionary_set_value(reply, "shm", shm);
            xpc_connection_send_message(conn, reply);
            xpc_release(shm);
            xpc_release(reply);
            os_log(log, "shared the ring with pid %d", xpc_connection_get_pid(conn));
        });
        xpc_connection_activate(conn);
    });
    xpc_connection_activate(listener);
    os_log(log, "ready");
    dispatch_main();
}
