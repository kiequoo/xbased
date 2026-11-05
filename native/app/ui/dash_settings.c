/*
 * hakuX front end — the dashboard's settings: the global ones (tabs), a
 * game's, its mods and cheats, its save states, the controls and their
 * remapping, online play.
 */
#include "native/app/ui/dash_int.h"
#include "native/app/ui/input_map.h"
#include "native/core/xcheat.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ shared rows */

static void rows_video(List *l)
{
    static const char *const sv[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    static const char *const sn[] = {"1x  (640x480)",   "2x  (1280x960)",  "3x  (1920x1440)", "4x  (2560x1920)",
                                     "5x  (3200x2400)", "6x  (3840x2880)", "7x  (4480x3360)", "8x  (5120x3840)"};
    opt_choice(l, S_RENDER_SCALE, "scale", sv, sn, 8, "1");
    static const char *av[] = {"auto", "4:3", "16:9", "stretch"};
    const char *an[] = {T(S_AUTO), "4:3", "16:9", T(S_STRETCH)};
    opt_choice(l, S_ASPECT, "aspect", av, an, 4, "auto");
    opt_bool(l, S_FXAA, "fxaa", false, false);
    static const char *anv[] = {"0", "2", "4", "8", "16"};
    const char *ann[] = {T(S_OFF), "2x", "4x", "8x", "16x"};
    opt_choice(l, S_ANISO, "aniso", anv, ann, 5, "0");
    static const char *uv[] = {"bilinear", "fsr"};
    const char *un[] = {T(S_BILINEAR), "AMD FSR 1"};
    opt_choice(l, S_UPSCALE, "upscale", uv, un, 2, "bilinear");
    static const char *cv[] = {"0", "1", "2", "3"};
    const char *cn[] = {T(S_OFF), T(S_CRT_SOFT), T(S_CRT_TV), T(S_CRT_ARCADE)};
    opt_choice(l, S_CRT, "crt", cv, cn, 4, "0");
}

/* PAL games at 60 Hz, as a PAL console with PAL-60 on runs them, or 50 Hz */
static void rows_pal(List *l)
{
    static const char *const pv[] = {"1", "0"};
    static const char *const pn[] = {"60 Hz", "50 Hz"};
    opt_choice(l, S_PAL_GAMES, "pal60", pv, pn, 2, "1");
}

/* the console's memory: 64 MiB, or a debug kit's 128 for the betas and
 * debug builds that need it (titles marked for 64 MiB keep 64: core.c) */
static void rows_ram(List *l)
{
    static const char *const rv[] = {"64", "128"};
    const char *rn[] = {T(S_RAM_64), T(S_RAM_128)};
    opt_choice(l, S_RAM_SIZE, "ram", rv, rn, 2, "64");
}

static void rows_console(List *l)
{
    static const char *lv[] = {"auto", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    const char *ln[] = {T(S_AUTO), "English", "日本語", "Deutsch", "Français", "Español", "Italiano", "한국어", "中文",
                        "Português"};
    opt_choice(l, S_CONSOLE_LANG, "lang", lv, ln, 10, "auto");
    opt_videoflag(l, S_WIDESCREEN, 0x10000);
}

/* ------------------------------------------------------------ settings */

static void tab_bar(const enum UiStr *tabs, int n)
{
    const float W = ui_w();
    float x = 60;
    for (int i = 0; i < n; i++) {
        const char *t = T(tabs[i]);
        const float w = ui_text_width(t, 24, UI_FONT_BOLD) + 44;
        const bool s = D.tab == i;
        const bool over = D.in.mx >= x && D.in.mx < x + w && D.in.my >= 124 && D.in.my < 168;
        if (over && D.in.click) {
            D.tab = i;
            D.sel[dash_top()] = 0;
        }
        ui_round(x, 124, w, 44, 22, s ? (UiColor){0x3C, 0x9A, 0x12, 0xFF} : (UiColor){0x10, 0x2C, 0x07, 0xE0});
        ui_round_outline(x, 124, w, 44, 22, s ? 2 : 1, s ? UI_GREEN_HI : UI_GREEN_DIM);
        ui_text(x + w / 2, 132, t, 24, UI_FONT_BOLD, s ? UI_WHITE : UI_GREY, UI_ALIGN_CENTER);
        x += w + 12;
    }
    if (x < W - 200) {
        draw_button_glyph(x + 8, 132, 0, 0); /* nothing: keeps the layout simple */
    }
    if (D.in.lb) {
        D.tab = (D.tab + n - 1) % n;
        D.sel[dash_top()] = 0;
    }
    if (D.in.rb) {
        D.tab = (D.tab + 1) % n;
        D.sel[dash_top()] = 0;
    }
}

/* SDL may call it on a thread of its own: the folder waits for the UI's thread */
static char g_new_folder[1024];
static SDL_AtomicInt g_new_folder_set;

void add_folder_cb(void *ud, const char *const *files, int filter)
{
    (void)ud;
    (void)filter;
    if (files && files[0] && !SDL_GetAtomicInt(&g_new_folder_set)) {
        snprintf(g_new_folder, sizeof(g_new_folder), "%s", files[0]);
        SDL_SetAtomicInt(&g_new_folder_set, 1);
    }
}

/* a game file: any name (what the folders' scan leaves out: another
 * extension, a folder deeper than it looks) */
static char g_new_file[1024];
static SDL_AtomicInt g_new_file_set;

static void SDLCALL add_file_cb(void *ud, const char *const *files, int filter)
{
    (void)ud;
    (void)filter;
    if (files && files[0] && !SDL_GetAtomicInt(&g_new_file_set)) {
        snprintf(g_new_file, sizeof(g_new_file), "%s", files[0]);
        SDL_SetAtomicInt(&g_new_file_set, 1);
    }
}

void dash_pick_game_file(void)
{
    static const SDL_DialogFileFilter f[] = {
        {"Xbox disc images (*.iso, *.xiso, *.cci, *.chd)", "iso;xiso;cci;chd"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(add_file_cb, NULL, SDL_GetKeyboardFocus(), f, 2, NULL, false);
}

/* a game file played once, not added to the library (the dialog's, or one
 * dropped on the window): started from the UI's thread (dash_take_folder) */
static char g_play_file[1024];
static SDL_AtomicInt g_play_file_set;

void dash_play_file_later(const char *path)
{
    if (path && path[0] && !SDL_GetAtomicInt(&g_play_file_set)) {
        snprintf(g_play_file, sizeof(g_play_file), "%s", path);
        SDL_SetAtomicInt(&g_play_file_set, 1);
    }
}

static void SDLCALL play_file_cb(void *ud, const char *const *files, int filter)
{
    (void)ud;
    (void)filter;
    if (files && files[0]) {
        dash_play_file_later(files[0]);
    }
}

void dash_pick_play_file(void)
{
    static const SDL_DialogFileFilter f[] = {
        {"Xbox disc images (*.iso, *.xiso, *.cci, *.chd)", "iso;xiso;cci;chd"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(play_file_cb, NULL, SDL_GetKeyboardFocus(), f, 2, NULL, false);
}

void dash_take_folder(void)
{
    if (SDL_GetAtomicInt(&g_play_file_set)) {
        static LibGame once;
        memset(&once, 0, sizeof(once));
        if (xd_read_game(g_play_file, &once.g)) {
            xd_free_game(&once.g);
            launch_game(&once, NULL);
        } else {
            xd_free_game(&once.g);
            dash_toast(T(S_NOT_A_GAME));
        }
        SDL_SetAtomicInt(&g_play_file_set, 0);
    }
    if (SDL_GetAtomicInt(&g_new_folder_set)) {
        cfg_add(&D.global, "gamedir", g_new_folder);
        cfg_save(&D.global);
        SDL_SetAtomicInt(&g_new_folder_set, 0);
    }
    if (SDL_GetAtomicInt(&g_new_file_set)) {
        const char *have[32];
        const int n = cfg_all(&D.global, "gamefile", have, 32);
        bool dup = false;
        for (int i = 0; i < n; i++) {
            dup |= !strcmp(have[i], g_new_file);
        }
        if (!dup) {
            cfg_add(&D.global, "gamefile", g_new_file);
            cfg_save(&D.global);
        }
        SDL_SetAtomicInt(&g_new_file_set, 0);
    }
}

static const char *video_mode_name(void)
{
    return "";
}

void scr_settings(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(T(S_SETTINGS), NULL);
    static const enum UiStr tabs[] = {S_VIDEO, S_AUDIO, S_SYSTEM, S_INTERFACE, S_CONTROLS, S_HOTKEYS};
    D.editing_game = false;
    tab_bar(tabs, 6);
    List l;
    list_begin(&l, 74, 196, W - 148, H - 290, 58);
    (void)video_mode_name;
    switch (D.tab) {
    case 0:
        rows_video(&l);
        opt_bool(&l, S_FULLSCREEN, "fullscreen", true, true);
        opt_bool(&l, S_SHOW_FPS, "showfps", false, false);
        break;
    case 1: {
        /* the console's audio setting and the speakers it plays on: stereo,
         * or 5.1 (the Xbox's Dolby Digital: the games' six channels) */
        static const char *av[] = {"stereo", "5.1"};
        const char *an[] = {T(S_STEREO), T(S_SURROUND51)};
        opt_choice(&l, S_AUDIO_OUTPUT, "audio", av, an, 2, "stereo");
        opt_int(&l, S_VOLUME, "volume", 0, 100, 5, 100, "%");
        opt_bool(&l, S_MUTE_BG, "mutebg", true, false);
        opt_bool(&l, S_UI_SOUNDS, "ui.sounds", true, false);
        break;
    }
    case 2: {
        static const char *lv[] = {"auto", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
        const char *ln[] = {T(S_AUTO), "English", "日本語", "Deutsch", "Français", "Español", "Italiano", "한국어", "中文",
                            "Português"};
        opt_choice(&l, S_CONSOLE_LANG, "lang", lv, ln, 10, "auto");
        opt_videoflag(&l, S_WIDESCREEN, 0x10000);
        opt_videoflag(&l, S_P480, 0x80000);
        opt_videoflag(&l, S_P720, 0x20000);
        opt_videoflag(&l, S_I1080, 0x40000);
        rows_pal(&l);
        rows_ram(&l);
        static const char *pv[] = {"hdtv", "standard", "svideo", "scart", "vga"};
        static const char *pn[] = {"HDTV (component)", "Standard (composite)", "S-Video", "SCART (RGB)", "VGA"};
        opt_choice(&l, S_AVPACK, "avpack", pv, pn, 5, "hdtv");
        opt_bool(&l, S_ASYNC_SHADERS, "asyncpipe", true, false);
        opt_bool(&l, S_PREBUILD, "prebuild", true, false);
        break;
    }
    case 3: {
        /* the interface's language: "auto" (the system's) or one */
        static const char *uv[L_COUNT + 1];
        static const char *un[L_COUNT + 1];
        uv[0] = "auto";
        un[0] = T(S_AUTO);
        for (int i = 0; i < L_COUNT; i++) {
            uv[i + 1] = i18n_code((enum UiLang)i);
            un[i + 1] = i18n_name((enum UiLang)i);
        }
        const char *before = cfg_get(&D.global, "ui.lang");
        char was[16];
        snprintf(was, sizeof(was), "%s", before ? before : "auto");
        opt_choice(&l, S_UI_LANG, "ui.lang", uv, un, L_COUNT + 1, "auto");
        const char *now = cfg_get(&D.global, "ui.lang");
        if (now && strcmp(now, was)) {
            i18n_set(now);
        }
        opt_bool(&l, S_COVERS_ONLINE, "covers", true, false);
        opt_bool(&l, S_ANIMATED_BG, "ui.anim", true, false);
        opt_bool(&l, S_UI_SOUNDS, "ui.sounds", true, false);
        opt_bool(&l, S_CHECK_UPDATES, "update.check", true, false);
        opt_bool(&l, S_SWAP_AB, "ui.swapab", false, false);
        opt_bool(&l, S_SHOW_HIDDEN, "ui.showhidden", false, false);
        opt_bool(&l, S_CLOCK_12H, "ui.clock12", false, false);
        const char *dirs[16];
        const int nd = cfg_all(&D.global, "gamedir", dirs, 16);
        for (int i = 0; i < nd; i++) {
            char label[64];
            snprintf(label, sizeof(label), "%s %d", T(S_GAME_FOLDERS), i + 1);
            if (list_row(&l, label, dirs[i], NULL) || (list_selected(&l) && D.in.x)) {
                char d[512];
                snprintf(d, sizeof(d), "%s", dirs[i]);
                cfg_remove_value(&D.global, "gamedir", d);
                cfg_save(&D.global);
                break;
            }
        }
        if (list_row(&l, T(S_ADD_FOLDER), "+", NULL)) {
            SDL_ShowOpenFolderDialog(add_folder_cb, NULL, SDL_GetKeyboardFocus(), NULL, false);
        }
        const char *files[32];
        const int nf = cfg_all(&D.global, "gamefile", files, 32);
        for (int i = 0; i < nf; i++) {
            char label[64];
            snprintf(label, sizeof(label), "%s %d", T(S_GAME_FILE), i + 1);
            if (list_row(&l, label, files[i], NULL) || (list_selected(&l) && D.in.x)) {
                char f[1024];
                snprintf(f, sizeof(f), "%s", files[i]);
                cfg_remove_value(&D.global, "gamefile", f);
                cfg_save(&D.global);
                break;
            }
        }
        if (list_row(&l, T(S_ADD_GAME_FILE), "+", NULL)) {
            dash_pick_game_file();
        }
        if (list_row(&l, T(S_PLAY_GAME_FILE), ">", NULL)) {
            dash_pick_play_file();
        }
        break;
    }
    case 4:
        for (int p = 0; p < 4; p++) {
            char label[48], val[128];
            snprintf(label, sizeof(label), "%s %d", T(S_PLAYER), p + 1);
            im_device_name(&D.global, p, val, sizeof(val));
            int delta = 0;
            if (list_row(&l, label, val, &delta)) {
                D.player = p;
                dash_push(SCR_CONTROLS);
            }
            if (delta) {
                im_cycle_device(&D.global, p, delta);
            }
        }
        opt_int(&l, S_DEADZONE, "deadzone", 0, 50, 5, 10, "%");
        {
            /* the sticks' range and shape, set on their own screen while
             * they're watched; left and right step the range here too */
            const char *shape = cfg_get(&D.global, "stick.shape");
            const bool round = shape && !strcmp(shape, "round");
            const int range = cfg_int(&D.global, "stick.range", 100);
            char val[96];
            snprintf(val, sizeof(val), "%d%%  ·  %s", range, T(round ? S_SHAPE_ROUND : S_SHAPE_SQUARE));
            int delta = 0;
            if (list_row(&l, T(S_STICK_RANGE), val, &delta)) {
                dash_push(SCR_STICKS);
            }
            if (delta) {
                const int r = range + delta * 5;
                cfg_set_int(&D.global, "stick.range", r < 50 ? 50 : r > 150 ? 150 : r);
            }
        }
        {
            /* the triggers' dead zone and full press, set on their own screen */
            char val[64];
            snprintf(val, sizeof(val), "%d%%  ·  %d%%", cfg_int(&D.global, "trigger.deadzone", 10),
                     cfg_int(&D.global, "trigger.full", 100));
            if (list_row(&l, T(S_TRIGGERS), val, NULL)) {
                dash_push(SCR_TRIGGERS);
            }
        }
        opt_bool(&l, S_RUMBLE, "rumble", true, false);
        break;
    case 5:
        for (int h = 0; h < IM_HOTKEYS; h++) {
            char val[64];
            im_hotkey_name(&D.global, h, val, sizeof(val));
            if (list_row(&l, T(im_hotkey_label(h)), val, NULL)) {
                D.remap_target = 1000 + h;
                D.remap_since = ui_time();
                dash_push(SCR_REMAP);
            }
        }
        break;
    }
    list_end(&l);
    if (D.in.back) {
        dash_save_cfg();
        dash_pop();
    }
    draw_hints(T(S_CHANGE), T(S_BACK), D.tab == 3 ? T(S_REMOVE) : NULL, NULL);
}

/* ------------------------------------------------------------ a game's */

static void reset_game(Dash *d)
{
    /* everything but its mods and cheats */
    const char *keep[64];
    char kept[64][464], keys[64][48];
    int nk = 0;
    for (int i = 0; i < d->game.n && nk < 64; i++) {
        if (!strcmp(d->game.key[i], "mod") || !strcmp(d->game.key[i], "cheat")) {
            snprintf(keys[nk], sizeof(keys[nk]), "%s", d->game.key[i]);
            snprintf(kept[nk], sizeof(kept[nk]), "%s", d->game.val[i]);
            nk++;
        }
    }
    (void)keep;
    char path[1024];
    snprintf(path, sizeof(path), "%s", d->game.path);
    memset(&d->game, 0, sizeof(d->game));
    snprintf(d->game.path, sizeof(d->game.path), "%s", path);
    for (int i = 0; i < nk; i++) {
        cfg_add(&d->game, keys[i], kept[i]);
    }
    dash_save_cfg();
}

void scr_game_settings(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(T(S_GAME_SETTINGS), D.cur->g.title);
    D.editing_game = true;
    List l;
    list_begin(&l, 74, 150, W - 148, H - 244, 58);
    rows_video(&l);
    static const char *lv[] = {"auto", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    const char *ln[] = {T(S_AUTO), "English", "日本語", "Deutsch", "Français", "Español", "Italiano", "한국어", "中文",
                        "Português"};
    opt_choice(&l, S_CONSOLE_LANG, "lang", lv, ln, 10, "auto");
    opt_videoflag(&l, S_WIDESCREEN, 0x10000);
    rows_pal(&l);
    rows_ram(&l);
    opt_bool(&l, S_ASYNC_SHADERS, "asyncpipe", true, false);
    if (list_row(&l, T(S_RESET_GAME), NULL, NULL)) {
        dash_ask(T(S_RESET_GAME), reset_game);
    }
    list_end(&l);
    ui_text(74, H - 82, T(S_NEEDS_RESTART), 18, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT);
    if (D.in.back) {
        dash_pop();
    }
    draw_hints(T(S_CHANGE), T(S_BACK), NULL, NULL);
    (void)rows_console;
}

/* ------------------------------------------------------------ mods and cheats */

typedef struct {
    char id[64], name[128], desc[512];
    bool cheat;
} ModItem;

static ModItem g_mods[96];
static int g_nmods;
static char g_mods_key[24];

/* The Xbox scene's cheat files for the title (cheats/<TITLE ID>/*.etm,
 * .xbtf: core/xcheat.h), an entry an option, kept in the game's settings
 * as xcheat=cheats/<TITLE ID>/<file>|<its options' states, 1 on>. */
typedef struct {
    char file[160];                 /* cheats/<TITLE ID>/<file> */
    char from[88];                  /* the file's own title */
    char label[88];
    int idx, nopts;
} XOpt;

static XOpt g_xopts[256];
static int g_nxopts;

static void xcheats_scan(const char *root)
{
    char dir[1400];
    snprintf(dir, sizeof(dir), "%scheats/%08X", root, D.cur->g.title_id);
    int n = 0;
    char **names = SDL_GlobDirectory(dir, NULL, 0, &n);
    for (int i = 0; names && i < n; i++) {
        char rel[160], full[1600];
        snprintf(rel, sizeof(rel), "cheats/%08X/%s", D.cur->g.title_id, names[i]);
        bool dup = false;
        for (int k = 0; k < g_nxopts; k++) {
            dup |= !SDL_strcasecmp(g_xopts[k].file, rel);
        }
        snprintf(full, sizeof(full), "%s/%s", dir, names[i]);
        NcXCheat x;
        if (dup || !nc_xcheat_load(full, &x)) {
            continue;
        }
        if (nc_xcheat_for_title(&x, D.cur->g.title_id)) {
            for (int o = 0; o < x.nopts && g_nxopts < 256; o++) {
                XOpt *e = &g_xopts[g_nxopts++];
                snprintf(e->file, sizeof(e->file), "%s", rel);
                snprintf(e->from, sizeof(e->from), "%s", x.name);
                snprintf(e->label, sizeof(e->label), "%s", x.labels[o]);
                e->idx = o;
                e->nopts = x.nopts;
            }
        }
        nc_xcheat_free(&x);
    }
    SDL_free(names);
}

static void xcheats_load(void)
{
    g_nxopts = 0;
    xcheats_scan(cfg_data_dir());           /* the user's first */
    if (strcmp(cfg_data_dir(), cfg_exe_dir())) {
        xcheats_scan(cfg_exe_dir());
    }
}

/* the game's states for a cheat file ("" if none) */
static const char *xcheat_states(const char *file)
{
    const char *v[64];
    const int n = cfg_all(&D.game, "xcheat", v, 64);
    const size_t fl = strlen(file);
    for (int i = 0; i < n; i++) {
        if (!strncmp(v[i], file, fl) && v[i][fl] == '|') {
            return v[i] + fl + 1;
        }
    }
    return "";
}

static void xcheat_toggle(const XOpt *e)
{
    char states[NC_XCHEAT_MAX_OPTS + 1], old[NC_XCHEAT_MAX_OPTS + 1];
    snprintf(old, sizeof(old), "%s", xcheat_states(e->file));
    bool any = false;
    for (int i = 0; i < e->nopts; i++) {
        char c = i < (int)strlen(old) && old[i] == '1' ? '1' : '0';
        if (i == e->idx) {
            c = c == '1' ? '0' : '1';
        }
        states[i] = c;
        any |= c == '1';
    }
    states[e->nopts] = 0;
    if (old[0]) {
        char was[300];
        snprintf(was, sizeof(was), "%s|%s", e->file, old);
        cfg_remove_value(&D.game, "xcheat", was);
    }
    if (any) {
        char now[300];
        snprintf(now, sizeof(now), "%s|%s", e->file, states);
        cfg_add(&D.game, "xcheat", now);
    }
}

static void mods_load(void)
{
    if (!strcmp(g_mods_key, D.cur->g.key)) {
        return;
    }
    snprintf(g_mods_key, sizeof(g_mods_key), "%s", D.cur->g.key);
    g_nmods = 0;
    /* as the run finds them (win_mods.c): the shipped mods, then the user's
     * (the data folder's mods/); a release's folder, then the title's */
    char dirs[4][1200];
    snprintf(dirs[0], sizeof(dirs[0]), "%smods/%s", cfg_exe_dir(), D.cur->g.key);
    snprintf(dirs[1], sizeof(dirs[1]), "%smods/%.8s", cfg_exe_dir(), D.cur->g.key);
    snprintf(dirs[2], sizeof(dirs[2]), "%smods/%s", cfg_data_dir(), D.cur->g.key);
    snprintf(dirs[3], sizeof(dirs[3]), "%smods/%.8s", cfg_data_dir(), D.cur->g.key);
    const char *dir = dirs[0];
    const int ndirs = strcmp(cfg_data_dir(), cfg_exe_dir()) ? 4 : 2;
    for (int di = 0; di < ndirs; di++) {
    int n = 0;
    char **names = SDL_GlobDirectory(dirs[di], NULL, 0, &n);
    for (int i = 0; names && i < n && g_nmods < 96; i++) {
        char p[1400];
        const size_t nl = strlen(names[i]);
        const bool ips = nl > 4 && !SDL_strcasecmp(names[i] + nl - 4, ".ips");
        char id[64];
        snprintf(id, sizeof(id), "%.*s", (int)(ips ? nl - 4 : nl), names[i]);
        bool dup = false;
        for (int k = 0; k < g_nmods; k++) {
            dup |= !SDL_strcasecmp(g_mods[k].id, id);
        }
        if (dup) {
            continue;
        }
        if (ips) {
            /* an IPS patch: its name the file's, underscores as spaces */
            ModItem *m = &g_mods[g_nmods++];
            memset(m, 0, sizeof(*m));
            snprintf(m->id, sizeof(m->id), "%s", id);
            snprintf(m->name, sizeof(m->name), "%s", id);
            for (char *c = m->name; *c; c++) {
                *c = *c == '_' ? ' ' : *c;
            }
            snprintf(m->desc, sizeof(m->desc), "%s", T(S_IPS_PATCH));
            continue;
        }
        snprintf(p, sizeof(p), "%s/%s/mod.txt", dirs[di], names[i]);
        size_t len = 0;
        char *t = SDL_LoadFile(p, &len);
        if (!t) {
            continue;
        }
        ModItem *m = &g_mods[g_nmods++];
        memset(m, 0, sizeof(*m));
        snprintf(m->id, sizeof(m->id), "%s", names[i]);
        snprintf(m->name, sizeof(m->name), "%s", names[i]);
        for (char *s = t; s && *s;) {
            char *e = strchr(s, '\n');
            if (e) {
                *e = 0;
            }
            if (!strncmp(s, "name:", 5)) {
                snprintf(m->name, sizeof(m->name), "%s", s + 5 + strspn(s + 5, " "));
            } else if (!strncmp(s, "description:", 12)) {
                snprintf(m->desc, sizeof(m->desc), "%s", s + 12 + strspn(s + 12, " "));
            }
            s = e ? e + 1 : NULL;
        }
        m->name[strcspn(m->name, "\r")] = 0;
        m->desc[strcspn(m->desc, "\r")] = 0;
        SDL_free(t);
    }
    SDL_free(names);
    }
    /* cheats.txt: its [names] */
    char p[1400];
    snprintf(p, sizeof(p), "%s/cheats.txt", dir);
    size_t len = 0;
    char *t = SDL_LoadFile(p, &len);
    for (char *s = t; s && *s && g_nmods < 96;) {
        char *e = strchr(s, '\n');
        if (e) {
            *e = 0;
        }
        s[strcspn(s, "\r")] = 0;
        if (s[0] == '[' && strchr(s, ']')) {
            ModItem *m = &g_mods[g_nmods++];
            memset(m, 0, sizeof(*m));
            *strchr(s, ']') = 0;
            snprintf(m->id, sizeof(m->id), "%s", s + 1);
            snprintf(m->name, sizeof(m->name), "%s", s + 1);
            m->cheat = true;
        } else if (!strncmp(s, "note:", 5) && g_nmods && g_mods[g_nmods - 1].cheat) {
            snprintf(g_mods[g_nmods - 1].desc, sizeof(g_mods[0].desc), "%s", s + 5 + strspn(s + 5, " "));
        }
        s = e ? e + 1 : NULL;
    }
    SDL_free(t);
    /* the widescreen database's entry for the title (win_mods.c) */
    snprintf(p, sizeof(p), "%smods/widescreen.dat", cfg_exe_dir());
    t = SDL_LoadFile(p, &len);
    char want[16];
    snprintf(want, sizeof(want), "\n:%08x", D.cur->g.title_id);
    if (t && g_nmods < 96 && SDL_strcasestr(t, want)) {
        ModItem *m = &g_mods[g_nmods++];
        memset(m, 0, sizeof(*m));
        snprintf(m->id, sizeof(m->id), "widescreen-db");
        snprintf(m->name, sizeof(m->name), "%s", T(S_WS_DB));
        snprintf(m->desc, sizeof(m->desc), "%s", T(S_WS_DB_HELP));
    }
    SDL_free(t);
    xcheats_load();
}

static bool mod_on(const ModItem *m)
{
    const char *v[64];
    const int n = cfg_all(&D.game, m->cheat ? "cheat" : "mod", v, 64);
    for (int i = 0; i < n; i++) {
        if (!strcmp(v[i], m->id)) {
            return true;
        }
    }
    return false;
}

/* a line of help under a list, wrapped (three lines at most; CJK, without
 * spaces, cut anywhere) */
static void help_panel(const char *s)
{
    const float W = ui_w(), H = ui_h(), tw = W - 160;
    ui_round(60, H - 170, W - 120, 104, 12, UI_PANEL);
    float y = H - 160;
    for (int ln = 0; ln < 3 && *s; ln++) {
        char line[512];
        size_t n = 0, last_space = 0;
        while (s[n] && n < sizeof(line) - 1) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (s[n] == ' ') {
                last_space = n;
            }
            if (ui_text_width(line, 20, UI_FONT_REGULAR) > tw) {
                n = last_space ? last_space : n;
                while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
                    n--;    /* not inside a character */
                }
                break;
            }
            n++;
        }
        snprintf(line, sizeof(line), "%.*s", (int)n, s);
        ui_text(80, y, line, 20, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_LEFT);
        s += n;
        while (*s == ' ') {
            s++;
        }
        y += 28;
    }
}

/* the game's frame rate patch in the list (60 or 120 fps), if it has one */
static const ModItem *fps_patch(void)
{
    for (int i = 0; i < g_nmods; i++) {
        const ModItem *m = &g_mods[i];
        char s[200];
        snprintf(s, sizeof(s), "%s %s", m->id, m->name);
        for (char *c = s; *c; c++) {
            *c = (char)tolower((unsigned char)*c);
        }
        if (!m->cheat && (strstr(s, "60fps") || strstr(s, "60 fps") || strstr(s, "120fps") || strstr(s, "120 fps"))) {
            return m;
        }
    }
    return NULL;
}

void scr_mods(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(T(S_MODS), D.cur->g.title);
    mods_load();
    List l;
    list_begin(&l, 74, 150, W - 148, H - 330, 58);
    const ModItem *shown = NULL;
    /* the frame rate, this game's choice: the console's pace (Original), or
     * lifted ("uncap") up to 60 or 120 frames a second ("fpscap") or
     * without a limit */
    const char *help = NULL;
    {
        static const enum UiStr k_names[4] = { S_FR_CAPPED, S_FR_60, S_FR_120, S_FR_UNCAPPED };
        static const char *const k_caps[4] = { NULL, "60", "120", NULL };
        const char *cap = cfg_get(&D.game, "fpscap");
        const int cur = !cfg_bool(&D.game, "uncap", false) ? 0
                        : cap && atoi(cap) == 60 ? 1 : cap && atoi(cap) == 120 ? 2 : 3;
        int delta = 0;
        if (list_row(&l, T(S_FRAMERATE), T(k_names[cur]), &delta)) {
            delta = 1;
        } else if (D.in.repeat) {
            delta = 0;                      /* a step a press */
        }
        if (delta) {
            const int to = (cur + (delta > 0 ? 1 : 3)) % 4;
            if (to == 0) {
                cfg_unset(&D.game, "uncap");
            } else {
                cfg_set(&D.game, "uncap", "1");
            }
            if (k_caps[to]) {
                cfg_set(&D.game, "fpscap", k_caps[to]);
            } else {
                cfg_unset(&D.game, "fpscap");
            }
            /* unlocked where the game has a patch of its own for it: the
             * patch, made for the game, is the better way */
            const ModItem *fp = fps_patch();
            if (cur == 0 && to != 0 && fp && !mod_on(fp)) {
                static char tip[600];
                snprintf(tip, sizeof(tip), T(S_FPS_PATCH_TIP), fp->name);
                dash_toast(tip);
            }
        }
        if (list_selected(&l)) {
            const ModItem *fp = fps_patch();
            static char tip[600];
            if (fp) {
                snprintf(tip, sizeof(tip), T(S_FPS_PATCH_TIP), fp->name);
            }
            help = fp ? tip : T(S_UNCAP_HELP);
        }
    }
    for (int i = 0; i < g_nmods; i++) {
        ModItem *m = &g_mods[i];
        char label[160];
        snprintf(label, sizeof(label), "%s%s", m->cheat ? "[cheat] " : "", m->name);
        int delta = 0;
        if (list_row(&l, label, mod_on(m) ? T(S_ON) : T(S_OFF), &delta) || delta) {
            if (mod_on(m)) {
                cfg_remove_value(&D.game, m->cheat ? "cheat" : "mod", m->id);
            } else {
                cfg_add(&D.game, m->cheat ? "cheat" : "mod", m->id);
            }
        }
        if (list_selected(&l)) {
            shown = m;
        }
    }
    static char xhelp[512];
    for (int i = 0; i < g_nxopts; i++) {
        const XOpt *e = &g_xopts[i];
        char label[160];
        snprintf(label, sizeof(label), "[cheat] %s", e->label);
        const char *st = xcheat_states(e->file);
        const bool on = e->idx < (int)strlen(st) && st[e->idx] == '1';
        int delta = 0;
        if (list_row(&l, label, on ? T(S_ON) : T(S_OFF), &delta) || delta) {
            xcheat_toggle(e);
        }
        if (list_selected(&l)) {
            const char *base = strrchr(e->file, '/');
            snprintf(xhelp, sizeof(xhelp), "%s %s (%s). %s", T(S_XCHEAT_FROM), e->from, base ? base + 1 : e->file,
                     T(S_XCHEAT_HELP));
            help = xhelp;
        }
    }
    list_end(&l);
    if (D.toast[0] && ui_time() < D.toast_until) {
        /* a notice over the help's place (the frame rate patch's) */
    } else if (help) {
        help_panel(help);
    } else if (shown && shown->desc[0]) {
        help_panel(shown->desc);
    }
    if (D.in.back) {
        dash_pop();
    }
    draw_hints(T(S_CHANGE), T(S_BACK), NULL, NULL);
}

/* ------------------------------------------------------------ save states */

static SDL_Texture *g_thumb[11];
static char g_thumb_key[24];
static int g_del_slot;

static void del_state(Dash *d)
{
    char dir[1100], p[1200];
    states_dir(d->cur, dir, sizeof(dir));
    snprintf(p, sizeof(p), "%s/slot%d.hxs", dir, g_del_slot);
    remove(p);
    snprintf(p, sizeof(p), "%s/slot%d.bmp", dir, g_del_slot);
    remove(p);
    g_thumb_key[0] = 0;
}

/* a state's picture (the game writes a BMP of its frame) */
SDL_Texture *load_bmp(const char *path)
{
    SDL_Surface *s = SDL_LoadBMP(path);
    if (!s) {
        return NULL;
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(ui_renderer(), s);
    SDL_DestroySurface(s);
    return t;
}

/* the thumbnails go when the list does (another game, a slot saved) */
void states_forget(void)
{
    g_thumb_key[0] = 0;
}

void scr_states(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(T(S_SAVESTATES), D.cur->g.title);
    char dir[1100];
    states_dir(D.cur, dir, sizeof(dir));
    if (strcmp(g_thumb_key, D.cur->g.key)) {
        for (int i = 0; i < 11; i++) {
            if (g_thumb[i]) {
                SDL_DestroyTexture(g_thumb[i]);
                g_thumb[i] = NULL;
            }
            char p[1200];
            snprintf(p, sizeof(p), "%s/slot%d.bmp", dir, i);
            g_thumb[i] = load_bmp(p);
        }
        snprintf(g_thumb_key, sizeof(g_thumb_key), "%s", D.cur->g.key);
    }
    List l;
    list_begin(&l, 74, 150, W * 0.5f - 74, H - 244, 64);
    int chosen = -1, sel = 0;
    bool sel_has = false;
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
        if (list_row_icon(&l, g_thumb[s], label, has ? when : T(S_EMPTY), NULL, NULL) && has) {
            chosen = s;
        }
        if (list_selected(&l)) {
            sel = s;
            sel_has = has;
            if (D.in.x && has) {
                g_del_slot = s;
                dash_ask(T(S_CONFIRM_DELETE), del_state);
            }
        }
    }
    list_end(&l);
    /* the preview: the selected slot's picture, large (outside the list's clip) */
    if (sel) {
        const float px = W * 0.5f + 30, pw = W * 0.5f - 104, ph = pw * 3 / 4;
        ui_round(px - 8, 142, pw + 16, ph + 16, 12, UI_PANEL);
        if (g_thumb[sel]) {
            ui_image(g_thumb[sel], px, 150, pw, ph, 1);
        } else {
            ui_text(px + pw / 2, 150 + ph / 2 - 16, sel_has ? "" : T(S_EMPTY), 28, UI_FONT_REGULAR, UI_GREY,
                    UI_ALIGN_CENTER);
        }
    }
    if (chosen > 0) {
        char arg[1300];
        snprintf(arg, sizeof(arg), "loadstate=%s/slot%d.hxs", dir, chosen);
        launch_game(D.cur, arg);
    }
    if (D.in.back) {
        dash_pop();
    }
    draw_hints(T(S_LOAD_STATE), T(S_BACK), T(S_DELETE), NULL);
}

/* ------------------------------------------------------------ controls */

void scr_controls(void)
{
    const float W = ui_w(), H = ui_h();
    char title[96];
    snprintf(title, sizeof(title), "%s — %s %d", T(S_CONTROLS), T(S_PLAYER), D.player + 1);
    draw_header(title, D.editing_game && D.cur ? D.cur->g.title : NULL);
    Config *c = D.editing_game ? &D.game : &D.global;
    List l;
    list_begin(&l, 74, 150, W - 148, H - 244, 52);
    if (D.editing_game) {
        const bool custom = cfg_bool(&D.game, "map.custom", false);
        if (list_row(&l, T(S_CUSTOM_CONTROLS), custom ? T(S_ON) : T(S_OFF), NULL)) {
            if (custom) {
                im_clear_map(&D.game, D.player);
                cfg_unset(&D.game, "map.custom");
            } else {
                im_copy_map(&D.game, &D.global, D.player);
                cfg_set(&D.game, "map.custom", "1");
            }
        }
    } else {
        char val[128];
        im_device_name(c, D.player, val, sizeof(val));
        int delta = 0;
        list_row(&l, T(S_DEVICE), val, &delta);
        if (delta) {
            im_cycle_device(c, D.player, delta);
        }
    }
    if (!D.editing_game || cfg_bool(&D.game, "map.custom", false)) {
        for (int i = 0; i < IM_INPUTS; i++) {
            char val[96];
            im_binding_name(c, D.player, i, val, sizeof(val));
            if (list_row(&l, im_input_label(i), val, NULL)) {
                D.remap_target = i;
                D.remap_since = ui_time();
                dash_push(SCR_REMAP);
            }
            if (list_selected(&l) && D.in.x) {
                im_unbind(c, D.player, i);
            }
        }
        if (list_row(&l, T(S_RESET_DEFAULTS), NULL, NULL)) {
            im_clear_map(c, D.player);
        }
    }
    list_end(&l);
    if (D.in.lb || D.in.rb) {
        D.player = (D.player + (D.in.rb ? 1 : 3)) % 4;
    }
    if (D.in.back) {
        dash_save_cfg();
        dash_pop();
    }
    draw_hints(T(S_REMAP), T(S_BACK), T(S_DELETE), NULL);
}

/* ------------------------------------------------------------ the sticks */

/* The farthest each stick went, per direction: the stick's own position
 * (x, y: -1..1, up positive), so the outline follows the settings as they
 * change. */
#define STICK_DIRS 72
static float g_reach[2][STICK_DIRS][2];
static double g_sticks_seen;

static void stick_box(int s, SDL_Gamepad *pad, float bx, float by, float S, float dz, float range, bool round)
{
    const float cx = bx + S / 2, cy = by + S / 2, h = S / 2;
    ui_text(cx, by - 30, T(s ? S_IN_RSTICK : S_IN_LSTICK), 20, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_CENTER);
    ui_round(bx - 12, by - 12, S + 24, S + 24, 12, UI_PANEL);
    /* the range the games expect: the Duke's, the whole square */
    ui_rect(bx, by, S, S, ui_alpha(UI_GREEN, 0.07f));
    ui_round_outline(bx, by, S, S, 2, 2, UI_GREEN);
    ui_line(cx, by, cx, by + S, 1, ui_alpha(UI_GREEN, 0.18f));
    ui_line(bx, cy, bx + S, cy, 1, ui_alpha(UI_GREEN, 0.18f));
    if (dz > 0) {
        ui_ring(cx, cy, dz * h, dz * h, 0, 1.5f, ui_alpha(UI_GREY, 0.5f));
    }
    float x = 0, y = 0;
    if (pad) {
        x = SDL_GetGamepadAxis(pad, s ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        y = -SDL_GetGamepadAxis(pad, s ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        x = x < -1 ? -1 : x;
        y = y < -1 ? -1 : y;
        const float r = sqrtf(x * x + y * y);
        if (r > dz) {
            int d = (int)lroundf(atan2f(y, x) / (2 * (float)M_PI) * STICK_DIRS);
            d = (d % STICK_DIRS + STICK_DIRS) % STICK_DIRS;
            float *k = g_reach[s][d];
            if (r > sqrtf(k[0] * k[0] + k[1] * k[1])) {
                k[0] = x;
                k[1] = y;
            }
        }
    }
    /* the range the stick reached, as the game gets it: filled, outlined.
     * Directions a quick turn skipped (up to 30 degrees) take the line
     * between their neighbours. */
    float out[STICK_DIRS][2];
    bool got[STICK_DIRS];
    int ngot = 0;
    for (int i = 0; i < STICK_DIRS; i++) {
        const float *k = g_reach[s][i];
        got[i] = k[0] != 0 || k[1] != 0;
        if (got[i]) {
            im_stick(k[0], k[1], dz, range, round, &out[i][0], &out[i][1]);
            ngot++;
        }
    }
    bool fill[STICK_DIRS] = {false};
    for (int i = 0; i < STICK_DIRS && ngot > 1; i++) {
        if (!got[i] || got[(i + 1) % STICK_DIRS]) {
            continue;
        }
        int j = 2;
        while (j <= 7 && !got[(i + j) % STICK_DIRS]) {
            j++;
        }
        if (j > 7) {
            continue;
        }
        const float *p0 = out[i], *p1 = out[(i + j) % STICK_DIRS];
        for (int g = 1; g < j; g++) {
            const float t = (float)g / j;
            float *q = out[(i + g) % STICK_DIRS];
            q[0] = p0[0] + (p1[0] - p0[0]) * t;
            q[1] = p0[1] + (p1[1] - p0[1]) * t;
            fill[(i + g) % STICK_DIRS] = true;
        }
    }
    float reach = 0, px = 0, py = 0;
    bool prev = false;
    for (int i = 0; i <= STICK_DIRS; i++) {
        const int d = i % STICK_DIRS;
        if (!got[d] && !fill[d]) {
            prev = false;
            continue;
        }
        const float ox = out[d][0], oy = out[d][1];
        const float qx = cx + ox * h, qy = cy - oy * h;
        if (i < STICK_DIRS) {
            ui_line(cx, cy, qx, qy, 4, ui_alpha(UI_WHITE, 0.10f));
            /* against the square's edge in that direction */
            const float m = fmaxf(fabsf(ox), fabsf(oy));
            reach += m > 1 ? 1 : m;
        }
        if (prev) {
            ui_line(px, py, qx, qy, 2.5f, UI_WHITE);
        }
        px = qx;
        py = qy;
        prev = true;
    }
    reach /= STICK_DIRS;
    if (pad) {
        float ox, oy;
        im_stick(x, y, dz, range, round, &ox, &oy);
        ui_circle(cx + x * h, cy - y * h, 5, ui_alpha(UI_GREY, 0.8f));      /* the stick itself */
        ui_glow(cx + ox * h, cy - oy * h, 22, ui_alpha(UI_GREEN_HI, 0.5f));
        ui_circle(cx + ox * h, cy - oy * h, 8, UI_GREEN_HI);                /* what the game gets */
    }
    char t[64];
    snprintf(t, sizeof(t), "%s: %d%%", T(S_STICK_REACH), (int)lroundf(reach * 100));
    ui_text(cx, by + S + 18, t, 22, UI_FONT_BOLD, reach >= 0.95f ? UI_GREEN_HI : reach >= 0.8f ? UI_WHITE : UI_RED,
            UI_ALIGN_CENTER);
}

void scr_sticks(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(T(S_STICK_RANGE), NULL);
    if (ui_time() - g_sticks_seen > 0.5) {
        memset(g_reach, 0, sizeof(g_reach));    /* a fresh look each time it opens */
    }
    g_sticks_seen = ui_time();
    Config *c = D.editing_game ? &D.game : &D.global;
    const char *shape = cfg_get(c, "stick.shape") ? cfg_get(c, "stick.shape") : cfg_get(&D.global, "stick.shape");
    const bool round = shape && !strcmp(shape, "round");
    const Config *rc = cfg_get(c, "stick.range") ? c : &D.global;
    const Config *dc = cfg_get(c, "deadzone") ? c : &D.global;
    const float range = cfg_int(rc, "stick.range", 100) / 100.0f, dz = cfg_int(dc, "deadzone", 10) / 100.0f;

    /* the legend: the square, the outline */
    const float ly = 128;
    float lx = W / 2 - (ui_text_width(T(S_STICK_NEEDED), 18, UI_FONT_REGULAR) +
                        ui_text_width(T(S_STICK_REACHED), 18, UI_FONT_REGULAR) + 92) / 2;
    ui_round_outline(lx, ly + 3, 16, 16, 2, 2, UI_GREEN);
    lx += 24;
    lx += ui_text(lx, ly, T(S_STICK_NEEDED), 18, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT) + 32;
    ui_line(lx, ly + 11, lx + 22, ly + 11, 2.5f, UI_WHITE);
    lx += 30;
    ui_text(lx, ly, T(S_STICK_REACHED), 18, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT);

    SDL_Gamepad *pad = dash_active_pad();
    const float S = fminf(220, (W - 200) / 2 - 60), by = 196, gap = fminf(160, W - 2 * S - 200);
    const float bx0 = W / 2 - S - gap / 2, bx1 = W / 2 + gap / 2;
    stick_box(0, pad, bx0, by, S, dz, range, round);
    stick_box(1, pad, bx1, by, S, dz, range, round);
    ui_text(W / 2, by + S + 52, T(pad ? S_STICK_ROTATE : S_STICK_NO_PAD), 18, UI_FONT_REGULAR,
            pad ? UI_GREY : UI_RED, UI_ALIGN_CENTER);

    List l;
    list_begin(&l, 74, by + S + 96, W - 148, 3 * 44, 44);
    opt_int(&l, S_STICK_RANGE, "stick.range", 50, 150, 5, 100, "%");
    static const char *const sv[] = {"square", "round"};
    const char *sn[] = {T(S_SHAPE_SQUARE), T(S_SHAPE_ROUND)};
    opt_choice(&l, S_STICK_SHAPE, "stick.shape", sv, sn, 2, "square");
    opt_int(&l, S_DEADZONE, "deadzone", 0, 50, 5, 10, "%");
    list_end(&l);
    if (D.in.x) {
        memset(g_reach, 0, sizeof(g_reach));
    }
    if (D.in.back) {
        dash_save_cfg();
        dash_pop();
    }
    draw_hints(T(S_CHANGE), T(S_BACK), T(S_CLEAR), NULL);
    (void)H;
}

/* ------------------------------------------------------------ the triggers */

/* The farthest each trigger went since the screen opened (0..1) */
static float g_tpeak[2];
static double g_triggers_seen;

/* A trigger's travel in a bar (the travel up, as a trigger is pulled) with
 * its dead zone and full-press zone, and beside it what the game gets */
static void trigger_bar(int t, SDL_Gamepad *pad, float cx, float by, float BH, float dz, float full)
{
    const float TW = 64, OW = 30, tx = cx - 62, ox = cx + 30;
    const float bot = by + BH;
#define TY(v) (bot - (v) * BH)
    ui_text(cx, by - 46, T(t ? S_IN_RT : S_IN_LT), 20, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_CENTER);
    ui_round(cx - 118, by - 16, 236, BH + 32, 14, UI_PANEL);
    float a = 0;
    if (pad) {
        a = SDL_GetGamepadAxis(pad, t ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
        a = a < 0 ? 0 : a > 1 ? 1 : a;
        g_tpeak[t] = a > g_tpeak[t] ? a : g_tpeak[t];
    }
    const float out = im_trigger(a, dz, full);
    /* the scale: a tick every quarter, labelled at the ends and the middle */
    for (int q = 0; q <= 4; q++) {
        const float y = TY(q / 4.0f);
        ui_line(tx - 12, y, tx - 4, y, 1.5f, ui_alpha(UI_GREEN, 0.45f));
        if (q % 2 == 0) {
            char s[8];
            snprintf(s, sizeof(s), "%d", q * 25);
            ui_text(tx - 16, y - 8, s, 14, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_RIGHT);
        }
    }
    /* the trigger's travel: the track, the dead zone (grey) at the start, the
     * full press (green) at the end */
    ui_round(tx, by, TW, BH, 10, ui_alpha(UI_GREEN_DARK, 0.95f));
    if (dz > 0) {
        ui_rect(tx + 3, TY(dz), TW - 6, dz * BH - 3, ui_alpha(UI_GREY, 0.20f));
        ui_line(tx + 3, TY(dz), tx + TW - 3, TY(dz), 1.5f, ui_alpha(UI_GREY, 0.7f));
    }
    if (full < 1) {
        ui_rect(tx + 3, by + 3, TW - 6, TY(full) - by - 3, ui_alpha(UI_GREEN, 0.16f));
        ui_line(tx + 3, TY(full), tx + TW - 3, TY(full), 1.5f, ui_alpha(UI_GREEN, 0.8f));
    }
    if (a > 0) {
        ui_rect_grad(tx + 6, TY(a), TW - 12, a * BH - 6, ui_alpha(UI_WHITE, 0.55f), ui_alpha(UI_WHITE, 0.12f));
    }
    if (g_tpeak[t] > 0) {   /* the farthest it went */
        const float py = TY(g_tpeak[t]);
        ui_line(tx - 2, py, tx + TW + 2, py, 2, UI_WHITE);
    }
    ui_round_outline(tx, by, TW, BH, 10, 1.5f, ui_alpha(UI_GREEN, 0.55f));
    /* what the game gets, its fill joined to the trigger's position */
    ui_round(ox, by, OW, BH, 8, ui_alpha(UI_GREEN_DARK, 0.95f));
    if (out > 0) {
        ui_rect_grad(ox + 4, TY(out), OW - 8, out * BH - 4, UI_GREEN_HI, UI_GREEN_DIM);
        ui_glow(ox + OW / 2, TY(out), 26, ui_alpha(UI_GREEN_HI, 0.45f));
    }
    if (pad) {
        ui_line(tx + TW + 2, TY(a), ox - 2, TY(out), 1.5f, ui_alpha(UI_GREEN_HI, 0.6f));
        ui_circle(tx + TW + 2, TY(a), 4, UI_WHITE);
        ui_circle(ox - 2, TY(out), 4, UI_GREEN_HI);
    }
    ui_round_outline(ox, by, OW, BH, 8, out >= 1 ? 2.5f : 1.5f, out >= 1 ? UI_GREEN_HI : ui_alpha(UI_GREEN, 0.55f));
    char s[64];
    snprintf(s, sizeof(s), "%d%%", (int)lroundf(out * 100));
    ui_text(ox + OW + 10, TY(out) - 10, s, 18, UI_FONT_BOLD, out >= 1 ? UI_GREEN_HI : UI_WHITE, UI_ALIGN_LEFT);
#undef TY
    /* the reach: what the game got at the farthest press */
    const float reach = im_trigger(g_tpeak[t], dz, full);
    snprintf(s, sizeof(s), "%s: %d%%", T(S_STICK_REACH), (int)lroundf(reach * 100));
    ui_text(cx, bot + 30, s, 22, UI_FONT_BOLD, reach >= 0.99f ? UI_GREEN_HI : reach >= 0.8f ? UI_WHITE : UI_RED,
            UI_ALIGN_CENTER);
}

void scr_triggers(void)
{
    const float W = ui_w();
    draw_header(T(S_TRIGGERS), NULL);
    if (ui_time() - g_triggers_seen > 0.5) {
        memset(g_tpeak, 0, sizeof(g_tpeak));    /* a fresh look each time it opens */
    }
    g_triggers_seen = ui_time();
    Config *c = D.editing_game ? &D.game : &D.global;
    const Config *dc = cfg_get(c, "trigger.deadzone") ? c : &D.global;
    const Config *fc = cfg_get(c, "trigger.full") ? c : &D.global;
    const float dz = cfg_int(dc, "trigger.deadzone", 10) / 100.0f, full = cfg_int(fc, "trigger.full", 100) / 100.0f;

    /* the legend: the trigger's travel, what the game gets */
    const float ly = 128;
    float lx = W / 2 - (ui_text_width(T(S_TRIGGER_YOURS), 18, UI_FONT_REGULAR) +
                        ui_text_width(T(S_TRIGGER_GAME), 18, UI_FONT_REGULAR) + 80) / 2;
    ui_round(lx, ly + 2, 16, 18, 4, ui_alpha(UI_WHITE, 0.45f));
    lx += 24;
    lx += ui_text(lx, ly, T(S_TRIGGER_YOURS), 18, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT) + 32;
    ui_rect_grad(lx, ly + 2, 16, 18, UI_GREEN_HI, UI_GREEN_DIM);
    lx += 24;
    ui_text(lx, ly, T(S_TRIGGER_GAME), 18, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT);

    SDL_Gamepad *pad = dash_active_pad();
    const float by = 214, BH = 230, gap = fminf(420, W - 480);
    trigger_bar(0, pad, W / 2 - gap / 2, by, BH, dz, full);
    trigger_bar(1, pad, W / 2 + gap / 2, by, BH, dz, full);
    ui_text(W / 2, by + BH + 70, T(pad ? S_TRIGGER_PRESS : S_TRIGGER_NO_PAD), 18, UI_FONT_REGULAR,
            pad ? UI_GREY : UI_RED, UI_ALIGN_CENTER);

    List l;
    list_begin(&l, 74, by + BH + 112, W - 148, 2 * 44, 44);
    opt_int(&l, S_TRIGGER_DEADZONE, "trigger.deadzone", 0, 50, 2, 10, "%");
    opt_int(&l, S_TRIGGER_FULL, "trigger.full", 50, 100, 5, 100, "%");
    list_end(&l);
    if (D.in.x) {
        memset(g_tpeak, 0, sizeof(g_tpeak));
    }
    if (D.in.back) {
        dash_save_cfg();
        dash_pop();
    }
    draw_hints(T(S_CHANGE), T(S_BACK), T(S_CLEAR), NULL);
}

void scr_remap(void)
{
    const float W = ui_w(), H = ui_h();
    Config *c = D.editing_game ? &D.game : &D.global;
    const bool hotkey = D.remap_target >= 1000;
    const char *what = hotkey ? T(im_hotkey_label(D.remap_target - 1000)) : im_input_label(D.remap_target);
    ui_rect(0, 0, W, H, (UiColor){0, 0, 0, 150});
    ui_orb(W / 2, H * 0.36f, 50, 0.5f);
    ui_text(W / 2, H * 0.55f, T(S_PRESS_INPUT), 28, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_CENTER);
    ui_text(W / 2, H * 0.62f, what, 40, UI_FONT_BOLD, UI_GREEN_HI, UI_ALIGN_CENTER);
    const float left = 6 - (float)(ui_time() - D.remap_since);
    char t[32];
    snprintf(t, sizeof(t), "%.0f", left > 0 ? left : 0);
    ui_text(W / 2, H * 0.72f, t, 26, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_CENTER);
    if (ui_time() - D.remap_since < 0.3) {
        return;     /* the press that opened it */
    }
    bool done = left <= 0;
    if (D.in.key == SDLK_ESCAPE) {
        done = true;
    } else if (D.in.key) {
        if (hotkey) {
            im_set_hotkey(c, D.remap_target - 1000, D.in.key);
        } else {
            im_bind_key(c, D.player, D.remap_target, D.in.key);
        }
        done = true;
    } else if (!hotkey && D.in.pad_button >= 0) {
        im_bind_button(c, D.player, D.remap_target, D.in.pad_button);
        done = true;
    } else if (!hotkey && D.in.pad_axis) {
        im_bind_axis(c, D.player, D.remap_target, D.in.pad_axis);
        done = true;
    }
    if (done) {
        dash_save_cfg();
        D.sp--;     /* back without the back press reaching the list */
    }
}

/* ------------------------------------------------------------ online */

static void text_field(List *l, enum UiStr label, const char *key, const char *def)
{
    const char *v = cfg_get(&D.global, key);
    const bool editing = D.edit_key == key;
    char shown[300];
    snprintf(shown, sizeof(shown), "%s%s", editing ? D.edit_tmp : v ? v : def,
             editing && (int)(ui_time() * 2) % 2 ? "_" : "");
    if (list_row(l, T(label), shown, NULL)) {
        if (editing) {
            cfg_set(&D.global, key, D.edit_tmp);
            D.edit_key = NULL;
            SDL_StopTextInput(SDL_GetKeyboardFocus());
        } else {
            D.edit_key = key;
            snprintf(D.edit_tmp, sizeof(D.edit_tmp), "%s", v ? v : def);
            SDL_StartTextInput(SDL_GetKeyboardFocus());
        }
    }
    if (editing) {
        if (D.in.text && strlen(D.edit_tmp) + strlen(D.in.text) < sizeof(D.edit_tmp)) {
            strcat(D.edit_tmp, D.in.text);
        }
        if (D.in.key == SDLK_BACKSPACE && D.edit_tmp[0]) {
            D.edit_tmp[strlen(D.edit_tmp) - 1] = 0;
            D.in.back = false;
        }
        if (D.in.key == SDLK_ESCAPE) {
            D.edit_key = NULL;
            SDL_StopTextInput(SDL_GetKeyboardFocus());
            D.in.back = false;
        }
        if (!list_selected(l)) {
            /* moved off: keep what was typed */
        }
    }
}

void scr_online(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(T(S_ONLINE), NULL);
    D.editing_game = false;
    List l;
    list_begin(&l, 74, 150, W - 148, 330, 58);
    static const char *nv[] = {"off", "nat", "kai", "lan"};
    const char *nn[] = {T(S_NET_OFF), T(S_NET_INSIGNIA), T(S_NET_KAI), T(S_NET_LAN)};
    opt_choice(&l, S_NETWORK, "net", nv, nn, 4, "off");
    const char *net = cfg_get(&D.global, "net");
    const char *help = T(S_NET_OFF);
    const char *site = NULL;
    if (net && !strcmp(net, "nat")) {
        text_field(&l, S_DNS, "dns", "46.101.64.175");
        help = T(S_INSIGNIA_HELP);
        site = "https://insignia.live";
    } else if (net && !strcmp(net, "kai")) {
        text_field(&l, S_KAI_ENGINE, "kai", "127.0.0.1");
        help = T(S_KAI_HELP);
        site = "https://www.teamxlink.co.uk";
    } else if (net && !strcmp(net, "lan")) {
        text_field(&l, S_LAN_PEERS, "lanpeers", "");
        help = T(S_LAN_HELP);
    }
    if (site && list_row(&l, T(S_OPEN_WEBSITE), site, NULL)) {
        SDL_OpenURL(site);
    }
    list_end(&l);
    /* the help, wrapped */
    ui_round(60, 510, W - 120, H - 600, 12, UI_PANEL);
    const float tw = W - 160;
    const char *s = help;
    float y = 524;
    while (*s && y < H - 110) {
        char line[512];
        size_t n = 0, last_space = 0;
        while (s[n] && n < sizeof(line) - 1) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (s[n] == ' ') {
                last_space = n;
            }
            if (ui_text_width(line, 21, UI_FONT_REGULAR) > tw) {
                /* CJK has no spaces: cut anywhere then */
                n = last_space ? last_space : n;
                break;
            }
            n++;
        }
        snprintf(line, sizeof(line), "%.*s", (int)n, s);
        ui_text(80, y, line, 21, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_LEFT);
        s += n;
        while (*s == ' ') {
            s++;
        }
        y += 30;
    }
    if (D.in.back && !D.edit_key) {
        dash_save_cfg();
        dash_pop();
    }
    draw_hints(T(S_CHANGE), T(S_BACK), NULL, NULL);
}
