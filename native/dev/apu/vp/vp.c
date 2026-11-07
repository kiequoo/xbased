/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
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

#include "native/dev/apu/apu_int.h"
#include "adpcm.h"
#include "mix_inline.h"
#include "native/core/mmio.h"   /* nc_mmio_retry */

/* -------------------------------------------------------------------------
 * PERF_LOG-gated "silent-but-active voice" probe.
 *
 * Measures how much per-voice decode/resample/SVF/mix work voice_process()
 * spends on voices that are ACTIVE (fully processed) but MATHEMATICALLY
 * SILENT: envelope amplitude ea_value == 0 AND all 8 mixbin volumes fully
 * attenuated (vol & 0xFFF == 0xFFF, a route's gain 0 exactly). The
 * per-route gain is the envelope's level times the attenuation's, so
 * both-zero implies the voice emits silence this frame (but for the ramp
 * down from its last frame's gains, voice_process).
 *
 * This probe changes NO control flow and skips NO work — it only reads
 * already-computed locals (ea_value, vol[]) and increments relaxed-atomic
 * counters. With PERF_LOG=0 the entire block compiles to nothing, so the
 * release path is byte-identical to upstream.
 *
 * The counters race across concurrent voice_worker_thread workers and the
 * SE/EP-thread emit; relaxed atomics make that benign for a coarse ratio.
 * ------------------------------------------------------------------------- */
#ifndef PERF_LOG
#define PERF_LOG 0
#endif

/* Skip per-voice DECODE + SVF/HRTF/mix for mathematically-silent voices.
 * Defaulted here so non-CMake / host-test builds still compile; the CMake
 * HAKUX_DEFS block sets the shipping value (default 0). When 0, every guarded
 * block below is a compile-time no-op -> byte-identical to today. */
#ifndef HAKUX_AUDIO_SKIP_MUTED
#define HAKUX_AUDIO_SKIP_MUTED 0
#endif

#if PERF_LOG

#include <stdatomic.h>
#define VP_SILENT_LOG(...) nc_log("hakuX-vp", __VA_ARGS__)

static struct {
    atomic_uint_least64_t calls;          /* active calls reaching the eval point */
    atomic_uint_least64_t silent_calls;   /* of those, ea0 AND all-vol-max */
    atomic_uint_least64_t total_samples;  /* 32 per active call */
    atomic_uint_least64_t silent_samples; /* 32 per silent call */
    atomic_uint_least64_t ea0_calls;      /* sub-counter: ea_value==0 alone */
    atomic_uint_least64_t allvol_calls;   /* sub-counter: all 8 vol==0xFFF alone */
    atomic_uint_least64_t skipped_voices; /* HAKUX_AUDIO_SKIP_MUTED: voices that
                                           * took the decode+SVF/HRTF/mix skip.
                                           * 0 when the flag is off; ~=allvolmax
                                           * when on (confirms the gate fires). */
} g_vp_silent_stats;

#define VP_ADD(field, n) \
    atomic_fetch_add_explicit(&g_vp_silent_stats.field, (n), memory_order_relaxed)

void vp_silent_stats_log_and_reset(void)
{
    static int vp_silent_frame = 0;
    /* Throttle on the SE/EP-thread emit cadence (~once / few seconds). */
    if (++vp_silent_frame % 240 == 0) {
        uint64_t calls   = atomic_load_explicit(&g_vp_silent_stats.calls,
                                                memory_order_relaxed);
        uint64_t silent  = atomic_load_explicit(&g_vp_silent_stats.silent_calls,
                                                memory_order_relaxed);
        uint64_t tot_smp = atomic_load_explicit(&g_vp_silent_stats.total_samples,
                                                memory_order_relaxed);
        uint64_t sil_smp = atomic_load_explicit(&g_vp_silent_stats.silent_samples,
                                                memory_order_relaxed);
        uint64_t ea0     = atomic_load_explicit(&g_vp_silent_stats.ea0_calls,
                                                memory_order_relaxed);
        uint64_t allvol  = atomic_load_explicit(&g_vp_silent_stats.allvol_calls,
                                                memory_order_relaxed);
        uint64_t skipped = atomic_load_explicit(&g_vp_silent_stats.skipped_voices,
                                                memory_order_relaxed);
        int frac      = calls   ? (int)((silent  * 100) / calls)   : 0;
        int smp_frac  = tot_smp ? (int)((sil_smp * 100) / tot_smp) : 0;
        VP_SILENT_LOG("calls:%llu silent:%llu tot_smp:%llu silent_smp:%llu "
                      "frac:%d%% smp_frac:%d%% ea0:%llu allvolmax:%llu "
                      "skipped:%llu",
                      (unsigned long long)calls,
                      (unsigned long long)silent,
                      (unsigned long long)tot_smp,
                      (unsigned long long)sil_smp,
                      frac, smp_frac,
                      (unsigned long long)ea0,
                      (unsigned long long)allvol,
                      (unsigned long long)skipped);
        memset(&g_vp_silent_stats, 0, sizeof(g_vp_silent_stats));
    }
}

#else /* !PERF_LOG */
void vp_silent_stats_log_and_reset(void) {}
#endif /* PERF_LOG */

static const struct {
    uint64_t top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, // 2D
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, // 3D
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, // MP
};

static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    uint64_t notify_offset = d->regs[NV_PAPU_FENADDR];
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15; // Final byte is status, same for all notifiers

    // FIXME: Check notify enable
    // FIXME: Set NV1BA0_NOTIFICATION_STATUS_IN_PROGRESS when appropriate
    apu_ram_st8(notify_offset, status);

    // FIXME: Refactor this out of here
    // FIXME: Actually provied current envelope state
    apu_ram_st8(notify_offset - 1, 1);

    apu_or(&d->regs[NV_PAPU_ISTS],
              NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    apu_st(&d->set_irq, true);
}

/* A voice's processing state as a new voice's (VOICE_ON): filters and
 * resampler empty, its first frame fading in from silence at its own pitch,
 * frequencies and HRTF entry, its LFOs taking their delays again. */
static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    MCPXAPUVoiceFilter *vf = &d->vp.filters[v];
    memset(vf->svf, 0, sizeof(vf->svf));
    memset(vf->peq, 0, sizeof(vf->peq));
    memset(vf->i3dl2, 0, sizeof(vf->i3dl2));
    memset(vf->interp, 0, sizeof(vf->interp));
    vf->interp_idx = 0;
    /* five input samples read before the first output, so the voice's first
     * sample is the interpolator's centre (interp[3]) */
    vf->interp_pos = 5 << 16;
    vf->resample_buf_pos = NUM_SAMPLES_PER_FRAME;
    memset(vf->route_gain, 0, sizeof(vf->route_gain));
    vf->gains = false;
    vf->pitched = false;
    vf->dls2_set = false;
    vf->peq_set = false;
    memset(vf->lfo_phase, 0, sizeof(vf->lfo_phase));
    memset(vf->lfo_wait, 0, sizeof(vf->lfo_wait));
    vf->lfo_armed = false;
    vf->hrtf.params_set = false;
    hrtf_filter_clear_history(&vf->hrtf);
}

/* Filter coefficients take 2^(x / 4096) to a 1024th of an octave, then to
 * the nearest 1/65536, at most FFFFh (measured: D97Eh gave 3043h, which
 * reproduces all 32 samples of the ParaEQ's response there where the exact
 * exponential missed 30; FC0 0 passed 1 - 2^-15 of an impulse on the first
 * sample, so f is FFFFh / 65536, not 1). g_octave_frac holds the 1024
 * steps of an octave, 2^(j / 1024) * 65536. */
static double g_octave_frac[1024];

static void filter_coefficient_init(void)
{
    for (int j = 0; j < 1024; j++) {
        g_octave_frac[j] = exp2(j / 1024.0) * 65536.0;
    }
}

static float filter_coefficient(double x)
{
    int k = (int)floor(x / 4.0);
    int octave = (int)floor(k / 1024.0);
    double c = ldexp(g_octave_frac[k - octave * 1024], octave);
    return fmin(round(c), 65535.0) / 65536.0;
}

/* Route gains in 1/16384, attenuations in 1/64 dB (measured): straight lines
 * between whole decibels through round(4095 * 10^(-k / 20)), rounded down;
 * from 48 dB a straight line to nothing at 63 dB, so FC0h and above are
 * silent. Attenuation 0 is unity. A retail console's gains at 37
 * attenuations from 001h to FFDh match this to the 1/16384: F00h gave
 * 12/16384, F80h 4 and FB0h 1, where 10^(-att / 1280) gives 16, 13 and 12. */
static uint32_t attenuation_nodes[64];

static void attenuation_init(void)
{
    for (int k = 0; k <= 48; k++) {
        attenuation_nodes[k] = floor(4095.0 * pow(10.0, -k / 20.0) + 0.5);
    }
    for (int k = 49; k < 64; k++) {
        attenuation_nodes[k] = attenuation_nodes[48] * (63 - k) / 15;
    }
}

static uint32_t attenuation_gain(int att)
{
    if (att <= 0) {
        return 16384;
    }
    if (att >= 63 * 64) {
        return 0;
    }
    uint32_t n0 = attenuation_nodes[att >> 6];
    uint32_t n1 = attenuation_nodes[(att >> 6) + 1];
    return (256 * n0 - 4 * (n0 - n1) * (att & 63)) >> 6;
}

/* Hardware resamples with 8-point Lagrange interpolation, each weight rounded
 * to 1/65536. A retail console's response to an impulse at half a sample
 * (pitch -1000h) is exactly that kernel's, 1225, -245, 49 and -5 / 2048 on
 * each side, and its responses at each eighth of a sample match the weights
 * rounded to 1/65536 to the bit. Between 256ths of a sample the weights are
 * interpolated (assumed). Row r holds the weights of input samples -3..4
 * around the cursor at r / 256 of a sample past sample 0. */
static float lagrange_rows[257][8];

static void lagrange_init(void)
{
    for (int r = 0; r <= 256; r++) {
        for (int k = -3; k <= 4; k++) {
            double w = 1.0;
            for (int j = -3; j <= 4; j++) {
                if (j != k) {
                    w *= (r / 256.0 - j) / (k - j);
                }
            }
            lagrange_rows[r][k + 3] = round(w * 65536.0) / 65536.0;
        }
    }
}

/* A table's pointer from the guest's base: NULL with no base yet or without
 * room for all of it (the accesses then go by guest address, each checked).
 * The old pointer stays valid for a worker still using it: guest RAM is one
 * mapping for the run. */
static void refresh_table(uint8_t **table, uint32_t base, uint32_t size)
{
    apu_st_rel(table, base ? apu_ram_ptr(base, size) : NULL);
}

void mcpx_apu_vp_refresh_voice_cache(MCPXAPUState *d, uint32_t new_base)
{
    refresh_table(&d->vp.voice_cache, new_base, MCPX_HW_MAX_VOICES * NV_PAVS_SIZE);
}

void mcpx_apu_vp_refresh_ssl_cache(MCPXAPUState *d, uint32_t new_base)
{
    refresh_table(&d->vp.ssl_cache, new_base, MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE);
}

/* The table's pointer covers its MCPX_HW_MAX_VOICES voices; a handle past
 * them (the guest's: a 16-bit field) goes by guest address, checked. */
static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               uint64_t offset, uint32_t mask)
{
    uint32_t raw;
    uint8_t *c = apu_ld_acq(&d->vp.voice_cache);
    if (likely(c && voice_handle < MCPX_HW_MAX_VOICES)) {
        raw = apu_mem_ld32(c, voice_handle * NV_PAVS_SIZE + offset);
    } else {
        uint64_t voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
        raw = apu_ram_ld32(voice + offset);
    }
    return (raw & mask) >> ctz32(mask);
}

static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           uint64_t offset, uint32_t mask, uint32_t val)
{
    uint8_t *c = apu_ld_acq(&d->vp.voice_cache);
    if (likely(c && voice_handle < MCPX_HW_MAX_VOICES)) {
        uint64_t coff = voice_handle * NV_PAVS_SIZE + offset;
        uint32_t v = apu_mem_ld32(c, coff) & ~mask;
        apu_mem_st32(c, coff,
                           v | ((val << ctz32(mask)) & mask));
    } else {
        uint64_t voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
        uint32_t v = apu_ram_ld32(voice + offset) & ~mask;
        apu_ram_st32(voice + offset,
                    v | ((val << ctz32(mask)) & mask));
    }
}

/*
 * _nolock variants: the voice table `c` loaded once by the caller (NULL is
 * legal; the fallback path handles it), along the hot voice-frame path
 * (mcpx_apu_vp_frame / voice_worker_thread / voice_process). Cold MMIO
 * callers use voice_get_mask / voice_set_mask, which load it per call.
 */
static inline uint32_t voice_get_mask_nolock(MCPXAPUState *d, uint8_t *c,
                                             uint16_t voice_handle,
                                             uint64_t offset, uint32_t mask)
{
    uint32_t raw;
    if (likely(c && voice_handle < MCPX_HW_MAX_VOICES)) {
        raw = apu_mem_ld32(c, voice_handle * NV_PAVS_SIZE + offset);
    } else {
        uint64_t voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
        raw = apu_ram_ld32(voice + offset);
    }
    return (raw & mask) >> ctz32(mask);
}

