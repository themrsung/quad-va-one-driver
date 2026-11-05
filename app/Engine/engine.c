#include "engine.h"
#include <dispatch/dispatch.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <xpc/xpc.h>

#define CHUNK_FRAMES 4096
#define PULSE_HZ 22000.0
#define PULSE_DBFS (-80.0)
#define PULSE_SECONDS 1.0
#define PULSE_INTERVAL_SECONDS 60.0
#define PULSE_FADE_SECONDS 0.010
#define CROSSFADE_SECONDS 0.050
#define RATE_ADJUST_SELECTOR 'rAdj'
#define RATE_ADJUST_LIMIT 0.0005
#define P_SECONDS 20.0  // proportional time constant of the clock steering
#define I_SECONDS 300.0 // integral time constant

static struct {
    // Set up on the main thread while stopped.
    AudioObjectID inDev, outDev;
    AudioDeviceIOProcID outProc;
    int running;
    double sr;
    uint32_t target; // average fill to hold, in frames
    UInt32 lBuf, lCh, rBuf, rCh;

    // The ring shared with the driver (producer); we're the consumer, from the output IO thread.
    AADShared *shared;

    // Output IO thread only.
    int primed;
    float dry[CHUNK_FRAMES * 2];
    float wet[CHUNK_FRAMES * 2];
    AADDsp dsp;
    float mix; // 0 = dry, 1 = wet
    AADPreset rtPreset;
    int rtComfort;
    uint32_t rtSeq;
    uint64_t silentFrames;
    int64_t pulsePos; // −1: idle
    uint64_t pulseLen, fadeLen, intervalFrames;
    double phase, phaseInc, pulseAmp;

    // Main thread → output thread, through a sequence lock.
    _Atomic uint32_t seq;
    AADPreset preset;
    int comfort;
    _Atomic int pulseEnabled;

    // Output thread → main thread.
    _Atomic uint64_t fillSum, fillCount, underruns;
    _Atomic int pulsing;

    // Main thread.
    double integral, rateAdjust, lastFill;
} E = {.pulsePos = -1, .rateAdjust = 1, .pulseEnabled = 1};

// MARK: - Helpers

static UInt32 GetU32(AudioObjectID dev, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, UInt32 fallback) {
    AudioObjectPropertyAddress a = {sel, scope, kAudioObjectPropertyElementMain};
    UInt32 v = 0, size = sizeof(v);
    return AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &v) == noErr ? v : fallback;
}

static void SetRateAdjust(double adj) {
    if (!E.inDev) return;
    AudioObjectPropertyAddress a = {RATE_ADJUST_SELECTOR, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat64Type, &adj);
    CFPropertyListRef v = n;
    if (AudioObjectSetPropertyData(E.inDev, &a, 0, NULL, sizeof(v), &v) == noErr) E.rateAdjust = adj;
    CFRelease(n);
}

// Turns off one direction of an IOProc: playing to an output that also has an input (a USB mic,
// say) mustn't open that input, or macOS would show the microphone as in use.
static void DisableStreams(AudioObjectID dev, AudioDeviceIOProcID proc, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyIOProcStreamUsage, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr || size < sizeof(AudioHardwareIOProcStreamUsage)) return;
    AudioHardwareIOProcStreamUsage *u = calloc(1, size);
    u->mIOProc = (void *)proc;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, u) == noErr) {
        for (UInt32 i = 0; i < u->mNumberStreams; i++) u->mStreamIsOn[i] = 0;
        AudioObjectSetPropertyData(dev, &a, 0, NULL, size, u);
    }
    free(u);
}

