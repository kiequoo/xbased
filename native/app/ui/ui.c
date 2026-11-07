/*
 * hakuX front end — the dashboard's drawing (ui.h).
 *
 * Text: glyphs rasterized by stb_truetype at the size they are drawn at
 * (pixels: crisp at any window size) into one atlas texture, shelf-packed,
 * looked up in an open-addressed table. A codepoint the bundled font lacks
 * (CJK) comes from the first system font that has it (read the first time
 * one is needed).
 *
 * Two ways out, the same drawing: an SDL_Renderer on the dashboard's window,
 * or in a game's process the overlay of the game's presenter (swapchain.h):
 * the triangles of a frame gathered and handed over at its end, the window
 * the presenter's throughout. The overlay's textures are the presenter's,
 * the game's picture among them (ui_scene).
 */
#include "native/app/ui/ui.h"
#include "hw/xbox/hle/vk/swapchain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define GLYPHS 8192         /* table slots (a power of two) */
#define MAX_FACES 8

enum { UB_BLEND = HLE_OV_BLEND, UB_ADD = HLE_OV_ADD, UB_PREMUL = HLE_OV_PREMUL };

struct UiTex {
    SDL_Texture *t;         /* the renderer's */
    uint32_t ov;            /* the overlay's */
    int w, h;
    int blend;              /* UB_* */
};

typedef struct {
    stbtt_fontinfo info;
    unsigned char *data;
    bool ok;
} Face;

typedef struct {
    uint32_t key;           /* codepoint | style << 21 | px << 23 (0: empty) */
    uint16_t x, y, w, h;
    int16_t xoff, yoff;
    float adv;
} Glyph;

static struct {
    SDL_Window *win;
    SDL_Renderer *r;
    UiTex *atlas, *glow;
    int atlas_n;            /* its side */
    Face face[MAX_FACES];   /* 0 regular, 1 bold, 2.. fallbacks */
    int nfaces;
    bool fallbacks;         /* the system's fonts looked for */
    Glyph g[GLYPHS];
    int shelf_x, shelf_y, shelf_h;
    float scale;            /* pixels per 720p unit */
    int pw, ph;
    uint64_t t0;
    float alpha;            /* ui_set_alpha */
    SDL_Texture *targets[8];   /* the layers' render targets under the one drawn into */
    int ntargets;
    /* the overlay's */
    bool ov, cover;
    void (*present)(void);
    void (*size)(int *w, int *h);
    int32_t clip[4];
    HleOvVert *v;
    HleOvCmd *c;
    int nv, cv, nc, cc;
} U;

static UiTex g_scene = {.ov = HLE_OV_SCENE, .blend = UB_BLEND};
static UiTex g_scene_blur = {.ov = HLE_OV_SCENE_BLUR, .blend = UB_BLEND};

