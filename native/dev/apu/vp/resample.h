/*
 * hakuX native core: libsamplerate's float-to-short conversion, the one
 * part of its API the APU uses (the VP's own mix, vp.c). The voices'
 * resampler is vp.c's voice_resample.
 */
#ifndef NATIVE_DEV_APU_VP_RESAMPLE_H
#define NATIVE_DEV_APU_VP_RESAMPLE_H

#include <stddef.h>

void src_float_to_short_array(const float *in, short *out, int len);

#endif