// Finds which buffer and channel of the output's IO buffer list carry its stereo pair.
static OSStatus MapStereo(AudioObjectID dev) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size);
    if (err) return err;
    AudioBufferList *list = malloc(size);
    err = AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, list);
    if (err) { free(list); return err; }

    UInt32 pair[2] = {1, 2};
    AudioObjectPropertyAddress pa = {kAudioDevicePropertyPreferredChannelsForStereo, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 psize = sizeof(pair);
    if (AudioObjectGetPropertyData(dev, &pa, 0, NULL, &psize, pair) != noErr) { pair[0] = 1; pair[1] = 2; }

    UInt32 total = 0;
    for (UInt32 b = 0; b < list->mNumberBuffers; b++) total += list->mBuffers[b].mNumberChannels;
    if (total == 0) { free(list); return kAudioDeviceUnsupportedFormatError; }
    if (pair[0] < 1 || pair[0] > total) pair[0] = 1;
    if (pair[1] < 1 || pair[1] > total) pair[1] = total >= 2 ? 2 : 1;

    UInt32 base = 0;
    for (UInt32 b = 0; b < list->mNumberBuffers; b++) {
        UInt32 n = list->mBuffers[b].mNumberChannels;
        if (pair[0] > base && pair[0] <= base + n) { E.lBuf = b; E.lCh = pair[0] - base - 1; }
        if (pair[1] > base && pair[1] <= base + n) { E.rBuf = b; E.rCh = pair[1] - base - 1; }
        base += n;
    }
    free(list);
    return noErr;
}

static OSStatus CheckFloatOutput(AudioObjectID dev) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr || size == 0) return kAudioDeviceUnsupportedFormatError;
    AudioStreamID streams[32];
    if (size > sizeof(streams)) size = sizeof(streams);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, streams) != noErr) return kAudioDeviceUnsupportedFormatError;
    for (UInt32 i = 0; i < size / sizeof(AudioStreamID); i++) {
        AudioObjectPropertyAddress fa = {kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioStreamBasicDescription f;
        UInt32 fsize = sizeof(f);
        if (AudioObjectGetPropertyData(streams[i], &fa, 0, NULL, &fsize, &f) != noErr) return kAudioDeviceUnsupportedFormatError;
        if (!(f.mFormatFlags & kAudioFormatFlagIsFloat) || f.mBitsPerChannel != 32) return kAudioDeviceUnsupportedFormatError;
    }
    return noErr;
}

// MARK: - IO

static void RefreshParams(void) {
    uint32_t s1 = atomic_load_explicit(&E.seq, memory_order_acquire);
    if (s1 == E.rtSeq || (s1 & 1)) return;
    AADPreset p = E.preset;
    int en = E.comfort;
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&E.seq, memory_order_relaxed) != s1) return;
    E.rtPreset = p;
    E.rtComfort = en;
    E.rtSeq = s1;
}

// Fills E.dry with n frames from the shared ring, or silence. Returns whether it got audio.
static int ReadRing(uint32_t n) {
    AADShared *sh = E.shared;
    uint64_t r = atomic_load_explicit(&sh->readPos, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&sh->writePos, memory_order_acquire);
    uint64_t avail = w - r;
    if (!E.primed && avail >= E.target + n) E.primed = 1;
    if (E.primed && avail < n) {
        E.primed = 0;
        atomic_fetch_add_explicit(&E.underruns, 1, memory_order_relaxed);
    }
    if (!E.primed) {
        memset(E.dry, 0, n * 2 * sizeof(float));
        return 0;
    }
    if (avail > AAD_SHARED_FRAMES) { // we fell a whole lap behind (or the ring was reset): resync
        atomic_store_explicit(&sh->readPos, w, memory_order_release);
        E.primed = 0;
        memset(E.dry, 0, n * 2 * sizeof(float));
        return 0;
    }
    uint32_t start = (uint32_t)(r & (AAD_SHARED_FRAMES - 1));
    uint32_t first = n < AAD_SHARED_FRAMES - start ? n : AAD_SHARED_FRAMES - start;
    memcpy(E.dry, &sh->data[start * 2], first * 2 * sizeof(float));
    if (n > first) memcpy(E.dry + first * 2, &sh->data[0], (n - first) * 2 * sizeof(float));
    atomic_store_explicit(&sh->readPos, r + n, memory_order_release);
    atomic_fetch_add_explicit(&E.fillSum, avail - n, memory_order_relaxed);
    atomic_fetch_add_explicit(&E.fillCount, 1, memory_order_relaxed);
    return 1;
}