UiColor ui_mix(UiColor a, UiColor b, float t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return (UiColor){(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                     (uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.a + (b.a - a.a) * t)};
}

static SDL_FColor fc(UiColor c)
{
    return (SDL_FColor){c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
}

static float clamp01(float x)
{
    return x < 0 ? 0 : x > 1 ? 1 : x;
}

/* ------------------------------------------------------------ the two ways out */

/* A vertex's colour as drawn: the fade applied, premultiplied for a
 * premultiplied texture. */
static SDL_FColor vcol(SDL_FColor c, int blend)
{
    c.a *= U.alpha;
    if (blend == UB_PREMUL) {
        c.r *= c.a;
        c.g *= c.a;
        c.b *= c.a;
    }
    return c;
}

static uint32_t pack(SDL_FColor c)
{
    const uint32_t r = (uint32_t)lrintf(clamp01(c.r) * 255), g = (uint32_t)lrintf(clamp01(c.g) * 255);
    const uint32_t b = (uint32_t)lrintf(clamp01(c.b) * 255), a = (uint32_t)lrintf(clamp01(c.a) * 255);
    return r | g << 8 | b << 16 | a << 24;
}

/* Room for n more of the overlay's vertices in the command of texture tex
 * (a new one when the texture, blending or clip changes). */
static HleOvVert *ov_room(uint32_t tex, int blend, int n)
{
    if (U.nv + n > U.cv) {
        U.cv = (U.nv + n) * 2;
        U.v = SDL_realloc(U.v, (size_t)U.cv * sizeof(*U.v));
    }
    HleOvCmd *last = U.nc ? &U.c[U.nc - 1] : NULL;
    if (last && last->tex == tex && last->blend == (uint32_t)blend && !memcmp(last->clip, U.clip, sizeof(U.clip)) &&
        last->first + last->count == (uint32_t)U.nv) {
        last->count += (uint32_t)n;
    } else {
        if (U.nc == U.cc) {
            U.cc = U.cc ? U.cc * 2 : 64;
            U.c = SDL_realloc(U.c, (size_t)U.cc * sizeof(*U.c));
        }
        HleOvCmd *c = &U.c[U.nc++];
        c->tex = tex;
        c->blend = (uint32_t)blend;
        memcpy(c->clip, U.clip, sizeof(U.clip));
        c->first = (uint32_t)U.nv;
        c->count = (uint32_t)n;
    }
    HleOvVert *v = U.v + U.nv;
    U.nv += n;
    return v;
}

/* Triangles in window pixels, textured by t (NULL: plain colour). */
static void geom(UiTex *t, const SDL_Vertex *v, int nv, const int *idx, int ni)
{
    const int blend = t ? t->blend : UB_BLEND;
    if (U.ov) {
        if (t && !t->ov) {
            return;
        }
        HleOvVert *o = ov_room(t ? t->ov : 0, blend, ni);
        for (int i = 0; i < ni; i++) {
            const SDL_Vertex *s = &v[idx[i]];
            o[i] = (HleOvVert){s->position.x, s->position.y, s->tex_coord.x, s->tex_coord.y, pack(vcol(s->color, blend))};
        }
        return;
    }
    if ((U.alpha >= 1.0f && blend != UB_PREMUL) || nv > 512) {
        SDL_RenderGeometry(U.r, t ? t->t : NULL, v, nv, idx, ni);
        return;
    }
    SDL_Vertex f[512];
    for (int i = 0; i < nv; i++) {
        f[i] = v[i];
        f[i].color = vcol(v[i].color, blend);
    }
    SDL_RenderGeometry(U.r, t ? t->t : NULL, f, nv, idx, ni);
}

/* src of t (its pixels; NULL all) into dst (window pixels), modulated by c. */
static void blit(UiTex *t, const SDL_FRect *src, const SDL_FRect *dst, UiColor c)
{
    if (!t) {
        return;
    }
    if (U.ov) {
        if (!t->ov) {
            return;
        }
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        if (src && t->w > 0 && t->h > 0) {
            u0 = src->x / t->w;
            v0 = src->y / t->h;
            u1 = (src->x + src->w) / t->w;
            v1 = (src->y + src->h) / t->h;
        }
        const uint32_t col = pack(vcol(fc(c), t->blend));
        const float x0 = dst->x, y0 = dst->y, x1 = dst->x + dst->w, y1 = dst->y + dst->h;
        HleOvVert *o = ov_room(t->ov, t->blend, 6);
        o[0] = (HleOvVert){x0, y0, u0, v0, col};
        o[1] = (HleOvVert){x1, y0, u1, v0, col};
        o[2] = (HleOvVert){x1, y1, u1, v1, col};
        o[3] = o[0];
        o[4] = o[2];
        o[5] = (HleOvVert){x0, y1, u0, v1, col};
        return;
    }
    float a = c.a / 255.f * U.alpha;
    if (t->blend == UB_PREMUL) {
        c.r = (uint8_t)(c.r * a);
        c.g = (uint8_t)(c.g * a);
        c.b = (uint8_t)(c.b * a);
    }
    SDL_SetTextureColorMod(t->t, c.r, c.g, c.b);
    SDL_SetTextureAlphaModFloat(t->t, a);
    SDL_RenderTexture(U.r, t->t, src, dst);
}

static void tex_blend(UiTex *t, int blend)
{
    t->blend = blend;
    if (t->t) {
        SDL_SetTextureBlendMode(t->t, blend == UB_ADD      ? SDL_BLENDMODE_ADD
                                      : blend == UB_PREMUL ? SDL_BLENDMODE_BLEND_PREMULTIPLIED
                                                           : SDL_BLENDMODE_BLEND);
    }
}

/* A w x h texture (px rows pitch bytes apart; NULL: left transparent). */
static UiTex *tex_new(int w, int h, const uint8_t *px, int pitch)
{
    if (w <= 0 || h <= 0) {
        return NULL;
    }
    UiTex *t = SDL_calloc(1, sizeof(*t));
    t->w = w;
    t->h = h;
    if (U.ov) {
        t->ov = hle_ov_tex_new((uint32_t)w, (uint32_t)h);
        if (!t->ov) {
            SDL_free(t);
            return NULL;
        }
        if (px) {
            hle_ov_tex_put(t->ov, 0, 0, (uint32_t)w, (uint32_t)h, px, (uint32_t)pitch);
        }
    } else {
        t->t = SDL_CreateTexture(U.r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!t->t) {
            SDL_free(t);
            return NULL;
        }
        if (px) {
            SDL_UpdateTexture(t->t, NULL, px, pitch);
        }
        SDL_SetTextureScaleMode(t->t, SDL_SCALEMODE_LINEAR);
    }
    tex_blend(t, UB_BLEND);
    return t;
}

static void tex_put(UiTex *t, const SDL_Rect *rc, const uint8_t *px, int pitch)
{
    if (t->ov) {
        hle_ov_tex_put(t->ov, (uint32_t)rc->x, (uint32_t)rc->y, (uint32_t)rc->w, (uint32_t)rc->h, px, (uint32_t)pitch);
    } else if (t->t) {
        SDL_UpdateTexture(t->t, rc, px, pitch);
    }
}

UiTex *ui_texture_rgba(const uint8_t *rgba, int w, int h)
{
    return tex_new(w, h, rgba, w * 4);
}

UiTex *ui_texture_surface(SDL_Surface *s)
{
    SDL_Surface *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (!c) {
        return NULL;
    }
    UiTex *t = tex_new(c->w, c->h, c->pixels, c->pitch);
    SDL_DestroySurface(c);
    return t;
}

void ui_texture_free(UiTex *t)
{
    if (!t || t == &g_scene || t == &g_scene_blur) {
        return;
    }
    if (t->t) {
        SDL_DestroyTexture(t->t);
    }
    if (t->ov) {
        hle_ov_tex_free(t->ov);
    }
    SDL_free(t);
}

void ui_texture_size(const UiTex *t, float *w, float *h)
{
    *w = t ? (float)t->w : 0;
    *h = t ? (float)t->h : 0;
}

UiTex *ui_scene(bool blurred)
{
    if (!U.ov) {
        return NULL;
    }
    UiTex *t = blurred ? &g_scene_blur : &g_scene;
    t->w = U.pw;
    t->h = U.ph;
    return t;
}

void ui_fill(UiTex *t, UiColor c)
{
    const SDL_FRect d = {0, 0, (float)U.pw, (float)U.ph};
    blit(t, NULL, &d, c);
}

void ui_set_alpha(float a)
{
    U.alpha = clamp01(a);
}

void ui_cover(bool on)
{
    U.cover = on;
}

bool ui_is_overlay(void)
{
    return U.ov;
}

/* ------------------------------------------------------------ fonts */

static unsigned char *read_file(const char *path)
{
    size_t n = 0;
    return SDL_LoadFile(path, &n);
}

static bool face_load(Face *f, const char *path, int index)
{
    f->data = read_file(path);
    if (!f->data) {
        return false;
    }
    const int off = stbtt_GetFontOffsetForIndex(f->data, index);
    f->ok = off >= 0 && stbtt_InitFont(&f->info, f->data, off);
    if (!f->ok) {
        SDL_free(f->data);
        f->data = NULL;
    }
    return f->ok;
}

static void fallback_fonts(void)
{
    static const char *const k_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\malgun.ttf",
        "C:\\Windows\\Fonts\\msgothic.ttc", "C:\\Windows\\Fonts\\segoeui.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
    };
    U.fallbacks = true;
    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]) && U.nfaces < MAX_FACES; i++) {
        if (face_load(&U.face[U.nfaces], k_paths[i], 0)) {
            U.nfaces++;
        }
    }
}

