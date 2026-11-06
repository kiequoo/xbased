/*
 * Adapted from SWH LADSPA Plugins package, modified for xemu
 *
 * Source:  https://github.com/swh/ladspa/blob/master/svf_1214.xml
 * Author:  Steve Harris, andy@vellocet
 * License: GPLv2
 *
 */

#ifndef SVF_H
#define SVF_H

#include <math.h>

/* Hardware keeps filter values to 1/16 of a 24-bit sample, rounding halves
 * up (measured: ringing DLS2 and ParaEQ responses match this to the bit
 * across all 32 samples of a frame, where full precision missed most of them
 * by a few units). */
static inline float svf_round(float x)
{
    return floorf(x * 524288.0f + 0.5f) / 524288.0f;
}

// Constants to match filter types
#define F_LP 1
#define F_HP 2
#define F_BP 3
#define F_BR 4
#define F_AP 5

/* Structure to hold parameters for SV filter. Plain data (the output is
 * chosen by type, not a pointer into the struct), so a voice's filters can be
 * saved in a savestate. */
typedef struct {
    float f;     // frequency coefficient
    float q;     // damping
    float h;     // high pass output
    float b;     // band pass output
    float l;     // low pass output
    int type;    // F_LP ...: the output run_svf returns
} sv_filter;

static inline void setup_svf(sv_filter *sv, float fc, float q, int t) {
    sv->f = fc;
    sv->q = q;
    sv->type = t;
}

/* Run one sample through the SV filter. Filter is by andy@vellocet. No input
 * scaling or waveshaping: on hardware the filter is linear with unity
 * passband gain at any resonance (a retail console's impulse responses at
 * FC0 E000h, C000h and 8000h with FC1 8000h and 2000h match the plain
 * filter, high then band then low). Each step is rounded as the hardware
 * rounds it (svf_round). */
static inline float run_svf(sv_filter *sv, float in) {
    sv->h = svf_round(in - sv->l - sv->q * sv->b);
    sv->b = svf_round(sv->b + sv->f * sv->h);
    sv->l = svf_round(sv->l + sv->f * sv->b);
    switch (sv->type) {
    case F_LP:
        return sv->l;
    case F_HP:
        return sv->h;
    case F_BP:
        return sv->b;
    case F_BR:
        return sv->l + sv->h;
    default:
        return sv->l - sv->h;
    }
}

#endif
