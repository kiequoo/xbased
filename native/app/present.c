/*
 * hakuX native core, app front end — presenting the HLE GPU's front image.
 *
 * Once per display refresh the window's thread hands its Android window to
 * the HLE GPU's presenter (hw/xbox/hle/vk/swapchain.h), which draws the
 * front frame into it through a Vulkan swapchain: here, the frame's place in
 * the window (letterboxed to the chosen shape) and the anti-aliasing chosen
 * (nc_present_set_fxaa, nc_present_set_upscale).
 */
#include "native/app/app.h"
#include "native/dev/dev.h"
#include "hw/xbox/hle/vk/display.h"
#include "hw/xbox/hle/vk/swapchain.h"

#include <string.h>
#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"
#include <time.h>

static int g_aspect = NC_ASPECT_AUTO;
static bool g_widescreen;
static int g_overscan;          /* percent: > 0 the edges cut off, < 0 a border */
static bool g_fxaa;
static int g_upscale = NC_UPSCALE_BILINEAR;
static bool g_shown;

/* Frame rate from the HLE GPU's presented frames. */
static uint64_t g_win_presented, g_win_start_ns, g_last_present_ns, g_seen_presented;
static float g_fps, g_avg_ms, g_last_ms;

/* debug.hakux.present, for comparisons on the device (read every 64
 * refreshes): "fxaa" / "nofxaa", "fsr" / "bilinear" (the upscaling),
 * "noaverage" (a larger frame sampled bilinearly); otherwise the settings. */
static struct {
    int fxaa, upscale;      /* -1: the setting's */
    bool average;
} g_dbg = {-1, -1, true};

static bool g_show_fps;

void nc_present_set_show_fps(bool on)
{
    g_show_fps = on;
}

int nc_upscale_from_name(const char *v)
{
    return v && (!strcmp(v, "fsr") || !strcmp(v, "sgsr")) ? NC_UPSCALE_FSR : NC_UPSCALE_BILINEAR;
}

bool nc_present_init(void)
{
    hle_disp_gamma_by_consumer(true); /* before the core starts: frames come without it */
    nc_log("hakuX-app", "present: Vulkan; FXAA %s, upscaling %s", g_fxaa ? "on" : "off",
           g_upscale == NC_UPSCALE_FSR ? "FSR" : "bilinear");
    return true;
}

void nc_present_set_fxaa(bool on)
{
    g_fxaa = on;
}

void nc_present_set_upscale(int mode)
{
    g_upscale = mode;
}

void nc_present_set_aspect(int mode)
{
    g_aspect = mode;
}

void nc_present_set_widescreen(bool on)
{
    g_widescreen = on;
}

