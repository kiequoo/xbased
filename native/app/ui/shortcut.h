/*
 * hakuX front end — a game's shortcut on the desktop (shortcut.c): xbased
 * with the game's file as its argument, which boots it straight away.
 */
#ifndef NATIVE_APP_UI_SHORTCUT_H
#define NATIVE_APP_UI_SHORTCUT_H

#include <stdbool.h>
#include <stddef.h>

/* name: the shortcut's (the game's title); game: its disc image; icon_png,
 * icon_ico: its icon (either may be NULL: xbased's). Windows: a .lnk on the
 * desktop; Linux: a .desktop file there and in the applications menu.
 * *where: the file made. */
bool sc_make(const char *name, const char *game, const char *icon_png, const char *icon_ico, char *where,
             size_t n);

#endif
