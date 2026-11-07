/*
 * xbased (hakuX for PC) — the front end: an SDL3 window, the controllers and
 * the keyboard, the core on its own thread, the HLE GPU's frames presented
 * into the window once per display refresh (present.c), as the Android
 * app's NativeCoreActivity and app_main.c do.
 *
 *   hakux [disc image] [option...]
 *
 * With no disc image, a file dialog asks for one. Options are the Android
 * app's arguments: the core's (native/core/options.h: scale=, loadstate=,
 * video=, lang=, net=, mods=, uncap, ...), the window's (aspect=auto|4:3|
 * 16:9|stretch, fxaa=0|1, upscale=bilinear|fsr), and
 *   hdd=DIR      the hard disk folder (default: hdd beside the executable)
 *   mod=ID       a mod of the game's on (its folder in mods\<TITLE ID>.<version>
 *                beside the executable; mod=* all of them), cheat=NAME a cheat
 *   fullscreen=0 start in a window (full screen, borderless, otherwise)
 *   console      a console window for the log (it always goes to xbased.log)
 * xbased.ini beside the executable holds options too, one a line ('#'
 * comments): the command line's come after, and win.
 *
 * Keys: F11 or Alt+Enter full screen, F2 pause, F5 save the state, F8 load
 * it (slot 1; Shift: slot 2), Esc leaves full screen. Player 1 plays on the
 * keyboard while no controller is plugged in (win_keys below).
 *
 * Loading a state, a title's relaunch (XLaunchNewImage) and its reboot
 * start the core again: the process runs itself again with the option the
 * Android app passes its activity (loadstate=, relaunch).
 */
#include "native/app/app.h"
#include "native/app/ui/config.h"
#include "native/app/ui/console_cfg.h"
#include "native/app/ui/dash.h"
#include "native/app/ui/i18n.h"
#include "native/app/ui/input_map.h"
#include "native/app/ui/ui.h"
#include "hw/xbox/hle/vk/swapchain.h"
#include "native/app/ui/xbox_data.h"
#include "native/app/ui/xemu_import.h"
#include "native/app/win_auto.h"
#include "native/app/win_mods.h"
#include "native/core/cheats.h"
#include "native/core/core.h"
#include "native/core/mods.h"
#include "native/core/options.h"
#include "native/dev/usb.h"
#include "native/kernel/kernel.h"
#include "native/net/net.h"
#include "native/state/save.h"
#include "native/hle/hle_native.h"
#include "hw/xbox/hle/vk/native_window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "native/app/desk.h"   /* windows.h, or its few calls over POSIX */
#ifdef _WIN32
#include <shellapi.h>
#endif

/* native/dev/apu/out.c */
void nc_audio_set_paused(bool on);
void nc_audio_set_volume(float v);
/* native/dev/ohci.c */
void nc_dev_ohci_set_pads(unsigned mask);
/* hw/xbox/hle/pb/pbmode.h */
bool hle_pb_active(void);

#define MAX_ARGS 512

static NcCore g_core;
static int g_disc_fd = -1;
static SDL_AtomicInt g_core_state; /* 0 booting, 1 running, -1 failed or stopped */
static struct ANativeWindow g_nw;
static char g_exe_dir[MAX_PATH];
static char g_data_dir[MAX_PATH]; /* settings, saves, states, the log (cfg_data_dir) */
static char g_disc[1024];
static char *g_args[MAX_ARGS];     /* the options: xbased.ini's, the game's, the command line's */
static int g_nargs;
/* The command line's own options, which a run started again gets: the
 * settings' and the game's are read again by the new run. Passing all of
 * them on, each run added the settings again: a few relaunches or loaded
 * states filled the list, and what came last (the state to load, the
 * relaunch flag) was dropped, so a state loaded restarted the game, and a
 * title that relaunches (Chicago Enforcer) did so over and over. */
static char *g_cmdline[MAX_ARGS];
static int g_ncmdline;
static int g_ncfg_args;            /* how many of them xbased.ini gave */
static ImMap g_map;     /* the controls: the game's file over xbased.ini (input_map.h) */
static char g_key[24];  /* the game's "<TITLE ID>.<version>" */
static uint32_t g_title_id;  /* ... its title id */

/* ------------------------------------------------------------ helpers */

static void exe_dir(void)
{
    DWORD n = GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
    while (n && g_exe_dir[n - 1] != '\\' && g_exe_dir[n - 1] != '/') {
        n--;
    }
    g_exe_dir[n] = 0;
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", cfg_data_dir());
}

static void add_arg(const char *a)
{
    static bool told;
    if (g_nargs < MAX_ARGS && a && a[0]) {
        g_args[g_nargs++] = strdup(a);
    } else if (a && a[0] && !told) {
        told = true;
        nc_log("hakuX-app", "more than %d options: %s and those after it left out", MAX_ARGS, a);
    }
}

/* xbased.ini: one option a line (hakux.ini, an earlier build's, becomes it) */
static void read_ini(void)
{
    char path[MAX_PATH + 16], old[MAX_PATH + 16];
    snprintf(path, sizeof(path), "%sxbased.ini", g_data_dir);
    snprintf(old, sizeof(old), "%shakux.ini", g_data_dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        MoveFileA(old, path);
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t')) {
            p[--n] = 0;
        }
        if (n && p[0] != '#' && p[0] != ';') {
            add_arg(p);
        }
    }
    fclose(f);
}

static const char *opt_value(const char *name)
{
    const size_t n = strlen(name);
    const char *v = NULL;
    for (int i = 0; i < g_nargs; i++) {
        if (!strncmp(g_args[i], name, n) && g_args[i][n] == '=') {
            v = g_args[i] + n + 1; /* the last one wins */
        }
    }
    return v;
}

static bool opt_flag(const char *name)
{
    for (int i = 0; i < g_nargs; i++) {
        if (!strcmp(g_args[i], name)) {
            return true;
        }
    }
    return false;
}

/* Full screen (borderless, the desktop's mode) unless told otherwise
 * ("fullscreen=0"): a console's picture, not a window's. */
static bool want_fullscreen(void)
{
    const char *v = opt_value("fullscreen");
    return v ? strcmp(v, "0") != 0 : true;
}

/* The game's own options (games/<TITLE ID>.<version>.ini) after the global
 * ones and before the command line's (which come last and win); the
 * console's language and video flags as console_cfg.c keeps them; the dashboard's
 * "uncap=1" as the flag the core takes, "fullscreen" (on unless "=0") once. */
