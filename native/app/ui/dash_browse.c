/*
 * hakuX front end — the file browser (dash_browse.c): a screen of the
 * dashboard's own for adding games and picking files, run with a gamepad,
 * the keyboard or the mouse alike. The system's file dialogs it replaces
 * took the mouse or the keyboard only, the gamepad stood still.
 *
 * Two panels: the places (the library's folders, the user's own folders,
 * every drive or mounted file system with its free space) and the folder
 * open (its folders, then the files asked for; the others on Y). A folder
 * is read on a thread of its own, so a slow drive (a USB disk spinning up,
 * a network share) doesn't hold the dashboard.
 */
#include "native/app/ui/dash_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define SEP '\\'
#else
#include <sys/statvfs.h>
#include <unistd.h>
#define SEP '/'
#endif

#define MAX_PLACES 64
#define PATH_MAX_B 1024

enum PlaceKind { PK_LIBRARY, PK_HOME, PK_FOLDER, PK_ROOT, PK_DISK, PK_REMOVABLE, PK_NETWORK, PK_OPTICAL };

typedef struct Place {
    char path[PATH_MAX_B];
    char name[160];
    char sub[160];
    enum PlaceKind kind;
    int section;            /* 0 the library's, 1 folders, 2 drives */
} Place;

typedef struct Entry {
    char *name;
    bool dir;
    bool match;             /* a file of the kind asked for */
    uint64_t size;
} Entry;

/* a folder read on a thread: the answer for gen */
typedef struct Listing {
    Entry *e;
    int n;
    bool failed;
} Listing;

static struct {
    bool games;                     /* adding to the library (else one file picked) */
    char title[160];
    char exts[96];                  /* ";iso;chd;" (lower case), "" any */
    SDL_DialogFileCallback cb;
    void *ud;
    char cwd[PATH_MAX_B];           /* "": no folder open (the places) */
    Place places[MAX_PLACES];
    int nplaces;
    Listing cur;                    /* the folder open, as read */
    bool loading;
    double loading_since;
    bool all;                       /* every file shown, not just the kind asked for */
    int focus;                      /* 0 the places, 1 the folder */
    int psel, pscroll, fsel, fscroll;
    char reselect[PATH_MAX_B];      /* after going up: the folder come from, selected */
    /* the reader */
    SDL_Mutex *mu;
    SDL_AtomicInt gen;
    Listing ready;
    int ready_gen;                  /* -1: nothing ready */
    bool snap;                      /* the selection brought into view once (a folder read) */
} B = {.ready_gen = -1};

/* ------------------------------------------------------------ paths */

static bool is_sep(char c)
{
    return c == '/' || c == '\\';
}

/* no separator at the end, but for a root ("/", "C:\") */
static void tidy(char *p)
{
    size_t n = strlen(p);
#ifdef _WIN32
    for (char *c = p; *c; c++) {
        *c = *c == '/' ? '\\' : *c;
    }
    if (n == 2 && p[1] == ':') {
        p[2] = '\\';
        p[3] = 0;
        return;
    }
    while (n > 3 && is_sep(p[n - 1])) {
        p[--n] = 0;
    }
#else
    while (n > 1 && is_sep(p[n - 1])) {
        p[--n] = 0;
    }
#endif
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    const size_t n = strlen(dir);
    snprintf(out, cap, "%s%s%s", dir, n && is_sep(dir[n - 1]) ? "" : (char[]){SEP, 0}, name);
}

/* The folder above p; "" at a root (the places). */
static void parent(const char *p, char *out, size_t cap)
{
    snprintf(out, cap, "%s", p);
    size_t n = strlen(out);
#ifdef _WIN32
    if (n <= 3) {
        out[0] = 0;
        return;
    }
#else
    if (n <= 1) {
        out[0] = 0;
        return;
    }
#endif
    while (n && !is_sep(out[n - 1])) {
        n--;
    }
    if (!n) {
        out[0] = 0;
        return;
    }
    out[n] = 0;
    tidy(out);
#ifdef _WIN32
    if (out[0] == '\\' && out[1] == '\\' && !strchr(out + 2, '\\')) {
        out[0] = 0;     /* \\server: no folder to show */
    }
#endif
}

static const char *base_name(const char *p)
{
    const char *b = p;
    for (const char *c = p; *c; c++) {
        if (is_sep(*c) && c[1]) {
            b = c + 1;
        }
    }
    return b;
}

static bool same_path(const char *a, const char *b)
{
#ifdef _WIN32
    char x[PATH_MAX_B], y[PATH_MAX_B];
    snprintf(x, sizeof(x), "%s", a);
    snprintf(y, sizeof(y), "%s", b);
    tidy(x);
    tidy(y);
    return !SDL_strcasecmp(x, y);
#else
    char x[PATH_MAX_B], y[PATH_MAX_B];
    snprintf(x, sizeof(x), "%s", a);
    snprintf(y, sizeof(y), "%s", b);
    tidy(x);
    tidy(y);
    return !strcmp(x, y);
#endif
}

/* p is folder or inside it */
static bool inside(const char *p, const char *folder)
{
    char f[PATH_MAX_B];
    snprintf(f, sizeof(f), "%s", folder);
    tidy(f);
    const size_t n = strlen(f);
#ifdef _WIN32
    const bool pre = !SDL_strncasecmp(p, f, n);
#else
    const bool pre = !strncmp(p, f, n);
#endif
    return pre && (!p[n] || is_sep(p[n]) || (n && is_sep(f[n - 1])));
}

