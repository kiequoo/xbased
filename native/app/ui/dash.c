/*
 * hakuX front end â€” the dashboard (dash.h): its loop, input, lists, and
 * the main, games and game screens. Settings, controls, memory and online
 * are in dash_settings.c, dash_memory.c.
 */
#include "native/app/ui/dash.h"
#include "native/app/ui/console_cfg.h"
#include "native/app/ui/dash_int.h"
#include "native/app/ui/shortcut.h"
#include "native/app/ui/sound.h"
#include "native/app/ui/update.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifndef XBASED_VERSION
#define XBASED_VERSION "1.0.55"
#endif

Dash D;
static SDL_Window *g_win;
static UiTex *g_layer, *g_prev;   /* this screen's foreground, the last one's */
static float g_trans = 1;               /* the transition's progress, 0-1 */
static int g_trans_dir = 1;
static SDL_Gamepad *g_pads[8];
static SDL_JoystickID g_last_pad;

SDL_Gamepad *dash_active_pad(void)
{
    for (int i = 0; i < 8; i++) {
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == g_last_pad) {
            return g_pads[i];
        }
    }
    for (int i = 0; i < 8; i++) {
        if (g_pads[i]) {
            return g_pads[i];
        }
    }
    /* in game the controllers were opened before the menu: one of them */
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    SDL_Gamepad *g = n > 0 ? SDL_OpenGamepad(ids[0]) : NULL;
    SDL_free(ids);
    if (g) {
        g_pads[0] = g;
    }
    return g;
}

/* ------------------------------------------------------------ screens */

/* Snapdragon GSR became AMD FSR 1 (1.0.11): a setting that chose it chooses FSR,
 * written so; true when it changed. */
static bool upscale_migrate(Config *c)
{
    const char *v = cfg_get(c, "upscale");
    if (v && !strcmp(v, "sgsr")) {
        cfg_set(c, "upscale", "fsr");
        return true;
    }
    return false;
}

/* a text field's typing ends with its screen (B can leave one mid-word) */
static void typing_off(void)
{
    if (D.typing) {
        D.typing = false;
        D.edit_key = NULL;
        SDL_StopTextInput(SDL_GetKeyboardFocus());
    }
}

void dash_push(enum Screen s)
{
    typing_off();
    if (D.sp < 15) {
        D.stack[++D.sp] = s;
        D.anim = 0;
    }
}

void dash_pop(void)
{
    typing_off();
    if (D.ask) {
        D.ask = NULL;
        return;
    }
    if (D.sp > 0) {
        if (dash_top() == SCR_GAME_SETTINGS || dash_top() == SCR_MODS || dash_top() == SCR_CONTROLS) {
            dash_save_cfg();
        }
        D.sp--;
        D.anim = 0;
        D.editing_game = dash_top() == SCR_GAME_SETTINGS || dash_top() == SCR_MODS ||
                         (dash_top() == SCR_CONTROLS && D.editing_game);
    }
}

enum Screen dash_top(void)
{
    return D.stack[D.sp];
}

void dash_toast(const char *msg)
{
    snprintf(D.toast, sizeof(D.toast), "%s", msg);
    /* time to read it: longer for a longer one */
    const double t = 3 + strlen(D.toast) / 30.0;
    D.toast_until = ui_time() + (t < 12 ? t : 12);
}

/* s in lines no wider than max_w (at spaces; CJK, which has none, between
 * characters): their count, at most max. */
static int wrap_lines(const char *s, float size, int font, float max_w, char lines[][512], int max)
{
    int nl = 0;
    while (*s && nl < max) {
        size_t n = 0, last_space = 0;
        char *line = lines[nl];
        while (s[n] && n < 511) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (s[n] == ' ') {
                last_space = n;
            }
            if (ui_text_width(line, size, font) > max_w) {
                if (last_space) {
                    n = last_space;
                } else {
                    while (n > 1 && (s[n] & 0xC0) == 0x80) {
                        n--;
                    }
                }
                break;
            }
            n++;
        }
        snprintf(line, 512, "%.*s", (int)n, s);
        nl++;
        s += n ? n : 1;
        while (*s == ' ') {
            s++;
        }
    }
    return nl;
}

/* The toast: a line, or lines wrapped to the screen's width (from the
 * bottom up), in a rounded box. */
static void draw_toast(void)
{
    const float W = ui_w();
    char lines[8][512];
    const int nl = wrap_lines(D.toast, 22, UI_FONT_REGULAR, W - 240, lines, 8);
    float widest = 0;
    for (int i = 0; i < nl; i++) {
        const float w = ui_text_width(lines[i], 22, UI_FONT_REGULAR);
        widest = w > widest ? w : widest;
    }
    const float tw = widest + 40, th = 14 + 30.0f * (float)nl, y = ui_h() - 76 - th;
    ui_round((W - tw) / 2, y, tw, th, 22, (UiColor){0x10, 0x30, 0x08, 0xF0});
    ui_round_outline((W - tw) / 2, y, tw, th, 22, 1.5f, UI_GREEN);
    for (int i = 0; i < nl; i++) {
        ui_text(W / 2, y + 10 + 30.0f * (float)i, lines[i], 22, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_CENTER);
    }
}

/* While the library is read: the games so far, in a corner (under the
 * clock), on every screen; what's there can be played meanwhile. */
static void draw_scan_badge(void)
{
    const float W = ui_w(), y = 78, h = 28;
    char t[96];
    snprintf(t, sizeof(t), T(S_LOADING_GAMES), lib_count());
    const float tw = ui_text_width(t, 18, UI_FONT_REGULAR), w = tw + 58, x = W - 60 - w;
    ui_round(x, y, w, h, h / 2, (UiColor){0x10, 0x30, 0x08, 0xE0});
    ui_round_outline(x, y, w, h, h / 2, 1, UI_GREEN_DIM);
    /* an orbit turning, as the menu's orb's: it's working */
    ui_ring(x + 19, y + h / 2, 9, 3.5f, (float)ui_time() * 4.0f, 2, UI_GREEN_HI);
    ui_circle(x + 19, y + h / 2, 3, UI_GREEN_HI);
    ui_text(x + 36, y + 4, t, 18, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_LEFT);
}

void dash_ask(const char *q, void (*on_yes)(Dash *))
{
    D.ask = q;
    D.on_yes = on_yes;
    D.nchoices = 0;
}

void dash_choose(const char *q, const char *const *choices, int n, void (*on_choice)(Dash *, int))
{
    snprintf(D.ask_buf, sizeof(D.ask_buf), "%s", q);
    D.ask = D.ask_buf;
    D.nchoices = n < 2 ? 2 : n > 3 ? 3 : n;
    for (int i = 0; i < D.nchoices; i++) {
        D.choices[i] = choices[i];
    }
    D.on_choice = on_choice;
}

/* the in-game menu's (dash_ingame): a game runs, its kernel the EEPROM's
 * writer till it ends; the settings meet the EEPROM back in the dashboard */
static bool g_game_running;

void dash_save_cfg(void)
{
    cfg_save(&D.global);
    if (!g_game_running) {
        cc_sync(&D.global); /* the console's language and video flags: the EEPROM's too */
    }
    if (D.cur && D.game.path[0]) {
        if (D.game.n) {
            cfg_save(&D.game);
        } else {
            remove(D.game.path);   /* nothing set: the global settings */
        }
    }
}

float ease(float t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

/* ------------------------------------------------------------ input */

static double g_rep_at;
static int g_rep_dir;   /* held: 1 up 2 down 3 left 4 right */

static void held(int dir, bool down)
{
    if (down) {
        g_rep_dir = dir;
        g_rep_at = ui_time() + 0.38;
    } else if (g_rep_dir == dir) {
        g_rep_dir = 0;
    }
}

static void press(int dir)
{
    D.in.up |= dir == 1;
    D.in.down |= dir == 2;
    D.in.left |= dir == 3;
    D.in.right |= dir == 4;
    D.in.any = true;
}

static double g_cursor_at;  /* when the pointer showed; 0 hidden */

static void cursor(bool show)
{
    if (show) {
        if (g_cursor_at <= 0) {
            SDL_ShowCursor();
        }
        g_cursor_at = ui_time();
    } else if (g_cursor_at > 0) {
        SDL_HideCursor();
        g_cursor_at = 0;
    }
}

static void poll_input(void)
{
    static float ax[2];
    In *in = &D.in;
    const bool pad = in->pad;
    memset(in, 0, sizeof(*in));
    in->pad = pad;
    in->pad_button = -1;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            D.quit = true;
            break;
        case SDL_EVENT_KEY_DOWN: {
            in->pad = false;
            cursor(false);
            in->key = e.key.key;
            in->repeat |= e.key.repeat;
            in->any = true;
            const SDL_Keycode k = e.key.key;
            if (D.typing && ((k >= SDLK_A && k <= SDLK_Z) || (k >= SDLK_0 && k <= SDLK_9) || k == SDLK_SPACE ||
                             k == SDLK_BACKSPACE || k == SDLK_DELETE || k == SDLK_TAB)) {
                break;  /* a letter for the text field (in->key, in->text) */
            }
            if (k == SDLK_F3 || (k == SDLK_F && (e.key.mod & SDL_KMOD_CTRL))) {
                in->find = true;
            } else if (k == SDLK_Z) {
                in->lt = true;
            } else if (k == SDLK_C) {
                in->rt = true;
            } else if (k == SDLK_V) {
                in->view = true;
            } else if (k == SDLK_UP || k == SDLK_W) {
                press(1);
            } else if (k == SDLK_DOWN || k == SDLK_S) {
                press(2);
            } else if (k == SDLK_LEFT || k == SDLK_A) {
                press(3);
            } else if (k == SDLK_RIGHT || k == SDLK_D) {
                press(4);
            } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !(e.key.mod & SDL_KMOD_ALT)) {
                in->ok = true;
            } else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) {
                in->back = true;
            } else if (k == SDLK_Q || k == SDLK_PAGEUP) {
                in->lb = true;
            } else if (k == SDLK_E || k == SDLK_PAGEDOWN) {
                in->rb = true;
            } else if (k == SDLK_X || k == SDLK_DELETE) {
                in->x = true;
            } else if (k == SDLK_Y || k == SDLK_TAB) {
                in->y = true;
            } else if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
                SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
            }
            break;
        }
        case SDL_EVENT_DROP_FILE:
            /* a disc image dropped on the window: played once, not added */
            dash_play_file_later(e.drop.data);

            break;
        case SDL_EVENT_TEXT_INPUT:
            in->text = e.text.text;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            /* a real move, not the window appearing under the pointer */
            if (fabsf(e.motion.xrel) + fabsf(e.motion.yrel) >= 2) {
                in->moved = true;
                in->pad = false;
                cursor(true);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            in->pad = false;
            cursor(true);
            in->any = true;
            if (e.button.button == SDL_BUTTON_LEFT) {
                in->click = true;
            } else if (e.button.button == SDL_BUTTON_RIGHT) {
                in->rclick = true;
                in->back = true;
            } else if (e.button.button == SDL_BUTTON_X1) {
                in->back = true;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            in->wheel += e.wheel.y;
            cursor(true);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            for (int i = 0; i < 8; i++) {
                if (!g_pads[i]) {
                    g_pads[i] = SDL_OpenGamepad(e.gdevice.which);
                    break;
                }
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (int i = 0; i < 8; i++) {
                if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == e.gdevice.which) {
                    SDL_CloseGamepad(g_pads[i]);
                    g_pads[i] = NULL;
                }
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            const bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            g_last_pad = e.gbutton.which;
            in->pad = true;
            cursor(false);
            if (down) {
                in->pad_button = e.gbutton.button;
                in->any = true;
            }
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: held(1, down); if (down) press(1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: held(2, down); if (down) press(2); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: held(3, down); if (down) press(3); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: held(4, down); if (down) press(4); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: in->ok |= down; break;
            case SDL_GAMEPAD_BUTTON_EAST: in->back |= down; break;
            case SDL_GAMEPAD_BUTTON_WEST: in->x |= down; break;
            case SDL_GAMEPAD_BUTTON_NORTH: in->y |= down; break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: in->lb |= down; break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: in->rb |= down; break;
            case SDL_GAMEPAD_BUTTON_START: in->start |= down; break;
            case SDL_GAMEPAD_BUTTON_BACK: in->view |= down; break;
            default: break;
            }
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            const float v = e.gaxis.value / 32767.0f;
            if (fabsf(v) > 0.5f) {
                in->pad_axis = (e.gaxis.axis + 1) * (v < 0 ? -1 : 1);
            }
            g_last_pad = e.gaxis.which;
            if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                /* a trigger pulled past half (once a pull) */
                static bool pulled[2];
                const int t = e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                if (v > 0.5f && !pulled[t]) {
                    pulled[t] = true;
                    *(t ? &in->rt : &in->lt) = true;
                    in->any = in->pad = true;
                } else if (v < 0.3f) {
                    pulled[t] = false;
                }
            }
            in->pad |=(dash_top() == SCR_STICKS || dash_top() == SCR_TRIGGERS) && fabsf(v) > 0.5f; /* its hints: the gamepad's */
            /* the sticks' screen: the stick is being looked at, not steering */
            if (dash_top() != SCR_STICKS &&
                (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
                const int a = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY;
                const float was = ax[a];
                ax[a] = v;
                const int neg = a ? 1 : 3, pos = a ? 2 : 4;
                if (fabsf(v) > 0.6f && fabsf(was) <= 0.6f) {
                    in->pad = true;
                    held(v < 0 ? neg : pos, true);
                    press(v < 0 ? neg : pos);
                } else if (fabsf(v) < 0.4f && fabsf(was) >= 0.4f) {
                    held(neg, false);
                    held(pos, false);
                }
            }
            break;
        }
        default:
            break;
        }
    }
    /* the pointer: hidden until the mouse moves, hidden again by the
     * keyboard or a gamepad or after a few still seconds; nothing points
     * while it's hidden */
    if (g_cursor_at > 0 && ui_time() - g_cursor_at > 3) {
        cursor(false);
    }
    float mx, my;
    SDL_GetMouseState(&mx, &my);
    float dpi = SDL_GetWindowPixelDensity(g_win);
    in->mx = g_cursor_at > 0 ? mx * dpi / ui_scale() : -1;
    in->my = g_cursor_at > 0 ? my * dpi / ui_scale() : -1;
    if (g_cursor_at <= 0) {
        in->moved = in->click = in->rclick = false;
        in->wheel = 0;
    }
    if (g_rep_dir && ui_time() >= g_rep_at) {
        press(g_rep_dir);
        in->repeat = true;
        g_rep_at = ui_time() + 0.09;
    }
    /* show gamepad glyphs whenever a controller is connected; fall back to
     * keyboard glyphs only when nothing is plugged in */
    for (int i = 0; i < 8; i++) {
        if (g_pads[i]) { in->pad = true; break; }
    }
}

