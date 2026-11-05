// Checks the Comfort DSP: the gain curve's shape, expansion as configured, the ceiling, silence.
#include "../app/Engine/dsp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) printf("ok    "); else { printf("FAIL  "); failures++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static AADDsp dsp;

// Output/input RMS in dB over the last half of a steady sine.
static double gainAt(const AADPreset *p, double sr, double hz, double peakDb) {
    int n = (int)sr * 2;
    float *x = malloc(sizeof(float) * n * 2);
    double a = pow(10, peakDb / 20);
    for (int i = 0; i < n; i++) x[2 * i] = x[2 * i + 1] = (float)(a * sin(2 * M_PI * hz * i / sr));
    aad_dsp_init(&dsp, sr);
    aad_dsp_process(&dsp, p, x, n);
    double sIn = 0, sOut = 0;
    for (int i = n / 2 + 2000; i < n; i++) { // skip the limiter's delay
        double in = a * sin(2 * M_PI * hz * i / sr);
        sIn += in * in; sOut += (double)x[2 * i] * x[2 * i];
    }
    free(x);
    return 10 * log10(sOut / sIn);
}

int main(void) {
    const double sr = 48000;
    const AADPreset plain = {.expand = 0, .downThresholdDb = -100, .upRatio = 1, .downRatio = 1, .attackMs = 10, .releaseMs = 100};
    AADPreset p = {.expand = 1, .upThresholdDb = -20, .upRatio = 1.5, .maxBoostDb = 6, .downThresholdDb = -50, .downRatio = 2,
                   .attackMs = 10, .releaseMs = 150, .makeupDb = -6};

    // The gain curve alone (plain): untouched lows, a dip around 3.2 kHz, a shelf toward −2.5 dB.
    struct { double hz, want, tol; } curve[] = {{100, 0, 0.05}, {1000, -0.25, 0.25}, {3200, -1.6, 0.3}, {12000, -2.5, 0.4}, {18000, -2.5, 0.3}};
    for (int k = 0; k < 5; k++) {
        double g = gainAt(&plain, sr, curve[k].hz, -10);
        CHECK(fabs(g - curve[k].want) <= curve[k].tol, "curve at %5.0f Hz: %+.2f dB (want %+.2f)", curve[k].hz, g, curve[k].want);
    }
    // Plain doesn't limit: a full-scale low tone comes out at full scale.
    CHECK(fabs(gainAt(&plain, sr, 100, 0)) < 0.05, "plain doesn't limit");

    // Expansion, measured against plain at the same frequency so the curve cancels out.
    // Sine RMS is peak − 3 dB. At −10 dBFS peak: RMS −13 → 7 dB above −20 → +3.5 dB, makeup −6 → −2.5 dB.
    double g = gainAt(&p, sr, 1000, -10) - gainAt(&plain, sr, 1000, -10);
    CHECK(fabs(g - (-2.5)) < 0.3, "upward expansion: %.2f dB (want −2.5)", g);
    // RMS −33: between thresholds → makeup only.
    g = gainAt(&p, sr, 1000, -30) - gainAt(&plain, sr, 1000, -30);
    CHECK(fabs(g - (-6)) < 0.3, "between thresholds: %.2f dB (want −6)", g);
    // RMS −63: 13 dB under −50 at 2:1 → −13, makeup −6 → −19.
    g = gainAt(&p, sr, 1000, -60) - gainAt(&plain, sr, 1000, -60);
    CHECK(fabs(g - (-19)) < 0.5, "downward expansion: %.2f dB (want −19)", g);
    // Boost capped: RMS −3, 17 dB over → 8.5 capped at 6, makeup −6 → 0; the limiter then holds it under −1 dBFS.
    AADPreset loud = p; loud.makeupDb = 0;
    int n = (int)sr;
    float *x = malloc(sizeof(float) * n * 2);
    for (int i = 0; i < n; i++) x[2 * i] = x[2 * i + 1] = (float)(sin(2 * M_PI * 997 * i / sr) * (i % 9600 < 200 ? 1.0 : 0.3));
    aad_dsp_init(&dsp, sr);
    aad_dsp_process(&dsp, &loud, x, n);
    float peak = 0; int finite = 1;
    for (int i = 0; i < n * 2; i++) { if (!isfinite(x[i])) finite = 0; if (fabsf(x[i]) > peak) peak = fabsf(x[i]); }
    CHECK(finite, "output finite");
    CHECK(peak <= powf(10, -1.0f / 20) + 1e-6f, "peak %.3f dBFS stays under the −1 dBFS ceiling", 20 * log10f(peak));

    // Digital silence stays digital silence (so the keep-alive still sees silence in Comfort).
    for (int i = 0; i < n * 2; i++) x[i] = 0;
    aad_dsp_init(&dsp, sr);
    aad_dsp_process(&dsp, &p, x, n);
    aad_dsp_process(&dsp, &plain, x, n);
    int silent = 1; for (int i = 0; i < n * 2; i++) if (x[i] != 0) silent = 0;
    CHECK(silent, "silence in, silence out");

    // Every rate the driver offers.
    double rates[] = {44100, 88200, 96000, 176400, 192000};
    for (int r = 0; r < 5; r++) {
        g = gainAt(&p, rates[r], 1000, -10) - gainAt(&plain, rates[r], 1000, -10);
        double c = gainAt(&plain, rates[r], 12000, -10);
        CHECK(fabs(g - (-2.5)) < 0.3 && fabs(c - (-2.5)) < 0.4, "at %.0f Hz: expansion %.2f dB, curve at 12 kHz %.2f dB", rates[r], g, c);
    }
    free(x);
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}