static void game_options(const char *disc)
{
    XdGame g;
    xd_read_game(disc, &g);
    xd_free_game(&g);
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%sgames/%s.ini", g_data_dir, g.key);
    Config c;
    cfg_load(&c, path);
    /* the command line's options after the game's: move them to the end */
    char *cmdline[MAX_ARGS];
    int ncmd = 0;
    for (int i = g_ncfg_args; i < g_nargs; i++) {
        cmdline[ncmd++] = g_args[i];
    }
    g_nargs = g_ncfg_args;
    for (int i = 0; i < c.n; i++) {
        char a[600];
        if (c.flag[i]) {
            snprintf(a, sizeof(a), "%s", c.key[i]);
        } else {
            snprintf(a, sizeof(a), "%s=%s", c.key[i], c.val[i]);
        }
        add_arg(a);
    }
    for (int i = 0; i < ncmd; i++) {
        if (g_nargs < MAX_ARGS) {
            g_args[g_nargs++] = cmdline[i];
        }
    }
    if (c.n) {
        nc_log("hakuX-app", "%d options of the game's (%s)", c.n, path);
    }
    snprintf(g_key, sizeof(g_key), "%s", g.key);
    g_title_id = g.title_id;
    Config global;
    snprintf(path, sizeof(path), "%sxbased.ini", g_data_dir);
    cfg_load(&global, path);
    im_resolve(&g_map, &global, &c);
    /* flags written as values */
    static const char *const k_flags[] = {"uncap", "fullscreen"};
    for (int f = 0; f < 2; f++) {
        const char *v = opt_value(k_flags[f]);
        bool on = opt_flag(k_flags[f]) || f == 1;  /* full screen by default */
        if (v) {
            on = strcmp(v, "0") != 0;
        }
        for (int i = 0; i < g_nargs; i++) {
            const size_t n = strlen(k_flags[f]);
            if (!strncmp(g_args[i], k_flags[f], n) && (g_args[i][n] == '=' || g_args[i][n] == 0)) {
                g_args[i][0] = 0;  /* gone; added back once below */
            }
        }
        if (on) {
            add_arg(k_flags[f]);
        } else if (f == 1) {
            add_arg("fullscreen=0");
        }
    }
    i18n_set(opt_value("ui.lang"));     /* the app's messages */
    /* The console's language and video flags: what changed in the EEPROM
     * taken into the global settings first (console_cfg.c), then the
     * command line's, else the game's own, else the global ones (for "auto"
     * or none, the interface's language). The game's own language, or the
     * command line's, is kept even on a disc that seems to lack it. */
    cc_sync(&global);
    const char *cl_lang = NULL, *cl_video = NULL;
    for (int i = 0; i < ncmd; i++) {
        if (!strncmp(cmdline[i], "lang=", 5)) {
            cl_lang = cmdline[i] + 5;
        } else if (!strncmp(cmdline[i], "video=", 6)) {
            cl_video = cmdline[i] + 6;
        }
    }
    const int asked = cl_lang ? atoi(cl_lang) : cfg_get(&c, "lang") ? atoi(cfg_get(&c, "lang")) : 0;
    const bool fixed = asked >= 1 && asked <= 9;
    char a[32];
    snprintf(a, sizeof(a), "lang=%d", fixed ? asked : cc_lang(&global));
    add_arg(a);
    if (fixed) {
        add_arg("langfixed=1");
    }
    if (!cl_video && !cfg_get(&c, "video")) {
        snprintf(a, sizeof(a), "video=0x%x", cc_video(&global));
        add_arg(a);
    }
}

static void make_hdd(const char *root)
{
    char p[1024];
    mkdir(root, 0775);
    for (int i = 0; i <= 5; i++) {
        snprintf(p, sizeof(p), "%s/Partition%d", root, i);
        mkdir(p, 0775);
    }
}

/* The game's window has the screen: the one shown next (the dashboard back,
 * a relaunched title's) is to have it, with the keyboard and the gamepad.
 * Windows locks the foreground: without help that one stayed behind
 * whatever window came next, its taskbar button flashing, as if minimized.
 * Any process may take it (the grant); and back to the dashboard, the game
 * shows the dashboard's window and gives it the foreground itself (its
 * owner may give it to any window): the dashboard that started it says its
 * window in XBASED_DASH_HWND. The grant alone left the dashboard without
 * the gamepad on an ROG Xbox Ally X. While this process still has the
 * foreground: before its window hides (hidden, another window has it). */
static void give_foreground(bool to_dash)
{
#ifdef _WIN32
    AllowSetForegroundWindow(ASFW_ANY);
    const char *e = to_dash ? getenv("XBASED_DASH_HWND") : NULL;
    HWND h = e ? (HWND)(uintptr_t)strtoull(e, NULL, 16) : NULL;
    if (h && IsWindow(h)) {
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
    }
#else
    (void)to_dash;
#endif
}

/* The process ends at once with code. On Windows ExitProcess would first
 * run every DLL's detach (the GPU driver's, the gamepads', the audio's) with
 * the emulation's threads cut off wherever they were, and one waiting on
 * another there hangs the process: a black full-screen window, and the
 * dashboard waiting behind it. TerminateProcess skips that; what the run
 * wrote is written (stderr isn't buffered, files are closed as they go).
 * A code with bit 30 is a run started again (run_again): no dashboard. */
static _Noreturn void exit_now(int code)
{
    fflush(NULL);
#ifdef _WIN32
    give_foreground(!(code & 0x40000000));
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    _exit(code);
}

static void fatal_box(const char *title, const char *msg)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, NULL);
}

static void hold_picture(char *path, size_t n);
static void wait_next_run(void *ev, void *proc);

/* The process again, with the same disc and options plus extra (minus any
 * earlier option of extra's name). hold: the next run shows this one's
 * picture until its first frame (a state loaded, a title relaunched),
 * otherwise black (what this one shows at the end of a transition). On
 * Windows the next run's window comes up under this one, held on top, and
 * this one goes once that one has drawn: no desktop, nor a window without
 * the keyboard, between the two. */
static void run_again(const char *extra, bool hold)
{
    nc_kernel_set_paused(&g_core.kernel, true);    /* the picture held is the last one */
    nc_audio_set_paused(true);
    char hold_path[MAX_PATH + 32] = "";
    if (hold) {
        hold_picture(hold_path, sizeof(hold_path));
    }
    char cmd[8192];
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    int n = snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"", exe, g_disc[0] ? g_disc : "none");
    const char *eq = extra ? strchr(extra, '=') : NULL;
    const size_t name_len = eq ? (size_t)(eq - extra) : extra ? strlen(extra) : 0;
    for (int i = 0; i < g_ncmdline && n < (int)sizeof(cmd) - 8; i++) {
        const char *a = g_cmdline[i];
        if (extra && !strncmp(a, extra, name_len) && (a[name_len] == '=' || a[name_len] == 0)) {
            continue;
        }
        if (!a[0] || !strcmp(a, "relaunch") || !strncmp(a, "loadstate=", 10)) {
            continue; /* one run's */
        }
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " \"%s\"", a);
    }
    if (extra) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " \"%s\"", extra);
    }
    nc_log("hakuX-app", "running again: %s", cmd);
