// Analog audio device: a virtual 2-channel output, as an AudioServerPlugIn.
//
// Apps play to it; the driver writes the mix into a ring shared with the app (Analog Audio.app),
// which sends it on to the real output device. The ring comes from the broker daemon over XPC, so
// there's no loopback input stream, and nothing for macOS to show as the microphone in use.
// No DSP here. The one extra is a custom property, 'rAdj', through which the app nudges this
// device's clock to follow the real device's clock, so the audio never needs resampling.
//
// Objects: the plug-in (1), the device (2) and its output stream (4).

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <xpc/xpc.h>
#include "../shared/AADShared.h"

enum {
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Device = 2,
    kObjectID_Stream_Output = 4,
};

#define kDevice_Name CFSTR("Analog audio device")
#define kManufacturer CFSTR("sjun.me")
#define kDevice_UID CFSTR("AnalogAudioDevice_UID")
#define kDevice_ModelUID CFSTR("AnalogAudioDevice_ModelUID")
#define kStorageKey_SampleRate CFSTR("sample rate")

#define kChannels 2
#define kZeroTimeStampPeriod 16384
#define kRateAdjustSelector 'rAdj'
#define kRateAdjustLimit 0.001

static const Float64 kSampleRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
#define kSampleRateCount (sizeof(kSampleRates) / sizeof(kSampleRates[0]))

// MARK: - State

static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER; // properties
static pthread_mutex_t gIOMutex = PTHREAD_MUTEX_INITIALIZER;    // clock
static UInt32 gRefCount = 0;
static AudioServerPlugInHostRef gHost = NULL;

static Float64 gSampleRate = 48000;
static Float64 gRateAdjust = 1.0; // >1: this device's clock runs faster than nominal
static UInt32 gIORunning = 0;
static Float64 gHostTicksPerSecond = 0;
static Float64 gZeroHostTime = 0;
static Float64 gZeroSampleTime = 0;

// The ring shared with the app, once the broker has handed it over. Never unmapped: if the
// broker restarts, the new region replaces it and the old one stays mapped (it's small, and the
// IO thread may still be writing to it).
static _Atomic(AADShared *) gShared = NULL;
static xpc_connection_t gBroker = NULL;
static dispatch_queue_t gBrokerQueue = NULL;

static void PublishSampleRate(void) {
    AADShared *s = atomic_load(&gShared);
    if (s) atomic_store(&s->sampleRate, (uint32_t)gSampleRate);
}

static void ConnectBroker(void);

static void RequestRing(void) {
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "op", "ring");
    xpc_connection_send_message_with_reply(gBroker, msg, gBrokerQueue, ^(xpc_object_t reply) {
        if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY) return; // errors arrive at the event handler
        xpc_object_t shm = xpc_dictionary_get_value(reply, "shm");
        void *region = NULL;
        size_t size = shm ? xpc_shmem_map(shm, &region) : 0;
        AADShared *s = region;
        if (size < sizeof(AADShared) || s->magic != AAD_SHARED_MAGIC || s->version != AAD_SHARED_VERSION ||
            s->frames != AAD_SHARED_FRAMES || s->channels != AAD_SHARED_CHANNELS)
            return;
        atomic_store(&gShared, s);
        PublishSampleRate();
    });
    xpc_release(msg);
}

// Connects to the broker and asks for the ring; reconnects whenever the broker goes away.
static void ConnectBroker(void) {
    gBroker = xpc_connection_create_mach_service(AAD_BROKER_SERVICE, gBrokerQueue, 0);
    xpc_connection_set_event_handler(gBroker, ^(xpc_object_t event) {
        if (event == XPC_ERROR_CONNECTION_INTERRUPTED) {
            RequestRing(); // the broker restarted: same name, new region
        } else if (event == XPC_ERROR_CONNECTION_INVALID) {
            xpc_release(gBroker); // not running or not installed: try again later
            gBroker = NULL;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), gBrokerQueue, ^{ ConnectBroker(); });
        }
    });
    xpc_connection_activate(gBroker);
    RequestRing();
}

#ifdef AAD_TESTING
__attribute__((visibility("default"))) void AnalogAudioDevice_TestAttach(AADShared *s) {
    atomic_store(&gShared, s);
    PublishSampleRate();
}
#endif

