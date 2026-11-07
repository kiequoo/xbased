/*
 * hakuX front end — the game library (library.h).
 *
 * The cache (<data>/covers/library.txt, cache_load): a line a file, so the
 * library shows at once and a scan reads only new or changed images; their
 * title icons (<key>.icon.png) beside. Box
 * art (<key>.png) comes from libretro-thumbnails' list of names, matched
 * to the game's title by its words, preferring the disc's region.
 */
#include "native/app/ui/library.h"
#include "native/app/ui/config.h"
#include "native/app/ui/ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

#define MAX_GAMES 16384     /* files: the games and their second copies */
#define HASH_SIZE 32768     /* by_path's slots (a power of two, over MAX_GAMES) */
#define THUMBS "https://raw.githubusercontent.com/libretro-thumbnails/Microsoft_-_Xbox/master/Named_Boxarts/"
#define TREE "https://api.github.com/repos/libretro-thumbnails/Microsoft_-_Xbox/git/trees/master?recursive=1"

struct ScanArgs;

static struct {
    SDL_Mutex *mu;
    LibGame *games[MAX_GAMES];      /* every entry, by title */
    int n;
    LibGame *by_path[HASH_SIZE];    /* the same, by file */
    SDL_Thread *scan, *fetch;
    SDL_AtomicInt scanning, fetching, stop, restart, busy;
    struct ScanArgs *pending;       /* a scan asked for while one ran */
    unsigned pass;                  /* the scan's, counted */
    bool loaded;                    /* the cache's games in */
    char status[160];
    LibGame *queue[MAX_GAMES];      /* box art to fetch */
    int nq;
} L;

/* strsep's: the text up to the next delimiter, *s past it (NULL at the end) */
static char *tok(char **s, const char *delim)
{
    char *t = *s;
    if (!t) {
        return NULL;
    }
    char *e = t + strcspn(t, delim);
    *s = *e ? e + 1 : NULL;
    *e = 0;
    return t;
}

static void covers_dir(char *out, size_t n)
{
    snprintf(out, n, "%scovers", cfg_data_dir());
    SDL_CreateDirectory(out);
}

static bool is_image_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!SDL_strcasecmp(dot, ".iso") || !SDL_strcasecmp(dot, ".xiso") || !SDL_strcasecmp(dot, ".cci") ||
                   !SDL_strcasecmp(dot, ".chd"));
}

static void icon_path(const XdGame *g, char *out, size_t n)
{
    char d[1100];
    covers_dir(d, sizeof(d));
    snprintf(out, n, "%s/%s.icon.png", d, g->key);
}

static void cover_path(const XdGame *g, char *out, size_t n)
{
    char d[1100];
    covers_dir(d, sizeof(d));
    snprintf(out, n, "%s/%s.png", d, g->key);
}

/* ------------------------------------------------------------ the user's */

/* custom.txt: a line a game the user changed, its key, 1 if hidden (else
 * 0), its name ("" for the disc's), then 1 if a favorite and when it was
 * last played (a time_t; both left out by older versions) */
typedef struct {
    char key[24];
    bool hidden, fav;
    char name[128];
    int64_t played;
} Custom;

static Custom *g_custom;
static int g_ncustom;
static bool g_custom_loaded, g_custom_broken, g_show_hidden = true;   /* the Games screen sets it while it draws */

static void custom_path(char *out, size_t n)
{
    char d[1100];
    covers_dir(d, sizeof(d));
    snprintf(out, n, "%s/custom.txt", d);
}

/* L.mu held */
static void custom_load(void)
{
    if (g_custom_loaded) {
        return;
    }
    g_custom_loaded = true;
    g_custom = calloc(MAX_GAMES, sizeof(Custom));
    char p[1200];
    custom_path(p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f || !g_custom) {
        if (f) {
            fclose(f);
        }
        /* there but not readable (in use, say): left alone, not written over */
        g_custom_broken = !g_custom || SDL_GetPathInfo(p, NULL);
        return;
    }
    char line[400];
    while (g_ncustom < MAX_GAMES && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line, *key = tok(&s, "\t"), *hid = tok(&s, "\t"), *name = tok(&s, "\t");
        char *fav = tok(&s, "\t"), *played = tok(&s, "\t");
        if (!key || !*key || !hid) {
            continue;
        }
        Custom *c = &g_custom[g_ncustom++];
        snprintf(c->key, sizeof(c->key), "%s", key);
        c->hidden = *hid == '1';
        snprintf(c->name, sizeof(c->name), "%s", name ? name : "");
        c->fav = fav && *fav == '1';
        c->played = played ? strtoll(played, NULL, 10) : 0;
    }
    fclose(f);
}

/* L.mu held */
static void custom_save(void)
{
    if (g_custom_broken) {
        return;
    }
    char p[1200], tmp[1210];
    custom_path(p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.new", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return;
    }
    for (int i = 0; i < g_ncustom; i++) {
        const Custom *c = &g_custom[i];
        if (c->hidden || c->name[0] || c->fav || c->played) {
            fprintf(f, "%s\t%d\t%s\t%d\t%lld\n", c->key, c->hidden ? 1 : 0, c->name, c->fav ? 1 : 0,
                    (long long)c->played);
        }
    }
    /* whole, then in the old one's place: a crash halfway keeps the old */
    if (fclose(f) == 0) {
        remove(p);
        rename(tmp, p);
    } else {
        remove(tmp);
    }
}