static OSStatus OutputProc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inTime,
                           AudioBufferList *out, const AudioTimeStamp *outTime, void *ctx) {
    (void)dev; (void)now; (void)in; (void)inTime; (void)outTime; (void)ctx;
    if (!out || out->mNumberBuffers == 0) return noErr;
    for (UInt32 b = 0; b < out->mNumberBuffers; b++)
        if (out->mBuffers[b].mData) memset(out->mBuffers[b].mData, 0, out->mBuffers[b].mDataByteSize);
    if (E.lBuf >= out->mNumberBuffers || E.rBuf >= out->mNumberBuffers) return noErr;
    AudioBuffer *lb = &out->mBuffers[E.lBuf], *rb = &out->mBuffers[E.rBuf];
    if (!lb->mData || !rb->mData || lb->mNumberChannels == 0) return noErr;
    uint32_t frames = lb->mDataByteSize / (sizeof(float) * lb->mNumberChannels);
    float *lp = lb->mData, *rp = rb->mData;
    UInt32 lStride = lb->mNumberChannels, rStride = rb->mNumberChannels;

    RefreshParams();
    const int pulseEnabled = atomic_load_explicit(&E.pulseEnabled, memory_order_relaxed);
    const float mixStep = (float)(1.0 / (CROSSFADE_SECONDS * E.sr));

    for (uint32_t off = 0; off < frames;) {
        uint32_t n = frames - off < CHUNK_FRAMES ? frames - off : CHUNK_FRAMES;
        ReadRing(n);

        int silent = 1;
        for (uint32_t i = 0; i < n * 2; i++)
            if (E.dry[i] != 0.0f) { silent = 0; break; }

        // Passthrough is the dry signal untouched; Comfort crossfades to the processed one.
        const float *src = E.dry;
        float target = E.rtComfort ? 1.0f : 0.0f;
        if (E.mix > 0.0f || target > 0.0f) {
            if (E.mix == 0.0f) aad_dsp_reset(&E.dsp);
            memcpy(E.wet, E.dry, n * 2 * sizeof(float));
            aad_dsp_process(&E.dsp, &E.rtPreset, E.wet, n);
            if (E.mix != target) {
                for (uint32_t i = 0; i < n; i++) {
                    E.mix += target > E.mix ? mixStep : -mixStep;
                    if (E.mix > 1.0f) E.mix = 1.0f;
                    if (E.mix < 0.0f) E.mix = 0.0f;
                    E.wet[2 * i] = E.dry[2 * i] + (E.wet[2 * i] - E.dry[2 * i]) * E.mix;
                    E.wet[2 * i + 1] = E.dry[2 * i + 1] + (E.wet[2 * i + 1] - E.dry[2 * i + 1]) * E.mix;
                }
            }
            src = E.wet;
        }

        for (uint32_t i = 0; i < n; i++) {
            lp[(off + i) * lStride + E.lCh] = src[2 * i];
            rp[(off + i) * rStride + E.rCh] = src[2 * i + 1];
        }

        // Keep-alive: during silence only, so playing audio is never touched.
        if (!silent) {
            E.silentFrames = 0;
            E.pulsePos = -1;
        } else {
            if (E.pulsePos < 0) {
                E.silentFrames += n;
                if (pulseEnabled && E.silentFrames >= E.intervalFrames) {
                    E.pulsePos = 0;
                    E.phase = 0;
                    E.silentFrames = 0;
                }
            }
            if (E.pulsePos >= 0) {
                for (uint32_t i = 0; i < n && (uint64_t)E.pulsePos < E.pulseLen; i++, E.pulsePos++) {
                    uint64_t k = (uint64_t)E.pulsePos;
                    double env = 1.0;
                    if (k < E.fadeLen) env = 0.5 - 0.5 * cos(M_PI * (double)k / (double)E.fadeLen);
                    else if (k >= E.pulseLen - E.fadeLen) env = 0.5 - 0.5 * cos(M_PI * (double)(E.pulseLen - k) / (double)E.fadeLen);
                    float s = (float)(sin(E.phase) * E.pulseAmp * env);
                    E.phase += E.phaseInc;
                    if (E.phase > 2.0 * M_PI) E.phase -= 2.0 * M_PI;
                    lp[(off + i) * lStride + E.lCh] += s;
                    rp[(off + i) * rStride + E.rCh] += s;
                }
                if ((uint64_t)E.pulsePos >= E.pulseLen) E.pulsePos = -1;
            }
        }
        atomic_store_explicit(&E.pulsing, E.pulsePos >= 0, memory_order_relaxed);
        off += n;
    }
    return noErr;
}

