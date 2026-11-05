/*
 * hakuX front end — xemu's saves brought in (xemu_import.h).
 *
 * xemu.toml's [sys.files] names the files: bootrom_path (the MCPX 1.0 boot
 * ROM), flashrom_path (the BIOS), eeprom_path (the EEPROM, default beside
 * xemu.toml) and hdd_path (the hard disk: qcow2, or raw). The disk's E:
 * partition is FATX; its UDATA and TDATA trees are copied into the hard
 * disk folder's Partition1, then re-signed as the Android app's
 * SaveResigner does (android/.../data/hdd/SaveResigner.kt):
 *
 * Games sign with XAPI's XCalculateSignature: roamable = HMAC-SHA1(title
 * key, data), the title key being HMAC-SHA1(certificate key, the XBE
 * certificate's signature key)[0:16]; locked to the console = HMAC-SHA1(HD
 * key, roamable). Downloaded content's ContentMeta.xbx: HMAC-SHA1(HMAC-SHA1
 * (HD key, title id), its header). xbased's kernel signs as a console whose
 * title keys are zero and whose HD key is its EEPROM's (native/kernel/ex.c,
 * nc_kernel_set_hd_key). A signature is looked for where games put it (the
 * start, 4, the end, the end before padding, a .sig file beside) and
 * replaced only when it checks with xemu's keys; a file whose signature
 * isn't found stays as it was.
 *
 * The certificate key comes from the BIOS's 2BL (RC4 under the boot ROM's
 * key at 0x1A5; the 24 KiB below the flash's last 512 bytes), found by the
 * EEPROM key before it (its HMAC midstates); the HD key from the EEPROM's
 * encrypted section. Neither is kept.
 */
#include "native/app/ui/xemu_import.h"
#include "native/app/ui/config.h"
#include "native/app/ui/library.h"
#include "native/kernel/xc_crypto.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <utime.h>
#endif

/* raw deflate, for a qcow2 image's compressed clusters */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#define E_OFFSET 0xABE80000ull      /* the E: partition (Partition1) on the disk */
#define E_SIZE 0x1312D6000ull
#define MAX_FILE (128u << 20)
#define MAX_TITLES 512
#define MAX_FILES 8192

static uint32_t rd32le(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64be(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = v << 8 | p[i];
    }
    return v;
}

static uint32_t rd32be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* ------------------------------------------------------------ the keys */

/* The retail and debug kernels' EEPROM keys by their HMAC-SHA1 midstates
 * (inner, outer), as xemu's hw/xbox/eeprom_generation.c gives them. */
static const uint32_t k_eeprom_keys[][2][5] = {
    {{0x72127625, 0x336472B9, 0xBE609BEA, 0xF55E226B, 0x99958DAC},
     {0x76441D41, 0x4DE82659, 0x2E8EF85E, 0xB256FACA, 0xC4FE2DE8}},
    {{0x39B06E79, 0xC9BD25E8, 0xDBC6B498, 0x40B4389D, 0x86BBD7ED},
     {0x9B49BED3, 0x84B430FC, 0x6B8749CD, 0xEBFE5FE5, 0xD96E7393}},
    {{0x8058763A, 0xF97D4E0E, 0x865A9762, 0x8A3D920D, 0x08995B2C},
     {0x01075307, 0xA2F1E037, 0x1186EEEA, 0x88DA9992, 0x168A5609}},
    {{0x85F9E51A, 0xE04613D2, 0x6D86A50C, 0x77C32E3C, 0x4BD717A4},
     {0x5D7A9C6B, 0xE1922BEB, 0xB82CCDBC, 0x3137AB34, 0x486B52B3}},
};
#define N_EEPROM_KEYS (sizeof(k_eeprom_keys) / sizeof(k_eeprom_keys[0]))

static void hmac_mid(unsigned key, const uint8_t *data, size_t len, uint8_t out[20])
{
    NcSha1 s;
    uint8_t inner[20];
    nc_sha1_init(&s);
    memcpy(s.h, k_eeprom_keys[key][0], sizeof(s.h));
    s.len = 64;
    nc_sha1_update(&s, data, len);
    nc_sha1_final(&s, inner);
    nc_sha1_init(&s);
    memcpy(s.h, k_eeprom_keys[key][1], sizeof(s.h));
    s.len = 64;
    nc_sha1_update(&s, inner, sizeof(inner));
    nc_sha1_final(&s, out);
}

/* The HD key in an EEPROM image's encrypted section; false: it checks with
 * none of the keys (kept in the clear). */
static bool eeprom_hd_key(const uint8_t *e, uint8_t key[16])
{
    for (unsigned k = 0; k < N_EEPROM_KEYS; k++) {
        uint8_t seed[20], plain[0x1C], hash[20];
        hmac_mid(k, e, 20, seed);
        memcpy(plain, e + 20, sizeof(plain));
        NcRc4 r;
        nc_rc4_key(&r, seed, sizeof(seed));
        nc_rc4_crypt(&r, plain, sizeof(plain));
        hmac_mid(k, plain, sizeof(plain), hash);
        if (!memcmp(hash, e, 20)) {
            memcpy(key, plain + 8, 16);
            return true;
        }
    }
    return false;
}

