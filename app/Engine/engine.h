// The app's audio path: reads what the driver writes into the shared ring, optionally processes it
// (Comfort), and plays it to the chosen output device, adding the keep-alive pulse during silence.
//
// Call everything from one thread (the main thread); the audio runs on the two devices' IO
// threads and never takes a lock.
#pragma once
#include <CoreAudio/CoreAudio.h>
#include "dsp.h"
#include "../../shared/AADShared.h"

typedef struct {
    int running;
    double sampleRate;
    double fillFrames;   // average shared-ring fill over the last control period
    double targetFrames;
    double rateAdjust;   // the virtual device's clock adjustment now in effect
    uint64_t underruns;
    uint64_t drops;      // frames the driver had to drop (ring full)
    int pulsing;
} AADStatus;

// Gets the shared ring from the broker (blocking, main thread). NULL if the broker isn't reachable.
AADShared *aad_link(void);
// Called on the main thread when the broker connection drops; the ring must be fetched again.
void aad_link_set_lost_handler(void (*handler)(void));

// Both devices must already run at the same nominal sample rate.
OSStatus aad_engine_start(AudioObjectID virtualDevice, AudioObjectID outputDevice, AADShared *shared);
void aad_engine_stop(void);
void aad_engine_set_comfort(int enabled, AADPreset preset);
void aad_engine_set_pulse(int enabled);
// Call about once a second: steers the virtual device's clock to the output device's.
void aad_engine_control_tick(double seconds);
AADStatus aad_engine_status(void);
