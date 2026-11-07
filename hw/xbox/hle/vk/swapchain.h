/*
 * hakuX HLE GPU — the window's presenter (vk/swapchain.c): the front frame
 * drawn into an Android window through a Vulkan swapchain.
 */
#ifndef HW_XBOX_HLE_VK_SWAPCHAIN_H
#define HW_XBOX_HLE_VK_SWAPCHAIN_H

#include <stdbool.h>
#include <stdint.h>

struct ANativeWindow;

/* A frame smaller than its place in the window: scaled up bilinearly, or by
 * AMD FidelityFX Super Resolution 1 (a larger one is always averaged down). */
enum { HLE_UPSCALE_BILINEAR, HLE_UPSCALE_FSR };

/* This refresh would put up what win already shows (the same front frame,
 * gamma ramp, window and swapchain, and key: the caller's placement and
 * filters, as at the last present that went through): the caller may skip
 * it, the panel keeping the image. A 30 fps title on a 120 Hz panel drew,
 * submitted and presented each frame four times. */
bool hle_present_unchanged(struct ANativeWindow *win, uint64_t key);

/* Each display refresh, on the window's thread, these three in order.
 * begin: an image of win's swapchain (made or remade as the window comes,
 * goes or changes size), waiting for one: the panel's pace. *w, *h: the
 * window's size. false: nothing to draw into (the HLE GPU not up yet, no
 * window): skip this refresh, no frame or end. */
bool hle_present_begin(struct ANativeWindow *win, uint32_t *w, uint32_t *h);
/* frame: the front frame, held until a newer one is taken; its size, false
 * when there is none yet. */
bool hle_present_frame(uint32_t *w, uint32_t *h);
/* end: the frame drawn into the window's middle (sx, sy: its half size in
 * clip space, 1 the whole window), the rest black, through the display's
 * gamma ramp, and presented. fxaa: FXAA over each new frame first; average:
 * false samples a frame larger than its place bilinearly too (comparisons).
 * key: what hle_present_unchanged compares next. */
void hle_present_end(float sx, float sy, bool fxaa, int upscale, bool average, uint64_t key);

/* A picture of the next refresh that goes through, as the window shows it,
 * written to path (a BMP) on the window's thread. */
void hle_present_shot(const char *path);
/* The CRT filter on the window's picture: 0 none, 1 soft scanlines, 2 a
 * TV, 3 an arcade monitor (any thread; the next refresh). */
void hle_present_set_crt(int kind);
/* The frame rate shown in the window's corner, 0 none (any thread). */
void hle_present_set_osd(float fps);

/* The overlay: the front end's interface composited over the picture by the
 * presenter itself (the in-game menu, the transitions, notices), so the
 * window never changes hands. Triangles in window pixels, each command a
 * run of them with one texture, blending and clip. All of it on the
 * window's thread, as the refreshes. With none set, a refresh is drawn as
 * it was before there was one. */
typedef struct HleOvVert {
    float x, y, u, v;
    uint32_t rgba;              /* R | G << 8 | B << 16 | A << 24 */
} HleOvVert;
typedef struct HleOvCmd {
    uint32_t tex;               /* hle_ov_tex_new's, 0 white, or HLE_OV_SCENE* */
    uint32_t blend;             /* HLE_OV_BLEND, _ADD, _PREMUL */
    int32_t clip[4];            /* x, y, w, h in window pixels; w 0: none */
    uint32_t first, count;      /* vertices (three a triangle) */
} HleOvCmd;
enum { HLE_OV_BLEND, HLE_OV_ADD, HLE_OV_PREMUL };
/* the picture as the window would show it this refresh (letterboxed, its
 * filters, the gamma ramp), and the same blurred: textures of the overlay's
 * while it covers the window (HLE_OV_COVER) */
#define HLE_OV_SCENE 0xFFFFFF01u
#define HLE_OV_SCENE_BLUR 0xFFFFFF02u
/* the overlay draws the whole window (the picture only where it puts it) */
#define HLE_OV_COVER 1u

/* A w x h RGBA texture, transparent until written; 0: none left. */
uint32_t hle_ov_tex_new(uint32_t w, uint32_t h);
/* Its pixels at x, y (w x h, rows stride bytes apart), copied now and
 * uploaded with the next refresh. */
void hle_ov_tex_put(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *rgba, uint32_t stride);
void hle_ov_tex_free(uint32_t id);
/* The overlay of the refreshes from the next one (copied), flags HLE_OV_*;
 * nc 0: none. While one is set every refresh is drawn. */
void hle_present_set_overlay(const HleOvVert *v, uint32_t nv, const HleOvCmd *c, uint32_t nc, uint32_t flags);
/* Refreshes presented since the start (one went through: the window shows
 * the presenter's). */
uint64_t hle_present_count(void);
/* The overlay can be drawn (the presenter and its window up, its pipelines
 * made): on the window's thread. */
bool hle_ov_ready(void);

#endif
