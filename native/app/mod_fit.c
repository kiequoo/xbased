/*
 * xbased — whether a mod, patch or cheat fits the disc's executable
 * (mod_fit.h).
 */
#include "native/app/mod_fit.h"
#include "native/core/xdvdfs.h"

#include <Zydis/Zydis.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* default.xbe opened on a copy of the image's fd (the caller's stays open) */
static bool open_xbe(int disc_fd, NcDisc *d, NcDiscFile *f)
{
    const int fd = disc_fd >= 0 ? dup(disc_fd) : -1;
    memset(d, 0, sizeof(*d));
    if (fd < 0) {
        return false;
    }
    if (!nc_disc_open(d, fd)) {
        close(fd);
        return false;
    }
    if (!nc_disc_find(d, "default.xbe", f)) {
        nc_disc_close(d);
        return false;
    }
    return true;
}

bool fit_read_xbe(int disc_fd, FitXbe *x)
{
    memset(x, 0, sizeof(*x));
    NcDisc d;
    NcDiscFile f;
    if (!open_xbe(disc_fd, &d, &f)) {
        return false;
    }
    uint8_t h[0x1000];
    bool ok = false;
    if (f.size >= sizeof(h) && nc_disc_read(&d, &f, 0, h, sizeof(h)) && !memcmp(h, "XBEH", 4)) {
        const uint32_t c = rd32(h + 0x118) - rd32(h + 0x104);
        if (c + 0xB0 <= sizeof(h)) {
            x->title_id = rd32(h + c + 8);
            x->regions = rd32(h + c + 0xA0);
            x->version = rd32(h + c + 0xAC);
            x->time = rd32(h + 0x114);
            ok = true;
        }
    }
    nc_disc_close(&d);
    return ok;
}

/* ---- cheats ---- */

#define TIME_VA 0x00010114u     /* the XBE header's build time, where it loads */
#define SAVED_VA 0x00010148u    /* where the XOR-obscured ones keep the result */

typedef struct {
    ZydisDecodedInstruction in;
    ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];
} Insn;

static bool decode_at(const ZydisDecoder *dec, const uint8_t *b, uint32_t n, uint32_t at, Insn *i)
{
    return at < n && ZYAN_SUCCESS(ZydisDecoderDecodeFull(dec, b + at, n - at, &i->in, i->op));
}

static bool is_abs(const ZydisDecodedOperand *o, uint32_t va)
{
    return o->type == ZYDIS_OPERAND_TYPE_MEMORY && o->mem.base == ZYDIS_REGISTER_NONE &&
           o->mem.index == ZYDIS_REGISTER_NONE && (uint32_t)o->mem.disp.value == va;
}

static bool names_va(const Insn *i, uint32_t va)
{
    for (int k = 0; k < i->in.operand_count_visible; k++) {
        const ZydisDecodedOperand *o = &i->op[k];
        if (is_abs(o, va) || (o->type == ZYDIS_OPERAND_TYPE_IMMEDIATE && (uint32_t)o->imm.value.u == va)) {
            return true;
        }
    }
    return false;
}

