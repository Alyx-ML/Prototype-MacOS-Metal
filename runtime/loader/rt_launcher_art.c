/*
 * The title art of the launcher window. See rt_launcher_art.h.
 *
 * Formats (the same reading rules as runtime/src/rt_menu.c for art.rcf):
 *   - art.rcf: a Cement archive. Header 0x3c bytes: the directory offset
 *     and size at 0x24 and 0x28, the name table offset and size at 0x2c
 *     and 0x30, the entry count at 0x38. A directory entry is 12 bytes
 *     (hash, offset, size). The name table lists the names in the order of
 *     the file offsets: 16 bytes (the length at +12), the name, 3 bytes.
 *   - *.p3d.rz: "RZ", the inflated size at +8, a zlib stream at +16.
 *   - Pure3D: "P3D\xff", then chunks of (id, header size, total size).
 *     A texture is chunk 0x19000 (name: a length byte and the name), with
 *     an image chunk 0x19006 that holds an image data chunk 0x19002: the
 *     data size at +12, then a DDS file at +16.
 *   - DDS: the height at +12, the width at +16, the FourCC at +84, the
 *     blocks at +128. The game stores the bottom row first.
 */
#include "rt_launcher_art.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define RCF_HEADER 0x3cu
#define RCF_MAX_ENTRIES (1u << 20)
#define RCF_MAX_TABLE (64u << 20)
#define FRONTEND_NAME "\\art\\hud\\fe_frontend.p3d.rz"
#define FRONTEND_MAX (8u << 20)  /* compressed size limit */
#define INFLATED_MAX (32u << 20) /* inflated size limit */
#define P3D_TEXTURE 0x19000u
#define P3D_IMAGE 0x19006u
#define P3D_IMAGE_DATA 0x19002u
#define DDS_HEADER 128u

typedef struct art_texture {
    const char *name;
    uint32_t width, height;
    const char *fourcc;
} art_texture;

static const art_texture logo_tex = { "protomainmenu__I1F.dds", 512, 128, "DXT5" };
static const art_texture background_tex = { "protostart__IA.dds", 512, 512, "DXT1" };

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool read_at(int fd, void *dst, size_t n, uint64_t off, const char *what, char *err, size_t errn)
{
    size_t done = 0;
    while (done < n)
    {
        ssize_t r = pread(fd, (uint8_t *)dst + done, n - done, (off_t)(off + done));
        if (r < 0)
        {
            if (errno == EINTR) continue;
            snprintf(err, errn, "art.rcf: %s cannot be read (%s).", what, strerror(errno));
            return false;
        }
        if (r == 0)
        {
            snprintf(err, errn, "art.rcf: %s is cut off.", what);
            return false;
        }
        done += (size_t)r;
    }
    return true;
}

typedef struct rcf_ent {
    uint32_t off, size;
} rcf_ent;

