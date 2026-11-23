// Adaptive ring delay: how much fill the engine should hold in the shared ring.
//
// The ring holds a base fill (the virtual device's buffer plus a margin). Under load the producer
// (coreaudiod running the virtual device) or the consumer (our output IOProc) can run late, and
// the ring runs dry: a dropout. So the target grows when the system is tight and shrinks back once
// it has been quiet for a while, never past a user-set maximum.
//
// Growing and shrinking happen through the engine's clock steering, which fills or drains the
// ring by at most ~0.5 ms per second, so audio stays bit-exact. The one exception is an actual
// dropout: the engine re-primes anyway, so the target jumps straight up and the re-prime fills it.
//
// Pure logic, no Core Audio: tests/adapt_test.c drives it.
#pragma once
#include <stdint.h>

typedef struct {
    double base;  // frames: the fill to hold when the system is quiet
    double cap;   // frames: the most extra fill allowed (the user's maximum)
    double extra; // frames held on top of base now
    double quiet; // seconds since the last sign of trouble
    double sr;
    uint64_t underruns, overloads; // totals seen at the last tick
} AADAdapt;

#define AAD_ADAPT_JUMP_MS 20.0        // added at once on a dropout
#define AAD_ADAPT_STEP_MS 2.0         // added per tick while tight
#define AAD_ADAPT_TIGHT_MS 1.0        // the ring dipping below this after a read counts as tight
#define AAD_ADAPT_QUIET_SECONDS 120.0 // calm this long before shrinking
#define AAD_ADAPT_DECAY_MS_PER_S 0.1  // shrink rate once quiet (1 ms every 10 s)

void aad_adapt_init(AADAdapt *a, double sampleRate, double baseFrames, double capMs, uint64_t underruns, uint64_t overloads);
void aad_adapt_set_cap(AADAdapt *a, double capMs);
// One control period. minFill: the lowest ring fill (frames) after a read this period, or < 0
// if there were no reads. Returns the fill target in frames.
double aad_adapt_tick(AADAdapt *a, double seconds, uint64_t underruns, uint64_t overloads, double minFill);
