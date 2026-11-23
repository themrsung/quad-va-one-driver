// The adaptive ring delay policy (app/Engine/adapt.c): grows on dropouts and tight moments,
// shrinks once quiet, never past the cap. ./tests/run.sh builds and runs it.
#include <math.h>
#include <stdio.h>
#include "../app/Engine/adapt.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) printf("ok    "); else { printf("FAIL  "); failures++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static double ms(const AADAdapt *a, double frames) { return frames / a->sr * 1000.0; }

int main(void) {
    AADAdapt a;
    const double sr = 48000, base = 768, calm = 10 * 48; // calm: 10 ms of fill after every read
    aad_adapt_init(&a, sr, base, 50, 5, 7);              // totals carried over from before start
    double t = aad_adapt_tick(&a, 1, 5, 7, calm);
    CHECK(t == base, "quiet: holds the base (%.0f frames)", t);
    t = aad_adapt_tick(&a, 1, 5, 7, -1);
    CHECK(t == base, "no reads this period: no change");

    t = aad_adapt_tick(&a, 1, 6, 7, calm);
    CHECK(fabs(ms(&a, t - base) - 20) < 1e-9, "dropout: jumps 20 ms (+%.1f ms)", ms(&a, t - base));
    t = aad_adapt_tick(&a, 1, 6, 8, calm);
    CHECK(fabs(ms(&a, t - base) - 22) < 1e-9, "overload: +2 ms (+%.1f ms)", ms(&a, t - base));
    t = aad_adapt_tick(&a, 1, 6, 8, 0.5 * 48);
    CHECK(fabs(ms(&a, t - base) - 24) < 1e-9, "fill dipped under 1 ms: +2 ms (+%.1f ms)", ms(&a, t - base));
    t = aad_adapt_tick(&a, 1, 9, 8, calm);
    t = aad_adapt_tick(&a, 1, 12, 8, calm);
    CHECK(fabs(ms(&a, t - base) - 50) < 1e-9, "never past the cap (+%.1f ms)", ms(&a, t - base));

    double before = t;
    for (int i = 0; i < 119; i++) t = aad_adapt_tick(&a, 1, 12, 8, calm);
    CHECK(t == before, "holds for the first 2 minutes of calm");
    t = aad_adapt_tick(&a, 1, 12, 8, calm);
    CHECK(t < before && fabs(ms(&a, before - t) - 0.1) < 1e-9, "then shrinks 0.1 ms/s");
    for (int i = 0; i < 1000; i++) t = aad_adapt_tick(&a, 1, 12, 8, calm);
    CHECK(t == base, "back to the base after a long calm");

    aad_adapt_tick(&a, 1, 13, 8, calm);
    t = aad_adapt_tick(&a, 1, 13, 8, 0);
    CHECK(t > base + 20 * 48, "trouble during the decay resets it: grows again");
    aad_adapt_set_cap(&a, 10);
    CHECK(fabs(ms(&a, a.extra) - 10) < 1e-9, "lowering the cap trims the headroom (+%.1f ms)", ms(&a, a.extra));
    aad_adapt_set_cap(&a, 0);
    t = aad_adapt_tick(&a, 1, 20, 9, 0);
    CHECK(t == base, "cap 0: adaptation off");

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}