#ifdef _WIN32
    SetEnvironmentVariableA("XBASED_HOLD", hold_path[0] ? hold_path : NULL);
    char ev_name[96];
    snprintf(ev_name, sizeof(ev_name), "Local\\xbased-ready-%lu-%llu", (unsigned long)GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, ev_name);
    SetEnvironmentVariableA("XBASED_READY", ev ? ev_name : NULL);
    char rect[64] = "";
    if (g_nw.sdl_window && !(SDL_GetWindowFlags(g_nw.sdl_window) & SDL_WINDOW_FULLSCREEN)) {
        int x = 0, y = 0, w = 0, h = 0;
        SDL_GetWindowPosition(g_nw.sdl_window, &x, &y);
        SDL_GetWindowSize(g_nw.sdl_window, &w, &h);
        snprintf(rect, sizeof(rect), "%d,%d,%d,%d", x, y, w, h);
    }
    SetEnvironmentVariableA("XBASED_RECT", rect[0] ? rect : NULL);
    if (g_nw.hwnd) {
        SetWindowPos((HWND)g_nw.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    STARTUPINFOA si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi;
    if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, g_exe_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        wait_next_run(ev, pi.hProcess);
        CloseHandle(pi.hProcess);
        /* the dashboard waits for this process: the new one's id in the
         * exit code (bit 30 set) lets it follow the game on (dash.c) */
        exit_now((int)(0x40000000u | (pi.dwProcessId & 0x3FFFFFFFu)));
    }
    if (g_nw.hwnd) {
        SetWindowPos((HWND)g_nw.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#else
    if (hold_path[0]) {
        setenv("XBASED_HOLD", hold_path, 1);
    } else {
        unsetenv("XBASED_HOLD");
    }
    /* this process becomes the new run (the same arguments as above) */
    char *argv[MAX_ARGS + 4];
    int k = 0;
    argv[k++] = exe;
    argv[k++] = g_disc[0] ? g_disc : (char *)"none";
    for (int i = 0; i < g_ncmdline; i++) {
        char *a = g_cmdline[i];
        if (extra && !strncmp(a, extra, name_len) && (a[name_len] == '=' || a[name_len] == 0)) {
            continue;
        }
        if (!a[0] || !strcmp(a, "relaunch") || !strncmp(a, "loadstate=", 10)) {
            continue;
        }
        argv[k++] = a;
    }
    if (extra) {
        argv[k++] = (char *)extra;
    }
    argv[k] = NULL;
    fflush(stderr);
    execv(exe, argv);
#endif
    exit_now(0);
}

/* ------------------------------------------------------------- the core */

static void *core_thread(void *arg)
{
    nc_win_thread_name("vcpu");
    nc_win_thread_priority(1);
    if (!nc_core_boot(&g_core, g_disc_fd)) {
        nc_log("hakuX-app", "core: boot failed");
        if (!nc_hle_fatal()) {
            nc_hle_set_fatal(NC_FATAL_BOOT, "The game stopped while starting.");
        }
        SDL_SetAtomicInt(&g_core_state, -1);
        return NULL;
    }
    SDL_SetAtomicInt(&g_core_state, 1);
    nc_core_run(&g_core);
    nc_log("hakuX-app", "core: stopped");
    SDL_SetAtomicInt(&g_core_state, -1);
    return NULL;
}

/* ------------------------------------------------------------ the input */

/* Controllers by player: each one plugged in takes the first free port. */
static SDL_Gamepad *g_pads[4];
static uint16_t g_rumble[4][2];


/* Player p's gamepad: its device's (auto: the p-th plugged in, padK: the
 * K-th), NULL for the keyboard, none, or nothing plugged. */
static SDL_Gamepad *player_pad(int p)
{
    const int d = g_map.device[p];
    if (d < 0) {
        return NULL;
    }
    const int k = d == 0 ? p : d - 1;
    return k < 4 ? g_pads[k] : NULL;
}

static void pads_changed(void)
{
    unsigned mask = 0;
    for (int p = 0; p < 4; p++) {
        const int d = g_map.device[p];
        /* player 1 always (the keyboard, unless set to none); the others with a pad */
        if (d != -2 && (player_pad(p) || (p == 0 && d <= 0) || d == -1)) {
            mask |= 1u << p;
        }
    }
    nc_dev_ohci_set_pads(mask ? mask : 1);
}

static void pad_added(SDL_JoystickID id)
{
    for (unsigned i = 0; i < 4; i++) {
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            return;
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        if (!g_pads[i]) {
            g_pads[i] = SDL_OpenGamepad(id);
            nc_log("hakuX-app", "controller for player %u: %s", i + 1,
                   g_pads[i] ? SDL_GetGamepadName(g_pads[i]) : SDL_GetError());
            pads_changed();
            return;
        }
    }
}

/* A joystick SDL has no gamepad mapping for (the generic DirectInput pads):
 * the usual layout of those (buttons 0-3 the face buttons, 4-5 the
 * shoulders, 6-7 the triggers, 8-9 Back and Start, 10-11 the sticks'
 * clicks, the hat the d-pad, axes 0-1 and 2-3 the sticks), so it becomes a
 * gamepad. gamecontrollerdb.txt beside the executable, if there, has the
 * right one for many more. */
static void joystick_added(SDL_JoystickID id)
{
    if (SDL_IsGamepad(id)) {
        return;
    }
    char guid[64];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    const char *name = SDL_GetJoystickNameForID(id);
    char map[512];
    snprintf(map, sizeof(map),
             "%s,%s,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,"
             "back:b8,start:b9,leftstick:b10,rightstick:b11,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
             "leftx:a0,lefty:a1,rightx:a2,righty:a3,",
             guid, name ? name : "Joystick");
    nc_log("hakuX-app", "joystick without a gamepad mapping: \"%s\" (%s); the generic layout given", name ? name : "?",
           guid);
    SDL_AddGamepadMapping(map); /* a gamepad now: SDL_EVENT_GAMEPAD_ADDED follows */
}

static void pad_removed(SDL_JoystickID id)
{
    for (unsigned i = 0; i < 4; i++) {
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
            nc_log("hakuX-app", "player %u's controller unplugged", i + 1);
            pads_changed();
        }
    }
}

static void update_pads(bool focused)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    for (unsigned i = 0; i < 4; i++) {
        NcPadState s;
        memset(&s, 0, sizeof(s));
        SDL_Gamepad *pad = player_pad((int)i);
        /* the keyboard: player 1 on auto, any player set to it */
        const bool kb = focused && (g_map.device[i] == -1 || (i == 0 && g_map.device[i] == 0));
        ImPad m;
        im_read(&g_map, (int)i, kb ? keys : NULL, pad, &m);
        s.buttons = m.buttons;
        memcpy(s.analog, m.analog, sizeof(s.analog));
        s.lx = m.lx;
        s.ly = m.ly;
        s.rx = m.rx;
        s.ry = m.ry;
        if (i == 0) {
            nc_auto_pad(&s);
        }
        nc_pad_set(i, &s);
        uint16_t l = 0, r = 0;
        nc_pad_rumble(i, &l, &r);
        if (!g_map.rumble) {
            l = r = 0;
        }
        if (pad && (l != g_rumble[i][0] || r != g_rumble[i][1])) {
            SDL_RumbleGamepad(pad, l, r, l || r ? 1000 : 0);
            g_rumble[i][0] = l;
            g_rumble[i][1] = r;
        }
    }
}

/* ------------------------------------------------------------ states */