/* L.mu held: the user's for key (made if want) */
static Custom *custom_of(const char *key, bool want)
{
    custom_load();
    for (int i = 0; i < g_ncustom; i++) {
        if (!strcmp(g_custom[i].key, key)) {
            return &g_custom[i];
        }
    }
    if (!want || !g_custom || g_ncustom >= MAX_GAMES) {
        return NULL;
    }
    Custom *c = &g_custom[g_ncustom++];
    memset(c, 0, sizeof(*c));
    snprintf(c->key, sizeof(c->key), "%s", key);
    return c;
}

/* L.mu held: a game as the user left it */
static void custom_apply(LibGame *g)
{
    const Custom *c = custom_of(g->g.key, false);
    g->hidden = c && c->hidden;
    g->fav = c && c->fav;
    g->played = c ? c->played : 0;
    if (c && c->name[0]) {
        if (!g->orig[0]) {
            snprintf(g->orig, sizeof(g->orig), "%s", g->g.title);
        }
        snprintf(g->g.title, sizeof(g->g.title), "%s", c->name);
    }
}

/* ------------------------------------------------------------ the entries */

int lib_title_group(const char *t)
{
    for (; *t; t++) {
        const unsigned char c = (unsigned char)*t;
        if (c < 0x80 && isalpha(c)) {
            return toupper(c) - 'A' + 1;
        }
        if (c >= 0x80 || isdigit(c)) {
            return 0;
        }
        /* spaces, quotes and the like before the name: past them */
    }
    return 0;
}

/* numbers and the rest first, then A to Z; the same name: by file */
static int cmp_title(const void *a, const void *b)
{
    const LibGame *x = *(LibGame *const *)a, *y = *(LibGame *const *)b;
    const int gx = lib_title_group(x->g.title), gy = lib_title_group(y->g.title);
    if (gx != gy) {
        return gx - gy;
    }
    const int c = SDL_strcasecmp(x->g.title, y->g.title);
    return c ? c : strcmp(x->g.path, y->g.path);
}

static unsigned path_hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (unsigned char)*s) * 16777619u;
    }
    return h;
}

/* L.mu held: the entry of a file */
static LibGame *by_path(const char *path)
{
    for (unsigned h = path_hash(path) & (HASH_SIZE - 1);; h = (h + 1) & (HASH_SIZE - 1)) {
        LibGame *g = L.by_path[h];
        if (!g || !strcmp(g->g.path, path)) {
            return g;
        }
    }
}

/* L.mu held: g in its place by title */
static void insert_sorted(LibGame *g)
{
    int lo = 0, hi = L.n;
    while (lo < hi) {
        const int m = (lo + hi) / 2;
        if (cmp_title(&L.games[m], &g) < 0) {
            lo = m + 1;
        } else {
            hi = m;
        }
    }
    memmove(&L.games[lo + 1], &L.games[lo], (size_t)(L.n - lo) * sizeof(L.games[0]));
    L.games[lo] = g;
    L.n++;
}

/* L.mu held: a new entry (L.n < MAX_GAMES) */
static void insert(LibGame *g)
{
    insert_sorted(g);
    unsigned h = path_hash(g->g.path) & (HASH_SIZE - 1);
    while (L.by_path[h]) {
        h = (h + 1) & (HASH_SIZE - 1);
    }
    L.by_path[h] = g;
}

static bool better(const LibGame *a, const LibGame *b)
{
    return a->g.size != b->g.size ? a->g.size < b->g.size : strcmp(a->g.path, b->g.path) < 0;
}

/* L.mu held: one entry a game, version and regions shown, the smallest
 * file of those there (an XISO before a full dump of the same disc) */
static void dedupe(LibGame *g)
{
    g->dup = false;
    if (!g->g.title_id) {
        return;
    }
    LibGame *best = NULL;
    for (int i = 0; i < L.n; i++) {
        LibGame *e = L.games[i];
        if (e->g.title_id == g->g.title_id && e->g.version == g->g.version && e->g.regions == g->g.regions) {
            e->dup = true;
            if (!e->gone && (!best || better(e, best))) {
                best = e;
            }
        }
    }
    if (best) {
        best->dup = false;
    }
}

static int cmp_key(const void *a, const void *b)
{
    const LibGame *x = *(LibGame *const *)a, *y = *(LibGame *const *)b;
    if (x->g.title_id != y->g.title_id) {
        return x->g.title_id < y->g.title_id ? -1 : 1;
    }
    if (x->g.version != y->g.version) {
        return x->g.version < y->g.version ? -1 : 1;
    }
    if (x->g.regions != y->g.regions) {
        return x->g.regions < y->g.regions ? -1 : 1;
    }
    if (x->gone != y->gone) {
        return x->gone ? 1 : -1;
    }
    return better(x, y) ? -1 : better(y, x) ? 1 : 0;
}

/* L.mu held: dedupe for every game at once */
static void dedupe_all(void)
{
    LibGame **v = malloc((size_t)(L.n ? L.n : 1) * sizeof(*v));
    if (!v) {
        return;
    }
    memcpy(v, L.games, (size_t)L.n * sizeof(*v));
    qsort(v, L.n, sizeof(*v), cmp_key);
    for (int i = 0; i < L.n; i++) {
        const bool first = !i || v[i]->g.title_id != v[i - 1]->g.title_id || v[i]->g.version != v[i - 1]->g.version ||
                           v[i]->g.regions != v[i - 1]->g.regions;
        v[i]->dup = v[i]->g.title_id && (!first || v[i]->gone);
    }
    free(v);
}

