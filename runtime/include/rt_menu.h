/* Mac enhancement rows in the options screens of the game (rt_menu.c).
 *
 * The options screens are GFx movies in the player's art.rcf. When the game
 * opens art.rcf, the runtime reads five movies from the file, checks their
 * SHA-256 values and makes patched copies:
 *   protooptionsoverlay.gfx   toggle clips for the new rows
 *   protovideooption.gfx      the Video rows of the main menu (rt_enh.h)
 *   protocontrolleroption.gfx the Controller rows of the main menu
 *   protopausevideo.gfx       the Video rows of the pause menu
 *   protopausecontroller.gfx  the Controller rows of the pause menu
 * The file handle then shows a virtual art.rcf: the original bytes, the
 * patched movies after the original end, and directory entries that point
 * to the patched movies. The file on disk does not change.
 */
#ifndef RT_MENU_H
#define RT_MENU_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct rt_file_overlay rt_file_overlay;

/* The virtual file for a read-only open of host (descriptor fd). Returns
   NULL when host is not art.rcf. A movie that is not the supported version
   stops the process. */
const rt_file_overlay *rt_menu_overlay(const char *host, int fd);

/* The size of the virtual file. */
uint64_t rt_overlay_size(const rt_file_overlay *ov);

/* Read at most n bytes at pos of the virtual file. Returns the number of
   bytes (0 at the end of the file), or -1 with errno set. */
ssize_t rt_overlay_pread(const rt_file_overlay *ov, int fd, void *dst, size_t n, uint64_t pos);

#endif
