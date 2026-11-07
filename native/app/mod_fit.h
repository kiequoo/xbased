/*
 * xbased — whether a mod, patch or cheat fits the disc's executable
 * (mod_fit.c). A game's releases can share a title ID and still have
 * executables built apart (another region, a later printing): a patch or
 * cheat writes to fixed places of the one it was made for, and on another
 * it lands on other code. What tells them apart is default.xbe itself: its
 * certificate's regions and its header's build time (0x114), which the
 * scene's cheat files compare before they patch anything.
 */
#ifndef NATIVE_APP_MOD_FIT_H
#define NATIVE_APP_MOD_FIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIT_REGION_NA 1u            /* the certificate's game regions */
#define FIT_REGION_JAPAN 2u
#define FIT_REGION_REST 4u

typedef struct FitXbe {
    uint32_t title_id, version;
    uint32_t regions;               /* FIT_REGION_*: where the release is sold */
    uint32_t time;                  /* the header's build time: the build's own */
} FitXbe;

/* The disc image's default.xbe (fd: the image, left open). */
bool fit_read_xbe(int disc_fd, FitXbe *x);

/* The builds a cheat file (NcXCheat.data) was made for: the build times it
 * compares before it patches anything (the XBE header's, at 0x10114), as
 * plain compares or behind a XOR of a table of its own. Up to max into
 * times; returns how many. *checks: it compares its build at all (some do
 * where the time can't be read without running them); one that doesn't
 * writes blind, whatever the build. */
int fit_cheat_builds(const uint8_t *data, uint32_t size, uint32_t *times, int max, bool *checks);
/* not made for another build: made for this one, or one that checks but
 * whose builds can't be read (it patches nothing on another), or one that
 * doesn't check (the user's own: the database ships none) */
bool fit_cheat_for_build(const uint8_t *data, uint32_t size, uint32_t time);

/* An IPS patch of default.xbe against the disc's: false (why: the first
 * record that doesn't fit) if a record in code lands mid-instruction and
 * leaves the code after it decoding differently. The patches of a release's
 * executable pass on that one: they change an operand, or replace whole
 * instructions and end where one ended. */
bool fit_ips_code(int disc_fd, const uint8_t *ips, size_t len, char *why, size_t wn);

/* One of the database's patches (exe_dir/mods/patches.txt: "<title id>/<file>
 * <size> <FNV-1a 64>"), unchanged: rel is "<title id>/<file>" under mods/. */
bool fit_db_patch(const char *exe_dir, const char *rel);

/* FNV-1a 64 of a file (tools/db/build_db.py hashes the same way). */
bool fit_file_hash(const char *path, uint64_t *hash, uint64_t *size);

#endif
