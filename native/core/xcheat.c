/*
 * hakuX native core — Xbox cheat files (xcheat.h).
 *
 * The format, as XBMC4Xbox reads it (xbmc/utils/Trainer.cpp): at 0x0A the
 * offset of the options' states, at 0x0E of a table of text offsets (the
 * cheat's title, its credits, then an option each, 0 at the end), at 0x12
 * of its three title IDs, at 0x16 its entry point. An .xbtf comes
 * scrambled, then a creation key and a scroller text before that header;
 * its entry is at 0x1A, 0x16 then the offset of a table it wants filled
 * with its helpers' addresses (an SMBus write, a code patch, a hook, the
 * in-game keys, the LCD).
 *
 * The console's loader (a kernel patch in XBMC's InstallTrainer) calls the
 * entry with EDX and EBP at the cheat's base, before the title's entry
 * point. Here a start stub does it: the title's main thread runs the stub,
 * the stub calls the cheat, then jumps to the entry point.
 */
#include "native/core/xcheat.h"
#include "native/core/nc.h"
#include "native/core/vm.h"
#include "native/core/xcheat_toys.h"
#include "native/kernel/kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static bool ends_with(const char *s, const char *ext)
{
    const size_t n = strlen(s), e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        char c = s[n - e + i];
        c = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        if (c != ext[i]) {
            return false;
        }
    }
    return true;
}

/* an .xbtf's bytes in the clear (the loader's own: its first dword XORed
 * with three of its bytes' sum times 0xFFFFFF, the rest with that dword's
 * low byte and a running subtrahend) */
static void unscramble(uint8_t *b, uint32_t n)
{
    const uint8_t s = (uint8_t)(b[0x27] + b[0x2F] + b[0x37]);
    wr32(b, rd32(b) ^ (uint32_t)s * 0xFFFFFFu);
    const uint8_t key = b[0];
    uint32_t sub = 0;
    for (uint32_t i = 4, left = n - 4; left; i++, left--) {
        b[i] ^= key;
        b[i] = (uint8_t)(b[i] - (uint8_t)sub);
        sub += 3 + left;
    }
}

/* a NUL-terminated string at off inside the cheat, at most n bytes */
static bool text_at(const NcXCheat *x, uint32_t off, char *out, size_t n)
{
    if (off >= x->size) {
        return false;
    }
    size_t i = 0;
    for (; i + 1 < n && off + i < x->size && x->data[off + i]; i++) {
        out[i] = (char)x->data[off + i];
    }
    out[i] = 0;
    return true;
}

bool nc_xcheat_load(const char *path, NcXCheat *x)
{
    memset(x, 0, sizeof(*x));
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0x40 || len > 1 << 20) {
        fclose(f);
        return false;
    }
    x->raw = malloc((size_t)len + 1);
    if (!x->raw || fread(x->raw, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        nc_xcheat_free(x);
        return false;
    }
    fclose(f);
    x->raw[len] = 0;
    x->xbtf = ends_with(path, ".xbtf");
    x->data = x->raw;
    x->size = (uint32_t)len;
    if (x->xbtf) {
        unscramble(x->raw, (uint32_t)len);
        const size_t key = strnlen((const char *)x->raw + 4, (size_t)len - 4);
        if (key < 7 || x->raw[4 + 6] != '-' || 4 + key + 1 >= (size_t)len) {
            nc_xcheat_free(x);
            return false;
        }
        const uint8_t *t = x->raw + 4 + key + 1;
        const size_t text = strnlen((const char *)t, (size_t)len - (size_t)(t - x->raw));
        x->data = (uint8_t *)t + text + 1;
        if (x->data + 0x1E > x->raw + len) {
            nc_xcheat_free(x);
            return false;
        }
        x->size = (uint32_t)(x->raw + len - x->data);
    }
    const uint32_t texts = rd32(x->data + 0x0E), ids = rd32(x->data + 0x12);
    x->opts_off = rd32(x->data + 0x0A);
    if (texts > x->size || x->opts_off >= texts || ids + 12 > x->size) {
        nc_xcheat_free(x);
        return false;
    }
    for (int i = 0; i < 3; i++) {
        x->title_ids[i] = rd32(x->data + ids + 4 * i);
    }
    /* the texts: its title, its credits, then the options' labels */
    const uint32_t most = texts - x->opts_off + 2;
    for (uint32_t i = 0; i < most && texts + 4 * i + 4 <= x->size; i++) {
        const uint32_t off = rd32(x->data + texts + 4 * i);
        if (!off) {
            break;
        }
        char s[88];
        if (!text_at(x, off, s, sizeof(s))) {
            nc_xcheat_free(x);
            return false;
        }
        if (i == 0) {
            snprintf(x->name, sizeof(x->name), "%s", s);
        } else if (i >= 2 && x->nopts < NC_XCHEAT_MAX_OPTS) {
            snprintf(x->labels[x->nopts++], sizeof(x->labels[0]), "%s", s);
        }
    }
    return true;
}