/* Scripted input and pictures (tests): HAKUX_AUTO_CMD=<file>, a command a
 * line as it grows: UP DOWN LEFT RIGHT OK BACK X Y LB RB, "shot <name>" (a
 * BMP in HAKUX_AUTO_DIR), "quit"; real input events too: "KEY <name>",
 * "MOUSE x y" (720p units), CLICK, RCLICK, "WHEEL n". */
static char g_shot_req[256];
static long long g_auto_off = -1;   /* -1: from the file's end at the next look */

static void auto_poll(void)
{
    long long off = g_auto_off;
    static double seen;
    const char *path = getenv("HAKUX_AUTO_CMD");
    if (!path || ui_time() - seen < 0.1) {
        return;
    }
    seen = ui_time();
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    if (off < 0) {
        fseek(f, 0, SEEK_END);
        off = ftell(f);
        g_auto_off = off;
    }
    fseek(f, (long)off, SEEK_SET);
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        const size_t n = strlen(line);
        if (!n || line[n - 1] != '\n') {
            break;
        }
        off += (long long)n;
        g_auto_off = off;
        line[strcspn(line, "\r\n")] = 0;
        In *in = &D.in;
        in->any = true;
        if (!strcmp(line, "UP")) {
            in->up = true;
        } else if (!strcmp(line, "DOWN")) {
            in->down = true;
        } else if (!strcmp(line, "LEFT")) {
            in->left = true;
        } else if (!strcmp(line, "RIGHT")) {
            in->right = true;
        } else if (!strcmp(line, "OK")) {
            in->ok = true;
        } else if (!strcmp(line, "BACK")) {
            in->back = true;
        } else if (!strcmp(line, "X")) {
            in->x = true;
        } else if (!strcmp(line, "Y")) {
            in->y = true;
        } else if (!strcmp(line, "LB")) {
            in->lb = true;
        } else if (!strcmp(line, "RB")) {
            in->rb = true;
        } else if (!strcmp(line, "PAD")) {
            in->pad = true;
        } else if (!strncmp(line, "STICKS ", 7)) {
            /* a virtual gamepad's sticks (SDL's values: lx ly rx ry, down
             * positive) and optionally its triggers (lt rt, 0..32767),
             * plugged in the first time */
            static SDL_Joystick *vj;
            int v[6] = {0};
            const int nv = sscanf(line + 7, "%d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
            if (nv >= 2) {
                if (!vj) {
                    SDL_VirtualJoystickDesc d;
                    SDL_INIT_INTERFACE(&d);
                    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
                    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
                    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
                    d.name = "xbased virtual gamepad";
                    vj = SDL_OpenJoystick(SDL_AttachVirtualJoystick(&d));
                }
                for (int a = 0; vj && a < (nv > 4 ? 6 : 4); a++) {
                    SDL_SetJoystickVirtualAxis(vj, a, (Sint16)(v[a] > 32767 ? 32767 : v[a] < -32768 ? -32768 : v[a]));
                }
            }
        } else if (!strncmp(line, "shot ", 5)) {
            const char *dir = getenv("HAKUX_AUTO_DIR");
            snprintf(g_shot_req, sizeof(g_shot_req), "%s/shot_%s.bmp", dir ? dir : ".", line + 5);
        } else if (!strncmp(line, "size ", 5)) {
            int w = 0, h = 0;
            if (sscanf(line + 5, "%d %d", &w, &h) == 2) {
                SDL_SetWindowFullscreen(g_win, false);
                SDL_SetWindowSize(g_win, w, h);
            }
        } else if (!strncmp(line, "TEXT ", 5)) {
            /* text typed into a text field, as the keyboard sends it */
            static char typed[256];
            snprintf(typed, sizeof(typed), "%s", line + 5);
            in->text = typed;
        } else if (!strncmp(line, "KEY ", 4)) {
            /* a key as the keyboard sends it (its SDL name: Return, Backspace, Down, X) */
            SDL_Event e = {0};
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.key = SDL_GetKeyFromName(line + 4);
            e.key.windowID = SDL_GetWindowID(g_win);
            e.key.down = true;
            SDL_PushEvent(&e);
            e.type = SDL_EVENT_KEY_UP;
            e.key.down = false;
            SDL_PushEvent(&e);
        } else if (!strncmp(line, "MOUSE ", 6)) {
            /* the pointer to (x, y) in 720p units, as a mouse moves it */
            float x = 0, y = 0;
            if (sscanf(line + 6, "%f %f", &x, &y) == 2) {
                const float k = ui_scale() / SDL_GetWindowPixelDensity(g_win);
                SDL_WarpMouseInWindow(g_win, x * k, y * k);
                SDL_Event e = {0};
                e.type = SDL_EVENT_MOUSE_MOTION;
                e.motion.windowID = SDL_GetWindowID(g_win);
                e.motion.x = x * k;
                e.motion.y = y * k;
                e.motion.xrel = 4;
                e.motion.yrel = 4;
                SDL_PushEvent(&e);
            }
        } else if (!strcmp(line, "CLICK") || !strcmp(line, "RCLICK")) {
            SDL_Event e = {0};
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.windowID = SDL_GetWindowID(g_win);
            e.button.button = line[0] == 'R' ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
            e.button.down = true;
            e.button.clicks = 1;
            SDL_PushEvent(&e);
            e.type = SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.down = false;
            SDL_PushEvent(&e);
        } else if (!strncmp(line, "WHEEL ", 6)) {
            SDL_Event e = {0};
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.windowID = SDL_GetWindowID(g_win);
            e.wheel.y = (float)atof(line + 6);
            SDL_PushEvent(&e);
        } else if (!strcmp(line, "full")) {
            SDL_SetWindowFullscreen(g_win, true);
        } else if (!strcmp(line, "quit")) {
            D.quit = true;
        }
    }
    fclose(f);
}

/* the frame drawn, before it is presented: the picture asked for */
static void auto_shot(void)
{
    if (!g_shot_req[0]) {
        return;
    }
    ui_shot(g_shot_req);
    g_shot_req[0] = 0;
}

/* ------------------------------------------------------------ drawing helpers */

static const char *const k_key[6] = {"Enter", "Esc", "Del", "Tab", "Z / C", "V"};
static const char *const k_pad_wide[6] = {NULL, NULL, NULL, NULL, "LT / RT", "View"};

void draw_button_glyph(float x, float y, int which, float size)
{
    static const UiColor k_col[4] = {{0x5E, 0xC8, 0x2C, 0xFF}, {0xE0, 0x3A, 0x30, 0xFF},
                                     {0x30, 0x7C, 0xE8, 0xFF}, {0xF0, 0xC0, 0x20, 0xFF}};
    static const char *const k_pad[4] = {"A", "B", "X", "Y"};
    static const char *const k_key[4] = {"Enter", "Esc", "Del", "Tab"};
    if (D.in.pad && which >= 4) {
        /* the triggers, the View button: their names on a dark key */
        const float w = ui_text_width(k_pad_wide[which], size * 0.56f, UI_FONT_BOLD) + size * 0.7f;
        ui_round(x, y + size * 0.08f, w, size * 0.84f, size * 0.42f, ui_alpha((UiColor){0x30, 0x34, 0x30, 0xFF}, 0.95f));
        ui_text(x + w / 2, y + size * 0.19f, k_pad_wide[which], size * 0.56f, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_CENTER);
    } else if (D.in.pad) {
        const int gi = cfg_bool(&D.global, "ui.swapab", false) ? (which ^ 1) : which;
        ui_circle(x + size / 2, y + size / 2, size / 2, k_col[gi]);
        ui_glow(x + size / 2, y + size / 2, size, ui_alpha(k_col[gi], 0.35f));
        ui_text(x + size / 2, y + size * 0.06f, k_pad[gi], size * 0.8f, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_CENTER);
    } else {
        const float w = ui_text_width(k_key[which], size * 0.62f, UI_FONT_BOLD) + size * 0.6f;
        ui_round(x, y + size * 0.08f, w, size * 0.84f, 5, ui_alpha(UI_WHITE, 0.9f));
        ui_text(x + w / 2, y + size * 0.16f, k_key[which], size * 0.62f, UI_FONT_BOLD, UI_BLACK, UI_ALIGN_CENTER);
    }
}

static float glyph_w(int which, float size)
{
    if (D.in.pad && which >= 4) {
        return ui_text_width(k_pad_wide[which], size * 0.56f, UI_FONT_BOLD) + size * 0.7f;
    }
    return D.in.pad ? size : ui_text_width(k_key[which], size * 0.62f, UI_FONT_BOLD) + size * 0.6f;
}

float draw_hint_at(float px, int which, const char *text)
{
    const float py = ui_h() - 58, s = 30;
    draw_button_glyph(px, py, which, s);
    px += glyph_w(which, s) + 10;
    return px + ui_text(px, py + 3, text, 22, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_LEFT) + 34;
}

float draw_hints(const char *a, const char *b, const char *x, const char *y)
{
    const char *t[4] = {a, b, x, y};
    float px = 60;
    for (int i = 0; i < 4; i++) {
        if (t[i]) {
            px = draw_hint_at(px, i, t[i]);
        }
    }
    return px;
}

void draw_header(const char *title, const char *sub)
{
    const float W = ui_w();
    ui_rect_hgrad(0, 34, W * 0.7f, 2, ui_alpha(UI_GREEN, 0.0f), ui_alpha(UI_GREEN, 0.0f));
    ui_glow(110, 62, 120, ui_alpha(UI_GREEN, 0.25f));
    ui_text(60, 30, title, 46, UI_FONT_BOLD, UI_GREEN_HI, UI_ALIGN_LEFT);
    if (sub) {
        ui_text(62, 82, sub, 20, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT);
    }
    ui_rect_hgrad(60, 110, W - 120, 2, ui_alpha(UI_GREEN, 0.9f), ui_alpha(UI_GREEN, 0.0f));
    /* the clock */
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    ui_text(W - 60, 40, dash_hm(tmv.tm_hour, tmv.tm_min), 30, UI_FONT_BOLD, UI_GREEN, UI_ALIGN_RIGHT);
}