#if HAKUX_AUDIO_SKIP_MUTED
/* True iff every route's target gain is ZERO THIS frame, i.e. all 8 mixbin
 * volumes are 0xFFF (silent even under the amplitude LFO; voice_process also
 * needs the last frame's gains at zero, as they ramp). This is the hoisted
 * form of the vol[] reads at the bottom of voice_process (the VOLA/VOLB/VOLC
 * packing, including the split vol[6]/vol[7] nibble assembly): it MUST stay
 * bit-identical to those reads. Read here BEFORE the resample block so the
 * decode / SVF / HRTF / mix can be skipped. A divergence either drops an
 * audible voice (dangerous) or fails to skip a silent one (benign no-win). */
static inline bool voice_all_vols_maxed(MCPXAPUState *d, uint8_t *c,
                                        uint16_t v)
{
    uint16_t vol[8];
    vol[0] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                            NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                            NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                            NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                            NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME5);

    vol[6] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8;
    vol[6] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                             NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4;
    vol[6] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                             NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8;
    vol[7] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                             NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4;
    vol[7] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                             NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        if ((vol[i] & 0xFFF) != 0xFFF) { /* the route is audible */
            return false;
        }
    }
    return true;
}
#endif /* HAKUX_AUDIO_SKIP_MUTED */

static inline void voice_set_mask_nolock(MCPXAPUState *d, uint8_t *c,
                                         uint16_t voice_handle, uint64_t offset,
                                         uint32_t mask, uint32_t val)
{
    if (likely(c && voice_handle < MCPX_HW_MAX_VOICES)) {
        uint64_t coff = voice_handle * NV_PAVS_SIZE + offset;
        uint32_t v = apu_mem_ld32(c, coff) & ~mask;
        v |= (val << ctz32(mask)) & mask;
        apu_wr32(c + coff, v);                  /* reported with the batch */
        d->vp.voice_wrote[voice_handle] = 1;
    } else {
        uint64_t voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
        uint32_t v = apu_ram_ld32(voice + offset) & ~mask;
        apu_ram_st32(voice + offset,
                    v | ((val << ctz32(mask)) & mask));
    }
}

/* The voices the batch wrote, to the HLE's write tracking: one report per
 * run of neighbouring voices (the frame thread, after the workers). */
static void report_written_voices(MCPXAPUState *d, uint8_t *c)
{
    uint8_t *w = d->vp.voice_wrote;
    const uint32_t base = (uint32_t)(c - g_apu_ram);
    for (int v = 0; v < MCPX_HW_MAX_VOICES;) {
        uint64_t word;
        if (!(v & 7)) {
            memcpy(&word, w + v, 8);
            if (!word) {
                v += 8;
                continue;
            }
        }
        if (!w[v]) {
            v++;
            continue;
        }
        int e = v;
        while (e < MCPX_HW_MAX_VOICES && w[e]) {
            w[e++] = 0;
        }
        hle_host_mem_set_dirty(base + v * NV_PAVS_SIZE, (e - v) * NV_PAVS_SIZE);
        v = e;
    }
}

/* The guest's VOICE_OFFs whose notification waits for the voice's release
 * (voice_off): a bit per voice, its notifier and the frames it has waited.
 * Not part of a savestate (cleared by the reset before one loads). */
static uint64_t g_off_pending[MCPX_HW_MAX_VOICES / 64];
static uint8_t g_off_notifier[MCPX_HW_MAX_VOICES];
static uint8_t g_off_frames[MCPX_HW_MAX_VOICES];
#define OFF_NOTIFY_MAX_FRAMES 64        /* ~43 ms of frames: then it goes anyway */

static void off_notify_now(MCPXAPUState *d, uint16_t v)
{
    const uint64_t bit = 1ull << (v % 64);
    if (__atomic_fetch_and(&g_off_pending[v / 64], ~bit, __ATOMIC_ACQ_REL) & bit) {
        set_notify_status(d, v, g_off_notifier[v], NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
    }
}

/* The frame thread stopped for the guest (the FE trapped or halted, the SE
 * off) and the guest didn't act: the VOICE_OFFs still waiting are notified,
 * no frame coming to walk the lists or count their 64. True when one was. */
bool mcpx_apu_vp_off_notify_all(MCPXAPUState *d)
{
    bool any = false;
    for (unsigned w = 0; w < MCPX_HW_MAX_VOICES / 64; w++) {
        for (uint64_t p = __atomic_load_n(&g_off_pending[w], __ATOMIC_ACQUIRE); p; p &= p - 1) {
            off_notify_now(d, (uint16_t)(w * 64 + __builtin_ctzll(p)));
            any = true;
        }
    }
    return any;
}

/* The idle voice the SE reported last (SE2FE_IDLE_VOICE, idle_report),
 * 0xFFFF: none. Not part of a savestate (cleared by the reset before one
 * loads). */
static uint16_t g_idle_last = 0xFFFF;

bool mcpx_apu_vp_off_pending(void)
{
    for (unsigned w = 0; w < MCPX_HW_MAX_VOICES / 64; w++) {
        if (__atomic_load_n(&g_off_pending[w], __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
    return false;
}

/* The guest turns a voice off (NV1BA0_PIO_VOICE_OFF). On the hardware the
 * SE finds it idle in its next pass and reports both together: the
 * SE2FE_IDLE_VOICE trap, whose handler releases the voice, and its done
 * notification. Posted here at once, the notification's interrupt ran
 * DirectSound's voice service DPC up to a frame (and the frame thread's
 * throttle) before the trap: a stream voice it restarted there waited for a
 * release the ISR defers while that DPC runs (Wreckless hung in menus and
 * in missions, its DPC spinning at 0x12CD7B). When the title takes idle
 * traps, the notification waits for the voice's idle trap and comes with it,
 * as on the hardware (mcpx_apu_vp_frame), or until the voice has left the
 * lists, is turned on again, or 64 frames (or their time, the frame thread
 * stopped for the guest). Waiting past the trap, until the trap's DPC had
 * released the voice, Blood Wake hung loading a mission: its DirectSound
 * turns a voice off at DISPATCH_LEVEL and spins at 0xFE047 until its ISR
 * has seen the notification, the DPC kept out, the trap stopping the frames
 * that would have counted the 64. */
static void voice_off(MCPXAPUState *d, uint16_t v)
{
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
        off_notify_now(d, v);           /* an earlier one first */
        g_off_notifier[v] = (uint8_t)notifier;
        g_off_frames[v] = 0;
        __atomic_fetch_or(&g_off_pending[v / 64], 1ull << (v % 64), __ATOMIC_ACQ_REL);
        return;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

/* _nolock variant of voice_off — the caller has the voice table. Used from voice_step_envelope /
 * voice_get_samples on the worker hot path. */
static void voice_off_nolock(MCPXAPUState *d, uint8_t *c, uint16_t v)
{
    voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                          NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

/* vp_write: the guest CPU's register write is being applied */
static __thread bool t_guest_write;

/* A voice's spinlock. The VP's batch holds it while the voice workers
 * process the frame: 0.2 ms on average, but now and then 4-40 ms (a worker
 * preempted). The guest's DirectSound waited for it in a register write,
 * inside one MMIO access, its interrupts held off all that time: Forza's
 * vblank came 20-30 ms late and its frame a vblank late, a few times a
 * minute in a race. In the native core such a write gives up after 0.2 ms
 * and the guest's store runs again (nc_mmio_retry): it waits as long, but
 * from instruction to instruction, the interrupts taken in between. Only
 * where nothing was changed yet (may_retry), and where the access can run
 * again (an MMIO site's; a faulting access waits here as before). */
static bool voice_spin_lock(MCPXAPUState *d, uint16_t v, bool may_retry)
{
    if (may_retry && t_guest_write) {
        if (apu_spin_trylock(&d->vp.voice_spinlocks[v])) {
            return true;
        }
        const int64_t t0 = apu_host_us();
        for (unsigned i = 1;; i++) {
#if defined(__aarch64__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
            if (!__atomic_load_n(&d->vp.voice_spinlocks[v].v, __ATOMIC_RELAXED) &&
                apu_spin_trylock(&d->vp.voice_spinlocks[v])) {
                return true;
            }
            if (!(i & 63) && apu_host_us() - t0 >= 200) {
                if (nc_mmio_retry()) {
                    return false;
                }
                break;
            }
        }
    }
    apu_spin_lock(&d->vp.voice_spinlocks[v]);
    return true;
}

/* false: not taken, the guest's write runs again (voice_spin_lock); a
 * handle past the voices is no voice: the write is dropped (done) */
static bool voice_lock(MCPXAPUState *d, uint16_t v, bool lock, bool may_retry)
{
    if (!APU_CHECK(v < MCPX_HW_MAX_VOICES)) {
        return true;
    }
    if (!voice_spin_lock(d, v, may_retry)) {
        return false;
    }
    uint64_t mask = 1LL << (v % 64);
    if (lock) {
        d->vp.voice_locked[v / 64] |= mask;
    } else {
        d->vp.voice_locked[v / 64] &= ~mask;
    }
    apu_spin_unlock(&d->vp.voice_spinlocks[v]);
    return true;
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    if (v >= MCPX_HW_MAX_VOICES) {
        return false;
    }
    uint64_t mask = 1LL << (v % 64);
    return (apu_ld(&d->vp.voice_locked[v / 64]) & mask) != 0;
}

/* An HRIR tap is a sign-magnitude minifloat: bit 7 the sign, a 3-bit
 * exponent and a 4-bit mantissa, (16 + m) * 2^(e - 1) / 2048, or m / 2048
 * when the exponent is 0, from 1/2048 to 31/32. Measured on a retail console
 * with DC through single taps: 01h gave 1/2048, 20h 1/64, 40h 1/16 and 7Fh
 * 31/32, 81h and C0h the negatives of 01h and 40h. Read as two's complement
 * / 128, 40h was 0.5, eight times too loud, and small negative taps (81h)
 * became -127/128, which turned DirectSound's HRIRs to noise. */
static float hrir_tap_to_float(uint8_t tap)
{
    int e = (tap >> 4) & 7;
    int m = tap & 15;
    float v = (e ? (16 + m) << (e - 1) : m) / 2048.0f;
    return (tap & 0x80) ? -v : v;
}

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               uint8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] = hrir_tap_to_float(value);
    d->vp.hrir_sm = 2;
}

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;


    //assert((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) == 0);

    /* The decoded method and argument are a trap's while the FE is trapped:
     * its handler reads them, and the hardware FE takes no other method
     * until the handler clears the trap. Overwritten by the guest's next
     * write (VOICE_LOCK 1 between the VP's IDLE_VOICE trap and its
     * handler), PGR2's DirectSound read the trap's method with that
     * argument, a voice handle of 1 it had no voice for, and dereferenced a
     * NULL voice (guest address 0x64: the process died). */
    if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) != NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        d->regs[NV_PAPU_FEDECMETH] = method;
        d->regs[NV_PAPU_FEDECPARAM] = argument;
    }
    unsigned int selected_handle, list;
    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, d->regs[NV_PAPU_FECV], argument & 1, true);
        break;
    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;
    case NV1BA0_PIO_VOICE_ON: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        DPRINTF("VOICE %d ON\n", selected_handle);
        if (!APU_CHECK(selected_handle < MCPX_HW_MAX_VOICES)) {
            break;
        }

        off_notify_now(d, selected_handle);    /* its last VOICE_OFF's, before it plays again */

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            /* voice is added to the top of the selected list */
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            /* voice is added after the antecedent voice */
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(
                d, antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           next_handle);
            voice_set_mask(d, antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           selected_handle);
        }

        // FIXME: Should set CBO here?
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0; // FIXME: verify this
        d->vp.ssl[selected_handle].ssl_index = 0; // FIXME: verify this

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                voice_get_mask(d, selected_handle, NV_PAVS_VOICE_CFG_ENV0,
                               NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK ||
                   ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY) {
            /* attack and decay count up from zero when a voice starts in
             * them (measured) */
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                voice_get_mask(d, selected_handle, NV_PAVS_VOICE_CFG_ENVA,
                               NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }
        // FIXME: Will count be overwritten in other cases too?

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                voice_get_mask(d, selected_handle, NV_PAVS_VOICE_CFG_ENV1,
                               NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK ||
                   ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY) {
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                voice_get_mask(d, selected_handle, NV_PAVS_VOICE_CFG_ENVF,
                               NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }
        // FIXME: Will count be overwritten in other cases too?

        voice_reset_filters(d, selected_handle);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);
        break;
    }
    case NV1BA0_PIO_VOICE_RELEASE: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        if (!APU_CHECK(selected_handle < MCPX_HW_MAX_VOICES)) {
            break;
        }

        // FIXME: What if already in release? Restart envelope?
        // FIXME: Should release count ascend or descend?

        uint16_t rr;
        rr = voice_get_mask(d, selected_handle, NV_PAVS_VOICE_TAR_LFO_ENV,
                            NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = voice_get_mask(d, selected_handle, NV_PAVS_VOICE_CFG_MISC,
                            NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);
        break;
    }
    case NV1BA0_PIO_VOICE_OFF:
        if (APU_CHECK((argument & NV1BA0_PIO_VOICE_OFF_HANDLE) < MCPX_HW_MAX_VOICES)) {
            voice_off(d, argument & NV1BA0_PIO_VOICE_OFF_HANDLE);
        }
        break;
    case NV1BA0_PIO_VOICE_PAUSE:
        voice_set_mask(d, argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE,
                       NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;
    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        d->vp.hrtf.current_entry = handle;
        break;
    }
    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_VBIN,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_FMT,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_ENV0,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_ENVA,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_ENV1,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_ENVF,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_MISC,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        unsigned int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        uint32_t current_voice = d->regs[NV_PAPU_FECV];
        /* the voice reads its entry each frame (voice_process), so a new
         * handle or a rewritten entry takes effect on the next frame */
        voice_set_mask(d, current_voice, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_VOLA,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_VOLB,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_VOLC,
                       0xFFFFFFFF, argument);
        break;
    /* DirectSound's SetLFO writes the delays (50h) and depths (70h) with
     * methods 350h and 370h: like the other voice methods, the record offset
     * plus 300h */
    case NV1BA0_PIO_SET_VOICE_LFO_DELAY:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_PAR_LFO_DELAY,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_LFO_ENV,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_MOD:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_LFO_MOD,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_FCA,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_FCB,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CUR_PSL_START,
                       NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;
    case NV1BA0_PIO_SET_HRIR ... NV1BA0_PIO_SET_HRIR_X - 1: {
        if (!APU_CHECK((unsigned)d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT)) {
            break;
        }
        slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
        uint8_t left0 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
        uint8_t right0 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
        uint8_t left1 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
        uint8_t right1 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
        int coeff_idx = slot * 2;
        set_hrir_coeff_tar(d, 0, coeff_idx, left0);
        set_hrir_coeff_tar(d, 1, coeff_idx, right0);
        coeff_idx += 1;
        set_hrir_coeff_tar(d, 0, coeff_idx, left1);
        set_hrir_coeff_tar(d, 1, coeff_idx, right1);
        break;
    }
    case NV1BA0_PIO_SET_HRIR_X: {
        if (!APU_CHECK((unsigned)d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT)) {
            break;
        }
        uint8_t left30 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
        uint8_t right30 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
        int16_t itd = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
        set_hrir_coeff_tar(d, 0, 30, left30);
        set_hrir_coeff_tar(d, 1, 30, right30);
        d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
        break;
    }
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        // FIXME: Is there an upper limit for the SGE table size?
        // FIXME: NV_PAPU_VPSGEADDR is probably bad, as outbuf SGE use the same
        // handle range (or that is also wrong)
        uint64_t sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        apu_ram_st32(sge_address,
                    argument &
                        NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        DPRINTF("Wrote inbuf SGE[0x%X] = 0x%08X\n", d->vp.inbuf_sge_handle,
                argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }
    CASE_4(NV1BA0_PIO_SET_OUTBUF_BA, 8): // 8 byte pitch, 4 entries
#ifdef DEBUG_MCPX
        slot = (method - NV1BA0_PIO_SET_OUTBUF_BA) / 8;
        //FIXME: Use NV1BA0_PIO_SET_OUTBUF_BA_ADDRESS = 0x007FFF00 ?
        DPRINTF("outbuf_ba[%d]: 0x%08X\n", slot, argument);
#endif
        //assert(false); //FIXME: Enable assert! no idea what this reg does
        break;
    CASE_4(NV1BA0_PIO_SET_OUTBUF_LEN, 8): // 8 byte pitch, 4 entries
#ifdef DEBUG_MCPX
        slot = (method - NV1BA0_PIO_SET_OUTBUF_LEN) / 8;
        //FIXME: Use NV1BA0_PIO_SET_OUTBUF_LEN_VALUE = 0x007FFF00 ?
        DPRINTF("outbuf_len[%d]: 0x%08X\n", slot, argument);
#endif
        //assert(false); //FIXME: Enable assert! no idea what this reg does
        break;
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        /* Output buffer SGEs are not stored in the voices' input SGE table:
         * on a retail console a voice whose samples are in input SGE entry 0
         * kept playing them after the GP's output buffer SGEs 0 and 1 were
         * set, as DirectSound and the kernel do when they start the GP.
         * Written there, the entry took the GP FIFO's page and the voice
         * played silence. Output buffers are not emulated, so the pages are
         * not kept. */
        // NV_PAPU_EPFADDR   EP outbufs
        // NV_PAPU_GPFADDR   GP outbufs
        DPRINTF("Wrote outbuf SGE[0x%X] = 0x%08X\n", d->vp.outbuf_sge_handle,
                argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        int ssl = 0;
        uint32_t current_voice = d->regs[NV_PAPU_FECV];
        if (!APU_CHECK(current_voice < MCPX_HW_MAX_VOICES)) {
            break;
        }
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        // d->vp.ssl[current_voice].ssl_index = 0;
        DPRINTF("SSL%c Base = %x, Count = %d\n", 'A' + ssl,
                d->vp.ssl[current_voice].base[ssl],
                d->vp.ssl[current_voice].count[ssl]);
        break;
    }
    // FIXME: Refactor into above
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        int ssl = 1;
        uint32_t current_voice = d->regs[NV_PAPU_FECV];
        if (!APU_CHECK(current_voice < MCPX_HW_MAX_VOICES)) {
            break;
        }
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        // d->vp.ssl[current_voice].ssl_index = 0;
        DPRINTF("SSL%c Base = %x, Count = %d\n", 'A' + ssl,
                d->vp.ssl[current_voice].base[ssl],
                d->vp.ssl[current_voice].count[ssl]);
        break;
    }
    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS*NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }
    case NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET ...
         NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH+8*64-1: {
        // 64 offset/base pairs relative to segment base
        // FIXME: Entries are 64b, assuming they are stored
        // like this <[offset,length],...>
        assert((method & 0x3) == 0);
        uint64_t addr = d->regs[NV_PAPU_VPSSLADDR]
                      + (d->vp.ssl_base_page * 8)
                      + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
        apu_ram_st32(addr, argument);
        DPRINTF("  ssl_segment[%x + %x].%s = %x\n",
            d->vp.ssl_base_page,
            (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET)/8,
            method & 4 ? "length" : "offset",
            argument);
        break;
    }
    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (argument >>  0) & 0x1f;
        d->vp.hrtf_submix[1] = (argument >>  8) & 0x1f;
        d->vp.hrtf_submix[2] = (argument >> 16) & 0x1f;
        d->vp.hrtf_submix[3] = (argument >> 24) & 0x1f;
        break;
    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        d->vp.hrtf_headroom = argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT;
        break;
    case NV1BA0_PIO_SET_SUBMIX_HEADROOM ...
         NV1BA0_PIO_SET_SUBMIX_HEADROOM+4*(NUM_MIXBINS-1):
        assert((method & 3) == 0);
        slot = (method-NV1BA0_PIO_SET_SUBMIX_HEADROOM)/4;
        d->vp.submix_headroom[slot] =
            argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT;
        break;
    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            DPRINTF("idle voice %d\n", argument);
            apu_st(&d->set_irq, true);
        } else {
            /* Without the forced trap an idle voice is skipped: on hardware
             * an inactive voice can stay in a list with FETFORCE1's
             * SE2FE_IDLE_VOICE clear, and the rest of the list keeps
             * mixing. */
            DPRINTF("ignoring idle voice %d without FETFORCE1 request\n",
                    argument);
        }
        break;
    default:
        DPRINTF("fe_method: unknown method 0x%x\n", method);
        break;
    }
}

