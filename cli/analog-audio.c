// analog-audio: media control from a shell, ssh included.
//
//   analog-audio play | resume    start or resume whatever app has Now Playing
//   analog-audio pause | stop     pause it
//
// Goes through MediaRemote (what the media keys and Control Center use), which has separate play
// and pause commands, so both are idempotent rather than a toggle.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

enum { kMRPlay = 0, kMRPause = 1 };
typedef Boolean (*MRSendCommandFn)(int command, CFDictionaryRef options);

static int Media(int command) {
    void *mr = dlopen("/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote", RTLD_LAZY);
    MRSendCommandFn send = mr ? (MRSendCommandFn)dlsym(mr, "MRMediaRemoteSendCommand") : NULL;
    if (!send) { fprintf(stderr, "analog-audio: MediaRemote unavailable\n"); return 1; }
    if (!send(command, NULL)) { fprintf(stderr, "analog-audio: no app took the command\n"); return 1; }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false); // let the request go out before exiting
    return 0;
}

int main(int argc, char **argv) {
    const char *c = argc == 2 ? argv[1] : "";
    if (!strcmp(c, "play") || !strcmp(c, "resume")) return Media(kMRPlay);
    if (!strcmp(c, "pause") || !strcmp(c, "stop")) return Media(kMRPause);
    fprintf(stderr, "usage: analog-audio play|resume|pause|stop\n");
    return 2;
}
