/*
 * hakuX front end — the Games screen's search, filters, order and view
 * (dash_int.h): a screen of rows (the search, a publisher, a year, a
 * region, the order, the view, favorites and the last played first) with
 * an on-screen keyboard for the search, so a gamepad types as well as the
 * keyboard does. What a game shows of itself is its disc's: the title, the
 * publisher its title ID stands for, its certificate's year and regions.
 */
#include "native/app/ui/dash_int.h"
#include "native/app/ui/xbox_data.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LIST 16384

static struct {
    char q[64];         /* the search: words a title (or its publisher) has, any order */
    char pub[64];       /* a publisher ("" any; "?" an unknown one) */
    int year;           /* 0 any, -1 unknown */
    int region;         /* 0 any, else the region's bit (1 NTSC-U, 2 NTSC-J, 4 PAL) */
    bool editing;       /* typing the search */
    int kx, ky;         /* the on-screen keyboard's key */
    const LibGame *last;    /* the last played, in the last list */
} F;

/* ------------------------------------------------------------ the order */

static int g_sort;   /* the order, read once a list (cmp_item runs a lot) */

static int sort_mode(void)
{
    return g_sort;
}

static int read_sort(void)
{
    const char *v = cfg_get(&D.global, "ui.sort");
    return v && !strcmp(v, "publisher") ? 1 : v && !strcmp(v, "year") ? 2 : 0;
}

int dash_find_view(void)
{
    const char *v = cfg_get(&D.global, "ui.view");
    return v && !strcmp(v, "small") ? 1 : v && !strcmp(v, "icons") ? 2 : 0;
}

static void set_view(int v)
{
    static const char *const k[3] = {"large", "small", "icons"};
    cfg_set(&D.global, "ui.view", k[(v % 3 + 3) % 3]);
}

void dash_find_next_view(void)
{
    set_view(dash_find_view() + 1);
}

/* 0 the last played (first), 1 a favorite, 2 the rest */
static int rank(const LibGame *g)
{
    if (g == F.last) {
        return 0;
    }
    return g->fav && cfg_bool(&D.global, "ui.favfirst", true) ? 1 : 2;
}

static const char *pub_of(const LibGame *g)
{
    return lib_publisher(g);
}

/* lowercase letters and digits: "Spider-Man 2" as "spiderman2" */
static void squash(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (; *s && o + 1 < n; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c >= 0x80) {
            out[o++] = (char)c;
        } else if (isalnum(c)) {
            out[o++] = (char)tolower(c);
        }
    }
    out[o] = 0;
}

static bool matches(const LibGame *g)
{
    if (F.pub[0] && strcmp(F.pub[0] == '?' ? "" : F.pub, pub_of(g))) {
        return false;
    }
    if (F.year && (F.year < 0 ? g->g.year != 0 : g->g.year != F.year)) {
        return false;
    }
    if (F.region && !(g->g.regions & (unsigned)F.region)) {
        return false;
    }
    if (!F.q[0]) {
        return true;
    }
    char title[300], orig[300], file[300], pub[100], fn[300];
    squash(g->g.title, title, sizeof(title));
    squash(g->orig, orig, sizeof(orig));
    xd_clean_name(g->g.path, fn, sizeof(fn));  /* "M:I - OP SURMA" is "Mission Impossible ... .iso" */
    squash(fn, file, sizeof(file));
    squash(pub_of(g), pub, sizeof(pub));
    /* every word of the search in the title, the disc's own name, the file's or the publisher */
    for (const char *w = F.q; *w;) {
        const size_t wn = strcspn(w, " ");
        char word[64], sw[64];
        snprintf(word, sizeof(word), "%.*s", (int)wn, w);
        squash(word, sw, sizeof(sw));
        if (sw[0] && !strstr(title, sw) && !strstr(orig, sw) && !strstr(file, sw) && !strstr(pub, sw)) {
            return false;
        }
        w += wn;
        w += *w == ' ';
    }
    return true;
}

typedef struct {
    LibGame *g;
    int idx;    /* the library's order (by title) */
    int rank;
} Item;