/* The voice whose state a guest method changes that the VP's frame changes
 * too (the envelope's state and counts, the play offset), 0xFFFF for none:
 * the other methods set fields only the guest writes. */
static uint16_t method_voice(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    switch (method) {
    case NV1BA0_PIO_VOICE_ON:
    case NV1BA0_PIO_VOICE_RELEASE:
        return argument & NV1BA0_PIO_VOICE_ON_HANDLE;
    case NV1BA0_PIO_VOICE_OFF:
        return argument & NV1BA0_PIO_VOICE_OFF_HANDLE;
    case NV1BA0_PIO_VOICE_PAUSE:
        return argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        return (uint16_t)d->regs[NV_PAPU_FECV];
    default:
        return 0xFFFF;
    }
}

uint64_t mcpx_apu_vp_read(void *opaque, uint32_t addr, unsigned int size)
{
    DPRINTF("mcpx apu VP: read [0x%" PRIx64 "] (%s)\n", addr,
            get_method_str(addr));

    switch (addr) {
    case NV1BA0_PIO_FREE:
        /* we don't simulate the queue for now,
         * pretend to always be empty */
        return 0x80;
    default:
        break;
    }

    return 0;
}

void mcpx_apu_vp_write(void *opaque, uint32_t addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    DPRINTF("mcpx apu VP: [0x%" PRIx64 "] %s = 0x%lx\n", addr,
            get_method_str(addr), val);

    switch (addr) {
    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
    case NV1BA0_PIO_VOICE_LOCK:
    case NV1BA0_PIO_VOICE_ON:
    case NV1BA0_PIO_VOICE_RELEASE:
    case NV1BA0_PIO_VOICE_OFF:
    case NV1BA0_PIO_VOICE_PAUSE:
    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
    case NV1BA0_PIO_SET_CURRENT_VOICE:
    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
    case NV1BA0_PIO_SET_VOICE_TAR_HRTF:
    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
    case NV1BA0_PIO_SET_VOICE_LFO_DELAY:
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
    case NV1BA0_PIO_SET_VOICE_LFO_MOD:
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
    case NV1BA0_PIO_SET_HRIR ... NV1BA0_PIO_SET_HRIR_X - 1:
    case NV1BA0_PIO_SET_HRIR_X:
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET:
    CASE_4(NV1BA0_PIO_SET_OUTBUF_BA, 8): // 8 byte pitch, 4 entries
    CASE_4(NV1BA0_PIO_SET_OUTBUF_LEN, 8): // 8 byte pitch, 4 entries
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET:
    case NV1BA0_PIO_SET_CURRENT_SSL:
    case NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET ...
         NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH+8*64-1:
    case NV1BA0_PIO_SET_VOICE_SSL_A:
    case NV1BA0_PIO_SET_VOICE_SSL_B:
    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
    case NV1BA0_PIO_SET_HRTF_HEADROOM:
    case NV1BA0_PIO_SET_SUBMIX_HEADROOM ...
         NV1BA0_PIO_SET_SUBMIX_HEADROOM+4*(NUM_MIXBINS-1):
        /* TODO: these should instead be queueing up fe commands */
        apu_spin_lock(&d->vp.fe_spin);     /* against the VP's traps */
        if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) ==
            NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
            /* the trap's method and argument stay for its handler (fe_method) */
            static unsigned s_kept;
            if (s_kept++ < 16) {
                nc_log("hakuX-apu", "method %04x (%08x) while trapped: the trap's %04x (%08x) kept",
                                    (unsigned)addr, (unsigned)val, d->regs[NV_PAPU_FEDECMETH],
                                    d->regs[NV_PAPU_FEDECPARAM]);
            }
        }
        t_guest_write = true;
        /* A voice whose state the frame changes too is the guest's while the
         * method runs, as its own VOICE_LOCK makes it: the frame leaves it
         * for the next (voice_take), and a batch processing it is waited for
         * first, the store run again if that takes long (voice_spin_lock).
         * Unguarded, the guest's VOICE_OFF and a worker's envelope update
         * read-modify-wrote the same word and one store was lost: a voice
         * turned off played on, never reported idle, its release waited for
         * for good. */
        const uint16_t v = method_voice(d, addr, val);
        const bool guard = v < MCPX_HW_MAX_VOICES && !is_voice_locked(d, v);
        if (!guard || voice_lock(d, v, true, true)) {
            if (guard) {
                /* the voice held before the method's stores (VOICE_ON's list
                 * insert) are seen: the unlock's release orders only the
                 * stores before it */
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
            }
            fe_method(d, addr, val);
            if (guard) {
                voice_lock(d, v, false, false);
            }
        }
        t_guest_write = false;
        apu_spin_unlock(&d->vp.fe_spin);
        break;

    case NV1BA0_PIO_GET_VOICE_POSITION:
    case NV1BA0_PIO_SET_CONTEXT_DMA_NOTIFY:
    case NV1BA0_PIO_SET_CURRENT_SSL_CONTEXT_DMA:
        DPRINTF("unhandled method: %" PRIx64 " = %" PRIx64 "\n", addr,
                val);
        assert(0);
    default:
        break;
    }
}

/* hakuX: VP addresses are guest controlled. With AHB guest RAM an
 * out-of-range read faults instead of reading adjacent heap (Phase 0
 * finding); clamp and count. */
uint32_t g_vp_oob_clamps;

static inline uint64_t vp_ram_addr(MCPXAPUState *d, uint64_t addr, size_t n)
{
    if (addr + n <= d->ram_size) {
        return addr;
    }
    apu_inc(&g_vp_oob_clamps);
    return 0;
}

static uint64_t get_data_ptr(MCPXAPUState *d, uint64_t sge_base,
                           unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / APU_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        apu_rd32(&d->ram_ptr[vp_ram_addr(d, sge_base + entry * 8, 4)]);
    DPRINTF("Addr: 0x%08X\n", prd_address);
    return prd_address + addr % APU_PAGE_SIZE;
}