/* The certificate key in a BIOS for the MCPX 1.0 boot ROM. */
static bool bios_cert_key(const uint8_t *mcpx, size_t mn, const uint8_t *flash, size_t fn, uint8_t key[16])
{
    enum { BL_SIZE = 0x6000, BL_END = 0x200, RC4_AT = 0x1A5 };
    if (mn != 512 || fn < 0x40000) {
        return false;
    }
    uint8_t *bl = malloc(BL_SIZE);
    if (!bl) {
        return false;
    }
    memcpy(bl, flash + fn - BL_END - BL_SIZE, BL_SIZE);
    NcRc4 r;
    nc_rc4_key(&r, mcpx + RC4_AT, 16);
    nc_rc4_crypt(&r, bl, BL_SIZE);
    bool found = false;
    for (size_t off = 0; off + 48 <= BL_SIZE && !found; off++) {
        uint8_t pad[64];
        memset(pad, 0x36, sizeof(pad));
        for (int i = 0; i < 16; i++) {
            pad[i] ^= bl[off + i];
        }
        NcSha1 s;
        nc_sha1_init(&s);
        nc_sha1_update(&s, pad, sizeof(pad));      /* one block: s.h is the midstate */
        for (unsigned k = 0; k < N_EEPROM_KEYS; k++) {
            if (!memcmp(s.h, k_eeprom_keys[k][0], sizeof(s.h))) {
                memcpy(key, bl + off + 16, 16);
                found = true;
                break;
            }
        }
    }
    free(bl);
    return found;
}

/* xbased's kernel's HD key for the EEPROM it keeps as <hdd>/eeprom.bin (ex.c):
 * its encrypted section's; for an image that kept the section in the clear,
 * its bytes 0x36-0x45; before its first run, the one it will be made with. */
static void native_hd_key(const char *hdd, uint8_t key[16])
{
    static const uint8_t legacy[16] = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '1', 0x00, 0x50, 0xF2, 0x00,
                                       0x00, 0x01};
    char p[1100];
    snprintf(p, sizeof(p), "%s/eeprom.bin", hdd);
    size_t n = 0;
    uint8_t *e = SDL_LoadFile(p, &n);
    if (!e || n < 0x46) {
        memcpy(key, legacy, 16);
    } else if (!eeprom_hd_key(e, key)) {
        memcpy(key, e + 0x36, 16);
    }
    SDL_free(e);
}

/* ------------------------------------------------------------ the disk */

typedef struct Disk {
    SDL_IOStream *io;
    bool qcow;
    int cbits;                  /* qcow2: cluster bits */
    uint64_t *l1;
    uint32_t l1n;
    struct {
        uint64_t off;
        uint64_t *t;
    } l2c[16];
    unsigned l2next;
    uint8_t *cl, *zbuf;         /* a cluster's bytes; a compressed one's */
    char err[200];
} Disk;

static bool io_at(SDL_IOStream *io, uint64_t off, void *buf, size_t n)
{
    return SDL_SeekIO(io, (Sint64)off, SDL_IO_SEEK_SET) == (Sint64)off && SDL_ReadIO(io, buf, n) == n;
}

static bool disk_open(Disk *d, const char *path)
{
    memset(d, 0, sizeof(*d));
    d->io = SDL_IOFromFile(path, "rb");
    if (!d->io) {
        snprintf(d->err, sizeof(d->err), "xemu's hard disk image can't be opened (is xemu still running?): %s", path);
        return false;
    }
    uint8_t h[104];
    if (!io_at(d->io, 0, h, sizeof(h))) {
        snprintf(d->err, sizeof(d->err), "xemu's hard disk image is too short");
        return false;
    }
    if (memcmp(h, "QFI\xfb", 4)) {
        return true;    /* raw */
    }
    d->qcow = true;
    const uint32_t ver = rd32be(h + 4);
    d->cbits = (int)rd32be(h + 20);
    if (ver < 2 || ver > 3 || rd64be(h + 8) || d->cbits < 9 || d->cbits > 21 || rd32be(h + 32) ||
        (ver == 3 && (rd64be(h + 72) & 0x6))) {
        snprintf(d->err, sizeof(d->err), "xemu's hard disk image is a qcow2 image of a kind xbased can't read "
                 "(a backing file, encryption or an external data file)");
        return false;
    }
    d->l1n = rd32be(h + 36);
    const uint64_t l1off = rd64be(h + 40);
    uint8_t *t = malloc((size_t)d->l1n * 8 + 1);
    d->l1 = malloc((size_t)d->l1n * 8 + 1);
    d->cl = malloc((size_t)1 << d->cbits);
    d->zbuf = malloc((size_t)2 << d->cbits);
    if (!t || !d->l1 || !d->cl || !d->zbuf || !io_at(d->io, l1off, t, (size_t)d->l1n * 8)) {
        free(t);
        snprintf(d->err, sizeof(d->err), "xemu's hard disk image is damaged (its L1 table)");
        return false;
    }
    for (uint32_t i = 0; i < d->l1n; i++) {
        d->l1[i] = rd64be(t + i * 8);
    }
    free(t);
    return true;
}

static void disk_close(Disk *d)
{
    if (d->io) {
        SDL_CloseIO(d->io);
    }
    free(d->l1);
    for (int i = 0; i < 16; i++) {
        free(d->l2c[i].t);
    }
    free(d->cl);
    free(d->zbuf);
}

static const uint64_t *l2_table(Disk *d, uint64_t off)
{
    for (int i = 0; i < 16; i++) {
        if (d->l2c[i].t && d->l2c[i].off == off) {
            return d->l2c[i].t;
        }
    }
    const size_t cs = (size_t)1 << d->cbits;
    unsigned slot = d->l2next++ % 16;
    if (!d->l2c[slot].t) {
        d->l2c[slot].t = malloc(cs);
    }
    uint64_t *t = d->l2c[slot].t;
    if (!t || !io_at(d->io, off, t, cs)) {
        d->l2c[slot].off = 0;
        return NULL;
    }
    for (size_t i = 0; i < cs / 8; i++) {
        t[i] = rd64be((const uint8_t *)&t[i]);
    }
    d->l2c[slot].off = off;
    return t;
}

