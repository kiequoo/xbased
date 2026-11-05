/*
 * hakuX native core — cheat files of the Xbox scene (xcheat.c): the .etm and
 * .xbtf files EvolutionX, XBMC4Xbox and the Xored launcher ran (known there
 * as trainers). Each is x86 code for one game's title IDs with a list of
 * options, run once before the game's entry point; with its options on, it
 * patches the game's code.
 */
#ifndef NATIVE_CORE_XCHEAT_H
#define NATIVE_CORE_XCHEAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_XCHEAT_MAX_OPTS 100

typedef struct NcXCheat {
    uint8_t *raw;               /* the file's bytes (an .xbtf's unscrambled) */
    uint8_t *data;              /* the cheat itself (an .xbtf's past its key and text) */
    uint32_t size;
    bool xbtf;
    uint32_t opts_off;          /* its options' states, a byte each (0 off, 1 on) */
    int nopts;
    char name[88];              /* its own title (its first text) */
    char labels[NC_XCHEAT_MAX_OPTS][88];
    uint32_t title_ids[3];
} NcXCheat;

/* A cheat file read: false if it isn't one (or is damaged). */
bool nc_xcheat_load(const char *path, NcXCheat *x);
void nc_xcheat_free(NcXCheat *x);
/* made for that title */
bool nc_xcheat_for_title(const NcXCheat *x, uint32_t title_id);

/* The run's cheat file and its options' states ("1" on, "0" off, in order),
 * before boot (the option "xcheat=<file>|<states>"). */
void nc_xcheat_set(const char *path, const char *states);

struct NcKernel;
struct NcVm;
/* After the XBE loads: the cheat set for this title put in memory with a
 * start that runs it, then jumps to entry. Returns where the title's main
 * thread starts: that start, or entry itself (no cheat, another title's). */
uint32_t nc_xcheat_install(struct NcKernel *k, struct NcVm *vm, uint32_t entry, uint32_t title_id);

#ifdef __cplusplus
}
#endif

#endif
