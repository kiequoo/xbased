/*
 * hakuX front end — xbased's updates (update.h).
 *
 * The latest release comes from GitHub's API through curl, as the box art
 * does (library.c): Windows 10 and 11 ship it, and Linux distributions do.
 * Windows' package is a zip of the portable folder: unpacked with the
 * system's tar into the folder, each file replacing its old copy. The
 * running executable can't be overwritten there, but it can be renamed:
 * it moves aside (UPD_OLD) and goes at the next start. The settings,
 * saves, states and hard disk aren't in the package: they stay as they
 * are. On Linux the AppImage file is replaced (the running one keeps its
 * mount). Then the new version starts and this one quits.
 */
#include "native/app/ui/update.h"
#include "native/app/ui/config.h"
#include "native/core/nc.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#define REPO "kiequoo/xbased-arm64"
#define API "https://api.github.com/repos/" REPO "/releases/latest"
#define UPD_OLD ".xbased-old"
#ifdef _WIN32
#define ASSET_SUFFIX "-windows-x64.zip"
#elif defined(__aarch64__)
#define ASSET_SUFFIX "-aarch64.AppImage"
#else
#define ASSET_SUFFIX "-x86_64.AppImage"
#endif

static struct {
    SDL_AtomicInt state;
    SDL_Thread *thread;
    char current[32];
    char version[32];  /* the release's, without its "v" */
    char page[256];
    char asset[512];   /* its package's download URL */
} U;

static bool run(const char *const *args)
{
    return cfg_run(args);
}

static bool curl(const char *url, const char *to, const char *max_time)
{
    const char *args[] = {"curl", "-sfL", "--max-time", max_time, "-A", "xbased",
                          "-H", "Accept: application/vnd.github+json", "-o", to, url, NULL};
    return run(args);
}

/* "1.0.12" over "1.0.9": the numbers compared one by one */
static bool newer(const char *a, const char *b)
{
    while (*a || *b) {
        const long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) {
            return x > y;
        }
        a += *a == '.';
        b += *b == '.';
        if ((*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9'))) {
            break;
        }
    }
    return false;
}

/* The string value of the key at or after s ("key": "value"), into out. */
static const char *json_str(const char *s, const char *key, char *out, size_t n)
{
    char k[64];
    snprintf(k, sizeof(k), "\"%s\"", key);
    const char *p = strstr(s, k);
    if (!p) {
        return NULL;
    }
    p = strchr(p + strlen(k), '"');
    if (!p) {
        return NULL;
    }
    p++;
    size_t o = 0;
    for (; *p && *p != '"' && o + 1 < n; p++) {
        out[o++] = *p;
    }
    out[o] = 0;
    return p;
}

static void tmp_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s%s", cfg_data_dir(), name);
}

static int check_thread(void *arg)
{
    char js[1200];
    tmp_path(js, sizeof(js), "update.json");
    size_t len = 0;
    char *body = curl(API, js, "15") ? SDL_LoadFile(js, &len) : NULL;
    SDL_RemovePath(js);
    char tag[64] = "";
    if (body && json_str(body, "tag_name", tag, sizeof(tag))) {
        const char *v = tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag;
        snprintf(U.version, sizeof(U.version), "%s", v);
        snprintf(U.page, sizeof(U.page), "https://github.com/" REPO "/releases/tag/%s", tag);
        for (const char *p = body; (p = json_str(p, "browser_download_url", U.asset, sizeof(U.asset)));) {
            const size_t al = strlen(U.asset), sl = strlen(ASSET_SUFFIX);
            if (al > sl && !strcmp(U.asset + al - sl, ASSET_SUFFIX)) {
                break;
            }
            U.asset[0] = 0;
        }
        nc_log("hakuX-app", "update: latest release %s (this is %s)%s", U.version, U.current,
                U.asset[0] ? "" : ", without a package for this platform");
    } else {
        nc_log("hakuX-app", "update: no answer from GitHub");
    }
    SDL_free(body);
    SDL_SetAtomicInt(&U.state, U.version[0] && newer(U.version, U.current) ? UPD_AVAILABLE : UPD_IDLE);
    return 0;
}