static float envelope_level(MCPXAPUState *d, uint8_t *c, uint16_t v,
                            uint32_t lvl_reg, uint32_t lvl_mask, double units)
{
    voice_set_mask_nolock(d, c, v, lvl_reg, lvl_mask, MIN(255, (int)units / 8));
    return units / 2048.0;
}

/* Steps an envelope by one frame and returns its level. Measured on a retail
 * console with a DC voice read back every frame:
 * - Each phase steps before its frame plays: attack rate 2 gives 1/32 on its
 *   first frame and full on its 32nd (2048ths in R * 16 frames). Delay and
 *   hold last 16T + 1 frames.
 * - Decay falls by e^(-1/2R) a frame from full to the sustain level, s/255
 *   (in 2048ths, rounded down). A decay rate of 0 holds one more frame at
 *   full, then sustains.
 * - Release holds the level byte, as 256ths, for its first frame, then falls
 *   by e^(-1/2R) a frame. From 11R frames on it runs down in a straight line
 *   to nothing at 15R, when the envelope ends.
 * - The F envelope reads 0 while off; the amplitude envelope reads full. */
static float voice_step_envelope(MCPXAPUState *d, uint8_t *c, uint16_t v,
                           uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    uint32_t count = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
    uint32_t attack_rate =
        voice_get_mask_nolock(d, c, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);
    uint32_t hold_time =
        voice_get_mask_nolock(d, c, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
    uint32_t decay_rate =
        voice_get_mask_nolock(d, c, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
    uint32_t sustain_level =
        voice_get_mask_nolock(d, c, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
    uint32_t release_rate = voice_get_mask_nolock(d, c, v, rr_reg, rr_mask);
    double sustain = floor(sustain_level * 2048.0 / 255.0);
    bool amplitude = count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT;

    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        return envelope_level(d, c, v, lvl_reg, lvl_mask, amplitude ? 2048 : 0);
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY:
        if (count == 0) {
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur + 1);
        } else {
            count--;
        }
        voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return envelope_level(d, c, v, lvl_reg, lvl_mask, 0);
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        count = MIN(count + 1, 0xFFFF);
        double units = 2048;
        if (attack_rate != 0) {
            units = MIN(2048, floor(2048.0 * count / (attack_rate * 16)));
        }
        if (attack_rate == 0 || count >= attack_rate * 16) {
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur + 1);
            count = hold_time * 16;
        }
        voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return envelope_level(d, c, v, lvl_reg, lvl_mask, units);
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD:
        if (count == 0) {
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur + 1);
        } else {
            count--;
        }
        voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return envelope_level(d, c, v, lvl_reg, lvl_mask, 2048);
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        double units = 2048;
        if (decay_rate != 0) {
            count = MIN(count + 1, 0xFFFF);
            units = floor(2048.0 * exp(-(double)count / (2 * decay_rate)));
        }
        if (decay_rate == 0 || units <= sustain) {
            units = decay_rate == 0 ? 2048 : sustain;
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur + 1);
            count = 0;
        }
        voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return envelope_level(d, c, v, lvl_reg, lvl_mask, units);
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN:
        voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask_nolock(d, c, v, lvl_reg, lvl_mask, sustain_level);
        return sustain / 2048.0;
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        /* The count runs down from R * 16 (VOICE_RELEASE) */
        uint32_t t = release_rate * 16 > count ? release_rate * 16 - count : 0;
        if (release_rate == 0 || t >= release_rate * 15) {
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur + 1);
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
            return 0.0f;
        }
        voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask,
                              count ? count - 1 : 0);
        double start = voice_get_mask_nolock(d, c, v, lvl_reg, lvl_mask) * 8.0;
        double fall = floor(start * exp(-(double)t / (2 * release_rate)));
        double tail = floor(start * 2.0 * (release_rate * 15 - t) /
                                (2048.0 * release_rate) +
                            0.5);
        return (t < release_rate * 11 ? MAX(fall, tail) : tail) / 2048.0;
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (amplitude) {
            voice_off_nolock(d, c, v);
        }
        return 0.0f;
    default:
        fprintf(stderr, "Unknown envelope state 0x%x\n", cur);
        assert(false);
        return 0.0f;
    }
}

/* Steps the voice's two LFOs by one frame. A modulates amplitude, pitch and
 * cutoff, F modulates pitch. The fields are as Xbox DirectSound's SetLFO
 * packs them: TAR_LFO_ENV holds A's delta in bits 21:12 and F's in 31:22,
 * TAR_LFO_MOD (70h) the signed depths (A amplitude, A pitch, A cutoff,
 * F pitch, a byte each), PAR_LFO_DELAY (50h) the delays (A 14:0, F 30:16),
 * enabled by MISC bits 14 and 15. Measured on a retail console: each LFO's
 * 16-bit phase advances by its delta each frame, after its delay in frames,
 * as a bipolar triangle that rises first. At full value a unit of depth is
 * 8 attenuation units (1/8 dB), 32 pitch units or 256 cutoff units. */
static void voice_step_lfos(MCPXAPUState *d, uint8_t *c, uint16_t v,
                            float *attenuation, float *pitch, float *cutoff)
{
    MCPXAPUVoiceFilter *filter = &d->vp.filters[v];
    if (!filter->lfo_armed) {
        uint32_t misc =
            voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF);
        filter->lfo_wait[0] =
            (misc & NV_PAVS_VOICE_CFG_MISC_LFOA_DELAY_ENABLE) ?
                voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_LFO_DELAY,
                                      NV_PAVS_VOICE_PAR_LFO_DELAY_LFOA) :
                0;
        filter->lfo_wait[1] =
            (misc & NV_PAVS_VOICE_CFG_MISC_LFOF_DELAY_ENABLE) ?
                voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_LFO_DELAY,
                                      NV_PAVS_VOICE_PAR_LFO_DELAY_LFOF) :
                0;
        filter->lfo_armed = true;
    }

    const uint32_t delta_mask[2] = { NV_PAVS_VOICE_TAR_LFO_ENV_LFOA_DELTA,
                                     NV_PAVS_VOICE_TAR_LFO_ENV_LFOF_DELTA };
    float value[2] = { 0.0f, 0.0f };
    for (int k = 0; k < 2; k++) {
        if (filter->lfo_wait[k]) {
            filter->lfo_wait[k]--;
            continue;
        }
        filter->lfo_phase[k] +=
            voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_LFO_ENV, delta_mask[k]);
        int p = filter->lfo_phase[k];
        value[k] = (p < 0x4000 ? p :
                    p < 0xC000 ? 0x8000 - p :
                                 p - 0x10000) /
                   16384.0f;
    }

    uint32_t mod = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_LFO_MOD, 0xFFFFFFFF);
    int8_t a_amplitude = GET_MASK(mod, NV_PAVS_VOICE_TAR_LFO_MOD_LFOA_AMPLITUDE);
    int8_t a_pitch = GET_MASK(mod, NV_PAVS_VOICE_TAR_LFO_MOD_LFOA_PITCH);
    int8_t a_cutoff = GET_MASK(mod, NV_PAVS_VOICE_TAR_LFO_MOD_LFOA_CUTOFF);
    int8_t f_pitch = GET_MASK(mod, NV_PAVS_VOICE_TAR_LFO_MOD_LFOF_PITCH);
    *attenuation = a_amplitude * 8.0f * value[0];
    *pitch = (a_pitch * value[0] + f_pitch * value[1]) * 32.0f;
    *cutoff = a_cutoff * 256.0f * value[0];
}

static int voice_get_samples(MCPXAPUState *d, uint8_t *c, uint32_t v,
                       float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO);
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask_nolock(
        d, c, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 }; /* B8, B16, ADPCM, B32 */
    unsigned int container_size_index = voice_get_mask_nolock(
        d, c, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    bool paused = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED);
    bool loop =
        voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_LOOP);
    uint32_t ebo = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST);
    bool multipass = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS);
    bool linked = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_LINKED); /* FIXME? */

    assert(!multipass); // Multipass is handled before this

    int ssl_index = 0;
    int ssl_seg = 0;
    int page = 0;
    int count = 0;
    int seg_len = 0;
    int seg_cs = 0;
    int seg_spb = 0;
    int seg_s = 0;
    uint64_t segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    uint32_t adpcm_block[36*2/4];
    MCPXAPUVoiceFilter *const vf = &d->vp.filters[v];
    const int16_t *adpcm_decoded = vf->adpcm_pcm;
    uint32_t pcm_page = UINT32_MAX;     /* a buffer's page last looked up, its address */
    uint64_t pcm_page_base = 0;

    // This is probably cleared when the first sample is played
    // FIXME: How will this behave if CBO > EBO on first play?
    // FIXME: How will this behave if paused?
    voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) {
        return -1;
    }

    if (stream) {
        if (!persist) {
            // FIXME: Confirm. Unsure if this should wait until end of SSL or
            // terminate immediately. Definitely not before end of envelope.
            int eacur = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                DPRINTF("Voice %d envelope not in release state (%d) and "
                        "persist is not set. Ending stream now!\n",
                        v, eacur);
                voice_off_nolock(d, c, v);
                return -1;
            }
        }

        DPRINTF("**** STREAMING (%d) ****\n", v);
        assert(!loop);

        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        // Check to see if the stream has ended
        if (count == 0) {
            DPRINTF("Stream has ended\n");
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                voice_off_nolock(d, c, v);
            } else {
                set_notify_status(
                    d, v, MCPX_HW_NOTIFIER_SSLA_DONE + d->vp.ssl[v].ssl_index,
                    NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        uint64_t ssl_offset = page * 8;
        {
            /* the table's pointer covers its MCPX_HW_MAX_SSL_PRDS entries;
             * the guest's base can be past them (by guest address, checked) */
            uint8_t *c = apu_ld_acq(&d->vp.ssl_cache);
            if (likely(c && (uint32_t)page < MCPX_HW_MAX_SSL_PRDS)) {
                segment_offset = apu_mem_ld32(c, ssl_offset);
                segment_length = apu_mem_ld32(c, ssl_offset + 4);
            } else {
                uint64_t addr = d->regs[NV_PAPU_VPSSLADDR] + ssl_offset;
                segment_offset = apu_ram_ld32(addr);
                segment_length = apu_ram_ld32(addr + 4);
            }
        }
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        assert(seg_cs == container_size_index);
        assert((seg_spb + 1) == samples_per_block);
        assert(seg_s == stereo);
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }

        assert(seg_len > 0);
        ebo = seg_len - 1; // FIXME: Confirm seg_len-1 is last valid sample index

        DPRINTF("Segment: SSL%c[%d]\n", 'A' + ssl_index, ssl_seg);
        DPRINTF("Page: %x\n", page);
        DPRINTF("Count: %d\n", count);
        DPRINTF("Segment offset: 0x%" PRIx64 "\n", segment_offset);
        DPRINTF("Segment length: %x\n", segment_length);
        DPRINTF("...len = 0x%x\n", seg_len);
        DPRINTF("...cs  = %d (%s)\n", seg_cs, container_size_str[seg_cs]);
        DPRINTF("...spb = %d\n", seg_spb);
        DPRINTF("...s   = %d (%s)\n", seg_s, seg_s ? "stereo" : "mono");
    } else {
        DPRINTF("**** BUFFER (%d) ****\n", v);
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);

    if (adpcm) {
        /* 36 bytes a channel: a block of more (the guest's samples per
         * block above its channels) fits neither adpcm_block nor the 65
         * samples a channel adpcm_decoded holds */
        if (!APU_CHECK(samples_per_block <= channels)) {
            return -1;
        }
        block_size = 36;
        DPRINTF("ADPCM:\n");
    } else {
        assert(container_size_index < 4);
        assert(sample_size < 4);
        block_size = container_size;
        DPRINTF("PCM:\n");
        DPRINTF("  Container Size: %s\n",
                container_size_str[container_size_index]);
        DPRINTF("  Sample Size: %s\n", sample_size_str[sample_size]);
    }

    DPRINTF("CBO=%d EBO=%d\n", cbo, ebo);

    block_size *= samples_per_block;

    // FIXME: Restructure this loop
    int sample_count = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
#if HAKUX_AUDIO_SKIP_MUTED
        /* Muted voice: skip the per-sample int->float DECODE (adpcm_decode /
         * ldXX / host-RAM reads) and write silence instead. `continue` jumps to
         * the for-header's update step, so sample_count++ and cbo++ STILL run,
         * keeping the returned sample_count -- and therefore the resampler's
         * input-consumption pattern and the CBO/SSL bookkeeping below -- bit-
         * identical to the un-gated path. Only the sample VALUES change. */
        if (d->vp.filters[v].cb_skip_decode) {
            samples[sample_count][0] = 0.0f;
            samples[sample_count][1] = 0.0f;
            continue;
        }