static bool IsValidRate(Float64 rate) {
    for (UInt32 i = 0; i < kSampleRateCount; i++)
        if (kSampleRates[i] == rate) return true;
    return false;
}

static AudioStreamBasicDescription Format(Float64 rate) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * kChannels;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

// MARK: - Interface declarations

static HRESULT Driver_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG Driver_AddRef(void *inDriver);
static ULONG Driver_Release(void *inDriver);
static OSStatus Driver_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus Driver_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
static OSStatus Driver_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus Driver_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus Driver_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus Driver_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static OSStatus Driver_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static Boolean Driver_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
static OSStatus Driver_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
static OSStatus Driver_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static OSStatus Driver_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, UInt32 *, void *);
static OSStatus Driver_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);
static OSStatus Driver_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus Driver_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus Driver_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
static OSStatus Driver_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
static OSStatus Driver_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
static OSStatus Driver_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *, void *, void *);
static OSStatus Driver_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    Driver_QueryInterface,
    Driver_AddRef,
    Driver_Release,
    Driver_Initialize,
    Driver_CreateDevice,
    Driver_DestroyDevice,
    Driver_AddDeviceClient,
    Driver_RemoveDeviceClient,
    Driver_PerformDeviceConfigurationChange,
    Driver_AbortDeviceConfigurationChange,
    Driver_HasProperty,
    Driver_IsPropertySettable,
    Driver_GetPropertyDataSize,
    Driver_GetPropertyData,
    Driver_SetPropertyData,
    Driver_StartIO,
    Driver_StopIO,
    Driver_GetZeroTimeStamp,
    Driver_WillDoIOOperation,
    Driver_BeginIOOperation,
    Driver_DoIOOperation,
    Driver_EndIOOperation,
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

// MARK: - Factory and COM

__attribute__((visibility("default")))
void *AnalogAudioDevice_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    (void)inAllocator;
    return CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID) ? gDriverRef : NULL;
}

static HRESULT Driver_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (outInterface == NULL) return kAudioHardwareIllegalOperationError;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gStateMutex);
        gRefCount++;
        pthread_mutex_unlock(&gStateMutex);
        *outInterface = gDriverRef;
        result = S_OK;
    }
    CFRelease(requested);
    return result;
}

static ULONG Driver_AddRef(void *inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    ULONG count = ++gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return count;
}

static ULONG Driver_Release(void *inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount > 0) gRefCount--;
    ULONG count = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return count;
}

// MARK: - Lifecycle

static OSStatus Driver_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    gHostTicksPerSecond = 1e9 * (Float64)tb.denom / (Float64)tb.numer;

    // Restore the last sample rate.
    CFPropertyListRef saved = NULL;
    if (gHost && gHost->CopyFromStorage && gHost->CopyFromStorage(gHost, kStorageKey_SampleRate, &saved) == 0 && saved) {
        Float64 rate = 0;
        if (CFGetTypeID(saved) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)saved, kCFNumberFloat64Type, &rate) && IsValidRate(rate))
            gSampleRate = rate;
        CFRelease(saved);
    }
#ifndef AAD_TESTING
    gBrokerQueue = dispatch_queue_create("me.sjun.AnalogAudioDevice.broker", DISPATCH_QUEUE_SERIAL);
    dispatch_async(gBrokerQueue, ^{ ConnectBroker(); });
#endif
    return 0;
}

static OSStatus Driver_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *out) {
    (void)d; (void)desc; (void)ci; (void)out;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus Driver_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev) {
    (void)d; (void)dev;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus Driver_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci) {
    (void)ci;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}

static OSStatus Driver_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci) {
    (void)ci;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}

// Sample rate changes go through the host: request, then the host calls back here once IO is
// quiesced. The change action is the new rate.
static OSStatus Driver_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info) {
    (void)info;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    Float64 rate = (Float64)action;
    if (!IsValidRate(rate)) return kAudioHardwareIllegalOperationError;

    pthread_mutex_lock(&gStateMutex);
    pthread_mutex_lock(&gIOMutex);
    gSampleRate = rate;
    gZeroHostTime = (Float64)mach_absolute_time();
    gZeroSampleTime = 0;
    pthread_mutex_unlock(&gIOMutex);
    pthread_mutex_unlock(&gStateMutex);
    PublishSampleRate();

    if (gHost && gHost->WriteToStorage) {
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat64Type, &rate);
        gHost->WriteToStorage(gHost, kStorageKey_SampleRate, n);
        CFRelease(n);
    }
    return 0;
}

