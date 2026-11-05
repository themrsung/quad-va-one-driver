// Loads a test build of the driver (AAD_TESTING: no broker; the test hands it the ring) with a
// fake host and exercises it outside coreaudiod. Also checks that xpc_shmem shares, not copies.
// ./tests/run.sh builds and runs it.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <xpc/xpc.h>
#include <mach/mach.h>
#include "../shared/AADShared.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) printf("ok    "); else { printf("FAIL  "); failures++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static AudioServerPlugInDriverRef D;
static CFMutableDictionaryRef storage;
static volatile int configChanges = 0;

static OSStatus HostPropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a) {
    (void)h; (void)o; (void)n; (void)a; return 0;
}
static OSStatus HostCopy(AudioServerPlugInHostRef h, CFStringRef key, CFPropertyListRef *out) {
    (void)h; CFPropertyListRef v = CFDictionaryGetValue(storage, key); *out = v ? CFRetain(v) : NULL; return v ? 0 : -1;
}
static OSStatus HostWrite(AudioServerPlugInHostRef h, CFStringRef key, CFPropertyListRef v) {
    (void)h; CFDictionarySetValue(storage, key, v); return 0;
}
static OSStatus HostDelete(AudioServerPlugInHostRef h, CFStringRef key) { (void)h; CFDictionaryRemoveValue(storage, key); return 0; }
static OSStatus HostRequest(AudioServerPlugInHostRef h, AudioObjectID dev, UInt64 action, void *info) {
    (void)h;
    OSStatus e = (*D)->PerformDeviceConfigurationChange(D, dev, action, info);
    configChanges++;
    return e;
}
static AudioServerPlugInHostInterface host = {HostPropertiesChanged, HostCopy, HostWrite, HostDelete, HostRequest};