static bool ext_match(const char *name)
{
    if (!B.exts[0]) {
        return true;
    }
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1] || strlen(dot) > 12) {
        return false;
    }
    char e[16];
    snprintf(e, sizeof(e), ";%s;", dot + 1);
    for (char *c = e; *c; c++) {
        *c = (char)SDL_tolower((unsigned char)*c);
    }
    return strstr(B.exts, e) != NULL;
}

static void fmt_size(uint64_t b, char *out, size_t cap)
{
    if (b >= 1000ull << 30) {
        snprintf(out, cap, "%.1f TB", b / 1099511627776.0);
    } else if (b >= 10ull << 30) {
        snprintf(out, cap, "%.0f GB", b / 1073741824.0);
    } else if (b >= 1ull << 30) {
        snprintf(out, cap, "%.1f GB", b / 1073741824.0);
    } else if (b >= 1ull << 20) {
        snprintf(out, cap, "%.0f MB", b / 1048576.0);
    } else {
        snprintf(out, cap, "%.0f KB", b / 1024.0 + 0.5);
    }
}

static void space_line(char *out, size_t cap, const char *fs, uint64_t free_b, uint64_t total)
{
    char f[32], t[32], line[96];
    fmt_size(free_b, f, sizeof(f));
    fmt_size(total, t, sizeof(t));
    snprintf(line, sizeof(line), T(S_BR_FREE_OF), f, t);
    snprintf(out, cap, "%s%s%s", fs && fs[0] ? fs : "", fs && fs[0] ? "  ·  " : "", total ? line : "");
}

/* ------------------------------------------------------------ the library */

static bool in_library(const char *p, bool dir)
{
    const char *v[48];
    const int nd = cfg_all(&D.global, "gamedir", v, 16);
    for (int i = 0; i < nd; i++) {
        if (inside(p, v[i])) {
            return true;
        }
    }
    if (!dir) {
        const int nf = cfg_all(&D.global, "gamefile", v, 32);
        for (int i = 0; i < nf; i++) {
            if (same_path(p, v[i])) {
                return true;
            }
        }
    }
    return false;
}

static void places_load(void);

/* what the new folder holds goes: its scan finds them (folders it holds,
 * game files added one by one from it) */
static void drop_inside(const char *key, int max, const char *folder)
{
    const char *v[48];
    char keep[48][PATH_MAX_B];
    const int n = cfg_all(&D.global, key, v, max);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (inside(v[i], folder)) {
            snprintf(keep[k++], PATH_MAX_B, "%s", v[i]);
        }
    }
    for (int i = 0; i < k; i++) {
        cfg_remove_value(&D.global, key, keep[i]);
    }
}

static void add_folder(const char *p)
{
    const char *v[16];
    if (in_library(p, true)) {
        dash_toast(T(S_BR_ALREADY));
        return;
    }
    drop_inside("gamedir", 16, p);
    drop_inside("gamefile", 32, p);
    if (cfg_all(&D.global, "gamedir", v, 16) >= 16) {
        dash_toast(T(S_BR_FULL));
        return;
    }
    cfg_add(&D.global, "gamedir", p);
    cfg_save(&D.global);
    dash_toast(T(S_BR_FOLDER_ADDED));
    char was[PATH_MAX_B] = "";
    if (B.psel >= 0 && B.psel < B.nplaces) {
        snprintf(was, sizeof(was), "%s", B.places[B.psel].path);
    }
    places_load();
    for (int i = 0; i < B.nplaces; i++) {
        if (was[0] && same_path(B.places[i].path, was)) {
            B.psel = i;     /* the same place selected, the list grown above it */
            break;
        }
    }
}

static void add_file(const char *p)
{
    const char *v[32];
    if (in_library(p, false)) {
        dash_toast(T(S_BR_ALREADY));
        return;
    }
    XdGame g;
    memset(&g, 0, sizeof(g));
    const bool ok = xd_read_game(p, &g);
    xd_free_game(&g);
    if (!ok) {
        dash_toast(T(S_NOT_A_GAME));
        return;
    }
    if (cfg_all(&D.global, "gamefile", v, 32) >= 32) {
        dash_toast(T(S_BR_FULL));
        return;
    }
    cfg_add(&D.global, "gamefile", p);
    cfg_save(&D.global);
    dash_toast(T(S_BR_GAME_ADDED));
}

/* ------------------------------------------------------------ the places */

static Place *place_add(const char *path, const char *name, const char *sub, enum PlaceKind kind, int section)
{
    if (B.nplaces >= MAX_PLACES || !path || !path[0]) {
        return NULL;
    }
    char p[PATH_MAX_B];
    snprintf(p, sizeof(p), "%s", path);
    tidy(p);
    for (int i = 0; i < B.nplaces; i++) {
        if (B.places[i].section == section && same_path(B.places[i].path, p)) {
            return NULL;
        }
    }
    Place *pl = &B.places[B.nplaces++];
    snprintf(pl->path, sizeof(pl->path), "%s", p);
    snprintf(pl->name, sizeof(pl->name), "%s", name && name[0] ? name : base_name(p));
    snprintf(pl->sub, sizeof(pl->sub), "%s", sub ? sub : "");
    pl->kind = kind;
    pl->section = section;
    return pl;
}

static bool is_dir(const char *p)
{
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
}

static void user_folder(SDL_Folder f, enum UiStr name, enum PlaceKind kind)
{
    const char *p = SDL_GetUserFolder(f);
    if (p && is_dir(p)) {
        place_add(p, T(name), NULL, kind, 1);
    }
}