/* states\<TITLE ID>.<version>\slot<N>.hxs, its picture slot<N>.bmp beside */
static void state_path(int slot, char *out, size_t n)
{
    char dir[MAX_PATH + 64];
    snprintf(dir, sizeof(dir), "%sstates", g_data_dir);
    mkdir(dir, 0775);
    snprintf(dir, sizeof(dir), "%sstates/%s", g_data_dir, g_key);
    mkdir(dir, 0775);
    snprintf(out, n, "%s/slot%d.hxs", dir, slot);
}

/* The save runs on a thread of its own, as NativeCoreActivity's does: it
 * waits for the GPU to park at a frame, which the window's thread presents
 * meanwhile (from that thread, it waited on itself). */
static SDL_AtomicInt g_saving;
static SDL_AtomicInt g_saved_slot;  /* for the notice: slot + 1, or -1 not now (overlay_tick) */
static void notice(const char *text, const char *after_file);

static void *save_thread(void *arg)
{
    const int slot = (int)(intptr_t)arg;
    char path[MAX_PATH * 2];
    state_path(slot, path, sizeof(path));
    const char *err = nc_state_save(path);
    nc_log("hakuX-app", "savestate %d: %s", slot, err ? err : "saved");
    if (!err) {
        /* its picture: the frame shown, the dashboard's thumbnail */
        char pic[MAX_PATH * 2];
        snprintf(pic, sizeof(pic), "%.*s.bmp", (int)(strlen(path) - 4), path);
        remove(pic);
        hle_present_shot(pic);
        /* the notice after the picture: not in it */
        for (int i = 0; i < 100 && GetFileAttributesA(pic) == INVALID_FILE_ATTRIBUTES; i++) {
            SDL_Delay(10);
        }
    }
    SDL_SetAtomicInt(&g_saved_slot, err ? -1 : slot + 1);
    SDL_SetAtomicInt(&g_saving, 0);
    return NULL;
}

static void save_state(int slot)
{
    if (SDL_GetAtomicInt(&g_core_state) != 1 || !hle_pb_active()) {
        nc_log("hakuX-app", "savestate: not available now");
        SDL_SetAtomicInt(&g_saved_slot, -1);
        return;
    }
    if (!SDL_CompareAndSwapAtomicInt(&g_saving, 0, 1)) {
        return; /* one at a time */
    }
    pthread_t t;
    if (pthread_create(&t, NULL, save_thread, (void *)(intptr_t)slot) == 0) {
        pthread_detach(t);
    } else {
        SDL_SetAtomicInt(&g_saving, 0);
    }
}

static void load_state(int slot)
{
    char path[MAX_PATH * 2], opt[MAX_PATH * 2 + 16];
    state_path(slot, path, sizeof(path));
    const char *err = nc_state_compat(path);
    if (err) {
        nc_log("hakuX-app", "savestate %d: %s", slot, err);
        return;
    }
    snprintf(opt, sizeof(opt), "loadstate=%s", path);
    run_again(opt, true);
}

static bool g_uncap;    /* the run's frame rate setting (fast forward goes back to it) */
static int g_fps_cap;   /* ... its limit, uncapped (0: none) */
static float g_volume = 1.0f;
static bool g_mute_bg = true;   /* silent while the window isn't the active one */

/* screenshots\<title>-<date>.bmp beside the executable */
static void screenshot(void)
{
    char dir[MAX_PATH + 32], p[MAX_PATH + 128];
    snprintf(dir, sizeof(dir), "%sscreenshots", g_data_dir);
    mkdir(dir, 0775);
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(p, sizeof(p), "%s/%s-%04d%02d%02d-%02d%02d%02d.bmp", dir, g_key, t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
    hle_present_shot(p);
    notice(T(S_SHOT_SAVED), p);
}

/* ------------------------------------------------------------ the overlay */

/* The interface is the game's overlay (ui_init_overlay): its presenter
 * composites it over the picture in its own window pass, so the window is
 * never handed to anything else. Up only while something is drawn (the
 * in-game menu, a transition, a notice); otherwise the presenter draws as
 * if there were none. */
static bool g_fx = true;        /* ui.fx: the transitions */
static UiTex *g_hold;           /* the picture the run before this one left (XBASED_HOLD) */
static char g_hold_path[MAX_PATH + 32];
static int g_boot;              /* 0 before the first frame, 1 coming in, 2 done */
static double g_boot_t0;
static bool g_ov_up;            /* an overlay of the window loop's is set */
static char g_notice[200];
static double g_notice_t0;
static char g_notice_file[MAX_PATH + 160];  /* shown once this is written (a picture) */
#ifdef _WIN32
static HANDLE g_ready_ev;       /* the run before this one waits on it (run_again) */
#endif

static void ov_present(void)
{
    if (g_nw.width <= 0 || !nc_present_frame(&g_nw)) {
        SDL_Delay(8);
    }
}

static void ov_size(int *w, int *h)
{
    *w = g_nw.width;
    *h = g_nw.height;
}

static bool ov_ok(void)
{
    return ui_is_overlay() && hle_ov_ready();
}

/* A picture of what the window shows now, for the next run (run_again). */
static void hold_picture(char *path, size_t n)
{
    path[0] = 0;
    if (!ov_ok()) {
        return;
    }
    ui_overlay_off();
    g_ov_up = false;
    snprintf(path, n, "%shandoff.bmp", g_data_dir);
    remove(path);
    hle_present_shot(path);
    for (int i = 0; i < 60 && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES; i++) {
        ov_present();
    }
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        path[0] = 0;
    }
}

/* The next run's window under this one's until it has drawn (its event),
 * it ended, or 15 seconds: this window keeps showing what it shows. */
static void wait_next_run(void *ev, void *proc)
{
#ifdef _WIN32
    const uint64_t until = SDL_GetTicks() + 15000;
    while (SDL_GetTicks() < until) {
        if ((ev && WaitForSingleObject((HANDLE)ev, 0) == WAIT_OBJECT_0) ||
            WaitForSingleObject((HANDLE)proc, 0) == WAIT_OBJECT_0) {
            break;
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
        }
        ov_present();
    }
    if (ev) {
        CloseHandle((HANDLE)ev);
    }
#else
    (void)ev;
    (void)proc;
#endif
}

/* Out of the game: its picture breaks into shards, black behind (what the
 * dashboard's window, or the next run's, comes up on). */
static void fx_out(void)
{
    if (!ov_ok()) {
        return;
    }
    const double t0 = ui_time();
    for (;;) {
        const float k = g_fx ? (float)((ui_time() - t0) / 0.5) : 1.0f;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
        }
        nc_auto_tick();     /* scripted pictures of it too */
        ui_begin();
        ui_cover(true);
        ui_fx_shatter(ui_scene(false), k);
        ui_end();
        if (k >= 1) {
            break;
        }
    }
    g_ov_up = true;     /* black stays up */
}

static void notice(const char *text, const char *after_file)
{
    snprintf(g_notice, sizeof(g_notice), "%s", text);
    snprintf(g_notice_file, sizeof(g_notice_file), "%s", after_file ? after_file : "");
    g_notice_t0 = after_file ? 0 : ui_time();
}

