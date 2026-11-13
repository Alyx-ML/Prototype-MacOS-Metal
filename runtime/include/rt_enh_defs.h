/* The Mac enhancement settings: the key in the settings file, the default
 * value and the maximum value of each rt_enh_id. rt_enh.c (the game) and
 * the launcher window of the loader (runtime/loader/rt_launcher.m) use this
 * table, so the two read and write the same file format.
 */
#ifndef RT_ENH_DEFS_H
#define RT_ENH_DEFS_H

#include "rt_enh.h"

typedef struct rt_enh_def {
    const char *key;
    int def;
    int max;
} rt_enh_def;

static const rt_enh_def rt_enh_defs[RT_ENH_COUNT] = {
    [RT_ENH_HDR] = {"SettingMacHDR", 0, 1},
    [RT_ENH_SHADOW_RES] = {"SettingMacShadowResolution", 0, 2},
    [RT_ENH_METALFX] = {"SettingMacMetalFX", 0, 1},
    [RT_ENH_SHARPEN] = {"SettingMacSharpening", 0, 3},
    [RT_ENH_SMAA] = {"SettingMacSMAA", 0, 2},
    [RT_ENH_BUTTON_ICONS] = {"SettingMacButtonIcons", 1, 1},
    [RT_ENH_TRIGGERS] = {"SettingMacAdaptiveTriggers", 1, 1},
    [RT_ENH_RUMBLE] = {"SettingMacRumble", 1, 1},
    [RT_ENH_FOV] = {"SettingMacFieldOfView", 0, 6},
    [RT_ENH_AO] = {"SettingMacAmbientOcclusion", 0, 3},
    [RT_ENH_PCSS] = {"SettingMacSoftShadows", 0, 2},
    [RT_ENH_TONEMAP] = {"SettingMacToneMapping", 0, 1},
    [RT_ENH_BLOOM] = {"SettingMacKawaseBloom", 0, 1},
    [RT_ENH_CONTACT] = {"SettingMacContactShadows", 0, 1},
};

/* A removed setting: anisotropic filtering is now always 16x. A file can
   still have this line; the readers ignore it, and the next save writes
   the file without it. */
#define RT_ENH_REMOVED_KEY "SettingMacAnisotropic"

#endif
