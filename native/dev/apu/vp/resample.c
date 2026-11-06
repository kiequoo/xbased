#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "resample.h"

/*
 * The VP's own mix to 16-bit samples (libsamplerate's
 * src_float_to_short_array, with NEON). The voices' resampler is the
 * 8-point Lagrange interpolator in vp.c (voice_resample).
 */
void src_float_to_short_array(const float *in, short *out, int len)
{
    if (!in || !out || len <= 0) {
        return;
    }

#if defined(__aarch64__)
    float32x4_t scale = vdupq_n_f32(32767.0f);
    float32x4_t hi = vdupq_n_f32(1.0f);
    float32x4_t lo = vdupq_n_f32(-1.0f);
    int i = 0;
    for (; i + 4 <= len; i += 4) {
        float32x4_t v = vld1q_f32(&in[i]);
        v = vminq_f32(vmaxq_f32(v, lo), hi);
        v = vmulq_f32(v, scale);
        int32x4_t iv = vcvtq_s32_f32(v);
        int16x4_t sv = vqmovn_s32(iv);
        vst1_s16(&out[i], sv);
    }
    for (; i < len; i++) {
        float v = in[i];
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        out[i] = (short)(v * 32767.0f);
    }
#else
    for (int i = 0; i < len; ++i) {
        float v = in[i];
        if (v > 1.0f) {
            v = 1.0f;
        } else if (v < -1.0f) {
            v = -1.0f;
        }
        out[i] = (short)(v * 32767.0f);
    }
#endif
}