static bool shown(const LibGame *g)
{
    return !g->gone && !g->dup && (!g->hidden || g_show_hidden);
}

/* ------------------------------------------------------------ the cache */

/* <data>/covers/library.txt: a line a file (games and second copies) as
 * the last scan left them, its path, size, modification time, title ID,
 * version, regions, year and title, so the library shows at once and a
 * scan reads only new or changed files. A file on a disk that's away
 * (its folder not there) stays in it for when the disk is back. */
typedef struct ScanArgs {
    char dirs[48][1024];    /* folders, and game files added one by one */
    int n;
} ScanArgs;

static void cache_file(char *out, size_t n)
{
    char d[1100];
    covers_dir(d, sizeof(d));
    snprintf(out, n, "%s/library.txt", d);
}

/* path in one of the folders, or one of the files */
static int under(const ScanArgs *a, const char *path)
{
    for (int i = 0; i < a->n; i++) {
        const char *d = a->dirs[i];
        const size_t n = strlen(d);
#ifdef _WIN32
        const bool pre = n && !SDL_strncasecmp(path, d, n);
#else
        const bool pre = n && !strncmp(path, d, n);
#endif
        if (pre && (!path[n] || path[n] == '/' || path[n] == '\\' || d[n - 1] == '/' || d[n - 1] == '\\')) {
            return i;
        }
    }
    return -1;
}

/* L.mu held: the last run's games, those in a's folders (there: which of
 * them are, looked at before) */
static void cache_load(const ScanArgs *a, const bool *there)
{
    char p[1200];
    cache_file(p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) {
        return;
    }
    char line[1600];
    while (L.n < MAX_GAMES && fgets(line, sizeof(line), f)) {
        char *fld[8];
        int k = 0;
        for (char *s = line, *t; k < 8 && (t = tok(&s, "\t")); k++) {
            fld[k] = t;
        }
        if (k < 7) {
            continue;
        }
        const int in = under(a, fld[0]);
        if (in < 0 || by_path(fld[0])) {
            continue;
        }
        LibGame *e = calloc(1, sizeof(*e));
        if (!e) {
            break;
        }
        XdGame *g = &e->g;
        snprintf(g->path, sizeof(g->path), "%s", fld[0]);
        g->size = strtoll(fld[1], NULL, 10);
        e->mtime = strtoll(fld[2], NULL, 10);
        g->title_id = (uint32_t)strtoul(fld[3], NULL, 16);
        g->version = (uint32_t)strtoul(fld[4], NULL, 10);
        g->regions = (uint32_t)strtoul(fld[5], NULL, 10);
        const char *title = fld[k - 1];
        if (k == 8) {
            g->year = atoi(fld[6]);
        } else {
            e->mtime = -1;      /* written before the year was: read again */
        }
        snprintf(g->title, sizeof(g->title), "%.*s", (int)strcspn(title, "\r\n"), title);
        snprintf(g->key, sizeof(g->key), "%08X.%u", g->title_id, g->version);
        e->gone = !there[in];   /* its disk is away */
        custom_apply(e);
        insert(e);
    }
    fclose(f);
    dedupe_all();
}

static void cache_save(const ScanArgs *a)
{
    bool there[48];
    for (int i = 0; i < a->n; i++) {
        there[i] = SDL_GetPathInfo(a->dirs[i], NULL);
    }
    char p[1200], tmp[1210];
    cache_file(p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.new", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return;
    }
    SDL_LockMutex(L.mu);
    for (int i = 0; i < L.n; i++) {
        const LibGame *g = L.games[i];
        if (g->gone || g->replaced) {
            /* gone from a library folder that's there: gone for good (a game's own
             * folder deleted too); else the disk it's on is away, for when it's back */
            const int in = under(a, g->g.path);
            if (g->replaced || in < 0 || there[in]) {
                continue;
            }
        }
        fprintf(f, "%s\t%lld\t%lld\t%08X\t%u\t%u\t%d\t%s\n", g->g.path, (long long)g->g.size, (long long)g->mtime,
                g->g.title_id, g->g.version, g->g.regions, g->g.year, g->orig[0] ? g->orig : g->g.title);
    }
    SDL_UnlockMutex(L.mu);
    const bool ok = fclose(f) == 0;
    if (ok) {
        remove(p);
        rename(tmp, p);
    } else {
        remove(tmp);
    }
}

/* ------------------------------------------------------------ scanning */

static bool scan_stopped(void)
{
    return SDL_GetAtomicInt(&L.stop) || SDL_GetAtomicInt(&L.restart);
}