static UiTex *make_glow(void)
{
    enum { N = 128 };
    uint8_t *px = SDL_malloc(N * N * 4);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            const float dx = (x + 0.5f) / N * 2 - 1, dy = (y + 0.5f) / N * 2 - 1;
            float d = sqrtf(dx * dx + dy * dy);
            float a = d >= 1 ? 0 : (1 - d) * (1 - d);
            a = a * a * 0.6f + a * 0.4f;
            uint8_t *p = px + (y * N + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (uint8_t)(a * 255);
        }
    }
    UiTex *t = ui_texture_rgba(px, N, N);
    SDL_free(px);
    if (t) {
        tex_blend(t, UB_ADD);
    }
    return t;
}

/* the fonts and the textures every frame may use */
static bool init_common(const char *font_dir)
{
    char p[1024];
    snprintf(p, sizeof(p), "%sPlay-Regular.ttf", font_dir);
    if (!face_load(&U.face[0], p, 0)) {
        SDL_Log("ui: no font at %s", p);
        return false;
    }
    snprintf(p, sizeof(p), "%sPlay-Bold.ttf", font_dir);
    if (!face_load(&U.face[1], p, 0)) {
        U.face[1] = U.face[0];
    }
    U.nfaces = 2;
    U.atlas = tex_new(U.atlas_n, U.atlas_n, NULL, 0);
    U.glow = make_glow();
    U.t0 = SDL_GetTicksNS();
    U.alpha = 1.0f;
    return U.atlas != NULL;
}

bool ui_init(SDL_Window *win, const char *font_dir)
{
    memset(&U, 0, sizeof(U));
    U.win = win;
    U.r = SDL_CreateRenderer(win, NULL);
    if (!U.r) {
        U.r = SDL_CreateRenderer(win, "vulkan");   /* a Vulkan window (the game's, on Linux) */
    }
    if (!U.r) {
        SDL_Log("ui: no renderer: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(U.r, 1);
    /* shapes (untextured geometry) blend with what is under them */
    SDL_SetRenderDrawBlendMode(U.r, SDL_BLENDMODE_BLEND);
    U.atlas_n = 2048;
    return init_common(font_dir);
}

bool ui_init_overlay(const char *font_dir, void (*present)(void), void (*size)(int *w, int *h))
{
    memset(&U, 0, sizeof(U));
    U.ov = true;
    U.present = present;
    U.size = size;
    U.atlas_n = 1024;       /* a menu's glyphs, at any window size */
    return init_common(font_dir);
}

void ui_shutdown(void)
{
    ui_texture_free(U.atlas);
    ui_texture_free(U.glow);
    if (U.ov && U.nc) {
        hle_present_set_overlay(NULL, 0, NULL, 0, 0);
    }
    if (U.r) {
        SDL_DestroyRenderer(U.r);
    }
    for (int i = 0; i < U.nfaces; i++) {
        if (U.face[i].data && !(i == 1 && U.face[1].data == U.face[0].data)) {
            SDL_free(U.face[i].data);
        }
    }
    SDL_free(U.v);
    SDL_free(U.c);
    memset(&U, 0, sizeof(U));
}

SDL_Renderer *ui_renderer(void)
{
    return U.r;
}

void ui_begin(void)
{
    if (U.ov) {
        U.size(&U.pw, &U.ph);
        if (U.pw < 1 || U.ph < 1) {
            U.pw = 1280;
            U.ph = 720;
        }
        U.nv = U.nc = 0;
        memset(U.clip, 0, sizeof(U.clip));
        U.cover = false;
    } else {
        SDL_GetCurrentRenderOutputSize(U.r, &U.pw, &U.ph);
    }
    U.scale = U.ph / 720.0f;
    if (U.pw / U.scale < 960) {
        U.scale = U.pw / 960.0f;   /* narrow windows: at least 960 units wide */
    }
    U.alpha = 1.0f;
    if (!U.ov) {
        SDL_SetRenderDrawColor(U.r, 0, 0, 0, 255);
        SDL_RenderClear(U.r);
    }
}

void ui_overlay_submit(bool cover)
{
    if (!U.ov) {
        return;
    }
    if (cover && !U.nc) {
        /* nothing drawn, still covering: black, not the picture */
        HleOvVert *o = ov_room(0, UB_BLEND, 3);
        memset(o, 0, 3 * sizeof(*o));
    }
    hle_present_set_overlay(U.v, (uint32_t)U.nv, U.c, (uint32_t)U.nc, cover ? HLE_OV_COVER : 0);
}

void ui_overlay_off(void)
{
    if (U.ov) {
        hle_present_set_overlay(NULL, 0, NULL, 0, 0);
    }
}

void ui_end(void)
{
    if (U.ov) {
        ui_overlay_submit(U.cover);
        if (U.present) {
            U.present();
        }
        return;
    }
    SDL_RenderPresent(U.r);
}

void ui_shot(const char *path)
{
    if (U.ov) {
        hle_present_shot(path);
        return;
    }
    SDL_Surface *s = SDL_RenderReadPixels(U.r, NULL);
    if (s) {
        SDL_SaveBMP(s, path);
        SDL_DestroySurface(s);
    }
}

float ui_w(void)
{
    return U.pw / U.scale;
}

float ui_h(void)
{
    return U.ph / U.scale;
}

float ui_scale(void)
{
    return U.scale;
}

double ui_time(void)
{
    return (SDL_GetTicksNS() - U.t0) / 1e9;
}

/* ------------------------------------------------------------ text */

static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) {
        c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63);
        p += 3;
    } else if (c >= 0xE0 && p[0] && p[1]) {
        c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63);
        p += 2;
    } else if (c >= 0xC0 && p[0]) {
        c = (c & 31) << 6 | (p[0] & 63);
        p += 1;
    }
    *s = (const char *)p;
    return c;
}

