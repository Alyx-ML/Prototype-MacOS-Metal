/* Mac enhancement rows in the options screens of the game. See rt_menu.h.
 *
 * The rows use the options framework of the movies: each row is an
 * optionscontainer object with a toggle clip of protooptionsoverlay.gfx, a
 * game database key and a list of values. The database keys are the keys of
 * rt_enh.h; the engine hooks in rt_enh.c read and write them.
 *
 * There are four screens: Video and Controller in the main menu, and the
 * same two screens in the pause menu. The options list of a screen sets the
 * position of each row (32 or 33 pixels for each row). The longer lists
 * need more space:
 *   - The Video screens have a gamma preview box above the list. The game
 *     shows the gamma row only on Xbox 360 and PS3, but it loads the
 *     preview image (protogammaimage.gfx) into mcGammaImageAnchor on all
 *     platforms. The patch removes the box and the anchor clip, and moves
 *     the list up to the top of the list of the Controller screen. Without
 *     the anchor clip, the Load and Unload calls of the script do nothing.
 *   - The main menu Controller screen centers its list. The screen moves
 *     down by 48 pixels, so that the top of the longer list stays where the
 *     original list starts.
 */
#include "rt.h"
#include "rt_enh.h"
#include "rt_menu.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------- buffers */

typedef struct obuf {
    uint8_t *p;
    size_t n, cap;
} obuf;