static void add_game(const char *path, const SDL_PathInfo *pi)
{
    SDL_LockMutex(L.mu);
    LibGame *e = by_path(path);
    if (e && e->g.size == (int64_t)pi->size && e->mtime == (int64_t)pi->modify_time) {
        e->seen = L.pass;       /* as it was */
        if (e->gone) {
            e->gone = false;
            dedupe(e);
        }
        SDL_UnlockMutex(L.mu);
        return;
    }
    const bool full = !e && L.n >= MAX_GAMES;
    SDL_UnlockMutex(L.mu);
    if (full) {
        return;
    }
    /* new, or changed since: read */
    XdGame g;
    xd_read_game(path, &g);
    if (g.icon) {
        char ip[1200];
        icon_path(&g, ip, sizeof(ip));
        stbi_write_png(ip, g.icon_w, g.icon_h, 4, g.icon, g.icon_w * 4);
        xd_free_game(&g);
    }
    g.size = (int64_t)pi->size;
    SDL_LockMutex(L.mu);
    if (!e) {
        e = calloc(1, sizeof(*e));
        if (!e) {
            SDL_UnlockMutex(L.mu);
            return;
        }
        e->g = g;
        e->mtime = pi->modify_time;
        e->seen = L.pass;
        custom_apply(e);
        insert(e);
    } else {
        /* changed (replaced, or written before the year was kept): a new entry
         * in its place, the old one out of the list but left as it was (the
         * screen may be drawing it) */
        LibGame *n = L.n < MAX_GAMES ? calloc(1, sizeof(*n)) : NULL;
        if (!n) {
            SDL_UnlockMutex(L.mu);
            return;
        }
        n->g = g;
        n->mtime = pi->modify_time;
        n->seen = L.pass;
        custom_apply(n);
        e->gone = e->replaced = true;
        e->dup = false;
        unsigned h = path_hash(path) & (HASH_SIZE - 1);
        while (L.by_path[h] != e) {
            h = (h + 1) & (HASH_SIZE - 1);
        }
        L.by_path[h] = n;   /* the file's entry now */
        insert_sorted(n);
        dedupe(e);
        e = n;
    }
    dedupe(e);
    SDL_UnlockMutex(L.mu);
    if (SDL_GetAtomicInt(&L.busy)) {
        SDL_Delay(10);  /* a game is reading its disc meanwhile */
    }
}

/* a folder's names, one level (SDL_GlobDirectory goes down the whole tree:
 * with scan_dir's own going down too, a game a few folders deep was read
 * once a level, under as many spellings of its path) */
typedef struct {
    char **v;
    int n, cap;
} Names;

static SDL_EnumerationResult SDLCALL collect_name(void *ud, const char *dir, const char *name)
{
    (void)dir;
    Names *l = ud;
    if (l->n == l->cap) {
        const int cap = l->cap ? l->cap * 2 : 64;
        char **v = realloc(l->v, (size_t)cap * sizeof(*v));
        if (!v) {
            return SDL_ENUM_FAILURE;
        }
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n] = SDL_strdup(name);
    l->n += l->v[l->n] != NULL;
    return scan_stopped() ? SDL_ENUM_SUCCESS : SDL_ENUM_CONTINUE;
}

static void scan_dir(const char *dir, int depth)
{
    if (depth > 6 || scan_stopped()) {
        return;
    }
    Names l = {0};
    SDL_EnumerateDirectory(dir, collect_name, &l);
    char **names = l.v;
    const int n = l.n;
    for (int i = 0; names && i < n && !scan_stopped(); i++) {
        char p[1200];
        snprintf(p, sizeof(p), "%s%s%s", dir, dir[strlen(dir) - 1] == '/' || dir[strlen(dir) - 1] == '\\' ? "" : "/",
                 names[i]);
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(p, &pi)) {
            continue;
        }
        if (pi.type == SDL_PATHTYPE_DIRECTORY) {
            scan_dir(p, depth + 1);
        } else if (pi.type == SDL_PATHTYPE_FILE && is_image_name(names[i]) && pi.size > 1 << 20 &&
                   strlen(p) < sizeof(((XdGame *)0)->path) - 1) {
            add_game(p, &pi);
        }
    }
    for (int i = 0; i < n; i++) {
        SDL_free(names[i]);
    }
    free(names);
}

static int scan_thread(void *arg)
{
    ScanArgs *a = arg;
    for (;;) {
        bool there[48];
        for (int i = 0; i < a->n && !L.loaded; i++) {
            there[i] = SDL_GetPathInfo(a->dirs[i], NULL);
        }
        SDL_LockMutex(L.mu);
        L.pass++;
        if (!L.loaded) {
            L.loaded = true;
            cache_load(a, there);   /* the library at once, as the last run left it */
        }
        SDL_UnlockMutex(L.mu);
        for (int i = 0; i < a->n && !scan_stopped(); i++) {
            SDL_PathInfo pi;
            if (SDL_GetPathInfo(a->dirs[i], &pi) && pi.type == SDL_PATHTYPE_FILE) {
                add_game(a->dirs[i], &pi);  /* any name: it was chosen */
            } else {
                scan_dir(a->dirs[i], 0);
            }
        }
        const bool whole = !scan_stopped();
        SDL_LockMutex(L.mu);
        if (whole) {
            /* the files not found this time: gone (away, or their folder left the library) */
            for (int i = 0; i < L.n; i++) {
                if (L.games[i]->seen != L.pass || L.games[i]->replaced) {
                    L.games[i]->gone = true;
                }
            }
            dedupe_all();
        }
        ScanArgs *next = SDL_GetAtomicInt(&L.stop) ? NULL : L.pending;
        if (next) {
            L.pending = NULL;
        }
        SDL_SetAtomicInt(&L.restart, 0);
        if (!next) {
            SDL_SetAtomicInt(&L.scanning, 0);
        }
        SDL_UnlockMutex(L.mu);
        if (whole || SDL_GetAtomicInt(&L.stop)) {
            cache_save(a);  /* closed halfway: what was read so far, for the next start */
        }
        free(a);
        if (!next) {
            break;
        }
        a = next;   /* the folders changed meanwhile: again, with them */
    }
    return 0;
}