/* Each refresh of the window's loop, before it: the overlay it needs, if
 * any. Before the game's first frame, the picture the run before left (or
 * black); then that picture fading into the game's, or the game's rushing
 * in out of white; a notice over the game a moment. */
static void overlay_tick(void)
{
    if (!ui_is_overlay()) {
        return;
    }
    const int saved = SDL_SetAtomicInt(&g_saved_slot, 0);
    if (saved) {
        char t[200];
        if (saved > 0) {
            snprintf(t, sizeof(t), T(S_STATE_SAVED), saved - 1);
        } else {
            snprintf(t, sizeof(t), "%s", T(S_STATE_NOT_NOW));
        }
        notice(t, NULL);
    }
    if (g_notice_file[0] && GetFileAttributesA(g_notice_file) != INVALID_FILE_ATTRIBUTES) {
        g_notice_file[0] = 0;   /* written: the notice now, not in the picture */
        g_notice_t0 = ui_time();
    }
    const double now = ui_time();
    if (g_boot < 2) {
        const bool shown = nc_present_shown();
        if (!shown && (g_hold || g_fx)) {
            ui_begin();
            if (g_hold) {
                ui_fill(g_hold, UI_WHITE);
                /* a while on: darker, and the console's ring turning */
                const float d = (float)(now - 0.8);
                if (d > 0) {
                    ui_rect(0, 0, ui_w(), ui_h(), (UiColor){0, 0, 0, (uint8_t)(d > 1 ? 110 : 110 * d)});
                    const float a = d > 0.5f ? 1.0f : d * 2;
                    ui_ring(ui_w() - 64, ui_h() - 64, 22, 22, 0, 3, ui_alpha(UI_GREEN_DIM, 0.6f * a));
                    const float r = (float)(now * 5.0);
                    for (int i = 0; i < 3; i++) {
                        ui_circle(ui_w() - 64 + 22 * cosf(r + i * 0.35f), ui_h() - 64 + 22 * sinf(r + i * 0.35f),
                                  3.2f - i * 0.6f, ui_alpha(UI_GREEN_HI, a * (1 - i * 0.25f)));
                    }
                }
            }
            ui_overlay_submit(true);
            g_ov_up = true;
            return;
        }
        if (shown && g_boot == 0) {
            g_boot = 1;
            g_boot_t0 = now;
        }
        const double len = g_hold ? 0.3 : 0.42;
        if (g_boot == 1 && now - g_boot_t0 < len && (g_hold || g_fx)) {
            const float k = (float)((now - g_boot_t0) / len);
            ui_begin();
            if (g_hold) {
                ui_fill(ui_scene(false), UI_WHITE);
                ui_fill(g_hold, ui_alpha(UI_WHITE, 1 - k));
            } else {
                ui_fx_flash_in(ui_scene(false), k, (UiColor){255, 255, 255, 230});
            }
            ui_overlay_submit(true);
            g_ov_up = true;
            return;
        }
        if (shown || !(g_hold || g_fx)) {
            g_boot = 2;
            if (g_hold) {
                ui_texture_free(g_hold);
                g_hold = NULL;
                remove(g_hold_path);
            }
        }
    }
    if (g_notice[0] && g_notice_t0 > 0 && now - g_notice_t0 < 2.4) {
        const float t = (float)(now - g_notice_t0);
        const float a = t < 0.15f ? t / 0.15f : t > 2.0f ? (2.4f - t) / 0.4f : 1.0f;
        ui_begin();
        ui_set_alpha(a);
        const float sz = 22, w = ui_text_width(g_notice, sz, UI_FONT_REGULAR) + 56;
        const float x = (ui_w() - w) / 2, y = ui_h() - 96 + (1 - a) * 12;
        ui_round(x, y, w, 46, 23, (UiColor){0x08, 0x16, 0x05, 0xE0});
        ui_round_outline(x, y, w, 46, 23, 1.5f, ui_alpha(UI_GREEN, 0.7f));
        ui_glow(x + 24, y + 23, 14, ui_alpha(UI_GREEN, 0.8f));
        ui_text(x + 40, y + 10, g_notice, sz, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_LEFT);
        ui_overlay_submit(false);
        g_ov_up = true;
        return;
    }
    if (g_notice[0] && g_notice_t0 > 0) {
        g_notice[0] = 0;
    }
    if (g_ov_up) {
        ui_overlay_off();
        g_ov_up = false;
    }
}

/* What applies at once of the settings (the picture's shape and filters,
 * the volume): the game's over the global ones. */
static void apply_cfg(const Config *global, const Config *game, bool sound)
{
    const Config *pick[2] = {game, global};
    for (int k = 1; k >= 0; k--) {
        const Config *c = pick[k];
        const char *v;
        if ((v = cfg_get(c, "aspect"))) {
            nc_present_set_aspect(!strcmp(v, "4:3") ? NC_ASPECT_4_3 : !strcmp(v, "16:9") ? NC_ASPECT_16_9
                                  : !strcmp(v, "stretch") ? NC_ASPECT_STRETCH : NC_ASPECT_AUTO);
        }
        if ((v = cfg_get(c, "overscan"))) {
            nc_present_set_overscan(atoi(v));
        }
        if ((v = cfg_get(c, "fxaa"))) {
            nc_present_set_fxaa(atoi(v) != 0);
        }
        if ((v = cfg_get(c, "upscale"))) {
            nc_present_set_upscale(nc_upscale_from_name(v));
        }
        if ((v = cfg_get(c, "crt"))) {
            hle_present_set_crt(atoi(v));
        }
        if ((v = cfg_get(c, "showfps"))) {
            nc_present_set_show_fps(atoi(v) != 0);
        }
        if (!sound) {
            continue;
        }
        if ((v = cfg_get(c, "volume"))) {
            g_volume = atoi(v) / 100.0f;
            nc_audio_set_volume(g_volume);
        }
        if ((v = cfg_get(c, "mutebg"))) {
            g_mute_bg = atoi(v) != 0;
        }
    }
    g_fx = cfg_bool(global, "ui.fx", true);
}

/* the menu's settings as they are being changed: the picture under it */
static void live_cfg(const struct Config *global, const struct Config *game)
{
    apply_cfg(global, game, false);
}

static void quit_to_dash(SDL_Window *win)
{
    /* straight out: nothing of the run is taken back first (the
     * presenter's swapchain, the paused threads), so nothing can hold the
     * window */
    nc_log("hakuX-app", "quit to the dashboard");
    give_foreground(true);
    SDL_HideWindow(win);
    exit_now(0);
}

/* The in-game menu: the emulation paused, the menu drawn by the game's own
 * presenter over its picture, blurred (the overlay). Without the overlay:
 * the presenter off the window (its Vulkan surface dropped) while the
 * dashboard's renderer draws the menu over the last frame; then the
 * presenter takes the window back. */