static OSStatus get(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope sc, void *out, UInt32 size, UInt32 *outSize) {
    AudioObjectPropertyAddress a = {s, sc, kAudioObjectPropertyElementMain};
    UInt32 got = 0;
    OSStatus e = (*D)->GetPropertyData(D, o, 0, &a, 0, NULL, size, &got, out);
    if (outSize) *outSize = got;
    return e;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s path/to/AnalogAudioDevice.driver\n", argv[0]); return 2; }
    storage = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFStringRef path = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, true);
    CFBundleRef bundle = CFBundleCreate(NULL, url);
    CHECK(bundle && CFBundleLoadExecutable(bundle), "bundle loads");
    void *(*factory)(CFAllocatorRef, CFUUIDRef) = CFBundleGetFunctionPointerForName(bundle, CFSTR("AnalogAudioDevice_Create"));
    CHECK(factory != NULL, "factory exported");
    void *unk = factory(NULL, kAudioServerPlugInTypeUUID);
    CHECK(unk != NULL, "factory returns the driver for the plug-in type");
    CHECK(factory(NULL, kAudioServerPlugInDriverInterfaceUUID) == NULL, "factory refuses other types");

    IUnknownVTbl **iunk = unk;
    void *iface = NULL;
    CHECK((*iunk)->QueryInterface(unk, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &iface) == 0 && iface, "QueryInterface");
    D = iface;
    CHECK((*D)->Initialize(D, &host) == 0, "Initialize");

    AudioObjectID devs[4]; UInt32 sz;
    get(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, devs, sizeof(devs), &sz);
    CHECK(sz == sizeof(AudioObjectID) && devs[0] == 2, "one device, id 2");
    AudioObjectID dev = devs[0];

    {
        AudioObjectPropertyAddress a = {kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        CFStringRef uid = CFSTR("AnalogAudioDevice_UID"); AudioObjectID found = 0; UInt32 got = 0;
        (*D)->GetPropertyData(D, kAudioObjectPlugInObject, 0, &a, sizeof(uid), &uid, sizeof(found), &got, &found);
        CHECK(found == dev, "UID translates to the device");
    }

    CFStringRef name = NULL;
    get(dev, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &name, sizeof(name), NULL);
    char buf[128] = {0};
    if (name) CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
    CHECK(name && strcmp(buf, "Analog audio device") == 0, "name is \"%s\"", buf);

    UInt32 v = 9;
    get(dev, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeOutput, &v, 4, NULL);
    CHECK(v == 1, "can be default output");
    get(dev, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeInput, &v, 4, NULL);
    CHECK(v == 0, "can't be default input");

    AudioObjectID streams[4];
    get(dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, streams, sizeof(streams), &sz);
    CHECK(sz == 0, "no input streams (nothing for the microphone indicator)");
    get(dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, streams, sizeof(streams), &sz);
    CHECK(sz == 4 && streams[0] == 4, "output stream 4");
    get(dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeGlobal, streams, sizeof(streams), &sz);
    CHECK(sz == 4, "only the output stream in global scope");

    AudioValueRange rates[10];
    get(dev, kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal, rates, sizeof(rates), &sz);
    CHECK(sz / sizeof(AudioValueRange) == 6, "6 sample rates (44.1–192 kHz)");

    AudioStreamBasicDescription f;
    get(4, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, &f, sizeof(f), NULL);
    CHECK(f.mSampleRate == 48000 && f.mChannelsPerFrame == 2 && (f.mFormatFlags & kAudioFormatFlagIsFloat) && f.mBitsPerChannel == 32,
          "format 48 kHz, 2 ch, float32");

    {
        AudioObjectPropertyAddress a = {'zzzz', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        CHECK(!(*D)->HasProperty(D, dev, 0, &a), "unknown property absent");
        a.mSelector = kAudioDevicePropertyNominalSampleRate;
        Boolean settable = false;
        (*D)->IsPropertySettable(D, dev, 0, &a, &settable);
        CHECK(settable, "sample rate settable");
        Float64 r = 96000;
        CHECK((*D)->SetPropertyData(D, dev, 0, &a, 0, NULL, sizeof(r), &r) == 0, "set 96 kHz");
        for (int i = 0; i < 100 && configChanges == 0; i++) usleep(10000);
        Float64 now = 0;
        get(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &now, sizeof(now), NULL);
        CHECK(now == 96000, "rate now %.0f", now);
        CHECK(CFDictionaryGetValue(storage, CFSTR("sample rate")) != NULL, "rate saved to storage");
        r = 12345;
        CHECK((*D)->SetPropertyData(D, dev, 0, &a, 0, NULL, sizeof(r), &r) != 0, "invalid rate refused");
    }

    // Rate adjust round trip and clamp.
    {
        AudioObjectPropertyAddress a = {'rAdj', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        double adj = 1.0002;
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat64Type, &adj);
        CFPropertyListRef pl = n;
        CHECK((*D)->SetPropertyData(D, dev, 0, &a, 0, NULL, sizeof(pl), &pl) == 0, "set rAdj");
        CFRelease(n);
        CFPropertyListRef back = NULL; UInt32 got;
        (*D)->GetPropertyData(D, dev, 0, &a, 0, NULL, sizeof(back), &got, &back);
        double readback = 0;
        if (back) { CFNumberGetValue(back, kCFNumberFloat64Type, &readback); CFRelease(back); }
        CHECK(readback == 1.0002, "rAdj reads back %.6f", readback);
    }

    // Clock: zero time stamps advance at rate × adjustment.
    {
        CHECK((*D)->StartIO(D, dev, 1) == 0, "StartIO");
        Float64 s0, s1; UInt64 h0, h1, seed;
        (*D)->GetZeroTimeStamp(D, dev, 1, &s0, &h0, &seed);
        usleep(700000); // > 2 periods of 16384 frames at 96 kHz
        (*D)->GetZeroTimeStamp(D, dev, 1, &s1, &h1, &seed);
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        double secs = (double)(h1 - h0) * tb.numer / tb.denom / 1e9;
        double rate = secs > 0 ? (s1 - s0) / secs : 0;
        CHECK(s1 > s0 && s1 - s0 == 16384 * (int)((s1 - s0) / 16384), "zero stamps step by whole periods (%.0f frames)", s1 - s0);
        CHECK(fabs(rate - 96000 * 1.0002) < 0.01, "clock runs at %.3f Hz (want %.3f)", rate, 96000 * 1.0002);
    }

    // The shared ring: WriteMix appends there, in order, bit-exact; a full ring drops, not overwrites.
    {
        AADShared *sh = calloc(1, sizeof(AADShared));
        sh->magic = AAD_SHARED_MAGIC; sh->version = AAD_SHARED_VERSION;
        sh->frames = AAD_SHARED_FRAMES; sh->channels = AAD_SHARED_CHANNELS;
        void (*attach)(AADShared *) = CFBundleGetFunctionPointerForName(bundle, CFSTR("AnalogAudioDevice_TestAttach"));
        CHECK(attach != NULL, "test build exports the attach hook");
        AudioServerPlugInIOCycleInfo cyc = {0};
        enum { N = 512 };
        float out[N * 2];
        for (int i = 0; i < N * 2; i++) out[i] = (float)i / (N * 2) - 0.5f;
        (*D)->DoIOOperation(D, dev, 4, 1, kAudioServerPlugInIOOperationWriteMix, N, &cyc, out, NULL);
        CHECK(atomic_load(&sh->writePos) == 0, "nothing written before the ring is attached");
        attach(sh);
        CHECK(atomic_load(&sh->sampleRate) == 96000, "driver publishes its rate (%u)", atomic_load(&sh->sampleRate));
        // Start near the end so the write wraps around.
        atomic_store(&sh->writePos, AAD_SHARED_FRAMES - 100);
        atomic_store(&sh->readPos, AAD_SHARED_FRAMES - 100);
        (*D)->DoIOOperation(D, dev, 4, 1, kAudioServerPlugInIOOperationWriteMix, N, &cyc, out, NULL);
        CHECK(atomic_load(&sh->writePos) == AAD_SHARED_FRAMES - 100 + N, "write position advances by the frames written");
        int exact = memcmp(&sh->data[(AAD_SHARED_FRAMES - 100) * 2], out, 100 * 2 * sizeof(float)) == 0 &&
                    memcmp(&sh->data[0], out + 200, (N - 100) * 2 * sizeof(float)) == 0;
        CHECK(exact, "bit-exact across the wrap");
        // Fill it: reader stuck.
        uint64_t before = atomic_load(&sh->writePos);
        atomic_store(&sh->readPos, before - AAD_SHARED_FRAMES + 100);
        (*D)->DoIOOperation(D, dev, 4, 1, kAudioServerPlugInIOOperationWriteMix, N, &cyc, out, NULL);
        CHECK(atomic_load(&sh->writePos) == before && atomic_load(&sh->drops) == N, "full ring: dropped, write position held");
        CHECK((*D)->StopIO(D, dev, 1) == 0, "StopIO");
    }

    // xpc_shmem: two mappings of one region must see each other's writes (shared, not copied).
    {
        vm_address_t addr = 0;
        vm_size_t size = round_page(sizeof(AADShared));
        vm_allocate(mach_task_self(), &addr, size, VM_FLAGS_ANYWHERE);
        xpc_object_t shm = xpc_shmem_create((void *)addr, size);
        void *a = NULL, *b = NULL;
        size_t sa = xpc_shmem_map(shm, &a), sb = xpc_shmem_map(shm, &b);
        CHECK(sa >= sizeof(AADShared) && sb >= sizeof(AADShared) && a != b, "mapped twice");
        ((volatile uint32_t *)a)[100] = 0xC0FFEE;
        CHECK(((volatile uint32_t *)b)[100] == 0xC0FFEE && ((volatile uint32_t *)addr)[100] == 0xC0FFEE, "writes show through every mapping");
        xpc_release(shm);
    }

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}
