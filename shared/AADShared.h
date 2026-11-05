// The audio ring shared by the driver (producer) and the app (consumer). The broker daemon owns
// the memory and hands the same region to both over XPC, so audio never goes through an input
// stream (which macOS would show as the microphone being in use).
#pragma once
#include <stdatomic.h>
#include <stdint.h>

#define AAD_BROKER_SERVICE "me.sjun.AnalogAudio.broker"
#define AAD_SHARED_MAGIC 0x41414431u // 'AAD1'
#define AAD_SHARED_VERSION 1u
#define AAD_SHARED_FRAMES 32768u     // power of two
#define AAD_SHARED_CHANNELS 2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t frames;
    uint32_t channels;
    _Atomic uint64_t writePos;   // frames written, ever (driver)
    _Atomic uint64_t readPos;    // frames read, ever (app); the app may jump it to writePos
    _Atomic uint64_t drops;      // frames the driver couldn't write because the ring was full
    _Atomic uint32_t sampleRate; // the driver's current rate, Hz
    uint32_t reserved[17];
    float data[AAD_SHARED_FRAMES * AAD_SHARED_CHANNELS]; // interleaved stereo float32
} AADShared;

_Static_assert(sizeof(AADShared) % 16 == 0, "keep the data aligned");