void lib_scan(const char *const *folders, int n)
{
    if (!L.mu) {
        L.mu = SDL_CreateMutex();
    }
    ScanArgs *a = calloc(1, sizeof(*a));
    if (!a) {
        return;
    }
    for (int i = 0; i < n && i < 48; i++) {
        snprintf(a->dirs[a->n++], sizeof(a->dirs[0]), "%s", folders[i]);
    }
    SDL_LockMutex(L.mu);
    if (SDL_GetAtomicInt(&L.scanning)) {
        /* one runs: it stops where it is and starts over with these */
        free(L.pending);
        L.pending = a;
        SDL_SetAtomicInt(&L.restart, 1);
        SDL_UnlockMutex(L.mu);
        return;
    }
    SDL_SetAtomicInt(&L.scanning, 1);
    SDL_UnlockMutex(L.mu);
    if (L.scan) {
        SDL_WaitThread(L.scan, NULL);
    }
    L.scan = SDL_CreateThread(scan_thread, "lib-scan", a);
}

bool lib_scanning(void)
{
    return SDL_GetAtomicInt(&L.scanning) != 0;
}

void lib_set_busy(bool busy)
{
    SDL_SetAtomicInt(&L.busy, busy);
}

int lib_count(void)
{
    if (!L.mu) {
        return 0;
    }
    SDL_LockMutex(L.mu);
    int k = 0;
    for (int i = 0; i < L.n; i++) {
        k += shown(L.games[i]);
    }
    SDL_UnlockMutex(L.mu);
    return k;
}

LibGame *lib_get(int idx)
{
    if (!L.mu) {
        return NULL;
    }
    LibGame *r = NULL;
    SDL_LockMutex(L.mu);
    for (int i = 0, k = 0; i < L.n; i++) {
        if (shown(L.games[i]) && k++ == idx) {
            r = L.games[i];
            break;
        }
    }
    SDL_UnlockMutex(L.mu);
    return r;
}

int lib_list(LibGame **out, int max)
{
    if (!L.mu) {
        return 0;
    }
    int k = 0;
    SDL_LockMutex(L.mu);
    for (int i = 0; i < L.n && k < max; i++) {
        if (shown(L.games[i])) {
            out[k++] = L.games[i];
        }
    }
    SDL_UnlockMutex(L.mu);
    return k;
}

/* The publishers by the two letters that start their title IDs. */
static const struct {
    char code[3];
    const char *name;
} k_publishers[] = {
    {"AC", "Acclaim"}, {"AH", "ARUSH Entertainment"}, {"AQ", "Aqua System"}, {"AS", "ASK"}, {"AT", "Atlus"},
    {"AV", "Activision"}, {"AY", "Aspyr"}, {"BA", "Bandai"}, {"BL", "Black Box"}, {"BM", "BAM! Entertainment"},
    {"BR", "Broccoli"}, {"BS", "Bethesda Softworks"}, {"BU", "Bunkasha Games"}, {"BV", "Buena Vista Games"},
    {"BW", "BBC Multimedia"}, {"BZ", "Blizzard"}, {"CC", "Capcom"}, {"CK", "Kemco"}, {"CM", "Codemasters"},
    {"CV", "Crave Entertainment"}, {"DC", "DreamCatcher"}, {"DX", "Davilex"}, {"EA", "Electronic Arts"},
    {"EC", "Encore"}, {"EL", "Enlight Software"}, {"EM", "Empire Interactive"}, {"ES", "Eidos Interactive"},
    {"FI", "Fox Interactive"}, {"FS", "From Software"}, {"GE", "Genki"}, {"GV", "Groove Games"},
    {"HE", "Tru Blu"}, {"HP", "Hip Interactive"}, {"HU", "Hudson Soft"}, {"HW", "Highwaymen"},
    {"IA", "Mad Catz"}, {"IF", "Idea Factory"}, {"IG", "Infogrames"}, {"IL", "Interlex"},
    {"IM", "Imagine Media"}, {"IO", "Ignition"}, {"IP", "Interplay"}, {"IX", "InXile Entertainment"},
    {"JA", "Jaleco"}, {"JW", "JoWooD"}, {"KB", "Kemco"}, {"KI", "Kids Station"}, {"KN", "Konami"},
    {"KO", "KOEI"}, {"KU", "Kobi"}, {"LA", "LucasArts"}, {"LS", "Black Bean Games"}, {"MD", "Metro3D"},
    {"ME", "Medix"}, {"MI", "Microids"}, {"MJ", "Majesco"}, {"MM", "Myelin Media"}, {"MP", "MediaQuest"},
    {"MS", "Microsoft"}, {"MW", "Midway"}, {"MX", "Empire Interactive"}, {"NK", "NewKidCo"},
    {"NL", "NovaLogic"}, {"NM", "Namco"}, {"OX", "Oxygen Interactive"}, {"PC", "Playlogic"},
    {"PL", "Phantagram"}, {"RA", "Rage"}, {"SA", "Sammy"}, {"SC", "SCi Games"}, {"SE", "SEGA"},
    {"SN", "SNK"}, {"SS", "Simon & Schuster"}, {"SU", "Success"}, {"SW", "Swing! Deutschland"},
    {"TA", "Takara"}, {"TC", "Tecmo"}, {"TD", "The 3DO Company"}, {"TK", "Takuyo"},
    {"TM", "TDK Mediactive"}, {"TQ", "THQ"}, {"TS", "Titus Interactive"}, {"TT", "Take-Two Interactive"},
    {"US", "Ubisoft"}, {"VC", "Victor Interactive"}, {"VN", "Vivendi Universal"}, {"VU", "Vivendi Universal"},
    {"WB", "Warner Bros."}, {"WC", "Wizard Corporation"}, {"WR", "Warner Bros."}, {"XI", "XPEC Entertainment"},
    {"XK", "Xbox Korea"}, {"XM", "Evolved Games"}, {"XP", "XPEC"}, {"XR", "Panorama"}, {"YB", "YBM Sisa"},
    {"ZD", "Zushi Games"},
};

