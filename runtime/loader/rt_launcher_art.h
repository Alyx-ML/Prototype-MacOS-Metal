/*
 * The title art of the launcher window (rt_launcher_art.c).
 *
 * The art comes from the game files of the player at run time: the
 * repository and the release do not hold game art. The launcher reads
 * art.rcf in the selected game folder, inflates \art\hud\fe_frontend.p3d.rz
 * and decodes two textures of the title screen of the game:
 *   - the logo (protomainmenu__I1F.dds, 512 x 128, DXT5),
 *   - a background glow (protostart__IA.dds, 512 x 512, DXT1).
 */
#ifndef RT_LAUNCHER_ART_H
#define RT_LAUNCHER_ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An image with 8-bit RGBA pixels (straight alpha), top row first. */
typedef struct rt_art_image {
    uint32_t width, height;
    uint8_t *rgba; /* malloc; width * height * 4 bytes */
} rt_art_image;

/* Read the logo and the background from game_dir/art.rcf. On success both
   images are set and the result is true. On failure err tells the problem,
   both images are empty and the result is false. */
bool rt_launcher_load_art(const char *game_dir, rt_art_image *logo, rt_art_image *background, char *err, size_t errn);

/* Release the pixels of img and set it empty. */
void rt_art_free(rt_art_image *img);

#endif
