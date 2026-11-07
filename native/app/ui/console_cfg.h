/*
 * hakuX front end — the console's settings kept in two places (console_cfg.c):
 * the app's (xbased.ini's lang= and video=, a game's file over them) and the
 * console's EEPROM (<hdd>/eeprom.bin: its language and video flags), which a
 * title, the Xbox dashboard or a hand (an EEPROM editor) may change.
 */
#ifndef NATIVE_APP_UI_CONSOLE_CFG_H
#define NATIVE_APP_UI_CONSOLE_CFG_H

#include <stdbool.h>

#include "native/app/ui/config.h"

/* The video flags the settings show (widescreen, 720p, 1080i, 480p,
 * letterbox); the rest of the EEPROM's (PAL-60, the refresh) are the
 * kernel's to set. */
#define CC_VIDEO_USER 0x001F0000u
#define CC_VIDEO_DEFAULT 0x00080000u    /* 480p */

/* The console's language from a settings file: its lang= (1 English ... 9
 * Portuguese), else (none, or "auto") the interface's (i18n_set first). */
int cc_lang(const Config *c);
/* ... and its video flags: video=, else 480p */
unsigned cc_video(const Config *c);

/* The EEPROM and the global settings made one: what changed in the EEPROM
 * since the app last wrote it (a title's setting, an editor's) taken into
 * the settings (saved), then the settings written to it. True when the
 * settings changed. The dashboard's at its start, back from a game and on
 * each save, the game's before it reads its options. */
bool cc_sync(Config *global);

#endif