static int cmp_item(const void *a, const void *b)
{
    const Item *x = a, *y = b;
    if (x->rank != y->rank) {
        return x->rank - y->rank;
    }
    if (x->rank == 2) {
        if (sort_mode() == 1) {
            const char *px = pub_of(x->g), *py = pub_of(y->g);
            if (!*px != !*py) {
                return *px ? -1 : 1;    /* an unknown publisher last */
            }
            const int c = SDL_strcasecmp(px, py);
            if (c) {
                return c;
            }
        } else if (sort_mode() == 2 && x->g->g.year != y->g->g.year) {
            if (!x->g->g.year != !y->g->g.year) {
                return x->g->g.year ? -1 : 1;
            }
            return x->g->g.year - y->g->g.year;
        }
    }
    return x->idx - y->idx;
}

int dash_find_list(LibGame **out, int max)
{
    static LibGame *all[MAX_LIST];
    static Item it[MAX_LIST];
    const int n = lib_list(all, MAX_LIST);
    g_sort = read_sort();
    F.last = NULL;
    if (cfg_bool(&D.global, "ui.lastfirst", true)) {
        for (int i = 0; i < n; i++) {
            if (all[i]->played && (!F.last || all[i]->played > F.last->played)) {
                F.last = all[i];
            }
        }
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (matches(all[i])) {
            it[k] = (Item){all[i], i, rank(all[i])};
            k++;
        }
    }
    qsort(it, k, sizeof(it[0]), cmp_item);
    k = k < max ? k : max;
    for (int i = 0; i < k; i++) {
        out[i] = it[i].g;
    }
    return k;
}

int dash_find_group(const LibGame *g)
{
    const int r = rank(g);
    if (r < 2) {
        return -2 + r;      /* -2 the last played, -1 the favorites */
    }
    switch (sort_mode()) {
    case 1: {
        unsigned h = 2166136261u;
        for (const char *s = pub_of(g); *s; s++) {
            h = (h ^ (unsigned char)*s) * 16777619u;
        }
        return (int)(h & 0x7FFFFFFF);
    }
    case 2:
        return g->g.year;
    default:
        return lib_title_group(g->g.title);
    }
}

const char *dash_find_group_name(const LibGame *g)
{
    static char b[64];
    const int r = rank(g);
    if (r < 2) {
        return T(r ? S_FAVORITES : S_LAST_PLAYED);
    }
    switch (sort_mode()) {
    case 1:
        return *pub_of(g) ? pub_of(g) : T(S_UNKNOWN);
    case 2:
        if (!g->g.year) {
            return T(S_UNKNOWN);
        }
        snprintf(b, sizeof(b), "%d", g->g.year);
        return b;
    default: {
        const int k = lib_title_group(g->g.title);
        snprintf(b, sizeof(b), "%c", k ? 'A' + k - 1 : '#');
        return b;
    }
    }
}

static const char *region_name(int r)
{
    return r == 1 ? "NTSC-U" : r == 2 ? "NTSC-J" : r == 4 ? "PAL" : T(S_ALL);
}

const char *dash_find_desc(void)
{
    static char b[200];
    b[0] = 0;
    size_t o = 0;
    if (F.q[0]) {
        o += (size_t)snprintf(b + o, sizeof(b) - o, "\"%s\"", F.q);
    }
    if (F.pub[0] && o < sizeof(b)) {
        o += (size_t)snprintf(b + o, sizeof(b) - o, "%s%s", o ? " · " : "", F.pub[0] == '?' ? T(S_UNKNOWN) : F.pub);
    }
    if (F.year && o < sizeof(b)) {
        if (F.year > 0) {
            o += (size_t)snprintf(b + o, sizeof(b) - o, "%s%d", o ? " · " : "", F.year);
        } else {
            o += (size_t)snprintf(b + o, sizeof(b) - o, "%s%s", o ? " · " : "", T(S_UNKNOWN));
        }
    }
    if (F.region && o < sizeof(b)) {
        snprintf(b + o, sizeof(b) - o, "%s%s", o ? " · " : "", region_name(F.region));
    }
    return b;
}

/* ------------------------------------------------------------ the screen */

