/*
 * HRTF Filter
 *
 * Copyright (c) 2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
/* hakuX native core: the same filter (31-tap HRIR convolution per channel,
 * an interaural delay with linear interpolation), laid out for NEON. The
 * delay line holds its samples twice, so any delay's window is contiguous;
 * the coefficients are stored reversed and padded to 32 with a zero tap, so
 * the convolution is a dot product of two contiguous arrays. */

#ifndef HW_XBOX_MCPX_HRTF_H
#define HW_XBOX_MCPX_HRTF_H

#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "native/dev/apu/apu_regs.h"

#define HRTF_SAMPLES_PER_FRAME  NUM_SAMPLES_PER_FRAME
#define HRTF_NUM_TAPS           31
#define HRTF_TAPS_PADDED        32
#define HRTF_MAX_DELAY_SAMPLES  42
#define HRTF_BUFLEN             (HRTF_NUM_TAPS + HRTF_MAX_DELAY_SAMPLES)

typedef struct {
    int buf_pos;
    struct {
        /* the last HRTF_BUFLEN samples, each at i and i + HRTF_BUFLEN; the
         * padded tap reads one past the second copy */
        float buf[2 * HRTF_BUFLEN + 2];
        /* tap k at HRTF_NUM_TAPS - 1 - k; the last (padding) is 0 */
        float hrir_coeff_cur[HRTF_TAPS_PADDED];
        float hrir_coeff_tar[HRTF_TAPS_PADDED];
    } ch[2];
    float itd_cur;
    float itd_tar;
    /* the parameters have a previous frame to move from; clear for a voice's
     * first frame, which starts at its entry's values */
    bool params_set;
} HrtfFilter;

static inline void hrtf_filter_init(HrtfFilter *f)
{
    memset(f, 0, sizeof(*f));
}

static inline void hrtf_filter_clear_history(HrtfFilter *f)
{
    f->buf_pos = 0;
    memset(f->ch[0].buf, 0, sizeof(f->ch[0].buf));
    memset(f->ch[1].buf, 0, sizeof(f->ch[1].buf));
}

/* Taps apply as written: hardware does not normalize or scale them. On a
 * retail console a single tap of 40h passed DC at 1/16 and one of 7Fh at
 * 31/32. */
static inline void
hrtf_filter_set_target_params(HrtfFilter *f, float hrir_coeff[2][HRTF_NUM_TAPS],
                              float itd)
{
    f->itd_tar =
        fmaxf(-HRTF_MAX_DELAY_SAMPLES, fminf(itd, HRTF_MAX_DELAY_SAMPLES));

    for (int ch = 0; ch < 2; ch++) {
        const float *coeff = hrir_coeff[ch];
        float *tar = f->ch[ch].hrir_coeff_tar;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            tar[HRTF_NUM_TAPS - 1 - k] = coeff[k];
        }
        tar[HRTF_NUM_TAPS] = 0.0f;
    }
}

static inline void hrtf_filter_settle_parameters(HrtfFilter *f)
{
    for (int ch = 0; ch < 2; ch++) {
        memcpy(f->ch[ch].hrir_coeff_cur, f->ch[ch].hrir_coeff_tar,
               sizeof(f->ch[ch].hrir_coeff_cur));
    }
    f->itd_cur = f->itd_tar;
}

/* The parameters at their targets: the frame needs no interpolation. */
static inline bool hrtf_filter_settled(const HrtfFilter *f)
{
    if (f->itd_cur != f->itd_tar) {
        return false;
    }
    for (int ch = 0; ch < 2; ch++) {
        if (memcmp(f->ch[ch].hrir_coeff_cur, f->ch[ch].hrir_coeff_tar,
                   sizeof(f->ch[ch].hrir_coeff_cur)) != 0) {
            return false;
        }
    }
    return true;
}

/* sum of c[m] * w[m] over the padded taps. With tar, each c[m] is first
 * taken iw of the way to tar[m] (the frame's interpolation, not stored);
 * with dfrac, each w[m] is first interpolated toward w[m - 1] (the sample
 * one further back). */