/* a cmp of a 32-bit operand with an immediate: the immediate */
static bool cmp_imm(const Insn *i, uint32_t *imm)
{
    if (i->in.mnemonic != ZYDIS_MNEMONIC_CMP || i->in.operand_count_visible < 2 ||
        i->op[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        return false;
    }
    *imm = (uint32_t)i->op[1].imm.value.u;
    return true;
}

static bool is_gpr32(ZydisRegister r)
{
    return r == ZYDIS_REGISTER_EAX || r == ZYDIS_REGISTER_EBX || r == ZYDIS_REGISTER_ECX || r == ZYDIS_REGISTER_EDX ||
           r == ZYDIS_REGISTER_ESI || r == ZYDIS_REGISTER_EDI || r == ZYDIS_REGISTER_EBP;
}

static bool plausible(uint32_t t)
{
    return t >= 0x3A000000u && t < 0x47000000u;     /* 2000 .. 2007 */
}

static void add_time(uint32_t t, uint32_t *times, int max, int *n)
{
    for (int i = 0; i < *n; i++) {
        if (times[i] == t) {
            return;
        }
    }
    if (*n < max) {
        times[(*n)++] = t;
    }
}

int fit_cheat_builds(const uint8_t *b, uint32_t size, uint32_t *times, int max, bool *checks)
{
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    int n = 0;
    bool found = false;
    /* plain: cmp dword [0x10114], K; or mov reg, [0x10114] (or its address)
     * and within three instructions a cmp of reg or [reg] with K */
    for (uint32_t k = 0; k + 4 <= size; k++) {
        if (rd32(b + k) != TIME_VA) {
            continue;
        }
        for (uint32_t back = 1; back < 8 && back <= k; back++) {
            Insn i0;
            if (!decode_at(&dec, b, size, k - back, &i0) || i0.in.length <= back || !names_va(&i0, TIME_VA)) {
                continue;
            }
            uint32_t imm;
            if (i0.in.mnemonic == ZYDIS_MNEMONIC_CMP && is_abs(&i0.op[0], TIME_VA) && i0.op[0].size == 32) {
                if (cmp_imm(&i0, &imm)) {
                    found = true;
                    if (plausible(imm)) {
                        add_time(imm, times, max, &n);
                    }
                }
                break;
            }
            if (i0.in.mnemonic != ZYDIS_MNEMONIC_MOV || i0.op[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                !is_gpr32(i0.op[0].reg.value)) {
                continue;
            }
            const ZydisRegister reg = i0.op[0].reg.value;
            uint32_t at = k - back + i0.in.length;
            for (int j = 0; j < 3; j++) {
                Insn nx;
                if (!decode_at(&dec, b, size, at, &nx)) {
                    break;
                }
                at += nx.in.length;
                const ZydisDecodedOperand *o = &nx.op[0];
                const bool of_reg = (o->type == ZYDIS_OPERAND_TYPE_REGISTER && o->reg.value == reg) ||
                                    (o->type == ZYDIS_OPERAND_TYPE_MEMORY && o->size == 32 && o->mem.base == reg &&
                                     o->mem.index == ZYDIS_REGISTER_NONE && !o->mem.disp.value);
                if (nx.in.mnemonic == ZYDIS_MNEMONIC_CMP && of_reg) {
                    if (cmp_imm(&nx, &imm)) {
                        found = true;
                        if (plausible(imm)) {
                            add_time(imm, times, max, &n);
                        }
                    }
                    break;
                }
            }
            break;
        }
    }
    /* XOR-obscured: mov edi, 0x10114; lea esi, [ebp+A]; lea ecx, [ebp+B];
     * lea edx, [ebp+C]; sub ecx, esi; mov ebx, [0x10144]; shr ecx, 2; a loop
     * XORs the time with each dword of A..B XOR the dword at C; the last cmp
     * of [edi] or [0x10148] before the first conditional jump names what it
     * must come to (ebp: the file's start) */
    static const uint8_t head[5] = {0xBF, 0x14, 0x01, 0x01, 0x00};
    for (uint32_t k = 0; k + 43 <= size; k++) {
        if (memcmp(b + k, head, 5) || b[k + 5] != 0x8D || b[k + 6] != 0xB5 || b[k + 11] != 0x8D || b[k + 12] != 0x8D ||
            b[k + 17] != 0x8D || b[k + 18] != 0x95 || memcmp(b + k + 23, "\x2B\xCE\x8B\x1D\x44\x01\x01\x00\xC1\xE9\x02", 11)) {
            continue;
        }
        found = true;
        const uint32_t A = rd32(b + k + 7), B = rd32(b + k + 13), C = rd32(b + k + 19);
        if (B < A || B > size || C + 4 > size) {
            continue;
        }
        bool have_k = false;
        uint32_t K = 0, at = k + 34;
        const uint32_t stop = at + 96 < size ? at + 96 : size;
        Insn i;
        while (at < stop && ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, b + at, stop - at, &i.in, i.op))) {
            at += i.in.length;
            uint32_t imm;
            const ZydisDecodedOperand *o = &i.op[0];
            if (i.in.mnemonic == ZYDIS_MNEMONIC_CMP && o->type == ZYDIS_OPERAND_TYPE_MEMORY && o->size == 32 &&
                ((o->mem.base == ZYDIS_REGISTER_EDI && o->mem.index == ZYDIS_REGISTER_NONE && !o->mem.disp.value) ||
                 is_abs(o, SAVED_VA)) &&
                cmp_imm(&i, &imm)) {
                K = imm;
                have_k = true;
            }
            const ZydisMnemonic m = i.in.mnemonic;
            if (m == ZYDIS_MNEMONIC_RET || m == ZYDIS_MNEMONIC_JMP ||
                (i.in.meta.category == ZYDIS_CATEGORY_COND_BR && m != ZYDIS_MNEMONIC_LOOP &&
                 m != ZYDIS_MNEMONIC_LOOPE && m != ZYDIS_MNEMONIC_LOOPNE)) {
                break;
            }
        }
        if (!have_k) {
            continue;
        }
        uint32_t x = 0;
        for (uint32_t a = A; a + 4 <= B; a += 4) {
            x ^= rd32(b + a) ^ rd32(b + C);
        }
        if (plausible(K ^ x)) {
            add_time(K ^ x, times, max, &n);
        }
    }
    *checks = found;
    return n;
}