/* the publishers (or years) the library has, in order */
static int publishers(const char **out, int max)
{
    static LibGame *all[MAX_LIST];
    const int n = lib_list(all, MAX_LIST);
    int k = 0;
    bool unknown = false;
    for (int i = 0; i < n; i++) {
        const char *p = pub_of(all[i]);
        if (!*p) {
            unknown = true;
            continue;
        }
        int j = 0;
        while (j < k && strcmp(out[j], p)) {
            j++;
        }
        if (j == k && k < max - 1) {
            /* in its place, A to Z */
            int at = k;
            while (at > 0 && SDL_strcasecmp(out[at - 1], p) > 0) {
                out[at] = out[at - 1];
                at--;
            }
            out[at] = p;
            k++;
        }
    }
    if (unknown && k < max) {
        out[k++] = "?";
    }
    return k;
}

static int years(int *out, int max)
{
    static LibGame *all[MAX_LIST];
    const int n = lib_list(all, MAX_LIST);
    bool seen[11] = {false}, unknown = false;
    for (int i = 0; i < n; i++) {
        const int y = all[i]->g.year;
        if (y >= 2000 && y <= 2010) {
            seen[y - 2000] = true;
        } else {
            unknown = true;
        }
    }
    int k = 0;
    for (int y = 0; y <= 10 && k < max; y++) {
        if (seen[y]) {
            out[k++] = 2000 + y;
        }
    }
    if (unknown && k < max) {
        out[k++] = -1;
    }
    return k;
}