// MARK: - Control

// MARK: - Broker

static AADShared *gLinked;
static xpc_connection_t gBroker;
static void (*gLost)(void);

void aad_link_set_lost_handler(void (*handler)(void)) { gLost = handler; }

AADShared *aad_link(void) {
    if (gLinked) return gLinked;
    if (!gBroker) {
        gBroker = xpc_connection_create_mach_service(AAD_BROKER_SERVICE, NULL, 0);
        const char *req = "anchor apple generic and certificate leaf[subject.OU] = \"" AAD_TEAM_ID "\"";
        if (xpc_connection_set_peer_code_signing_requirement(gBroker, req) != 0) {
            xpc_release(gBroker);
            gBroker = NULL;
            return NULL;
        }
        xpc_connection_set_event_handler(gBroker, ^(xpc_object_t event) {
            if (xpc_get_type(event) != XPC_TYPE_ERROR) return;
            // The broker restarted (new region) or is gone: forget the ring; the app restarts.
            dispatch_async(dispatch_get_main_queue(), ^{
                if (event == XPC_ERROR_CONNECTION_INVALID && gBroker) {
                    xpc_release(gBroker);
                    gBroker = NULL;
                }
                gLinked = NULL;
                if (gLost) gLost();
            });
        });
        xpc_connection_activate(gBroker);
    }
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "op", "ring");
    xpc_object_t reply = xpc_connection_send_message_with_reply_sync(gBroker, msg);
    xpc_release(msg);
    AADShared *s = NULL;
    if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY) {
        xpc_object_t shm = xpc_dictionary_get_value(reply, "shm");
        void *region = NULL;
        size_t size = shm ? xpc_shmem_map(shm, &region) : 0;
        AADShared *candidate = region;
        if (size >= sizeof(AADShared) && candidate->magic == AAD_SHARED_MAGIC && candidate->version == AAD_SHARED_VERSION &&
            candidate->frames == AAD_SHARED_FRAMES && candidate->channels == AAD_SHARED_CHANNELS)
            s = candidate; // kept mapped for good: the output thread may hold it
    }
    xpc_release(reply);
    gLinked = s;
    return s;
}