const char *dash_hm(int hour, int minute)
{
    static char b[4][16];
    static int k;
    k = (k + 1) & 3;
    if (cfg_bool(&D.global, "ui.clock12", false)) {
        snprintf(b[k], sizeof(b[k]), "%d:%02d %s", hour % 12 ? hour % 12 : 12, minute, hour < 12 ? "AM" : "PM");
    } else {
        snprintf(b[k], sizeof(b[k]), "%02d:%02d", hour, minute);
    }
    return b[k];
}

/* ------------------------------------------------------------ lists */

void list_begin(List *l, float x, float y, float w, float h, float row_h)
{
    memset(l, 0, sizeof(*l));
    l->x = x;
    l->y = y;
    l->w = w;
    l->h = h;
    l->row_h = row_h;
    l->sel = &D.sel[dash_top()];
    l->scroll = &D.scroll[dash_top()];
    l->visible = (int)(h / row_h);
    l->focused = !D.ask;
    ui_round(x - 14, y - 14, w + 28, h + 28, 14, UI_PANEL);
    ui_round_outline(x - 14, y - 14, w + 28, h + 28, 14, 1.5f, ui_alpha(UI_GREEN, 0.35f));
    ui_clip(x - 12, y - 4, w + 24, h + 8);
}

static bool row_common(List *l, int *delta, float *ry, bool *is_sel)
{
    const int i = l->count++;
    if (*l->sel == i && l->focused) {
        if (delta) {
            *delta = D.in.left ? -1 : D.in.right ? 1 : 0;
        }
    } else if (delta) {
        *delta = 0;
    }
    if (i < *l->scroll || i >= *l->scroll + l->visible) {
        return false;
    }
    *ry = l->y + (i - *l->scroll) * l->row_h;
    /* the mouse: hover selects, a click chooses */
    const bool over = D.in.mx >= l->x && D.in.mx < l->x + l->w && D.in.my >= *ry && D.in.my < *ry + l->row_h;
    if (over && D.in.moved && l->focused) {
        *l->sel = i;
    }
    *is_sel = *l->sel == i;
    if (*is_sel) {
        const float pulse = 0.75f + 0.25f * sinf((float)ui_time() * 4);
        ui_round(l->x - 6, *ry + 3, l->w + 12, l->row_h - 6, 8, ui_alpha(UI_GREEN, 0.22f * pulse));
        ui_round_outline(l->x - 6, *ry + 3, l->w + 12, l->row_h - 6, 8, 2, ui_alpha(UI_GREEN_HI, 0.8f * pulse));
        ui_rect(l->x - 6, *ry + 9, 4, l->row_h - 18, UI_GREEN_HI);
    }
    return true;
}

static bool row_chosen(List *l, int i)
{
    if (!l->focused) {
        return false;
    }
    const float ry = l->y + (i - *l->scroll) * l->row_h;
    const bool over = D.in.mx >= l->x && D.in.mx < l->x + l->w && D.in.my >= ry && D.in.my < ry + l->row_h;
    return (*l->sel == i && D.in.ok) || (over && D.in.click && i >= *l->scroll && i < *l->scroll + l->visible);
}

bool list_row(List *l, const char *label, const char *value, int *delta)
{
    float ry;
    bool sel;
    const int i = l->count;
    if (row_common(l, delta, &ry, &sel)) {
        const float ty = ry + (l->row_h - 26) / 2 - 2;
        ui_text_fit(l->x + 12, ty, label, 26, sel ? UI_FONT_BOLD : UI_FONT_REGULAR, sel ? UI_WHITE : UI_GREY,
                    UI_ALIGN_LEFT, value ? l->w * 0.55f : l->w - 24);
        if (value) {
            const float vx = l->w - 12;
            UiColor vc = sel ? UI_GREEN_HI : UI_GREEN;
            if (delta && sel) {
                ui_text(l->x + vx, ty, ">", 26, UI_FONT_BOLD, vc, UI_ALIGN_RIGHT);
                const float vw = ui_text_fit(l->x + vx - 22, ty, value, 26, UI_FONT_BOLD, vc, UI_ALIGN_RIGHT, l->w * 0.4f);
                ui_text(l->x + vx - 30 - vw, ty, "<", 26, UI_FONT_BOLD, vc, UI_ALIGN_RIGHT);
            } else {
                ui_text_fit(l->x + vx, ty, value, 26, UI_FONT_REGULAR, vc, UI_ALIGN_RIGHT, l->w * 0.42f);
            }
        }
        /* mouse: a click on a value's arrows changes it */
        if (delta && sel && D.in.click && D.in.my >= ry && D.in.my < ry + l->row_h && D.in.mx > l->x + l->w * 0.55f) {
            *delta = D.in.mx > l->x + l->w - 40 ? 1 : -1;
            D.in.click = false;
        }
    }
    return row_chosen(l, i);
}

bool list_row_icon(List *l, UiTex *icon, const char *label, const char *sub, const char *value, int *delta)
{
    float ry;
    bool sel;
    const int i = l->count;
    if (row_common(l, delta, &ry, &sel)) {
        const float is = l->row_h - 14;
        ui_round(l->x + 6, ry + 7, is, is, 6, ui_alpha(UI_GREEN_DARK, 1));
        if (icon) {
            ui_image(icon, l->x + 8, ry + 9, is - 4, is - 4, 1);
        }
        const float tx = l->x + is + 22;
        ui_text_fit(tx, ry + (sub ? l->row_h * 0.16f : (l->row_h - 26) / 2 - 2), label, 26,
                    sel ? UI_FONT_BOLD : UI_FONT_REGULAR, sel ? UI_WHITE : UI_GREY, UI_ALIGN_LEFT, l->w - is - 200);
        if (sub) {
            ui_text_fit(tx, ry + l->row_h * 0.55f, sub, 18, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT, l->w - is - 200);
        }
        if (value) {
            ui_text(l->x + l->w - 12, ry + (l->row_h - 22) / 2, value, 22, UI_FONT_REGULAR, UI_GREEN, UI_ALIGN_RIGHT);
        }
    }
    return row_chosen(l, i);
}

bool list_selected(const List *l)
{
    return l->count > 0 && *l->sel == l->count - 1;
}

void list_end(List *l)
{
    ui_unclip();
    if (!l->count) {
        return;
    }
    if (l->focused) {
        if (D.in.up) {
            *l->sel = (*l->sel + l->count - 1) % l->count;
        }
        if (D.in.down) {
            *l->sel = (*l->sel + 1) % l->count;
        }
        const bool over = D.in.mx >= l->x && D.in.mx < l->x + l->w && D.in.my >= l->y && D.in.my < l->y + l->h;
        if (D.in.wheel && over) {
            *l->scroll -= (int)D.in.wheel;
        }
    }
    if (*l->sel >= l->count) {
        *l->sel = l->count - 1;
    }
    if (D.in.up || D.in.down || !D.in.wheel) {
        if (*l->sel < *l->scroll) {
            *l->scroll = *l->sel;
        }
        if (*l->sel >= *l->scroll + l->visible) {
            *l->scroll = *l->sel - l->visible + 1;
        }
    }
    if (*l->scroll > l->count - l->visible) {
        *l->scroll = l->count - l->visible;
    }
    if (*l->scroll < 0) {
        *l->scroll = 0;
    }
    /* the scroll bar */
    if (l->count > l->visible) {
        const float bh = l->h * l->visible / l->count, by = l->y + (l->h - bh) * *l->scroll / (l->count - l->visible);
        ui_round(l->x + l->w + 6, l->y, 4, l->h, 2, ui_alpha(UI_GREEN, 0.15f));
        ui_round(l->x + l->w + 6, by, 4, bh, 2, ui_alpha(UI_GREEN, 0.8f));
    }
}

/* ------------------------------------------------------------ option rows */

static Config *edited(void)
{
    return D.editing_game ? &D.game : &D.global;
}

/* a game's value, else "global (X)" with the global one's name */
static void value_text(const char *v, const char *gv, const char *(*name)(const char *, void *), void *ctx, char *out,
                       size_t n)
{
    if (D.editing_game && !v) {
        snprintf(out, n, "%s (%s)", T(S_DEFAULT_GLOBAL), name(gv, ctx));
    } else {
        snprintf(out, n, "%s", name(v ? v : gv, ctx));
    }
}

typedef struct {
    const char *const *vals, *const *names;
    int n;
    const char *def;
} ChoiceCtx;

static const char *choice_name(const char *v, void *p)
{
    ChoiceCtx *c = p;
    for (int i = 0; i < c->n; i++) {
        if (v && !strcmp(v, c->vals[i])) {
            return c->names[i];
        }
    }
    for (int i = 0; i < c->n; i++) {
        if (!strcmp(c->def, c->vals[i])) {
            return c->names[i];
        }
    }
    return v ? v : "";
}

void opt_choice(List *l, enum UiStr label, const char *key, const char *const *vals, const char *const *names, int n,
                const char *def)
{
    Config *c = edited();
    const char *v = cfg_get(c, key), *gv = cfg_get(&D.global, key);
    ChoiceCtx ctx = {vals, names, n, def};
    char text[256];
    value_text(D.editing_game ? v : (v ? v : def), gv ? gv : def, choice_name, &ctx, text, sizeof(text));
    int delta = 0;
    const bool chosen = list_row(l, T(label), text, &delta);
    if (chosen) {
        delta = 1;
    }
    if (!delta) {
        return;
    }
    /* the values in turn; a game's: "global" before the first */
    int cur = -1;
    const char *now = D.editing_game ? v : (v ? v : def);
    for (int i = 0; now && i < n; i++) {
        if (!strcmp(now, vals[i])) {
            cur = i;
        }
    }
    const int slots = D.editing_game ? n + 1 : n;
    int pos = D.editing_game ? cur + 1 : (cur < 0 ? 0 : cur);
    pos = (pos + delta + slots) % slots;
    if (D.editing_game && pos == 0) {
        cfg_unset(c, key);
    } else {
        cfg_set(c, key, vals[D.editing_game ? pos - 1 : pos]);
    }
}

static const char *bool_name(const char *v, void *p)
{
    (void)p;
    return v && strcmp(v, "0") ? T(S_ON) : T(S_OFF);
}

void opt_bool(List *l, enum UiStr label, const char *key, bool def, bool flag_style)
{
    Config *c = edited();
    const char *v = cfg_get(c, key), *gv = cfg_get(&D.global, key);
    char text[256];
    value_text(D.editing_game ? v : (v ? v : def ? "1" : "0"), gv ? gv : def ? "1" : "0", bool_name, NULL, text,
               sizeof(text));
    int delta = 0;
    if (list_row(l, T(label), text, &delta)) {
        delta = 1;
    } else if (D.in.repeat) {
        /* an on/off row steps once a press: a held direction's repeat went on
         * to the next value at once (a game's: on, then its global value
         * again, which read as the option turning itself off) */
        delta = 0;
    }
    if (!delta) {
        return;
    }
    const bool on = v ? strcmp(v, "0") != 0 : (gv ? strcmp(gv, "0") != 0 : def);
    if (D.editing_game && v) {
        /* a game's: on, off, global in turn */
        if (on == (delta > 0)) {
            cfg_unset(c, key);
            return;
        }
    }
    if (flag_style) {
        /* a flag option ("uncap"): present or "0" */
        cfg_set(c, key, on ? "0" : "1");
    } else {
        cfg_set(c, key, on ? "0" : "1");
    }
}

typedef struct {
    const char *unit;
} IntCtx;

static const char *int_name(const char *v, void *p)
{
    static char b[4][32];
    static int k;
    IntCtx *c = p;
    k = (k + 1) & 3;
    snprintf(b[k], sizeof(b[k]), "%s%s", v ? v : "0", c->unit ? c->unit : "");
    return b[k];
}