void upd_check_start(const char *current)
{
    if (U.thread) {
        return;
    }
    snprintf(U.current, sizeof(U.current), "%s", current);
    SDL_SetAtomicInt(&U.state, UPD_CHECKING);
    U.thread = SDL_CreateThread(check_thread, "update-check", NULL);
    if (!U.thread) {
        SDL_SetAtomicInt(&U.state, UPD_IDLE);
    }
}

UpdState upd_state(void)
{
    return (UpdState)SDL_GetAtomicInt(&U.state);
}

const char *upd_version(void)
{
    return U.version;
}

const char *upd_page(void)
{
    return U.page[0] ? U.page : "https://github.com/" REPO "/releases/latest";
}

/* ------------------------------------------------------------ the install */

#ifdef _WIN32
/* The portable folder, written where it is (cfg_data_dir chose it). */
static const char *install_dir(void)
{
    return !strcmp(cfg_data_dir(), cfg_exe_dir()) ? cfg_exe_dir() : NULL;
}
#else
static const char *appimage(void)
{
    const char *a = SDL_getenv("APPIMAGE");
    return a && *a ? a : NULL;
}
#endif

bool upd_can_install(void)
{
    if (!U.asset[0]) {
        return false;
    }
#ifdef _WIN32
    return install_dir() != NULL;
#else
    return appimage() != NULL;
#endif
}

/* A folder and what it holds, gone. */
static void remove_tree(const char *dir)
{
    int n = 0;
    char **names = SDL_GlobDirectory(dir, NULL, 0, &n);
    for (int i = 0; names && i < n; i++) {
        char p[1200];
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        SDL_PathInfo pi;
        if (SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) {
            remove_tree(p);
        } else {
            SDL_RemovePath(p);
        }
    }
    SDL_free(names);
    SDL_RemovePath(dir);
}

/* The files aside from an update (UPD_OLD), gone, in the folder and below. */
static void remove_old(const char *dir, int depth)
{
    int n = 0;
    char **names = SDL_GlobDirectory(dir, NULL, 0, &n);
    for (int i = 0; names && i < n; i++) {
        char p[1200];
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(p, &pi)) {
            continue;
        }
        const size_t l = strlen(names[i]), ol = strlen(UPD_OLD);
        if (pi.type == SDL_PATHTYPE_DIRECTORY) {
            if (depth < 3) {
                remove_old(p, depth + 1);
            }
        } else if (l > ol && !strcmp(names[i] + l - ol, UPD_OLD)) {
            SDL_RemovePath(p);
        }
    }
    SDL_free(names);
}

void upd_cleanup(void)
{
#ifdef _WIN32
    const char *dir = install_dir();
    if (dir) {
        char d[1100];
        snprintf(d, sizeof(d), "%s", dir);
        if (d[0] && (d[strlen(d) - 1] == '\\' || d[strlen(d) - 1] == '/')) {
            d[strlen(d) - 1] = 0;
        }
        remove_old(d, 0);
        char t[1200];
        snprintf(t, sizeof(t), "%supdate.tmp", dir);
        remove_tree(t);
    }
#else
    const char *a = appimage();
    if (a) {
        char p[1200];
        snprintf(p, sizeof(p), "%s.new", a);
        SDL_RemovePath(p);
    }
#endif
}

#ifdef _WIN32
/* The unpacked folder's files moved over the installed ones (rel: the path
 * below both). A file that can't be removed (the running executable) moves
 * aside first. */
static bool move_over(const char *from, const char *to, const char *rel)
{
    char src[1200], dst[1200];
    snprintf(src, sizeof(src), "%s%s", from, rel);
    snprintf(dst, sizeof(dst), "%s%s", to, rel);
    int n = 0;
    char **names = SDL_GlobDirectory(src, NULL, 0, &n);
    bool ok = names != NULL;
    SDL_CreateDirectory(dst);
    for (int i = 0; ok && i < n; i++) {
        char r[1200], s[1200], d[1200];
        snprintf(r, sizeof(r), "%s/%s", rel, names[i]);
        snprintf(s, sizeof(s), "%s%s", from, r);
        snprintf(d, sizeof(d), "%s%s", to, r);
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(s, &pi)) {
            continue;
        }
        if (pi.type == SDL_PATHTYPE_DIRECTORY) {
            ok = move_over(from, to, r);
            continue;
        }
        if (SDL_GetPathInfo(d, NULL) && !SDL_RemovePath(d)) {
            char aside[1300];
            snprintf(aside, sizeof(aside), "%s" UPD_OLD, d);
            SDL_RemovePath(aside);
            if (!SDL_RenamePath(d, aside)) {
                nc_log("hakuX-app", "update: %s is in use: %s", d, SDL_GetError());
                ok = false;
                break;
            }
        }
        if (!SDL_RenamePath(s, d)) {
            nc_log("hakuX-app", "update: %s couldn't be written: %s", d, SDL_GetError());
            ok = false;
        }
    }
    SDL_free(names);
    return ok;
}