#endif
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != block_index) {
                uint32_t linear_addr = block_index * block_size;
                if (stream) {
                    uint64_t addr = segment_offset + linear_addr;
                    int max_seg_byte = (seg_len >> 6) * block_size;
                    assert(linear_addr + block_size <= max_seg_byte);
                    memcpy(adpcm_block,
                           &d->ram_ptr[vp_ram_addr(d, addr, block_size)],
                           block_size); // FIXME: Use idiomatic DMA function
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        uint64_t addr = get_data_ptr(d, d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            apu_rd32(&d->ram_ptr[vp_ram_addr(d, addr, 4)]);
                        linear_addr += 4;
                    }
                }
                /* the block this voice decoded last, byte for byte: its
                 * samples are still there */
                if (vf->adpcm_raw_len != block_size || vf->adpcm_channels != channels ||
                    memcmp(vf->adpcm_raw, adpcm_block, block_size)) {
                    adpcm_decode_block(vf->adpcm_pcm, (uint8_t *)adpcm_block,
                                       block_size, channels);
                    memcpy(vf->adpcm_raw, adpcm_block, block_size);
                    vf->adpcm_raw_len = block_size;
                    vf->adpcm_channels = channels;
                }
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            uint64_t addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                /* the buffer's page looked up once per page, not per sample
                 * (a read of the guest's page table each) */
                uint32_t linear_addr = ba + cbo * block_size;
                if (linear_addr / APU_PAGE_SIZE != pcm_page) {
                    pcm_page = linear_addr / APU_PAGE_SIZE;
                    pcm_page_base = get_data_ptr(d, d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                                 pcm_page * APU_PAGE_SIZE);
                }
                addr = pcm_page_base + linear_addr % APU_PAGE_SIZE;
            }

            /* Read directly from the host-mapped RAM pointer. */
            const uint8_t *src = &d->ram_ptr[vp_ram_addr(
                d, addr, (size_t)container_size * channels)];
            for (unsigned int channel = 0; channel < channels; channel++) {
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    fval = uint8_to_float(src[0]);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    fval = int16_to_float(apu_rd16(src));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    fval = int24_to_float(apu_rd32(src));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    fval = int32_to_float(apu_rd32(src));
                    break;
                default:
                    assert(false);
                    fval = 0;
                    break;
                }
                samples[sample_count][channel] = fval;
                src += container_size;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }
    }

    if (cbo >= ebo) {
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                DPRINTF("SSL%c[%d]\n", 'A' + ssl_index, d->vp.ssl[v].ssl_seg);
            } else {
                int next_index = (ssl_index + 1) % 2;
                DPRINTF("SSL%c\n", 'A' + next_index);
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                voice_off_nolock(d, c, v);
                DPRINTF("end of buffer!\n");
            }
        }
    }

    voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    return sample_count;
}

/* Fills the voice's block of input, NUM_SAMPLES_PER_FRAME samples decoded
 * from its buffer or stream (voice_get_samples), silence past its end. */
static void voice_fill_block(MCPXAPUState *d, uint8_t *c, uint16_t v,
                             MCPXAPUVoiceFilter *vf)
{
    int sample_count = 0;
    while (sample_count < NUM_SAMPLES_PER_FRAME) {
        int active = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        if (!active) {
            break;
        }
        int count = voice_get_samples(
            d, c, v, (float(*)[2]) & vf->resample_buf[2 * sample_count],
            NUM_SAMPLES_PER_FRAME - sample_count);
        if (count < 0) {
            break;
        }
        sample_count += count;
    }

    if (sample_count < NUM_SAMPLES_PER_FRAME) {
        memset(&vf->resample_buf[2 * sample_count], 0,
               2 * (NUM_SAMPLES_PER_FRAME - sample_count) * sizeof(float));
    }
}

/* A frame of the voice's output through the 8-point Lagrange interpolator
 * (lagrange_rows). steps are the cursor's advance per output sample in
 * 16.16; the input is read a block at a time as the cursor needs it. Only
 * channel 0 is interpolated when channels is 1 (a mono voice, or a 3D voice,
 * which plays its channel 0 alone), and copied to channel 1. */
static void voice_resample(MCPXAPUState *d, uint8_t *c, uint16_t v,
                           float samples[NUM_SAMPLES_PER_FRAME][2],
                           const uint32_t *steps, int channels)
{
    assert(v < MCPX_HW_MAX_VOICES);
    MCPXAPUVoiceFilter *vf = &d->vp.filters[v];
    uint32_t pos = vf->interp_pos;
    int idx = vf->interp_idx & 7;
    int buf_pos = vf->resample_buf_pos;

    for (int n = 0; n < NUM_SAMPLES_PER_FRAME; n++) {
        while (pos >= 0x10000) {
            if (buf_pos >= NUM_SAMPLES_PER_FRAME) {
                voice_fill_block(d, c, v, vf);
                buf_pos = 0;
            }
            const float l = vf->resample_buf[2 * buf_pos];
            const float r = vf->resample_buf[2 * buf_pos + 1];
            vf->interp[idx][0] = vf->interp[idx + 8][0] = l;
            vf->interp[idx][1] = vf->interp[idx + 8][1] = r;
            idx = (idx + 1) & 7;
            buf_pos++;
            pos -= 0x10000;
        }

        /* the last eight input samples, oldest first: x[3] is at the
         * cursor, x[4] one sample past it */
        const float (*x)[2] = &vf->interp[idx];
        const uint32_t frac = pos;
        float acc0 = x[3][0], acc1 = x[3][1];
        if (frac) {
            const float *lo = lagrange_rows[frac >> 8];
            const float *hi = lagrange_rows[(frac >> 8) + 1];
            const float a = (frac & 0xFF) / 256.0f;
            float w[8];
            for (int k = 0; k < 8; k++) {
                w[k] = lo[k] + (hi[k] - lo[k]) * a;
            }
            acc0 = 0.0f;
            for (int k = 0; k < 8; k++) {
                acc0 += w[k] * x[k][0];
            }
            if (channels == 2) {
                acc1 = 0.0f;
                for (int k = 0; k < 8; k++) {
                    acc1 += w[k] * x[k][1];
                }
            }
        }
        samples[n][0] = acc0;
        samples[n][1] = channels == 2 ? acc1 : acc0;
        pos += steps[n];
    }

    vf->interp_pos = pos;
    vf->interp_idx = idx;
    vf->resample_buf_pos = buf_pos;
}

/* The cursor's advance for each sample of the frame, 2^(pitch / 4096) in
 * 16.16. The pitch moves from the last frame's to this frame's in 32 steps
 * across the frame, in its log form: on a retail console a pitch change from
 * 0 to -1000h advanced the cursor 23 samples on that frame, the sum of
 * 2^(-n/32), where stepping at once gives 16 and a linear ramp of the step
 * 24. 2^(p_n / 4096) is geometric in n, so a moving pitch takes one
 * exponential and a multiply a sample. */
static void voice_pitch_steps(uint32_t steps[NUM_SAMPLES_PER_FRAME],
                              double from, double to)
{
    double s = exp2(from / 4096.0) * 65536.0;
    if (from == to) {
        const uint32_t step = s;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            steps[i] = step;
        }
        return;
    }
    const double r = exp2((to - from) / (32.0 * 4096.0));
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        steps[i] = s;
        s *= r;
    }
}

static void get_multipass_samples(MCPXAPUState *d, uint8_t *c,
                                  float mixbins[][NUM_SAMPLES_PER_FRAME],
                                  uint16_t v, float samples[][2])
{
    // DirectSound sets bin to 31, but hardware would allow other bins
    int mp_bin = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);

    /* The bin is read as 24-bit samples scaled up by 2^headroom, the voice
     * format's headroom field, which undoes the bin headroom its voices were
     * mixed with. On a retail console, from DC 008000h, a multipass voice
     * gave 008000h at headroom 0 and 400000h at 7, and 008000h again when the
     * source bin's headroom was 7 too. DirectSound sets both headrooms to 7,
     * so read unscaled its submixes came out 42 dB too quiet. */
    int headroom = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                         NV_PAVS_VOICE_CFG_FMT_HEADROOM);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        float x = floorf(mixbins[mp_bin][i] * 8388608.0f + 0.5f);
        x = fminf(fmaxf(x, -8388608.0f), 8388607.0f) / 8388608.0f;
        samples[i][0] = ldexpf(x, headroom);
        samples[i][1] = samples[i][0];
    }

    // DirectSound sets clear mix to true
    bool clear_mix = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX);
    if (clear_mix) {
        memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
    }
}

/* A route's gain for each sample of the frame (route_gain's units times
 * scale): from where the last frame ended to this frame's in 32 steps, each
 * rounded down. */
static inline void route_gain_ramp(float g[NUM_SAMPLES_PER_FRAME], float from,
                                   float to, float scale)
{
    if (from == to) {   /* whole units: the steps are all the gain itself */
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            g[i] = to * scale;
        }
        return;
    }
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        g[i] = floorf(from + (to - from) * (i / 32.0f)) * scale;
    }
}

/* 5.1 out (g_apu_out_channels 6): the VP's own mix to the six speakers, the
 * frame's (the shares' added in, voice_work_share) */
static float g_vp_sample6[NUM_SAMPLES_PER_FRAME][6];

static void voice_process(MCPXAPUState *d, uint8_t *c,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          float sample6[NUM_SAMPLES_PER_FRAME][6],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    MCPXAPUVoiceFilter *vf = &d->vp.filters[v];
    const bool three_d = v < MCPX_HW_MAX_3D_VOICES;
    bool stereo = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO);
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED);

    if (paused) {
        return;
    }

    float ef_value = voice_step_envelope(
        d, c, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    assert(ef_value >= 0.0f);
    assert(ef_value <= 1.0f);
    int16_t p = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_ENV0,
                               NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float lfo_attenuation, lfo_pitch, lfo_cutoff;
    voice_step_lfos(d, c, v, &lfo_attenuation, &lfo_pitch, &lfo_cutoff);
    double pitch = p + ps * 32.0 * ef_value + lfo_pitch;

    uint32_t steps[NUM_SAMPLES_PER_FRAME];
    voice_pitch_steps(steps, vf->pitched ? vf->pitch : pitch, pitch);
    vf->pitch = pitch;
    vf->pitched = true;

    float ea_value = voice_step_envelope(
        d, c, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    assert(ea_value >= 0.0f);
    assert(ea_value <= 1.0f);

#if HAKUX_AUDIO_SKIP_MUTED
    /* Hoisted silence predicate: read the 8 mixbin vols BEFORE the resample
     * block so a muted voice can skip decode (in voice_get_samples, via
     * cb_skip_decode) and filters/mix (early-return below). The resampler
     * still pulls its input, so CBO/SSL/SSLA_DONE/loop/voice_off advance
     * bit-exactly (we only null the sample VALUES). A voice whose routes
     * ended the last frame audible is not skipped: its gains ramp down to
     * silence across this one. */
    bool skip_muted = voice_all_vols_maxed(d, c, v);
    for (int b = 0; b < 8 && skip_muted; b++) {
        skip_muted = vf->route_gain[b] == 0.0f;
    }
#endif

    float samples[NUM_SAMPLES_PER_FRAME][2] = { 0 };

    bool multipass = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS);

#if !defined(HAKUX_AUDIO_NO_PROCESS) || !HAKUX_AUDIO_NO_PROCESS
#if HAKUX_AUDIO_SKIP_MUTED
    /* the per-voice skip decision for voice_get_samples (through
     * voice_resample -> voice_fill_block), for this frame's resample only.
     * Multipass voices have no decode loop. */
    vf->cb_skip_decode = skip_muted;
#endif
    if (multipass) {
        get_multipass_samples(d, c, mixbins, v, samples);
    } else {
        int active = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        if (!active) {
            return;
        }
        voice_resample(d, c, v, samples, steps, (stereo && !three_d) ? 2 : 1);
    }

    int active = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    if (!active) {
        return;
    }
#else
    /* HAKUX_AUDIO_NO_PROCESS=1: skip the expensive sample-production +
     * mixing path. Envelopes above still advance guest-observable
     * PAR_STATE / CUR_ECNT / level registers, and mcpx_apu_vp_frame's
     * outer voice-list walk (the v1 deadlock site) is untouched, so the
     * guest still sees voice-list progression and fe_method side
     * effects. `samples[]` is already zeroed at the declaration above,
     * and `mixbins[]` is left untouched -> downstream DSP mixes silence.
     * NOTE: this also short-circuits voice_resample / voice_get_samples,
     * which would normally advance NV_PAVS_VOICE_PAR_OFFSET_CBO and the
     * SSL segment/end-of-buffer / SSLA_DONE notifier / voice_off() path.
     * Titles that drive playback off CBO/SSL polling may stall; this is
     * an FPS-cost probe, not a shippable audio mode. */
    (void)samples;
    (void)multipass;
    (void)steps;
    (void)mixbins;
    return;
#endif

    /* A 3D voice plays its channel 0 alone, as a mono voice. On a retail
     * console a stereo 3D voice with 4000h on the left channel and 1000h on
     * the right, through an HRTF entry, gave both ears and routes 4-7 the
     * left channel only, filtered as a mono 3D voice's. */
    if (three_d && stereo) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            samples[i][1] = samples[i][0];
        }
        stereo = false;
        channels = 1;
    }

    int bin[8];
    bin[0] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (three_d) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                            NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                            NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                            NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                            NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME5);

    vol[6] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8;
    vol[6] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                             NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4;
    vol[6] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                             NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8;
    vol[7] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                             NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4;
    vol[7] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                             NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