#ifdef _WIN32
static void utf8_of(const wchar_t *w, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) {
        out[0] = 0;
    }
}

static void drives(void)
{
    /* no "insert a disc" box for an empty drive */
    const UINT was = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    const DWORD mask = GetLogicalDrives();
    for (int d = 0; d < 26; d++) {
        if (!(mask & (1u << d))) {
            continue;
        }
        wchar_t root[4] = {(wchar_t)('A' + d), ':', '\\', 0};
        char path[8] = {(char)('A' + d), ':', '\\', 0};
        const UINT type = GetDriveTypeW(root);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) {
            continue;
        }
        enum PlaceKind kind = type == DRIVE_REMOVABLE ? PK_REMOVABLE
                            : type == DRIVE_REMOTE    ? PK_NETWORK
                            : type == DRIVE_CDROM     ? PK_OPTICAL
                                                      : PK_DISK;
        enum UiStr kname = kind == PK_REMOVABLE ? S_BR_REMOVABLE
                         : kind == PK_NETWORK   ? S_BR_NETWORK
                         : kind == PK_OPTICAL   ? S_BR_OPTICAL
                                                : S_BR_LOCAL_DISK;
        char label[128] = "", fs[32] = "", name[180], sub[160] = "";
        wchar_t wl[MAX_PATH + 1] = {0}, wf[MAX_PATH + 1] = {0};
        bool ready = true;
        if (kind == PK_NETWORK) {
            /* the share's name; its volume isn't asked (a share gone away hangs) */
            wchar_t rn[MAX_PATH] = {0};
            DWORD len = MAX_PATH;
            wchar_t local[3] = {(wchar_t)('A' + d), ':', 0};
            if (WNetGetConnectionW(local, rn, &len) == NO_ERROR) {
                utf8_of(rn, sub, sizeof(sub));
            }
        } else if (GetVolumeInformationW(root, wl, MAX_PATH, NULL, NULL, NULL, wf, MAX_PATH)) {
            utf8_of(wl, label, sizeof(label));
            utf8_of(wf, fs, sizeof(fs));
            ULARGE_INTEGER fr, tot;
            if (GetDiskFreeSpaceExW(root, &fr, &tot, NULL)) {
                space_line(sub, sizeof(sub), fs, fr.QuadPart, tot.QuadPart);
            } else {
                snprintf(sub, sizeof(sub), "%s", fs);
            }
        } else {
            ready = false;
            snprintf(sub, sizeof(sub), "%s", T(S_BR_NOT_READY));
        }
        snprintf(name, sizeof(name), "%s (%c:)", label[0] ? label : T(kname), 'A' + d);
        place_add(path, name, sub, kind, 2);    /* not ready: still listed, it may be by the time it's opened */
        (void)ready;
    }
    SetErrorMode(was);
}
#else
/* /proc/self/mounts escapes a space as \040 */
static void unescape(char *s)
{
    char *o = s;
    for (char *c = s; *c; c++) {
        if (c[0] == '\\' && c[1] >= '0' && c[1] <= '7' && c[2] >= '0' && c[2] <= '7' && c[3] >= '0' && c[3] <= '7') {
            *o++ = (char)((c[1] - '0') * 64 + (c[2] - '0') * 8 + (c[3] - '0'));
            c += 3;
        } else {
            *o++ = *c;
        }
    }
    *o = 0;
}

static void mount_space(const char *path, const char *fs, char *sub, size_t cap)
{
    struct statvfs v;
    if (statvfs(path, &v) == 0) {
        space_line(sub, cap, fs, (uint64_t)v.f_bavail * v.f_frsize, (uint64_t)v.f_blocks * v.f_frsize);
    } else {
        snprintf(sub, cap, "%s", fs);
    }
}

