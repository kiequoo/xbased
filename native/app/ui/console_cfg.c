/*
 * hakuX front end — the console's language and video flags, the settings'
 * and the EEPROM's made one.
 *
 * The kernel serves the titles the EEPROM <hdd>/eeprom.bin holds, with the
 * app's settings over it for the run (native/kernel/ex.c); it writes the
 * file only when a title saves a setting (ExSaveNonVolatileSetting: the Xbox
 * dashboard's, a game's options), and never this run's settings. So the
 * file says what the console was set to last, by either side. Beside it,
 * <hdd>/eeprom.sync keeps what the app wrote in it last: the file different
 * from that was changed by a title or by hand since, and the settings take
 * it; then the settings, whichever way they came, are written to the file.
 * A file without the record (from before, or a new one) takes the settings.
 * A game's own settings (games/<key>.ini) stay its own: only the global
 * ones meet the EEPROM, the game's go over them for its run.
 */
#include "native/app/ui/console_cfg.h"
#include "native/app/ui/i18n.h"
#include "native/core/nc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EE_SIZE 0x100
#define EE_USER_SUM 0x60        /* the user section's checksum, over 0x64-0xBF */
#define EE_LANGUAGE 0x90
#define EE_VIDEO 0x94

int cc_lang(const Config *c)
{
    const char *v = cfg_get(c, "lang");
    const int l = v ? atoi(v) : 0;
    return l >= 1 && l <= 9 ? l : i18n_console_lang();
}

unsigned cc_video(const Config *c)
{
    const char *v = cfg_get(c, "video");
    return v ? (unsigned)strtoul(v, NULL, 16) : CC_VIDEO_DEFAULT;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* the Xbox's section checksum (ex.c's eeprom_checksum) */
static uint32_t checksum(const uint8_t *p, unsigned len)
{
    uint32_t high = 0, low = 0;
    for (unsigned i = 0; i + 4 <= len; i += 4) {
        const uint32_t v = rd32(p + i);
        high += (uint32_t)(((uint64_t)low + v) >> 32);
        low += v;
    }
    return ~(high + low);
}

static void hdd_dir(const Config *global, char *out, size_t n)
{
    const char *h = cfg_get(global, "hdd");
    if (h && *h) {
        snprintf(out, n, "%s", h);
    } else {
        snprintf(out, n, "%shdd", cfg_data_dir());
    }
}

/* a whole file in place of the old one, or nothing */
static bool write_file(const char *path, const void *data, size_t n)
{
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(data, 1, n, f) == n;
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
    }
    return ok;
}

bool cc_sync(Config *global)
{
    char hdd[1024], ee_path[1100], sync_path[1100];
    hdd_dir(global, hdd, sizeof(hdd));
    snprintf(ee_path, sizeof(ee_path), "%s/eeprom.bin", hdd);
    snprintf(sync_path, sizeof(sync_path), "%s/eeprom.sync", hdd);
    uint8_t ee[EE_SIZE];
    FILE *f = fopen(ee_path, "rb");
    const bool have = f && fread(ee, 1, sizeof(ee), f) == sizeof(ee) && fgetc(f) == EOF;
    if (f) {
        fclose(f);
    }
    if (!have) {
        return false;   /* none yet (the kernel makes it at the first boot), or not an EEPROM */
    }
    const uint32_t ee_lang = rd32(ee + EE_LANGUAGE), ee_video = rd32(ee + EE_VIDEO) & CC_VIDEO_USER;

    /* what changed in the EEPROM since the app wrote it: the settings' now */
    bool changed = false;
    unsigned was_lang = 0, was_video = 0;
    f = fopen(sync_path, "r");
    const bool known = f && fscanf(f, "lang=%u video=%x", &was_lang, &was_video) == 2;
    if (f) {
        fclose(f);
    }
    if (known && ee_lang != was_lang && ee_lang >= 1 && ee_lang <= 9) {
        char v[8];
        snprintf(v, sizeof(v), "%u", (unsigned)ee_lang);
        cfg_set(global, "lang", v);
        changed = true;
        nc_log("hakuX-app", "console language %u from the EEPROM (it was %u): the settings take it", ee_lang, was_lang);
    }
    if (known && ee_video != (was_video & CC_VIDEO_USER)) {
        char v[16];
        snprintf(v, sizeof(v), "0x%x", (cc_video(global) & ~CC_VIDEO_USER) | ee_video);
        cfg_set(global, "video", v);
        changed = true;
        nc_log("hakuX-app", "video flags %08x from the EEPROM (they were %08x): the settings take them", ee_video,
               was_video & CC_VIDEO_USER);
    }
    if (changed) {
        cfg_save(global);
    }

    /* the settings into the EEPROM, and the record of it */
    const uint32_t lang = (uint32_t)cc_lang(global), video = cc_video(global) & CC_VIDEO_USER;
    const bool summed = rd32(ee + EE_USER_SUM) == checksum(ee + EE_USER_SUM + 4, 0x5C);   /* an editor's may not be */
    if (lang != ee_lang || video != ee_video || !summed) {
        wr32(ee + EE_LANGUAGE, lang);
        wr32(ee + EE_VIDEO, (rd32(ee + EE_VIDEO) & ~CC_VIDEO_USER) | video);
        wr32(ee + EE_USER_SUM, checksum(ee + EE_USER_SUM + 4, 0x5C));
        if (!write_file(ee_path, ee, sizeof(ee))) {
            nc_log("hakuX-app", "can't write the console settings to %s", ee_path);
            return changed;
        }
    }
    if (!known || was_lang != lang || (was_video & CC_VIDEO_USER) != video) {
        char rec[64];
        const int n = snprintf(rec, sizeof(rec), "lang=%u video=%x\n", (unsigned)lang, (unsigned)video);
        write_file(sync_path, rec, (size_t)n);
    }
    return changed;
}
