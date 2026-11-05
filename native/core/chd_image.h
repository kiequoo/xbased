/*
 * hakuX native core — CHD disc images (chd_image.c): MAME's compressed hunks
 * (chdman createdvd from an XISO or a full dump) read as the plain image
 * they hold, through libchdr.
 */
#ifndef NATIVE_CORE_CHD_IMAGE_H
#define NATIVE_CORE_CHD_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An open file descriptor's image as a CHD, if it is one: registered by the
 * descriptor, so the disc's reads (nc_chd_read on it) go through it. False:
 * not a CHD (or one that can't be read). */
bool nc_chd_attach(int fd);
void nc_chd_detach(int fd);
/* fd registered as a CHD */
bool nc_chd_is(int fd);
/* len bytes of the image it holds at off; zeros past its end. False: a
 * hunk that can't be read. Any thread. */
bool nc_chd_read(int fd, void *buf, size_t len, uint64_t off);

#ifdef __cplusplus
}
#endif

#endif