static void ob_put(obuf *b, const void *d, size_t n)
{
    if (n > SIZE_MAX - b->n) rt_fatal("options menu patch: buffer size overflow");
    if (b->n + n > b->cap)
    {
        size_t cap = b->cap != 0 ? b->cap : 4096;
        uint8_t *p;
        while (cap < b->n + n)
        {
            if (cap > SIZE_MAX / 2) rt_fatal("options menu patch: buffer size overflow");
            cap *= 2;
        }
        p = realloc(b->p, cap);
        if (p == NULL) rt_fatal("options menu patch: out of host memory");
        b->p = p;
        b->cap = cap;
    }
    if (n != 0) memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void ob_u8(obuf *b, uint8_t v) { ob_put(b, &v, 1); }

static void ob_u16(obuf *b, uint16_t v)
{
    uint8_t t[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    ob_put(b, t, 2);
}

static void ob_u32(obuf *b, uint32_t v)
{
    uint8_t t[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    ob_put(b, t, 4);
}

static void ob_set_u16(obuf *b, size_t at, uint16_t v)
{
    if (at > b->n || b->n - at < 2) rt_fatal("options menu patch: write outside the buffer");
    b->p[at] = (uint8_t)v;
    b->p[at + 1] = (uint8_t)(v >> 8);
}

static void ob_set_u32(obuf *b, size_t at, uint32_t v)
{
    if (at > b->n || b->n - at < 4) rt_fatal("options menu patch: write outside the buffer");
    b->p[at] = (uint8_t)v;
    b->p[at + 1] = (uint8_t)(v >> 8);
    b->p[at + 2] = (uint8_t)(v >> 16);
    b->p[at + 3] = (uint8_t)(v >> 24);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------ SWF tags */

typedef struct tagh {
    uint16_t code;
    size_t hdr, len;
} tagh;

static tagh tag_at(const char *name, const uint8_t *d, size_t pos, size_t end)
{
    tagh t;
    uint16_t h;
    if (pos > end || end - pos < 2) rt_fatal("options menu patch: %s: tag header at %zu is cut off", name, pos);
    h = le16(d + pos);
    t.code = (uint16_t)(h >> 6);
    t.len = h & 63u;
    t.hdr = 2;
    if (t.len == 63)
    {
        if (end - pos < 6) rt_fatal("options menu patch: %s: tag header at %zu is cut off", name, pos);
        t.len = le32(d + pos + 2);
        t.hdr = 6;
    }
    if (t.len > end - pos - t.hdr) rt_fatal("options menu patch: %s: tag at %zu is longer than its container", name, pos);
    return t;
}

static void put_tag(obuf *o, uint16_t code, const uint8_t *body, size_t len)
{
    if (len > UINT32_MAX) rt_fatal("options menu patch: tag too long");
    if (len < 63)
        ob_u16(o, (uint16_t)((code << 6) | len));
    else
    {
        ob_u16(o, (uint16_t)((code << 6) | 63));
        ob_u32(o, (uint32_t)len);
    }
    ob_put(o, body, len);
}

/* The offset of the first tag after the movie header. */
static size_t first_tag(const char *name, const uint8_t *d, size_t n)
{
    unsigned nb;
    size_t rl;
    if (n < 9 || memcmp(d, "GFX", 3) != 0) rt_fatal("options menu patch: %s is not an uncompressed GFx movie", name);
    if (le32(d + 4) != n) rt_fatal("options menu patch: %s: the header length is not the movie size", name);
    nb = d[8] >> 3;
    rl = (5u + 4u * nb + 7u) / 8u;
    if (8 + rl + 4 > n) rt_fatal("options menu patch: %s: the header is cut off", name);
    return 8 + rl + 4;
}

/* Copy the movie header of d to o; finish_movie sets the length. */
static void start_movie(obuf *o, const uint8_t *d, size_t first)
{
    ob_put(o, d, first);
}

static void finish_movie(obuf *o)
{
    if (o->n > UINT32_MAX) rt_fatal("options menu patch: movie too long");
    ob_set_u32(o, 4, (uint32_t)o->n);
}

/* ----------------------------------------------------- matrix records */

typedef struct bitr {
    const uint8_t *d;
    size_t n, bit;
    const char *name;
} bitr;

static uint32_t br_u(bitr *r, unsigned k)
{
    uint32_t v = 0;
    while (k-- > 0)
    {
        if (r->bit >= r->n * 8) rt_fatal("options menu patch: %s: a matrix is cut off", r->name);
        v = (v << 1) | ((r->d[r->bit >> 3] >> (7 - (r->bit & 7))) & 1u);
        r->bit++;
    }
    return v;
}

static int32_t br_s(bitr *r, unsigned k)
{
    int64_t x;
    if (k == 0) return 0;
    x = br_u(r, k);
    if (x & (1ll << (k - 1))) x -= 1ll << k;
    return (int32_t)x;
}

typedef struct swf_matrix {
    bool has_scale, has_rot;
    int32_t sx, sy, r0, r1, tx, ty;
} swf_matrix;

/* Read a matrix at d (n bytes). Returns the size in bytes. */
static size_t read_matrix(const char *name, const uint8_t *d, size_t n, swf_matrix *m)
{
    bitr r = {d, n, 0, name};
    unsigned k;
    memset(m, 0, sizeof *m);
    m->has_scale = br_u(&r, 1) != 0;
    if (m->has_scale)
    {
        k = br_u(&r, 5);
        m->sx = br_s(&r, k);
        m->sy = br_s(&r, k);
    }
    m->has_rot = br_u(&r, 1) != 0;
    if (m->has_rot)
    {
        k = br_u(&r, 5);
        m->r0 = br_s(&r, k);
        m->r1 = br_s(&r, k);
    }
    k = br_u(&r, 5);
    m->tx = br_s(&r, k);
    m->ty = br_s(&r, k);
    return (r.bit + 7) / 8;
}

typedef struct bitw {
    obuf *o;
    uint8_t cur;
    unsigned nbit;
} bitw;

static void bw_put(bitw *w, uint32_t v, unsigned k)
{
    while (k-- > 0)
    {
        w->cur = (uint8_t)((w->cur << 1) | ((v >> k) & 1u));
        if (++w->nbit == 8)
        {
            ob_u8(w->o, w->cur);
            w->cur = 0;
            w->nbit = 0;
        }
    }
}

static void bw_flush(bitw *w)
{
    if (w->nbit != 0) ob_u8(w->o, (uint8_t)(w->cur << (8 - w->nbit)));
    w->cur = 0;
    w->nbit = 0;
}

static unsigned sbits(int32_t a, int32_t b)
{
    unsigned k = 1;
    while (k < 31 && ((int64_t)a < -(1ll << (k - 1)) || (int64_t)a > (1ll << (k - 1)) - 1 ||
                      (int64_t)b < -(1ll << (k - 1)) || (int64_t)b > (1ll << (k - 1)) - 1))
        k++;
    return k;
}

static void put_matrix(obuf *o, const swf_matrix *m)
{
    bitw w = {o, 0, 0};
    unsigned k;
    bw_put(&w, m->has_scale, 1);
    if (m->has_scale)
    {
        k = sbits(m->sx, m->sy);
        bw_put(&w, k, 5);
        bw_put(&w, (uint32_t)m->sx, k);
        bw_put(&w, (uint32_t)m->sy, k);
    }
    bw_put(&w, m->has_rot, 1);
    if (m->has_rot)
    {
        k = sbits(m->r0, m->r1);
        bw_put(&w, k, 5);
        bw_put(&w, (uint32_t)m->r0, k);
        bw_put(&w, (uint32_t)m->r1, k);
    }
    k = sbits(m->tx, m->ty);
    bw_put(&w, k, 5);
    bw_put(&w, (uint32_t)m->tx, k);
    bw_put(&w, (uint32_t)m->ty, k);
    bw_flush(&w);
}

/* ------------------------------------------------------------- the rows */

typedef struct menu_row {
    const char *toggle; /* clip name in the overlay sprite */
    const char *label;
    rt_enh_id id;
    const char *const *values; /* the label of each value 0..max */
    unsigned count;
} menu_row;

static const char *const v_off_on[] = {"$OFF", "$ON"};
static const char *const v_aniso[] = {"GAME", "16x"};
static const char *const v_shadow[] = {"GAME", "2x", "4x"};
static const char *const v_sharpen[] = {"$OFF", "$LOW", "$MED", "$HIGH"};
static const char *const v_icons[] = {"GAME", "CONTROLLER"};

#define ROW(t, l, id, v) {t, l, id, v, sizeof v / sizeof v[0]}

static const menu_row video_rows[] = {
    ROW("tglMacAnisotropic", "ANISOTROPIC FILTERING", RT_ENH_ANISOTROPY, v_aniso),
    ROW("tglMacHDR", "HDR", RT_ENH_HDR, v_off_on),
    ROW("tglMacShadowRes", "SHADOW RESOLUTION (RESTART)", RT_ENH_SHADOW_RES, v_shadow),
    ROW("tglMacMetalFX", "METALFX UPSCALING", RT_ENH_METALFX, v_off_on),
    ROW("tglMacSharpen", "SHARPENING (CAS)", RT_ENH_SHARPEN, v_sharpen),
    ROW("tglMacSMAA", "SMAA ANTI-ALIASING", RT_ENH_SMAA, v_off_on),
};

static const menu_row controller_rows[] = {
    ROW("tglMacButtonIcons", "BUTTON ICONS", RT_ENH_BUTTON_ICONS, v_icons),
    ROW("tglMacTriggers", "ADAPTIVE TRIGGERS (DUALSENSE)", RT_ENH_TRIGGERS, v_off_on),
    ROW("tglMacRumble", "ENHANCED RUMBLE", RT_ENH_RUMBLE, v_off_on),
};

#define NROWS(a) (sizeof a / sizeof a[0])

/* --------------------------------------------- overlay toggle clips */

#define TOGGLE_CHAR 1       /* the toggle symbol of protooptionsoverlay.gfx */
#define TOGGLE_X_TWIPS 40   /* x 2.0, as the original toggles */
#define FIRST_NEW_DEPTH 200 /* above every depth of the overlay sprites */

static void put_toggle(obuf *o, uint16_t depth, int32_t ty, const char *name)
{
    obuf b = {0};
    swf_matrix m = {0};
    m.tx = TOGGLE_X_TWIPS;
    m.ty = ty;
    ob_u8(&b, 0x26); /* HasName | HasMatrix | HasCharacter */
    ob_u16(&b, depth);
    ob_u16(&b, TOGGLE_CHAR);
    put_matrix(&b, &m);
    ob_put(&b, name, strlen(name) + 1);
    put_tag(o, 26, b.p, b.n);
    free(b.p);
}

/* Copy DefineSprite body (len bytes) to o with a toggle clip for each row
   in frame 1. The clips start below the last original row (y0 twips). */
static void patch_sprite(const char *name, const uint8_t *body, size_t len, const menu_row *rows, size_t nrows,
                         int32_t y0, obuf *o)
{
    size_t pos = 4, i;
    bool placed = false;
    if (len < 4) rt_fatal("options menu patch: %s: a sprite is cut off", name);
    /* The new depths must be free. */
    while (pos < len)
    {
        tagh t = tag_at(name, body, pos, len);
        const uint8_t *b = body + pos + t.hdr;
        if ((t.code == 26 && t.len >= 3) || (t.code == 70 && t.len >= 4))
        {
            uint16_t depth = le16(b + (t.code == 70 ? 2 : 1));
            if (depth >= FIRST_NEW_DEPTH && depth < FIRST_NEW_DEPTH + 2 * nrows)
                rt_fatal("options menu patch: %s: sprite %u uses depth %u", name, le16(body), depth);
        }
        pos += t.hdr + t.len;
        if (t.code == 0) break;
    }
    ob_put(o, body, 4);
    pos = 4;
    while (pos < len)
    {
        tagh t = tag_at(name, body, pos, len);
        if (!placed && t.code == 1)
        {
            for (i = 0; i < nrows; i++)
                put_toggle(o, (uint16_t)(FIRST_NEW_DEPTH + 2 * i), y0 + (int32_t)(600 * (i + 1)), rows[i].toggle);
            placed = true;
        }
        ob_put(o, body + pos, t.hdr + t.len);
        pos += t.hdr + t.len;
        if (t.code == 0) break;
    }
    if (!placed) rt_fatal("options menu patch: %s: sprite %u has no frame", name, le16(body));
}

#define VIDEO_SPRITE 5
#define VIDEO_LAST_ROW_TWIPS 1608 /* tglGraphicQuality y 80.4 */
#define CONTROLLER_SPRITE 8
#define CONTROLLER_LAST_ROW_TWIPS 5204 /* tglInvertWheelHeli y 260.2 */

static void patch_overlay(const char *name, const uint8_t *d, size_t n, obuf *o)
{
    size_t pos = first_tag(name, d, n);
    int found = 0;
    start_movie(o, d, pos);
    while (pos < n)
    {
        tagh t = tag_at(name, d, pos, n);
        const uint8_t *b = d + pos + t.hdr;
        if (t.code == 39 && t.len >= 2 && (le16(b) == VIDEO_SPRITE || le16(b) == CONTROLLER_SPRITE))
        {
            obuf s = {0};
            if (le16(b) == VIDEO_SPRITE)
                patch_sprite(name, b, t.len, video_rows, NROWS(video_rows), VIDEO_LAST_ROW_TWIPS, &s);
            else
                patch_sprite(name, b, t.len, controller_rows, NROWS(controller_rows), CONTROLLER_LAST_ROW_TWIPS, &s);
            put_tag(o, 39, s.p, s.n);
            free(s.p);
            found++;
        }
        else
            ob_put(o, d + pos, t.hdr + t.len);
        pos += t.hdr + t.len;
        if (t.code == 0) break;
    }
    if (pos != n) rt_fatal("options menu patch: %s: data after the end tag", name);
    if (found != 2) rt_fatal("options menu patch: %s: the Video and Controller sprites were not found", name);
    finish_movie(o);
}

/* ---------------------------------------------------------- AVM1 code */

typedef struct push {
    obuf *o;
    size_t at;
} push;

static push push_begin(obuf *o)
{
    push p;
    ob_u8(o, 0x96);
    p.o = o;
    p.at = o->n;
    ob_u16(o, 0);
    return p;
}

static void push_str(push *p, const char *s)
{
    ob_u8(p->o, 0);
    ob_put(p->o, s, strlen(s) + 1);
}

static void push_int(push *p, int32_t v)
{
    ob_u8(p->o, 7);
    ob_u32(p->o, (uint32_t)v);
}

static void push_end(push *p)
{
    size_t len = p->o->n - p->at - 2;
    if (len > 0xFFFF) rt_fatal("options menu patch: push action too long");
    ob_set_u16(p->o, p->at, (uint16_t)len);
}

#define A_POP 0x17
#define A_GETVARIABLE 0x1c
#define A_NEWOBJECT 0x40
#define A_INITOBJECT 0x43
#define A_GETMEMBER 0x4e
#define A_CALLMETHOD 0x52

/* list.push(new optionscontainer(overlay.toggle, key, optionscontainer.DBTYPE_INT,
   label, [{mLabel, mValue}...])), as the rows of the game. */
static void emit_row(obuf *c, const menu_row *r, const char *overlay, const char *list)
{
    push p;
    unsigned k;
    if (r->count != (unsigned)rt_enh_max(r->id) + 1u)
        rt_fatal("options menu patch: row %s has %u values, the setting has %d", r->label, r->count, rt_enh_max(r->id) + 1);
    for (k = r->count; k-- > 0;)
    {
        p = push_begin(c);
        push_str(&p, "mLabel");
        push_str(&p, r->values[k]);
        push_str(&p, "mValue");
        push_int(&p, (int32_t)k);
        push_int(&p, 2);
        push_end(&p);
        ob_u8(c, A_INITOBJECT);
    }
    p = push_begin(c);
    push_int(&p, (int32_t)r->count);
    push_str(&p, "Array");
    push_end(&p);
    ob_u8(c, A_NEWOBJECT);
    p = push_begin(c);
    push_str(&p, r->label);
    push_str(&p, "optionscontainer");
    push_end(&p);
    ob_u8(c, A_GETVARIABLE);
    p = push_begin(c);
    push_str(&p, "DBTYPE_INT");
    push_end(&p);
    ob_u8(c, A_GETMEMBER);
    p = push_begin(c);
    push_str(&p, rt_enh_key(r->id));
    push_str(&p, overlay);
    push_end(&p);
    ob_u8(c, A_GETVARIABLE);
    p = push_begin(c);
    push_str(&p, r->toggle);
    push_end(&p);
    ob_u8(c, A_GETMEMBER);
    p = push_begin(c);
    push_int(&p, 5);
    push_str(&p, "optionscontainer");
    push_end(&p);
    ob_u8(c, A_NEWOBJECT);
    p = push_begin(c);
    push_int(&p, 1);
    push_str(&p, list);
    push_end(&p);
    ob_u8(c, A_GETVARIABLE);
    p = push_begin(c);
    push_str(&p, "push");
    push_end(&p);
    ob_u8(c, A_CALLMETHOD);
    ob_u8(c, A_POP);
}

static int64_t map_end(int64_t x, int64_t at, int64_t len) { return x <= at ? x : x + len; }

/* Copy the action list b (n bytes) to o with ins inserted at offset at.
   Branch offsets and block sizes that cross the insertion get the new
   values. A branch to offset at goes to the inserted code. */
static void avm1_insert(const char *name, const uint8_t *b, size_t n, size_t at, const uint8_t *ins, size_t len, obuf *o)
{
    size_t i = 0, base = o->n;
    bool boundary = false;
    ob_put(o, b, at);
    ob_put(o, ins, len);
    ob_put(o, b + at, n - at);
    while (i < n)
    {
        uint8_t op = b[i];
        size_t plen = 0, alen = 1, ni;
        if (i == at) boundary = true;
        if (op >= 0x80)
        {
            if (n - i < 3) rt_fatal("options menu patch: %s: action at %zu is cut off", name, i);
            plen = le16(b + i + 1);
            alen = 3 + plen;
            if (alen > n - i) rt_fatal("options menu patch: %s: action at %zu is cut off", name, i);
        }
        ni = i < at ? i : i + len; /* the new offset of this action */
        if (op == 0x99 || op == 0x9d) /* Jump, If */
        {
            int64_t e = (int64_t)(i + alen), t, noff;
            if (plen != 2) rt_fatal("options menu patch: %s: branch at %zu has a bad length", name, i);
            t = e + (int16_t)le16(b + i + 3);
            if (t < 0 || t > (int64_t)n) rt_fatal("options menu patch: %s: branch at %zu goes outside the code", name, i);
            noff = map_end(t, (int64_t)at, (int64_t)len) - map_end(e, (int64_t)at, (int64_t)len);
            if (noff < INT16_MIN || noff > INT16_MAX) rt_fatal("options menu patch: %s: branch at %zu is too long", name, i);
            ob_set_u16(o, base + ni + 3, (uint16_t)(int16_t)noff);
        }
        else if (op == 0x9b || op == 0x8e || op == 0x94) /* DefineFunction, DefineFunction2, With */
        {
            size_t so, sz, e = i + alen;
            if (plen < 2) rt_fatal("options menu patch: %s: block at %zu has a bad length", name, i);
            so = op == 0x94 ? i + 3 : i + alen - 2;
            sz = le16(b + so);
            if (sz > n - e) rt_fatal("options menu patch: %s: block at %zu goes outside the code", name, i);
            if (at == e && sz != 0) rt_fatal("options menu patch: %s: insertion at the start of block %zu", name, i);
            if (e < at && at < e + sz)
            {
                if (sz + len > 0xFFFF) rt_fatal("options menu patch: %s: block at %zu is too long", name, i);
                ob_set_u16(o, base + ni + (so - i), (uint16_t)(sz + len));
            }
        }
        else if (op == 0x8f) /* Try */
        {
            size_t e = i + alen, sz;
            if (plen < 7) rt_fatal("options menu patch: %s: try at %zu has a bad length", name, i);
            sz = (size_t)le16(b + i + 4) + le16(b + i + 6) + le16(b + i + 8);
            if (e <= at && at < e + sz) rt_fatal("options menu patch: %s: insertion in try block %zu", name, i);
        }
        i += alen;
    }
    if (!boundary) rt_fatal("options menu patch: %s: offset %zu is not the start of an action", name, at);
}

/* A frame 1 clip of a screen movie that the patch removes. */
typedef struct removed_clip {
    uint16_t depth, character;
} removed_clip;

#define MAX_REMOVED 11

/* A screen movie: insert code at offset code_at of the DoAction tag whose
   body starts at file offset action_body, move the PlaceObject2 clip
   move_name down by move_dy twips and remove the frame 1 clips of
   removed. */
typedef struct screen_patch {
    size_t action_body, code_at;
    const menu_row *rows;
    size_t nrows;
    const char *overlay, *list;
    const char *move_name;
    int32_t move_dy;
    removed_clip removed[MAX_REMOVED];
    size_t nremoved;
} screen_patch;

static void move_clip(const char *name, const uint8_t *b, size_t len, int32_t dy, obuf *o)
{
    size_t q = 3;
    swf_matrix m;
    obuf t = {0};
    uint8_t f;
    if (len < 3) rt_fatal("options menu patch: %s: PlaceObject2 is cut off", name);
    f = b[0];
    if (!(f & 0x04)) rt_fatal("options menu patch: %s: the clip to move has no matrix", name);
    if (f & 0x02) q += 2;
    if (q > len) rt_fatal("options menu patch: %s: PlaceObject2 is cut off", name);
    ob_put(&t, b, q);
    q += read_matrix(name, b + q, len - q, &m);
    if (q > len) rt_fatal("options menu patch: %s: PlaceObject2 is cut off", name);
    if ((int64_t)m.ty + dy > INT32_MAX / 2 || (int64_t)m.ty + dy < INT32_MIN / 2)
        rt_fatal("options menu patch: %s: the clip position is out of range", name);
    m.ty += dy;
    put_matrix(&t, &m);
    ob_put(&t, b + q, len - q);
    put_tag(o, 26, t.p, t.n);
    free(t.p);
}

/* The name of a PlaceObject2 (NULL if it has none). */
static const char *place_name(const uint8_t *b, size_t len, const char *name)
{
    size_t q = 3;
    swf_matrix m;
    uint8_t f;
    if (len < 3 || !(b[0] & 0x20)) return NULL;
    f = b[0];
    if (f & 0x02) q += 2;
    if (f & 0x04)
    {
        if (q > len) return NULL;
        q += read_matrix(name, b + q, len - q, &m);
    }
    if (f & 0x08)
    {
        /* CXFORMWITHALPHA */
        bitr r = {b + q, q <= len ? len - q : 0, 0, name};
        unsigned ha = br_u(&r, 1), hm = br_u(&r, 1), k = br_u(&r, 4), j;
        for (j = 0; j < (ha ? 4u : 0u) + (hm ? 4u : 0u); j++) (void)br_u(&r, k);
        q += (r.bit + 7) / 8;
    }
    if (f & 0x10) q += 2;
    if (q >= len || memchr(b + q, 0, len - q) == NULL) return NULL;
    return (const char *)b + q;
}

static bool is_removed_depth(const screen_patch *sp, uint16_t depth)
{
    size_t i;
    for (i = 0; i < sp->nremoved; i++)
        if (sp->removed[i].depth == depth) return true;
    return false;
}

/* The index in sp->removed of a PlaceObject2 (body b) in frame 1, or -1. */
static int removed_index(const char *name, const uint8_t *b, size_t len, const screen_patch *sp, bool frame1)
{
    size_t i;
    uint16_t depth;
    if (len < 3) rt_fatal("options menu patch: %s: PlaceObject2 is cut off", name);
    depth = le16(b + 1);
    for (i = 0; i < sp->nremoved; i++)
    {
        if (sp->removed[i].depth != depth) continue;
        if (!frame1 || !(b[0] & 0x02) || len < 5 || le16(b + 3) != sp->removed[i].character)
            rt_fatal("options menu patch: %s: depth %u is not the expected frame 1 clip", name, depth);
        return (int)i;
    }
    return -1;
}

static void patch_screen(const char *name, const uint8_t *d, size_t n, const screen_patch *sp, obuf *o)
{
    size_t pos = first_tag(name, d, n), i, nremoved = 0;
    bool code_done = false, moved = sp->move_name == NULL, frame1 = true;
    obuf code = {0};
    for (i = 0; i < sp->nrows; i++) emit_row(&code, &sp->rows[i], sp->overlay, sp->list);
    start_movie(o, d, pos);
    while (pos < n)
    {
        tagh t = tag_at(name, d, pos, n);
        const uint8_t *b = d + pos + t.hdr;
        if ((t.code == 26 && removed_index(name, b, t.len, sp, frame1) >= 0))
            nremoved++;
        else if ((t.code == 28 && t.len >= 2 && is_removed_depth(sp, le16(b))) ||
                 (t.code == 70 && t.len >= 4 && is_removed_depth(sp, le16(b + 2))))
            rt_fatal("options menu patch: %s: tag %u at %zu changes a removed clip", name, t.code, pos);
        else if (t.code == 12 && pos + t.hdr == sp->action_body)
        {
            obuf a = {0};
            if (sp->code_at >= t.len || b[sp->code_at] != 0x96)
                rt_fatal("options menu patch: %s: the options list code is not at offset %zu", name, sp->code_at);
            avm1_insert(name, b, t.len, sp->code_at, code.p, code.n, &a);
            put_tag(o, 12, a.p, a.n);
            free(a.p);
            code_done = true;
        }
        else if (t.code == 26 && sp->move_name != NULL && !moved)
        {
            const char *pn = place_name(b, t.len, name);
            if (pn != NULL && strcmp(pn, sp->move_name) == 0)
            {
                move_clip(name, b, t.len, sp->move_dy, o);
                moved = true;
            }
            else
                ob_put(o, d + pos, t.hdr + t.len);
        }
        else
            ob_put(o, d + pos, t.hdr + t.len);
        if (t.code == 1) frame1 = false;
        pos += t.hdr + t.len;
        if (t.code == 0) break;
    }
    free(code.p);
    if (pos != n) rt_fatal("options menu patch: %s: data after the end tag", name);
    if (nremoved != sp->nremoved) rt_fatal("options menu patch: %s: %zu clips to remove were not found", name, sp->nremoved - nremoved);
    if (!code_done) rt_fatal("options menu patch: %s: the options DoAction was not found", name);
    if (!moved) rt_fatal("options menu patch: %s: the clip %s was not found", name, sp->move_name);
    finish_movie(o);
}

/* Main menu Video: the list starts at y 144, as the Controller list. The
   overlay moves from y 422.35 to y 128 (the first row is 16 pixels below
   the overlay). The removed clips are the gamma box (3), the line below it
   (13), its corners (15-21), the gamma test marks (23-27) and the anchor
   clip of the gamma image (29, mcGammaImageAnchor). */
static const screen_patch video_patch = {
    .action_body = 83,
    .code_at = 13997, /* push mVideoConfirmParams, before InitializeOptions */
    .rows = video_rows,
    .nrows = NROWS(video_rows),
    .overlay = "mcVideoOverlay",
    .list = "mVideoOptions",
    .move_name = "mcVideoOverlay",
    .move_dy = -5887,
    .removed = {{3, 2}, {13, 12}, {15, 15}, {17, 15}, {19, 15}, {21, 15}, {23, 18}, {25, 21}, {27, 24}, {29, 25}},
    .nremoved = 10,
};

static const screen_patch controller_patch = {
    .action_body = 5865,
    .code_at = 15571, /* push ControllerOptionSelection, before InitializeOptions */
    .rows = controller_rows,
    .nrows = NROWS(controller_rows),
    .overlay = "mcControllerOverlay",
    .list = "mControllerOptions",
    .move_name = "mcControllerOverlay",
    .move_dy = 48 * 20,
    .nremoved = 0,
};

/* Pause menu Video: the overlay moves from y 450.5 to y 110.8, the
   position of the pause menu Controller overlay. The removed clips are the
   gamma box (13, 15), its corners (17-23), the gamma test marks (25-29),
   the line below the box (33) and the anchor clip of the gamma image (35,
   mcGammaImageAnchor). */
static const screen_patch pause_video_patch = {
    .action_body = 4151,
    .code_at = 14175, /* push mVideoConfirmParams, before InitializeOptions */
    .rows = video_rows,
    .nrows = NROWS(video_rows),
    .overlay = "mcVideoOverlay",
    .list = "mVideoOptions",
    .move_name = "mcVideoOverlay",
    .move_dy = -6794,
    .removed = {{13, 40}, {15, 40}, {17, 43}, {19, 43}, {21, 43}, {23, 43}, {25, 46}, {27, 49}, {29, 52}, {33, 57}, {35, 58}},
    .nremoved = 11,
};

static const screen_patch pause_controller_patch = {
    .action_body = 4175,
    .code_at = 15068, /* push PauseControllerOptionSelection, before InitializeOptions */
    .rows = controller_rows,
    .nrows = NROWS(controller_rows),
    .overlay = "mcControllerOverlay",
    .list = "mControllerOptions",
    .move_name = NULL,
    .move_dy = 0,
    .nremoved = 0,
};

/* ------------------------------------------------------ the RCF file */

typedef struct movie_def {
    const char *rcf_name;
    size_t size;
    const char *sha256;
} movie_def;

enum { M_OVERLAY, M_VIDEO, M_CONTROLLER, M_PAUSE_VIDEO, M_PAUSE_CONTROLLER, M_COUNT };

static const movie_def movies[M_COUNT] = {
    [M_OVERLAY] = {"\\art\\hud\\protooptionsoverlay.gfx", 2692,
                   "6e333e07d22508f5f39cbe3153f6684f590fc8952adffd468a4fa22011ef3e79"},
    [M_VIDEO] = {"\\art\\hud\\protovideooption.gfx", 34329,
                 "b400e0b9ed18465d56f8f00856759faaac3714b97903e2bc9bda94d1e2ff8c6f"},
    [M_CONTROLLER] = {"\\art\\hud\\protocontrolleroption.gfx", 43952,
                      "eb42e2cea6bccccd31128889b17142079089484e5854ee8772920e0502c95571"},
    [M_PAUSE_VIDEO] = {"\\art\\hud\\protopausevideo.gfx", 61474,
                       "ee6b7512c86718e7ae4c4838424e0cad3afa3a1d101c2216cb74d82ae8a05d64"},
    [M_PAUSE_CONTROLLER] = {"\\art\\hud\\protopausecontroller.gfx", 61877,
                            "41c839719dd0aa5d914fb610447cb063f3db961c47ce93fc3f56ab7578114af9"},
};

#define RCF_HEADER 0x3c
#define RCF_ALIGN 2048u
#define RCF_MAX_ENTRIES (1u << 20)

struct rt_file_overlay {
    dev_t dev;
    ino_t ino;
    uint64_t base_size, size;
    struct {
        uint64_t off;
        uint8_t b[8];
    } patch[M_COUNT];
    uint64_t blob_off;
    obuf blob;
};

static const screen_patch *const screens[M_COUNT] = {
    [M_VIDEO] = &video_patch,
    [M_CONTROLLER] = &controller_patch,
    [M_PAUSE_VIDEO] = &pause_video_patch,
    [M_PAUSE_CONTROLLER] = &pause_controller_patch,
};

static pthread_mutex_t overlay_lock = PTHREAD_MUTEX_INITIALIZER;
static rt_file_overlay *overlay;

static void read_full(int fd, void *dst, size_t n, uint64_t off, const char *what)
{
    size_t done = 0;
    while (done < n)
    {
        ssize_t r = pread(fd, (uint8_t *)dst + done, n - done, (off_t)(off + done));
        if (r < 0)
        {
            if (errno == EINTR) continue;
            rt_fatal("options menu patch: art.rcf: %s cannot be read: %s", what, strerror(errno));
        }
        if (r == 0) rt_fatal("options menu patch: art.rcf: %s is cut off", what);
        done += (size_t)r;
    }
}

typedef struct rcf_ent {
    uint32_t hash, off, size, index;
} rcf_ent;

static int ent_cmp(const void *a, const void *b)
{
    const rcf_ent *x = a, *y = b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

static void check_sha(const movie_def *m, const uint8_t *d)
{
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    unsigned i;
    CC_SHA256(d, (CC_LONG)m->size, md);
    for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
    if (strcmp(hex, m->sha256) != 0)
        rt_fatal("options menu patch: %s in art.rcf has SHA-256 %s; the supported version has %s. Only the Steam release "
                 "of the game is supported.",
                 m->rcf_name, hex, m->sha256);
}

static rt_file_overlay *build_overlay(int fd, const struct stat *st)
{
    uint8_t h[RCF_HEADER];
    uint32_t doff, dsz, noff, nsz, cnt, i, k;
    uint8_t *dir, *names;
    rcf_ent *ents;
    size_t npos = 8;
    int found[M_COUNT];
    rt_file_overlay *ov;

    if ((uint64_t)st->st_size < RCF_HEADER) rt_fatal("options menu patch: art.rcf is too small");
    read_full(fd, h, RCF_HEADER, 0, "the header");
    if (memcmp(h, "ATG CORE CEMENT LIBRARY", 23) != 0) rt_fatal("options menu patch: art.rcf is not a Cement archive");
    doff = le32(h + 0x24);
    dsz = le32(h + 0x28);
    noff = le32(h + 0x2c);
    nsz = le32(h + 0x30);
    cnt = le32(h + 0x38);
    if (cnt == 0 || cnt > RCF_MAX_ENTRIES || dsz != cnt * 12u || (uint64_t)doff + dsz > (uint64_t)st->st_size ||
        (uint64_t)noff + nsz > (uint64_t)st->st_size || nsz < 8)
        rt_fatal("options menu patch: art.rcf has a directory that is not valid");
    dir = malloc(dsz);
    names = malloc(nsz);
    ents = calloc(cnt, sizeof *ents);
    if (dir == NULL || names == NULL || ents == NULL) rt_fatal("options menu patch: out of host memory");
    read_full(fd, dir, dsz, doff, "the directory");
    read_full(fd, names, nsz, noff, "the name table");
    for (i = 0; i < cnt; i++)
    {
        ents[i].hash = le32(dir + 12 * i);
        ents[i].off = le32(dir + 12 * i + 4);
        ents[i].size = le32(dir + 12 * i + 8);
        ents[i].index = i;
        if ((uint64_t)ents[i].off + ents[i].size > (uint64_t)st->st_size)
            rt_fatal("options menu patch: art.rcf entry %u is outside the file", i);
    }
    /* The name table lists the names in the order of the file offsets. */
    qsort(ents, cnt, sizeof *ents, ent_cmp);
    for (k = 0; k < M_COUNT; k++) found[k] = -1;
    for (i = 0; i < cnt; i++)
    {
        uint32_t len;
        const char *nm;
        if (nsz - npos < 16) rt_fatal("options menu patch: art.rcf name table is cut off");
        len = le32(names + npos + 12);
        if (len == 0 || len > nsz - npos - 16) rt_fatal("options menu patch: art.rcf name table is cut off");
        nm = (const char *)names + npos + 16;
        if (nm[len - 1] != 0) rt_fatal("options menu patch: art.rcf name %u has no end", i);
        for (k = 0; k < M_COUNT; k++)
            if (strcasecmp(nm, movies[k].rcf_name) == 0)
            {
                if (found[k] >= 0) rt_fatal("options menu patch: art.rcf has %s two times", movies[k].rcf_name);
                found[k] = (int)i;
            }
        npos += 16u + len;
        npos = npos + 3 <= nsz ? npos + 3 : nsz;
    }

    ov = calloc(1, sizeof *ov);
    if (ov == NULL) rt_fatal("options menu patch: out of host memory");
    ov->dev = st->st_dev;
    ov->ino = st->st_ino;
    ov->base_size = (uint64_t)st->st_size;
    ov->blob_off = (ov->base_size + RCF_ALIGN - 1) / RCF_ALIGN * RCF_ALIGN;
    for (k = 0; k < M_COUNT; k++)
    {
        const rcf_ent *e;
        uint8_t *d;
        obuf out = {0};
        uint64_t at;
        static const uint8_t zero[RCF_ALIGN];
        if (found[k] < 0) rt_fatal("options menu patch: art.rcf has no %s", movies[k].rcf_name);
        e = &ents[found[k]];
        if (e->size != movies[k].size)
            rt_fatal("options menu patch: %s in art.rcf has %u bytes; the supported version has %zu. Only the Steam "
                     "release of the game is supported.",
                     movies[k].rcf_name, e->size, movies[k].size);
        d = malloc(e->size);
        if (d == NULL) rt_fatal("options menu patch: out of host memory");
        read_full(fd, d, e->size, e->off, movies[k].rcf_name);
        check_sha(&movies[k], d);
        if (k == M_OVERLAY) patch_overlay(movies[k].rcf_name, d, e->size, &out);
        else patch_screen(movies[k].rcf_name, d, e->size, screens[k], &out);
        free(d);
        at = ov->blob_off + ov->blob.n;
        if (at + out.n > UINT32_MAX) rt_fatal("options menu patch: art.rcf is too large for the patched menus");
        ob_put(&ov->blob, out.p, out.n);
        if (ov->blob.n % RCF_ALIGN != 0) ob_put(&ov->blob, zero, RCF_ALIGN - ov->blob.n % RCF_ALIGN);
        ov->patch[k].off = (uint64_t)doff + 12u * e->index + 4u;
        ov->patch[k].b[0] = (uint8_t)at;
        ov->patch[k].b[1] = (uint8_t)(at >> 8);
        ov->patch[k].b[2] = (uint8_t)(at >> 16);
        ov->patch[k].b[3] = (uint8_t)(at >> 24);
        ov->patch[k].b[4] = (uint8_t)out.n;
        ov->patch[k].b[5] = (uint8_t)(out.n >> 8);
        ov->patch[k].b[6] = (uint8_t)(out.n >> 16);
        ov->patch[k].b[7] = (uint8_t)(out.n >> 24);
        rt_log("options menu patch: %s: %u bytes -> %zu bytes", movies[k].rcf_name, e->size, out.n);
        free(out.p);
    }
    ov->size = ov->blob_off + ov->blob.n;
    free(dir);
    free(names);
    free(ents);
    return ov;
}

static bool is_art_rcf(const char *host)
{
    const char *s = strrchr(host, '/');
    return strcasecmp(s != NULL ? s + 1 : host, "art.rcf") == 0;
}

const rt_file_overlay *rt_menu_overlay(const char *host, int fd)
{
    struct stat st;
    const rt_file_overlay *ov;
    if (!is_art_rcf(host)) return NULL;
    if (fstat(fd, &st) != 0) rt_fatal("options menu patch: %s cannot be read: %s", host, strerror(errno));
    if (pthread_mutex_lock(&overlay_lock) != 0) rt_fatal("options menu patch: lock failed");
    if (overlay == NULL) overlay = build_overlay(fd, &st);
    else if (overlay->dev != st.st_dev || overlay->ino != st.st_ino || overlay->base_size != (uint64_t)st.st_size)
        rt_fatal("options menu patch: %s is not the art.rcf that the game opened first", host);
    ov = overlay;
    if (pthread_mutex_unlock(&overlay_lock) != 0) rt_fatal("options menu patch: unlock failed");
    return ov;
}

uint64_t rt_overlay_size(const rt_file_overlay *ov)
{
    return ov->size;
}

ssize_t rt_overlay_pread(const rt_file_overlay *ov, int fd, void *dst, size_t n, uint64_t pos)
{
    uint8_t *d = dst;
    size_t done = 0;
    unsigned k;
    if (pos >= ov->size || n == 0) return 0;
    if (n > ov->size - pos) n = (size_t)(ov->size - pos);
    if (n > SSIZE_MAX) n = SSIZE_MAX;
    while (done < n)
    {
        uint64_t p = pos + done;
        size_t c;
        if (p < ov->base_size)
        {
            ssize_t r;
            c = n - done;
            if (c > ov->base_size - p) c = (size_t)(ov->base_size - p);
            r = pread(fd, d + done, c, (off_t)p);
            if (r < 0)
            {
                if (errno == EINTR) continue;
                return -1;
            }
            if (r == 0)
            {
                errno = EIO; /* the file became shorter */
                return -1;
            }
            done += (size_t)r;
        }
        else if (p < ov->blob_off)
        {
            c = n - done;
            if (c > ov->blob_off - p) c = (size_t)(ov->blob_off - p);
            memset(d + done, 0, c);
            done += c;
        }
        else
        {
            c = n - done;
            memcpy(d + done, ov->blob.p + (p - ov->blob_off), c);
            done += c;
        }
    }
    for (k = 0; k < M_COUNT; k++)
    {
        uint64_t a = ov->patch[k].off, b = a + 8, lo = a > pos ? a : pos, hi = b < pos + n ? b : pos + n;
        if (lo < hi) memcpy(d + (lo - pos), ov->patch[k].b + (lo - a), (size_t)(hi - lo));
    }
    return (ssize_t)n;
}
