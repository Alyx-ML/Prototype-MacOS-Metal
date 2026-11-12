/* Mac enhancement settings. See rt_enh.h. */
#include "rt.h"
#include "rt_enh.h"
#include "rt_file.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct enh_def {
    const char *key;
    int def;
    int max;
} enh_def;

static const enh_def defs[RT_ENH_COUNT] = {
    [RT_ENH_ANISOTROPY] = {"SettingMacAnisotropic", 1, 1},
    [RT_ENH_HDR] = {"SettingMacHDR", 0, 1},
    [RT_ENH_SHADOW_RES] = {"SettingMacShadowResolution", 0, 2},
    [RT_ENH_METALFX] = {"SettingMacMetalFX", 0, 1},
    [RT_ENH_SHARPEN] = {"SettingMacSharpening", 0, 3},
    [RT_ENH_SMAA] = {"SettingMacSMAA", 0, 1},
    [RT_ENH_BUTTON_ICONS] = {"SettingMacButtonIcons", 1, 1},
    [RT_ENH_TRIGGERS] = {"SettingMacAdaptiveTriggers", 1, 1},
    [RT_ENH_RUMBLE] = {"SettingMacRumble", 1, 1},
};

static _Atomic int values[RT_ENH_COUNT];
static _Atomic unsigned version;
static char file_path[RT_HOST_PATH];

static void check_id(rt_enh_id id)
{
    if ((unsigned)id >= RT_ENH_COUNT) rt_fatal("enhancement setting %d does not exist", (int)id);
}

int rt_enh_get(rt_enh_id id)
{
    check_id(id);
    return atomic_load_explicit(&values[id], memory_order_relaxed);
}

int rt_enh_default(rt_enh_id id)
{
    check_id(id);
    return defs[id].def;
}

int rt_enh_max(rt_enh_id id)
{
    check_id(id);
    return defs[id].max;
}

const char *rt_enh_key(rt_enh_id id)
{
    check_id(id);
    return defs[id].key;
}

unsigned rt_enh_version(void)
{
    return atomic_load_explicit(&version, memory_order_acquire);
}

bool rt_enh_find(const char *db_key, rt_enh_id *out)
{
    unsigned i;
    if (db_key == NULL) return false;
    for (i = 0; i < RT_ENH_COUNT; i++)
        if (strcmp(db_key, defs[i].key) == 0) {
            *out = (rt_enh_id)i;
            return true;
        }
    return false;
}

static void save(void)
{
    char tmp[RT_HOST_PATH + 8];
    FILE *f;
    unsigned i;
    int r = snprintf(tmp, sizeof tmp, "%s.tmp", file_path);
    if (r <= 0 || (size_t)r >= sizeof tmp) rt_fatal("enhancement settings: the path %s is too long", file_path);
    f = fopen(tmp, "w");
    if (f == NULL) rt_fatal("enhancement settings: %s cannot be written: %s", tmp, strerror(errno));
    for (i = 0; i < RT_ENH_COUNT; i++)
        if (fprintf(f, "%s %d\n", defs[i].key, rt_enh_get((rt_enh_id)i)) < 0)
            rt_fatal("enhancement settings: %s cannot be written: %s", tmp, strerror(errno));
    if (fclose(f) != 0) rt_fatal("enhancement settings: %s cannot be written: %s", tmp, strerror(errno));
    if (rename(tmp, file_path) != 0)
        rt_fatal("enhancement settings: %s cannot be renamed to %s: %s", tmp, file_path, strerror(errno));
}

void rt_enh_set(rt_enh_id id, int value)
{
    check_id(id);
    if (value < 0 || value > defs[id].max)
        rt_fatal("enhancement setting %s: the value %d is not in the range 0 to %d", defs[id].key, value, defs[id].max);
    if (file_path[0] == 0) rt_fatal("enhancement setting %s: rt_enh_init was not called", defs[id].key);
    if (atomic_exchange_explicit(&values[id], value, memory_order_relaxed) == value) return;
    atomic_fetch_add_explicit(&version, 1u, memory_order_release);
    save();
    rt_log("enhancement setting %s = %d", defs[id].key, value);
}