const char *lib_publisher(const LibGame *g)
{
    const char c[3] = {(char)(g->g.title_id >> 24), (char)(g->g.title_id >> 16), 0};
    for (size_t i = 0; i < sizeof(k_publishers) / sizeof(k_publishers[0]); i++) {
        if (!strcmp(k_publishers[i].code, c)) {
            return k_publishers[i].name;
        }
    }
    return "";
}

UiTex *lib_load_png(const char *path)
{
    int w, h, c;
    uint8_t *px = stbi_load(path, &w, &h, &c, 4);
    if (!px) {
        return NULL;
    }
    UiTex *t = ui_texture_rgba(px, w, h);
    stbi_image_free(px);
    return t;
}

UiTex *lib_icon(LibGame *g)
{
    if (!g->icon_tex) {
        char p[1200];
        icon_path(&g->g, p, sizeof(p));
        g->icon_tex = lib_load_png(p);
        if (!g->icon_tex) {
            g->icon_tex = (UiTex *)1;    /* none: not looked for again */
        }
    }
    return g->icon_tex == (UiTex *)1 ? NULL : g->icon_tex;
}

UiTex *lib_cover(LibGame *g)
{
    if (!g->cover_tex && !g->cover_tried) {
        char p[1200];
        cover_path(&g->g, p, sizeof(p));
        g->cover_tex = lib_load_png(p);
        g->cover_tried = true;
    }
    return g->cover_tex;
}

/* the same game as g (its key), not merely two discs that couldn't be read */
static bool same_game(const LibGame *e, const LibGame *g)
{
    return g->g.title_id && !strcmp(e->g.key, g->g.key);
}

void lib_set_name(LibGame *g, const char *name)
{
    SDL_LockMutex(L.mu);
    Custom *c = custom_of(g->g.key, true);
    if (c) {
        snprintf(c->name, sizeof(c->name), "%s", name ? name : "");
        for (int i = 0; i < L.n; i++) {
            LibGame *e = L.games[i];
            if (e != g && !same_game(e, g)) {
                continue;
            }
            if (c->name[0]) {
                if (!e->orig[0]) {
                    snprintf(e->orig, sizeof(e->orig), "%s", e->g.title);
                }
                snprintf(e->g.title, sizeof(e->g.title), "%s", c->name);
            } else if (e->orig[0]) {
                snprintf(e->g.title, sizeof(e->g.title), "%s", e->orig);
                e->orig[0] = 0;
            }
        }
        custom_save();
        qsort(L.games, L.n, sizeof(L.games[0]), cmp_title);
    }
    SDL_UnlockMutex(L.mu);
}

void lib_set_hidden(LibGame *g, bool hidden)
{
    SDL_LockMutex(L.mu);
    Custom *c = custom_of(g->g.key, true);
    if (c) {
        c->hidden = hidden;
        for (int i = 0; i < L.n; i++) {
            if (L.games[i] == g || same_game(L.games[i], g)) {
                L.games[i]->hidden = hidden;
            }
        }
        custom_save();
    }
    SDL_UnlockMutex(L.mu);
}

/* L.mu held: the favorite and the time played, the same for every entry
 * of the game (its other copies) */
static void custom_sync(const LibGame *g)
{
    const Custom *c = custom_of(g->g.key, false);
    for (int i = 0; i < L.n; i++) {
        LibGame *e = L.games[i];
        if (e == g || same_game(e, g)) {
            e->fav = c && c->fav;
            e->played = c ? c->played : 0;
        }
    }
}

void lib_set_favorite(LibGame *g, bool fav)
{
    SDL_LockMutex(L.mu);
    Custom *c = custom_of(g->g.key, true);
    if (c) {
        c->fav = fav;
        custom_save();
        custom_sync(g);
    }
    SDL_UnlockMutex(L.mu);
}

void lib_set_played(LibGame *g)
{
    SDL_LockMutex(L.mu);
    Custom *c = custom_of(g->g.key, true);
    if (c) {
        c->played = (int64_t)time(NULL);
        custom_save();
        custom_sync(g);
    }
    SDL_UnlockMutex(L.mu);
}

void lib_show_hidden(bool show)
{
    g_show_hidden = show;
}

bool lib_set_cover_file(LibGame *g, const char *path)
{
    int w, h, c;
    uint8_t *px = stbi_load(path, &w, &h, &c, 4);
    if (!px) {
        return false;
    }
    char to[1200];
    cover_path(&g->g, to, sizeof(to));
    const bool ok = stbi_write_png(to, w, h, 4, px, w * 4) != 0;
    stbi_image_free(px);
    if (g->cover_tex) {
        ui_texture_free(g->cover_tex);
        g->cover_tex = NULL;
    }
    g->cover_tried = false;
    return ok;
}

