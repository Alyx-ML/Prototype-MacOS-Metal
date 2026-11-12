/* Mac enhancement settings (rt_enh.c).
 *
 * The settings are the rows that the runtime adds to the Video and the
 * Controller options screens of the game. Each setting has a game database
 * key (the name that the options screen gives to db_GetInt and db_SetInt)
 * and an integer value from 0 to its maximum.
 *
 * The values are stored in the file that rt_enh_init gets, one line for
 * each setting: "<key> <value>". Each call of rt_enh_set writes the file.
 * rt_enh_get can be called from any thread.
 */
#ifndef RT_ENH_H
#define RT_ENH_H

#include <stdbool.h>

typedef enum rt_enh_id {
    RT_ENH_HDR,            /* 0 off, 1 on (EDR output) */
    RT_ENH_SHADOW_RES,     /* 0 game size, 1 2x, 2 4x */
    RT_ENH_METALFX,        /* 0 off, 1 MetalFX spatial upscale to the display size */
    RT_ENH_SHARPEN,        /* 0 off, 1 low, 2 medium, 3 high */
    RT_ENH_SMAA,           /* 0 off, 1 SMAA 1x, 2 SMAA T2x (rt_d3d9_post.m) */
    RT_ENH_BUTTON_ICONS,   /* 0 game icons, 1 icons of the connected controller */
    RT_ENH_TRIGGERS,       /* 0 off, 1 DualSense adaptive triggers */
    RT_ENH_RUMBLE,         /* 0 game rumble, 1 enhanced rumble */
    RT_ENH_FOV,            /* gameplay camera field of view: 0 game, n = game + 5n degrees (n 1 to 6) */
    RT_ENH_AO,             /* 0 off, 1 low, 2 medium, 3 high (XeGTAO ambient occlusion) */
    RT_ENH_PCSS,           /* 0 off, 1 low, 2 high (PCSS soft shadows) */
    RT_ENH_TONEMAP,        /* 0 game, 1 GT7 tone mapping (Polyphony Digital) */
    RT_ENH_BLOOM,          /* 0 game blur, 1 dual Kawase blur of the game's bloom */
    RT_ENH_CONTACT,        /* 0 off, 1 on (Bend Studio screen-space contact shadows) */
    RT_ENH_COUNT
} rt_enh_id;

/* Read the settings file. A file that does not exist gives the default
   values. A line that cannot be read stops the process with the line. */
void rt_enh_init(const char *path);

int rt_enh_get(rt_enh_id id);
int rt_enh_default(rt_enh_id id);
int rt_enh_max(rt_enh_id id);
const char *rt_enh_key(rt_enh_id id);

/* Set and save a value. A value out of range stops the process. */
void rt_enh_set(rt_enh_id id, int value);

/* The setting of a game database key. Returns false for a key that is not
   a Mac enhancement key. */
bool rt_enh_find(const char *db_key, rt_enh_id *out);

/* The number of value changes since the start. The renderer compares it to
   see that a setting changed. */
unsigned rt_enh_version(void);

/* The type of the controller that has XInput user index 0 (rt_xinput.m). */
enum { RT_PAD_NONE, RT_PAD_PLAYSTATION, RT_PAD_OTHER };
int rt_xinput_pad_kind(void);

#endif