static OSStatus Driver_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info) {
    (void)action; (void)info;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}

static void RequestSampleRate(Float64 rate) {
    if (!gHost) return;
    // Called off the property-setting thread, as the host requires.
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, (UInt64)rate, NULL);
    });
}

// MARK: - Properties
//
// One routine answers every property: with outData NULL it only reports the size, which gives
// HasProperty and GetPropertyDataSize for free.

#define RETURN_VALUE(type, value)                                                    \
    do {                                                                             \
        if (outData) {                                                               \
            if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; \
            *(type *)outData = (value);                                              \
        }                                                                            \
        *outDataSize = sizeof(type);                                                 \
        return 0;                                                                    \
    } while (0)

static OSStatus ReturnIDs(const AudioObjectID *ids, UInt32 count, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    if (outData) {
        UInt32 n = inDataSize / sizeof(AudioObjectID);
        if (n > count) n = count;
        memcpy(outData, ids, n * sizeof(AudioObjectID));
        *outDataSize = n * sizeof(AudioObjectID);
    } else {
        *outDataSize = count * sizeof(AudioObjectID);
    }
    return 0;
}

static OSStatus ReturnBytes(const void *bytes, UInt32 size, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    if (outData) {
        if (inDataSize < size) return kAudioHardwareBadPropertySizeError;
        memcpy(outData, bytes, size);
    }
    *outDataSize = size;
    return 0;
}