static void drives(void)
{
    char sub[160];
    mount_space("/", NULL, sub, sizeof(sub));
    place_add("/", T(S_BR_ROOT), sub, PK_ROOT, 2);
    /* the disks a desktop mounts (/media, /run/media), the ones mounted by
     * hand (/mnt), and any other real file system that isn't the system's
     * own (/boot, /usr, /var...) */
    static const char *const k_real[] = {"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "jfs", "reiserfs",
                                         "ntfs", "ntfs3", "fuseblk", "exfat", "vfat", "msdos", "hfsplus",
                                         "udf", "iso9660", "zfs", "nfs", "nfs4", "cifs", "smb3", "9p",
                                         "drvfs", "virtiofs", "fuse.sshfs", "fuse.rclone", "bcachefs", "nilfs2"};
    static const char *const k_system[] = {"/boot", "/efi", "/usr", "/var", "/etc", "/opt", "/srv", "/snap",
                                           "/proc", "/sys", "/dev", "/run", "/tmp", "/root", "/nix", "/gnu"};
    FILE *f = fopen("/proc/self/mounts", "r");
    char line[2048];
    while (f && fgets(line, sizeof(line), f)) {
        char dev[512], mnt[1024], fs[64];
        if (sscanf(line, "%511s %1023s %63s", dev, mnt, fs) != 3) {
            continue;
        }
        unescape(mnt);
        if (!strcmp(mnt, "/")) {
            continue;
        }
        const bool desktop = !strncmp(mnt, "/media/", 7) || !strncmp(mnt, "/run/media/", 11) || !strncmp(mnt, "/mnt/", 5);
        bool real = false, system = false;
        for (size_t i = 0; i < sizeof(k_real) / sizeof(k_real[0]); i++) {
            real |= !strcmp(fs, k_real[i]);
        }
        for (size_t i = 0; i < sizeof(k_system) / sizeof(k_system[0]); i++) {
            const size_t n = strlen(k_system[i]);
            system |= !strncmp(mnt, k_system[i], n) && (!mnt[n] || mnt[n] == '/');
        }
        /* under /media, /run/media and /mnt anything but the kernel's own
         * (a FUSE mount of any kind: an NTFS disk, a cloud drive) */
        static const char *const k_virtual[] = {"tmpfs", "devtmpfs", "ramfs", "proc", "sysfs", "overlay", "squashfs",
                                                "autofs", "cgroup", "cgroup2", "devpts", "mqueue", "debugfs",
                                                "tracefs", "securityfs", "pstore", "bpf", "binfmt_misc", "fusectl",
                                                "configfs", "hugetlbfs", "efivarfs", "nsfs", "rpc_pipefs"};
        bool virt = false;
        for (size_t i = 0; i < sizeof(k_virtual) / sizeof(k_virtual[0]); i++) {
            virt |= !strcmp(fs, k_virtual[i]);
        }
        const bool shown = desktop ? !virt : real && !system;
        if (!shown || access(mnt, R_OK | X_OK) != 0) {
            continue;
        }
        const bool net = !strcmp(fs, "nfs") || !strcmp(fs, "nfs4") || !strcmp(fs, "cifs") || !strcmp(fs, "smb3") ||
                         !strcmp(fs, "fuse.sshfs") || !strcmp(fs, "fuse.rclone");
        const bool optical = !strcmp(fs, "iso9660") || !strcmp(fs, "udf");
        const bool usb = !strncmp(mnt, "/media/", 7) || !strncmp(mnt, "/run/media/", 11);
        if (net) {
            snprintf(sub, sizeof(sub), "%s  ·  %s", fs, dev);     /* a share gone away hangs statvfs */
        } else {
            mount_space(mnt, fs, sub, sizeof(sub));
        }
        place_add(mnt, base_name(mnt), sub, net ? PK_NETWORK : optical ? PK_OPTICAL : usb ? PK_REMOVABLE : PK_DISK, 2);
    }
    if (f) {
        fclose(f);
    }
}
#endif

static void places_load(void)
{
    B.nplaces = 0;
    const char *v[16];
    const int nd = cfg_all(&D.global, "gamedir", v, 16);
    for (int i = 0; i < nd; i++) {
        place_add(v[i], NULL, v[i], PK_LIBRARY, 0);
    }
    user_folder(SDL_FOLDER_HOME, S_BR_HOME, PK_HOME);
    user_folder(SDL_FOLDER_DESKTOP, S_BR_DESKTOP, PK_FOLDER);
    user_folder(SDL_FOLDER_DOCUMENTS, S_BR_DOCUMENTS, PK_FOLDER);
    user_folder(SDL_FOLDER_DOWNLOADS, S_BR_DOWNLOADS, PK_FOLDER);
    drives();
}

/* ------------------------------------------------------------ a folder, read on a thread */

typedef struct Job {
    char path[PATH_MAX_B];
    char exts[96];
    int gen;
    char **names;
    int n, cap;
} Job;

static SDL_EnumerationResult SDLCALL collect(void *ud, const char *dir, const char *name)
{
    Job *j = ud;
    (void)dir;
    if (j->n == j->cap) {
        j->cap = j->cap ? j->cap * 2 : 256;
        char **nn = realloc(j->names, sizeof(*nn) * (size_t)j->cap);
        if (!nn) {
            return SDL_ENUM_FAILURE;
        }
        j->names = nn;
    }
    j->names[j->n++] = SDL_strdup(name);
    return SDL_ENUM_CONTINUE;
}

static int cmp_entry(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->dir != y->dir) {
        return x->dir ? -1 : 1;
    }
    return SDL_strcasecmp(x->name, y->name);
}

static bool hidden(const char *path, const char *name)
{
    /* Windows' own on any disk (a USB stick, a Windows drive from Linux) */
    if (name[0] == '$' || !SDL_strcasecmp(name, "System Volume Information")) {
        return true;
    }
#ifdef _WIN32
    wchar_t w[PATH_MAX_B];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, PATH_MAX_B)) {
        return false;
    }
    const DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
#else
    (void)path;
    return name[0] == '.' || !strcmp(name, "lost+found");
#endif
}

static void listing_free(Listing *l)
{
    for (int i = 0; i < l->n; i++) {
        SDL_free(l->e[i].name);
    }
    free(l->e);
    memset(l, 0, sizeof(*l));
}

static int SDLCALL read_thread(void *arg)
{
    Job *j = arg;
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#endif
    Listing l = {0};
    l.failed = !SDL_EnumerateDirectory(j->path, collect, j);
    l.e = calloc((size_t)(j->n ? j->n : 1), sizeof(Entry));
    for (int i = 0; l.e && i < j->n; i++) {
        char p[PATH_MAX_B * 2];
        join(p, sizeof(p), j->path, j->names[i]);
        SDL_PathInfo pi;
        if (hidden(p, j->names[i]) || !SDL_GetPathInfo(p, &pi) ||
            (pi.type != SDL_PATHTYPE_DIRECTORY && pi.type != SDL_PATHTYPE_FILE)) {
            SDL_free(j->names[i]);
            continue;
        }
        Entry *e = &l.e[l.n++];
        e->name = j->names[i];
        e->dir = pi.type == SDL_PATHTYPE_DIRECTORY;
        e->size = pi.size;
    }
    free(j->names);
    if (l.e) {
        qsort(l.e, (size_t)l.n, sizeof(Entry), cmp_entry);
    }
    SDL_LockMutex(B.mu);
    if (j->gen == SDL_GetAtomicInt(&B.gen)) {
        listing_free(&B.ready);
        B.ready = l;
        B.ready_gen = j->gen;
    } else {
        listing_free(&l);
    }
    SDL_UnlockMutex(B.mu);
    free(j);
    return 0;
}