bool fit_cheat_for_build(const uint8_t *data, uint32_t size, uint32_t time)
{
    uint32_t t[32];
    bool checks;
    const int n = fit_cheat_builds(data, size, t, 32, &checks);
    for (int i = 0; i < n; i++) {
        if (t[i] == time) {
            return true;
        }
    }
    return n == 0;
}

/* ---- IPS patches ---- */

typedef struct {
    uint32_t off, len;
    const uint8_t *bytes;   /* NULL: a run of rle */
    uint8_t rle;
} IpsRec;

static int ips_parse(const uint8_t *p, size_t len, IpsRec *recs, int max)
{
    if (len < 8 || memcmp(p, "PATCH", 5)) {
        return -1;
    }
    int n = 0;
    size_t at = 5;
    while (at + 3 <= len && memcmp(p + at, "EOF", 3)) {
        if (at + 5 > len || n >= max) {
            return -1;
        }
        IpsRec *r = &recs[n++];
        r->off = (uint32_t)p[at] << 16 | (uint32_t)p[at + 1] << 8 | p[at + 2];
        r->len = (uint32_t)p[at + 3] << 8 | p[at + 4];
        at += 5;
        if (!r->len) {
            if (at + 3 > len) {
                return -1;
            }
            r->len = (uint32_t)p[at] << 8 | p[at + 1];
            r->rle = p[at + 2];
            r->bytes = NULL;
            at += 3;
        } else {
            if (at + r->len > len) {
                return -1;
            }
            r->bytes = p + at;
            at += r->len;
        }
    }
    return n;
}

#define WIN_BACK 160u           /* the sweeps start up to this far before */
#define WIN_AFTER 80u

/* The instruction starts the sweeps from several points before at agree on
 * (a sweep falls into step with the real ones within a few instructions;
 * four of the seven decide). bounds: offsets into buf, sorted. */
static int sweep_bounds(const ZydisDecoder *dec, const uint8_t *buf, uint32_t n, uint32_t at, uint32_t *bounds,
                        int max)
{
    static const uint32_t backs[7] = {24, 40, 56, 72, 96, 128, 160};
    static uint8_t votes[WIN_BACK + 0x10000 + WIN_AFTER + 32];
    memset(votes, 0, n);
    for (int b = 0; b < 7; b++) {
        uint32_t pc = at > backs[b] ? at - backs[b] : 0;
        ZydisDecodedInstruction ins;
        while (pc < n && ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(dec, NULL, buf + pc, n - pc, &ins))) {
            votes[pc]++;
            pc += ins.length;
        }
    }
    int k = 0;
    for (uint32_t i = 0; i < n && k < max; i++) {
        if (votes[i] >= 4) {
            bounds[k++] = i;
        }
    }
    return k;
}

