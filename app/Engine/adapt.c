#include "adapt.h"

static double Clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

void aad_adapt_init(AADAdapt *a, double sampleRate, double baseFrames, double capMs, uint64_t underruns, uint64_t overloads) {
    a->sr = sampleRate;
    a->base = baseFrames;
    a->extra = 0;
    a->quiet = 0;
    a->underruns = underruns;
    a->overloads = overloads;
    aad_adapt_set_cap(a, capMs);
}

void aad_adapt_set_cap(AADAdapt *a, double capMs) {
    a->cap = capMs > 0 ? capMs / 1000.0 * a->sr : 0;
    a->extra = Clamp(a->extra, 0, a->cap);
}

double aad_adapt_tick(AADAdapt *a, double seconds, uint64_t underruns, uint64_t overloads, double minFill) {
    double ms = a->sr / 1000.0;
    int dropout = underruns != a->underruns;
    int overload = overloads != a->overloads;
    int tight = minFill >= 0 && minFill < AAD_ADAPT_TIGHT_MS * ms;
    a->underruns = underruns;
    a->overloads = overloads;

    if (dropout) {
        a->extra += AAD_ADAPT_JUMP_MS * ms;
        a->quiet = 0;
    } else if (overload || tight) {
        a->extra += AAD_ADAPT_STEP_MS * ms;
        a->quiet = 0;
    } else {
        a->quiet += seconds;
        if (a->quiet >= AAD_ADAPT_QUIET_SECONDS) a->extra -= AAD_ADAPT_DECAY_MS_PER_S * ms * seconds;
    }
    a->extra = Clamp(a->extra, 0, a->cap);
    return a->base + a->extra;
}