static OSStatus PlugInProperty(const AudioObjectPropertyAddress *a, UInt32 qualSize, const void *qual,
                               UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    static const AudioObjectID devices[] = {kObjectID_Device};
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: RETURN_VALUE(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass: RETURN_VALUE(AudioClassID, kAudioPlugInClassID);
    case kAudioObjectPropertyOwner: RETURN_VALUE(AudioObjectID, kAudioObjectUnknown);
    case kAudioObjectPropertyManufacturer: RETURN_VALUE(CFStringRef, kManufacturer);
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList: return ReturnIDs(devices, 1, inDataSize, outDataSize, outData);
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        AudioObjectID found = kAudioObjectUnknown;
        if (outData) {
            if (qualSize != sizeof(CFStringRef) || qual == NULL) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *(const CFStringRef *)qual;
            if (uid && CFStringCompare(uid, kDevice_UID, 0) == kCFCompareEqualTo) found = kObjectID_Device;
        }
        RETURN_VALUE(AudioObjectID, found);
    }
    case kAudioPlugInPropertyResourceBundle: RETURN_VALUE(CFStringRef, CFSTR(""));
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus DeviceProperty(const AudioObjectPropertyAddress *a, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    static const AudioObjectID output[] = {kObjectID_Stream_Output};

    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: RETURN_VALUE(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass: RETURN_VALUE(AudioClassID, kAudioDeviceClassID);
    case kAudioObjectPropertyOwner: RETURN_VALUE(AudioObjectID, kObjectID_PlugIn);
    case kAudioObjectPropertyName: RETURN_VALUE(CFStringRef, kDevice_Name);
    case kAudioObjectPropertyManufacturer: RETURN_VALUE(CFStringRef, kManufacturer);
    case kAudioDevicePropertyDeviceUID: RETURN_VALUE(CFStringRef, kDevice_UID);
    case kAudioDevicePropertyModelUID: RETURN_VALUE(CFStringRef, kDevice_ModelUID);
    case kAudioDevicePropertyTransportType: RETURN_VALUE(UInt32, kAudioDeviceTransportTypeVirtual);
    case kAudioDevicePropertyClockDomain: RETURN_VALUE(UInt32, 0);
    case kAudioDevicePropertyDeviceIsAlive: RETURN_VALUE(UInt32, 1);
    case kAudioDevicePropertyIsHidden: RETURN_VALUE(UInt32, 0);
    case kAudioDevicePropertyLatency: RETURN_VALUE(UInt32, 0);
    case kAudioDevicePropertySafetyOffset: RETURN_VALUE(UInt32, 0);
    case kAudioDevicePropertyZeroTimeStampPeriod: RETURN_VALUE(UInt32, kZeroTimeStampPeriod);
    case kAudioDevicePropertyDeviceIsRunning: {
        pthread_mutex_lock(&gStateMutex);
        UInt32 running = gIORunning > 0;
        pthread_mutex_unlock(&gStateMutex);
        RETURN_VALUE(UInt32, running);
    }
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        RETURN_VALUE(UInt32, a->mScope == kAudioObjectPropertyScopeInput ? 0 : 1);
    case kAudioDevicePropertyRelatedDevices: {
        static const AudioObjectID self[] = {kObjectID_Device};
        return ReturnIDs(self, 1, inDataSize, outDataSize, outData);
    }
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams:
        if (a->mScope == kAudioObjectPropertyScopeInput) return ReturnIDs(NULL, 0, inDataSize, outDataSize, outData);
        return ReturnIDs(output, 1, inDataSize, outDataSize, outData);
    case kAudioObjectPropertyControlList: return ReturnIDs(NULL, 0, inDataSize, outDataSize, outData);
    case kAudioDevicePropertyNominalSampleRate: {
        pthread_mutex_lock(&gStateMutex);
        Float64 rate = gSampleRate;
        pthread_mutex_unlock(&gStateMutex);
        RETURN_VALUE(Float64, rate);
    }
    case kAudioDevicePropertyAvailableNominalSampleRates: {
        AudioValueRange ranges[kSampleRateCount];
        for (UInt32 i = 0; i < kSampleRateCount; i++) ranges[i] = (AudioValueRange){kSampleRates[i], kSampleRates[i]};
        UInt32 size = sizeof(ranges);
        if (outData) {
            UInt32 n = inDataSize / sizeof(AudioValueRange);
            if (n > kSampleRateCount) n = kSampleRateCount;
            memcpy(outData, ranges, n * sizeof(AudioValueRange));
            size = n * sizeof(AudioValueRange);
        }
        *outDataSize = size;
        return 0;
    }
    case kAudioDevicePropertyPreferredChannelsForStereo: {
        static const UInt32 pair[2] = {1, 2};
        return ReturnBytes(pair, sizeof(pair), inDataSize, outDataSize, outData);
    }
    case kAudioDevicePropertyPreferredChannelLayout: {
        UInt32 size = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription));
        if (outData) {
            if (inDataSize < size) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *layout = outData;
            memset(layout, 0, size);
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            layout->mNumberChannelDescriptions = kChannels;
            layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            layout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
        }
        *outDataSize = size;
        return 0;
    }
    case kAudioObjectPropertyCustomPropertyInfoList: {
        static const AudioServerPlugInCustomPropertyInfo info[] = {
            {kRateAdjustSelector, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone},
        };
        return ReturnBytes(info, sizeof(info), inDataSize, outDataSize, outData);
    }
    case kRateAdjustSelector: {
        if (outData) {
            if (inDataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
            pthread_mutex_lock(&gIOMutex);
            Float64 adj = gRateAdjust;
            pthread_mutex_unlock(&gIOMutex);
            *(CFPropertyListRef *)outData = CFNumberCreate(NULL, kCFNumberFloat64Type, &adj);
        }
        *outDataSize = sizeof(CFPropertyListRef);
        return 0;
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus StreamProperty(const AudioObjectPropertyAddress *a, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: RETURN_VALUE(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass: RETURN_VALUE(AudioClassID, kAudioStreamClassID);
    case kAudioObjectPropertyOwner: RETURN_VALUE(AudioObjectID, kObjectID_Device);
    case kAudioObjectPropertyOwnedObjects: return ReturnIDs(NULL, 0, inDataSize, outDataSize, outData);
    case kAudioStreamPropertyIsActive: RETURN_VALUE(UInt32, 1);
    case kAudioStreamPropertyDirection: RETURN_VALUE(UInt32, 0);
    case kAudioStreamPropertyTerminalType: RETURN_VALUE(UInt32, kAudioStreamTerminalTypeLine);
    case kAudioStreamPropertyStartingChannel: RETURN_VALUE(UInt32, 1);
    case kAudioStreamPropertyLatency: RETURN_VALUE(UInt32, 0);
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        pthread_mutex_lock(&gStateMutex);
        AudioStreamBasicDescription f = Format(gSampleRate);
        pthread_mutex_unlock(&gStateMutex);
        RETURN_VALUE(AudioStreamBasicDescription, f);
    }
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        UInt32 size = kSampleRateCount * sizeof(AudioStreamRangedDescription);
        if (outData) {
            UInt32 n = inDataSize / sizeof(AudioStreamRangedDescription);
            if (n > kSampleRateCount) n = kSampleRateCount;
            AudioStreamRangedDescription *out = outData;
            for (UInt32 i = 0; i < n; i++) {
                out[i].mFormat = Format(kSampleRates[i]);
                out[i].mSampleRateRange = (AudioValueRange){kSampleRates[i], kSampleRates[i]};
            }
            size = n * sizeof(AudioStreamRangedDescription);
        }
        *outDataSize = size;
        return 0;
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus Property(AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 qualSize, const void *qual,
                         UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    switch (obj) {
    case kObjectID_PlugIn: return PlugInProperty(a, qualSize, qual, inDataSize, outDataSize, outData);
    case kObjectID_Device: return DeviceProperty(a, inDataSize, outDataSize, outData);
    case kObjectID_Stream_Output: return StreamProperty(a, inDataSize, outDataSize, outData);
    default: return kAudioHardwareBadObjectError;
    }
}

static Boolean Driver_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a) {
    (void)pid;
    if (d != gDriverRef || a == NULL) return false;
    UInt32 size = 0;
    return Property(obj, a, 0, NULL, 0, &size, NULL) == 0;
}

static OSStatus Driver_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *outSettable) {
    (void)pid;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || outSettable == NULL) return kAudioHardwareIllegalOperationError;
    UInt32 size = 0;
    OSStatus err = Property(obj, a, 0, NULL, 0, &size, NULL);
    if (err) return err;
    switch (obj) {
    case kObjectID_Device:
        *outSettable = a->mSelector == kAudioDevicePropertyNominalSampleRate || a->mSelector == kRateAdjustSelector;
        break;
    case kObjectID_Stream_Output:
        *outSettable = a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat;
        break;
    default: *outSettable = false;
    }
    return 0;
}

static OSStatus Driver_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                           UInt32 qualSize, const void *qual, UInt32 *outDataSize) {
    (void)pid;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || outDataSize == NULL) return kAudioHardwareIllegalOperationError;
    return Property(obj, a, qualSize, qual, 0, outDataSize, NULL);
}

