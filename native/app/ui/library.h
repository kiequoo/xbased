/*
 * hakuX front end — the game library (library.c): the game folders'
 * disc images, their titles and icons (cached), and box art downloaded
 * from the libretro thumbnails (Microsoft - Xbox, Named_Boxarts).
 */
#ifndef NATIVE_APP_UI_LIBRARY_H
#define NATIVE_APP_UI_LIBRARY_H

#include "native/app/ui/xbox_data.h"

#include <SDL3/SDL.h>
#include <stdbool.h>

typedef struct LibGame {
    XdGame g;               /* g.title: the name shown (the user's, if renamed) */
    SDL_Texture *icon_tex, *cover_tex;
    bool cover_tried;       /* its file looked for */
    int64_t mtime;
    char orig[128];         /* renamed: the disc's own name; "" otherwise */
    bool hidden;            /* hidden from the Games screen (lib_show_hidden) */
} LibGame;

/* Scans the folders (folders: n paths) on a thread; lib_poll from the UI's
 * thread picks the results up. */
void lib_scan(const char *const *folders, int n);
bool lib_scanning(void);
int lib_count(void);
LibGame *lib_get(int i);
/* Textures for the ones on screen (the UI's thread). */
SDL_Texture *lib_icon(LibGame *g);
SDL_Texture *lib_cover(LibGame *g);
/* Box art: g's (or every game's without one) fetched on a thread. */
void lib_fetch_cover(LibGame *g);
void lib_fetch_all_covers(void);
const char *lib_fetch_status(void);   /* NULL: idle */
void lib_poll(void);
void lib_shutdown(void);
/* A PNG (or JPEG) file as a texture, NULL if it can't be read. */
SDL_Texture *lib_load_png(const char *path);
/* The user's own for a game, kept in <data>/covers/custom.txt: a name
 * (NULL or "": the disc's again), hidden or not; hidden games are left out
 * of lib_count / lib_get unless shown. */
void lib_set_name(LibGame *g, const char *name);
void lib_set_hidden(LibGame *g, bool hidden);
void lib_show_hidden(bool show);
/* A picture file (PNG or JPEG) as the game's box art: false if it can't
 * be read. The UI's thread. */
bool lib_set_cover_file(LibGame *g, const char *path);
/* The game's title icon as a file: its PNG; *ico an ICO of it (made on
 * the first call). False without one. */
bool lib_icon_files(const LibGame *g, char *png, size_t pn, char *ico, size_t in);

#endif