static void open_dir(const char *path, const char *reselect)
{
    if (!B.mu) {
        B.mu = SDL_CreateMutex();
    }
    snprintf(B.cwd, sizeof(B.cwd), "%s", path);
    tidy(B.cwd);
    snprintf(B.reselect, sizeof(B.reselect), "%s", reselect ? reselect : "");
    listing_free(&B.cur);
    B.fsel = 0;
    B.fscroll = 0;
    if (!B.cwd[0]) {
        B.loading = false;
        B.focus = 0;
        return;
    }
    B.focus = 1;
    B.loading = true;
    B.loading_since = ui_time();
    Job *j = calloc(1, sizeof(*j));
    if (!j) {
        B.loading = false;
        B.cur.failed = true;
        return;
    }
    snprintf(j->path, sizeof(j->path), "%s", B.cwd);
    j->gen = SDL_AddAtomicInt(&B.gen, 1) + 1;
    SDL_Thread *t = SDL_CreateThread(read_thread, "browse", j);
    if (t) {
        SDL_DetachThread(t);
    } else {
        free(j);
        B.loading = false;
        B.cur.failed = true;
    }
    cfg_set(&D.global, "ui.browse", B.cwd);
}

static void poll_ready(void)
{
    if (!B.mu || !B.loading) {
        return;
    }
    SDL_LockMutex(B.mu);
    if (B.ready_gen == SDL_GetAtomicInt(&B.gen)) {
        B.cur = B.ready;
        memset(&B.ready, 0, sizeof(B.ready));
        B.ready_gen = -1;
        B.loading = false;
    }
    SDL_UnlockMutex(B.mu);
    if (!B.loading) {
        for (int i = 0; i < B.cur.n; i++) {
            B.cur.e[i].match = !B.cur.e[i].dir && ext_match(B.cur.e[i].name);
        }
        if (B.reselect[0]) {
            /* rows: 0 the folder above, then the shown entries */
            int row = 1;
            for (int i = 0; i < B.cur.n; i++) {
                const Entry *e = &B.cur.e[i];
                if (!e->dir && !e->match && !B.all) {
                    continue;
                }
                if (e->dir && !strcmp(e->name, B.reselect)) {
                    B.fsel = row;
                    break;
                }
                row++;
            }
            B.reselect[0] = 0;
        }
        B.snap = true;
    }
}

/* ------------------------------------------------------------ opening */

static void start(const char *title, const char *exts, bool games, SDL_DialogFileCallback cb, void *ud)
{
    snprintf(B.title, sizeof(B.title), "%s", title);
    B.exts[0] = 0;
    if (exts && exts[0]) {
        snprintf(B.exts, sizeof(B.exts), ";%s;", exts);
        for (char *c = B.exts; *c; c++) {
            *c = (char)SDL_tolower((unsigned char)*c);
        }
    }
    B.games = games;
    B.cb = cb;
    B.ud = ud;
    B.all = false;
    B.psel = B.pscroll = 0;
    places_load();
    /* where it was last, when that's still there */
    const char *last = cfg_get(&D.global, "ui.browse");
    if (last && last[0] && is_dir(last)) {
        open_dir(last, NULL);
    } else {
        open_dir("", NULL);
    }
    dash_push(SCR_BROWSE);
}

void dash_browse_games(void)
{
    start(T(S_ADD_GAMES), "iso;xiso;cci;chd", true, NULL, NULL);
}

void dash_browse_file(const char *title, const char *exts, SDL_DialogFileCallback cb, void *ud)
{
    start(title, exts, false, cb, ud);
}

/* ------------------------------------------------------------ drawing */

