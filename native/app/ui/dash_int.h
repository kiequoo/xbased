/*
 * hakuX front end — the dashboard's pieces (dash*.c): the input, the
 * screen stack, the lists every screen is made of.
 */
#ifndef NATIVE_APP_UI_DASH_INT_H
#define NATIVE_APP_UI_DASH_INT_H

#include "native/app/ui/config.h"
#include "native/app/ui/i18n.h"
#include "native/app/ui/library.h"
#include "native/app/ui/ui.h"

#include <stdbool.h>

typedef struct In {
    bool up, down, left, right, ok, back, x, y, lb, rb, start;
    bool lt, rt;            /* the triggers pulled (Z, C) */
    bool view;              /* the gamepad's View button (V) */
    bool find;              /* F3, Ctrl+F */
    float mx, my, wheel;    /* the mouse, in 720p units */
    bool click, moved, rclick;
    bool any;               /* something was pressed (screensaver, hints) */
    bool pad;               /* the last input came from a gamepad (the hints' glyphs) */
    SDL_Keycode key;        /* the key pressed (remapping), 0 none */
    int pad_button;         /* the gamepad button pressed, -1 none */
    int pad_axis;           /* an axis pushed past half: 1 + axis, negated for negative; 0 none */
    const char *text;       /* text typed (text fields) */
    bool repeat;            /* up/down/left/right came from a held key or stick repeating */
} In;

enum Screen {
    SCR_MAIN, SCR_GAMES, SCR_GAME, SCR_GAME_SETTINGS, SCR_MODS, SCR_STATES, SCR_SETTINGS, SCR_CONTROLS,
    SCR_REMAP, SCR_MEMORY, SCR_MEMORY_TITLE, SCR_ONLINE, SCR_ABOUT, SCR_INGAME, SCR_INGAME_SLOTS, SCR_STICKS,
    SCR_TRIGGERS, SCR_BROWSE, SCR_FIND, SCR_COUNT
};

typedef struct Dash {
    In in;
    enum Screen stack[16];
    int sp;
    int sel[SCR_COUNT], scroll[SCR_COUNT];
    float anim;                 /* the current screen's age, s */
    Config global, game;        /* xbased.ini, the selected game's */
    LibGame *cur;               /* the game the game screens are about */
    int tab;                    /* the settings' tab */
    int player;                 /* the controls' player (0-3) */
    bool editing_game;          /* lists edit the game's file over the global one */
    /* a question (yes/no), its callback; or one with choices (dash_choose) */
    const char *ask;
    void (*on_yes)(struct Dash *);
    const char *choices[3];
    int nchoices;                               /* 0: yes/no */
    void (*on_choice)(struct Dash *, int);      /* the choice, -1 backed out */
    char ask_buf[256];
    char toast[1024];
    double toast_until;
    char exe[1024];
    bool quit;
    /* remapping: the input being bound */
    int remap_target;
    double remap_since;
    /* text entry */
    char *edit_buf;
    size_t edit_cap;
    const char *edit_key;
    char edit_tmp[256];
    bool typing;                /* a text field takes the letters: they don't move or press */
} Dash;

extern Dash D;

/* screens */
void dash_push(enum Screen s);
void dash_pop(void);
enum Screen dash_top(void);
void dash_toast(const char *msg);
/* An hour and minute as the user reads clocks (Interface, 12-hour clock). */
const char *dash_hm(int hour, int minute);
void dash_ask(const char *question, void (*on_yes)(Dash *));
/* A question with 2 or 3 answers, the first one selected; the question is
 * copied. */
void dash_choose(const char *question, const char *const *choices, int n, void (*on_choice)(Dash *, int));
void dash_save_cfg(void);
/* the gamepad used last (its sticks shown), NULL when none */
SDL_Gamepad *dash_active_pad(void);

/* the lists: a panel of rows, the current screen's selection and scroll */
typedef struct List {
    float x, y, w, h, row_h;
    int count;      /* rows so far this frame */
    int *sel, *scroll;
    int visible;
    bool focused;
} List;
void list_begin(List *l, float x, float y, float w, float h, float row_h);
/* A row: its label and value; true when chosen (A, Enter, a click). delta:
 * left/right on it (-1, +1), may be NULL. */
bool list_row(List *l, const char *label, const char *value, int *delta);
bool list_row_icon(List *l, UiTex *icon, const char *label, const char *sub, const char *value, int *delta);
void list_end(List *l);
bool list_selected(const List *l);   /* the last row is the selected one */

/* settings rows over D.global or (editing a game) D.game */
void opt_choice(List *l, enum UiStr label, const char *key, const char *const *vals, const char *const *names, int n,
                const char *def);
void opt_bool(List *l, enum UiStr label, const char *key, bool def, bool flag_style);
void opt_int(List *l, enum UiStr label, const char *key, int lo, int hi, int step, int def, const char *unit);
void opt_videoflag(List *l, enum UiStr label, unsigned bit);

/* drawing helpers */
void draw_header(const char *title, const char *sub);
/* the hints' row: A, B, X, Y (NULL: left out); its end's x */
float draw_hints(const char *a, const char *b, const char *x, const char *y);
/* one more hint at px (which: as draw_button_glyph's); its end's x */
float draw_hint_at(float px, int which, const char *text);
/* 0 A, 1 B, 2 X, 3 Y, 4 the triggers (LT RT), 5 View */
void draw_button_glyph(float x, float y, int which, float size);
float ease(float t);

/* the screens */
void scr_settings(void);
void scr_controls(void);
void scr_remap(void);
void scr_sticks(void);
void scr_triggers(void);
void scr_memory(void);
void scr_memory_title(void);
void scr_online(void);
void scr_game_settings(void);
void scr_mods(void);
void scr_states(void);
/* The Games screen's search, filters, order and view (dash_find.c):
 * dash_find_list fills out with the games they let through, in order;
 * dash_find_group is a game's group in that order (the triggers jump
 * between groups), dash_find_group_name its name; dash_find_desc the
 * search and filters in a few words ("" for none). */
int dash_find_list(LibGame **out, int max);
int dash_find_group(const LibGame *g);
const char *dash_find_group_name(const LibGame *g);
const char *dash_find_desc(void);
int dash_find_view(void);   /* 0 large covers, 1 small covers, 2 icons and names */
void dash_find_next_view(void);
/* The search screen pushed; typing: the keyboard up at once */
void dash_find_open(bool typing);
void dash_find_clear(void);    /* no search, no filters */
void scr_find(void);
/* The file browser (dash_browse.c), the dashboard's own for the gamepad,
 * the keyboard and the mouse: dash_browse_games adds folders and game
 * files to the library; dash_browse_file has one file chosen (exts:
 * "png;jpg", NULL any) and hands it to cb as a file dialog would. */
void dash_browse_games(void);
void dash_browse_file(const char *title, const char *exts, SDL_DialogFileCallback cb, void *ud);
void scr_browse(void);
/* a game file started without adding it to the library: one chosen, or
 * dropped on the window; dash_take_folder starts it from the UI's thread */
void dash_play_file_later(const char *path);
void dash_pick_play_file(void);
void dash_take_folder(void);

/* the game's files */
void game_cfg_path(const LibGame *g, char *out, size_t n);
void states_dir(const LibGame *g, char *out, size_t n);
void launch_game(LibGame *g, const char *extra);

#endif