/* A guest cluster into d->cl. */
static bool qcow_cluster(Disk *d, uint64_t gc)
{
    const size_t cs = (size_t)1 << d->cbits;
    const int l2bits = d->cbits - 3;
    const uint64_t l1i = gc >> l2bits;
    uint64_t e = 0;
    if (l1i < d->l1n && (d->l1[l1i] & 0x00FFFFFFFFFFFE00ull)) {
        const uint64_t *t = l2_table(d, d->l1[l1i] & 0x00FFFFFFFFFFFE00ull);
        if (!t) {
            return false;
        }
        e = t[gc & (((uint64_t)1 << l2bits) - 1)];
    }
    if (e & (1ull << 62)) {
        /* compressed: raw deflate, sectors from the host offset */
        const int x = 62 - (d->cbits - 8);
        const uint64_t host = e & (((uint64_t)1 << x) - 1);
        const uint64_t sectors = ((e >> x) & (((uint64_t)1 << (d->cbits - 8)) - 1)) + 1;
        size_t len = (size_t)(sectors * 512 - (host & 511));
        if (len > (2u << d->cbits)) {
            len = 2u << d->cbits;
        }
        SDL_SeekIO(d->io, (Sint64)host, SDL_IO_SEEK_SET);
        len = SDL_ReadIO(d->io, d->zbuf, len);
        const int n = stbi_zlib_decode_noheader_buffer((char *)d->cl, (int)cs, (const char *)d->zbuf, (int)len);
        if (n < 0) {
            return false;
        }
        memset(d->cl + n, 0, cs - (size_t)n);
        return true;
    }
    const uint64_t off = e & 0x00FFFFFFFFFFFE00ull;
    if (!off || (e & 1)) {
        memset(d->cl, 0, cs);
        return true;
    }
    return io_at(d->io, off, d->cl, cs);
}

static bool disk_read(Disk *d, uint64_t off, void *buf, size_t n)
{
    if (!d->qcow) {
        return io_at(d->io, off, buf, n);
    }
    const size_t cs = (size_t)1 << d->cbits;
    uint8_t *o = buf;
    while (n) {
        const uint64_t gc = off >> d->cbits;
        const size_t in = (size_t)(off & (cs - 1));
        const size_t k = n < cs - in ? n : cs - in;
        if (!qcow_cluster(d, gc)) {
            return false;
        }
        memcpy(o, d->cl + in, k);
        o += k;
        off += k;
        n -= k;
    }
    return true;
}

/* ------------------------------------------------------------ FATX */

typedef struct Fatx {
    Disk *d;
    uint64_t base;
    uint32_t root, csize;
    uint64_t data, nclusters;
    bool wide;
    uint8_t *fat;
} Fatx;

typedef struct FxEntry {
    char name[43];
    uint8_t attr;
    uint32_t first, size, written;
} FxEntry;

static bool fatx_open(Fatx *f, Disk *d, uint64_t base, uint64_t size)
{
    memset(f, 0, sizeof(*f));
    f->d = d;
    f->base = base;
    uint8_t sb[16];
    if (!disk_read(d, base, sb, sizeof(sb)) || memcmp(sb, "FATX", 4)) {
        return false;
    }
    const uint32_t spc = rd32le(sb + 8);
    if (!spc || spc > 1024 || (spc & (spc - 1))) {
        return false;
    }
    f->root = rd32le(sb + 12);
    f->csize = spc * 512;
    const uint64_t clusters = size / f->csize;
    f->wide = clusters >= 0xFFF0;
    const uint64_t es = f->wide ? 4 : 2;
    const uint64_t fat_size = ((clusters + 1) * es + 0xFFF) & ~0xFFFull;
    f->data = 0x1000 + fat_size;
    f->nclusters = (size - f->data) / f->csize;
    f->fat = malloc((size_t)fat_size);
    return f->fat && disk_read(d, base + 0x1000, f->fat, (size_t)fat_size);
}

static uint32_t fatx_next(const Fatx *f, uint32_t c)
{
    return f->wide ? rd32le(f->fat + (size_t)c * 4) : (uint32_t)(f->fat[c * 2] | f->fat[c * 2 + 1] << 8);
}

static bool fatx_valid(const Fatx *f, uint32_t c)
{
    return c >= 1 && c <= f->nclusters;
}

static uint32_t fatx_follow(const Fatx *f, uint32_t c)
{
    const uint32_t n = fatx_next(f, c);
    return !n || n >= (f->wide ? 0xFFFFFFF0u : 0xFFF0u) ? 0 : n;
}

static bool fatx_cluster(Fatx *f, uint32_t c, uint8_t *buf)
{
    return disk_read(f->d, f->base + f->data + (uint64_t)(c - 1) * f->csize, buf, f->csize);
}

/* A directory's entries (malloc'ed, *n of them). */
static FxEntry *fatx_list(Fatx *f, uint32_t first, int *n)
{
    *n = 0;
    int cap = 64;
    FxEntry *out = malloc(sizeof(FxEntry) * (size_t)cap);
    uint8_t *buf = malloc(f->csize);
    unsigned guard = 0;
    for (uint32_t c = first; out && buf && fatx_valid(f, c) && guard++ < 65536; c = fatx_follow(f, c)) {
        if (!fatx_cluster(f, c, buf)) {
            break;
        }
        for (uint32_t o = 0; o + 64 <= f->csize; o += 64) {
            const uint8_t len = buf[o];
            if (len == 0 || len == 0xFF) {
                goto done;
            }
            if (len == 0xE5 || len > 42) {
                continue;
            }
            if (*n == cap) {
                cap *= 2;
                FxEntry *g = realloc(out, sizeof(FxEntry) * (size_t)cap);
                if (!g) {
                    goto done;
                }
                out = g;
            }
            FxEntry *e = &out[(*n)++];
            memcpy(e->name, buf + o + 2, len);
            e->name[len] = 0;
            e->attr = buf[o + 1];
            e->first = rd32le(buf + o + 0x2C);
            e->size = rd32le(buf + o + 0x30);
            e->written = rd32le(buf + o + 0x34);
        }
    }
done:
    free(buf);
    return out;
}