void opt_int(List *l, enum UiStr label, const char *key, int lo, int hi, int step, int def, const char *unit)
{
    Config *c = edited();
    const char *v = cfg_get(c, key), *gv = cfg_get(&D.global, key);
    char dv[16];
    snprintf(dv, sizeof(dv), "%d", def);
    IntCtx ctx = {unit};
    char text[256];
    value_text(D.editing_game ? v : (v ? v : dv), gv ? gv : dv, int_name, &ctx, text, sizeof(text));
    int delta = 0;
    list_row(l, T(label), text, &delta);
    if (delta) {
        int x = v ? atoi(v) : gv ? atoi(gv) : def;
        x += delta * step;
        x = x < lo ? lo : x > hi ? hi : x;
        cfg_set_int(c, key, x);
    }
}

/* one bit of the console's video flags (video=0x...) */
void opt_videoflag(List *l, enum UiStr label, unsigned bit)
{
    Config *c = edited();
    const char *v = cfg_get(c, "video"), *gv = cfg_get(&D.global, "video");
    const unsigned def = 0x80000;   /* 480p (the app's default) */
    const unsigned flags = v ? (unsigned)strtoul(v, NULL, 16) : gv ? (unsigned)strtoul(gv, NULL, 16) : def;
    char text[256];
    if (D.editing_game && !v) {
        snprintf(text, sizeof(text), "%s (%s)", T(S_DEFAULT_GLOBAL), flags & bit ? T(S_ON) : T(S_OFF));
    } else {
        snprintf(text, sizeof(text), "%s", flags & bit ? T(S_ON) : T(S_OFF));
    }
    int delta = 0;
    if (list_row(l, T(label), text, &delta)) {
        delta = 1;
    }
    if (delta) {
        char b[16];
        snprintf(b, sizeof(b), "0x%x", flags ^ bit);
        cfg_set(c, "video", b);
    }
}

/* ------------------------------------------------------------ the game's files */

void game_cfg_path(const LibGame *g, char *out, size_t n)
{
    char d[1100];
    snprintf(d, sizeof(d), "%sgames", cfg_data_dir());
    SDL_CreateDirectory(d);
    snprintf(out, n, "%s/%s.ini", d, g->g.key);
}

void states_dir(const LibGame *g, char *out, size_t n)
{
    snprintf(out, n, "%sstates/%s", cfg_data_dir(), g->g.key);
}

static SDL_Process *g_child;
static double g_hide_at;    /* the window's hiding: the game's taking over, or this time (0: hidden) */
static bool child_running(void);

/* The way into a game and back: the screen as it was breaks into shards
 * (the launch's sound over it), the game's window comes up on the black
 * left behind; back from it, the screen rushes in out of black. The
 * screen's picture: a layer of the window's size (drawn into, the frame
 * the launch was asked in; each frame on the way back). */
#define GO_OUT 0.5          /* seconds */
#define GO_BACK 0.42
static struct {
    bool on;                /* breaking */
    double t0;              /* since (< 0: the screen's picture not taken yet) */
    double back;            /* coming back since (0: not) */
    char path[1024], extra[1300];
    bool has_extra;
    UiTex *pic;
} g_go;

static void go_spawn(void)
{
    const char *args[4] = {D.exe, g_go.path, g_go.has_extra ? g_go.extra : NULL, NULL};
#ifdef _WIN32
    {   /* the game gives this window the foreground as it quits (win_main.c give_foreground) */
        char hv[32];
        void *h = SDL_GetPointerProperty(SDL_GetWindowProperties(g_win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        snprintf(hv, sizeof(hv), "%llx", (unsigned long long)(uintptr_t)h);
        SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "XBASED_DASH_HWND", hv, true);
    }
#endif
    g_child = SDL_CreateProcess(args, false);
    if (!g_child) {
        dash_toast(SDL_GetError());
        g_go.back = ui_time();      /* the screen back */
        return;
    }
    /* shown until the game's window takes the screen (no desktop between) */
    g_hide_at = ui_time() + 8.0;
}

void launch_game(LibGame *g, const char *extra)
{
    if (g_go.on || child_running()) {
        return;
    }
    dash_save_cfg();
    lib_set_played(g);      /* the last played first, when the Games screen wants it */
    snprintf(g_go.path, sizeof(g_go.path), "%s", g->g.path);
    g_go.has_extra = extra != NULL;
    snprintf(g_go.extra, sizeof(g_go.extra), "%s", extra ? extra : "");
    snd_play(SND_LAUNCH);
    g_go.back = 0;
    if (!cfg_bool(&D.global, "ui.fx", true)) {
        SDL_Delay(400);         /* the launch's sound, then the game */
        go_spawn();
        return;
    }
    g_go.on = true;
    g_go.t0 = -1;
}

#ifdef _WIN32
/* A game run again (a state loaded, a restart) is a new process on
 * Windows: the one that ends says its id (exit code, bit 30), and the
 * dashboard follows that one instead of coming back. */
static HANDLE g_follow;

static bool follow(DWORD code)
{
    if (!(code & 0x40000000u) || code == STILL_ACTIVE) {
        return false;
    }
    g_follow = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, code & 0x3FFFFFFFu);
    return g_follow != NULL;
}
#endif

static bool child_running(void)
{
#ifdef _WIN32
    if (g_follow) {
        return true;
    }
#endif
    return g_child != NULL;
}

static void wait_child(void)
{
    int code = 0;
#ifdef _WIN32
    if (g_follow) {
        if (WaitForSingleObject(g_follow, 0) != WAIT_OBJECT_0) {
            return;
        }
        DWORD c = 0;
        GetExitCodeProcess(g_follow, &c);
        CloseHandle(g_follow);
        g_follow = NULL;
        if (follow(c)) {
            return;
        }
    } else
#endif
    if (g_child && SDL_WaitProcess(g_child, false, &code)) {
        SDL_DestroyProcess(g_child);
        g_child = NULL;
#ifdef _WIN32
        if (follow((DWORD)code)) {
            return;
        }
#endif
    } else {
        return;
    }
    g_hide_at = 0;
    g_go.back = cfg_bool(&D.global, "ui.fx", true) ? ui_time() : 0;   /* the screen rushes back in */
    SDL_ShowWindow(g_win);
    /* the game had the screen: past Windows' lock on the foreground, so the
     * dashboard has the keyboard and the gamepads again, not just its picture
     * (the game grants it as it ends, this attaches to the foreground's input
     * where that wasn't enough) */
    SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
    SDL_RaiseWindow(g_win);
    SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "0");
    cfg_load(&D.global, D.global.path);     /* the game may have changed settings */
    upscale_migrate(&D.global);
    cc_sync(&D.global);                     /* ... or its EEPROM (a title's own options) */
    g_auto_off = -1;    /* the scripted commands meanwhile were the game's */
}

/* ------------------------------------------------------------ main */

static const char *k_main_items[] = {NULL, NULL, NULL, NULL, NULL};

/* Exit, confirmed: the app closes (from a gamepad too, full screen) */
static void exit_yes(Dash *d)
{
    d->quit = true;
}

static void scr_main(void)
{
    const float W = ui_w(), H = ui_h();
    const enum UiStr items[] = {S_GAMES, S_MEMORY, S_SETTINGS, S_ONLINE, S_EXIT};
    const int n = 5;
    int *sel = &D.sel[SCR_MAIN];
    (void)k_main_items;
    /* a tall window: the orb on top, the menu under it */
    const bool tall = W < H * 1.1f;
    const float ox = tall ? W * 0.5f : W * 0.32f, oy = tall ? H * 0.27f : H * 0.5f;
    const float orb_r = tall ? fminf(H * 0.11f, W * 0.18f) : H * 0.13f;
    ui_orb(ox, oy, orb_r, 0);
    /* the logo, over the orb */
    ui_text(ox, oy - orb_r * 0.36f, "xbased", orb_r * 0.56f, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_CENTER);
    /* the items: green capsules fanned on the orb's right */
    for (int i = 0; i < n; i++) {
        const float t = ease(D.anim * 3 - i * 0.12f);
        const float ang = (i - (n - 1) / 2.0f) * 0.32f;
        const float bw = tall ? W * 0.7f : W * 0.30f, bh = 56;
        const float bx = tall ? (W - bw) / 2 + (1 - t) * 60 : ox + orb_r * 1.5f + cosf(ang) * 40 - 30 + (1 - t) * 60;
        const float by = tall ? oy + orb_r * 1.9f + i * (bh + 18) : oy + sinf(ang) * orb_r * 2.9f - 28;
        const bool s = *sel == i;
        const bool over = D.in.mx >= bx && D.in.mx < bx + bw && D.in.my >= by && D.in.my < by + bh;
        if (over && D.in.moved) {
            *sel = i;
        }
        if (!tall) {
            ui_line(ox + orb_r * 0.9f * cosf(ang), oy + orb_r * 0.9f * sinf(ang), bx, by + bh / 2, s ? 3 : 1.5f,
                    ui_alpha(UI_GREEN, (s ? 0.8f : 0.35f) * t));
        }
        if (s) {
            ui_glow(bx + bw / 2, by + bh / 2, bw * 0.7f, ui_alpha(UI_GREEN, 0.25f * t));
        }
        ui_round(bx, by, bw, bh, bh / 2, ui_alpha(s ? (UiColor){0x3C, 0x9A, 0x12, 0xFF} : (UiColor){0x14, 0x3A, 0x08, 0xFF}, 0.92f * t));
        ui_round_outline(bx, by, bw, bh, bh / 2, s ? 2.5f : 1.5f, ui_alpha(s ? UI_GREEN_HI : UI_GREEN_DIM, t));
        ui_text(bx + 34, by + 11, T(items[i]), 32, UI_FONT_BOLD, ui_alpha(s ? UI_WHITE : UI_GREY, t), UI_ALIGN_LEFT);
        if ((s && D.in.ok) || (over && D.in.click)) {
            switch (items[i]) {
            case S_GAMES: dash_push(SCR_GAMES); break;
            case S_MEMORY: dash_push(SCR_MEMORY); break;
            case S_SETTINGS: dash_push(SCR_SETTINGS); break;
            case S_ONLINE: dash_push(SCR_ONLINE); break;
            default: dash_ask(T(S_CONFIRM_EXIT), exit_yes); break;
            }
        }
    }
    if (D.in.up) {
        *sel = (*sel + n - 1) % n;
    }
    if (D.in.down) {
        *sel = (*sel + 1) % n;
    }
    if (D.in.y) {
        dash_push(SCR_ABOUT);
    }
    char v[64];
    snprintf(v, sizeof(v), "xbased %s", XBASED_VERSION);
    ui_text(W - 60, H - 52, v, 20, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_RIGHT);
    draw_hints(T(S_SELECT), NULL, NULL, T(S_ABOUT));
}

/* ------------------------------------------------------------ games */

static void draw_card(LibGame *g, float x, float y, float w, float h, bool sel, float alpha)
{
    if (g->hidden) {
        alpha *= 0.45f;     /* a hidden game, shown (Show hidden games) */
    }
    if (sel) {
        ui_glow(x + w / 2, y + h / 2, w * 1.1f, ui_alpha(UI_GREEN, 0.35f * alpha));
    }
    ui_round(x, y, w, h, 10, ui_alpha((UiColor){0x0E, 0x26, 0x07, 0xFF}, alpha));
    UiTex *cv = lib_cover(g);
    if (cv) {
        ui_image(cv, x + 4, y + 4, w - 8, h - 8, alpha);
    } else {
        /* no box art: the disc's own icon on a green card, its title under */
        ui_rect_grad(x + 4, y + 4, w - 8, h - 8, ui_alpha((UiColor){0x1E, 0x52, 0x0A, 0xFF}, alpha),
                     ui_alpha((UiColor){0x06, 0x14, 0x03, 0xFF}, alpha));
        UiTex *ic = lib_icon(g);
        /* sized to the card: the small covers' view has them a third as big */
        const float fs = fminf(17, fmaxf(10, w * 0.13f)), is = fminf(w * 0.62f, h - fs * 4.2f);
        if (ic) {
            ui_image(ic, x + (w - is) / 2, y + h * 0.12f, is, is, alpha);
        } else {
            ui_text(x + w / 2, y + h * 0.12f + is * 0.12f, "X", is * 0.7f, UI_FONT_BOLD, ui_alpha(UI_GREEN, alpha),
                    UI_ALIGN_CENTER);
        }
        ui_text_fit(x + w / 2, y + h - fs * 2.6f, g->g.title, fs, UI_FONT_BOLD, ui_alpha(UI_WHITE, alpha),
                    UI_ALIGN_CENTER, w - 10);
    }
    ui_round_outline(x, y, w, h, 10, sel ? 3 : 1, ui_alpha(sel ? UI_GREEN_HI : UI_GREEN_DIM, alpha));
}

