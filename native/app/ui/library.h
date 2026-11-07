/*
 * hakuX front end — the game library (library.c): the game folders'
 * disc images, their titles and icons (cached), and box art downloaded
 * from the libretro thumbnails (Microsoft - Xbox, Named_Boxarts).
 */
#ifndef NATIVE_APP_UI_LIBRARY_H
#define NATIVE_APP_UI_LIBRARY_H

#include "native/app/ui/ui.h"
#include "native/app/ui/xbox_data.h"

#include <SDL3/SDL.h>
#include <stdbool.h>

typedef struct LibGame {
    XdGame g;               /* g.title: the name shown (the user's, if renamed) */
    UiTex *icon_tex, *cover_tex;
    bool cover_tried;       /* its file looked for */
    int64_t mtime;
    char orig[128];         /* renamed: the disc's own name; "" otherwise */
    bool hidden;            /* hidden from the Games screen (lib_show_hidden) */
    bool fav;               /* a favorite (first on the Games screen) */
    int64_t played;         /* when it was last played (time_t), 0 never */
    /* the library's own (library.c): a file is an entry for good (its
     * pointer stays valid until lib_shutdown); one that went away, or a
     * second copy of a game and version another entry shows, isn't shown */
    bool gone, dup;
    bool replaced;          /* its file changed: another entry has it now */
    bool fetch_tried;       /* box art looked for online this run */
    unsigned seen;          /* the scan pass that found it last */
} LibGame;

/* Scans the folders (folders: n paths, folders or game files) on a thread:
 * the games the last run found show at once (the cache), each file found
 * joins as it's read, and those not found any more leave at the end. A
 * scan asked for while one runs replaces it (the folders changed). */
void lib_scan(const char *const *folders, int n);
bool lib_scanning(void);
/* A game is being played: the scan goes easy on the disks meanwhile. */
void lib_set_busy(bool busy);
int lib_count(void);
LibGame *lib_get(int i);
/* The games shown (lib_count's), in their order (by title: numbers and
 * the like first, then A to Z); at most max. */
int lib_list(LibGame **out, int max);
/* A title's letter group: 0 for numbers and the rest, 1-26 A to Z. */
int lib_title_group(const char *title);
/* The publisher the title ID's first two letters stand for, "" if unknown. */
const char *lib_publisher(const LibGame *g);
/* Textures for the ones on screen (the UI's thread). */
UiTex *lib_icon(LibGame *g);
UiTex *lib_cover(LibGame *g);
/* Box art: g's (or every game's without one) fetched on a thread. */
void lib_fetch_cover(LibGame *g);
void lib_fetch_all_covers(void);
const char *lib_fetch_status(void);   /* NULL: idle */
void lib_poll(void);
void lib_shutdown(void);
/* A PNG (or JPEG) file as a texture, NULL if it can't be read. */
UiTex *lib_load_png(const char *path);
/* The user's own for a game, kept in <data>/covers/custom.txt: a name
 * (NULL or "": the disc's again), hidden or not; hidden games are left out
 * of lib_count / lib_get unless shown. */
void lib_set_name(LibGame *g, const char *name);
void lib_set_hidden(LibGame *g, bool hidden);
void lib_show_hidden(bool show);
/* A favorite or not; played now (launched). Kept in custom.txt too. */
void lib_set_favorite(LibGame *g, bool fav);
void lib_set_played(LibGame *g);
/* A picture file (PNG or JPEG) as the game's box art: false if it can't
 * be read. The UI's thread. */
bool lib_set_cover_file(LibGame *g, const char *path);
/* The game's title icon as a file: its PNG; *ico an ICO of it (made on
 * the first call). False without one. */
bool lib_icon_files(const LibGame *g, char *png, size_t pn, char *ico, size_t in);

#endif