/* FAT time (low word) and date (high word), years from 2000, as a time_t (UTC). */
static time_t fat_time(uint32_t v)
{
    if (!v || v == 0xFFFFFFFFu) {
        return 0;
    }
    struct tm t = {0};
    const uint32_t tm = v & 0xFFFF, dt = v >> 16;
    t.tm_year = 100 + (int)(dt >> 9);
    t.tm_mon = (int)((dt >> 5) & 0xF) - 1;
    t.tm_mday = (int)(dt & 0x1F);
    t.tm_hour = (int)(tm >> 11);
    t.tm_min = (int)((tm >> 5) & 0x3F);
    t.tm_sec = (int)(tm & 0x1F) * 2;
#ifdef _WIN32
    return _mkgmtime(&t);
#else
    return timegm(&t);
#endif
}

static bool name_ok(const char *s)
{
    if (!*s || !strcmp(s, ".") || !strcmp(s, "..")) {
        return false;
    }
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || strchr("<>:\"/\\|?*", *s)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------ the run */

typedef struct Run {
    Fatx fx;
    char files[MAX_FILES][300];     /* copied files, relative to Partition1 ("UDATA/4d530004/...") */
    int nfiles;
    uint32_t titles[MAX_TITLES];
    int ntitles;
    bool failed;
    char err[512];
    FILE *log;                      /* <data>/xemu_import.log: each file's outcome */
} Run;

static void add_title(Run *r, uint32_t id)
{
    for (int i = 0; i < r->ntitles; i++) {
        if (r->titles[i] == id) {
            return;
        }
    }
    if (r->ntitles < MAX_TITLES) {
        r->titles[r->ntitles++] = id;
    }
}

static void set_time(const char *path, uint32_t fat)
{
    const time_t t = fat_time(fat);
    if (t > 0) {
        struct utimbuf u = {t, t};
        utime(path, &u);
    }
}

/* A FATX tree (dir's first cluster) into host folder to; rel the path from
 * Partition1 for the list of files. */
static void copy_tree(Run *r, uint32_t first, const char *to, const char *rel, int depth)
{
    if (depth > 8 || r->failed) {
        return;
    }
    SDL_CreateDirectory(to);
    int n = 0;
    FxEntry *e = fatx_list(&r->fx, first, &n);
    uint8_t *buf = malloc(r->fx.csize);
    for (int i = 0; e && buf && i < n && !r->failed; i++) {
        if (!name_ok(e[i].name)) {
            continue;
        }
        char p[1300], rp[300];
        snprintf(p, sizeof(p), "%s/%s", to, e[i].name);
        snprintf(rp, sizeof(rp), "%s/%s", rel, e[i].name);
        if (e[i].attr & 0x10) {
            copy_tree(r, e[i].first, p, rp, depth + 1);
            continue;
        }
        SDL_IOStream *out = SDL_IOFromFile(p, "wb");
        if (!out) {
            r->failed = true;
            snprintf(r->err, sizeof(r->err), "%s can't be written", p);
            break;
        }
        uint32_t left = e[i].size;
        unsigned guard = 0;
        for (uint32_t c = e[i].first; left && fatx_valid(&r->fx, c) && guard++ < 1u << 20; c = fatx_follow(&r->fx, c)) {
            if (!fatx_cluster(&r->fx, c, buf)) {
                r->failed = true;
                snprintf(r->err, sizeof(r->err), "xemu's hard disk image can't be read (%s)", rp);
                break;
            }
            const uint32_t k = left < r->fx.csize ? left : r->fx.csize;
            SDL_WriteIO(out, buf, k);
            left -= k;
        }
        SDL_CloseIO(out);
        set_time(p, e[i].written);
        if (r->nfiles < MAX_FILES) {
            snprintf(r->files[r->nfiles++], sizeof(r->files[0]), "%s", rp);
        }
    }
    free(buf);
    free(e);
}

/* ------------------------------------------------------------ re-signing */

typedef struct Keys {
    uint8_t cert[16], src_hd[16], dst_hd[16];
    bool have_cert;
    /* per title: its auth key (HMAC(cert, title's signature key)[0:16]) */
    uint32_t ids[MAX_TITLES];
    uint8_t auth[MAX_TITLES][16];
    bool have[MAX_TITLES];
    int n;
} Keys;

static const uint8_t k_zero[16];

static void hmac(const uint8_t *key, size_t kl, const uint8_t *d, size_t n, uint8_t out[20])
{
    nc_hmac_sha1(key, kl, d, n, NULL, 0, out);
}

/* The library's games' keys for the titles imported: the games whose own
 * title is one first, then the others' alternate titles. */
static void title_keys(Run *r, Keys *k)
{
    k->n = r->ntitles;
    memcpy(k->ids, r->titles, sizeof(uint32_t) * (size_t)r->ntitles);
    bool alt_have[MAX_TITLES] = {0};
    uint8_t alt[MAX_TITLES][16];
    for (int pass = 0; pass < 2; pass++) {
        const int games = lib_count();
        for (int gi = 0; gi < games; gi++) {
            LibGame *lg = lib_get(gi);
            if (!lg) {
                continue;
            }
            int own = -1;
            for (int i = 0; i < k->n; i++) {
                own = k->ids[i] == lg->g.title_id ? i : own;
            }
            if ((pass == 0) != (own >= 0) || (own >= 0 && k->have[own])) {
                continue;
            }
            bool missing = false;
            for (int i = 0; i < k->n; i++) {
                missing |= !k->have[i];
            }
            if (!missing) {
                goto merge;
            }
            XdGame g;
            if (!xd_read_game(lg->g.path, &g) || !g.has_keys) {
                xd_free_game(&g);
                continue;
            }
            xd_free_game(&g);
            for (int i = 0; i < k->n; i++) {
                uint8_t mac[20];
                if (!k->have[i] && g.title_id == k->ids[i]) {
                    hmac(k->cert, 16, g.sig_key, 16, mac);
                    memcpy(k->auth[i], mac, 16);
                    k->have[i] = true;
                }
                for (int a = 0; a < 16; a++) {
                    if (g.alt_ids[a] && g.alt_ids[a] == k->ids[i] && !alt_have[i]) {
                        hmac(k->cert, 16, g.alt_keys[a], 16, mac);
                        memcpy(alt[i], mac, 16);
                        alt_have[i] = true;
                    }
                }
            }
        }
    }
merge:
    for (int i = 0; i < k->n; i++) {
        if (!k->have[i] && alt_have[i]) {
            memcpy(k->auth[i], alt[i], 16);
            k->have[i] = true;
        }
    }
}

enum { ROAM, NOROAM };

typedef struct Hit {
    int file;               /* this file's index in the run's list */
    uint32_t a, b;          /* the data it covers */
    int kind;
    int holder;             /* -1: the signature is in this file; else in that file of the list */
    uint32_t off;
} Hit;

static uint8_t *load(const char *p, size_t *n)
{
    return SDL_LoadFile(p, n);
}

static uint32_t unpadded(const uint8_t *d, uint32_t n, uint8_t pad)
{
    while (n > 0 && d[n - 1] == pad) {
        n--;
    }
    return n;
}

static int find_bytes(const uint8_t *hay, size_t n, const uint8_t s[20])
{
    for (size_t i = 0; i + 20 <= n; i++) {
        if (!memcmp(hay + i, s, 20)) {
            return (int)i;
        }
    }
    return -1;
}

static bool is_meta(const char *name)
{
    static const char *const k[] = {"titlemeta.xbx", "titleimage.xbx", "savemeta.xbx", "saveimage.xbx"};
    for (int i = 0; i < 4; i++) {
        if (!SDL_strcasecmp(name, k[i])) {
            return true;
        }
    }
    return false;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static bool same_dir(const char *a, const char *b)
{
    const size_t la = (size_t)(base_name(a) - a), lb = (size_t)(base_name(b) - b);
    return la == lb && !memcmp(a, b, la);
}

/* Where file fi's signature is, under auth and the source HD key. */
static bool find_sig(Run *r, const char *root, int fi, const uint8_t *d, uint32_t n, const uint8_t auth[16],
                     const uint8_t hd[16], Hit *hit)
{
    uint32_t ms[3] = {n, unpadded(d, n, 0), unpadded(d, n, 0x20)};
    for (int mi = 0; mi < 3; mi++) {
        const uint32_t m = ms[mi];
        if (m <= 20 || (mi >= 1 && m == ms[0]) || (mi == 2 && m == ms[1])) {
            continue;
        }
        const int64_t ends[4] = {(int64_t)m - 24, (int64_t)m - 20, (int64_t)m - 4, m};
        static const uint32_t starts[4] = {0, 4, 20, 24};
        for (int ai = 0; ai < 4; ai++) {
            const uint32_t a = starts[ai];
            for (int bi = 0; bi < 4; bi++) {
                const int64_t b = ends[bi];
                if (b <= (int64_t)a) {
                    continue;
                }
                uint8_t sig[2][20];
                hmac(auth, 16, d + a, (size_t)(b - a), sig[ROAM]);
                hmac(hd, 16, sig[ROAM], 20, sig[NOROAM]);
                const int64_t ps[4] = {0, 4, (int64_t)m - 24, (int64_t)m - 20};
                for (int pi = 0; pi < 4; pi++) {
                    const int64_t p = ps[pi];
                    if (p < 0 || p + 20 > m || (p + 20 > a && p < b)) {
                        continue;
                    }
                    for (int kind = 0; kind < 2; kind++) {
                        if (!memcmp(d + p, sig[kind], 20)) {
                            *hit = (Hit){fi, a, (uint32_t)b, kind, -1, (uint32_t)p};
                            return true;
                        }
                    }
                }
                if (a == 0 && b == n) {
                    /* in a small file beside it (a .sig file) */
                    for (int s = 0; s < r->nfiles; s++) {
                        if (s == fi || !same_dir(r->files[s], r->files[fi])) {
                            continue;
                        }
                        char sp[1400];
                        snprintf(sp, sizeof(sp), "%s/%s", root, r->files[s]);
                        size_t sn = 0;
                        uint8_t *sd = load(sp, &sn);
                        if (sd && sn >= 20 && sn <= 4096) {
                            for (int kind = 0; kind < 2; kind++) {
                                const int q = find_bytes(sd, sn, sig[kind]);
                                if (q >= 0) {
                                    *hit = (Hit){fi, a, (uint32_t)b, kind, s, (uint32_t)q};
                                    SDL_free(sd);
                                    return true;
                                }
                            }
                        }
                        SDL_free(sd);
                    }
                }
            }
        }
    }
    return false;
}

static void write_at(const char *p, uint32_t off, const uint8_t sig[20])
{
    SDL_PathInfo pi;
    const bool dated = SDL_GetPathInfo(p, &pi);
    SDL_IOStream *io = SDL_IOFromFile(p, "r+b");
    if (io) {
        SDL_SeekIO(io, off, SDL_IO_SEEK_SET);
        SDL_WriteIO(io, sig, 20);
        SDL_CloseIO(io);
    }
    if (dated) {
        const time_t t = (time_t)(pi.modify_time / 1000000000);
        struct utimbuf u = {t, t};
        utime(p, &u);
    }
}

/* ContentMeta.xbx: its header [0x14, header size) signed at 0 with
 * HMAC(HMAC(HD key, title id at 0x24), header). */
static bool resign_content(const char *p, const uint8_t *d, size_t n, const Keys *k)
{
    if (n < 0x28) {
        return false;
    }
    const uint32_t size = rd32le(d + 0x18);
    if (size <= 0x14 || size > n) {
        return false;
    }
    uint8_t ck[20], sig[20];
    hmac(k->src_hd, 16, d + 0x24, 4, ck);
    hmac(ck, 20, d + 0x14, size - 0x14, sig);
    if (memcmp(d, sig, 20)) {
        return false;
    }
    hmac(k->dst_hd, 16, d + 0x24, 4, ck);
    hmac(ck, 20, d + 0x14, size - 0x14, sig);
    write_at(p, 0, sig);
    return true;
}

/* Signatures over a file's header, at its end: [0, p) at p for p up to 64,
 * either kind (Conker: Live & Reloaded signs its first 12 bytes at 12, then
 * the whole file, the first signature in it, at the end). Into hits. */
static int find_header_sigs(int fi, const uint8_t *d, uint32_t n, const uint8_t auth[16], const uint8_t hd[16],
                            const Hit *primary, Hit *hits)
{
    int k = 0;
    for (uint32_t p = 4; p <= 64 && p + 20 <= n; p += 4) {
        if (primary && p + 20 > primary->off && p < primary->off + 20 && primary->holder < 0) {
            continue;   /* the primary's own place */
        }
        uint8_t sig[2][20];
        hmac(auth, 16, d, p, sig[ROAM]);
        hmac(hd, 16, sig[ROAM], 20, sig[NOROAM]);
        for (int kind = 0; kind < 2; kind++) {
            if (!memcmp(d + p, sig[kind], 20)) {
                hits[k++] = (Hit){fi, 0, p, kind, -1, p};
                break;
            }
        }
        if (k == 2) {
            break;
        }
    }
    return k;
}

static int cmp_span(const void *x, const void *y)
{
    const Hit *a = x, *b = y;
    const uint32_t sa = a->b - a->a, sb = b->b - b->a;
    return sa < sb ? -1 : sa > sb;
}

#define LOG(...) (r->log ? (void)fprintf(r->log, __VA_ARGS__) : (void)0)

static void resign(Run *r, const char *root, Keys *k, XiReport *rep)
{
    Hit *hits = calloc((size_t)r->nfiles * 3 + 1, sizeof(Hit));
    int nh = 0;
    bool nokey[MAX_TITLES] = {0};
    for (int fi = 0; hits && fi < r->nfiles; fi++) {
        const char *rel = r->files[fi];
        /* AREA/<title id>/... */
        const char *s1 = strchr(rel, '/');
        const char *s2 = s1 ? strchr(s1 + 1, '/') : NULL;
        if (!s2 || s2 - s1 != 9 || is_meta(base_name(rel))) {
            continue;
        }
        char idt[9];
        memcpy(idt, s1 + 1, 8);
        idt[8] = 0;
        char *endp;
        const uint32_t tid = (uint32_t)strtoul(idt, &endp, 16);
        if (*endp) {
            continue;
        }
        char p[1400];
        snprintf(p, sizeof(p), "%s/%s", root, rel);
        size_t n = 0;
        uint8_t *d = load(p, &n);
        if (!d || n <= 20 || n > MAX_FILE) {
            SDL_free(d);
            continue;
        }
        if (!SDL_strcasecmp(base_name(rel), "ContentMeta.xbx")) {
            if (resign_content(p, d, n, k)) {
                rep->resigned++;
                LOG("%s: content signature replaced\n", rel);
            } else {
                rep->unsigned_files++;
                LOG("%s: content signature not found\n", rel);
            }
            SDL_free(d);
            continue;
        }
        int ti = -1;
        for (int i = 0; i < k->n; i++) {
            ti = k->ids[i] == tid ? i : ti;
        }
        if (ti < 0 || !k->have[ti]) {
            if (ti >= 0) {
                nokey[ti] = true;
            }
            LOG("%s: its game isn't in the library\n", rel);
            SDL_free(d);
            continue;
        }
        Hit h;
        const bool found = find_sig(r, root, fi, d, (uint32_t)n, k->auth[ti], k->src_hd, &h);
        const int nhead = find_header_sigs(fi, d, (uint32_t)n, k->auth[ti], k->src_hd, found ? &h : NULL, hits + nh);
        for (int i = 0; i < nhead; i++) {
            LOG("%s: %s signature of its header [0, %u) at %u\n", rel, hits[nh + i].kind == NOROAM ? "console" : "roaming",
                hits[nh + i].b, hits[nh + i].off);
        }
        nh += nhead;
        if (found) {
            hits[nh++] = h;
            LOG("%s: %s signature of [%u, %u) at %s+%u\n", rel, h.kind == NOROAM ? "console" : "roaming", h.a, h.b,
                h.holder >= 0 ? r->files[h.holder] : "itself", h.off);
        } else if (!nhead) {
            rep->unsigned_files++;
            LOG("%s: no signature found (%u bytes)\n", rel, (unsigned)n);
        }
        SDL_free(d);
    }
    /* a signature over another one's place after it (narrower ranges first),
     * and a file holding others' signatures signs itself last, over theirs */
    qsort(hits, (size_t)nh, sizeof(Hit), cmp_span);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nh; i++) {
            bool holder = false;
            for (int j = 0; j < nh; j++) {
                holder |= hits[j].holder == hits[i].file;
            }
            if ((hits[i].holder < 0 && holder) != (pass == 1)) {
                continue;
            }
            char p[1400], sp[1400];
            snprintf(p, sizeof(p), "%s/%s", root, r->files[hits[i].file]);
            size_t n = 0;
            uint8_t *d = load(p, &n);
            if (!d || hits[i].b > n) {
                SDL_free(d);
                continue;
            }
            uint8_t sig[20], nr[20];
            hmac(k_zero, 16, d + hits[i].a, hits[i].b - hits[i].a, sig);
            if (hits[i].kind == NOROAM) {
                hmac(k->dst_hd, 16, sig, 20, nr);
                memcpy(sig, nr, 20);
            }
            SDL_free(d);
            if (hits[i].holder >= 0) {
                snprintf(sp, sizeof(sp), "%s/%s", root, r->files[hits[i].holder]);
            }
            write_at(hits[i].holder >= 0 ? sp : p, hits[i].off, sig);
            rep->resigned++;
        }
    }
    for (int i = 0; i < k->n; i++) {
        rep->no_key += nokey[i];
    }
    free(hits);
}