#if PERF_LOG
    /* Silent-but-active voice probe (read-only; no control-flow change).
     * This call has done the full resample work and has both predicate
     * inputs live: ea_value (the EA amplitude envelope) and vol[0..7]. */
    {
        bool ea0 = (ea_value == 0.0f);
        bool allvol_max = true;
        for (int i = 0; i < 8; i++) {
            if ((vol[i] & 0xFFF) != 0xFFF) { /* the route is audible */
                allvol_max = false;
                break;
            }
        }
        VP_ADD(calls, 1);
        VP_ADD(total_samples, NUM_SAMPLES_PER_FRAME);
        if (ea0) {
            VP_ADD(ea0_calls, 1);
        }
        if (allvol_max) {
            VP_ADD(allvol_calls, 1);
        }
        if (ea0 && allvol_max) {
            VP_ADD(silent_calls, 1);
            VP_ADD(silent_samples, NUM_SAMPLES_PER_FRAME);
        }
    }
#endif

    // FIXME: If phase negations means to flip the signal upside down
    //        we should modify volume of bin6 and bin7 here.

#if HAKUX_AUDIO_SKIP_MUTED
    /* Muted voice: the resample above already advanced CBO/SSL/SSLA_DONE/
     * loop/voice_off bit-exactly. Every route is at FFFh (silent even under
     * the amplitude LFO) and ended the last frame silent, so the filters and
     * mix contribute ZERO -- skip them. Remember the mute so the filter state
     * is reset-to-rest on the next audible frame. */
    if (skip_muted) {
#if PERF_LOG
        VP_ADD(skipped_voices, 1);
#endif
        vf->gains = true;
        vf->muted_prev = true;
        return;
    }
    /* Audible this frame. If this voice was muted, its filters were frozen
     * mid-decay across the muted frames: resume from rest instead. */
    if (vf->muted_prev) {
        memset(vf->svf, 0, sizeof(vf->svf));
        memset(vf->peq, 0, sizeof(vf->peq));
        memset(vf->i3dl2, 0, sizeof(vf->i3dl2));
        hrtf_filter_clear_history(&vf->hrtf);
        vf->muted_prev = false;
    }
#endif

    int fmode = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);

    uint16_t hrtf_handle = HRTF_NULL_HANDLE;
    if (three_d) {
        hrtf_handle = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                            NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
    }
    /* A 3D voice whose HRTF entry is FFFFh filters as a 2D voice and has no
     * I3DL2 filter: on a retail console mode 3 ran DLS2 from FCA and then the
     * ParaEQ from FCB on routes 4-7 alike, mode 2 the ParaEQ from FCB, and
     * mode 1 DLS2 alone. */
    const bool spatial = three_d && hrtf_handle != HRTF_NULL_HANDLE;
    bool lpf, peq;
    if (spatial) {
        /* 1:DLS2+I3DL2 2:ParaEQ+I3DL2 3:I3DL2 */
        lpf = (fmode == 1);
        peq = (fmode == 2);
    } else {
        /* 0:Bypass 1:DLS2 2:ParaEQ 3(Mono):DLS2+ParaEQ 3(Stereo):Bypass */
        lpf = stereo ? (fmode == 1) : (fmode & 1);
        peq = stereo ? (fmode == 2) : (fmode >= 2);
    }

    /* The F envelope moves the DLS2 and ParaEQ frequencies by its FC scale
     * (ENV1 bits 31:24), 256 units, a 16th of an octave, per step at full
     * level, and the A LFO by its cutoff depth. On a retail console an fs/4
     * tone through the DLS2 filter at C000h, with the F envelope's attack at
     * scale +64, came out 1/8 octave higher each frame up to 4 octaves, and
     * the same through the ParaEQ's cut. Each coefficient moves from the
     * last frame's to this frame's across the frame, linearly in f: stepped
     * at once, a frame's peak came out up to 10% high. Only channel 0 is
     * filtered on a mono voice (channel 1 is its copy). */
    int8_t fc_scale = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_ENV1,
                                            NV_PAVS_VOICE_CFG_ENV1_EF_FCSCALE);
    double fc_bend = fc_scale * 256.0 * ef_value + lfo_cutoff;

    /* DLS2: the state-variable low-pass, f from FC0 (filter_coefficient), its
     * damping FC1 / 32768 with no lower clamp (FC1 0 self-oscillated on the
     * console, 400h and 800h rang as the unclamped filter does), its output
     * not clamped to full scale either. */
    if (lpf) {
        for (unsigned int ch = 0; ch < channels; ch++) {
            uint32_t fca = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_FCA + ch * 4,
                                                 0xFFFFFFFF);
            int16_t fc = GET_MASK(fca, NV_PAVS_VOICE_TAR_FCA_FC0);
            uint16_t q = GET_MASK(fca, NV_PAVS_VOICE_TAR_FCA_FC1);
            float fc_f = filter_coefficient(fc + fc_bend);
            float f_from = vf->dls2_set ? vf->dls2_f[ch] : fc_f;
            vf->dls2_f[ch] = fc_f;
            /* at most 1: the console was measured up to 8000h (1.0), and past
             * about 1.5 the filter diverges with f near 1, which the console
             * can't (Medal of Honor: European Assault's voices run FC0 0 and
             * FC1 FFFFh, f 1 and damping 2: unbounded, their samples went to
             * NaN and the whole mix to full scale) */
            float q_f = fminf(q / (1.0f * 0x8000), 1.0f);
            sv_filter *filter = &vf->svf[ch];
            setup_svf(filter, fc_f, q_f, F_LP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                filter->f = f_from + (fc_f - f_from) * (i / 32.0f);
                samples[i][ch] = run_svf(filter, samples[i][ch]);
            }
        }
    }
    vf->dls2_set = lpf;

    /* ParaEQ: the DLS2 filter's SVF as a peaking EQ, y = x + G * q * band,
     * its output rounded as the filter's steps are. Its word's bits 15:0 give
     * the frequency in the DLS2 log form and bits 31:16 the signed gain,
     * G = gain / 8192; MISC bits 20:18 give q = 2^-Q. The word is FCB on mono
     * 2D voices, FCA on 3D voices (whose FCB is the I3DL2's), and FCA for
     * channel 0 and FCB for channel 1 on stereo voices. A retail console's
     * impulse responses at four frequencies, gains -4 to +2 and Q 0 to 7, in
     * each layout, match this. */
    if (peq) {
        int q_field = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_MISC,
                                            NV_PAVS_VOICE_CFG_MISC_PARAEQ_Q);
        float q_f = ldexpf(1.0f, -q_field);
        for (unsigned int ch = 0; ch < channels; ch++) {
            uint64_t reg = spatial ? NV_PAVS_VOICE_TAR_FCA :
                           stereo  ? NV_PAVS_VOICE_TAR_FCA + ch * 4 :
                                     NV_PAVS_VOICE_TAR_FCB;
            uint32_t word = voice_get_mask_nolock(d, c, v, reg, 0xFFFFFFFF);
            float fc_f = filter_coefficient((int16_t)(word & 0xFFFF) + fc_bend);
            float f_from = vf->peq_set ? vf->peq_f[ch] : fc_f;
            vf->peq_f[ch] = fc_f;
            float g = (int16_t)(word >> 16) / 8192.0f * q_f;
            sv_filter *filter = &vf->peq[ch];
            setup_svf(filter, fc_f, q_f, F_BP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                filter->f = f_from + (fc_f - f_from) * (i / 32.0f);
                samples[i][ch] = svf_round(samples[i][ch] +
                                           g * run_svf(filter, samples[i][ch]));
            }
        }
    }
    vf->peq_set = peq;

    if (channels == 1) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            samples[i][1] = samples[i][0];
        }
    }

    /* I3DL2: a 3D voice's pair of one-pole low-passes, y = (1 - a) x + a y',
     * each value rounded as the SVF's are, with a = FCB 15:0 / 65536 for the
     * direct path and FCB 31:16 / 65536 for the room path. On a retail
     * console 2000h gave 0.875 * 0.125^n and C000h 0.25 * 0.75^n, exactly,
     * and an impulse through a path with a = 8000h settled at 10h for good,
     * which only the rounding explains. The direct path feeds routes 0-4 and
     * 6 and the room path routes 5 and 7, in filter modes 1, 2 and 3
     * alike. */
    float room[NUM_SAMPLES_PER_FRAME];
    const bool i3dl2 = spatial && fmode != 0 && !stereo;
    if (i3dl2) {
        uint32_t fcb = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF);
        float a_direct = (fcb & 0xFFFF) / 65536.0f;
        float a_room = (fcb >> 16) / 65536.0f;
        float *state = vf->i3dl2;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float x = samples[i][0];
            state[0] = svf_round(x + (state[0] - x) * a_direct);
            state[1] = svf_round(x + (state[1] - x) * a_room);
            samples[i][0] = samples[i][1] = state[0];
            room[i] = state[1];
        }
    }

    /* Routes 0-1 of a 3D voice carry the HRTF's left ear and routes 2-3 its
     * right ear; its routes 4-7 carry the voice itself, not the HRTF output;
     * without an HRTF entry (FFFFh) routes 0-3 are silent (measured on a
     * retail console). The voice reads its entry every frame, so rewriting
     * the entry or moving the voice to another one takes effect on the next
     * frame. */
    float ears[NUM_SAMPLES_PER_FRAME][2];
    if (spatial) {
        if (g_apu_config.hrtf && APU_CHECK(hrtf_handle < HRTF_ENTRY_COUNT)) {
            hrtf_filter_set_target_params(&vf->hrtf,
                                          d->vp.hrtf.entries[hrtf_handle].hrir,
                                          d->vp.hrtf.entries[hrtf_handle].itd);
            hrtf_filter_process(&vf->hrtf, samples, ears);
        } else {
            memcpy(ears, samples, sizeof(ears));
        }
    }

    /* Each route's source as a frame of contiguous samples (mix_one_bin
     * reads contiguous float[32]) */
    float channel_buf[2][NUM_SAMPLES_PER_FRAME];
    float ear_buf[2][NUM_SAMPLES_PER_FRAME];
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        channel_buf[0][i] = samples[i][0];
    }
    if (channels == 2) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            channel_buf[1][i] = samples[i][1];
        }
    }
    if (spatial) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            ear_buf[0][i] = ears[i][0];
            ear_buf[1][i] = ears[i][1];
        }
    }

    /* A route's gain, in 1/16384, is its attenuation's gain times the
     * amplitude envelope's level in 2048ths, rounded down, the attenuation
     * moved by the A LFO (a route at FFFh stays silent under a negative
     * one). It moves from where the last frame ended to this frame's in 32
     * steps across the frame, each sample's gain rounded down, and a new
     * voice fades in from silence over its first frame. On a retail console
     * a volume change read sample n of its frame n/32 of the way, a DC
     * voice's first frame rose 0, 1/4, 1/2, 3/4 and 31/32 of its level at
     * samples 0, 8, 16, 24 and 31, and sustain 80h at attenuation 100h gave
     * 5188/16384. Stepped at frame edges, envelopes buzzed at the 1.5 kHz
     * frame rate and volume changes clicked. The bin's headroom applies to
     * HRTF submixes too: the HRTF headroom had no measurable effect on
     * hardware (at 1 the submixes and the voice's other routes came out as
     * at 0). */
    const uint32_t level = lrintf(ea_value * 2048.0f);
    const int lfo_att = (int)floorf(lfo_attenuation);
    float g_from[8], g_to[8], scale[8];
    bool live[8];
    const float *src[8];
    for (int b = 0; b < 8; b++) {
        live[b] = false;
        if (three_d && b < 4 && !spatial) {
            vf->route_gain[b] = 0.0f;
            continue;
        }
        g_from[b] = vf->gains ? vf->route_gain[b] : 0.0f;
        g_to[b] = 0.0f;
        if (vol[b] != 0xFFF) {
            g_to[b] = (attenuation_gain(vol[b] + lfo_att) * level) >> 11;
        }
        vf->route_gain[b] = g_to[b];
        live[b] = g_from[b] != 0.0f || g_to[b] != 0.0f;
        scale[b] = 1.0f / (float)(16384 << d->vp.submix_headroom[bin[b]]);
        src[b] = (three_d && b < 4)            ? ear_buf[b / 2] :
                 (i3dl2 && (b == 5 || b == 7)) ? room :
                                                 channel_buf[b % channels];
    }
    vf->gains = true;

    /* The VP's own mix (the default, no DSP): the frame's mixbins are
     * cleared unread at its end, but for the bins a multipass voice reads,
     * which the next voices of its group read (scheduled together,
     * voice_work_schedule) */
    const bool mon_vp = d->monitor.point == MCPX_APU_DEBUG_MON_VP;
    const uint32_t mp_bins = d->vp.voice_work_dispatch.mp_bins;
    for (int b = 0; b < 8; b++) {
        if (!live[b] || (mon_vp && !(mp_bins & (1u << bin[b])))) {
            continue;
        }
        if (g_from[b] == g_to[b]) {
            mix_one_bin(mixbins[bin[b]], src[b], g_to[b] * scale[b]);
        } else {
            float g[NUM_SAMPLES_PER_FRAME];
            route_gain_ramp(g, g_from[b], g_to[b], scale[b]);
            mix_one_bin_ramp(mixbins[bin[b]], src[b], g);
        }
    }

    if (mon_vp) {
        /* For VP mon, simply mix all voices together here, selecting the
         * maximal gain of any of the voice's routes, sample by sample, as the
         * overall gain for this voice; a 3D voice with an HRTF entry plays its
         * two ears.
         *
         * Routes into a bin a multipass voice reads are left out: they are
         * mixed into the bin instead, and that sub-mix is mixed in here when
         * the multipass voice is processed. MON_VP is not how the hardware
         * works anyway (the DSP mixes the bins), so DirectSound compatibility
         * is enough. */
        float g[NUM_SAMPLES_PER_FRAME] = { 0 };
        float gb[NUM_SAMPLES_PER_FRAME];
        for (int b = 0; b < 8; b++) {
            if (!live[b] || (mp_bins & (1u << bin[b]))) {
                continue;
            }
            route_gain_ramp(gb, g_from[b], g_to[b], scale[b]);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                g[i] = fmaxf(g[i], gb[i]);
            }
        }
        const float *left = spatial ? ear_buf[0] : channel_buf[0];
        const float *right = spatial ? ear_buf[1] : channel_buf[channels - 1];
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            sample_buf[i][0] += g[i] * left[i];
            sample_buf[i][1] += g[i] * right[i];
        }
        if (g_apu_out_channels == 6) {
            /* 5.1: each of the voice's speaker mixbins (DirectSound's 0-5:
             * FL FR C LFE BL BR) at its own gain. One in none of them (a
             * submix, an effect's send: the DSP's, not run here) plays in
             * the front pair, as in stereo. */
            bool spoke = false;
            for (int b = 0; b < 8; b++) {
                if (!live[b] || (mp_bins & (1u << bin[b])) || bin[b] > 5) {
                    continue;
                }
                spoke = true;
                route_gain_ramp(gb, g_from[b], g_to[b], scale[b]);
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                    sample6[i][bin[b]] += gb[i] * src[b][i];
                }
            }
            for (int i = 0; !spoke && i < NUM_SAMPLES_PER_FRAME; i++) {
                sample6[i][0] += g[i] * left[i];
                sample6[i][1] += g[i] * right[i];
            }
        }
    }
}