void rt_enh_init(const char *path)
{
    char line[256];
    unsigned i, num = 0;
    FILE *f;
    int r = snprintf(file_path, sizeof file_path, "%s", path);
    if (r <= 0 || (size_t)r >= sizeof file_path) rt_fatal("enhancement settings: the path %s is too long", path);
    for (i = 0; i < RT_ENH_COUNT; i++) atomic_store_explicit(&values[i], defs[i].def, memory_order_relaxed);
    f = fopen(path, "r");
    if (f == NULL) {
        if (errno != ENOENT) rt_fatal("enhancement settings: %s cannot be read: %s", path, strerror(errno));
        return;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char key[128], extra[2];
        int value, n;
        rt_enh_id id;
        num++;
        if (strchr(line, '\n') == NULL && !feof(f)) rt_fatal("enhancement settings: %s line %u is too long", path, num);
        n = sscanf(line, "%127s %d %1s", key, &value, extra);
        if (n == EOF) continue;
        if (n != 2) rt_fatal("enhancement settings: %s line %u is not \"<key> <value>\"", path, num);
        if (!rt_enh_find(key, &id)) rt_fatal("enhancement settings: %s line %u: the key %s is unknown", path, num, key);
        if (value < 0 || value > defs[id].max)
            rt_fatal("enhancement settings: %s line %u: %s %d is not in the range 0 to %d", path, num, key, value,
                     defs[id].max);
        atomic_store_explicit(&values[id], value, memory_order_relaxed);
    }
    if (ferror(f)) rt_fatal("enhancement settings: %s cannot be read: %s", path, strerror(errno));
    fclose(f);
}

/* ------------------------------------------------------------ engine hooks */

/* Engine hooks (instruction_replacements.sci). Each hook gets the guest
   registers at the entry of a game function.
   - rt_enh_db_get_int: db_GetInt(key, a, b), stdcall.
   - rt_enh_db_set_int: db_SetInt(key, index, value, flag), stdcall. The
     menu script calls it as DoCallBack('db_SetInt', key, mProfileIndex,
     value, index). The function gives (index, value, flag) to the
     database setter at 0x107F71F0, so the value is argument 2.
   - rt_enh_db_restore: fe_RestoreGameDBDefault(key, a), stdcall.
   For a Mac enhancement key the hook does the call: eax is the result,
   ecx is the return address, esp is after the arguments and edx is 1.
   For a different key the hook sets edx to 0 and changes nothing more,
   and the game function continues.
   - rt_enh_glyph_set: eax is the SettingsDefaultController value that
     selects the button icon set. With "Button Icons" on and a controller
     connected, the hook replaces a controller value (2 to 10) with 2
     (PlayStation icons) or 4 (Xbox icons). */
void rt_enh_db_get_int(_cpu *cpu);
void rt_enh_db_set_int(_cpu *cpu);
void rt_enh_db_restore(_cpu *cpu);
void rt_enh_glyph_set(_cpu *cpu);

/* The setting of the key in argument 0, or false for a game key. */
static bool hook_key(_cpu *cpu, rt_enh_id *id)
{
    char key[64];
    uint32_t p = ARG(0);
    if (p == 0) return false;
    if (rt_gstr(key, sizeof key, p) != 0) return false; /* longer than every Mac key */
    return rt_enh_find(key, id);
}

/* Return from a stdcall function with nargs arguments. */
static void hook_return(_cpu *cpu, unsigned nargs, uint32_t v)
{
    ecx = rd32(esp);
    esp += 4u + 4u * nargs;
    eax = v;
    edx = 1;
}

void rt_enh_db_get_int(_cpu *cpu)
{
    rt_enh_id id;
    if (!hook_key(cpu, &id)) { edx = 0; return; }
    hook_return(cpu, 3, (uint32_t)rt_enh_get(id));
}

void rt_enh_db_set_int(_cpu *cpu)
{
    rt_enh_id id;
    int32_t v;
    if (!hook_key(cpu, &id)) { edx = 0; return; }
    v = (int32_t)ARG(2);
    rt_enh_set(id, v);
    hook_return(cpu, 4, 1);
}

void rt_enh_db_restore(_cpu *cpu)
{
    rt_enh_id id;
    if (!hook_key(cpu, &id)) { edx = 0; return; }
    rt_enh_set(id, rt_enh_default(id));
    hook_return(cpu, 2, 1);
}

void rt_enh_glyph_set(_cpu *cpu)
{
    int kind;
    if (rt_enh_get(RT_ENH_BUTTON_ICONS) == 0 || eax < 2 || eax > 10) return;
    kind = rt_xinput_pad_kind();
    if (kind == RT_PAD_PLAYSTATION) eax = 2;
    else if (kind == RT_PAD_OTHER) eax = 4;
}