static void ingame_menu(SDL_Window *win, bool *paused, bool minimized)
{
    if (SDL_GetAtomicInt(&g_core_state) != 1) {
        return;
    }
    nc_kernel_set_paused(&g_core.kernel, true);
    nc_audio_set_paused(true);
    int slot = 0;
    const char *title = strrchr(g_disc, '\\');
    if (ov_ok()) {
        ui_overlay_off();
        g_ov_up = false;
        g_notice[0] = 0;
        const int act = dash_ingame(win, g_key, title ? title + 1 : g_disc, NULL, &slot, live_cfg);
        SDL_HideCursor();
        nc_auto_resync();
        if (act == DASH_INGAME_QUIT) {
            fx_out();
            quit_to_dash(win);
        }
        if (act == DASH_INGAME_RESTART) {
            fx_out();
            run_again(NULL, false);
        }
        ui_overlay_off();
        g_ov_up = false;
        nc_kernel_set_paused(&g_core.kernel, *paused || minimized);
        nc_audio_set_paused(*paused || minimized);
        char path[MAX_PATH + 64];
        snprintf(path, sizeof(path), "%sxbased.ini", g_data_dir);
        Config global, game;
        cfg_load(&global, path);
        snprintf(path, sizeof(path), "%sgames/%s.ini", g_data_dir, g_key);
        cfg_load(&game, path);
        im_resolve(&g_map, &global, &game);
        pads_changed();
        apply_cfg(&global, &game, true);
        if (act == DASH_INGAME_SAVE) {
            save_state(slot);
        } else if (act == DASH_INGAME_LOAD) {
            load_state(slot);
        }
        return;
    }
    char pic[MAX_PATH + 32];
    snprintf(pic, sizeof(pic), "%smenu_frame.bmp", g_data_dir);
    remove(pic);
    hle_present_shot(pic);
    for (int i = 0; i < 30 && GetFileAttributesA(pic) == INVALID_FILE_ATTRIBUTES; i++) {
        nc_present_frame(&g_nw);
        SDL_Delay(5);
    }
    nc_present_frame(NULL);     /* the surface goes */
    const int act = dash_ingame(win, g_key, title ? title + 1 : g_disc, pic, &slot, NULL);
    remove(pic);
    if (act == DASH_INGAME_QUIT) {
        quit_to_dash(win);
    }
    SDL_HideCursor();   /* the game doesn't use the mouse; the menu may have shown it */
    nc_auto_resync();
    /* the presenter back on the window before anything is shown or saved */
    SDL_Delay(50);
    nc_present_frame(&g_nw);
    nc_kernel_set_paused(&g_core.kernel, *paused || minimized);
    nc_audio_set_paused(*paused || minimized);
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%sxbased.ini", g_data_dir);
    Config global, game;
    cfg_load(&global, path);
    snprintf(path, sizeof(path), "%sgames/%s.ini", g_data_dir, g_key);
    cfg_load(&game, path);
    im_resolve(&g_map, &global, &game);
    pads_changed();
    apply_cfg(&global, &game, true);
    switch (act) {
    case DASH_INGAME_SAVE:
        save_state(slot);
        break;
    case DASH_INGAME_LOAD:
        load_state(slot);
        break;
    case DASH_INGAME_RESTART:
        run_again(NULL, false);
        break;
    default:
        break;
    }
}

/* debug.hakux.state ("save:<slot>", "load:<slot>"), as the app's menu would */
static void state_request_poll(void)
{
    static char last[PROP_VALUE_MAX];
    static bool primed;
    static unsigned n;
    if (n++ % 30) {
        return;
    }
    char v[PROP_VALUE_MAX] = "";
    __system_property_get("debug.hakux.state", v);
    if (!primed || !strcmp(v, last)) {
        primed = true;
        snprintf(last, sizeof(last), "%s", v);
        return;
    }
    snprintf(last, sizeof(last), "%s", v);
    if (!strncmp(v, "save:", 5)) {
        save_state(atoi(v + 5));
    } else if (!strncmp(v, "load:", 5)) {
        load_state(atoi(v + 5));
    }
}

/* ------------------------------------------------------------- main */

/* xbased.log beside the executable: a run the process started again (a
 * title's relaunch, a state loaded) goes on in the same log. */
static void log_setup(bool console, bool again)
{
    char path[MAX_PATH + 16];
    snprintf(path, sizeof(path), "%sxbased.log", g_data_dir);
#ifdef _WIN32
    if (console && AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    } else {
        freopen(path, again ? "a" : "w", stderr);
    }
#else
    (void)console;
    freopen(path, again ? "a" : "w", stderr);
#endif
    setvbuf(stderr, NULL, _IONBF, 0);
}

/* The file dialog's answer (SDL calls back on a thread of its own). */
static SDL_AtomicInt g_dialog_done;
static char g_dialog_path[1024];

static void SDLCALL dialog_cb(void *ud, const char *const *list, int filter)
{
    if (list && list[0]) {
        snprintf(g_dialog_path, sizeof(g_dialog_path), "%s", list[0]);
    }
    SDL_SetAtomicInt(&g_dialog_done, 1);
}