/* the last card of the grid: the file browser, games added */
static void draw_add_card(float x, float y, float w, float h, bool sel, float alpha)
{
    if (sel) {
        ui_glow(x + w / 2, y + h / 2, w * 1.1f, ui_alpha(UI_GREEN, 0.35f * alpha));
    }
    ui_round(x, y, w, h, 10, ui_alpha((UiColor){0x08, 0x16, 0x05, 0xE8}, alpha));
    const float cx = x + w / 2, cy = y + h * 0.4f, r = w * 0.26f;
    ui_ring(cx, cy, r, r, 0, 3, ui_alpha(sel ? UI_GREEN_HI : UI_GREEN, alpha));
    ui_rect(cx - r * 0.5f, cy - 3, r, 6, ui_alpha(sel ? UI_GREEN_HI : UI_GREEN, alpha));
    ui_rect(cx - 3, cy - r * 0.5f, 6, r, ui_alpha(sel ? UI_GREEN_HI : UI_GREEN, alpha));
    ui_text_fit(cx, y + h * 0.68f, T(S_ADD_GAMES), 20, UI_FONT_BOLD, ui_alpha(sel ? UI_WHITE : UI_GREY, alpha),
                UI_ALIGN_CENTER, w - 14);
    ui_round_outline(x, y, w, h, 10, sel ? 3 : 1, ui_alpha(sel ? UI_GREEN_HI : UI_GREEN_DIM, alpha));
}

/* a favorite's star: five points on a dark disc, top right of a card */
static void draw_star(float cx, float cy, float r, float alpha)
{
    ui_circle(cx, cy, r * 1.25f, ui_alpha((UiColor){0x08, 0x10, 0x04, 0xFF}, 0.85f * alpha));
    SDL_FPoint p[10];
    for (int i = 0; i < 10; i++) {
        const float a = -1.5707963f + i * 0.6283185f, k = i % 2 ? r * 0.45f : r;
        p[i] = (SDL_FPoint){cx + cosf(a) * k, cy + sinf(a) * k};
    }
    const UiColor gold = ui_alpha((UiColor){0xF0, 0xC0, 0x20, 0xFF}, alpha);
    for (int i = 0; i < 10; i++) {
        ui_line(cx, cy, p[i].x, p[i].y, r * (i % 2 ? 0.5f : 0.36f), gold);
        ui_line(p[i].x, p[i].y, p[(i + 1) % 10].x, p[(i + 1) % 10].y, 1.6f, gold);
    }
}

/* the icons view's tile: the disc's icon, the title, the publisher and year under */
static void draw_tile(LibGame *g, float x, float y, float w, float h, bool sel, float alpha)
{
    if (g->hidden) {
        alpha *= 0.45f;
    }
    if (sel) {
        ui_glow(x + w / 2, y + h / 2, w * 0.7f, ui_alpha(UI_GREEN, 0.3f * alpha));
    }
    ui_round(x, y, w, h, 8, ui_alpha(sel ? (UiColor){0x1A, 0x44, 0x0C, 0xFF} : (UiColor){0x0E, 0x26, 0x07, 0xFF}, alpha));
    const float is = h - 10;
    UiTex *ic = lib_icon(g);
    if (ic) {
        ui_image(ic, x + 5, y + 5, is, is, alpha);
    } else {
        ui_round(x + 5, y + 5, is, is, 6, ui_alpha((UiColor){0x1E, 0x52, 0x0A, 0xFF}, alpha));
        ui_text(x + 5 + is / 2, y + 5 + is * 0.18f, "X", is * 0.6f, UI_FONT_BOLD, ui_alpha(UI_GREEN, alpha),
                UI_ALIGN_CENTER);
    }
    const float tx = x + is + 14, tw = w - is - 22 - (g->fav ? 18 : 0);
    ui_text_fit(tx, y + 6, g->g.title, 19, sel ? UI_FONT_BOLD : UI_FONT_REGULAR, ui_alpha(UI_WHITE, alpha),
                UI_ALIGN_LEFT, tw);
    char info[120];
    const char *pub = lib_publisher(g);
    if (g->g.year) {
        snprintf(info, sizeof(info), "%s%s%d", pub, *pub ? " · " : "", g->g.year);
    } else {
        snprintf(info, sizeof(info), "%s", pub);
    }
    ui_text_fit(tx, y + h - 26, info, 15, UI_FONT_REGULAR, ui_alpha(UI_GREY, alpha), UI_ALIGN_LEFT, w - is - 22);
    if (g->fav) {
        draw_star(x + w - 14, y + 15, 7, alpha);
    }
    ui_round_outline(x, y, w, h, 8, sel ? 2.5f : 1, ui_alpha(sel ? UI_GREEN_HI : UI_GREEN_DIM, alpha));
}

static void open_game(LibGame *g)
{
    D.cur = g;
    char p[1200];
    game_cfg_path(g, p, sizeof(p));
    cfg_load(&D.game, p);
    upscale_migrate(&D.game);
    D.sel[SCR_GAME] = 0;
    dash_push(SCR_GAME);
}

#define MAX_SHOWN 16384

static void scr_games(void)
{
    const float W = ui_w(), H = ui_h();
    /* hidden games left out here only (the rest of the dashboard sees them all) */
    lib_show_hidden(cfg_bool(&D.global, "ui.showhidden", false));
    static LibGame *vis[MAX_SHOWN];
    const int total = lib_count(), n = dash_find_list(vis, MAX_SHOWN);
    const char *desc = dash_find_desc();
    const bool filtered = desc[0] != 0;
    char sub[300];
    if (filtered) {
        char cnt[64];
        snprintf(cnt, sizeof(cnt), T(S_SHOWING), n, total);
        snprintf(sub, sizeof(sub), "%s    %s", cnt, desc);
    } else {
        snprintf(sub, sizeof(sub), "%d", total);
    }
    draw_header(T(S_GAMES), sub);
    if (D.in.y || D.in.find) {
        dash_find_open(true);   /* the search, the keyboard up */
        return;
    }
    if (D.in.view) {
        dash_find_next_view();
    }
    if (!total) {
        ui_text(W / 2, H * 0.45f, lib_scanning() ? T(S_SCANNING) : T(S_NO_GAMES), 26, UI_FONT_REGULAR, UI_WHITE,
                UI_ALIGN_CENTER);
        if (D.in.back) {
            dash_pop();
        }
        draw_hints(T(S_ADD_GAMES), T(S_BACK), NULL, NULL);    /* while it scans too */
        if (D.in.ok || D.in.click) {
            dash_browse_games();
        }
        return;
    }
    if (!n) {
        /* the search found nothing: say so, B clears it */
        ui_text(W / 2, H * 0.42f, T(S_NO_MATCH), 26, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_CENTER);
        if (D.in.back) {
            dash_find_clear();
        }
        draw_hints(NULL, T(S_CLEAR_FILTERS), NULL, T(S_FIND));
        return;
    }
    /* the games, then (no search on) a card that adds more (the file browser) */
    const int nc = n + (filtered ? 0 : 1);
    const int view = dash_find_view();
    const float top = 140, bottom = H - 150, gw = W - 120;
    float cw, ch, gx, gy;   /* a cell, the gaps between */
    int cols, vrows;        /* across, and rows on screen */
    if (view == 0) {
        /* big box-shaped cards (box art is 7:10): the largest whose rows (one
         * or two) hold every game; more: two rows that scroll */
        gx = 24;
        gy = 36;
        ch = (bottom - top) / 2 - 18;
        cw = ch * 0.70f;
        cols = (int)((gw + gx) / (cw + gx)) > 0 ? (int)((gw + gx) / (cw + gx)) : 1;
        float best = 0;
        for (int r = 1; r <= 2; r++) {
            const int need = (nc + r - 1) / r;
            const float by_h = (bottom - top - 30) / r - (r > 1 ? 18 : 0);
            const float by_w = ((gw + gx) / need - gx) / 0.70f;
            const float chr = fminf(by_h, by_w);
            if (chr > best && chr >= ch * 0.9f) {
                best = chr;
                ch = chr;
                cw = chr * 0.70f;
                cols = (int)((gw + gx) / (cw + gx));
            }
        }
        vrows = 2;
    } else if (view == 1) {
        /* small covers: four rows */
        gx = 14;
        gy = 16;
        vrows = 4;
        ch = (bottom - top + 10 - gy * (vrows - 1)) / vrows;
        cw = ch * 0.70f;
        cols = (int)((gw + gx) / (cw + gx));
    } else {
        /* icons and names: rows of tiles */
        gx = 14;
        gy = 10;
        ch = 58;
        cw = 272;
        vrows = (int)((bottom - top + 10 + gy) / (ch + gy));
        cols = (int)((gw + gx) / (cw + gx));
    }
    cols = cols > 0 ? cols : 1;
    const float x0 = (W - (cols * (cw + gx) - gx)) / 2;
    /* the selection is the game's: it stays on it while games join or the order changes */
    static LibGame *s_selg;
    static bool s_on_add;
    int *sel = &D.sel[SCR_GAMES], *scroll = &D.scroll[SCR_GAMES];
    if (s_on_add && !filtered) {
        *sel = n;
    } else if (s_selg) {
        for (int i = 0; i < n; i++) {
            if (vis[i] == s_selg) {
                *sel = i;
                break;
            }
        }
    }
    *sel = *sel >= nc ? nc - 1 : *sel < 0 ? 0 : *sel;
    if (D.in.left) {
        *sel = *sel > 0 ? *sel - 1 : nc - 1;
    }
    if (D.in.right) {
        *sel = (*sel + 1) % nc;
    }
    if (D.in.up && *sel >= cols) {
        *sel -= cols;
    }
    if (D.in.down) {
        *sel = *sel + cols < nc ? *sel + cols : nc - 1;
    }
    if (D.in.lb) {
        *sel = *sel >= cols * vrows ? *sel - cols * vrows : 0;
    }
    if (D.in.rb) {
        *sel = *sel + cols * vrows < nc ? *sel + cols * vrows : nc - 1;
    }
    /* the triggers: the next (or this, then the last) group's first game,
     * by the order (a letter, a publisher, a year; the favorites) */
    static double s_jump_at;
    static char s_jump[80];
    if ((D.in.lt || D.in.rt) && n) {
        int i = *sel < n ? *sel : n - 1;
        const int gcur = dash_find_group(vis[i]);
        if (D.in.rt) {
            while (i < n && dash_find_group(vis[i]) == gcur) {
                i++;
            }
            i = i < n ? i : *sel < n ? *sel : n - 1;    /* the last group: stays */
        } else {
            if ((*sel >= n || i > 0) && (*sel >= n || dash_find_group(vis[i - 1]) == gcur)) {
                /* within a group (or on the add card, after the last): this group's first */
                while (i > 0 && dash_find_group(vis[i - 1]) == gcur) {
                    i--;    /* this group's first */
                }
            } else if (i > 0) {
                const int gprev = dash_find_group(vis[i - 1]);
                i--;
                while (i > 0 && dash_find_group(vis[i - 1]) == gprev) {
                    i--;
                }
            }
        }
        *sel = i;
        snprintf(s_jump, sizeof(s_jump), "%s", dash_find_group_name(vis[i]));
        s_jump_at = ui_time();
        snd_play(SND_TAB);
    }
    if (D.in.wheel) {
        *scroll -= (int)D.in.wheel;
    }
    const int rows = (nc + cols - 1) / cols;
    static int s_prev = -1, s_prev_cols, s_prev_view;
    if (*sel != s_prev || cols != s_prev_cols || view != s_prev_view) {
        /* the selection moved (a press, a jump, games joining before it): in sight */
        if (*sel / cols < *scroll) {
            *scroll = *sel / cols;
        }
        if (*sel / cols > *scroll + vrows - 1) {
            *scroll = *sel / cols - vrows + 1;
        }
    }
    *scroll = *scroll > rows - vrows ? rows - vrows : *scroll;
    *scroll = *scroll < 0 ? 0 : *scroll;
    /* the rows on screen (and the selected card's glow), not the next one's top */
    ui_clip(0, top - 20, W, vrows * (ch + gy) - gy + 20 + (view == 2 ? 6 : 16));
    for (int r = 0; r <= vrows; r++) {
        for (int c = 0; c < cols; c++) {
            const int i = (*scroll + r) * cols + c;
            if (i >= nc) {
                break;
            }
            const bool s = i == *sel;
            const float k = s && view < 2 ? (view ? 1.08f : 1.06f) : 1.0f;
            const float x = x0 + c * (cw + gx) - cw * (k - 1) / 2, y = top + r * (ch + gy) - ch * (k - 1) / 2;
            const bool over = D.in.mx >= x && D.in.mx < x + cw && D.in.my >= y && D.in.my < y + ch &&
                              D.in.my < bottom + 20;
            if (over && D.in.moved) {
                *sel = i;
            }
            const float a = ease(D.anim * 4 - (r * cols + c) * (view ? 0.01f : 0.04f));
            if (i == n) {
                draw_add_card(x, y, cw * k, ch * k, s, a);
                if (over && D.in.click) {
                    dash_browse_games();
                }
                continue;
            }
            LibGame *g = vis[i];
            if (view == 2) {
                draw_tile(g, x, y, cw, ch, s, a);
            } else {
                draw_card(g, x, y, cw * k, ch * k, s, a);
                if (g->fav) {
                    draw_star(x + cw * k - (view ? 11 : 18), y + (view ? 11 : 18), view ? 6 : 10, a);
                }
            }
            if (over && D.in.click) {
                open_game(g);
            }
        }
    }
    ui_unclip();
    s_prev = *sel;
    s_prev_cols = cols;
    s_prev_view = view;
    s_on_add = *sel == n && !filtered;
    s_selg = *sel < n ? vis[*sel] : NULL;
    /* the letter (or group) jumped to, big, for a moment */
    if (s_jump[0] && ui_time() - s_jump_at < 0.9) {
        const float t = (float)(ui_time() - s_jump_at), al = t < 0.6f ? 1 : 1 - (t - 0.6f) / 0.3f;
        const float tw = ui_text_width(s_jump, 64, UI_FONT_BOLD), bw = fmaxf(tw + 60, 130), bh = 104;
        ui_round((W - bw) / 2, (H - bh) / 2 - 20, bw, bh, 18, ui_alpha((UiColor){0x08, 0x1A, 0x04, 0xF0}, al));
        ui_round_outline((W - bw) / 2, (H - bh) / 2 - 20, bw, bh, 18, 2, ui_alpha(UI_GREEN_HI, al));
        ui_text(W / 2, (H - bh) / 2 - 2, s_jump, 64, UI_FONT_BOLD, ui_alpha(UI_WHITE, al), UI_ALIGN_CENTER);
    }
    LibGame *g = s_selg;
    if (*sel == n) {
        ui_text_fit(60, H - 128, T(S_ADD_GAMES), 32, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT, W - 400);
        if (D.in.ok) {
            dash_browse_games();
        }
    }
    if (g) {
        const float tw = ui_text_fit(60, H - 128, g->g.title, 32, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT, W - 560);
        if (g->fav) {
            draw_star(60 + tw + 22, H - 110, 10, 1);
        }
        char info[200];
        const char *pub = lib_publisher(g);
        char year[16] = "";
        if (g->g.year) {
            snprintf(year, sizeof(year), "%d  ", g->g.year);
        }
        snprintf(info, sizeof(info), "%s%s%s%08X  v%u  %.1f GB", pub, *pub ? "  " : "", year, g->g.title_id,
                 g->g.version, g->g.size / 1073741824.0);
        ui_text_fit(W - 60, H - 120, info, 20, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_RIGHT, 480);
        if (D.in.ok) {
            open_game(g);
        }
        if (D.in.x) {
            launch_game(g, NULL);   /* straight in */
        }
    }
    const char *fs = lib_fetch_status();
    if (fs) {
        ui_text(W - 60, lib_scanning() ? 116 : 84, fs, 18, UI_FONT_REGULAR, UI_GREEN, UI_ALIGN_RIGHT);
    }
    if (D.in.back) {
        if (filtered) {
            dash_find_clear();  /* the search off first, then out */
        } else {
            dash_pop();
        }
    }
    float hx = draw_hints(T(S_SELECT), T(filtered ? S_CLEAR_FILTERS : S_BACK), T(S_PLAY), T(S_FIND));
    hx = draw_hint_at(hx, 4, T(S_JUMP));
    draw_hint_at(hx, 5, T(S_VIEW));
}