static void atlas_reset(void)
{
    memset(U.g, 0, sizeof(U.g));
    U.shelf_x = U.shelf_y = U.shelf_h = 0;
}

static const Glyph *glyph(uint32_t cp, int style, int px)
{
    if (px > 255) {
        px = 255;
    }
    const uint32_t key = (cp & 0x1FFFFF) | (uint32_t)(style & 3) << 21 | (uint32_t)px << 23;
    uint32_t h = key * 2654435761u;
    for (int probe = 0; probe < GLYPHS; probe++) {
        Glyph *g = &U.g[(h + probe) & (GLYPHS - 1)];
        if (g->key == key) {
            return g;
        }
        if (g->key) {
            continue;
        }
        /* not cached: the first face with the codepoint */
        Face *f = &U.face[style == UI_FONT_BOLD ? 1 : 0];
        int gi = stbtt_FindGlyphIndex(&f->info, cp);
        if (!gi && !U.fallbacks && cp > ' ') {
            fallback_fonts();
        }
        for (int i = 2; !gi && i < U.nfaces; i++) {
            if ((gi = stbtt_FindGlyphIndex(&U.face[i].info, cp))) {
                f = &U.face[i];
            }
        }
        const float sc = stbtt_ScaleForPixelHeight(&f->info, (float)px);
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
        stbtt_GetGlyphBitmapBox(&f->info, gi, sc, sc, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, hh = y1 - y0;
        if (U.shelf_x + w + 1 > U.atlas_n) {
            U.shelf_x = 0;
            U.shelf_y += U.shelf_h + 1;
            U.shelf_h = 0;
        }
        if (U.shelf_y + hh + 1 > U.atlas_n) {
            atlas_reset();      /* full: start over (glyphs redraw as needed) */
            return glyph(cp, style, px);
        }
        g->key = key;
        g->x = (uint16_t)U.shelf_x;
        g->y = (uint16_t)U.shelf_y;
        g->w = (uint16_t)w;
        g->h = (uint16_t)hh;
        g->xoff = (int16_t)x0;
        g->yoff = (int16_t)y0;
        g->adv = adv * sc;
        /* the face's ascent: baselines line up across faces */
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
        g->yoff = (int16_t)(y0 + (int)lrintf(asc * sc));
        if (w > 0 && hh > 0) {
            unsigned char *a = SDL_malloc((size_t)w * hh);
            stbtt_MakeGlyphBitmap(&f->info, a, w, hh, w, sc, sc, gi);
            uint8_t *rgba = SDL_malloc((size_t)w * hh * 4);
            for (int i = 0; i < w * hh; i++) {
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
                rgba[i * 4 + 3] = a[i];
            }
            const SDL_Rect rc = {U.shelf_x, U.shelf_y, w, hh};
            tex_put(U.atlas, &rc, rgba, w * 4);
            SDL_free(a);
            SDL_free(rgba);
        }
        U.shelf_x += w + 1;
        if (hh > U.shelf_h) {
            U.shelf_h = hh;
        }
        return g;
    }
    atlas_reset();
    return glyph(cp, style, px);
}

static float text_run(float x, float y, const char *s, float size, int font, UiColor c, bool draw, float max_w,
                      const char **stop)
{
    const int px = (int)lrintf(size * U.scale);
    float pen = x * U.scale;
    const float start = pen;
    while (*s) {
        const char *at = s;
        const uint32_t cp = utf8_next(&s);
        if (cp == '\n') {
            break;
        }
        const Glyph *g = glyph(cp, font, px);
        if (max_w > 0 && (pen + g->adv - start) / U.scale > max_w) {
            if (stop) {
                *stop = at;
            }
            return (pen - start) / U.scale;
        }
        if (draw && g->w) {
            const SDL_FRect src = {g->x, g->y, g->w, g->h};
            const SDL_FRect dst = {floorf(pen) + g->xoff, floorf(y * U.scale) + g->yoff, g->w, g->h};
            blit(U.atlas, &src, &dst, c);
        }
        pen += g->adv;
    }
    if (stop) {
        *stop = NULL;
    }
    return (pen - start) / U.scale;
}

float ui_text_width(const char *s, float size, int font)
{
    return text_run(0, 0, s, size, font, UI_WHITE, false, 0, NULL);
}

float ui_text(float x, float y, const char *s, float size, int font, UiColor c, int align)
{
    if (align != UI_ALIGN_LEFT) {
        const float w = ui_text_width(s, size, font);
        x -= align == UI_ALIGN_CENTER ? w / 2 : w;
    }
    return text_run(x, y, s, size, font, c, true, 0, NULL);
}

float ui_text_fit(float x, float y, const char *s, float size, int font, UiColor c, int align, float max_w)
{
    const float w = ui_text_width(s, size, font);
    if (w <= max_w) {
        return ui_text(x, y, s, size, font, c, align);
    }
    const float ell = ui_text_width("...", size, font);
    const char *stop = NULL;
    char buf[512];
    text_run(0, 0, s, size, font, c, false, max_w - ell, &stop);
    const size_t n = stop ? (size_t)(stop - s) : strlen(s);
    snprintf(buf, sizeof(buf), "%.*s...", (int)(n < sizeof(buf) - 4 ? n : sizeof(buf) - 4), s);
    return ui_text(x, y, buf, size, font, c, align == UI_ALIGN_CENTER ? UI_ALIGN_CENTER : align);
}

/* ------------------------------------------------------------ shapes */

static void quad(float x, float y, float w, float h, UiColor c0, UiColor c1, UiColor c2, UiColor c3)
{
    const float s = U.scale;
    SDL_Vertex v[4] = {
        {{x * s, y * s}, fc(c0), {0, 0}},
        {{(x + w) * s, y * s}, fc(c1), {0, 0}},
        {{(x + w) * s, (y + h) * s}, fc(c2), {0, 0}},
        {{x * s, (y + h) * s}, fc(c3), {0, 0}},
    };
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    geom(NULL, v, 4, idx, 6);
}

void ui_rect(float x, float y, float w, float h, UiColor c)
{
    quad(x, y, w, h, c, c, c, c);
}

void ui_rect_grad(float x, float y, float w, float h, UiColor top, UiColor bottom)
{
    quad(x, y, w, h, top, top, bottom, bottom);
}

void ui_rect_hgrad(float x, float y, float w, float h, UiColor left, UiColor right)
{
    quad(x, y, w, h, left, right, right, left);
}

#define ARC 8   /* segments a corner */

static int round_path(float x, float y, float w, float h, float r, SDL_FPoint *pt)
{
    r = fminf(r, fminf(w, h) / 2);
    const float cx[4] = {x + w - r, x + w - r, x + r, x + r}, cy[4] = {y + r, y + h - r, y + h - r, y + r};
    int n = 0;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i <= ARC; i++) {
            const float a = (float)(-M_PI / 2 + (k + (float)i / ARC) * M_PI / 2);
            pt[n++] = (SDL_FPoint){cx[k] + cosf(a) * r, cy[k] + sinf(a) * r};
        }
    }
    return n;
}