/* the on-screen keyboard: four rows of ten, then Space, Delete and Done */
static const char *const k_rows[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM-:&"};

static void type(const char *s)
{
    if (strlen(F.q) + strlen(s) < sizeof(F.q)) {
        strcat(F.q, s);
    }
}

static void rub_out(void)
{
    size_t n = strlen(F.q);
    if (!n) {
        return;
    }
    do {
        n--;    /* a whole UTF-8 character */
    } while (n > 0 && ((unsigned char)F.q[n] & 0xC0) == 0x80);
    F.q[n] = 0;
}

static void edit(bool on)
{
    F.editing = on;
    D.typing = on;
    if (on) {
        SDL_StartTextInput(SDL_GetKeyboardFocus());
    } else {
        SDL_StopTextInput(SDL_GetKeyboardFocus());
    }
}

/* the bottom row's keys: their first column and width */
static const int k_wide_at[3] = {0, 4, 7}, k_wide_n[3] = {4, 3, 3};

static int wide_of(int kx)
{
    return kx >= 7 ? 2 : kx >= 4 ? 1 : 0;
}

/* the keyboard at (x, y), keys of ks; the input was the screen's */
static void osk(float x, float y, float ks, const In *in)
{
    const float gap = 6;
    if (in->left) {
        F.kx = F.ky == 4 ? k_wide_at[(wide_of(F.kx) + 2) % 3] : (F.kx + 9) % 10;
    }
    if (in->right) {
        F.kx = F.ky == 4 ? k_wide_at[(wide_of(F.kx) + 1) % 3] : (F.kx + 1) % 10;
    }
    if (in->up) {
        F.ky = (F.ky + 4) % 5;
    }
    if (in->down) {
        F.ky = (F.ky + 1) % 5;
    }
    int pressed = -1;   /* the key chosen: 0-39 a character, 40 Space, 41 Delete, 42 Done */
    for (int r = 0; r < 5; r++) {
        const int nk = r < 4 ? 10 : 3;
        for (int c = 0; c < nk; c++) {
            const int col = r < 4 ? c : k_wide_at[c], wn = r < 4 ? 1 : k_wide_n[c];
            const float kx = x + col * (ks + gap), ky = y + r * (ks + gap), kw = wn * ks + (wn - 1) * gap;
            const bool over = in->mx >= kx && in->mx < kx + kw && in->my >= ky && in->my < ky + ks;
            if (over && in->moved) {
                F.ky = r;
                F.kx = col;
            }
            const bool sel = F.ky == r && (r < 4 ? F.kx == c : wide_of(F.kx) == c);
            ui_round(kx, ky, kw, ks, 8, sel ? ui_alpha(UI_GREEN, 0.9f) : (UiColor){0x12, 0x2C, 0x0A, 0xF0});
            ui_round_outline(kx, ky, kw, ks, 8, sel ? 2 : 1, sel ? UI_GREEN_HI : UI_GREEN_DIM);
            char label[64];
            if (r < 4) {
                snprintf(label, sizeof(label), "%c", k_rows[r][c]);
            } else {
                snprintf(label, sizeof(label), "%s", T(c == 0 ? S_KB_SPACE : c == 1 ? S_DELETE : S_KB_DONE));
            }
            ui_text_fit(kx + kw / 2, ky + ks * 0.22f, label, ks * 0.46f, UI_FONT_BOLD, sel ? UI_BLACK : UI_WHITE,
                        UI_ALIGN_CENTER, kw - 8);
            if (over && in->click) {
                pressed = r < 4 ? r * 10 + c : 40 + c;
            }
        }
    }
    /* A (not the keyboard's Enter, which is done) presses the selected key */
    if (in->ok && in->key != SDLK_RETURN && in->key != SDLK_KP_ENTER) {
        pressed = F.ky < 4 ? F.ky * 10 + F.kx : 40 + wide_of(F.kx);
    }
    if (pressed >= 0 && pressed < 40) {
        const char s[2] = {k_rows[pressed / 10][pressed % 10], 0};
        type(s);
    } else if (pressed == 40) {
        type(" ");
    } else if (pressed == 41) {
        rub_out();
    } else if (pressed == 42) {
        edit(false);
    }
}

void dash_find_open(bool typing)
{
    D.sel[SCR_FIND] = D.scroll[SCR_FIND] = 0;
    dash_push(SCR_FIND);
    edit(typing);
}

static void clear_all(void)
{
    F.q[0] = F.pub[0] = 0;
    F.year = F.region = 0;
}

void dash_find_clear(void)
{
    clear_all();
}

void scr_find(void)
{
    const float W = ui_w(), H = ui_h();
    D.editing_game = false;     /* the rows below are the global options */
    static LibGame *res[MAX_LIST];
    lib_show_hidden(cfg_bool(&D.global, "ui.showhidden", false));
    const int total = lib_count(), n = dash_find_list(res, MAX_LIST);
    char sub[96];
    snprintf(sub, sizeof(sub), T(S_SHOWING), n, total);
    draw_header(T(S_FIND), sub);
    const bool was_editing = F.editing;
    In in = D.in;
    if (F.editing) {
        /* the keys go to the keyboard, typed or on the screen */
        if (D.in.text) {
            type(D.in.text);
        }
        if (D.in.key == SDLK_BACKSPACE || D.in.key == SDLK_DELETE || D.in.x) {
            rub_out();
        }
        if (D.in.y) {
            type(" ");
        }
        if (D.in.back || D.in.key == SDLK_RETURN || D.in.key == SDLK_KP_ENTER) {
            edit(false);
            in.ok = false;
        }
        memset(&D.in, 0, sizeof(D.in));
        D.in.mx = D.in.my = -1;
        D.in.pad = in.pad;
    }
    const float lx = 70, lw = F.editing ? 520 : W - 140, ly = 140, lh = H - 140 - 110;
    List l;
    list_begin(&l, lx, ly, lw, lh, 52);
    l.focused = !F.editing && !D.ask;
    char shown[96];
    snprintf(shown, sizeof(shown), "%s%s", F.q[0] ? F.q : (F.editing ? "" : T(S_SEARCH_HINT)),
             F.editing && (int)(ui_time() * 2) % 2 ? "_" : "");
    if (list_row(&l, T(S_SEARCH), shown, NULL)) {
        edit(true);
    }
    int delta = 0;
    /* a publisher */
    const char *pubs[128];
    const int np = publishers(pubs, 128);
    int pi = -1;
    for (int i = 0; i < np; i++) {
        if (!strcmp(pubs[i], F.pub)) {
            pi = i;
        }
    }
    if (list_row(&l, T(S_PUBLISHER), F.pub[0] ? (F.pub[0] == '?' ? T(S_UNKNOWN) : F.pub) : T(S_ALL), &delta)) {
        delta = 1;
    }
    if (delta && np) {
        pi = pi + delta < -1 ? np - 1 : pi + delta >= np ? -1 : pi + delta;
        snprintf(F.pub, sizeof(F.pub), "%s", pi < 0 ? "" : pubs[pi]);
    }
    /* a year */
    int ys[16];
    const int ny = years(ys, 16);
    int yi = -1;
    for (int i = 0; i < ny; i++) {
        if (ys[i] == F.year) {
            yi = i;
        }
    }
    char yt[32];
    snprintf(yt, sizeof(yt), "%d", F.year);
    if (list_row(&l, T(S_YEAR), !F.year ? T(S_ALL) : F.year < 0 ? T(S_UNKNOWN) : yt, &delta)) {
        delta = 1;
    }
    if (delta && ny) {
        yi = yi + delta < -1 ? ny - 1 : yi + delta >= ny ? -1 : yi + delta;
        F.year = yi < 0 ? 0 : ys[yi];
    }
    /* a region */
    static const int k_regions[4] = {0, 1, 4, 2};
    int ri = 0;
    while (ri < 3 && k_regions[ri] != F.region) {
        ri++;
    }
    if (list_row(&l, T(S_REGION), region_name(F.region), &delta)) {
        delta = 1;
    }
    if (delta) {
        F.region = k_regions[(ri + delta + 4) % 4];
    }
    /* the order, the view */
    static const char *const sv[] = {"name", "publisher", "year"};
    const char *sn[] = {T(S_SORT_NAME), T(S_PUBLISHER), T(S_YEAR)};
    opt_choice(&l, S_SORT_BY, "ui.sort", sv, sn, 3, "name");
    static const char *const vv[] = {"large", "small", "icons"};
    const char *vn[] = {T(S_VIEW_LARGE), T(S_VIEW_SMALL), T(S_VIEW_ICONS)};
    opt_choice(&l, S_VIEW, "ui.view", vv, vn, 3, "large");
    opt_bool(&l, S_FAV_FIRST, "ui.favfirst", true, false);
    opt_bool(&l, S_LAST_FIRST, "ui.lastfirst", true, false);
    if (list_row(&l, T(S_CLEAR_FILTERS), NULL, NULL)) {
        clear_all();
    }
    if (list_row(&l, T(S_COVERS_ALL), NULL, NULL)) {
        lib_fetch_all_covers();
    }
    if (list_row(&l, T(S_SHOW_GAMES), NULL, NULL)) {
        dash_pop();
    }
    list_end(&l);
    if (F.editing) {
        /* the keyboard, and the first games the search finds under it */
        const float ks = 52, kx = lx + lw + 60;
        if (!was_editing) {
            /* started this frame (the search row chosen): the press isn't a key's */
            memset(&in, 0, sizeof(in));
            in.mx = in.my = -1;
            in.pad = D.in.pad;
        }
        osk(kx, ly, ks, &in);
        float y = ly + 5 * (ks + 6) + 18;
        for (int i = 0; i < n && i < 4 && y < H - 130; i++, y += 30) {
            ui_text_fit(kx, y, res[i]->g.title, 22, UI_FONT_REGULAR, i ? UI_GREY : UI_WHITE, UI_ALIGN_LEFT,
                        W - kx - 70);
        }
        if (!n) {
            ui_text(kx, y, T(S_NO_MATCH), 22, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT);
        }
        D.in = in;  /* the hints' glyphs: the input's own */
        if (in.pad) {
            draw_hints(T(S_KB_TYPE), T(S_KB_DONE), T(S_DELETE), T(S_KB_SPACE));
        } else {
            draw_hints(T(S_KB_DONE), NULL, T(S_DELETE), NULL);  /* Enter, Del: the letters are typed */
        }
        return;
    }
    if (D.in.back || D.in.y) {
        dash_pop();
    }
    draw_hints(T(S_CHANGE), T(S_BACK), NULL, T(S_SHOW_GAMES));
}