/* ------------------------------------------------------------ xemu.toml */

/* key = 'value' or "value" in [sys.files] */
static bool toml_value(const char *text, const char *key, char *out, size_t n)
{
    const char *sec = strstr(text, "[sys.files]");
    if (!sec) {
        return false;
    }
    for (const char *s = strchr(sec, '\n'); s && *s;) {
        s++;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s == '[') {
            return false;
        }
        const size_t kl = strlen(key);
        if (!strncmp(s, key, kl) && (s[kl] == ' ' || s[kl] == '=' || s[kl] == '\t')) {
            const char *v = strchr(s, '=');
            if (!v) {
                return false;
            }
            v++;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            const char q = *v;
            if (q != '\'' && q != '"') {
                return false;
            }
            v++;
            size_t m = 0;
            for (; *v && *v != q && *v != '\n' && m + 1 < n; v++) {
                if (q == '"' && *v == '\\' && v[1]) {
                    v++;    /* basic strings: \\ and \" */
                }
                out[m++] = *v;
            }
            out[m] = 0;
            return m > 0;
        }
        s = strchr(s, '\n');
    }
    return false;
}

bool xi_find_toml(char *out, size_t n)
{
    char c[4][1100];
    int k = 0;
#ifdef _WIN32
    const char *app = getenv("APPDATA");
    if (app) {
        snprintf(c[k++], sizeof(c[0]), "%s\\xemu\\xemu\\xemu.toml", app);
    }
#else
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) {
        snprintf(c[k++], sizeof(c[0]), "%s/xemu/xemu/xemu.toml", xdg);
    }
    if (home) {
        snprintf(c[k++], sizeof(c[0]), "%s/.local/share/xemu/xemu/xemu.toml", home);
        snprintf(c[k++], sizeof(c[0]), "%s/.var/app/app.xemu.xemu/data/xemu/xemu/xemu.toml", home);
    }