static inline float hrtf_dot(const float *c, const float *tar, float iw,
                             const float *w, float dfrac)
{
    const bool interp = dfrac > 0.0f;
    const float w0 = 1 - dfrac;
#if defined(__aarch64__)
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = vdupq_n_f32(0.0f);
    for (int m = 0; m < HRTF_TAPS_PADDED; m += 8) {
        float32x4_t c0 = vld1q_f32(c + m), c1 = vld1q_f32(c + m + 4);
        if (tar) {
            c0 = vfmaq_n_f32(c0, vsubq_f32(vld1q_f32(tar + m), c0), iw);
            c1 = vfmaq_n_f32(c1, vsubq_f32(vld1q_f32(tar + m + 4), c1), iw);
        }
        float32x4_t s0 = vld1q_f32(w + m), s1 = vld1q_f32(w + m + 4);
        if (interp) {
            s0 = vfmaq_n_f32(vmulq_n_f32(vld1q_f32(w + m - 1), dfrac), s0, w0);
            s1 = vfmaq_n_f32(vmulq_n_f32(vld1q_f32(w + m + 3), dfrac), s1, w0);
        }
        a0 = vfmaq_f32(a0, c0, s0);
        a1 = vfmaq_f32(a1, c1, s1);
    }
    return vaddvq_f32(vaddq_f32(a0, a1));
#else
    float acc = 0.0f;
    for (int m = 0; m < HRTF_TAPS_PADDED; m++) {
        float cm = c[m];
        if (tar) {
            cm = cm + (tar[m] - cm) * iw;
        }
        float s = w[m];
        if (interp) {
            s = s * w0 + w[m - 1] * dfrac;
        }
        acc += cm * s;
    }
    return acc;
#endif
}

/* When a voice's entry changes, its taps move from the last frame's to the
 * new ones in 32 steps across one frame (measured: tap 0 changing from 7Fh to
 * 40h read 3E0000h, 2F8000h, 210000h, 128000h and 05D000h at samples 0, 8,
 * 16, 24 and 31 for a DC of 400000h). The delay is assumed to move the same
 * way. A voice's first frame starts at its entry's values. */
static inline void hrtf_filter_process(HrtfFilter *f,
                                       float in[HRTF_SAMPLES_PER_FRAME][2],
                                       float out[HRTF_SAMPLES_PER_FRAME][2])
{
    if (!f->params_set) {
        hrtf_filter_settle_parameters(f);
        f->params_set = true;
    }
    const bool settled = hrtf_filter_settled(f);
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        const float iw = n / (float)HRTF_SAMPLES_PER_FRAME;
        const float itd = settled ? f->itd_cur : f->itd_cur + (f->itd_tar - f->itd_cur) * iw;
        const int pos = f->buf_pos;

        for (int ch = 0; ch < 2; ch++) {
            float *buf = f->ch[ch].buf;

            // Push new sample
            buf[pos] = in[n][ch];
            buf[pos + HRTF_BUFLEN] = in[n][ch];

            // Interaural time difference (channel delay)
            float d = itd * (ch == 0 ? +1.0f : -1.0f);
            if (d < 0.0f) {
                d = 0.0f;
            }
            int di = d;
            float dfrac = d - di;

            // HRIR Convolution: tap k reads the sample di + k back,
            // buf[pos + HRTF_BUFLEN - di - k]; reversed, from tap 30 up
            const float *w = &buf[pos + HRTF_BUFLEN - di - (HRTF_NUM_TAPS - 1)];
            out[n][ch] = hrtf_dot(f->ch[ch].hrir_coeff_cur,
                                  settled ? NULL : f->ch[ch].hrir_coeff_tar, iw, w, dfrac);
        }

        f->buf_pos = (pos + 1) % HRTF_BUFLEN;
    }

    hrtf_filter_settle_parameters(f);
}

#endif