static void get_voice_bin_src_dst(MCPXAPUState *d, uint8_t *c, int v,
                                  uint32_t *src, uint32_t *dst, uint32_t *clr)
{
    uint32_t src_v = 0;
    uint32_t dst_v = 0;
    uint32_t clr_v = 0;

    bool multipass = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS);
    if (multipass) {
        int mp_bin = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        bool clear_mix = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX);
        src_v |= (1u << mp_bin);
        if (clear_mix) {
            clr_v |= (1u << mp_bin);
        }
    }

    int bin[8];
    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    } else {
        bin[0] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                                NV_PAVS_VOICE_CFG_VBIN_V0BIN);
        bin[1] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                                NV_PAVS_VOICE_CFG_VBIN_V1BIN);
        bin[2] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                                NV_PAVS_VOICE_CFG_VBIN_V2BIN);
        bin[3] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                                NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    }
    bin[4] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_V7BIN);

    for (int i = 0; i < 8; i++) {
        dst_v |= 1u << bin[i];
    }

    if (src) {
        *src = src_v;
    }
    if (dst) {
        *dst = dst_v;
    }
    if (clr) {
        *clr = clr_v;
    }
}

/* One share of the frame's voices: processed outside the lock, then added
 * in under it. Called and returning with vwd->lock held, by a helper
 * thread or (share 0) the frame thread. */
static void voice_work_share(MCPXAPUState *d, VoiceWorker *self)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;
    pthread_mutex_unlock(&vwd->lock);

    // Process queued voices
    const bool mon_vp = d->monitor.point == MCPX_APU_DEBUG_MON_VP;
    if (mon_vp) {
        /* only the bins a multipass voice reads are mixed (voice_process),
         * and they stay in the share: their group is scheduled together */
        for (uint32_t m = vwd->mp_bins; m; m &= m - 1) {
            memset(self->mixbins[ctz32(m)], 0, sizeof(self->mixbins[0]));
        }
        memset(self->sample_buf, 0, sizeof(self->sample_buf));
        if (g_apu_out_channels == 6) {
            memset(self->sample6, 0, sizeof(self->sample6));
        }
    } else {
        memset(self->mixbins, 0, sizeof(self->mixbins));
    }
    /* The frame's voice table (voice_process and its callees take
     * it as `c`; their stores are reported with the batch). */
    {
        uint8_t *c = vwd->table;
        for (int i = 0; i < self->queue_len; i++) {
            voice_process(d, c, self->mixbins, self->sample_buf, self->sample6,
                          self->queue[i].voice, self->queue[i].list);
        }
    }

    pthread_mutex_lock(&vwd->lock);

    // Add voice contributions (the mixbins: the DSP's, which the VP's own mix has none of)
    if (!mon_vp) {
        for (int b = 0; b < NUM_MIXBINS; b++) {
            for (int s = 0; s < NUM_SAMPLES_PER_FRAME; s++) {
                vwd->mixbins[b][s] += self->mixbins[b][s];
            }
        }
    } else {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->vp.sample_buf[i][0] += self->sample_buf[i][0];
            d->vp.sample_buf[i][1] += self->sample_buf[i][1];
        }
        for (int i = 0; g_apu_out_channels == 6 && i < NUM_SAMPLES_PER_FRAME; i++) {
            for (int ch = 0; ch < 6; ch++) {
                g_vp_sample6[i][ch] += self->sample6[i][ch];
            }
        }
    }

    self->queue_len = 0;
}

/* A helper: shares 1 and up (share 0 is the frame thread's own). */
static void *voice_worker_thread(void *arg)
{
    MCPXAPUState *d = arg;
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    pthread_mutex_lock(&vwd->lock);

    int worker_id = ctz64(vwd->workers_pending);
    VoiceWorker *self = &d->vp.voice_work_dispatch.workers[worker_id];
    self->queue_len = 0;

    do {
        if (self->queue_len) {
            voice_work_share(d, self);
        }

        vwd->workers_pending &= ~(1 << worker_id);
        if (!vwd->workers_pending) {
            pthread_cond_signal(&vwd->work_finished);
        }

        pthread_cond_wait(&self->cond, &vwd->lock);
    } while (!vwd->workers_should_exit);

    return NULL;
}

/* A listed voice for the frame, by its state under its spinlock (the guest's
 * methods change that state holding the voice: mcpx_apu_vp_write's guard).
 * The frame never waits on the guest for long: a voice the guest holds (its
 * VOICE_LOCK, or a method running) is waited for HELD_WAIT_US, holding none
 * of its own (the guest's unlock takes it), the first frame it is held, then
 * left for a later frame. Waiting for the unlock, the frame deadlocked with a
 * guest that locks the voices of a group (a multichannel stream) one by one,
 * its next lock waiting on one the frame's batch held already; and with one
 * that waits on the APU holding a voice (a notification, the GP's mailbox:
 * the frame that produces it never came). On the hardware the VP keeps its
 * pace whatever the CPU holds. The wait lasts while the listener can't tell:
 * up to HELD_WAIT_US, while more than a callback's worth is queued for the
 * output (the frames then catch up before the next callback), slept for
 * after HELD_SPIN_US. A title's holds end within a fraction of a millisecond
 * (Forza's race: 0.22 ms at most in a minute); spun for throughout, the wait
 * kept the holding thread off a core they shared: holds then lasted until
 * the spin gave up, and about one a second was left out of its frames (a
 * gap of 0.7-2.7 ms in the voice). */
#define HELD_WAIT_US 8000
#define HELD_SPIN_US 50                 /* then slept for, HELD_NAP_US at a time */
#define HELD_NAP_US 100
#define HELD_STUCK_FRAMES 1500          /* a second of GP frames */
/* HAKUX_APU_STATS: frames a voice was left out; holds waited out, the
 * longest of those (us); the longest run of frames a voice was left out */
uint32_t g_vp_held_skips, g_vp_held_waits, g_vp_held_wait_max_us, g_vp_held_run_max;

typedef enum VoiceTake {
    VOICE_TAKEN,    /* active: its spinlock kept for the batch */
    VOICE_IDLE,     /* off: for the idle report */
    VOICE_HELD,     /* the guest's: a later frame's */
} VoiceTake;

static VoiceTake voice_take(MCPXAPUState *d, uint8_t *c, uint16_t v)
{
    ApuSpin *s = &d->vp.voice_spinlocks[v];
    apu_spin_lock(s);
    if (is_voice_locked(d, v)) {
        apu_spin_unlock(s);
        uint16_t *held = &d->vp.held_frames[v];
        bool got = false;
        if (!*held) {
            const int64_t t0 = apu_host_us();
            int64_t us = 0;
            while (us < HELD_WAIT_US && apu_out_queued() >= d->monitor.device_buffer_bytes) {
                if (!is_voice_locked(d, v)) {
                    apu_spin_lock(s);
                    if ((got = !is_voice_locked(d, v))) {
                        break;
                    }
                    apu_spin_unlock(s);
                } else if (us < HELD_SPIN_US) {
#if defined(__aarch64__)
                    __asm__ __volatile__("yield" ::: "memory");
#endif
                } else {
                    usleep(HELD_NAP_US);
                }
                us = apu_host_us() - t0;
            }
            if (got) {
                apu_inc(&g_vp_held_waits);
                if ((uint32_t)us > apu_ld(&g_vp_held_wait_max_us)) {
                    apu_st(&g_vp_held_wait_max_us, (uint32_t)us);
                }
            }
        }
        if (!got) {
            apu_inc(&g_vp_held_skips);
            if (*held < UINT16_MAX && ++*held == HELD_STUCK_FRAMES) {
                nc_log("hakuX-apu", "stuck: voice %u held by the guest (its VOICE_LOCK) for a second, "
                                    "left out of the frames since; FECTL %08x ISTS %08x",
                       v, apu_ld(&d->regs[NV_PAPU_FECTL]), apu_ld(&d->regs[NV_PAPU_ISTS]));
            }
            return VOICE_HELD;
        }
    }
    if (d->vp.held_frames[v]) {
        if (d->vp.held_frames[v] > apu_ld(&g_vp_held_run_max)) {
            apu_st(&g_vp_held_run_max, d->vp.held_frames[v]);
        }
        d->vp.held_frames[v] = 0;
    }
    if (!voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
        apu_spin_unlock(s);
        return VOICE_IDLE;
    }
    return VOICE_TAKEN;
}

/* an active voice for the batch, its spinlock taken (voice_take) */
static void voice_work_enqueue(MCPXAPUState *d, int v, int list)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    assert(vwd->queue_len < ARRAY_SIZE(vwd->queue));
    vwd->queue[vwd->queue_len++] = (VoiceWorkItem){
        .voice = v,
        .list = list,
    };
}

static void voice_work_release_voice_locks(MCPXAPUState *d)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    for (int i = 0; i < vwd->queue_len; i++) {
        apu_spin_unlock(&d->vp.voice_spinlocks[vwd->queue[i].voice]);
    }
}

/* Fewer voices than this a frame: all the frame thread's, no helper woken
 * (a wake and a merge cost more than the voices). */
#define VOICES_INLINE 8

static void voice_work_schedule(MCPXAPUState *d, uint8_t *c)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;
    const int shares = vwd->queue_len < VOICES_INLINE ? 1 : vwd->num_workers;
    int next_worker_to_schedule = 0;
    bool group = false;
    uint32_t dirty = 0;

    // To simplify submix scheduling, voices are grouped by DirectSound's
    // multipass layout:
    //
    // - MP bin is constant
    // - MP voice always clears MP bin
    // - MP source voices are ordered consecutively in voice lists
    //
    // Hardware takes any bin: on a retail console multipass voices reading
    // bins 9 and 12 mixed correctly. A frame that departs from the layout
    // runs all its voices in list order on one worker.
    bool serial = false;
    uint32_t mp_bins = 0;
    for (int i = 0; i < vwd->queue_len; i++) {
        uint32_t src, dst, clr;
        get_voice_bin_src_dst(d, c, vwd->queue[i].voice, &src, &dst, &clr);
        if ((src && (src != MULTIPASS_BIN_MASK || clr != MULTIPASS_BIN_MASK)) ||
            (!src && !(dst & MULTIPASS_BIN_MASK) &&
             (dirty & MULTIPASS_BIN_MASK))) {
            serial = true;
        }
        mp_bins |= src;
        dirty = (dirty & ~clr) | dst;
    }
    vwd->mp_bins = mp_bins;
    dirty = 0;

    for (int i = 0; i < vwd->queue_len; i++) {
        uint32_t src, dst, clr;
        get_voice_bin_src_dst(d, c, vwd->queue[i].voice, &src, &dst, &clr);

        if (serial) {
            group = true;
        } else if ((dst & MULTIPASS_BIN_MASK) & ~dirty) {
            group = true;
        }

        // Assign voice to worker
        VoiceWorker *worker = &vwd->workers[next_worker_to_schedule];
        worker->queue[worker->queue_len++] = vwd->queue[i];
        vwd->workers_pending |= 1 << next_worker_to_schedule;

        dirty = (dirty & ~clr) | dst;
        if ((clr & MULTIPASS_BIN_MASK) && !serial) {
            group = false;
        }

        if (!group) {
            next_worker_to_schedule = (next_worker_to_schedule + 1) % shares;
        }
    }
}