/* ------------------------------------------------------------ a game */

static char g_cover_file[1024];
static SDL_AtomicInt g_cover_file_set;

static void SDLCALL cover_file_cb(void *ud, const char *const *files, int filter)
{
    (void)ud;
    (void)filter;
    if (files && files[0] && !SDL_GetAtomicInt(&g_cover_file_set)) {
        snprintf(g_cover_file, sizeof(g_cover_file), "%s", files[0]);
        SDL_SetAtomicInt(&g_cover_file_set, 1);
    }
}

/* The game's name, edited in place: Enter (A) to type and again to keep,
 * Esc to leave it; kept empty, the disc's own again. */
static void rename_row(List *l, LibGame *g)
{
    static const char k_rename[] = "rename";
    const bool editing = D.edit_key == k_rename;
    char shown[300];
    snprintf(shown, sizeof(shown), "%s%s", editing ? D.edit_tmp : g->g.title,
             editing && (int)(ui_time() * 2) % 2 ? "_" : "");
    if (list_row(l, T(S_RENAME), shown, NULL)) {
        if (editing) {
            lib_set_name(g, D.edit_tmp);
            D.edit_key = NULL;
            D.typing = false;
            SDL_StopTextInput(SDL_GetKeyboardFocus());
        } else {
            D.edit_key = k_rename;
            D.typing = true;
            snprintf(D.edit_tmp, sizeof(D.edit_tmp), "%s", g->g.title);
            SDL_StartTextInput(SDL_GetKeyboardFocus());
        }
    }
    if (editing) {
        if (D.in.text && strlen(D.edit_tmp) + strlen(D.in.text) < sizeof(D.edit_tmp)) {
            strcat(D.edit_tmp, D.in.text);
        }
        if (D.in.key == SDLK_BACKSPACE && D.edit_tmp[0]) {
            size_t n = strlen(D.edit_tmp);
            do {
                n--;                        /* a whole UTF-8 character */
            } while (n > 0 && ((unsigned char)D.edit_tmp[n] & 0xC0) == 0x80);
            D.edit_tmp[n] = 0;
            D.in.back = false;
        }
        if (D.in.key == SDLK_ESCAPE || D.in.back || !list_selected(l)) {
            /* left as it was (B, Esc, another row) */
            D.edit_key = NULL;
            D.typing = false;
            SDL_StopTextInput(SDL_GetKeyboardFocus());
            D.in.back = false;
        }
    }
}

static void scr_game(void)
{
    LibGame *g = D.cur;
    const float W = ui_w(), H = ui_h();
    draw_header(g->g.title, NULL);
    const float ch = H - 300, cw = ch * 0.7f;
    draw_card(g, 70, 150, cw, ch, false, ease(D.anim * 4));
    /* the facts under the cover */
    char info[200];
    static const char *const k_reg[8] = {"-", "NTSC-U", "NTSC-J", "NTSC-U/J", "PAL", "NTSC-U/PAL", "NTSC-J/PAL",
                                         "World"};
    const char *pub = lib_publisher(g);
    char year[16] = "";
    if (g->g.year) {
        snprintf(year, sizeof(year), "   %d", g->g.year);
    }
    snprintf(info, sizeof(info), "%s %08X   v%u   %s %s%s%s%s", T(S_TITLE_ID), g->g.title_id, g->g.version,
             T(S_REGION), k_reg[g->g.regions & 7], *pub ? "   " : "", pub, year);
    ui_text_fit(70, 160 + ch, info, 18, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT, W - 140);
    List l;
    list_begin(&l, 110 + cw, 150, W - 180 - cw, ch, 60);
    if (list_row(&l, T(S_PLAY), NULL, NULL)) {
        launch_game(g, NULL);
    }
    if (list_row(&l, T(S_LOAD_STATE), NULL, NULL)) {
        D.sel[SCR_STATES] = 0;
        dash_push(SCR_STATES);
    }
    if (list_row(&l, T(S_GAME_SETTINGS), NULL, NULL)) {
        D.editing_game = true;
        D.sel[SCR_GAME_SETTINGS] = 0;
        dash_push(SCR_GAME_SETTINGS);
    }
    if (list_row(&l, T(S_MODS), NULL, NULL)) {
        D.editing_game = true;
        D.sel[SCR_MODS] = 0;
        dash_push(SCR_MODS);
    }
    if (list_row(&l, T(S_CONTROLS), NULL, NULL)) {
        D.editing_game = true;
        D.player = 0;
        dash_push(SCR_CONTROLS);
    }
    if (list_row(&l, T(S_DOWNLOAD_COVER), NULL, NULL)) {
        lib_fetch_cover(g);
    }
    if (list_row(&l, T(S_CHOOSE_COVER), NULL, NULL)) {
        dash_browse_file(T(S_CHOOSE_COVER), "png;jpg;jpeg", cover_file_cb, NULL);
    }
    if (list_row(&l, T(g->fav ? S_REMOVE_FAV : S_ADD_FAV), NULL, NULL)) {
        lib_set_favorite(g, !g->fav);
        dash_toast(T(g->fav ? S_FAV_ADDED : S_FAV_REMOVED));
    }
    rename_row(&l, g);
    if (list_row(&l, T(S_SHORTCUT), NULL, NULL)) {
        char png[1200], ico[1200], where[1200];
        const bool icon = lib_icon_files(g, png, sizeof(png), ico, sizeof(ico));
        dash_toast(sc_make(g->g.title, g->g.path, icon ? png : NULL, icon ? ico : NULL, where, sizeof(where))
                       ? T(S_SHORTCUT_DONE) : T(S_SHORTCUT_FAIL));
    }
    if (list_row(&l, T(g->hidden ? S_UNHIDE_GAME : S_HIDE_GAME), NULL, NULL)) {
        lib_set_hidden(g, !g->hidden);
        if (g->hidden && !cfg_bool(&D.global, "ui.showhidden", false)) {
            dash_pop();     /* gone from the Games screen */
            return;
        }
    }
    list_end(&l);
    /* a cover image picked (the dialog's callback may run on a thread of its own) */
    if (SDL_GetAtomicInt(&g_cover_file_set)) {
        if (!lib_set_cover_file(g, g_cover_file)) {
            dash_toast(T(S_COVER_BAD));
        }
        SDL_SetAtomicInt(&g_cover_file_set, 0);
    }
    const char *fs = lib_fetch_status();
    if (fs) {
        ui_text(W - 60, lib_scanning() ? 116 : 84, fs, 18, UI_FONT_REGULAR, UI_GREEN, UI_ALIGN_RIGHT);
    }
    if (D.in.back) {
        dash_pop();
    }
    draw_hints(T(S_SELECT), T(S_BACK), NULL, NULL);
}

static void scr_about(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(T(S_ABOUT), NULL);
    ui_orb(W / 2, H * 0.36f, 60, 0);
    char v[96];
    snprintf(v, sizeof(v), "%s %s", T(S_VERSION), XBASED_VERSION);
    ui_text(W / 2, H * 0.52f, "xbased", 54, UI_FONT_BOLD, UI_GREEN_HI, UI_ALIGN_CENTER);
    ui_text(W / 2, H * 0.62f, v, 22, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_CENTER);
    ui_text_fit(W / 2, H * 0.67f, T(S_BASED_ON), 20, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_CENTER, W - 80);
    ui_text(W / 2, H * 0.715f, "github.com/crazyriddler/xbased", 20, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_CENTER);
    ui_text(W / 2, H * 0.77f, "Fonts: Play (SIL OFL). Box art: libretro-thumbnails.", 18, UI_FONT_REGULAR, UI_GREY,
            UI_ALIGN_CENTER);
    if (D.in.back || D.in.ok) {
        dash_pop();
    }
    draw_hints(NULL, T(S_BACK), NULL, NULL);
}