static int ent_cmp(const void *a, const void *b)
{
    const rcf_ent *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

/* Find FRONTEND_NAME in the archive. Returns false with err on a problem. */
static bool find_frontend(int fd, uint64_t file_size, rcf_ent *out, char *err, size_t errn)
{
    uint8_t h[RCF_HEADER];
    uint32_t doff, dsz, noff, nsz, cnt, i;
    uint8_t *dir = NULL, *names = NULL;
    rcf_ent *ents = NULL;
    size_t npos = 8;
    bool ok = false, found = false;

    if (file_size < RCF_HEADER)
    {
        snprintf(err, errn, "art.rcf is too small.");
        return false;
    }
    if (!read_at(fd, h, RCF_HEADER, 0, "the header", err, errn)) return false;
    if (memcmp(h, "ATG CORE CEMENT LIBRARY", 23) != 0)
    {
        snprintf(err, errn, "art.rcf is not a Cement archive.");
        return false;
    }
    doff = le32(h + 0x24);
    dsz = le32(h + 0x28);
    noff = le32(h + 0x2c);
    nsz = le32(h + 0x30);
    cnt = le32(h + 0x38);
    if (cnt == 0 || cnt > RCF_MAX_ENTRIES || dsz != cnt * 12u || nsz < 8 || nsz > RCF_MAX_TABLE ||
        (uint64_t)doff + dsz > file_size || (uint64_t)noff + nsz > file_size)
    {
        snprintf(err, errn, "art.rcf has a directory that is not valid.");
        return false;
    }
    dir = malloc(dsz);
    names = malloc(nsz);
    ents = calloc(cnt, sizeof *ents);
    if (dir == NULL || names == NULL || ents == NULL)
    {
        snprintf(err, errn, "Out of memory while art.rcf was read.");
        goto done;
    }
    if (!read_at(fd, dir, dsz, doff, "the directory", err, errn) ||
        !read_at(fd, names, nsz, noff, "the name table", err, errn))
        goto done;
    for (i = 0; i < cnt; i++)
    {
        ents[i].off = le32(dir + 12u * i + 4u);
        ents[i].size = le32(dir + 12u * i + 8u);
        if ((uint64_t)ents[i].off + ents[i].size > file_size)
        {
            snprintf(err, errn, "art.rcf entry %u is outside the file.", i);
            goto done;
        }
    }
    qsort(ents, cnt, sizeof *ents, ent_cmp);
    for (i = 0; i < cnt; i++)
    {
        uint32_t len;
        const char *nm;
        if (nsz - npos < 16)
        {
            snprintf(err, errn, "The name table of art.rcf is cut off.");
            goto done;
        }
        len = le32(names + npos + 12);
        if (len == 0 || len > nsz - npos - 16)
        {
            snprintf(err, errn, "The name table of art.rcf is cut off.");
            goto done;
        }
        nm = (const char *)names + npos + 16;
        if (nm[len - 1] != 0)
        {
            snprintf(err, errn, "Name %u of art.rcf has no end.", i);
            goto done;
        }
        if (strcasecmp(nm, FRONTEND_NAME) == 0)
        {
            if (found)
            {
                snprintf(err, errn, "art.rcf has %s two times.", FRONTEND_NAME);
                goto done;
            }
            found = true;
            *out = ents[i];
        }
        npos += 16u + len;
        npos = npos + 3 <= nsz ? npos + 3 : nsz;
    }
    if (!found)
    {
        snprintf(err, errn, "art.rcf has no %s.", FRONTEND_NAME);
        goto done;
    }
    ok = true;
done:
    free(dir);
    free(names);
    free(ents);
    return ok;
}

/* Inflate the RZ data. Returns the inflated bytes (malloc) or NULL with err. */
static uint8_t *inflate_rz(const uint8_t *d, uint32_t n, uint32_t *out_size, char *err, size_t errn)
{
    z_stream z;
    uint8_t *out;
    uint32_t usize;
    int r;
    if (n < 16 || d[0] != 'R' || d[1] != 'Z')
    {
        snprintf(err, errn, "%s in art.rcf is not RZ data.", FRONTEND_NAME);
        return NULL;
    }
    usize = le32(d + 8);
    if (usize < 12 || usize > INFLATED_MAX)
    {
        snprintf(err, errn, "%s in art.rcf has an inflated size of %u bytes.", FRONTEND_NAME, usize);
        return NULL;
    }
    out = malloc(usize);
    if (out == NULL)
    {
        snprintf(err, errn, "Out of memory while art.rcf was read.");
        return NULL;
    }
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK)
    {
        free(out);
        snprintf(err, errn, "zlib cannot start (%s).", z.msg != NULL ? z.msg : "no message");
        return NULL;
    }
    z.next_in = (Bytef *)(uintptr_t)(d + 16);
    z.avail_in = n - 16;
    z.next_out = out;
    z.avail_out = usize;
    r = inflate(&z, Z_FINISH);
    if (r != Z_STREAM_END || z.total_out != usize)
    {
        snprintf(err, errn, "%s in art.rcf cannot be inflated (zlib %d%s%s).", FRONTEND_NAME, r,
                 z.msg != NULL ? ": " : "", z.msg != NULL ? z.msg : "");
        inflateEnd(&z);
        free(out);
        return NULL;
    }
    if (inflateEnd(&z) != Z_OK)
    {
        snprintf(err, errn, "zlib cannot end.");
        free(out);
        return NULL;
    }
    *out_size = usize;
    return out;
}

/* The chunk at p (inside [p, end)): id, header size, total size. */
static bool chunk_at(const uint8_t *d, size_t p, size_t end, uint32_t *id, uint32_t *hs, uint32_t *ts)
{
    if (end - p < 12) return false;
    *id = le32(d + p);
    *hs = le32(d + p + 4);
    *ts = le32(d + p + 8);
    return *hs >= 12 && *hs <= *ts && *ts <= end - p;
}