void nc_xcheat_free(NcXCheat *x)
{
    free(x->raw);
    memset(x, 0, sizeof(*x));
}

bool nc_xcheat_for_title(const NcXCheat *x, uint32_t title_id)
{
    for (int i = 0; i < 3; i++) {
        if (title_id && x->title_ids[i] == title_id) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------ the run's */

#define MAX_RUN 8                   /* cheat files at once */

static char g_paths[MAX_RUN][1024], g_states_of[MAX_RUN][NC_XCHEAT_MAX_OPTS + 1];
static int g_nrun;
static char g_path[1024], g_states[NC_XCHEAT_MAX_OPTS + 1];   /* the one installing */

void nc_xcheat_set(const char *path, const char *states)
{
    if (!path || !path[0] || g_nrun >= MAX_RUN) {
        return;
    }
    for (int i = 0; i < g_nrun; i++) {
        if (!strcmp(g_paths[i], path)) {
            return;                         /* named twice: the first */
        }
    }
    snprintf(g_paths[g_nrun], sizeof(g_paths[0]), "%s", path);
    snprintf(g_states_of[g_nrun], sizeof(g_states_of[0]), "%s", states ? states : "");
    g_nrun++;
}

#define KERNEL_STORE 0x8000000Cu   /* where the console's loader keeps its block's address */

static uint32_t install_one(struct NcKernel *k, struct NcVm *vm, uint32_t entry, uint32_t title_id);

uint32_t nc_xcheat_install(struct NcKernel *k, struct NcVm *vm, uint32_t entry, uint32_t title_id)
{
    /* the last first: each one's start runs it, then the next one's start
     * (the first file's cheat runs first) */
    for (int i = g_nrun - 1; i >= 0; i--) {
        snprintf(g_path, sizeof(g_path), "%s", g_paths[i]);
        snprintf(g_states, sizeof(g_states), "%s", g_states_of[i]);
        entry = install_one(k, vm, entry, title_id);
    }
    return entry;
}

static uint32_t install_one(struct NcKernel *k, struct NcVm *vm, uint32_t entry, uint32_t title_id)
{
    NcXCheat x;
    if (!nc_xcheat_load(g_path, &x)) {
        nc_log("hakuX-native", "cheats: %s can't be read", g_path);
        return entry;
    }
    if (!nc_xcheat_for_title(&x, title_id)) {
        nc_log("hakuX-native", "cheats: %s is for titles %08x %08x %08x, not %08x", g_path, x.title_ids[0],
               x.title_ids[1], x.title_ids[2], title_id);
        nc_xcheat_free(&x);
        return entry;
    }
    /* the options as chosen (the file's own defaults past them) */
    int on = 0;
    for (int i = 0; i < x.nopts && x.opts_off + i < x.size; i++) {
        if (g_states[i]) {
            x.data[x.opts_off + i] = g_states[i] == '1';
        }
        on += x.data[x.opts_off + i] != 0;
    }
    /* the block: the loader's head (its base at +2, as the helpers read it
     * through KERNEL_STORE), the cheat, its helpers, the start stub */
    static const uint8_t lcd_clear[] = {0xC3}, lcd_write[] = {0xC2, 0x08, 0x00};
    const uint32_t helpers = sizeof(k_sm_bus) + sizeof(k_patch_it_toy) + sizeof(k_hookit_toy) +
                             sizeof(k_igk_main_toy) + sizeof(k_hook_igk_toy) + sizeof(lcd_clear) +
                             sizeof(lcd_write);
    const uint32_t total = 8 + x.size + helpers + 32;
    const uint32_t va = nc_mm_contig_alloc(k, (total + NC_PAGE - 1) & ~(NC_PAGE - 1));
    uint8_t *h = va ? nc_vm_host(vm, va) : NULL;
    if (!h) {
        nc_log("hakuX-native", "cheats: no memory for %s", g_path);
        nc_xcheat_free(&x);
        return entry;
    }
    memset(h, 0xFF, total);
    const uint32_t base = va + 8;
    h[0] = 0x60;                            /* pushad; mov edx, base */
    h[1] = 0xBA;
    wr32(h + 2, base);
    memcpy(h + 8, x.data, x.size);
    uint8_t *t = h + 8;                     /* the cheat, as it runs */
    uint32_t at = base + x.size;            /* next free byte's address */
#define PUT(bytes, n) (memcpy(h + (at - va), (bytes), (n)), at += (n), at - (n))
    const uint32_t entry_off = rd32(t + 0x1A) ? rd32(t + 0x1A) : rd32(t + 0x16);
    if (x.xbtf && rd32(t + 0x1A)) {
        /* its helpers, where its table has none of its own */
        const uint32_t table = rd32(t + 0x16);
        if (table + 24 <= x.size) {
            uint8_t *slot = t + table;
            const uint32_t smbus = PUT(k_sm_bus, sizeof(k_sm_bus));
            const uint32_t patch = PUT(k_patch_it_toy, sizeof(k_patch_it_toy));
            const uint32_t hook = PUT(k_hookit_toy, sizeof(k_hookit_toy));
            const uint32_t igk = PUT(k_igk_main_toy, sizeof(k_igk_main_toy));
            const uint32_t hook_igk = PUT(k_hook_igk_toy, sizeof(k_hook_igk_toy));
            wr32(h + (hook_igk - va) + 5, igk); /* its mov edx, the in-game keys' address */
            const uint32_t clear = PUT(lcd_clear, sizeof(lcd_clear));
            const uint32_t write = PUT(lcd_write, sizeof(lcd_write));
            const uint32_t fill[6] = {smbus, patch, hook, hook_igk, clear, write};
            for (int i = 0; i < 6; i++) {
                if (!rd32(slot + 4 * i)) {
                    wr32(slot + 4 * i, fill[i]);
                }
            }
        }
    }
    /* the start: pushad; mov edx, base; mov ebp, edx; mov ecx, cheat entry;
     * call ecx; popad; mov eax, entry; jmp eax */
    uint8_t stub[24];
    unsigned n = 0;
    stub[n++] = 0x60;
    stub[n++] = 0xBA;
    wr32(stub + n, base);
    n += 4;
    stub[n++] = 0x8B;
    stub[n++] = 0xEA;
    stub[n++] = 0xB9;
    wr32(stub + n, base + entry_off);
    n += 4;
    stub[n++] = 0xFF;
    stub[n++] = 0xD1;
    stub[n++] = 0x61;
    stub[n++] = 0xB8;
    wr32(stub + n, entry);
    n += 4;
    stub[n++] = 0xFF;
    stub[n++] = 0xE0;
    const uint32_t start = PUT(stub, n);
#undef PUT
    /* the helpers find the block through the kernel's store */
    if (nc_vm_phys(vm, KERNEL_STORE) != UINT32_MAX) {
        uint8_t *store = nc_vm_host(vm, KERNEL_STORE);
        if (store) {
            wr32(store, va);
        }
    }
    nc_log("hakuX-native", "cheats: %s (%s), %d of %d options on, at %08x", g_path, x.name, on, x.nopts, base);
    nc_xcheat_free(&x);
    return start;
}