static bool install(void)
{
    const char *dir = install_dir();
    char tmp[1100], zip[1200], unpacked[1200];
    snprintf(tmp, sizeof(tmp), "%supdate.tmp", dir);
    remove_tree(tmp);
    SDL_CreateDirectory(tmp);
    snprintf(zip, sizeof(zip), "%s/xbased.zip", tmp);
    snprintf(unpacked, sizeof(unpacked), "%s/files", tmp);
    SDL_CreateDirectory(unpacked);
    if (!curl(U.asset, zip, "600")) {
        nc_log("hakuX-app", "update: the download failed");
        return false;
    }
    /* Windows' own tar (bsdtar) unpacks zips; not another one on the PATH */
    const char *root = SDL_getenv("SystemRoot");
    char tar[300];
    snprintf(tar, sizeof(tar), "%s\\System32\\tar.exe", root ? root : "C:\\Windows");
    const char *args[] = {tar, "-xf", zip, "-C", unpacked, NULL};
    if (!run(args)) {
        nc_log("hakuX-app", "update: the package couldn't be unpacked");
        return false;
    }
    char exe[1200];
    snprintf(exe, sizeof(exe), "%s/xbased.exe", unpacked);
    if (!SDL_GetPathInfo(exe, NULL)) {
        nc_log("hakuX-app", "update: the package has no xbased.exe");
        return false;
    }
    char to[1100];
    snprintf(to, sizeof(to), "%s", dir);
    if (to[0] && (to[strlen(to) - 1] == '\\' || to[strlen(to) - 1] == '/')) {
        to[strlen(to) - 1] = 0;
    }
    const bool ok = move_over(unpacked, to, "");
    remove_tree(tmp);
    return ok;
}
#else
static bool install(void)
{
    const char *a = appimage();
    char part[1200];
    snprintf(part, sizeof(part), "%s.new", a);
    if (!curl(U.asset, part, "600")) {
        nc_log("hakuX-app", "update: the download failed");
        SDL_RemovePath(part);
        return false;
    }
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(part, &pi) || pi.size < 1024 * 1024) {
        nc_log("hakuX-app", "update: the download is too small for an AppImage");
        SDL_RemovePath(part);
        return false;
    }
    chmod(part, 0755);
    if (!SDL_RenamePath(part, a)) {
        nc_log("hakuX-app", "update: %s couldn't be replaced: %s", a, SDL_GetError());
        SDL_RemovePath(part);
        return false;
    }
    return true;
}
#endif

static int install_thread(void *arg)
{
    const bool ok = install();
    nc_log("hakuX-app", "update: %s %s", ok ? "installed" : "not installed:", ok ? U.version : "the release page instead");
    SDL_SetAtomicInt(&U.state, ok ? UPD_INSTALLED : UPD_FAILED);
    return 0;
}

void upd_install_start(void)
{
    if (U.thread) {
        SDL_WaitThread(U.thread, NULL);
        U.thread = NULL;
    }
    SDL_SetAtomicInt(&U.state, UPD_INSTALLING);
    U.thread = SDL_CreateThread(install_thread, "update-install", NULL);
    if (!U.thread) {
        SDL_SetAtomicInt(&U.state, UPD_FAILED);
    }
}

bool upd_relaunch(void)
{
    char exe[1200];
#ifdef _WIN32
    snprintf(exe, sizeof(exe), "%sxbased.exe", cfg_exe_dir());
#else
    snprintf(exe, sizeof(exe), "%s", appimage());
#endif
    const char *args[] = {exe, NULL};
    SDL_Process *p = SDL_CreateProcess(args, false);
    if (!p) {
        return false;
    }
    SDL_DestroyProcess(p);  /* it runs on */
    return true;
}
