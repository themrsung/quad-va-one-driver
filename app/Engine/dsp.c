#include "dsp.h"
#include <math.h>
#include <string.h>

static inline float dbToGain(float db) { return expf(db * 0.11512925f); } // ln(10)/20

// RBJ audio EQ cookbook filters.
static void Peaking(AADBiquad *f, double sr, double hz, double db, double q) {
    double A = pow(10, db / 40), w = 2 * M_PI * hz / sr, c = cos(w), alpha = sin(w) / (2 * q);
    double a0 = 1 + alpha / A;
    f->b0 = (1 + alpha * A) / a0;
    f->b1 = -2 * c / a0;
    f->b2 = (1 - alpha * A) / a0;
    f->a1 = -2 * c / a0;
    f->a2 = (1 - alpha / A) / a0;
}

static void HighShelf(AADBiquad *f, double sr, double hz, double db) {
    double A = pow(10, db / 40), w = 2 * M_PI * hz / sr, c = cos(w), s = sin(w);
    double alpha = s / 2 * sqrt(2); // shelf slope 1
    double k = 2 * sqrt(A) * alpha;
    double a0 = (A + 1) - (A - 1) * c + k;
    f->b0 = A * ((A + 1) + (A - 1) * c + k) / a0;
    f->b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
    f->b2 = A * ((A + 1) + (A - 1) * c - k) / a0;
    f->a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
    f->a2 = ((A + 1) - (A - 1) * c - k) / a0;
}

static inline float Run(AADBiquad *f, int ch, float x) {
    double y = f->b0 * x + f->z1[ch];
    f->z1[ch] = f->b1 * x - f->a1 * y + f->z2[ch];
    f->z2[ch] = f->b2 * x - f->a2 * y;
    return (float)y;
}

void aad_dsp_init(AADDsp *d, double sampleRate) {
    memset(d, 0, sizeof(*d));
    d->sampleRate = sampleRate;
    int la = (int)(0.002 * sampleRate + 0.5); // 2 ms
    if (la < 1) la = 1;
    if (la > AAD_LOOKAHEAD_MAX) la = AAD_LOOKAHEAD_MAX;
    d->lookahead = la;
    aad_dsp_reset(d);
}

void aad_dsp_reset(AADDsp *d) {
    double sr = d->sampleRate;
    int la = d->lookahead;
    memset(d, 0, sizeof(*d));
    d->sampleRate = sr;
    d->lookahead = la;
    Peaking(&d->presence, sr, AAD_PRESENCE_HZ, AAD_PRESENCE_DB, AAD_PRESENCE_Q);
    HighShelf(&d->shelf, sr, AAD_SHELF_HZ, AAD_SHELF_DB);
    d->power = 1e-6f; // −60 dB: start quiet rather than at −inf
    d->levelDb = -60;
    d->limGain = 1;
}

void aad_dsp_process(AADDsp *d, const AADPreset *p, float *lr, uint32_t frames) {
    const float sr = (float)d->sampleRate;
    const float atk = 1.0f - expf(-1.0f / (fmaxf(p->attackMs, 0.1f) * 0.001f * sr));
    const float rel = 1.0f - expf(-1.0f / (fmaxf(p->releaseMs, 1.0f) * 0.001f * sr));
    const float avg = 1.0f - expf(-1.0f / (0.020f * sr));
    const float smooth = 1.0f - expf(-1.0f / (0.010f * sr)); // gain changes glide over ~10 ms
    const int L = d->lookahead;
    const float limAtk = 1.0f - expf(-4.0f / (float)L);
    const float limRel = 1.0f - expf(-1.0f / (0.080f * sr));
    const float ceiling = dbToGain(AAD_CEILING_DB);

    for (uint32_t i = 0; i < frames; i++) {
        // The gain curve.
        float l = Run(&d->shelf, 0, Run(&d->presence, 0, lr[2 * i]));
        float r = Run(&d->shelf, 1, Run(&d->presence, 1, lr[2 * i + 1]));

        // Expander: stereo-linked RMS (20 ms mean square), then attack/release on the level in dB,
        // so the ballistics follow the music rather than the waveform's ripple. Plain profiles
        // glide to 0 dB, so switching profiles never steps.
        float target = 0;
        if (p->expand) {
            d->power += avg * (0.5f * (l * l + r * r) - d->power);
            float rmsDb = 10.0f * log10f(d->power + 1e-12f);
            d->levelDb += (rmsDb > d->levelDb ? atk : rel) * (rmsDb - d->levelDb);
            float levelDb = d->levelDb;
            float g = 0;
            if (levelDb > p->upThresholdDb) {
                g = (levelDb - p->upThresholdDb) * (p->upRatio - 1.0f);
                if (g > p->maxBoostDb) g = p->maxBoostDb;
            } else if (levelDb < p->downThresholdDb) {
                g = (levelDb - p->downThresholdDb) * (p->downRatio - 1.0f);
                if (g < -40.0f) g = -40.0f;
            }
            target = g + p->makeupDb;
        }
        d->gainDb += smooth * (target - d->gainDb);
        float gain = dbToGain(d->gainDb);
        l *= gain;
        r *= gain;

        // Limiter (expanding profiles only): the gain each frame needs to stay under the ceiling,
        // and the minimum of that over the lookahead window (sliding minimum on a monotonic deque).
        // Plain profiles keep the same delay, with the limiter released, so latency never changes.
        float peak = fmaxf(fabsf(l), fabsf(r));
        float need = p->expand && peak > ceiling ? ceiling / peak : 1.0f;
        long long n = d->n++;
        while (d->dqCount > 0) {
            int back = (d->dqHead + d->dqCount - 1) % AAD_LOOKAHEAD_MAX;
            if (d->dqVal[back] >= need) d->dqCount--;
            else break;
        }
        int slot = (d->dqHead + d->dqCount) % AAD_LOOKAHEAD_MAX;
        d->dqVal[slot] = need;
        d->dqIdx[slot] = (int)(n % 1000000000LL);
        d->dqCount++;
        while (d->dqCount > 0) {
            long long age = (n % 1000000000LL) - d->dqIdx[d->dqHead];
            if (age < 0) age += 1000000000LL;
            if (age >= L) {
                d->dqHead = (d->dqHead + 1) % AAD_LOOKAHEAD_MAX;
                d->dqCount--;
            } else break;
        }
        float limTarget = d->dqVal[d->dqHead];
        d->limGain += (limTarget < d->limGain ? limAtk : limRel) * (limTarget - d->limGain);

        // Delay line: write the newest frame, read the oldest.
        d->delay[2 * d->pos] = l;
        d->delay[2 * d->pos + 1] = r;
        d->pos = (d->pos + 1) % L;
        float ol = d->delay[2 * d->pos] * d->limGain;
        float orr = d->delay[2 * d->pos + 1] * d->limGain;
        if (p->expand) {
            ol = fminf(fmaxf(ol, -ceiling), ceiling);
            orr = fminf(fmaxf(orr, -ceiling), ceiling);
        }
        lr[2 * i] = ol;
        lr[2 * i + 1] = orr;
    }
}