void nc_present_set_overscan(int pct)
{
    g_overscan = pct < -20 ? -20 : pct > 20 ? 20 : pct;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void count_frames(void)
{
    const uint64_t now = now_ns();
    const uint64_t presented = hle_disp_stats().presented;
    if (!g_win_start_ns) {
        g_win_start_ns = now;
        g_win_presented = g_seen_presented = presented;
        return;
    }
    if (presented > g_seen_presented) {
        if (g_last_present_ns) {
            g_last_ms = (float)(now - g_last_present_ns) / 1e6f / (float)(presented - g_seen_presented);
        }
        g_last_present_ns = now;
        g_seen_presented = presented;
    }
    /* Frames over the time between the first and the last of them seen, a
     * second or more apart: counted in fixed half seconds, a frame more or
     * less in one read as 58 or 62 at a steady 60. */
    if (g_last_present_ns > g_win_start_ns && g_last_present_ns - g_win_start_ns >= 1000000000ull) {
        const uint64_t n = presented - g_win_presented;
        const float ms = (float)(g_last_present_ns - g_win_start_ns) / 1e6f;
        g_fps = (float)n * 1000.0f / ms;
        g_avg_ms = n ? ms / (float)n : 0.0f;
        g_win_start_ns = g_last_present_ns;
        g_win_presented = presented;
    } else if (now - g_win_start_ns >= 2000000000ull) {
        g_fps = 0.0f;                       /* nothing shown for a while */
        g_avg_ms = 0.0f;
        g_win_start_ns = now;
        g_win_presented = presented;
    }
}

void nc_present_stats(float *fps, float *avg_ms, float *last_ms)
{
    *fps = g_fps;
    *avg_ms = g_avg_ms;
    *last_ms = g_last_ms;
}

bool nc_present_shown(void)
{
    return __atomic_load_n(&g_shown, __ATOMIC_RELAXED);
}

static void debug_poll(void)
{
    static unsigned n;
    if (n++ % 64) {
        return;
    }
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.hakux.present", v);
    g_dbg.fxaa = strstr(v, "nofxaa") ? 0 : strstr(v, "fxaa") ? 1 : -1;
    g_dbg.upscale = strstr(v, "fsr") ? NC_UPSCALE_FSR : strstr(v, "bilinear") ? NC_UPSCALE_BILINEAR : -1;
    g_dbg.average = !strstr(v, "noaverage");
}

/* The frame's half size in the window (clip space): letterboxed to the
 * chosen shape, the frame's own (4:3 for 640x480), 4:3, 16:9, or the
 * window's (stretched). */
static void place(uint32_t fw, uint32_t fh, uint32_t ww, uint32_t wh, float *sx, float *sy)
{
    const float wa = (float)ww / (float)wh;
    float fa = (float)fw / (float)fh;
    switch (g_aspect) {
    case NC_ASPECT_4_3:
        fa = 4.0f / 3.0f;
        break;
    case NC_ASPECT_16_9:
        fa = 16.0f / 9.0f;
        break;
    case NC_ASPECT_STRETCH:
        fa = wa;
        break;
    default:
        /* A standard-definition frame (640x480, 720x480: under 16:10) from a
         * console set to widescreen is anamorphic 16:9. */
        if (g_widescreen && fa < 1.6f) {
            fa = 16.0f / 9.0f;
        }
        break;
    }
    *sx = *sy = 1.0f;
    if (wa > fa) {
        *sx = fa / wa;
    } else {
        *sy = wa / fa;
    }
    /* overscan as a TV's: that much of the frame hidden past its place's
     * edges (the frame grown: 10% hidden, 90% shown), or underscan, that
     * much border; the same shape */
    const float k = g_overscan > 0 ? 100.0f / (100.0f - (float)g_overscan) : 1.0f + (float)g_overscan / 100.0f;
    *sx *= k;
    *sy *= k;
}

/* A change of the shape chosen (Settings, the in-game menu's over the
 * picture) eases the frame into its new place over a fifth of a second
 * instead of jumping; the frame's own changes (a game's mode) don't. */
static float g_cur_sx = -1.0f, g_cur_sy, g_from_sx, g_from_sy;
static uint64_t g_tween_t0;
static int g_shape = -1;

static void tween(float *sx, float *sy)
{
    const int shape = g_aspect | (int)g_widescreen << 4 | (g_overscan + 64) << 8;
    const uint64_t now = now_ns();
    if (g_cur_sx < 0.0f) {
        g_shape = shape;
    } else if (shape != g_shape) {
        g_shape = shape;
        g_from_sx = g_cur_sx;
        g_from_sy = g_cur_sy;
        g_tween_t0 = now;
    }
    if (g_tween_t0) {
        const float k = (float)(now - g_tween_t0) / 220e6f;
        if (k >= 1.0f) {
            g_tween_t0 = 0;
        } else {
            const float e = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
            *sx = g_from_sx + (*sx - g_from_sx) * e;
            *sy = g_from_sy + (*sy - g_from_sy) * e;
        }
    }
    g_cur_sx = *sx;
    g_cur_sy = *sy;
}

/* With nothing new to show, the refresh goes by: the panel keeps the image,
 * and the loop waits for the next vsync, as acquiring an image paced it. */
static void wait_next_vsync(void)
{
    const uint64_t now = now_ns(), next = nc_dev_nv2a_next_vsync(now);
    const uint64_t until = next ? next + 500000ull : now + 4000000ull;
    const struct timespec ts = {(time_t)(until / 1000000000ull), (long)(until % 1000000000ull)};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

/* Desktop, the vblank not locked to the display (a variable refresh rate
 * display, G-Sync or FreeSync, or a refresh a frame isn't a whole number
 * of): with nothing new to show, the loop waits for the next frame and
 * shows it as it comes, at most until the next vsync (+0.5 ms; 4 ms without
 * one) for the window's events. Waiting for the vsync instead, a frame went
 * out on the panel's first tick after it came: at 60 fps beside a 280 Hz
 * mode 8 to 26 ms apart, and a variable refresh rate display, whose refresh
 * follows each present, swung with the game at a steady 60. Locked (60 Hz,
 * 120 Hz), the presents keep to the vsyncs as before: shown as they came
 * there, they were a little less even (0.8 to 1.1 ms of jitter, 0.7 to
 * 0.95 before). */
#ifdef NC_DESKTOP
static void wait_next_frame(uint32_t seen)
{
    const uint64_t now = now_ns(), next = nc_dev_nv2a_next_vsync(now);
    const uint64_t until = next ? next + 500000ull : now + 4000000ull;
    const int ms = until > now ? (int)((until - now + 999999ull) / 1000000ull) : 1;
    hle_disp_wait_front(seen, ms);
}
#else
static void wait_next_frame(uint32_t seen)
{
    (void)seen;
}
#endif

bool nc_present_frame(struct ANativeWindow *win)
{
#ifdef NC_DESKTOP
    const uint32_t fronts = hle_disp_fronts();     /* before the look: one coming meanwhile ends the wait */
#else
    const uint32_t fronts = 0;
#endif
    count_frames();
    debug_poll();
    const bool fxaa = g_dbg.fxaa < 0 ? g_fxaa : g_dbg.fxaa != 0;
    const int upscale = g_dbg.upscale < 0 ? g_upscale : g_dbg.upscale;
    /* what places and filters the frame, beside it and the window */
    const uint64_t key = (uint64_t)fxaa | (uint64_t)upscale << 1 | (uint64_t)g_dbg.average << 4 |
                         (uint64_t)g_widescreen << 5 | (uint64_t)(uint32_t)g_aspect << 8 |
                         (uint64_t)(uint32_t)(g_overscan + 64) << 16;
#ifdef NC_DESKTOP
    const bool as_they_come = !nc_dev_nv2a_vblank_locked();
#else
    const bool as_they_come = false;
#endif
    if (!g_tween_t0 && hle_present_unchanged(win, key)) {
        if (as_they_come) {
            wait_next_frame(fronts);
        } else {
            nc_dev_nv2a_display_pickup(now_ns()); /* it looked: the vblank keeps clear of it */
            wait_next_vsync();
        }
        return true;
    }
    uint32_t ww = 0, wh = 0, fw = 0, fh = 0;
    if (!hle_present_begin(win, &ww, &wh)) {
        return false;
    }
    if (!as_they_come) {
        nc_dev_nv2a_display_pickup(now_ns()); /* the vblank keeps clear of it */
    }
    float sx = 1.0f, sy = 1.0f;
    if (hle_present_frame(&fw, &fh)) {
        place(fw, fh, ww, wh, &sx, &sy);
        tween(&sx, &sy);
        __atomic_store_n(&g_shown, true, __ATOMIC_RELAXED);
    }
    hle_present_set_osd(g_show_fps ? (g_fps > 0 ? g_fps : 0.01f) : 0);
    hle_present_end(sx, sy, fxaa, upscale, g_dbg.average, key);
    return true;
}