static OSStatus Driver_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                       UInt32 qualSize, const void *qual, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    (void)pid;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || outDataSize == NULL || outData == NULL) return kAudioHardwareIllegalOperationError;
    return Property(obj, a, qualSize, qual, inDataSize, outDataSize, outData);
}

static OSStatus Driver_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                       UInt32 qualSize, const void *qual, UInt32 inDataSize, const void *inData) {
    (void)pid; (void)qualSize; (void)qual;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || inData == NULL) return kAudioHardwareIllegalOperationError;

    if (obj == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        Float64 rate = *(const Float64 *)inData;
        if (!IsValidRate(rate)) return kAudioHardwareIllegalOperationError;
        pthread_mutex_lock(&gStateMutex);
        bool change = rate != gSampleRate;
        pthread_mutex_unlock(&gStateMutex);
        if (change) RequestSampleRate(rate);
        return 0;
    }
    if (obj == kObjectID_Device && a->mSelector == kRateAdjustSelector) {
        if (inDataSize != sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
        CFPropertyListRef value = *(const CFPropertyListRef *)inData;
        Float64 adj = 1;
        if (value == NULL || CFGetTypeID(value) != CFNumberGetTypeID() || !CFNumberGetValue((CFNumberRef)value, kCFNumberFloat64Type, &adj))
            return kAudioHardwareIllegalOperationError;
        if (adj > 1 + kRateAdjustLimit) adj = 1 + kRateAdjustLimit;
        if (adj < 1 - kRateAdjustLimit) adj = 1 - kRateAdjustLimit;
        pthread_mutex_lock(&gIOMutex);
        gRateAdjust = adj;
        pthread_mutex_unlock(&gIOMutex);
        return 0;
    }
    if (obj == kObjectID_Stream_Output &&
        (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *f = inData;
        AudioStreamBasicDescription want = Format(f->mSampleRate);
        if (!IsValidRate(f->mSampleRate) || f->mFormatID != want.mFormatID || f->mChannelsPerFrame != want.mChannelsPerFrame ||
            f->mBitsPerChannel != want.mBitsPerChannel || !(f->mFormatFlags & kAudioFormatFlagIsFloat))
            return kAudioDeviceUnsupportedFormatError;
        pthread_mutex_lock(&gStateMutex);
        bool change = f->mSampleRate != gSampleRate;
        pthread_mutex_unlock(&gStateMutex);
        if (change) RequestSampleRate(f->mSampleRate);
        return 0;
    }
    UInt32 size = 0;
    OSStatus err = Property(obj, a, 0, NULL, 0, &size, NULL);
    return err ? err : kAudioHardwareUnsupportedOperationError;
}

// MARK: - IO

static OSStatus Driver_StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (gIORunning == 0) {
        pthread_mutex_lock(&gIOMutex);
        gZeroHostTime = (Float64)mach_absolute_time();
        gZeroSampleTime = 0;
        pthread_mutex_unlock(&gIOMutex);
    }
    gIORunning++;
    pthread_mutex_unlock(&gStateMutex);
    return 0;
}