/* the instruction starts from s up to (and with) e, at most max */
static int starts(const ZydisDecoder *dec, const uint8_t *buf, uint32_t n, uint32_t s, uint32_t e, uint32_t *out,
                  int max)
{
    int k = 0;
    uint32_t pc = s;
    ZydisDecodedInstruction ins;
    while (pc <= e && pc < n && k < max) {
        out[k++] = pc;
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(dec, NULL, buf + pc, n - pc, &ins))) {
            break;
        }
        pc += ins.length;
    }
    return k;
}

bool fit_ips_code(int disc_fd, const uint8_t *ips, size_t len, char *why, size_t wn)
{
    if (why && wn) {
        why[0] = 0;
    }
    static IpsRec recs[4096];
    const int nrec = ips_parse(ips, len, recs, 4096);
    if (nrec < 0) {
        if (why) {
            snprintf(why, wn, "not an IPS patch");
        }
        return false;
    }
    NcDisc d;
    NcDiscFile f;
    if (!open_xbe(disc_fd, &d, &f)) {
        if (why) {
            snprintf(why, wn, "no default.xbe");
        }
        return false;
    }
    uint8_t h0[0x180];
    uint8_t *hdr = NULL;
    uint32_t base = 0, hsize = 0;
    if (nc_disc_read(&d, &f, 0, h0, sizeof(h0)) && !memcmp(h0, "XBEH", 4)) {
        base = rd32(h0 + 0x104);
        hsize = rd32(h0 + 0x108);
        if (hsize >= sizeof(h0) && hsize < (8u << 20) && (hdr = malloc(hsize)) && !nc_disc_read(&d, &f, 0, hdr, hsize)) {
            free(hdr);
            hdr = NULL;
        }
    }
    if (!hdr) {
        nc_disc_close(&d);
        if (why) {
            snprintf(why, wn, "default.xbe unreadable");
        }
        return false;
    }
    const uint32_t nsec = rd32(hdr + 0x11C), secs = rd32(hdr + 0x120) - base;
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    static uint8_t orig[WIN_BACK + 0x10000 + WIN_AFTER + 32], neu[sizeof(orig)];
    static uint32_t bounds[sizeof(orig)], ob[2048], nb[2048];
    bool fits = true;
    for (int r = 0; r < nrec && fits; r++) {
        const IpsRec *rc = &recs[r];
        if (rc->off + rc->len > f.size) {
            if (why) {
                snprintf(why, wn, "record at 0x%06X: past the end of default.xbe", rc->off);
            }
            fits = false;
            break;
        }
        /* its section: code (executable, not .rdata/.data) is checked; data,
         * the headers and what no section loads are taken as they are */
        uint32_t raw = 0, rsize = 0;
        bool code = false;
        for (uint32_t k = 0; k < nsec && secs + (k + 1) * 0x38 <= hsize; k++) {
            const uint8_t *sh = hdr + secs + k * 0x38;
            const uint32_t sraw = rd32(sh + 0xC), ssize = rd32(sh + 0x10);
            if (rc->off >= sraw && rc->off < sraw + ssize) {
                const uint32_t na = rd32(sh + 0x14) - base;
                char name[9] = "";
                if (na < hsize) {
                    snprintf(name, sizeof(name), "%.8s", (const char *)hdr + na);
                }
                code = (rd32(sh) & 4) && strncmp(name, ".rdata", 6) && strncmp(name, ".data", 5);
                raw = sraw;
                rsize = ssize;
                break;
            }
        }
        if (!code) {
            continue;
        }
        /* a window of the section around it, as it is and as patched */
        const uint32_t w0 = rc->off - raw > WIN_BACK ? rc->off - WIN_BACK : raw;
        uint32_t w1 = rc->off + rc->len + WIN_AFTER;
        w1 = w1 > raw + rsize ? raw + rsize : w1;
        const uint32_t n = w1 - w0;
        if (!nc_disc_read(&d, &f, w0, orig, n)) {
            fits = false;
            if (why) {
                snprintf(why, wn, "default.xbe unreadable at 0x%06X", w0);
            }
            break;
        }
        memcpy(neu, orig, n);
        for (int q = 0; q < nrec; q++) {
            const IpsRec *o = &recs[q];
            for (uint32_t i = 0; i < o->len; i++) {
                if (o->off + i >= w0 && o->off + i < w1) {
                    neu[o->off + i - w0] = o->bytes ? o->bytes[i] : o->rle;
                }
            }
        }
        const uint32_t at = rc->off - w0, end = at + rc->len;
        const int nbnd = sweep_bounds(&dec, orig, n, at, bounds, (int)(sizeof(bounds) / sizeof(bounds[0])));
        uint32_t s = at, e = end;
        bool have_e = false;
        for (int i = 0; i < nbnd; i++) {
            if (bounds[i] <= at) {
                s = bounds[i];
            }
            if (bounds[i] >= end && !have_e) {
                e = bounds[i];
                have_e = true;
            }
        }
        const int no = starts(&dec, orig, n, s, e, ob, 2048), nn = starts(&dec, neu, n, s, e, nb, 2048);
        bool same = no == nn;
        for (int i = 0; same && i < no; i++) {
            same = ob[i] == nb[i];
        }
        bool in_step = false;   /* re-encoded, and ends where an instruction of the code began */
        for (int i = 0; !same && i < nn; i++) {
            in_step |= nb[i] == e;
        }
        if (!same && !in_step) {
            fits = false;
            if (why) {
                snprintf(why, wn, "record at 0x%06X (%u bytes) cuts into an instruction of this executable",
                         rc->off, rc->len);
            }
        }
    }
    free(hdr);
    nc_disc_close(&d);
    return fits;
}