bool lib_icon_files(const LibGame *g, char *png, size_t pn, char *ico, size_t in)
{
    icon_path(&g->g, png, pn);
    size_t n = 0;
    uint8_t *d = SDL_LoadFile(png, &n);
    if (!d || n < 24) {
        SDL_free(d);
        return false;
    }
    /* an ICO holding the PNG as it is (Windows Vista and later read those) */
    const uint32_t w = (uint32_t)d[16] << 24 | (uint32_t)d[17] << 16 | (uint32_t)d[18] << 8 | d[19];
    const uint32_t h = (uint32_t)d[20] << 24 | (uint32_t)d[21] << 16 | (uint32_t)d[22] << 8 | d[23];
    char dir[1100];
    covers_dir(dir, sizeof(dir));
    snprintf(ico, in, "%s/%s.ico", dir, g->g.key);
    bool ok = SDL_GetPathInfo(ico, NULL);
    if (!ok) {
        uint8_t hdr[22] = {0, 0, 1, 0, 1, 0};
        hdr[6] = w >= 256 ? 0 : (uint8_t)w;
        hdr[7] = h >= 256 ? 0 : (uint8_t)h;
        hdr[10] = 1;                        /* planes */
        hdr[12] = 32;                       /* bits per pixel */
        for (int i = 0; i < 4; i++) {
            hdr[14 + i] = (uint8_t)(n >> (8 * i));
            hdr[18 + i] = (uint8_t)(22u >> (8 * i));
        }
        SDL_IOStream *io = SDL_IOFromFile(ico, "wb");
        ok = io && SDL_WriteIO(io, hdr, sizeof(hdr)) == sizeof(hdr) && SDL_WriteIO(io, d, n) == n;
        if (io) {
            SDL_CloseIO(io);
        }
    }
    SDL_free(d);
    return ok;
}

/* ------------------------------------------------------------ box art */

static bool run(const char *const *args)
{
    return cfg_run(args);
}

static bool download(const char *url, const char *to)
{
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.part", to);
    const char *args[] = {"curl", "-sfL", "--max-time", "60", "-A", "xbased", "-o", tmp, url, NULL};
    if (!run(args)) {
        remove(tmp);
        return false;
    }
    remove(to);
    return rename(tmp, to) == 0;
}

/* lowercase words, "&" as "and", groups dropped */
static void norm(const char *s, char *out, size_t n)
{
    size_t o = 0;
    int depth = 0;
    for (; *s && o + 5 < n; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '(' || c == '[') {
            depth++;
        } else if ((c == ')' || c == ']') && depth) {
            depth--;
        } else if (depth) {
            continue;
        } else if (c == '&') {
            memcpy(out + o, " and ", 5);
            o += 5;
        } else if (isalnum(c) || c >= 0x80) {
            out[o++] = (char)tolower(c);
        } else if (o && out[o - 1] != ' ') {
            out[o++] = ' ';
        }
    }
    while (o && out[o - 1] == ' ') {
        o--;
    }
    out[o] = 0;
}

/* words shared, over the words of both (articles and "the" ignored) */
static float score(const char *a, const char *b)
{
    char wa[32][32], wb[32][32];
    int na = 0, nb = 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", a);
    for (char *s = tmp, *t; na < 32 && (t = tok(&s, " "));) {
        if (*t && strcmp(t, "the") && strcmp(t, "and") && strcmp(t, "of")) {
            snprintf(wa[na++], 32, "%s", t);
        }
    }
    snprintf(tmp, sizeof(tmp), "%s", b);
    for (char *s = tmp, *t; nb < 32 && (t = tok(&s, " "));) {
        if (*t && strcmp(t, "the") && strcmp(t, "and") && strcmp(t, "of")) {
            snprintf(wb[nb++], 32, "%s", t);
        }
    }
    int common = 0;
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (!strcmp(wa[i], wb[j])) {
                common++;
                break;
            }
        }
    }
    return na + nb ? 2.0f * common / (na + nb) : 0;
}

static char **g_names;
static int g_nnames;

static bool names_load(void)
{
    if (g_names) {
        return true;
    }
    char d[1100], p[1200];
    covers_dir(d, sizeof(d));
    snprintf(p, sizeof(p), "%s/boxart_names.txt", d);
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(p, &pi) || pi.size < 1000) {
        char js[1200];
        snprintf(js, sizeof(js), "%s/tree.json", d);
        snprintf(L.status, sizeof(L.status), "libretro-thumbnails...");
        if (!download(TREE, js)) {
            return false;
        }
        /* the "path" values under Named_Boxarts/ */
        size_t n = 0;
        char *t = SDL_LoadFile(js, &n);
        FILE *f = t ? fopen(p, "wb") : NULL;
        for (char *s = t; f && (s = strstr(s, "\"path\":\"Named_Boxarts/"));) {
            s += 22;    /* past "path":"Named_Boxarts/ */
            char *e = strchr(s, '"');
            if (!e) {
                break;
            }
            if (e - s > 4 && !strncmp(e - 4, ".png", 4)) {
                /* JSON escapes: & for '&' and the like */
                for (char *c = s; c < e; c++) {
                    if (c[0] == '\\' && c[1] == 'u' && e - c >= 6) {
                        fputc((int)strtol((char[5]){c[2], c[3], c[4], c[5], 0}, NULL, 16), f);
                        c += 5;
                    } else if (c[0] == '\\' && c + 1 < e) {
                        fputc(*++c, f);
                    } else {
                        fputc(*c, f);
                    }
                }
                fputc('\n', f);
            }
            s = e;
        }
        if (f) {
            fclose(f);
        }
        SDL_free(t);
        remove(js);
    }
    size_t n = 0;
    char *t = SDL_LoadFile(p, &n);
    if (!t) {
        return false;
    }
    g_names = calloc(4096, sizeof(char *));
    for (char *s = t, *l; g_nnames < 4096 && (l = tok(&s, "\n"));) {
        if (*l) {
            g_names[g_nnames++] = l;
        }
    }
    return g_nnames > 0;
}