/* The first child chunk with the id in [p, end), or false. */
static bool find_child(const uint8_t *d, size_t p, size_t end, uint32_t want, size_t *at, uint32_t *hs, uint32_t *ts)
{
    while (p < end)
    {
        uint32_t id;
        if (!chunk_at(d, p, end, &id, hs, ts)) return false;
        if (id == want)
        {
            *at = p;
            return true;
        }
        p += *ts;
    }
    return false;
}

static void c565(uint16_t c, uint8_t rgb[3])
{
    rgb[0] = (uint8_t)(((c >> 11) & 31u) * 255u / 31u);
    rgb[1] = (uint8_t)(((c >> 5) & 63u) * 255u / 63u);
    rgb[2] = (uint8_t)((c & 31u) * 255u / 31u);
}

/* Decode DXT1 or DXT5 blocks (w and h multiples of 4) into top-row-first
   RGBA: the game stores the bottom row first. */
static void decode_dxt(const uint8_t *src, uint32_t w, uint32_t h, bool dxt5, uint8_t *dst)
{
    uint32_t bx, by, i;
    size_t bs = dxt5 ? 16 : 8;
    for (by = 0; by < h; by += 4)
        for (bx = 0; bx < w; bx += 4)
        {
            const uint8_t *b = src;
            uint8_t alpha[16], cols[4][4];
            uint16_t c0, c1;
            uint32_t idx;
            src += bs;
            for (i = 0; i < 16; i++) alpha[i] = 255;
            if (dxt5)
            {
                uint8_t a[8];
                uint64_t bits = 0;
                a[0] = b[0];
                a[1] = b[1];
                for (i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                if (a[0] > a[1])
                    for (i = 0; i < 6; i++) a[2 + i] = (uint8_t)(((6u - i) * a[0] + (i + 1u) * a[1]) / 7u);
                else
                {
                    for (i = 0; i < 4; i++) a[2 + i] = (uint8_t)(((4u - i) * a[0] + (i + 1u) * a[1]) / 5u);
                    a[6] = 0;
                    a[7] = 255;
                }
                for (i = 0; i < 16; i++) alpha[i] = a[(bits >> (3 * i)) & 7u];
                b += 8;
            }
            c0 = le16(b);
            c1 = le16(b + 2);
            idx = le32(b + 4);
            c565(c0, cols[0]);
            c565(c1, cols[1]);
            cols[0][3] = cols[1][3] = cols[2][3] = cols[3][3] = 255;
            for (i = 0; i < 3; i++)
            {
                if (dxt5 || c0 > c1)
                {
                    cols[2][i] = (uint8_t)((2u * cols[0][i] + cols[1][i]) / 3u);
                    cols[3][i] = (uint8_t)((cols[0][i] + 2u * cols[1][i]) / 3u);
                }
                else
                {
                    cols[2][i] = (uint8_t)((cols[0][i] + cols[1][i]) / 2u);
                    cols[3][i] = 0;
                }
            }
            if (!dxt5 && c0 <= c1) cols[3][3] = 0;
            for (i = 0; i < 16; i++)
            {
                uint32_t x = bx + i % 4, y = by + i / 4;
                const uint8_t *c = cols[(idx >> (2 * i)) & 3u];
                uint8_t *o = dst + ((size_t)(h - 1 - y) * w + x) * 4;
                o[0] = c[0];
                o[1] = c[1];
                o[2] = c[2];
                o[3] = (uint8_t)(c[3] * alpha[i] / 255u);
            }
        }
}

/* Find and decode one texture of the Pure3D data. */
static bool decode_texture(const uint8_t *p3d, size_t n, const art_texture *t, rt_art_image *img, char *err, size_t errn)
{
    size_t p = 12, at, blocks;
    uint32_t hs, ts, ihs, its, dhs, dts, data_size, w, h;
    const uint8_t *dds;
    bool dxt5;
    while (p < n)
    {
        uint32_t id;
        size_t namelen;
        if (!chunk_at(p3d, p, n, &id, &hs, &ts))
        {
            snprintf(err, errn, "The Pure3D data of %s has a chunk that is not valid.", FRONTEND_NAME);
            return false;
        }
        namelen = strlen(t->name);
        if (id == P3D_TEXTURE && hs >= 13 && p3d[p + 12] >= namelen && (size_t)p3d[p + 12] <= hs - 13 &&
            memcmp(p3d + p + 13, t->name, namelen) == 0 && (namelen == p3d[p + 12] || p3d[p + 13 + namelen] == 0))
            break;
        p += ts;
    }
    if (p >= n)
    {
        snprintf(err, errn, "%s has no texture %s.", FRONTEND_NAME, t->name);
        return false;
    }
    if (!find_child(p3d, p + hs, p + ts, P3D_IMAGE, &at, &ihs, &its) ||
        !find_child(p3d, at + ihs, at + its, P3D_IMAGE_DATA, &at, &dhs, &dts) || dhs < 16)
    {
        snprintf(err, errn, "The texture %s has no image data.", t->name);
        return false;
    }
    data_size = le32(p3d + at + 12);
    if (data_size < DDS_HEADER || data_size > dhs - 16)
    {
        snprintf(err, errn, "The image data of %s has a size that is not valid.", t->name);
        return false;
    }
    dds = p3d + at + 16;
    w = le32(dds + 16);
    h = le32(dds + 12);
    if (memcmp(dds, "DDS ", 4) != 0 || le32(dds + 4) != 124 || w != t->width || h != t->height ||
        memcmp(dds + 84, t->fourcc, 4) != 0)
    {
        snprintf(err, errn, "The texture %s is not a %ux%u %s image. Only the Steam release of the game is supported.",
                 t->name, t->width, t->height, t->fourcc);
        return false;
    }
    dxt5 = strcmp(t->fourcc, "DXT5") == 0;
    blocks = (size_t)(w / 4) * (h / 4);
    if (data_size - DDS_HEADER < blocks * (dxt5 ? 16u : 8u))
    {
        snprintf(err, errn, "The image data of %s is cut off.", t->name);
        return false;
    }
    img->rgba = malloc((size_t)w * h * 4);
    if (img->rgba == NULL)
    {
        snprintf(err, errn, "Out of memory while art.rcf was read.");
        return false;
    }
    decode_dxt(dds + DDS_HEADER, w, h, dxt5, img->rgba);
    img->width = w;
    img->height = h;
    return true;
}

void rt_art_free(rt_art_image *img)
{
    free(img->rgba);
    img->rgba = NULL;
    img->width = img->height = 0;
}

bool rt_launcher_load_art(const char *game_dir, rt_art_image *logo, rt_art_image *background, char *err, size_t errn)
{
    char path[1024];
    struct stat st;
    rcf_ent e;
    uint8_t *packed = NULL, *p3d = NULL;
    uint32_t p3d_size = 0;
    bool ok = false;
    int fd, r;

    memset(logo, 0, sizeof *logo);
    memset(background, 0, sizeof *background);
    r = snprintf(path, sizeof path, "%s/art.rcf", game_dir);
    if (r <= 0 || (size_t)r >= sizeof path)
    {
        snprintf(err, errn, "The game folder path is too long.");
        return false;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        snprintf(err, errn, "art.rcf cannot be opened (%s).", strerror(errno));
        return false;
    }
    if (fstat(fd, &st) != 0)
    {
        snprintf(err, errn, "art.rcf cannot be read (%s).", strerror(errno));
        goto done;
    }
    if (!find_frontend(fd, (uint64_t)st.st_size, &e, err, errn)) goto done;
    if (e.size > FRONTEND_MAX)
    {
        snprintf(err, errn, "%s in art.rcf has %u bytes.", FRONTEND_NAME, e.size);
        goto done;
    }
    packed = malloc(e.size > 0 ? e.size : 1);
    if (packed == NULL)
    {
        snprintf(err, errn, "Out of memory while art.rcf was read.");
        goto done;
    }
    if (!read_at(fd, packed, e.size, e.off, FRONTEND_NAME, err, errn)) goto done;
    p3d = inflate_rz(packed, e.size, &p3d_size, err, errn);
    if (p3d == NULL) goto done;
    if (memcmp(p3d, "P3D\xff", 4) != 0 || le32(p3d + 4) != 12 || le32(p3d + 8) != p3d_size)
    {
        snprintf(err, errn, "%s in art.rcf is not Pure3D data.", FRONTEND_NAME);
        goto done;
    }
    if (!decode_texture(p3d, p3d_size, &logo_tex, logo, err, errn) ||
        !decode_texture(p3d, p3d_size, &background_tex, background, err, errn))
    {
        rt_art_free(logo);
        rt_art_free(background);
        goto done;
    }
    ok = true;
done:
    free(packed);
    free(p3d);
    if (close(fd) != 0 && ok)
    {
        snprintf(err, errn, "art.rcf cannot be closed (%s).", strerror(errno));
        rt_art_free(logo);
        rt_art_free(background);
        ok = false;
    }
    return ok;
}