static const char *pick_disc(void)
{
    static const SDL_DialogFileFilter filters[] = {
        {"Xbox disc images (*.iso, *.xiso)", "iso;xiso"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(dialog_cb, NULL, NULL, filters, 2, NULL, false);
    while (!SDL_GetAtomicInt(&g_dialog_done)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
        }
        SDL_Delay(20);
    }
    return g_dialog_path[0] ? g_dialog_path : NULL;
}

#ifndef XBASED_VERSION
#define XBASED_VERSION "1.0.55"
#endif
#ifdef _WIN32
#define NC_PLATFORM_NAME "Windows"
#else
#define NC_PLATFORM_NAME "Linux"
#endif

int main(int argc, char *argv[])
{
    exe_dir();
    read_ini();
    g_ncfg_args = g_nargs;
    const char *disc = NULL;
    for (int i = 1; i < argc; i++) {
        if (!disc && !strchr(argv[i], '=') && strcmp(argv[i], "uncap") && strcmp(argv[i], "relaunch") &&
            strcmp(argv[i], "fullscreen") && strcmp(argv[i], "console")) {
            disc = argv[i];
        } else {
            add_arg(argv[i]);
            if (g_ncmdline < MAX_ARGS && argv[i][0]) {
                g_cmdline[g_ncmdline++] = strdup(argv[i]);
            }
        }
    }
    bool again = opt_flag("relaunch");
    for (int i = 0; i < g_nargs; i++) {
        again |= !strncmp(g_args[i], "loadstate=", 10);
    }
    log_setup(opt_flag("console"), again);
    if (again) {
        nc_log("hakuX-app", "---- started again (%s)", opt_flag("relaunch") ? "the title's relaunch" : "a state loaded");
    }
    nc_log("hakuX-app", "xbased " XBASED_VERSION " (an original Xbox emulator by crazyriddler, based on hakuX), " NC_PLATFORM_NAME);
    nc_win_mods_cleanup(g_exe_dir);     /* what an update took away (win_mods.h) */

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifndef _WIN32
    /* PulseAudio's API first: PipeWire desktops serve it too, and where
     * PipeWire's own is missing (WSLg) SDL waits half a minute on it */
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "pulseaudio,pipewire,alsa");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fatal_box("xbased", SDL_GetError());
        return 1;
    }
    {
        char db[MAX_PATH + 32];
        snprintf(db, sizeof(db), "%sgamecontrollerdb.txt", g_exe_dir);
        const int n = SDL_AddGamepadMappingsFromFile(db);
        if (n > 0) {
            nc_log("hakuX-app", "%d gamepad mappings from %s", n, db);
        }
    }
    if (!disc || !strcmp(disc, "none")) {
        if (opt_flag("pick")) {
            disc = pick_disc();
            if (!disc) {
                return 0;
            }
        } else {
            /* the dashboard: the library and settings; each game in a process of its own */
            SDL_Window *dw = SDL_CreateWindow("xbased", 1280, 720,
                                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                                  (want_fullscreen() ? SDL_WINDOW_FULLSCREEN : 0));
            if (!dw) {
                fatal_box("xbased", SDL_GetError());
                return 1;
            }
            char exe[MAX_PATH];
            GetModuleFileNameA(NULL, exe, sizeof(exe));
            const int r = dash_run(dw, exe);
            SDL_DestroyWindow(dw);
            SDL_Quit();
            return r;
        }
    }
    snprintf(g_disc, sizeof(g_disc), "%s", disc);
    game_options(g_disc);

    g_disc_fd = open(g_disc, O_RDONLY | O_BINARY);
    if (g_disc_fd < 0) {
        nc_log("hakuX-app", "can't open the disc %s", g_disc);
    }
    char hdd[1024];
    const char *hv = opt_value("hdd");
    if (hv) {
        snprintf(hdd, sizeof(hdd), "%s", hv);
    } else {
        snprintf(hdd, sizeof(hdd), "%shdd", g_data_dir);
    }
    make_hdd(hdd);
    if (g_title_id) {
        /* downloaded content and title updates copied in by hand: signed for this console */
        const int resigned = xi_resign_downloads(hdd, g_title_id);
        if (resigned) {
            nc_log("hakuX-app", "%d downloaded content file(s) of %08x re-signed for this console", resigned, g_title_id);
        }
    }
    nc_io_set_hdd_root(hdd);
    setenv("HAKUX_HLE_MODE", "replace", 0);
    {
        char base[1100];
        snprintf(base, sizeof(base), "%s/../hle", hdd);
        mkdir(base, 0775);
        setenv("HAKUX_NATIVE_BASE", base, 0);
    }

    NcOptions opt;
    nc_options_init(&opt);
    for (int i = 0; i < g_nargs; i++) {
        const char *a = g_args[i];
        char resolved[2400];
        if (!strncmp(a, "xcheat=", 7) && a[7] && a[7] != '/' && a[7] != '\\' && a[8] != ':') {
            /* a cheat file under cheats/ (the shipped ones beside the
             * executable, the user's in the data folder) */
            char p[1300];
            snprintf(p, sizeof(p), "%s", a + 7);
            char *bar = strrchr(p, '|');
            const char *states = bar ? bar + 1 : "";
            if (bar) {
                *bar = 0;
            }
            char full[1400];
            snprintf(full, sizeof(full), "%s%s", g_data_dir, p);
            if (access(full, 0) != 0) {
                snprintf(full, sizeof(full), "%s%s", g_exe_dir, p);
            }
            snprintf(resolved, sizeof(resolved), "xcheat=%s|%s", full, states);
            a = resolved;
        }
        if (nc_option(&opt, a)) {
            continue; /* the core's */
        }
        if (!strncmp(a, "aspect=", 7)) {
            const char *v = a + 7;
            nc_present_set_aspect(!strcmp(v, "4:3")       ? NC_ASPECT_4_3
                                  : !strcmp(v, "16:9")    ? NC_ASPECT_16_9
                                  : !strcmp(v, "stretch") ? NC_ASPECT_STRETCH
                                                          : NC_ASPECT_AUTO);
        } else if (!strncmp(a, "overscan=", 9)) {
            nc_present_set_overscan(atoi(a + 9));
        } else if (!strncmp(a, "fxaa=", 5)) {
            nc_present_set_fxaa(atoi(a + 5) != 0);
        } else if (!strncmp(a, "upscale=", 8)) {
            nc_present_set_upscale(nc_upscale_from_name(a + 8));
        } else if (!strncmp(a, "crt=", 4)) {
            hle_present_set_crt(atoi(a + 4));
        } else if (!strncmp(a, "volume=", 7)) {
            g_volume = atoi(a + 7) / 100.0f;
            nc_audio_set_volume(g_volume);
        } else if (!strncmp(a, "showfps=", 8)) {
            nc_present_set_show_fps(atoi(a + 8) != 0);
        } else if (!strncmp(a, "mutebg=", 7)) {
            g_mute_bg = atoi(a + 7) != 0;
        }
    }
    nc_win_mods_prepare(g_exe_dir, g_disc_fd, g_args, g_nargs);
    nc_options_apply(&opt);
    g_uncap = opt.uncap;
    g_fps_cap = opt.fps_cap;
    nc_present_set_widescreen(opt.video >= 0 && (opt.video & NC_VIDEO_WIDESCREEN));
    nc_log("hakuX-app", "native core: disc %s, hdd %s%s", g_disc, hdd, opt.uncap ? ", frame rate uncapped" : "");

    const char *title = strrchr(g_disc, '\\');
    const char *title2 = strrchr(g_disc, '/');
    title = title2 > title ? title2 : title;
    char wtitle[512];
    snprintf(wtitle, sizeof(wtitle), "xbased - %s", title ? title + 1 : g_disc);
#ifdef _WIN32
    const SDL_WindowFlags vk_flag = 0;            /* the presenter makes its own Win32 surface */
#else
    const SDL_WindowFlags vk_flag = SDL_WINDOW_VULKAN; /* SDL_Vulkan_CreateSurface's (swapchain.c) */
#endif
    SDL_Window *win = SDL_CreateWindow(wtitle, 1280, 960, vk_flag | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                                              (want_fullscreen() ? SDL_WINDOW_FULLSCREEN : 0));
    if (!win) {
        fatal_box("xbased", SDL_GetError());
        return 1;
    }
    SDL_HideCursor();   /* the game doesn't use the mouse */
    {
        /* the run before's window's place (run_again), windowed */
        const char *r = getenv("XBASED_RECT");
        int x, y, w, h;
        if (r && sscanf(r, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0 &&
            !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)) {
            SDL_SetWindowSize(win, w, h);
            SDL_SetWindowPosition(win, x, y);
        }
    }
    SDL_PropertiesID props = SDL_GetWindowProperties(win);
    g_nw.hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    g_nw.hinstance = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, NULL);
    g_nw.sdl_window = win;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    g_nw.width = pw;
    g_nw.height = ph;

    if (!nc_present_init()) {
        return 1;
    }
    {
        /* the interface as the game's overlay: the in-game menu, the
         * transitions, the notices (its fonts beside the executable) */
        char fonts[MAX_PATH + 16];
        snprintf(fonts, sizeof(fonts), "%sfonts/", g_exe_dir);
        if (!ui_init_overlay(fonts, ov_present, ov_size)) {
            nc_log("hakuX-app", "no overlay: the in-game menu on a renderer of its own");
        }
        char path[MAX_PATH + 16];
        snprintf(path, sizeof(path), "%sxbased.ini", g_data_dir);
        Config global;
        cfg_load(&global, path);
        g_fx = cfg_bool(&global, "ui.fx", true);
        const char *hp = getenv("XBASED_HOLD");
        if (hp && hp[0] && ui_is_overlay()) {
            SDL_Surface *sf = SDL_LoadBMP(hp);
            if (sf) {
                g_hold = ui_texture_surface(sf);
                SDL_DestroySurface(sf);
                snprintf(g_hold_path, sizeof(g_hold_path), "%s", hp);
            }
        }
#ifdef _WIN32
        const char *rv = getenv("XBASED_READY");
        if (rv && rv[0]) {
            g_ready_ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, rv);
        }
        SetEnvironmentVariableA("XBASED_READY", NULL);
        SetEnvironmentVariableA("XBASED_HOLD", NULL);
        SetEnvironmentVariableA("XBASED_RECT", NULL);