static const char *best_name(const XdGame *g)
{
    char a[256], b[256], f[256];
    norm(g->title, a, sizeof(a));
    char fn[256];
    xd_clean_name(g->path, fn, sizeof(fn));
    norm(fn, f, sizeof(f));
    const char *best = NULL;
    float bs = 0;
    for (int i = 0; i < g_nnames; i++) {
        norm(g_names[i], b, sizeof(b));
        float s = score(a, b);
        const float s2 = score(f, b);
        if (s2 > s) {
            s = s2;
        }
        if (s < 0.6f) {
            continue;
        }
        /* the disc's region first (1 North America, 2 Japan, 4 the rest), no demos */
        const char *nm = g_names[i];
        if (strstr(nm, "(Demo)") || strstr(nm, "(Beta)") || strstr(nm, "(Proto)")) {
            s -= 0.3f;
        }
        if (((g->regions & 4) && strstr(nm, "(Europe")) || ((g->regions & 1) && strstr(nm, "(USA")) ||
            ((g->regions & 2) && strstr(nm, "(Japan"))) {
            s += 0.05f;
        }
        if (s > bs) {
            bs = s;
            best = nm;
        }
    }
    return best;
}

static void url_escape(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        const unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            snprintf(out + o, 4, "%%%02X", c);
            o += 3;
        }
    }
    out[o] = 0;
}

static int fetch_thread(void *arg)
{
    for (;;) {
        SDL_LockMutex(L.mu);
        if (!L.nq || SDL_GetAtomicInt(&L.stop)) {
            SDL_UnlockMutex(L.mu);
            break;
        }
        LibGame *g = L.queue[--L.nq];
        const int left = L.nq;
        SDL_UnlockMutex(L.mu);
        snprintf(L.status, sizeof(L.status), "%s (%d)", g->g.title, left + 1);
        if (!names_load()) {
            break;
        }
        XdGame named = g->g;            /* matched by the disc's name, not the user's */
        if (g->orig[0]) {
            snprintf(named.title, sizeof(named.title), "%s", g->orig);
        }
        const char *nm = best_name(&named);
        if (!nm) {
            continue;
        }
        char esc[600], url[800], to[1200];
        url_escape(nm, esc, sizeof(esc));
        snprintf(url, sizeof(url), THUMBS "%s", esc);
        cover_path(&g->g, to, sizeof(to));
        if (download(url, to)) {
            g->cover_tried = false;     /* the UI's thread loads it */
        }
    }
    L.status[0] = 0;
    SDL_SetAtomicInt(&L.fetching, 0);
    return 0;
}

static void fetch_start(void)
{
    if (SDL_GetAtomicInt(&L.fetching)) {
        return;
    }
    if (L.fetch) {
        SDL_WaitThread(L.fetch, NULL);
    }
    SDL_SetAtomicInt(&L.fetching, 1);
    L.fetch = SDL_CreateThread(fetch_thread, "lib-covers", NULL);
}

void lib_fetch_cover(LibGame *g)
{
    SDL_LockMutex(L.mu);
    for (int i = 0; i < L.n; i++) {
        if (L.games[i] == g && L.nq < MAX_GAMES) {
            L.queue[L.nq++] = g;
        }
    }
    SDL_UnlockMutex(L.mu);
    fetch_start();
}

void lib_fetch_all_covers(void)
{
    SDL_LockMutex(L.mu);
    for (int i = L.n - 1; i >= 0; i--) {
        char p[1200];
        cover_path(&L.games[i]->g, p, sizeof(p));
        LibGame *g = L.games[i];
        if (!g->gone && !g->dup && !g->fetch_tried && !SDL_GetPathInfo(p, NULL) && L.nq < MAX_GAMES) {
            g->fetch_tried = true;      /* once a run (none online: not asked again each scan) */
            L.queue[L.nq++] = g;
        }
    }
    SDL_UnlockMutex(L.mu);
    fetch_start();
}

const char *lib_fetch_status(void)
{
    return SDL_GetAtomicInt(&L.fetching) && L.status[0] ? L.status : NULL;
}

void lib_poll(void)
{
    /* covers fetched: their textures reload (cover_tried cleared) */
    if (!L.mu) {
        return;
    }
    SDL_LockMutex(L.mu);
    for (int i = 0; i < L.n; i++) {
        LibGame *g = L.games[i];
        if (!g->cover_tried && g->cover_tex) {
            ui_texture_free(g->cover_tex);
            g->cover_tex = NULL;
        }
    }
    SDL_UnlockMutex(L.mu);
}

void lib_shutdown(void)
{
    SDL_SetAtomicInt(&L.stop, 1);
    if (L.scan) {
        SDL_WaitThread(L.scan, NULL);
    }
    if (L.fetch) {
        SDL_WaitThread(L.fetch, NULL);
    }
    for (int i = 0; i < L.n; i++) {
        LibGame *g = L.games[i];
        if (g->icon_tex && g->icon_tex != (UiTex *)1) {
            ui_texture_free(g->icon_tex);
        }
        if (g->cover_tex) {
            ui_texture_free(g->cover_tex);
        }
        free(g);
    }
    L.n = 0;
    memset(L.by_path, 0, sizeof(L.by_path));
    free(L.pending);
    L.pending = NULL;
}