static void icon(float x, float y, float s, int what, UiColor c)
{
    /* what: 0 folder, 1 game file, 2 other file, 3 up, then a place's kind + 10 */
    const UiColor dark = UI_GREEN_DARK;
    switch (what) {
    case 0:
        ui_round(x, y + s * 0.12f, s * 0.46f, s * 0.2f, 3, c);
        ui_round(x, y + s * 0.22f, s, s * 0.64f, 4, c);
        break;
    case 1:
        ui_circle(x + s / 2, y + s / 2, s * 0.46f, c);
        ui_circle(x + s / 2, y + s / 2, s * 0.30f, ui_mix(c, UI_WHITE, 0.35f));
        ui_circle(x + s / 2, y + s / 2, s * 0.12f, dark);
        break;
    case 2:
        ui_round(x + s * 0.18f, y + s * 0.04f, s * 0.64f, s * 0.9f, 4, ui_alpha(c, 0.55f));
        break;
    case 3:
        ui_line(x + s * 0.62f, y + s * 0.18f, x + s * 0.3f, y + s * 0.5f, s * 0.12f, c);
        ui_line(x + s * 0.3f, y + s * 0.5f, x + s * 0.62f, y + s * 0.82f, s * 0.12f, c);
        break;
    case 10 + PK_LIBRARY:
        ui_circle(x + s / 2, y + s / 2, s * 0.46f, c);
        ui_circle(x + s / 2, y + s / 2, s * 0.14f, dark);
        ui_ring(x + s / 2, y + s / 2, s * 0.32f, s * 0.32f, 0, 1.5f, ui_alpha(dark, 0.6f));
        break;
    case 10 + PK_HOME:
        ui_line(x + s * 0.08f, y + s * 0.52f, x + s * 0.5f, y + s * 0.12f, s * 0.11f, c);
        ui_line(x + s * 0.5f, y + s * 0.12f, x + s * 0.92f, y + s * 0.52f, s * 0.11f, c);
        ui_round(x + s * 0.2f, y + s * 0.48f, s * 0.6f, s * 0.42f, 3, c);
        ui_round(x + s * 0.42f, y + s * 0.62f, s * 0.16f, s * 0.28f, 2, dark);
        break;
    case 10 + PK_FOLDER:
        icon(x, y, s, 0, c);
        break;
    case 10 + PK_ROOT:
    case 10 + PK_DISK:
        ui_round(x, y + s * 0.28f, s, s * 0.46f, 5, c);
        ui_circle(x + s * 0.8f, y + s * 0.51f, s * 0.07f, UI_GREEN_HI);
        ui_rect(x + s * 0.12f, y + s * 0.48f, s * 0.42f, s * 0.06f, dark);
        break;
    case 10 + PK_REMOVABLE:
        ui_round(x + s * 0.12f, y + s * 0.3f, s * 0.62f, s * 0.42f, 4, c);
        ui_round(x + s * 0.74f, y + s * 0.38f, s * 0.2f, s * 0.26f, 2, ui_mix(c, UI_WHITE, 0.3f));
        break;
    case 10 + PK_NETWORK:
        ui_round(x + s * 0.1f, y + s * 0.1f, s * 0.8f, s * 0.5f, 5, c);
        ui_rect(x + s * 0.46f, y + s * 0.6f, s * 0.08f, s * 0.2f, c);
        ui_rect(x + s * 0.12f, y + s * 0.8f, s * 0.76f, s * 0.07f, c);
        break;
    case 10 + PK_OPTICAL:
        ui_ring(x + s / 2, y + s / 2, s * 0.42f, s * 0.42f, 0, s * 0.1f, c);
        ui_circle(x + s / 2, y + s / 2, s * 0.1f, c);
        break;
    default:
        break;
    }
}

/* a panel the size of the lists' (list_begin), brighter while focused */
static void panel(float x, float y, float w, float h, bool focused)
{
    ui_round(x - 14, y - 14, w + 28, h + 28, 14, UI_PANEL);
    ui_round_outline(x - 14, y - 14, w + 28, h + 28, 14, focused ? 2 : 1.5f,
                     ui_alpha(focused ? UI_GREEN : UI_GREEN_DIM, focused ? 0.75f : 0.35f));
}

static void sel_bar(float x, float y, float w, float h, bool focused)
{
    const float pulse = focused ? 0.75f + 0.25f * sinf((float)ui_time() * 4) : 0.45f;
    ui_round(x - 6, y + 3, w + 12, h - 6, 8, ui_alpha(UI_GREEN, 0.22f * pulse));
    ui_round_outline(x - 6, y + 3, w + 12, h - 6, 8, 2, ui_alpha(UI_GREEN_HI, 0.8f * pulse));
    ui_rect(x - 6, y + 9, 4, h - 18, focused ? UI_GREEN_HI : UI_GREEN_DIM);
}

static void scroll_bar(float x, float y, float h, int count, int visible, int scroll)
{
    if (count <= visible) {
        return;
    }
    const float bh = h * visible / count, by = y + (h - bh) * scroll / (count - visible);
    ui_round(x, y, 4, h, 2, ui_alpha(UI_GREEN, 0.15f));
    ui_round(x, by, 4, bh, 2, ui_alpha(UI_GREEN, 0.8f));
}

/* keeps sel in [0, n) and on screen, scroll in range */
static void keep_visible(int *sel, int *scroll, int n, int visible, bool moved)
{
    if (*sel >= n) {
        *sel = n - 1;
    }
    if (*sel < 0) {
        *sel = 0;
    }
    if (moved) {
        if (*sel < *scroll) {
            *scroll = *sel;
        }
        if (*sel >= *scroll + visible) {
            *scroll = *sel - visible + 1;
        }
    }
    if (*scroll > n - visible) {
        *scroll = n - visible;
    }
    if (*scroll < 0) {
        *scroll = 0;
    }
}

/* the shown rows of the folder: -1 the folder above, else an entry */
static int rows_of(int *map, int cap)
{
    int n = 0;
    map[n++] = -1;
    for (int i = 0; i < B.cur.n && n < cap; i++) {
        if (B.cur.e[i].dir || B.cur.e[i].match || B.all) {
            map[n++] = i;
        }
    }
    return n;
}

static void go_up(void)
{
    char up[PATH_MAX_B];
    parent(B.cwd, up, sizeof(up));
    char from[PATH_MAX_B];
    snprintf(from, sizeof(from), "%s", base_name(B.cwd));
    if (!up[0]) {
        /* a root: the places, with it selected */
        const char *was = B.cwd;
        for (int i = 0; i < B.nplaces; i++) {
            if (same_path(B.places[i].path, was)) {
                B.psel = i;
            }
        }
    }
    open_dir(up, up[0] ? from : NULL);
}

static void choose_file(const char *path)
{
    if (B.games) {
        add_file(path);
        return;
    }
    SDL_DialogFileCallback cb = B.cb;
    void *ud = B.ud;
    dash_pop();
    if (cb) {
        const char *list[2] = {path, NULL};
        cb(ud, list, 0);
    }
}