static void
voice_work_dispatch(MCPXAPUState *d, uint8_t *c,
                    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    pthread_mutex_lock(&vwd->lock);

    if (vwd->queue_len) {
        if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
            memset(vwd->mixbins, 0, sizeof(vwd->mixbins));
        }
        vwd->table = c;

        // Signal only the helpers that have been assigned work
        voice_work_schedule(d, c);
        uint64_t pending = vwd->workers_pending & ~1ull;
        while (pending) {
            int id = ctz64(pending);
            pthread_cond_signal(&vwd->workers[id].cond);
            pending &= pending - 1;
        }
        /* share 0 here, meanwhile: a wake fewer, and no wait at all when
         * it is the only one */
        if (vwd->workers_pending & 1) {
            voice_work_share(d, &vwd->workers[0]);
            vwd->workers_pending &= ~1ull;
        }
        while (vwd->workers_pending) {  /* a spurious wake-up: workers still running */
            pthread_cond_wait(&vwd->work_finished, &vwd->lock);
        }
        voice_work_release_voice_locks(d);
        vwd->queue_len = 0;

        // Add voice contributions (none to the mixbins in the VP's own mix)
        if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
            for (int b = 0; b < NUM_MIXBINS; b++) {
                for (int s = 0; s < NUM_SAMPLES_PER_FRAME; s++) {
                    mixbins[b][s] += vwd->mixbins[b][s];
                }
            }
        }
    }

    pthread_mutex_unlock(&vwd->lock);
}

static int mcpx_apu_default_vp_worker_count(void)
{
    int cpu_count = MAX(1, (int)sysconf(_SC_NPROCESSORS_ONLN));

    /*
     * Two shares: the frame thread's and one helper's. A helper costs a
     * wake, a wait and a mixbin merge per GP frame (1500 a second), and on
     * big.LITTLE SoCs the third and fourth run on the little cores beside
     * the JIT and the renderers: on the RP5 (Forza's race start, slot 3)
     * four workers took 37% of a core where two took 27% and one 25%, when
     * the frame thread only waited for them. NC_APU_WORKERS overrides
     * (g_apu_config).
     *
     * Desktop Linux: one, the frame thread alone. The helper's wake and wait
     * cost more there than its share of the voices: a race in Forza
     * Motorsport (5.1) kept the frame thread busy 19% of the time with two
     * and 7% with one (Windows on the same machine: 8% with two, 11% with
     * one), and on a slower laptop busy enough to run the output dry.
     */
#if defined(NC_DESKTOP) && !defined(_WIN32)
    (void)cpu_count;
    return 1;
#else
    return cpu_count <= 2 ? 1 : 2;
#endif
}

static void voice_work_init(MCPXAPUState *d)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    int num_workers = g_apu_config.num_workers;
    if (num_workers <= 0) {
        num_workers = mcpx_apu_default_vp_worker_count();
    }
    vwd->num_workers = MAX(1, MIN(num_workers, MAX_VOICE_WORKERS));
    nc_log("hakuX-apu", "voices: %d share%s (the frame thread%s)", vwd->num_workers, vwd->num_workers > 1 ? "s" : "",
           vwd->num_workers > 1 ? " and helpers" : " alone");
    vwd->workers = calloc(vwd->num_workers, sizeof(VoiceWorker));
    vwd->workers_should_exit = false;
    vwd->workers_pending = 0;
    vwd->queue_len = 0;


    pthread_mutex_init(&vwd->lock, NULL);
    pthread_mutex_lock(&vwd->lock);
    apu_cond_init(&vwd->work_finished);
    for (int i = 0; i < vwd->num_workers; i++) {
        apu_cond_init(&vwd->workers[i].cond);
    }
    for (int i = 1; i < vwd->num_workers; i++) {    /* share 0: the frame thread */
        vwd->workers_pending |= 1 << i;
        apu_thread_start(&vwd->workers[i].thread, "apu.voice",
                           voice_worker_thread, d, true);
    }
    while (vwd->workers_pending) {
        pthread_cond_wait(&vwd->work_finished, &vwd->lock);
    }
    pthread_mutex_unlock(&vwd->lock);
}

static void voice_work_finalize(MCPXAPUState *d)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    pthread_mutex_lock(&vwd->lock);
    vwd->workers_should_exit = true;
    for (int i = 0; i < vwd->num_workers; i++) {
        pthread_cond_signal(&vwd->workers[i].cond);
    }
    pthread_mutex_unlock(&vwd->lock);
    for (int i = 1; i < vwd->num_workers; i++) {
        pthread_join(vwd->workers[i].thread, NULL);
    }
    free(vwd->workers);
    vwd->workers = NULL;
}

/* The SE reports the idle voices on its lists to the FE, one trap at a time:
 * on the hardware a pass reports each of them, in list order, and the guest
 * releases those it wants gone. Here a frame reports one, the next after the
 * one reported last (the first when that one has left the lists): reported
 * from the head each frame, an idle voice the guest leaves on its list hid
 * those after it for good. DirectSound releases a voice group (a
 * multichannel stream's linked voices) on its last voice's report alone:
 * MechAssault stops a three-voice stream before a mission and waits for it
 * to stop playing; with its first voice reported every frame and ignored,
 * the group was never released and the game spun in GetStatus, loading,
 * where the guest turned the three off within one frame (the Odin 3). */
static void idle_report(MCPXAPUState *d, const uint16_t *idle, unsigned n)
{
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) {
        if (idle[i] == g_idle_last) {
            k = i + 1 < n ? i + 1 : 0;
            break;
        }
    }
    if (!n || !apu_spin_trylock(&d->vp.fe_spin)) {
        return;
    }
    /* one trap at a time, its method and argument written whole (fe_spin:
     * the guest's methods run on its CPU meanwhile); only tried: the walk
     * holds the voice locks of its batch, which a guest write may be waiting
     * on (a voice not reported now is in a later frame: it stays listed) */
    if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) != NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        fe_method(d, SE2FE_IDLE_VOICE, idle[k]);
        off_notify_now(d, idle[k]);     /* its VOICE_OFF's, with the trap (voice_off) */
        g_idle_last = idle[k];
    }
    apu_spin_unlock(&d->vp.fe_spin);
}

void mcpx_apu_vp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));

    /* The voice table loaded once for the frame: the list walk below and
     * voice_work_dispatch / voice_work_schedule use it. */
    uint8_t *c = apu_ld_acq(&d->vp.voice_cache);

    /* The voices queued this frame: one twice (a cycle in the guest's
     * lists) would take its own lock again and spin forever, and more than
     * MCPX_HW_MAX_VOICES would overrun the queue. */
    uint64_t queued[MCPX_HW_MAX_VOICES / 64] = {0};
    uint64_t listed[MCPX_HW_MAX_VOICES / 64] = {0};   /* the voices on a list now */
    uint16_t idle[MCPX_HW_MAX_VOICES];  /* the idle voices on them, in list order */
    unsigned nidle = 0;

    /* Process all voices, mixing each into the affected MIXBINs */
    for (int list = 0; list < 3; list++) {
        uint64_t top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];
        DPRINTF("list %d current voice %d\n", list, d->regs[current]);

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            /* Make sure not to get stuck... */
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint32_t v = d->regs[current];
            if (!APU_CHECK(v < MCPX_HW_MAX_VOICES)) {
                break;
            }
            d->regs[next] = voice_get_mask_nolock(d, c, v,
                               NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            const uint64_t bit = 1ull << (v % 64);
            listed[v / 64] |= bit;
            if (!APU_CHECK(!(queued[v / 64] & bit))) {
                break;          /* its spinlock is the batch's already */
            }
            /* its state read under its spinlock: a voice the guest is
             * turning on is on the list before it is active, and was
             * reported idle */
            switch (voice_take(d, c, (uint16_t)v)) {
            case VOICE_TAKEN:
                queued[v / 64] |= bit;
                voice_work_enqueue(d, v, list);
                break;
            case VOICE_IDLE:
                if (nidle < ARRAY_SIZE(idle)) {
                    idle[nidle++] = (uint16_t)v;    /* reported below (idle_report) */
                }
                break;
            case VOICE_HELD:
                break;          /* listed: its VOICE_OFF's notification waits */
            }
            d->regs[current] = d->regs[next];
        }
    }
    idle_report(d, idle, nidle);
    /* the guest's VOICE_OFFs (voice_off): notified once their voice has
     * left the lists, its release done */
    for (unsigned w = 0; w < MCPX_HW_MAX_VOICES / 64; w++) {
        for (uint64_t p = __atomic_load_n(&g_off_pending[w], __ATOMIC_ACQUIRE); p; p &= p - 1) {
            const uint16_t v = (uint16_t)(w * 64 + __builtin_ctzll(p));
            if (!(listed[w] & (1ull << (v % 64))) || ++g_off_frames[v] >= OFF_NOTIFY_MAX_FRAMES) {
                off_notify_now(d, v);
            }
        }
    }
    voice_work_dispatch(d, c, mixbins);
    if (c) {
        report_written_voices(d, c);
    }

    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        /* Mix all voices together to hear any audible voice */
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] += isamp[2*i];
            d->monitor.frame_buf[off + i][1] += isamp[2*i+1];
        }

        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float[32][32]));
        if (g_apu_out_channels == 6) {
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                for (int ch = 0; ch < 6; ch++) {
                    const float s = fminf(fmaxf(g_vp_sample6[i][ch], -1.0f), 1.0f);
                    const int acc = g_apu_frame6[off + i][ch] + (int)lrintf(s * 32767.0f);
                    g_apu_frame6[off + i][ch] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
                }
            }
            memset(g_vp_sample6, 0, sizeof(g_vp_sample6));
        }
    }
}

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    attenuation_init();
    filter_coefficient_init();
    lagrange_init();
    for (int i = 0; i < MCPX_HW_MAX_VOICES; i++) {
        apu_spin_init(&d->vp.voice_spinlocks[i]);
    }
    apu_spin_init(&d->vp.fe_spin);

    d->vp.voice_cache = NULL;
    d->vp.ssl_cache = NULL;

    voice_work_init(d);
}

void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    voice_work_finalize(d);

    d->vp.voice_cache = NULL;
    d->vp.ssl_cache = NULL;
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    /* no entries, in no decode yet: a savestate's come with their field, or
     * are taken as two's complement (mcpx_apu_vp_restore_hrtf) */
    memset(&d->vp.hrtf, 0, sizeof(d->vp.hrtf));
    d->vp.hrir_sm = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset(d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    memset(d->vp.held_frames, 0, sizeof(d->vp.held_frames));
    memset(d->vp.voice_wrote, 0, sizeof(d->vp.voice_wrote));
    memset(g_off_pending, 0, sizeof(g_off_pending));
    g_idle_last = 0xFFFF;
    /* each voice as a new voice: a savestate without the voices' processing
     * state starts them so (a frame's fade in from silence) */
    for (int v = 0; v < ARRAY_SIZE(d->vp.filters); v++) {
        hrtf_filter_init(&d->vp.filters[v].hrtf);
        voice_reset_filters(d, v);
        d->vp.filters[v].muted_prev = false;
    }
    refresh_table(&d->vp.voice_cache, 0, 0);
    refresh_table(&d->vp.ssl_cache, 0, 0);
}

/* After a savestate: the HRTF entries' taps in the minifloat decode
 * (hrir_tap_to_float). The entries are stored decoded, so a state from an
 * earlier decode has each tap's byte recovered first: hrir_sm 1 held
 * (sign ? -mag : mag) / 128, hrir_sm 0 (or no such field) two's complement
 * / 128, both exact in a float. The 3D voices read their entry each frame
 * (voice_process), so nothing else needs aiming again. */
void mcpx_apu_vp_restore_hrtf(MCPXAPUState *d)
{
    if (d->vp.hrir_sm != 2) {
        for (unsigned e = 0; e < HRTF_ENTRY_COUNT; e++) {
            for (int ch = 0; ch < 2; ch++) {
                for (int k = 0; k < HRTF_NUM_TAPS; k++) {
                    float *t = &d->vp.hrtf.entries[e].hrir[ch][k];
                    uint8_t b;
                    if (d->vp.hrir_sm == 1) {
                        const int mag = (int)lrintf(fabsf(*t) * 128.0f) & 0x7F;
                        b = (uint8_t)((*t < 0.0f ? 0x80 : 0) | mag);
                    } else {
                        b = (uint8_t)(int8_t)lrintf(*t * 128.0f);
                    }
                    *t = hrir_tap_to_float(b);
                }
            }
        }
        d->vp.hrir_sm = 2;
    }
    unsigned entries = 0;
    for (unsigned e = 0; e < HRTF_ENTRY_COUNT; e++) {
        bool any = d->vp.hrtf.entries[e].itd != 0.0f;
        for (unsigned k = 0; k < HRTF_NUM_TAPS && !any; k++) {
            any = d->vp.hrtf.entries[e].hrir[0][k] != 0.0f || d->vp.hrtf.entries[e].hrir[1][k] != 0.0f;
        }
        entries += any;
    }
    nc_log("hakuX-native-state", "apu: %u HRTF entries hold coefficients", entries);
}
