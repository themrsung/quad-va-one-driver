// Comfort DSP: a fixed gain curve (presence dip and treble shelf), then, for profiles that expand,
// a stereo-linked broadband expander and a lookahead peak limiter.
// Real-time safe: no allocation, no locks.
#pragma once
#include <stdint.h>

typedef struct {
    int expand;            // 0: the gain curve only ("plain"); the fields below are ignored
    float upThresholdDb;   // RMS level above which the signal is pushed up
    float upRatio;         // 1 = off; 1.3 = +0.3 dB per dB above the threshold
    float maxBoostDb;      // cap on the upward gain
    float downThresholdDb; // RMS level below which the signal is pushed down
    float downRatio;       // 1 = off; 2 = −1 dB per dB below the threshold
    float attackMs;        // detector attack
    float releaseMs;       // detector release
    float makeupDb;        // output gain (negative: headroom for the upward gain)
} AADPreset;

// The gain curve, the same in every Comfort profile: a gentle dip where hearing is most sensitive
// and harshness lives, and a shelf that takes the edge off the treble without dulling it.
#define AAD_PRESENCE_HZ 3200.0
#define AAD_PRESENCE_DB (-1.5)
#define AAD_PRESENCE_Q 0.9
#define AAD_SHELF_HZ 7000.0
#define AAD_SHELF_DB (-2.5)

#define AAD_LOOKAHEAD_MAX 1024
#define AAD_CEILING_DB (-1.0f)

typedef struct { double b0, b1, b2, a1, a2; double z1[2], z2[2]; } AADBiquad;

typedef struct {
    double sampleRate;
    AADBiquad presence, shelf;
    float power;           // mean square, 20 ms average
    float levelDb;         // detector level after attack/release
    float gainDb;          // smoothed expander + makeup gain
    // limiter
    int lookahead;
    int pos;
    float delay[AAD_LOOKAHEAD_MAX * 2];
    int dqIdx[AAD_LOOKAHEAD_MAX];      // monotonic deque of (index, required gain) for the window minimum
    float dqVal[AAD_LOOKAHEAD_MAX];
    int dqHead, dqCount;
    long long n;
    float limGain;
} AADDsp;

void aad_dsp_init(AADDsp *d, double sampleRate);
void aad_dsp_reset(AADDsp *d);
// In place, interleaved stereo.
void aad_dsp_process(AADDsp *d, const AADPreset *p, float *lr, uint32_t frames);