/* ---- the database's patches ---- */

bool fit_file_hash(const char *path, uint64_t *hash, uint64_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint64_t h = 0xCBF29CE484222325ull, n = 0;
    unsigned char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < got; i++) {
            h = (h ^ buf[i]) * 0x100000001B3ull;
        }
        n += got;
    }
    fclose(f);
    *hash = h;
    *size = n;
    return true;
}

bool fit_db_patch(const char *exe_dir, const char *rel)
{
    char list[1200], path[1200];
    snprintf(list, sizeof(list), "%smods/patches.txt", exe_dir);
    snprintf(path, sizeof(path), "%smods/%s", exe_dir, rel);
    FILE *f = fopen(list, "r");
    if (!f) {
        return false;
    }
    bool found = false;
    uint64_t want_h = 0, want_n = 0;
    char line[512];
    const size_t rl = strlen(rel);
    while (!found && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || strlen(line) <= rl + 1 || line[rl] != ' ') {
            continue;
        }
        bool same = true;
        for (size_t i = 0; same && i < rl; i++) {
            char a = line[i], b = rel[i];
            a = a == '\\' ? '/' : (a >= 'A' && a <= 'Z' ? (char)(a + 32) : a);
            b = b == '\\' ? '/' : (b >= 'A' && b <= 'Z' ? (char)(b + 32) : b);
            same = a == b;
        }
        if (same) {
            char *sp = NULL;
            want_n = strtoull(line + rl + 1, &sp, 10);
            want_h = sp ? strtoull(sp, NULL, 16) : 0;
            found = true;
        }
    }
    fclose(f);
    uint64_t h, n;
    return found && fit_file_hash(path, &h, &n) && h == want_h && n == want_n;
}