#else
        unsetenv("XBASED_HOLD");
#endif
    }
    nc_vsync_start();
    nc_auto_init(g_data_dir, save_state, load_state);
#if defined(__x86_64__) || (defined(__aarch64__) && defined(__linux__))
    nc_core_fault_install(&g_core);
#endif
    pads_changed();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_t core;
    pthread_create(&core, &attr, core_thread, NULL);
    pthread_attr_destroy(&attr);

    nc_win_thread_name("window");
    bool quit = false, paused = false, minimized = false, focused = true, told = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.repeat) {
                    break;
                }
                /* the shortcuts (Settings > Shortcuts); Shift with quick save/load: slot 2 */
                if (ev.key.key == g_map.hotkey[IM_HK_FULL] ||
                    (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))) {
                    SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                } else if (ev.key.key == g_map.hotkey[IM_HK_PAUSE]) {
                    paused = !paused;
                    nc_kernel_set_paused(&g_core.kernel, paused || minimized);
                    nc_audio_set_paused(paused || minimized);
                    nc_log("hakuX-app", "%s", paused ? "paused" : "resumed");
                } else if (ev.key.key == g_map.hotkey[IM_HK_SAVE]) {
                    save_state(ev.key.mod & SDL_KMOD_SHIFT ? 2 : 1);
                } else if (ev.key.key == g_map.hotkey[IM_HK_LOAD]) {
                    load_state(ev.key.mod & SDL_KMOD_SHIFT ? 2 : 1);
                } else if (ev.key.key == g_map.hotkey[IM_HK_TURBO]) {
                    nc_win_prop_set("debug.hakux.uncap", "1");  /* frames as fast as they come, while held */
                    nc_win_prop_set("debug.hakux.fpscap", "0");
                } else if (ev.key.key == g_map.hotkey[IM_HK_SHOT]) {
                    screenshot();
                } else if (ev.key.key == g_map.hotkey[IM_HK_MENU]) {
                    ingame_menu(win, &paused, minimized);
                }
                break;
            case SDL_EVENT_KEY_UP:
                if (ev.key.key == g_map.hotkey[IM_HK_TURBO]) {
                    nc_win_prop_set("debug.hakux.uncap", g_uncap ? "1" : "0");
                    char cap[16];
                    snprintf(cap, sizeof(cap), "%d", g_fps_cap);
                    nc_win_prop_set("debug.hakux.fpscap", cap);
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                SDL_Gamepad *gp = SDL_GetGamepadFromID(ev.gbutton.which);
                if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
                    ingame_menu(win, &paused, minimized);
                    break;
                }
                /* EmuDeck-style combos: Select (Back) held + a second button */
                if (gp && SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK)) {
                    switch (ev.gbutton.button) {
                    case SDL_GAMEPAD_BUTTON_START:           /* Select+Start  -> quit */
                        nc_log("xbased", "quit to the dashboard");
                        _exit(0);
                    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:     /* Select+R3     -> menu */
                        ingame_menu(win, &paused, minimized);
                        break;
                    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:  /* Select+R1     -> save state */
                        save_state(1);
                        break;
                    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:   /* Select+L1     -> load state */
                        load_state(1);
                        break;
                    case SDL_GAMEPAD_BUTTON_NORTH:           /* Select+Y      -> screenshot */
                        screenshot();
                        break;
                    case SDL_GAMEPAD_BUTTON_LEFT_STICK:      /* Select+L3     -> pause */
                        paused = !paused;
                        nc_kernel_set_paused(&g_core.kernel, paused || minimized);
                        nc_audio_set_paused(paused || minimized);
                        nc_log("xbased", "%s", paused ? "paused" : "resumed");
                        break;
                    default: break;
                    }
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                /* Select+R2 (right trigger) -> fast-forward while held */
                if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                    SDL_Gamepad *gp = SDL_GetGamepadFromID(ev.gaxis.which);
                    bool want = gp && SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK) &&
                                ev.gaxis.value > 16384;  /* ~half pull */
                    static bool pad_turbo;
                    if (want != pad_turbo) {
                        pad_turbo = want;
                        nc_win_prop_set("debug.hakux.uncap", pad_turbo ? "1" : (g_uncap ? "1" : "0"));
                    }
                }
                break;
            }
            case SDL_EVENT_JOYSTICK_ADDED:
                joystick_added(ev.jdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                pad_added(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                pad_removed(ev.gdevice.which);
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                g_nw.width = ev.window.data1;
                g_nw.height = ev.window.data2;
                break;
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_RESTORED:
                minimized = ev.type == SDL_EVENT_WINDOW_MINIMIZED;
                nc_kernel_set_paused(&g_core.kernel, paused || minimized);
                nc_audio_set_paused(paused || minimized);
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                focused = true;
                nc_audio_set_volume(g_volume);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                focused = false;
                if (g_mute_bg) {
                    nc_audio_set_volume(0);
                }
                break;
            default:
                break;
            }
        }
        nc_auto_tick();
        if (nc_auto_take_menu()) {
            ingame_menu(win, &paused, minimized);
        }
        update_pads(focused);
        state_request_poll();

        const int state = SDL_GetAtomicInt(&g_core_state);
        if (state < 0) {
            if (g_core.kernel.relaunch) {
                run_again("relaunch", true);
            }
            if (g_core.kernel.quit) {
                if (g_core.kernel.quit_no_dash) {
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "xbased", T(S_NO_DASHBOARD), win);
                } else {
                    fx_out();
                }
                break;
            }
            if (!told) {
                told = true;
                const char *why = nc_hle_fatal();
                if (why) {
                    fatal_box("xbased", why);
                    break;
                }
            }
        }
        overlay_tick();
        if (minimized || g_nw.width <= 0 || !nc_present_frame(&g_nw)) {
            SDL_Delay(8); /* nothing to wait for the display on */
        }
#ifdef _WIN32
        if (g_ready_ev && hle_present_count() > 0) {
            SetEvent(g_ready_ev);   /* drawn: the run before may go */
            CloseHandle(g_ready_ev);
            g_ready_ev = NULL;
        }
#endif
    }
    nc_log("hakuX-app", "native core: leaving");
    exit_now(0); /* the core thread has no stop yet */
}