OSStatus aad_engine_start(AudioObjectID virtualDevice, AudioObjectID outputDevice, AADShared *shared) {
    aad_engine_stop();
    if (!shared) return kAudioHardwareNotRunningError;

    AudioObjectPropertyAddress ra = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    Float64 sr = 0;
    UInt32 size = sizeof(sr);
    OSStatus err = AudioObjectGetPropertyData(outputDevice, &ra, 0, NULL, &size, &sr);
    if (err) return err;
    if (sr <= 0) return kAudioDeviceUnsupportedFormatError;
    if ((err = CheckFloatOutput(outputDevice))) return err;
    if ((err = MapStereo(outputDevice))) return err;

    E.inDev = virtualDevice;
    E.outDev = outputDevice;
    E.sr = sr;
    E.shared = shared;
    UInt32 inBuf = GetU32(virtualDevice, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, 512);
    E.target = (inBuf > 512 ? inBuf : 512) + 256;

    // Skip whatever the driver wrote while nobody was reading.
    atomic_store(&shared->readPos, atomic_load(&shared->writePos));
    atomic_store(&E.fillSum, 0);
    atomic_store(&E.fillCount, 0);
    E.primed = 0;
    E.mix = 0;
    E.rtSeq = (uint32_t)-1;
    aad_dsp_init(&E.dsp, sr);
    E.silentFrames = 0;
    E.pulsePos = -1;
    double hz = PULSE_HZ < 0.45 * sr ? PULSE_HZ : 0.45 * sr; // clear of Nyquist, as QUAD Keep-Alive does
    E.phaseInc = 2.0 * M_PI * hz / sr;
    E.pulseAmp = pow(10.0, PULSE_DBFS / 20.0);
    E.pulseLen = (uint64_t)(PULSE_SECONDS * sr);
    E.fadeLen = (uint64_t)(PULSE_FADE_SECONDS * sr);
    E.intervalFrames = (uint64_t)(PULSE_INTERVAL_SECONDS * sr);
    E.integral = 0;
    E.lastFill = 0;
    SetRateAdjust(1.0);

    if ((err = AudioDeviceCreateIOProcID(outputDevice, OutputProc, NULL, &E.outProc))) goto fail;
    DisableStreams(outputDevice, E.outProc, kAudioObjectPropertyScopeInput);
    if ((err = AudioDeviceStart(outputDevice, E.outProc))) goto fail;
    E.running = 1;
    return noErr;

fail:
    aad_engine_stop();
    return err;
}

void aad_engine_stop(void) {
    if (E.outProc) {
        AudioDeviceStop(E.outDev, E.outProc);
        AudioDeviceDestroyIOProcID(E.outDev, E.outProc);
        E.outProc = NULL;
    }
    if (E.running) SetRateAdjust(1.0);
    E.running = 0;
    atomic_store(&E.pulsing, 0);
}

void aad_engine_set_comfort(int enabled, AADPreset preset) {
    atomic_fetch_add_explicit(&E.seq, 1, memory_order_acq_rel); // odd: writing
    E.preset = preset;
    E.comfort = enabled;
    atomic_fetch_add_explicit(&E.seq, 1, memory_order_release); // even: done
}

void aad_engine_set_pulse(int enabled) { atomic_store(&E.pulseEnabled, enabled); }

void aad_engine_control_tick(double seconds) {
    if (!E.running) return;
    uint64_t sum = atomic_exchange(&E.fillSum, 0);
    uint64_t count = atomic_exchange(&E.fillCount, 0);
    if (count == 0) return;
    double fill = (double)sum / (double)count;
    E.lastFill = fill;

    // Too full: the virtual device produces faster than the output consumes, so slow it down.
    double e = fill - E.target;
    E.integral += e * seconds;
    double iMax = RATE_ADJUST_LIMIT * E.sr * I_SECONDS;
    if (E.integral > iMax) E.integral = iMax;
    if (E.integral < -iMax) E.integral = -iMax;
    double adj = 1.0 - e / (E.sr * P_SECONDS) - E.integral / (E.sr * I_SECONDS);
    if (adj > 1 + RATE_ADJUST_LIMIT) adj = 1 + RATE_ADJUST_LIMIT;
    if (adj < 1 - RATE_ADJUST_LIMIT) adj = 1 - RATE_ADJUST_LIMIT;
    SetRateAdjust(adj);
}

AADStatus aad_engine_status(void) {
    AADStatus s = {0};
    s.running = E.running;
    s.sampleRate = E.sr;
    s.fillFrames = E.lastFill;
    s.targetFrames = E.target;
    s.rateAdjust = E.rateAdjust;
    s.underruns = atomic_load(&E.underruns);
    s.drops = E.shared ? atomic_load(&E.shared->drops) : 0;
    s.pulsing = atomic_load(&E.pulsing);
    return s;
}