void scr_browse(void)
{
    const float W = ui_w(), H = ui_h();
    poll_ready();
    draw_header(B.title, B.cwd[0] ? B.cwd : T(S_BR_PLACES));
    const bool asked = D.ask != NULL;
    In in = D.in;
    if (asked) {
        memset(&in, 0, sizeof(in));
        in.mx = in.my = -1;
    }

    const float top = 196, bottom = H - 104;
    const float px = 74, pw = 330, fx = px + pw + 48, fw = W - fx - 74;
    const float prow = 58, frow = 54;

    /* the bar over the folder: its name, and adding it */
    const bool can_add_dir = B.games && B.cwd[0];
    const bool dir_in = can_add_dir && in_library(B.cwd, true);
    float bx = 0, bw = 0;
    {
        const char *name = B.cwd[0] ? base_name(B.cwd) : T(S_BR_PLACES);
        ui_text_fit(fx, 134, name, 28, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT, fw * 0.55f);
        if (dir_in) {
            ui_text(fx + fw, 140, T(S_BR_IN_LIBRARY), 22, UI_FONT_REGULAR, UI_GREEN, UI_ALIGN_RIGHT);
        } else if (can_add_dir) {
            const char *t = T(S_BR_ADD_THIS);
            bw = ui_text_width(t, 22, UI_FONT_BOLD) + 74;
            bx = fx + fw - bw;
            const bool over = in.mx >= bx && in.mx < bx + bw && in.my >= 128 && in.my < 172;
            ui_round(bx, 128, bw, 44, 22, over ? (UiColor){0x3C, 0x9A, 0x12, 0xFF} : (UiColor){0x10, 0x2C, 0x07, 0xE0});
            ui_round_outline(bx, 128, bw, 44, 22, over ? 2 : 1, over ? UI_GREEN_HI : UI_GREEN_DIM);
            draw_button_glyph(bx + 12, 135, 2, 30);
            ui_text(bx + bw - 18, 138, t, 22, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_RIGHT);
            if (over && in.click) {
                in.click = false;
                in.x = true;
            }
        }
    }

    /* ---- the places */
    panel(px, top, pw, bottom - top, B.focus == 0);
    const int pvis = (int)((bottom - top) / prow);
    {
        bool moved = false;
        if (B.focus == 0 && B.nplaces) {
            if (in.up) {
                B.psel = (B.psel + B.nplaces - 1) % B.nplaces;
                moved = true;
            }
            if (in.down) {
                B.psel = (B.psel + 1) % B.nplaces;
                moved = true;
            }
            if (in.lb || in.rb) {
                B.psel += in.lb ? -pvis : pvis;
                B.psel = B.psel < 0 ? 0 : B.psel >= B.nplaces ? B.nplaces - 1 : B.psel;
                moved = true;
            }
        }
        if (in.wheel && in.mx >= px && in.mx < px + pw && in.my >= top && in.my < bottom) {
            B.pscroll -= (int)in.wheel;
        }
        keep_visible(&B.psel, &B.pscroll, B.nplaces, pvis, moved);
        ui_clip(px - 12, top - 4, pw + 24, bottom - top + 8);
        for (int r = 0; r < pvis && B.pscroll + r < B.nplaces; r++) {
            const int i = B.pscroll + r;
            const Place *pl = &B.places[i];
            const float y = top + r * prow;
            const bool over = in.mx >= px && in.mx < px + pw && in.my >= y && in.my < y + prow;
            if (over && in.moved) {
                B.psel = i;
                B.focus = 0;
            }
            const bool s = B.psel == i;
            if (s) {
                sel_bar(px, y, pw, prow, B.focus == 0);
            }
            if (i > 0 && B.places[i - 1].section != pl->section) {
                ui_rect(px + 8, y + 1, pw - 16, 1, ui_alpha(UI_GREEN, 0.25f));
            }
            icon(px + 8, y + 15, 28, 10 + pl->kind, s ? UI_GREEN_HI : UI_GREEN);
            ui_text_fit(px + 48, pl->sub[0] ? y + 6 : y + 15, pl->name, 22, s ? UI_FONT_BOLD : UI_FONT_REGULAR,
                        s ? UI_WHITE : UI_GREY, UI_ALIGN_LEFT, pw - 56);
            ui_text_fit(px + 48, y + 32, pl->sub, 15, UI_FONT_REGULAR, ui_alpha(UI_GREY, 0.85f), UI_ALIGN_LEFT, pw - 56);
            if ((over && in.click) || (s && B.focus == 0 && in.ok)) {
                in.click = in.ok = false;
                B.psel = i;
                open_dir(pl->path, NULL);
            }
        }
        ui_unclip();
        scroll_bar(px + pw + 6, top, bottom - top, B.nplaces, pvis, B.pscroll);
    }

    /* ---- the folder */
    panel(fx, top, fw, bottom - top, B.focus == 1);
    const int fvis = (int)((bottom - top) / frow);
    static int map[65536];
    int nrows = 0;
    if (!B.cwd[0]) {
        ui_text(fx + fw / 2, top + (bottom - top) / 2 - 30, T(S_BR_PICK_PLACE), 24, UI_FONT_REGULAR, UI_GREY,
                UI_ALIGN_CENTER);
    } else if (B.loading) {
        nrows = 1;
        map[0] = -1;
        if (ui_time() - B.loading_since > 0.15) {
            const float cx = fx + fw / 2, cy = top + (bottom - top) / 2;
            ui_ring(cx, cy - 20, 22, 22, (float)ui_time() * 5, 4, UI_GREEN);
            ui_text(cx, cy + 14, T(S_BR_READING), 22, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_CENTER);
        }
    } else {
        nrows = rows_of(map, (int)(sizeof(map) / sizeof(map[0])));
    }
    if (nrows) {
        bool moved = false;
        if (B.focus == 1) {
            if (in.up) {
                B.fsel = (B.fsel + nrows - 1) % nrows;
                moved = true;
            }
            if (in.down) {
                B.fsel = (B.fsel + 1) % nrows;
                moved = true;
            }
            if (in.lb || in.rb) {
                B.fsel += in.lb ? -fvis : fvis;
                B.fsel = B.fsel < 0 ? 0 : B.fsel >= nrows ? nrows - 1 : B.fsel;
                moved = true;
            }
        }
        if (in.wheel && in.mx >= fx && in.mx < fx + fw && in.my >= top && in.my < bottom) {
            B.fscroll -= (int)in.wheel;
        }
        keep_visible(&B.fsel, &B.fscroll, nrows, fvis, moved || B.snap);
        B.snap = false;
        ui_clip(fx - 12, top - 4, fw + 24, bottom - top + 8);
        for (int r = 0; r < fvis && B.fscroll + r < nrows; r++) {
            const int row = B.fscroll + r, ei = map[row];
            const float y = top + r * frow;
            const bool over = in.mx >= fx && in.mx < fx + fw && in.my >= y && in.my < y + frow;
            if (over && in.moved) {
                B.fsel = row;
                B.focus = 1;
            }
            const bool s = B.fsel == row;
            if (s) {
                sel_bar(fx, y, fw, frow, B.focus == 1);
            }
            const UiColor ic = s ? UI_GREEN_HI : UI_GREEN;
            char path[PATH_MAX_B * 2] = "";
            if (ei < 0) {
                icon(fx + 8, y + 13, 28, 3, ic);
                ui_text(fx + 50, y + 13, T(S_BR_UP), 24, s ? UI_FONT_BOLD : UI_FONT_REGULAR, s ? UI_WHITE : UI_GREY,
                        UI_ALIGN_LEFT);
            } else {
                const Entry *e = &B.cur.e[ei];
                join(path, sizeof(path), B.cwd, e->name);
                const bool lib = B.games && !dir_in && (e->dir || e->match) && in_library(path, e->dir);
                icon(fx + 8, y + 13, 28, e->dir ? 0 : e->match ? 1 : 2, e->dir || e->match ? ic : UI_GREY);
                float right = fw - 12;
                if (!e->dir) {
                    char sz[32];
                    fmt_size(e->size, sz, sizeof(sz));
                    right -= ui_text(fx + right, y + 15, sz, 20, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_RIGHT) + 18;
                }
                if (lib) {
                    right -= ui_text(fx + right, y + 15, T(S_BR_IN_LIBRARY), 18, UI_FONT_REGULAR, UI_GREEN,
                                     UI_ALIGN_RIGHT) + 18;
                }
                ui_text_fit(fx + 50, y + 13, e->name, 24, s ? UI_FONT_BOLD : UI_FONT_REGULAR,
                            s ? UI_WHITE : e->dir || e->match ? UI_GREY : ui_alpha(UI_GREY, 0.6f), UI_ALIGN_LEFT,
                            right - 54);
            }
            if ((over && in.click) || (s && B.focus == 1 && in.ok)) {
                in.click = in.ok = false;
                B.fsel = row;
                if (ei < 0) {
                    go_up();
                    break;
                } else if (B.cur.e[ei].dir) {
                    open_dir(path, NULL);
                    break;
                } else {
                    choose_file(path);
                    if (dash_top() != SCR_BROWSE) {
                        return;
                    }
                }
            }
        }
        ui_unclip();
        scroll_bar(fx + fw + 6, top, bottom - top, nrows, fvis, B.fscroll);
        if (!B.loading && nrows == 1) {
            ui_text(fx + fw / 2, top + (bottom - top) / 2 - 10,
                    B.cur.failed ? T(S_BR_NO_ACCESS) : T(S_BR_EMPTY), 22, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_CENTER);
        }
    }

    /* ---- the rest of the buttons */
    if (in.left && B.focus == 1) {
        B.focus = 0;
    } else if (in.right && B.focus == 0 && B.cwd[0]) {
        B.focus = 1;
    }
    if (in.x && can_add_dir && !B.loading) {
        add_folder(B.cwd);
    }
    if (in.y && B.exts[0]) {
        B.all = !B.all;
        B.fsel = B.fsel > 0 ? 0 : B.fsel;
    }
    if (in.back) {
        if (B.focus == 1 && B.cwd[0]) {
            go_up();
        } else {
            dash_pop();
            return;
        }
    }

    /* the hints: what A does where it is */
    const char *a = T(S_SELECT);
    if (B.focus == 1 && B.cwd[0] && nrows && B.fsel < nrows && map[B.fsel] >= 0 && !B.loading) {
        const Entry *e = &B.cur.e[map[B.fsel]];
        a = e->dir ? T(S_BR_OPEN) : B.games ? T(S_BR_ADD) : T(S_SELECT);
    } else {
        a = T(S_BR_OPEN);
    }
    draw_hints(a, T(S_BACK), can_add_dir && !dir_in ? T(S_BR_ADD_THIS) : NULL,
               B.exts[0] ? (B.all ? T(S_BR_FEWER_FILES) : T(S_BR_ALL_FILES)) : NULL);
}