void ui_round(float x, float y, float w, float h, float r, UiColor c)
{
    SDL_FPoint pt[4 * (ARC + 1)];
    const int n = round_path(x, y, w, h, r, pt);
    SDL_Vertex v[4 * (ARC + 1) + 1];
    int idx[3 * 4 * (ARC + 1)];
    const SDL_FColor col = fc(c);
    v[0] = (SDL_Vertex){{(x + w / 2) * U.scale, (y + h / 2) * U.scale}, col, {0, 0}};
    for (int i = 0; i < n; i++) {
        v[i + 1] = (SDL_Vertex){{pt[i].x * U.scale, pt[i].y * U.scale}, col, {0, 0}};
        idx[i * 3] = 0;
        idx[i * 3 + 1] = i + 1;
        idx[i * 3 + 2] = (i + 1) % n + 1;
    }
    geom(NULL, v, n + 1, idx, n * 3);
}

static void strip(const SDL_FPoint *pt, int n, float t, UiColor c, bool closed)
{
    SDL_Vertex v[2 * 4 * (ARC + 1) + 4];
    int idx[6 * 4 * (ARC + 1) + 6];
    if (n > 4 * (ARC + 1)) {
        n = 4 * (ARC + 1);
    }
    const SDL_FColor col = fc(c);
    for (int i = 0; i < n; i++) {
        const SDL_FPoint a = pt[(i + n - 1) % n], b = pt[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y;
        const float l = sqrtf(dx * dx + dy * dy);
        dx = l > 0 ? dx / l : 1;
        dy = l > 0 ? dy / l : 0;
        const float nx = -dy * t / 2, ny = dx * t / 2;
        v[i * 2] = (SDL_Vertex){{(pt[i].x + nx) * U.scale, (pt[i].y + ny) * U.scale}, col, {0, 0}};
        v[i * 2 + 1] = (SDL_Vertex){{(pt[i].x - nx) * U.scale, (pt[i].y - ny) * U.scale}, col, {0, 0}};
    }
    int k = 0;
    for (int i = 0; i < (closed ? n : n - 1); i++) {
        const int a = i * 2, b = ((i + 1) % n) * 2;
        idx[k++] = a;
        idx[k++] = a + 1;
        idx[k++] = b;
        idx[k++] = b;
        idx[k++] = a + 1;
        idx[k++] = b + 1;
    }
    geom(NULL, v, n * 2, idx, k);
}

void ui_round_outline(float x, float y, float w, float h, float r, float t, UiColor c)
{
    SDL_FPoint pt[4 * (ARC + 1)];
    strip(pt, round_path(x, y, w, h, r, pt), t, c, true);
}

/* a line in window pixels, t wide */
static void line_px(float x0, float y0, float x1, float y1, float t, UiColor c)
{
    float dx = x1 - x0, dy = y1 - y0;
    const float l = sqrtf(dx * dx + dy * dy);
    if (l <= 0) {
        return;
    }
    const float nx = -dy / l * t / 2, ny = dx / l * t / 2;
    const SDL_FColor col = fc(c);
    SDL_Vertex v[4] = {{{x0 + nx, y0 + ny}, col, {0, 0}},
                       {{x1 + nx, y1 + ny}, col, {0, 0}},
                       {{x1 - nx, y1 - ny}, col, {0, 0}},
                       {{x0 - nx, y0 - ny}, col, {0, 0}}};
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    geom(NULL, v, 4, idx, 6);
}

void ui_line(float x0, float y0, float x1, float y1, float t, UiColor c)
{
    const float s = U.scale;
    line_px(x0 * s, y0 * s, x1 * s, y1 * s, t * s, c);
}

void ui_glow(float cx, float cy, float r, UiColor c)
{
    const SDL_FRect d = {(cx - r) * U.scale, (cy - r) * U.scale, 2 * r * U.scale, 2 * r * U.scale};
    blit(U.glow, NULL, &d, c);
}

void ui_circle(float cx, float cy, float r, UiColor c)
{
    ui_round(cx - r, cy - r, 2 * r, 2 * r, r, c);
}

void ui_ring(float cx, float cy, float rx, float ry, float rot, float t, UiColor c)
{
    SDL_FPoint pt[4 * (ARC + 1)];
    const int n = 4 * (ARC + 1);
    const float cr = cosf(rot), sr = sinf(rot);
    for (int i = 0; i < n; i++) {
        const float a = (float)(2 * M_PI * i / n), ex = cosf(a) * rx, ey = sinf(a) * ry;
        pt[i] = (SDL_FPoint){cx + ex * cr - ey * sr, cy + ex * sr + ey * cr};
    }
    strip(pt, n, t, c, true);
}

void ui_image(UiTex *t, float x, float y, float w, float h, float alpha)
{
    if (!t) {
        return;
    }
    const float tw = (float)t->w, th = (float)t->h;
    const float k = fminf(w / tw, h / th), dw = tw * k, dh = th * k;
    const SDL_FRect d = {(x + (w - dw) / 2) * U.scale, (y + (h - dh) / 2) * U.scale, dw * U.scale, dh * U.scale};
    blit(t, NULL, &d, (UiColor){255, 255, 255, (uint8_t)lrintf(clamp01(alpha) * 255)});
}

void ui_clip(float x, float y, float w, float h)
{
    const SDL_Rect r = {(int)(x * U.scale), (int)(y * U.scale), (int)(w * U.scale), (int)(h * U.scale)};
    if (U.ov) {
        U.clip[0] = r.x;
        U.clip[1] = r.y;
        U.clip[2] = r.w > 0 ? r.w : 1;
        U.clip[3] = r.h > 0 ? r.h : 1;
        return;
    }
    SDL_SetRenderClipRect(U.r, &r);
}

void ui_unclip(void)
{
    if (U.ov) {
        memset(U.clip, 0, sizeof(U.clip));
        return;
    }
    SDL_SetRenderClipRect(U.r, NULL);
}

/* ------------------------------------------------------------ layers */

UiTex *ui_layer_begin(UiTex *layer)
{
    if (U.ov) {
        return layer;           /* the overlay draws straight */
    }
    if (!layer || layer->w != U.pw || layer->h != U.ph || !layer->t) {
        ui_texture_free(layer);
        layer = SDL_calloc(1, sizeof(*layer));
        layer->w = U.pw;
        layer->h = U.ph;
        layer->t = SDL_CreateTexture(U.r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, U.pw, U.ph);
        /* drawn over a transparent target with plain blending: its colour is
         * premultiplied (and additive glows keep theirs where alpha is 0) */
        tex_blend(layer, UB_PREMUL);
        if (layer->t) {
            SDL_SetTextureScaleMode(layer->t, SDL_SCALEMODE_LINEAR);
        }
    }
    if (U.ntargets < 8) {
        U.targets[U.ntargets++] = SDL_GetRenderTarget(U.r);
    }
    SDL_SetRenderTarget(U.r, layer->t);
    SDL_SetRenderDrawColor(U.r, 0, 0, 0, 0);
    SDL_RenderClear(U.r);
    return layer;
}

void ui_layer_end(void)
{
    if (U.ov) {
        return;
    }
    SDL_SetRenderTarget(U.r, U.ntargets ? U.targets[--U.ntargets] : NULL);
}

void ui_layer_draw3d(UiTex *layer, float angle, float depth, float alpha)
{
    if (!layer || alpha <= 0.003f || U.ov) {
        return;
    }
    enum { COLS = 24 };
    const float W = (float)U.pw, H = (float)U.ph, f = W * 1.25f, cx = W / 2, cy = H / 2;
    const float ca = cosf(angle), sa = sinf(angle);
    SDL_Vertex v[(COLS + 1) * 2];
    int idx[COLS * 6];
    const SDL_FColor col = {1, 1, 1, alpha};    /* premultiplied by geom (vcol) */
    for (int i = 0; i <= COLS; i++) {
        const float u = (float)i / COLS, X = (u - 0.5f) * W;
        const float z = X * sa + depth * W, x = X * ca;
        const float k = f / (f + z);
        v[i * 2] = (SDL_Vertex){{cx + x * k, cy - H / 2 * k}, col, {u, 0}};
        v[i * 2 + 1] = (SDL_Vertex){{cx + x * k, cy + H / 2 * k}, col, {u, 1}};
    }
    for (int i = 0; i < COLS; i++) {
        const int a = i * 2;
        idx[i * 6] = a;
        idx[i * 6 + 1] = a + 2;
        idx[i * 6 + 2] = a + 1;
        idx[i * 6 + 3] = a + 1;
        idx[i * 6 + 4] = a + 2;
        idx[i * 6 + 5] = a + 3;
    }
    geom(layer, v, (COLS + 1) * 2, idx, COLS * 6);
}

/* ------------------------------------------------------------ the transitions */

static float hrand(uint32_t i)
{
    i ^= i >> 16;
    i *= 0x7FEB352Du;
    i ^= i >> 15;
    i *= 0x846CA68Bu;
    i ^= i >> 16;
    return (float)(i & 0xFFFFFF) / 16777216.0f;
}

static float smooth01(float x)
{
    x = clamp01(x);
    return x * x * (3 - 2 * x);
}

void ui_fx_shatter(UiTex *t, float k)
{
    if (!t) {
        return;
    }
    enum { C = 8, R = 6, N = C * R * 2 };
    const float W = (float)U.pw, H = (float)U.ph, cx = W / 2, cy = H / 2;
    k = clamp01(k);
    /* the break: a jittered grid, each cell two shards */
    SDL_FPoint p[(C + 1) * (R + 1)];
    for (int j = 0; j <= R; j++) {
        for (int i = 0; i <= C; i++) {
            float x = W * i / C, y = H * j / R;
            if (i > 0 && i < C) {
                x += (hrand((uint32_t)(j * 31 + i)) - 0.5f) * 0.7f * W / C;
            }
            if (j > 0 && j < R) {
                y += (hrand((uint32_t)(j * 31 + i + 977)) - 0.5f) * 0.7f * H / R;
            }
            p[j * (C + 1) + i] = (SDL_FPoint){x, y};
        }
    }
    SDL_FPoint tri[N][3];
    int n = 0;
    for (int j = 0; j < R; j++) {
        for (int i = 0; i < C; i++) {
            const SDL_FPoint a = p[j * (C + 1) + i], b = p[j * (C + 1) + i + 1];
            const SDL_FPoint c = p[(j + 1) * (C + 1) + i + 1], d = p[(j + 1) * (C + 1) + i];
            if (hrand((uint32_t)(j * 131 + i * 7 + 5)) < 0.5f) {
                tri[n][0] = a, tri[n][1] = b, tri[n][2] = c, n++;
                tri[n][0] = a, tri[n][1] = c, tri[n][2] = d, n++;
            } else {
                tri[n][0] = a, tri[n][1] = b, tri[n][2] = d, n++;
                tri[n][0] = b, tri[n][1] = c, tri[n][2] = d, n++;
            }
        }
    }
    /* first the picture pushes at the viewer and cracks (a fifth of the
     * time), then the shards fly at the viewer, turning, and fade */
    const float crack = smooth01(k / 0.2f);
    const float fly = k <= 0.2f ? 0 : (k - 0.2f) / 0.8f, e = fly * fly;
    const float pre = 1.0f + 0.04f * crack;
    const float fade = 1.0f - smooth01((fly - 0.4f) / 0.6f);
    /* the nearer ones over the others */
    int order[N];
    float sc[N];
    for (int i = 0; i < N; i++) {
        order[i] = i;
        sc[i] = 0.6f + 1.4f * hrand((uint32_t)(i * 7 + 3));
    }
    for (int i = 1; i < N; i++) {
        const int o = order[i];
        int j = i;
        for (; j > 0 && sc[order[j - 1]] > sc[o]; j--) {
            order[j] = order[j - 1];
        }
        order[j] = o;
    }
    for (int q = 0; q < N; q++) {
        const int s = order[q];
        const float r2 = hrand((uint32_t)(s * 7 + 5));
        SDL_FPoint g = {(tri[s][0].x + tri[s][1].x + tri[s][2].x) / 3, (tri[s][0].y + tri[s][1].y + tri[s][2].y) / 3};
        float dx = g.x - cx, dy = g.y - cy;
        const float l = sqrtf(dx * dx + dy * dy);
        dx = l > 1 ? dx / l : 0;
        dy = l > 1 ? dy / l : -1;
        const float dist = (0.25f + 0.55f * sc[s]) * W * e;
        const float scale = pre * (1 + e * sc[s]);
        const float ang = (r2 - 0.5f) * 3.4f * e;
        const float ca = cosf(ang), sa = sinf(ang);
        const SDL_FPoint G = {cx + (g.x - cx) * pre + dx * dist, cy + (g.y - cy) * pre + dy * dist + e * H * 0.2f * r2};
        SDL_Vertex v[3];
        for (int m = 0; m < 3; m++) {
            const float ox = (tri[s][m].x - g.x) * scale, oy = (tri[s][m].y - g.y) * scale;
            v[m] = (SDL_Vertex){{G.x + ox * ca - oy * sa, G.y + ox * sa + oy * ca},
                                {1, 1, 1, fade},
                                {tri[s][m].x / W, tri[s][m].y / H}};
        }
        static const int idx[3] = {0, 1, 2};
        geom(t, v, 3, idx, 3);
        /* the cracks: light along the edges as it breaks */
        const float la = crack * (1 - smooth01(fly / 0.45f));
        if (la > 0.01f) {
            for (int m = 0; m < 3; m++) {
                line_px(v[m].position.x, v[m].position.y, v[(m + 1) % 3].position.x, v[(m + 1) % 3].position.y,
                        fmaxf(1.5f, H / 600.0f), (UiColor){255, 255, 255, (uint8_t)(200 * la)});
            }
        }
    }
    /* the flash at the break */
    const float flash = crack * (1 - smooth01(fly / 0.35f));
    if (flash > 0.01f) {
        const float s = U.scale;
        quad(0, 0, W / s, H / s, (UiColor){255, 255, 255, (uint8_t)(150 * flash)},
             (UiColor){255, 255, 255, (uint8_t)(150 * flash)}, (UiColor){255, 255, 255, (uint8_t)(150 * flash)},
             (UiColor){255, 255, 255, (uint8_t)(150 * flash)});
    }
}

void ui_fx_flash_in(UiTex *t, float k, UiColor from)
{
    k = clamp01(k);
    const float W = (float)U.pw, H = (float)U.ph, cx = W / 2, cy = H / 2;
    const float e = 1 - (1 - k) * (1 - k) * (1 - k);
    if (t) {
        /* the picture rushing in, and its trail: larger copies, fainter */
        for (int i = 0; i < 4; i++) {
            const float s = 1 + (0.14f + 0.07f * i) * (1 - e);
            const float a = i == 0 ? 1.0f : 0.30f * (1 - e) / (float)i;
            if (a <= 0.004f) {
                continue;
            }
            const SDL_FRect d = {cx - W * s / 2, cy - H * s / 2, W * s, H * s};
            blit(t, NULL, &d, (UiColor){255, 255, 255, (uint8_t)lrintf(a * 255)});
        }
    }
    const float w = (1 - k) * (1 - k);
    if (w > 0.004f) {
        const UiColor c = {from.r, from.g, from.b, (uint8_t)lrintf(from.a * w)};
        const float s = U.scale;
        quad(0, 0, W / s, H / s, c, c, c, c);
    }
}

/* ------------------------------------------------------------ the console's look */

void ui_backdrop(float k)
{
    const float W = ui_w(), H = ui_h();
    const double t = ui_time();
    ui_rect_grad(0, 0, W, H, (UiColor){0x02, 0x0A, 0x02, 0xFF}, (UiColor){0x04, 0x16, 0x03, 0xFF});
    /* the fog: slow green lights drifting */
    for (int i = 0; i < 6; i++) {
        const float ph = (float)(t * (0.05 + i * 0.013) + i * 1.7);
        const float x = W * (0.5f + 0.42f * sinf(ph * 1.3f + i)), y = H * (0.5f + 0.38f * cosf(ph + i * 2.1f));
        ui_glow(x, y, H * (0.55f + 0.15f * sinf(ph * 2)), ui_alpha((UiColor){0x3C, 0xA0, 0x10, 0xFF}, 0.22f * k));
    }
    /* the linked hexagons, drifting and pulsing (the original dashboard's backdrop) */
    {
        const float hs = 46, hx = hs * 1.5f, hy = hs * 0.866f * 2;
        const float ox = fmodf((float)(t * 6), hx * 2), oy = fmodf((float)(t * 3), hy);
        for (int gx = -1; gx * hx < W + hx; gx++) {
            for (int gy = -1; gy * hy < H * 0.7f + hy; gy++) {
                const float x = gx * hx - ox, y = gy * hy + ((gx & 1) ? hy / 2 : 0) - oy;
                const float wave = 0.5f + 0.5f * sinf((float)t * 1.3f + gx * 0.7f + gy * 1.1f);
                const float a = 0.13f * k * wave * (1 - y / (H * 0.75f));
                if (a <= 0.004f) {
                    continue;
                }
                for (int s = 0; s < 6; s++) {
                    const float a0 = (float)(s * M_PI / 3), a1 = (float)((s + 1) * M_PI / 3);
                    ui_line(x + cosf(a0) * hs * 0.5f, y + sinf(a0) * hs * 0.5f, x + cosf(a1) * hs * 0.5f,
                            y + sinf(a1) * hs * 0.5f, 1.2f, ui_alpha(UI_GREEN, a));
                }
            }
        }
    }
    /* the floor's grid, receding */
    const UiColor gl = ui_alpha(UI_GREEN, 0.10f * k);
    const float horizon = H * 0.62f;
    for (int i = 0; i <= 12; i++) {
        const float z = fmodf((float)(i + fmod(t * 0.35, 1.0)), 12.f) / 12.f;
        const float y = horizon + (H - horizon) * z * z;
        ui_line(0, y, W, y, 1.2f, ui_alpha(gl, z));
    }
    for (int i = -12; i <= 12; i++) {
        ui_line(W / 2 + i * W * 0.02f, horizon, W / 2 + i * W * 0.16f, H, 1.0f, gl);
    }
    /* specks rising */
    for (int i = 0; i < 40; i++) {
        const float sx = fmodf(i * 97.31f, 1.0f) * W;
        const float sp = 0.02f + fmodf(i * 0.618f, 1.0f) * 0.05f;
        const float sy = H - fmodf((float)(t * sp * H + i * 53.7), H + 20);
        ui_glow(sx + 6 * sinf((float)t + i), sy, 3 + (i % 3), ui_alpha(UI_GREEN_HI, 0.5f * k));
    }
    /* a vignette */
    ui_rect_grad(0, 0, W, H * 0.25f, (UiColor){0, 0, 0, 160}, (UiColor){0, 0, 0, 0});
    ui_rect_grad(0, H * 0.75f, W, H * 0.25f, (UiColor){0, 0, 0, 0}, (UiColor){0, 0, 0, 200});
}

void ui_orb(float cx, float cy, float r, float pulse)
{
    const double t = ui_time();
    const float p = 0.85f + 0.15f * sinf((float)t * 2.2f) + pulse * 0.3f;
    ui_glow(cx, cy, r * 3.2f, ui_alpha((UiColor){0x50, 0xD0, 0x10, 0xFF}, 0.35f * p));
    ui_glow(cx, cy, r * 1.9f, ui_alpha((UiColor){0x80, 0xFF, 0x30, 0xFF}, 0.45f * p));
    ui_circle(cx, cy, r, (UiColor){0x1A, 0x50, 0x06, 0xFF});
    ui_glow(cx - r * 0.25f, cy - r * 0.3f, r * 0.9f, ui_alpha(UI_GREEN_HI, 0.55f));
    ui_glow(cx, cy, r * 1.05f, ui_alpha(UI_GREEN, 0.5f * p));
    for (int i = 0; i < 3; i++) {
        const float rot = (float)(t * (0.4 + i * 0.25) + i * 1.1);
        ui_ring(cx, cy, r * (1.35f + i * 0.22f), r * (0.42f + i * 0.08f), rot, 2.0f,
                ui_alpha(UI_GREEN_HI, 0.55f - i * 0.12f));
    }
}
