/*
 * hakuX front end — xemu's saves brought in (xemu_import.c): the games'
 * saves (E:\UDATA, E:\TDATA) copied out of xemu's hard disk image into the
 * console's hard disk folder, and re-signed for xbased with the keys of the
 * user's own xemu files (its BIOS, MCPX boot ROM and EEPROM, as xemu.toml
 * names them). Copied without that, a game finds the saves damaged: xemu's
 * console signs them with its keys, xbased's kernel with its own.
 */
#ifndef NATIVE_APP_UI_XEMU_IMPORT_H
#define NATIVE_APP_UI_XEMU_IMPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XiReport {
    bool ok;
    char error[512];        /* !ok: why */
    int titles, files;      /* copied */
    int resigned;           /* signatures replaced */
    int unsigned_files;     /* files whose signature wasn't found (left as they were) */
    int no_key;             /* titles whose game isn't in the library (left as they were) */
    bool no_cert;           /* no BIOS key: only the console-locked part could change */
} XiReport;

/* xemu's settings file where xemu keeps it, into out; false: not there. */
bool xi_find_toml(char *out, size_t n);

/* The import, on a thread of its own: from the xemu.toml at toml into the
 * hard disk folder hdd (its Partition1 the E: drive). */
void xi_start(const char *toml, const char *hdd);
bool xi_running(void);
/* The last import's outcome, once (true when there was one to take). */
bool xi_take_report(XiReport *out);

/* Downloaded content and title updates copied in by hand for a title (any
 * ContentMeta.xbx under hdd's Partition1/TDATA/<title id>, its $c and $u
 * folders) signed for this console, as a game started from the dashboard
 * would find them: signed for another console (or by a tool with another
 * key), a game called them damaged. Returns how many were re-signed. */
int xi_resign_downloads(const char *hdd, uint32_t title_id);

#endif
