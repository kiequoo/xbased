/*
 * hakuX front end — a game's desktop shortcut (shortcut.h).
 *
 * The command it runs is the one front ends and Steam's non-Steam games
 * take too: xbased "<disc image>" boots the game without the dashboard.
 */
#include "native/app/ui/shortcut.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the name as a file name: no characters Windows or the desktop refuse */
static void file_name(const char *name, char *out, size_t n)
{
    size_t o = 0;
    for (const char *s = name; *s && o + 1 < n; s++) {
        const char c = *s;
        out[o++] = strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32 ? ' ' : c;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) {
        o--;
    }
    out[o] = 0;
    if (!o) {
        snprintf(out, n, "xbased game");
    }
}

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

/* UTF-8 to UTF-16, with Windows' own path separators */
static void wide(const char *s, wchar_t *out, int n)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n)) {
        out[0] = 0;
    }
    for (wchar_t *c = out; *c; c++) {
        *c = *c == L'/' ? L'\\' : *c;
    }
}

bool sc_make(const char *name, const char *game, const char *icon_png, const char *icon_ico, char *where,
             size_t n)
{
    (void)icon_png;
    wchar_t exe[MAX_PATH], dir[MAX_PATH], args[2100], gw[2048], icon[MAX_PATH], lnk[MAX_PATH + 300];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        return false;
    }
    wcscpy(dir, exe);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
    }
    wide(game, gw, 2048);
    _snwprintf(args, 2100, L"\"%ls\"", gw);
    args[2099] = 0;
    PWSTR desk = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Desktop, 0, NULL, &desk))) {
        return false;
    }
    char fname[300];
    file_name(name, fname, sizeof(fname));
    wchar_t wname[300];
    wide(fname, wname, 300);
    _snwprintf(lnk, MAX_PATH + 300, L"%ls\\%ls.lnk", desk, wname);
    lnk[MAX_PATH + 299] = 0;
    CoTaskMemFree(desk);
    const HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IShellLinkW *sl = NULL;
    bool ok = SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl));
    if (ok) {
        IShellLinkW_SetPath(sl, exe);
        IShellLinkW_SetArguments(sl, args);
        IShellLinkW_SetWorkingDirectory(sl, dir);
        wchar_t desc[300];
        wide(name, desc, 300);
        IShellLinkW_SetDescription(sl, desc);
        if (icon_ico) {
            wide(icon_ico, icon, MAX_PATH);
            IShellLinkW_SetIconLocation(sl, icon, 0);
        } else {
            IShellLinkW_SetIconLocation(sl, exe, 0);
        }
        IPersistFile *pf = NULL;
        ok = SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf));
        if (ok) {
            ok = SUCCEEDED(IPersistFile_Save(pf, lnk, TRUE));
            IPersistFile_Release(pf);
        }
        IShellLinkW_Release(sl);
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    if (ok && where) {
        WideCharToMultiByte(CP_UTF8, 0, lnk, -1, where, (int)n, NULL, NULL);
    }
    return ok;
}

#else
#include <sys/stat.h>
#include <unistd.h>

/* a desktop entry's Exec argument, quoted: ", `, $ and \ escaped */
static void exec_arg(const char *s, char *out, size_t n)
{
    size_t o = 0;
    out[o++] = '"';
    for (; *s && o + 4 < n; s++) {
        if (strchr("\"`$\\", *s)) {
            out[o++] = '\\';
        }
        out[o++] = *s;
    }
    out[o++] = '"';
    out[o] = 0;
}

static bool write_entry(const char *path, const char *name, const char *exe, const char *game, const char *icon)
{
    char qe[1200], qg[2200];
    exec_arg(exe, qe, sizeof(qe));
    exec_arg(game, qg, sizeof(qg));
    FILE *f = fopen(path, "w");
    if (!f) {
        return false;
    }
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nComment=Play it in xbased\nExec=%s %s\nIcon=%s\n"
               "Terminal=false\nCategories=Game;Emulator;\n",
            name, qe, qg, icon && *icon ? icon : "xbased");
    fclose(f);
    chmod(path, 0755);
    return true;
}

bool sc_make(const char *name, const char *game, const char *icon_png, const char *icon_ico, char *where,
             size_t n)
{
    (void)icon_ico;
    char exe[1024];
    const char *ai = getenv("APPIMAGE");     /* the AppImage, not its mounted copy */
    if (ai && *ai) {
        snprintf(exe, sizeof(exe), "%s", ai);
    } else {
        const ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (k <= 0) {
            return false;
        }
        exe[k] = 0;
    }
    const char *home = getenv("HOME");
    if (!home) {
        return false;
    }
    char desk[1024] = "";
    FILE *p = popen("xdg-user-dir DESKTOP 2>/dev/null", "r");
    if (p) {
        if (fgets(desk, sizeof(desk), p)) {
            desk[strcspn(desk, "\r\n")] = 0;
        }
        pclose(p);
    }
    if (!desk[0]) {
        snprintf(desk, sizeof(desk), "%s/Desktop", home);
    }
    char fname[300], path[1400], apps[1400];
    file_name(name, fname, sizeof(fname));
    snprintf(path, sizeof(path), "%s/%s.desktop", desk, fname);
    mkdir(desk, 0755);
    const bool ok = write_entry(path, name, exe, game, icon_png);
    /* and in the applications menu */
    snprintf(apps, sizeof(apps), "%s/.local/share/applications", home);
    mkdir(apps, 0755);
    char made[1400];
    snprintf(made, sizeof(made), "%s/xbased-%s.desktop", apps, fname);
    for (char *c = made + strlen(apps) + 1; *c; c++) {
        *c = *c == ' ' ? '-' : *c;
    }
    write_entry(made, name, exe, game, icon_png);
    if (ok && where) {
        snprintf(where, n, "%s", path);
    }
    return ok;
}
#endif