#endif
    for (int i = 0; i < k; i++) {
        if (SDL_GetPathInfo(c[i], NULL)) {
            snprintf(out, n, "%s", c[i]);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------ the thread */

static SDL_AtomicInt g_running, g_have_report;
static XiReport g_report;
static char g_toml[1100], g_hdd[1100];

/* Partition1's folder for a FATX top-level name, as the disk folder has it
 * already (any case), else the name. */
static void area_dir(const char *p1, const char *name, char *out, size_t n)
{
    int cnt = 0;
    char **names = SDL_GlobDirectory(p1, NULL, 0, &cnt);
    for (int i = 0; names && i < cnt; i++) {
        if (!SDL_strcasecmp(names[i], name)) {
            snprintf(out, n, "%s/%s", p1, names[i]);
            SDL_free(names);
            return;
        }
    }
    SDL_free(names);
    snprintf(out, n, "%s/%s", p1, name);
}

static int import_thread(void *arg)
{
    XiReport rep = {0};
    Run *r = calloc(1, sizeof(Run));
    Keys *k = calloc(1, sizeof(Keys));
    Disk disk;
    memset(&disk, 0, sizeof(disk));
    size_t tn = 0;
    char *toml = SDL_LoadFile(g_toml, &tn);
    char hddp[1100] = "", eep[1100] = "", mcpxp[1100] = "", flashp[1100] = "";
    if (!r || !k || !toml) {
        snprintf(rep.error, sizeof(rep.error), "xemu's settings (%s) can't be read", g_toml);
        goto out;
    }
    if (!toml_value(toml, "hdd_path", hddp, sizeof(hddp))) {
        snprintf(rep.error, sizeof(rep.error), "xemu's settings name no hard disk image (hdd_path)");
        goto out;
    }
    if (!toml_value(toml, "eeprom_path", eep, sizeof(eep))) {
        /* xemu's default: eeprom.bin beside xemu.toml */
        snprintf(eep, sizeof(eep), "%s", g_toml);
        char *s = strrchr(eep, '/'), *b = strrchr(eep, '\\');
        s = b > s ? b : s;
        snprintf(s ? s + 1 : eep, sizeof(eep) - (size_t)(s ? s + 1 - eep : 0), "eeprom.bin");
    }
    toml_value(toml, "bootrom_path", mcpxp, sizeof(mcpxp));
    toml_value(toml, "flashrom_path", flashp, sizeof(flashp));
    /* the keys: xemu's console's, and this one's */
    size_t en = 0, mn = 0, fn = 0;
    uint8_t *ee = SDL_LoadFile(eep, &en);
    if (!ee || en < 0x30 || !eeprom_hd_key(ee, k->src_hd)) {
        SDL_free(ee);
        snprintf(rep.error, sizeof(rep.error), "xemu's EEPROM (%s) can't be read", eep);
        goto out;
    }
    SDL_free(ee);
    uint8_t *mcpx = mcpxp[0] ? SDL_LoadFile(mcpxp, &mn) : NULL;
    uint8_t *flash = flashp[0] ? SDL_LoadFile(flashp, &fn) : NULL;
    k->have_cert = mcpx && flash && bios_cert_key(mcpx, mn, flash, fn, k->cert);
    SDL_free(mcpx);
    SDL_free(flash);
    rep.no_cert = !k->have_cert;
    native_hd_key(g_hdd, k->dst_hd);
    /* the disk */
    if (!disk_open(&disk, hddp)) {
        snprintf(rep.error, sizeof(rep.error), "%s", disk.err);
        goto out;
    }
    if (!fatx_open(&r->fx, &disk, E_OFFSET, E_SIZE)) {
        snprintf(rep.error, sizeof(rep.error), "xemu's hard disk image has no E: drive xbased can read");
        goto out;
    }
    char p1[1200];
    snprintf(p1, sizeof(p1), "%s/Partition1", g_hdd);
    SDL_CreateDirectory(p1);
    int n = 0;
    FxEntry *top = fatx_list(&r->fx, r->fx.root, &n);
    for (int i = 0; top && i < n && !r->failed; i++) {
        if (!(top[i].attr & 0x10) || (SDL_strcasecmp(top[i].name, "UDATA") && SDL_strcasecmp(top[i].name, "TDATA"))) {
            continue;
        }
        char area[1300];
        area_dir(p1, top[i].name, area, sizeof(area));
        const char *an = base_name(area);
        int tn2 = 0;
        FxEntry *ts = fatx_list(&r->fx, top[i].first, &tn2);
        for (int t = 0; ts && t < tn2 && !r->failed; t++) {
            char *endp;
            const uint32_t id = (uint32_t)strtoul(ts[t].name, &endp, 16);
            if (!(ts[t].attr & 0x10) || strlen(ts[t].name) != 8 || *endp || !name_ok(ts[t].name)) {
                continue;
            }
            char to[1400], rel[64];
            snprintf(to, sizeof(to), "%s/%s", area, ts[t].name);
            snprintf(rel, sizeof(rel), "%s/%s", an, ts[t].name);
            copy_tree(r, ts[t].first, to, rel, 0);
            add_title(r, id);
        }
        free(ts);
    }
    free(top);
    if (r->failed) {
        snprintf(rep.error, sizeof(rep.error), "%s", r->err);
        goto out;
    }
    rep.files = r->nfiles;
    rep.titles = r->ntitles;
    char lp[1200];
    snprintf(lp, sizeof(lp), "%sxemu_import.log", cfg_data_dir());
    r->log = fopen(lp, "w");
    LOG("xemu's settings: %s\nhard disk image: %s (%s)\nBIOS key: %s\n", g_toml, hddp, disk.qcow ? "qcow2" : "raw",
        k->have_cert ? "found" : "not found");
    if (k->have_cert) {
        title_keys(r, k);
    } else {
        k->n = r->ntitles;
        memcpy(k->ids, r->titles, sizeof(uint32_t) * (size_t)r->ntitles);
    }
    resign(r, p1, k, &rep);
    LOG("%d files of %d titles, %d signatures replaced, %d files without one found, %d titles without their game\n",
        rep.files, rep.titles, rep.resigned, rep.unsigned_files, rep.no_key);
    rep.ok = true;
    SDL_Log("xemu import: %d files of %d titles, %d signatures replaced, %d without one found, %d titles "
                "without their game, BIOS key %s", rep.files, rep.titles, rep.resigned, rep.unsigned_files, rep.no_key,
                k->have_cert ? "found" : "missing");
out:
    if (r && r->log) {
        fclose(r->log);
    }
    disk_close(&disk);
    free(r ? r->fx.fat : NULL);
    free(r);
    if (k) {
        memset(k, 0, sizeof(*k));      /* the keys aren't kept */
    }
    free(k);
    SDL_free(toml);
    g_report = rep;
    SDL_SetAtomicInt(&g_have_report, 1);
    SDL_SetAtomicInt(&g_running, 0);
    return 0;
}

void xi_start(const char *toml, const char *hdd)
{
    if (!SDL_CompareAndSwapAtomicInt(&g_running, 0, 1)) {
        return;
    }
    snprintf(g_toml, sizeof(g_toml), "%s", toml);
    snprintf(g_hdd, sizeof(g_hdd), "%s", hdd);
    SDL_Thread *t = SDL_CreateThread(import_thread, "xemu-import", NULL);
    if (t) {
        SDL_DetachThread(t);
    } else {
        SDL_SetAtomicInt(&g_running, 0);
    }
}

bool xi_running(void)
{
    return SDL_GetAtomicInt(&g_running) != 0;
}

bool xi_take_report(XiReport *out)
{
    if (!SDL_CompareAndSwapAtomicInt(&g_have_report, 1, 0)) {
        return false;
    }
    *out = g_report;
    return true;
}

/* ------------------------------------------------------------ downloads */

/* ContentMeta.xbx files under dir, depth levels down: each one signed for
 * this console (HMAC(HMAC(HD key, its title id at 0x24), its header) at 0),
 * re-signed where it isn't. */
static int resign_tree(const char *dir, int depth, const uint8_t hd[16])
{
    int done = 0, n = 0;
    char **names = SDL_GlobDirectory(dir, NULL, 0, &n);
    for (int i = 0; names && i < n; i++) {
        char p[1400];
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(p, &pi)) {
            continue;
        }
        if (pi.type == SDL_PATHTYPE_DIRECTORY && depth > 0) {
            done += resign_tree(p, depth - 1, hd);
            continue;
        }
        if (pi.type != SDL_PATHTYPE_FILE || SDL_strcasecmp(names[i], "ContentMeta.xbx")) {
            continue;
        }
        size_t len = 0;
        uint8_t *d = load(p, &len);
        const uint32_t size = d && len >= 0x28 ? rd32le(d + 0x18) : 0;
        if (size > 0x14 && size <= len) {
            uint8_t ck[20], sig[20];
            hmac(hd, 16, d + 0x24, 4, ck);
            hmac(ck, 20, d + 0x14, size - 0x14, sig);
            if (memcmp(d, sig, 20)) {
                write_at(p, 0, sig);
                done++;
            }
        }
        SDL_free(d);
    }
    SDL_free(names);
    return done;
}

int xi_resign_downloads(const char *hdd, uint32_t title_id)
{
    char tdata[1200];
    snprintf(tdata, sizeof(tdata), "%s/Partition1/TDATA", hdd);
    char want[16];
    snprintf(want, sizeof(want), "%08x", title_id);
    uint8_t hd[16];
    native_hd_key(hdd, hd);
    int done = 0, n = 0;
    char **names = SDL_GlobDirectory(tdata, NULL, 0, &n);
    for (int i = 0; names && i < n; i++) {
        if (!SDL_strcasecmp(names[i], want)) {     /* the folder's name in either case */
            char p[1300];
            snprintf(p, sizeof(p), "%s/%s", tdata, names[i]);
            done += resign_tree(p, 3, hd);
        }
    }
    SDL_free(names);
    return done;
}