/* ------------------------------------------------------------ the question */

static void draw_ask(void)
{
    const float W = ui_w(), H = ui_h();
    ui_rect(0, 0, W, H, (UiColor){0, 0, 0, 170});
    /* three answers take a wider panel (and the longer questions they come with) */
    const float w = D.nchoices == 3 ? 900 : 620, fs = D.nchoices == 3 ? 24 : 26;
    /* a longer question on lines of its own, the panel taller for them */
    char qlines[5][512];
    const int nq = wrap_lines(D.ask, fs, UI_FONT_BOLD, w - 40, qlines, 5);
    const float extra = (nq > 1 ? nq - 1 : 0) * (fs + 8);
    const float h = 230 + extra, x = (W - w) / 2, y = (H - h) / 2;
    ui_glow(W / 2, H / 2, w * 0.8f, ui_alpha(UI_GREEN, 0.18f));
    ui_round(x, y, w, h, 18, (UiColor){0x0A, 0x1C, 0x06, 0xF4});
    ui_round_outline(x, y, w, h, 18, 2, UI_GREEN);
    for (int i = 0; i < nq; i++) {
        ui_text_fit(W / 2, y + 50 + i * (fs + 8), qlines[i], fs, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_CENTER, w - 40);
    }
    /* the selection: No for a yes/no question (the safe one), else the first */
    static int sel = -1;
    static const char *sel_for;
    if (sel_for != D.ask) {
        sel_for = D.ask;     /* a new question (one closed elsewhere left its selection) */
        sel = -1;
    }
    const int n = D.nchoices ? D.nchoices : 2;
    const char *opts[3] = {T(S_YES), T(S_NO), NULL};
    for (int i = 0; D.nchoices && i < n; i++) {
        opts[i] = D.choices[i];
    }
    if (sel < 0) {
        sel = D.nchoices ? 0 : 1;
    }
    const float bw = n == 3 ? 260 : 220, gap = n == 3 ? 24 : 40, bh = 56;
    const float x0 = x + (w - (n * bw + (n - 1) * gap)) / 2;
    for (int i = 0; i < n; i++) {
        const float bx = x0 + i * (bw + gap), by = y + 130 + extra;
        const bool over = D.in.mx >= bx && D.in.mx < bx + bw && D.in.my >= by && D.in.my < by + bh;
        if (over && D.in.moved) {
            sel = i;
        }
        const bool s = i == sel;
        ui_round(bx, by, bw, bh, 28, s ? (UiColor){0x3C, 0x9A, 0x12, 0xFF} : (UiColor){0x14, 0x3A, 0x08, 0xFF});
        ui_round_outline(bx, by, bw, bh, 28, 2, s ? UI_GREEN_HI : UI_GREEN_DIM);
        ui_text_fit(bx + bw / 2, by + 14, opts[i], n == 3 ? 22 : 28, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_CENTER,
                    bw - 16);
        if (over && D.in.click) {
            sel = i;
            D.in.ok = true;
        }
    }
    if (D.in.left) {
        sel = (sel + n - 1) % n;
    } else if (D.in.right) {
        sel = (sel + 1) % n;
    }
    if (D.in.ok || D.in.back) {
        const int chosen = D.in.ok ? sel : -1;
        void (*yes_cb)(Dash *) = D.nchoices ? NULL : D.on_yes;
        void (*choice_cb)(Dash *, int) = D.nchoices ? D.on_choice : NULL;
        D.ask = NULL;
        D.nchoices = 0;
        sel = -1;
        if (yes_cb && chosen == 0) {
            yes_cb(&D);
        } else if (choice_cb) {
            choice_cb(&D, chosen);
        }
    }
}

/* ------------------------------------------------------------ updates */

static void update_chosen(Dash *d, int choice)
{
    if (choice == 0) {
        if (upd_can_install()) {
            upd_install_start();
        } else {
            SDL_OpenURL(upd_page());    /* a copy that can't replace itself: the release's page */
        }
    } else if (choice == 2) {
        cfg_set_bool(&d->global, "update.check", false);
        cfg_save(&d->global);
        dash_toast(T(S_UPDATE_TURNED_OFF));
    }
}

/* A newer release (update.c): asked about once, on the main screen; then
 * its install's progress, and the new version started in this one's place. */