static OSStatus Driver_StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (gIORunning > 0) gIORunning--;
    pthread_mutex_unlock(&gStateMutex);
    return 0;
}

// The clock: one zero time stamp every kZeroTimeStampPeriod frames, spaced by the nominal rate times the
// helper's adjustment. A change of adjustment takes effect from the next period on, so the
// timeline never jumps.
static OSStatus Driver_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                        Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed) {
    (void)client;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    Float64 now = (Float64)mach_absolute_time();
    Float64 ticksPerPeriod = gHostTicksPerSecond * kZeroTimeStampPeriod / (gSampleRate * gRateAdjust);
    while (gZeroHostTime + ticksPerPeriod <= now) {
        gZeroHostTime += ticksPerPeriod;
        gZeroSampleTime += kZeroTimeStampPeriod;
    }
    *outSampleTime = gZeroSampleTime;
    *outHostTime = (UInt64)gZeroHostTime;
    *outSeed = 1;
    pthread_mutex_unlock(&gIOMutex);
    return 0;
}

static OSStatus Driver_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                         Boolean *outWillDo, Boolean *outInPlace) {
    (void)client;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    bool will = op == kAudioServerPlugInIOOperationWriteMix;
    if (outWillDo) *outWillDo = will;
    if (outInPlace) *outInPlace = true;
    return 0;
}

static OSStatus Driver_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 frames,
                                        const AudioServerPlugInIOCycleInfo *info) {
    (void)client; (void)op; (void)frames; (void)info;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}

static OSStatus Driver_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client, UInt32 op,
                                     UInt32 frames, const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary) {
    (void)client; (void)stream; (void)secondary;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    (void)info;
    const Float32 *buf = main;
    AADShared *sh = atomic_load_explicit(&gShared, memory_order_acquire);
    if (op != kAudioServerPlugInIOOperationWriteMix || buf == NULL || sh == NULL) return 0;

    // Append the mix to the shared ring. If the app isn't draining it (not running), drop: the
    // app skips to the newest audio when it starts.
    uint64_t w = atomic_load_explicit(&sh->writePos, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&sh->readPos, memory_order_acquire);
    if (w - r + frames > AAD_SHARED_FRAMES) {
        atomic_fetch_add_explicit(&sh->drops, frames, memory_order_relaxed);
        return 0;
    }
    uint32_t start = (uint32_t)(w & (AAD_SHARED_FRAMES - 1));
    uint32_t first = frames < AAD_SHARED_FRAMES - start ? frames : AAD_SHARED_FRAMES - start;
    memcpy(&sh->data[start * kChannels], buf, first * kChannels * sizeof(Float32));
    if (frames > first) memcpy(&sh->data[0], buf + first * kChannels, (frames - first) * kChannels * sizeof(Float32));
    atomic_store_explicit(&sh->writePos, w + frames, memory_order_release);
    return 0;
}

static OSStatus Driver_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 frames,
                                      const AudioServerPlugInIOCycleInfo *info) {
    (void)client; (void)op; (void)frames; (void)info;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}