static void update_poll(void)
{
    static bool asked, failed;
    char msg[256];
    switch (upd_state()) {
    case UPD_AVAILABLE:
        if (!asked && !D.ask && dash_top() == SCR_MAIN) {
            asked = true;
            snprintf(msg, sizeof(msg), T(S_UPDATE_AVAILABLE), upd_version(), XBASED_VERSION);
            const char *choices[3] = {T(S_UPDATE_NOW), T(S_NOT_NOW), T(S_DONT_ASK)};
            dash_choose(msg, choices, 3, update_chosen);
        }
        break;
    case UPD_INSTALLING:
        snprintf(msg, sizeof(msg), T(S_UPDATING), upd_version());
        snprintf(D.toast, sizeof(D.toast), "%s", msg);
        D.toast_until = ui_time() + 0.5;
        break;
    case UPD_INSTALLED:
        if (upd_relaunch()) {
            D.quit = true;
            break;
        }
        /* fall through: started by hand, from the page */
    case UPD_FAILED:
        if (!failed) {
            failed = true;
            dash_toast(T(S_UPDATE_FAILED));
            SDL_OpenURL(upd_page());
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ the loop */

/* what the library scans: the game folders, and the game files added one by one */
static void folders(const char ***out, int *n)
{
    static const char *v[48];
    *n = cfg_all(&D.global, "gamedir", v, 16);
    *n += cfg_all(&D.global, "gamefile", v + *n, 32);
    *out = v;
}

int dash_run(SDL_Window *win, const char *exe)
{
    memset(&D, 0, sizeof(D));
    g_win = win;
    SDL_HideCursor();   /* it shows when the mouse moves (poll_input) */
    g_cursor_at = 0;
    snprintf(D.exe, sizeof(D.exe), "%s", exe);
    char p[1200];
    snprintf(p, sizeof(p), "%sxbased.ini", cfg_data_dir());
    cfg_load(&D.global, p);
    if (upscale_migrate(&D.global)) {
        cfg_save(&D.global);
    }
    if (cfg_get(&D.global, "uncap")) {
        /* the frame rate is each game's now (Mods & patches): no global one left unseen */
        cfg_unset(&D.global, "uncap");
        cfg_save(&D.global);
    }
    i18n_set(cfg_get(&D.global, "ui.lang"));
    cc_sync(&D.global);     /* the EEPROM changed by hand or by a title meanwhile */
    upd_cleanup();
    if (cfg_bool(&D.global, "update.check", true)) {
        upd_check_start(XBASED_VERSION);
    }
    char fonts[1200];
    snprintf(fonts, sizeof(fonts), "%sfonts/", cfg_exe_dir());
    if (!ui_init(win, fonts)) {
        return 1;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    snd_init();
    const char **fv;
    int fn;
    folders(&fv, &fn);
    lib_scan(fv, fn);
    if (cfg_bool(&D.global, "covers", true) && fn) {
        /* box art for games without, once the scan is done (lib_poll) */
    }
    bool was_scanning = true;
    while (!D.quit) {
        lib_set_busy(child_running());  /* the scan goes on while a game plays, gently */
        if (child_running()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) {
                    D.quit = true;
                } else if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST && g_hide_at > 0) {
                    g_hide_at = ui_time();
                }
            }
            if (g_hide_at > 0 && ui_time() >= g_hide_at) {
                SDL_HideWindow(g_win);
                g_hide_at = 0;
            }
            wait_child();
            SDL_Delay(50);
            continue;
        }
        poll_input();
        auto_poll();
        dash_take_folder();
        lib_poll();
        update_poll();
        /* a scan done: box art for the games without (those found since too) */
        const bool scanning = lib_scanning();
        if (was_scanning && !scanning && cfg_bool(&D.global, "covers", true) && lib_count()) {
            lib_fetch_all_covers();
        }
        was_scanning = scanning;
        const In saved = D.in;
        const bool asked = D.ask != NULL;  /* a question asked this frame answers from the next */
        if (D.ask) {
            /* a question takes the input; the screen under it sees none */
            memset(&D.in, 0, sizeof(D.in));
            D.in.mx = D.in.my = -1;
            D.in.pad = saved.pad;
        }
        /* the sounds of what was pressed */
        if (!D.ask && dash_top() != SCR_REMAP) {
            if (saved.ok || saved.click) {
                snd_play(SND_SELECT);
            } else if (saved.back) {
                snd_play(D.sp ? SND_BACK : SND_ERROR);
            } else if (saved.lb || saved.rb) {
                snd_play(SND_TAB);
            } else if (saved.up || saved.down || saved.left || saved.right) {
                snd_play(SND_MOVE);
            }
        }
        snd_enable(cfg_bool(&D.global, "ui.sounds", true));
        if (g_go.on && g_go.t0 >= 0) {
            /* into the game: the screen's picture breaks; the game's window
             * comes up on the black it leaves */
            const float k = (float)((ui_time() - g_go.t0) / GO_OUT);
            ui_begin();
            ui_fx_shatter(g_go.pic, k);
            auto_shot();
            ui_end();
            if (k >= 1) {
                g_go.on = false;
                go_spawn();
            }
            continue;
        }
        const bool go_pic = g_go.on;    /* the frame the launch was asked in, again: its picture */
        const bool go_back = g_go.back > 0 && ui_time() - g_go.back < GO_BACK;
        if (!go_back) {
            g_go.back = 0;
        }
        const int sp_before = D.sp;
        const enum Screen top_before = dash_top();
        ui_begin();
        if (go_pic || go_back) {
            g_go.pic = ui_layer_begin(g_go.pic);
        }
        ui_backdrop(cfg_bool(&D.global, "ui.anim", true) ? 1.0f : 0.0f);
        /* the screen's foreground in a layer: the transitions turn it in 3D */
        g_layer = ui_layer_begin(g_layer);
        lib_show_hidden(true);     /* the Games screen leaves hidden games out while it draws */
        switch (dash_top()) {
        case SCR_MAIN: scr_main(); break;
        case SCR_GAMES: scr_games(); break;
        case SCR_GAME: scr_game(); break;
        case SCR_GAME_SETTINGS: scr_game_settings(); break;
        case SCR_MODS: scr_mods(); break;
        case SCR_STATES: scr_states(); break;
        case SCR_SETTINGS: scr_settings(); break;
        case SCR_CONTROLS: scr_controls(); break;
        case SCR_REMAP: scr_remap(); break;
        case SCR_STICKS: scr_sticks(); break;
        case SCR_TRIGGERS: scr_triggers(); break;
        case SCR_MEMORY: scr_memory(); break;
        case SCR_MEMORY_TITLE: scr_memory_title(); break;
        case SCR_ONLINE: scr_online(); break;
        case SCR_ABOUT: scr_about(); break;
        case SCR_BROWSE: scr_browse(); break;
        case SCR_FIND: scr_find(); break;
        default: break;
        }
        ui_layer_end();
        /* a screen pushed: the old one swings away to the left, the new one
         * in from the right (popped: the other way), in perspective */
        if (D.sp != sp_before || dash_top() != top_before) {
            UiTex *t = g_prev;
            g_prev = g_layer;
            g_layer = t;
            g_trans = 0;
            g_trans_dir = D.sp >= sp_before ? 1 : -1;
            if (dash_top() == SCR_REMAP || top_before == SCR_REMAP) {
                g_trans = 1;    /* the remap overlay: no swing */
            }
            /* the new screen this frame: drawn again next frame into its layer */
        }
        if (g_trans < 1) {
            const float k = ease(g_trans), a = 1.2f * g_trans_dir;
            ui_layer_draw3d(g_prev, -a * k, 0.35f * k, 1 - k);
            ui_layer_draw3d(g_layer, a * (1 - k), 0.35f * (1 - k), k);
            g_trans += 1.0f / (60 * 0.38f);
        } else {
            ui_layer_draw3d(g_layer, 0, 0, 1);
        }
        if (D.ask) {
            if (asked) {
                D.in = saved;
            } else {
                memset(&D.in, 0, sizeof(D.in));
                D.in.mx = D.in.my = -1;
                D.in.pad = saved.pad;
            }
            draw_ask();
        }
        if (scanning && dash_top() != SCR_REMAP) {
            draw_scan_badge();
        }
        if (D.toast[0] && ui_time() < D.toast_until) {
            draw_toast();
        }
        if (go_pic || go_back) {
            ui_layer_end();
            if (go_pic) {
                ui_layer_draw3d(g_go.pic, 0, 0, 1);
                g_go.t0 = ui_time();
            } else {
                ui_fx_flash_in(g_go.pic, (float)((ui_time() - g_go.back) / GO_BACK), UI_BLACK);
            }
        }
        auto_shot();
        ui_end();
        D.anim += 1.0f / 60;
        /* xbased.ini changed by hand while xbased runs (a folder added in a
         * text editor): read again, or leaving would write the old one over it */
        static double ini_checked;
        if (ui_time() - ini_checked > 1.0) {
            ini_checked = ui_time();
            if (cfg_changed_outside(&D.global)) {
                cfg_load(&D.global, D.global.path);
                i18n_set(cfg_get(&D.global, "ui.lang"));
            }
        }
                /* the folders changed (one added, removed or swapped for another): scan again */
        static unsigned last_sig;
        folders(&fv, &fn);
        unsigned sig = 2166136261u;
        for (int i = 0; i < fn; i++) {
            for (const char *s = fv[i]; *s; s++) {
                sig = (sig ^ (unsigned char)*s) * 16777619u;
            }
            sig = (sig ^ 0xFF) * 16777619u;
        }
        if (last_sig && sig != last_sig) {
            lib_scan(fv, fn);
        }
        last_sig = sig;
    }
    dash_save_cfg();
    lib_shutdown();
    snd_shutdown();
    ui_shutdown();
    return 0;
}

/* ------------------------------------------------------------ the in-game menu */

UiTex *load_bmp(const char *path);
void states_forget(void);

static int g_ig_result, g_ig_slot;
static bool g_ig_saving;            /* the slots screen: saving (else loading) */
static bool g_ig_thumbs_stale;   /* the slots' pictures to read again */
static bool g_ig_done;

static void scr_ingame(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(D.cur->g.title, NULL);
    List l;
    const float lw = 520;
    list_begin(&l, (W - lw) / 2, 140, lw, H - 236, 56);
    if (list_row(&l, T(S_RESUME), NULL, NULL)) {
        g_ig_result = DASH_INGAME_RESUME;
        g_ig_done = true;
    }
    if (list_row(&l, T(S_SAVE_STATE), NULL, NULL)) {
        g_ig_saving = true;
        D.sel[SCR_INGAME_SLOTS] = 0;
        states_forget();
        g_ig_thumbs_stale = true;
        dash_push(SCR_INGAME_SLOTS);
    }
    if (list_row(&l, T(S_LOAD_STATE), NULL, NULL)) {
        g_ig_saving = false;
        D.sel[SCR_INGAME_SLOTS] = 0;
        states_forget();
        g_ig_thumbs_stale = true;
        dash_push(SCR_INGAME_SLOTS);
    }
    if (list_row(&l, T(S_GAME_SETTINGS), NULL, NULL)) {
        D.editing_game = true;
        D.sel[SCR_GAME_SETTINGS] = 0;
        dash_push(SCR_GAME_SETTINGS);
    }
    if (list_row(&l, T(S_CONTROLS), NULL, NULL)) {
        D.editing_game = false;
        D.player = 0;
        dash_push(SCR_CONTROLS);
    }
    if (list_row(&l, T(S_SETTINGS), NULL, NULL)) {
        D.tab = 0;
        dash_push(SCR_SETTINGS);
    }
    if (list_row(&l, T(S_RESTART), NULL, NULL)) {
        g_ig_result = DASH_INGAME_RESTART;
        g_ig_done = true;
    }
    if (list_row(&l, T(S_QUIT_GAME), NULL, NULL)) {
        g_ig_result = DASH_INGAME_QUIT;
        g_ig_done = true;
    }
    list_end(&l);
    if (D.in.back || D.in.start) {
        g_ig_result = DASH_INGAME_RESUME;
        g_ig_done = true;
    }
    draw_hints(T(S_SELECT), T(S_RESUME), NULL, NULL);
}

static UiTex *g_ig_thumb[11];

static void scr_ingame_slots(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(g_ig_saving ? T(S_SAVE_STATE) : T(S_LOAD_STATE), D.cur->g.title);
    char dir[1100];
    states_dir(D.cur, dir, sizeof(dir));
    if (g_ig_thumbs_stale) {    /* the pictures read again each time the list opens */
        g_ig_thumbs_stale = false;
        for (int i = 0; i < 11; i++) {
            if (g_ig_thumb[i]) {
                ui_texture_free(g_ig_thumb[i]);
                g_ig_thumb[i] = NULL;
            }
            char p[1200];
            snprintf(p, sizeof(p), "%s/slot%d.bmp", dir, i);
            g_ig_thumb[i] = load_bmp(p);
        }
    }
    List l;
    int sel = 0;
    list_begin(&l, 74, 150, W * 0.5f - 74, H - 244, 64);
    for (int s = 1; s <= 10; s++) {
        char p[1200], label[64], when[64] = "";
        snprintf(p, sizeof(p), "%s/slot%d.hxs", dir, s);
        SDL_PathInfo pi;
        const bool has = SDL_GetPathInfo(p, &pi);
        if (has) {
            SDL_DateTime dt;
            SDL_TimeToDateTime(pi.modify_time, &dt, true);
            snprintf(when, sizeof(when), "%04d-%02d-%02d %s", dt.year, dt.month, dt.day, dash_hm(dt.hour, dt.minute));
        }
        snprintf(label, sizeof(label), "%s %d", T(S_SLOT), s);
        if (list_row_icon(&l, g_ig_thumb[s], label, has ? when : T(S_EMPTY), NULL, NULL) && (has || g_ig_saving)) {
            g_ig_result = g_ig_saving ? DASH_INGAME_SAVE : DASH_INGAME_LOAD;
            g_ig_slot = s;
            g_ig_done = true;
        }
        if (list_selected(&l)) {
            sel = s;
        }
    }
    list_end(&l);
    /* the selected slot's picture, large (outside the list's clip) */
    if (sel) {
        const float px = W * 0.5f + 30, pw = W * 0.5f - 104, ph = pw * 3 / 4;
        ui_round(px - 8, 142, pw + 16, ph + 16, 12, UI_PANEL);
        if (g_ig_thumb[sel]) {
            ui_image(g_ig_thumb[sel], px, 150, pw, ph, 1);
        }
    }
    if (D.in.back) {
        dash_pop();
    }
    draw_hints(g_ig_saving ? T(S_SAVE_STATE) : T(S_LOAD_STATE), T(S_BACK), NULL, NULL);
}

int dash_ingame(SDL_Window *win, const char *key, const char *title, const char *frame_bmp, int *slot,
                void (*live)(const struct Config *global, const struct Config *game))
{
    memset(&D, 0, sizeof(D));
    g_game_running = true;
    g_win = win;
    SDL_HideCursor();   /* it shows when the mouse moves (poll_input) */
    g_cursor_at = 0;
    char p[1200];
    snprintf(p, sizeof(p), "%sxbased.ini", cfg_data_dir());
    cfg_load(&D.global, p);
    if (upscale_migrate(&D.global)) {
        cfg_save(&D.global);
    }
    i18n_set(cfg_get(&D.global, "ui.lang"));
    static LibGame game;
    memset(&game, 0, sizeof(game));
    snprintf(game.g.key, sizeof(game.g.key), "%s", key);
    xd_clean_name(title, game.g.title, sizeof(game.g.title));
    D.cur = &game;
    game_cfg_path(&game, p, sizeof(p));
    cfg_load(&D.game, p);
    upscale_migrate(&D.game);
    D.stack[0] = SCR_INGAME;
    D.sel[SCR_INGAME] = 0;  /* on Resume each time, as a console's: no Quit by habit */
    g_auto_off = -1;
    /* the game's overlay (its presenter draws the menu over its picture),
     * or a renderer of this menu's own on the window */
    const bool ov = ui_is_overlay();
    if (!ov) {
        char fonts[1200];
        snprintf(fonts, sizeof(fonts), "%sfonts/", cfg_exe_dir());
        if (!ui_init(win, fonts)) {
            return DASH_INGAME_RESUME;
        }
    }
    snd_init();
    snd_enable(cfg_bool(&D.global, "ui.sounds", true));
    UiTex *frame = ov ? NULL : load_bmp(frame_bmp);
    g_ig_done = false;
    g_ig_result = DASH_INGAME_RESUME;
    snd_play(SND_TAB);
    /* over the overlay: the game's picture blurs and the menu fades in (a
     * fifth of a second), and the other way out, back to the game */
    const double t_open = ui_time();
    double t_close = 0;
    for (;;) {
        if ((g_ig_done || D.quit) && !t_close) {
            const bool away = D.quit || g_ig_result == DASH_INGAME_QUIT || g_ig_result == DASH_INGAME_RESTART;
            if (!ov || away) {
                break;  /* the game breaks away itself (win_main.c) */
            }
            t_close = ui_time();
        }
        float k = (float)((ui_time() - t_open) / 0.2);
        if (t_close) {
            k = 1.0f - (float)((ui_time() - t_close) / 0.16);
            if (k <= 0) {
                break;
            }
        }
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        const float ek = ease(k);
        poll_input();
        auto_poll();
        if (t_close) {
            memset(&D.in, 0, sizeof(D.in));     /* closing: nothing more chosen */
            D.in.mx = D.in.my = -1;
        }
        const In saved = D.in;
        const bool asked = D.ask != NULL;  /* a question asked this frame answers from the next */
        if (D.ask) {
            memset(&D.in, 0, sizeof(D.in));
            D.in.mx = D.in.my = -1;
            D.in.pad = saved.pad;
        }
        if (!D.ask && dash_top() != SCR_REMAP) {
            if (saved.ok || saved.click) {
                snd_play(SND_SELECT);
            } else if (saved.back) {
                snd_play(SND_BACK);
            } else if (saved.up || saved.down || saved.left || saved.right) {
                snd_play(SND_MOVE);
            }
        }
        ui_begin();
        /* the paused game under a green veil: blurred, over the overlay */
        if (ov) {
            ui_cover(true);
            ui_fill(ui_scene(false), UI_WHITE);
            ui_fill(ui_scene(true), ui_alpha(UI_WHITE, ek));
            ui_set_alpha(ek);
        } else if (frame) {
            ui_fill(frame, UI_WHITE);
        }
        ui_rect_grad(0, 0, ui_w(), ui_h(), (UiColor){0, 12, 0, ov ? 150 : 200}, (UiColor){0, 24, 0, ov ? 190 : 225});
        ui_glow(ui_w() / 2, ui_h() / 2, ui_h() * 0.8f, ui_alpha(UI_GREEN, 0.10f));
        switch (dash_top()) {
        case SCR_INGAME: scr_ingame(); break;
        case SCR_INGAME_SLOTS: scr_ingame_slots(); break;
        case SCR_GAME_SETTINGS: scr_game_settings(); break;
        case SCR_CONTROLS: scr_controls(); break;
        case SCR_REMAP: scr_remap(); break;
        case SCR_STICKS: scr_sticks(); break;
        case SCR_TRIGGERS: scr_triggers(); break;
        case SCR_SETTINGS: scr_settings(); break;
        default: dash_pop(); break;
        }
        if (D.ask) {
            if (asked) {
                D.in = saved;
            } else {
                memset(&D.in, 0, sizeof(D.in));
                D.in.mx = D.in.my = -1;
                D.in.pad = saved.pad;
            }
            draw_ask();
        }
        ui_set_alpha(1);
        if (live) {
            live(&D.global, &D.game);   /* the picture's shape and filters under the menu, as chosen */
        }
        auto_shot();
        ui_end();
        D.anim += 1.0f / 60;
    }
    dash_save_cfg();
    for (int i = 0; i < 11; i++) {
        if (g_ig_thumb[i]) {
            ui_texture_free(g_ig_thumb[i]);
            g_ig_thumb[i] = NULL;
        }
    }
    ui_texture_free(frame);
    states_forget();
    snd_shutdown();
    if (!ov) {
        ui_shutdown();
    }
    *slot = g_ig_slot;
    return D.quit ? DASH_INGAME_QUIT : g_ig_result;
}
