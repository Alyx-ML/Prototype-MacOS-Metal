/*
 * Direct3D 9 on Metal: formats and resources (textures, surfaces, vertex
 * and index buffers, vertex declarations, shaders, queries). See
 * rt_d3d9_int.h.
 *
 * Lock memory: a lock returns guest memory. A texture level keeps its
 * guest copy after the first lock (as the system-memory copy of a
 * D3DPOOL_MANAGED texture does on Windows); the unlock uploads it. A
 * buffer lock uses a guest staging block; the unlock copies the locked
 * range into the Metal buffer.
 *
 * GPU hazards: a write to a resource that a command buffer in flight can
 * still read waits for that command buffer, except:
 *   - D3DLOCK_DISCARD on a buffer or a one-level texture renames the
 *     storage (a new Metal buffer or texture; the old one is reused after
 *     its command buffer completes);
 *   - D3DLOCK_NOOVERWRITE on a buffer: the application promises not to
 *     touch data that the GPU uses, so there is no wait.
 */
#import "rt_d3d9_int.h"

#include <stdlib.h>
#include <string.h>

#define MAX_RETIRED 8u

/* Renamed storage of one dynamic buffer: the engine can discard one
   buffer many times in one frame (each discard holds the old storage
   until its command buffer completes). The limit is in bytes, so that a
   small buffer can be renamed often and a large buffer uses a bounded
   amount of memory. When the limit is reached, the lock waits for the
   oldest storage. */
#define BUFFER_RETIRED_BYTES (16u << 20)
#define BUFFER_RETIRED_MAX 512u

/* IIDs (d3d9.h). UNCONFIRMED: recalled values; QueryInterface with an
   unknown IID stops the process with the IID text. */
static const rt_guid IID_IDirect3DResource9 = { 0x05EEC05Du, 0x8F7D, 0x4362, { 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04 } };
static const rt_guid IID_IDirect3DBaseTexture9 = { 0x580CA87Eu, 0x1D3C, 0x4D54, { 0x99, 0x1D, 0xB7, 0xD3, 0xE3, 0xC2, 0x98, 0xCE } };
static const rt_guid IID_IDirect3DTexture9 = { 0x85C31227u, 0x3DE5, 0x4F00, { 0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5 } };
static const rt_guid IID_IDirect3DCubeTexture9 = { 0xFFF32F81u, 0xD953, 0x473A, { 0x92, 0x23, 0x93, 0xD6, 0x52, 0xAB, 0xA9, 0x3F } };
/* UNCONFIRMED: the value of d3d9.h; the engine does not query it. */
static const rt_guid IID_IDirect3DVolumeTexture9 = { 0x2518526Cu, 0xE789, 0x4111, { 0xA7, 0xB9, 0x47, 0xEF, 0x32, 0x8D, 0x13, 0xE6 } };
static const rt_guid IID_IDirect3DSurface9 = { 0x0CFBAF3Au, 0x9FF6, 0x429A, { 0x99, 0xB3, 0xA2, 0x79, 0x6A, 0xF8, 0xB8, 0x9B } };
static const rt_guid IID_IDirect3DVertexBuffer9 = { 0xB64BB1B5u, 0xFD70, 0x4DF6, { 0xBF, 0x91, 0x19, 0xD0, 0xA1, 0x24, 0x55, 0xE3 } };
static const rt_guid IID_IDirect3DIndexBuffer9 = { 0x7C9DD65Eu, 0xD3F7, 0x4529, { 0xAC, 0xEE, 0x78, 0x58, 0x30, 0xAC, 0xDE, 0x35 } };
static const rt_guid IID_IDirect3DVertexDeclaration9 = { 0xDD13C59Cu, 0x36FA, 0x4098, { 0xA8, 0xFB, 0xC7, 0xED, 0x39, 0xDC, 0x85, 0x46 } };
static const rt_guid IID_IDirect3DVertexShader9 = { 0xEFC5557Eu, 0x6265, 0x4613, { 0x8A, 0x94, 0x43, 0x85, 0x78, 0x89, 0xEB, 0x36 } };
static const rt_guid IID_IDirect3DPixelShader9 = { 0x6D3BDBDCu, 0x5B02, 0x4415, { 0xB8, 0x52, 0xCE, 0x5E, 0x8B, 0xCC, 0xB2, 0x89 } };
static const rt_guid IID_IDirect3DQuery9 = { 0xD9771460u, 0xA695, 0x4F26, { 0xBB, 0xD3, 0x27, 0xB8, 0x40, 0xB5, 0x41, 0xCC } };

/* ---------------------------------------------------------------- formats */

#define SWZ(r, g, b, a) { MTLTextureSwizzle##r, MTLTextureSwizzle##g, MTLTextureSwizzle##b, MTLTextureSwizzle##a }
#define NOSWZ SWZ(Red, Green, Blue, Alpha)

static const d3d_format formats[] = {
    /* d3d, pf, pf_srgb, bytes, conv, block, depth, stencil, renderable, blendable, filterable, swizzle, swz, guest_bytes */
    { D3DFMT_A8R8G8B8, MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB, 4, 0, false, false, false, true, true, true, false, NOSWZ, 4 },
    { D3DFMT_X8R8G8B8, MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB, 4, 0, false, false, false, true, true, true, true, SWZ(Red, Green, Blue, One), 4 },
    { D3DFMT_A8B8G8R8, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB, 4, 0, false, false, false, true, true, true, false, NOSWZ, 4 },
    { D3DFMT_X8B8G8R8, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB, 4, 0, false, false, false, true, true, true, true, SWZ(Red, Green, Blue, One), 4 },
    { D3DFMT_R5G6B5, MTLPixelFormatB5G6R5Unorm, MTLPixelFormatInvalid, 2, 0, false, false, false, true, true, true, false, NOSWZ, 2 },
    { D3DFMT_X1R5G5B5, MTLPixelFormatBGR5A1Unorm, MTLPixelFormatInvalid, 2, 0, false, false, false, true, true, true, true, SWZ(Red, Green, Blue, One), 2 },
    { D3DFMT_A1R5G5B5, MTLPixelFormatBGR5A1Unorm, MTLPixelFormatInvalid, 2, 0, false, false, false, true, true, true, false, NOSWZ, 2 },
    { D3DFMT_A4R4G4B4, MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB, 4, D3DCONV_A4R4G4B4, false, false, false, false, false, true, false, NOSWZ, 2 },
    { D3DFMT_X4R4G4B4, MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB, 4, D3DCONV_X4R4G4B4, false, false, false, false, false, true, false, NOSWZ, 2 },
    { D3DFMT_R8G8B8, MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB, 4, D3DCONV_R8G8B8, false, false, false, false, false, true, false, NOSWZ, 3 },
    { D3DFMT_A8, MTLPixelFormatA8Unorm, MTLPixelFormatInvalid, 1, 0, false, false, false, false, false, true, false, NOSWZ, 1 },
    { D3DFMT_L8, MTLPixelFormatR8Unorm, MTLPixelFormatInvalid, 1, 0, false, false, false, false, false, true, true, SWZ(Red, Red, Red, One), 1 },
    { D3DFMT_A8L8, MTLPixelFormatRG8Unorm, MTLPixelFormatInvalid, 2, 0, false, false, false, false, false, true, true, SWZ(Red, Red, Red, Green), 2 },
    { D3DFMT_L16, MTLPixelFormatR16Unorm, MTLPixelFormatInvalid, 2, 0, false, false, false, false, false, true, true, SWZ(Red, Red, Red, One), 2 },
    { D3DFMT_G16R16, MTLPixelFormatRG16Unorm, MTLPixelFormatInvalid, 4, 0, false, false, false, true, true, true, true, SWZ(Red, Green, One, One), 4 },
    { D3DFMT_A16B16G16R16, MTLPixelFormatRGBA16Unorm, MTLPixelFormatInvalid, 8, 0, false, false, false, true, true, true, false, NOSWZ, 8 },
    { D3DFMT_A2B10G10R10, MTLPixelFormatRGB10A2Unorm, MTLPixelFormatInvalid, 4, 0, false, false, false, true, true, true, false, NOSWZ, 4 },
    { D3DFMT_A2R10G10B10, MTLPixelFormatBGR10A2Unorm, MTLPixelFormatInvalid, 4, 0, false, false, false, true, true, true, false, NOSWZ, 4 },
    { D3DFMT_V8U8, MTLPixelFormatRG8Snorm, MTLPixelFormatInvalid, 2, 0, false, false, false, false, false, true, true, SWZ(Red, Green, One, One), 2 },
    { D3DFMT_Q8W8V8U8, MTLPixelFormatRGBA8Snorm, MTLPixelFormatInvalid, 4, 0, false, false, false, false, false, true, false, NOSWZ, 4 },
    { D3DFMT_V16U16, MTLPixelFormatRG16Snorm, MTLPixelFormatInvalid, 4, 0, false, false, false, false, false, true, true, SWZ(Red, Green, One, One), 4 },
    { D3DFMT_R16F, MTLPixelFormatR16Float, MTLPixelFormatInvalid, 2, 0, false, false, false, true, true, true, true, SWZ(Red, One, One, One), 2 },
    { D3DFMT_G16R16F, MTLPixelFormatRG16Float, MTLPixelFormatInvalid, 4, 0, false, false, false, true, true, true, true, SWZ(Red, Green, One, One), 4 },
    { D3DFMT_A16B16G16R16F, MTLPixelFormatRGBA16Float, MTLPixelFormatInvalid, 8, 0, false, false, false, true, true, true, false, NOSWZ, 8 },
    { D3DFMT_R32F, MTLPixelFormatR32Float, MTLPixelFormatInvalid, 4, 0, false, false, false, true, false, true, true, SWZ(Red, One, One, One), 4 },
    { D3DFMT_G32R32F, MTLPixelFormatRG32Float, MTLPixelFormatInvalid, 8, 0, false, false, false, true, false, true, true, SWZ(Red, Green, One, One), 8 },
    { D3DFMT_A32B32G32R32F, MTLPixelFormatRGBA32Float, MTLPixelFormatInvalid, 16, 0, false, false, false, true, false, true, false, NOSWZ, 16 },
    { D3DFMT_DXT1, MTLPixelFormatBC1_RGBA, MTLPixelFormatBC1_RGBA_sRGB, 8, 0, true, false, false, false, false, true, false, NOSWZ, 8 },
    { D3DFMT_DXT2, MTLPixelFormatBC2_RGBA, MTLPixelFormatBC2_RGBA_sRGB, 16, 0, true, false, false, false, false, true, false, NOSWZ, 16 },
    { D3DFMT_DXT3, MTLPixelFormatBC2_RGBA, MTLPixelFormatBC2_RGBA_sRGB, 16, 0, true, false, false, false, false, true, false, NOSWZ, 16 },
    { D3DFMT_DXT4, MTLPixelFormatBC3_RGBA, MTLPixelFormatBC3_RGBA_sRGB, 16, 0, true, false, false, false, false, true, false, NOSWZ, 16 },
    { D3DFMT_DXT5, MTLPixelFormatBC3_RGBA, MTLPixelFormatBC3_RGBA_sRGB, 16, 0, true, false, false, false, false, true, false, NOSWZ, 16 },
    /* Depth: Apple GPUs have no 24-bit depth; D24 formats use 32-bit float. */
    { D3DFMT_D16, MTLPixelFormatDepth16Unorm, MTLPixelFormatInvalid, 2, 0, false, true, false, false, false, true, false, NOSWZ, 2 },
    { D3DFMT_D16_LOCKABLE, MTLPixelFormatDepth16Unorm, MTLPixelFormatInvalid, 2, 0, false, true, false, false, false, true, false, NOSWZ, 2 },
    { D3DFMT_D24S8, MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatInvalid, 8, 0, false, true, true, false, false, true, false, NOSWZ, 4 },
    { D3DFMT_D24FS8, MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatInvalid, 8, 0, false, true, true, false, false, true, false, NOSWZ, 4 },
    { D3DFMT_D24X8, MTLPixelFormatDepth32Float, MTLPixelFormatInvalid, 4, 0, false, true, false, false, false, true, false, NOSWZ, 4 },
    { D3DFMT_D32, MTLPixelFormatDepth32Float, MTLPixelFormatInvalid, 4, 0, false, true, false, false, false, true, false, NOSWZ, 4 },
};

const d3d_format *d3d_format_find(uint32_t fmt)
{
    size_t i;
    for (i = 0; i < sizeof formats / sizeof formats[0]; i++)
        if (formats[i].d3d == fmt) return &formats[i];
    return NULL;
}

/* Guest (lock) layout of one level. */
static void level_layout(const d3d_format *f, uint32_t w, uint32_t h, uint32_t *pitch, uint32_t *rows)
{
    if (f->block)
    {
        *pitch = ((w + 3u) / 4u) * f->guest_bytes;
        *rows = (h + 3u) / 4u;
    }
    else
    {
        *pitch = (w * f->guest_bytes + 3u) & ~3u;
        *rows = h;
    }
}

/* Bytes per row of the Metal data of one level. */
static uint32_t metal_pitch(const d3d_format *f, uint32_t w)
{
    return f->block ? ((w + 3u) / 4u) * f->bytes : w * f->bytes;
}

/* Convert guest rows to the Metal layout. dst has metal_pitch * rows bytes. */
static void convert_rows(const d3d_format *f, const uint8_t *src, uint32_t spitch, uint8_t *dst, uint32_t dpitch,
                         uint32_t w, uint32_t rows)
{
    uint32_t x, y;
    for (y = 0; y < rows; y++)
    {
        const uint8_t *s = src + (size_t)y * spitch;
        uint8_t *d = dst + (size_t)y * dpitch;
        switch (f->conv)
        {
        case D3DCONV_R8G8B8:
            for (x = 0; x < w; x++)
            {
                d[4 * x] = s[3 * x];
                d[4 * x + 1] = s[3 * x + 1];
                d[4 * x + 2] = s[3 * x + 2];
                d[4 * x + 3] = 0xFF;
            }
            break;
        case D3DCONV_A4R4G4B4:
        case D3DCONV_X4R4G4B4:
            for (x = 0; x < w; x++)
            {
                uint16_t v = (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);
                d[4 * x] = (uint8_t)((v & 0xFu) * 17u);
                d[4 * x + 1] = (uint8_t)(((v >> 4) & 0xFu) * 17u);
                d[4 * x + 2] = (uint8_t)(((v >> 8) & 0xFu) * 17u);
                d[4 * x + 3] = f->conv == D3DCONV_X4R4G4B4 ? 0xFF : (uint8_t)(((v >> 12) & 0xFu) * 17u);
            }
            break;
        default: memcpy(d, s, dpitch); break;
        }
    }
}

D3DRetired *d3d_retired_new(id<MTLBuffer> b, id<MTLTexture> t, uint64_t serial)
{
    D3DRetired *r = [D3DRetired new];
    r->buf = b;
    r->tex = t;
    r->serial = serial;
    return r;
}

@implementation D3DRetired
@end
@implementation D3DTexture
@end
@implementation D3DSurface
@end
@implementation D3DBuffer
@end
@implementation D3DDecl
@end
@implementation D3DShader
@end
@implementation D3DQuery
@end

/* ---------------------------------------------------------------- classes */

static void texture_destroy(rt_com_object *o);
static void surface_destroy(rt_com_object *o);
static void buffer_destroy(rt_com_object *o);
static void simple_destroy(rt_com_object *o);
static void shader_destroy(rt_com_object *o);

static const rt_com_iface tex_ifaces[] = { { &IID_IDirect3DResource9, 0 }, { &IID_IDirect3DBaseTexture9, 0 }, { &IID_IDirect3DTexture9, 0 } };
static const rt_com_iface cube_ifaces[] = { { &IID_IDirect3DResource9, 0 }, { &IID_IDirect3DBaseTexture9, 0 }, { &IID_IDirect3DCubeTexture9, 0 } };
static const rt_com_iface volume_ifaces[] = { { &IID_IDirect3DResource9, 0 }, { &IID_IDirect3DBaseTexture9, 0 }, { &IID_IDirect3DVolumeTexture9, 0 } };
static const rt_com_iface surf_ifaces[] = { { &IID_IDirect3DResource9, 0 }, { &IID_IDirect3DSurface9, 0 } };
static const rt_com_iface vb_ifaces[] = { { &IID_IDirect3DResource9, 0 }, { &IID_IDirect3DVertexBuffer9, 0 } };
static const rt_com_iface ib_ifaces[] = { { &IID_IDirect3DResource9, 0 }, { &IID_IDirect3DIndexBuffer9, 0 } };
static const rt_com_iface decl_ifaces[] = { { &IID_IDirect3DVertexDeclaration9, 0 } };
static const rt_com_iface vs_ifaces[] = { { &IID_IDirect3DVertexShader9, 0 } };
static const rt_com_iface ps_ifaces[] = { { &IID_IDirect3DPixelShader9, 0 } };
static const rt_com_iface query_ifaces[] = { { &IID_IDirect3DQuery9, 0 } };

const rt_com_class d3d_texture_class = { "IDirect3DTexture9", 1, { &rt_vt_D3DTexture9 }, tex_ifaces, 3, texture_destroy };
const rt_com_class d3d_cube_class = { "IDirect3DCubeTexture9", 1, { &rt_vt_D3DCubeTexture9 }, cube_ifaces, 3, texture_destroy };
const rt_com_class d3d_volume_class = { "IDirect3DVolumeTexture9", 1, { &rt_vt_D3DVolumeTexture9 }, volume_ifaces, 3, texture_destroy };
const rt_com_class d3d_surface_class = { "IDirect3DSurface9", 1, { &rt_vt_D3DSurface9 }, surf_ifaces, 2, surface_destroy };
const rt_com_class d3d_vb_class = { "IDirect3DVertexBuffer9", 1, { &rt_vt_D3DVertexBuffer9 }, vb_ifaces, 2, buffer_destroy };
const rt_com_class d3d_ib_class = { "IDirect3DIndexBuffer9", 1, { &rt_vt_D3DIndexBuffer9 }, ib_ifaces, 2, buffer_destroy };
const rt_com_class d3d_decl_class = { "IDirect3DVertexDeclaration9", 1, { &rt_vt_D3DVertexDeclaration9 }, decl_ifaces, 1, simple_destroy };
const rt_com_class d3d_vs_class = { "IDirect3DVertexShader9", 1, { &rt_vt_D3DVertexShader9 }, vs_ifaces, 1, shader_destroy };
const rt_com_class d3d_ps_class = { "IDirect3DPixelShader9", 1, { &rt_vt_D3DPixelShader9 }, ps_ifaces, 1, shader_destroy };
const rt_com_class d3d_query_class = { "IDirect3DQuery9", 1, { &rt_vt_D3DQuery9 }, query_ifaces, 1, simple_destroy };

/* Create the COM object for data obj (one reference). Stops the process
   when no guest memory is left. */
static rt_com_object *make_object(const rt_com_class *cls, id obj)
{
    void *data = (__bridge_retained void *)obj;
    rt_com_object *o = rt_com_create(cls, data);
    if (o == NULL)
    {
        CFBridgingRelease(data);
        rt_fatal("out of guest memory (%s)", cls->name);
    }
    return o;
}

static void simple_destroy(rt_com_object *o)
{
    id obj = (__bridge_transfer id)o->data;
    (void)obj;
}

static void write_iface(rt_com_object *o, uint32_t out)
{
    wr32(out, rt_com_ptr(o, 0));
}

/* GetDevice for every resource: the device with one new reference. */
static void get_device(_cpu *cpu, D3DDevice *d)
{
    uint32_t out = ARG(1);
    if (d->com == NULL) rt_fatal("D3D9: GetDevice on a resource of a released device");
    if (out == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    rt_com_addref(d->com);
    wr32(out, rt_com_ptr(d->com, 0));
    RET(D3D_OK);
}

/* ---------------------------------------------------------------- textures */

bool d3d_is_texture(const rt_com_object *o)
{
    return o->cls == &d3d_texture_class || o->cls == &d3d_cube_class || o->cls == &d3d_volume_class;
}

static rt_com_object *tex_obj(uint32_t p)
{
    rt_com_object *o = rt_com_this(p, 0, NULL);
    if (!d3d_is_texture(o)) rt_fatal("0x%08x is not a runtime texture (%s)", p, o->cls->name);
    return o;
}

static uint32_t full_mip_count(uint32_t w, uint32_t h)
{
    uint32_t n = 1, m = w > h ? w : h;
    while (m > 1)
    {
        m >>= 1;
        n++;
    }
    return n;
}

static MTLTextureDescriptor *texture_desc(D3DDevice *d, const d3d_format *f, bool cube, uint32_t w, uint32_t h,
                                          uint32_t mtl_levels, uint32_t usage, uint32_t samples)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    (void)d;
    td.textureType = cube ? MTLTextureTypeCube : (samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D);
    td.pixelFormat = f->pf;
    td.width = w;
    td.height = h;
    td.mipmapLevelCount = mtl_levels;
    td.sampleCount = samples > 1 ? samples : 1;
    td.usage = MTLTextureUsageShaderRead;
    if (f->swizzle || f->pf_srgb != MTLPixelFormatInvalid) td.usage |= MTLTextureUsagePixelFormatView;
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) td.usage |= MTLTextureUsageRenderTarget;
    td.storageMode = (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) || f->depth || samples > 1
                         ? MTLStorageModePrivate
                         : MTLStorageModeShared;
    if (samples > 1) td.usage = MTLTextureUsageRenderTarget;
    return td;
}

uint32_t d3d_create_texture(D3DDevice *d, bool cube, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage,
                            uint32_t fmt, uint32_t pool, uint32_t out)
{
    const d3d_format *f = d3d_format_find(fmt);
    D3DTexture *t;
    uint32_t full, mtl_levels, faces = cube ? 6u : 1u, l;
    rt_com_object *o;
    if (out == 0 || w == 0 || h == 0 || w > 16384u || h > 16384u || (cube && w != h)) return D3DERR_INVALIDCALL;
    if (f == NULL)
    {
        rt_log("D3D9: texture format 0x%08x is not supported", fmt);
        return D3DERR_INVALIDCALL;
    }
    if (pool > D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    if ((usage & D3DUSAGE_RENDERTARGET) && (!f->renderable || pool != D3DPOOL_DEFAULT)) return D3DERR_INVALIDCALL;
    if ((usage & D3DUSAGE_DEPTHSTENCIL) && (!f->depth || pool != D3DPOOL_DEFAULT)) return D3DERR_INVALIDCALL;
    if (f->depth && !(usage & D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
    if (f->block && (![d->mtl supportsBCTextureCompression])) return D3DERR_INVALIDCALL;
    full = full_mip_count(w, h);
    if (levels == 0 || levels > full) levels = full;
    mtl_levels = levels;
    if (usage & D3DUSAGE_AUTOGENMIPMAP)
    {
        if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
        levels = 1;
        mtl_levels = full;
    }
    t = [D3DTexture new];
    t->dev = d;
    t->cube = cube;
    t->fmt = fmt;
    t->f = f;
    t->usage = usage;
    t->pool = pool;
    t->width = w;
    t->height = h;
    t->levels = levels;
    t->retired = [NSMutableArray new];
    t->autogen_filter = 2; /* D3DTEXF_LINEAR */
    t->depth = 1;
    for (l = 0; l < levels; l++)
    {
        t->lw[l] = w >> l ? w >> l : 1u;
        t->lh[l] = h >> l ? h >> l : 1u;
        t->ld[l] = 1;
        level_layout(f, t->lw[l], t->lh[l], &t->pitch[l], &t->rows[l]);
        t->slice_pitch[l] = t->pitch[l] * t->rows[l];
        t->lsize[l] = t->slice_pitch[l];
    }
    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH)
    {
        uint32_t fc;
        for (fc = 0; fc < faces; fc++)
            for (l = 0; l < levels; l++)
            {
                t->mem[fc][l] = rt_heap_calloc(1, t->lsize[l]);
                if (t->mem[fc][l] == 0) rt_fatal("out of guest memory (system-memory texture)");
            }
    }
    else
    {
        t->tex = [d->mtl newTextureWithDescriptor:texture_desc(d, f, cube, w, h, mtl_levels, usage, 1)];
        if (t->tex == nil) return D3DERR_OUTOFVIDEOMEMORY;
    }
    o = make_object(cube ? &d3d_cube_class : &d3d_texture_class, t);
    t->com = o;
    write_iface(o, out);
    return D3D_OK;
}

uint32_t d3d_create_volume_texture(D3DDevice *d, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels,
                                   uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t out)
{
    const d3d_format *f = d3d_format_find(fmt);
    D3DTexture *t;
    MTLTextureDescriptor *td;
    uint32_t full = 1, m, l;
    uint64_t total = 0;
    rt_com_object *o;
    if (out == 0 || w == 0 || h == 0 || depth == 0 || w > 2048u || h > 2048u || depth > 2048u) return D3DERR_INVALIDCALL;
    if (f == NULL)
    {
        rt_log("D3D9: volume texture format 0x%08x is not supported", fmt);
        return D3DERR_INVALIDCALL;
    }
    if (pool > D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    /* A volume texture cannot be a render target, a depth buffer or a
       texture with automatic mipmaps. */
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP)) return D3DERR_INVALIDCALL;
    if (f->depth) return D3DERR_INVALIDCALL;
    if (f->block && (![d->mtl supportsBCTextureCompression])) return D3DERR_INVALIDCALL;
    m = w > h ? w : h;
    if (depth > m) m = depth;
    while (m > 1)
    {
        m >>= 1;
        full++;
    }
    if (levels == 0 || levels > full) levels = full;
    t = [D3DTexture new];
    t->dev = d;
    t->volume = true;
    t->fmt = fmt;
    t->f = f;
    t->usage = usage;
    t->pool = pool;
    t->width = w;
    t->height = h;
    t->depth = depth;
    t->levels = levels;
    t->retired = [NSMutableArray new];
    t->autogen_filter = 2; /* D3DTEXF_LINEAR */
    for (l = 0; l < levels; l++)
    {
        t->lw[l] = w >> l ? w >> l : 1u;
        t->lh[l] = h >> l ? h >> l : 1u;
        t->ld[l] = depth >> l ? depth >> l : 1u;
        level_layout(f, t->lw[l], t->lh[l], &t->pitch[l], &t->rows[l]);
        t->slice_pitch[l] = t->pitch[l] * t->rows[l];
        total = (uint64_t)t->slice_pitch[l] * t->ld[l];
        if (total > (256u << 20)) return D3DERR_INVALIDCALL;
        t->lsize[l] = (uint32_t)total;
    }
    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH)
    {
        for (l = 0; l < levels; l++)
        {
            t->mem[0][l] = rt_heap_calloc(1, t->lsize[l]);
            if (t->mem[0][l] == 0) rt_fatal("out of guest memory (system-memory volume texture)");
        }
    }
    else
    {
        td = [MTLTextureDescriptor new];
        td.textureType = MTLTextureType3D;
        td.pixelFormat = f->pf;
        td.width = w;
        td.height = h;
        td.depth = depth;
        td.mipmapLevelCount = levels;
        td.usage = MTLTextureUsageShaderRead;
        if (f->swizzle || f->pf_srgb != MTLPixelFormatInvalid) td.usage |= MTLTextureUsagePixelFormatView;
        td.storageMode = MTLStorageModeShared;
        t->tex = [d->mtl newTextureWithDescriptor:td];
        if (t->tex == nil)
            rt_fatal("D3D9: Metal cannot make a %ux%ux%u volume texture with %u levels (format 0x%08x)", w, h, depth,
                     levels, fmt);
    }
    o = make_object(&d3d_volume_class, t);
    t->com = o;
    write_iface(o, out);
    return D3D_OK;
}

static void texture_destroy(rt_com_object *o)
{
    D3DTexture *t = (__bridge_transfer D3DTexture *)o->data;
    uint32_t fc, l;
    for (fc = 0; fc < 6; fc++)
        for (l = 0; l < 16; l++)
        {
            if (t->surf[fc][l] != NULL)
            {
                D3DSurface *s = d3d_surf(t->surf[fc][l]);
                s->container = NULL;
                rt_com_release(t->surf[fc][l]);
            }
            if (t->mem[fc][l] != 0) rt_heap_free(t->mem[fc][l]);
        }
}

id<MTLTexture> d3d_texture_view(D3DTexture *t, bool srgb)
{
    MTLPixelFormat pf = srgb && t->f->pf_srgb != MTLPixelFormatInvalid ? t->f->pf_srgb : t->f->pf;
    __strong id<MTLTexture> *slot = srgb ? &t->view_srgb : &t->view;
    if (*slot != nil) return *slot;
    if (!t->f->swizzle && pf == t->f->pf)
        *slot = t->tex;
    else
    {
        MTLTextureType tt = t->tex.textureType;
        NSRange lv = NSMakeRange(0, t->tex.mipmapLevelCount), sl = NSMakeRange(0, t->cube ? 6 : 1);
        *slot = [t->tex newTextureViewWithPixelFormat:pf textureType:tt levels:lv slices:sl swizzle:t->f->swz];
        if (*slot == nil) rt_fatal("D3D9: texture view creation failed (format 0x%x)", t->fmt);
    }
    return *slot;
}

/* Generate the mipmaps of an AUTOGENMIPMAP texture (in command order). */
static void generate_mips(D3DDevice *d, D3DTexture *t)
{
    id<MTLBlitCommandEncoder> b;
    if (t->tex == nil || t->tex.mipmapLevelCount < 2) return;
    d3d_end_pass(d);
    @autoreleasepool
    {
        b = [d->cb blitCommandEncoder];
        [b generateMipmapsForTexture:t->tex];
        [b endEncoding];
    }
    t->last_use = d->serial;
}

/* Upload level l of face fc from the guest copy. */
static void upload_level(D3DDevice *d, D3DTexture *t, uint32_t fc, uint32_t l, bool discard)
{
    const d3d_format *f = t->f;
    uint32_t w = t->lw[l], rows = t->rows[l], mp = metal_pitch(f, w);
    const uint8_t *src = G2H(t->mem[fc][l]);
    uint8_t *conv = NULL;
    /* A volume level is ld[l] slices of rows lines; the slices follow each
       other in the guest copy (slice_pitch = pitch * rows), so the
       conversion treats them as rows * ld[l] lines. */
    MTLRegion r = MTLRegionMake3D(0, 0, 0, w, t->lh[l], t->ld[l]);
    if (t->tex.storageMode == MTLStorageModePrivate) rt_fatal("D3D9: upload to a render target texture");
    if (t->last_use != 0 && !d3d_serial_done(d, t->last_use))
    {
        if (discard && t->levels == 1 && !t->cube && !t->volume && t->tex.mipmapLevelCount == 1)
        {
            /* Rename: the GPU keeps the old texture. */
            id<MTLTexture> nt = nil;
            NSUInteger i;
            for (i = 0; i < t->retired.count; i++)
            {
                if (d3d_serial_done(d, t->retired[i]->serial))
                {
                    nt = t->retired[i]->tex;
                    [t->retired removeObjectAtIndex:i];
                    break;
                }
            }
            if (nt == nil && t->retired.count >= MAX_RETIRED)
            {
                d3d_wait_serial(d, t->retired[0]->serial);
                nt = t->retired[0]->tex;
                [t->retired removeObjectAtIndex:0];
            }
            if (nt == nil)
            {
                MTLTextureDescriptor *td = texture_desc(d, f, false, t->width, t->height, 1, t->usage, 1);
                nt = [d->mtl newTextureWithDescriptor:td];
                if (nt == nil) rt_fatal("D3D9: out of video memory (dynamic texture)");
            }
            [t->retired addObject:d3d_retired_new(nil, t->tex, t->last_use)];
            t->tex = nt;
            t->view = nil;
            t->view_srgb = nil;
            t->rt_srgb = nil;
        }
        else
            d3d_wait_serial(d, t->last_use);
    }
    if (f->conv != D3DCONV_NONE)
    {
        conv = malloc((size_t)mp * rows * t->ld[l]);
        if (conv == NULL) rt_fatal("out of host memory (texture conversion)");
        convert_rows(f, src, t->pitch[l], conv, mp, w, rows * t->ld[l]);
        src = conv;
    }
    else
        mp = t->pitch[l];
    [t->tex replaceRegion:r mipmapLevel:l slice:fc withBytes:src bytesPerRow:mp bytesPerImage:(NSUInteger)mp * rows];
    free(conv);
}

static uint32_t tex_lock(D3DTexture *t, uint32_t fc, uint32_t l, uint32_t locked_rect, uint32_t rect, uint32_t flags)
{
    uint32_t off = 0;
    if (l >= t->levels || locked_rect == 0) return D3DERR_INVALIDCALL;
    if (t->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
    if (t->pool == D3DPOOL_DEFAULT && !(t->usage & D3DUSAGE_DYNAMIC)) return D3DERR_INVALIDCALL;
    if (t->locked[fc] & (1u << l)) return D3DERR_INVALIDCALL;
    if (t->mem[fc][l] == 0)
    {
        t->mem[fc][l] = rt_heap_calloc(1, t->lsize[l]);
        if (t->mem[fc][l] == 0) return D3DERR_OUTOFVIDEOMEMORY;
    }
    if (rect != 0)
    {
        int32_t left = (int32_t)rd32(rect), top = (int32_t)rd32(rect + 4), right = (int32_t)rd32(rect + 8),
                bottom = (int32_t)rd32(rect + 12);
        if (left < 0 || top < 0 || right <= left || bottom <= top || (uint32_t)right > t->lw[l] ||
            (uint32_t)bottom > t->lh[l])
            return D3DERR_INVALIDCALL;
        if (t->f->block)
            off = ((uint32_t)top / 4u) * t->pitch[l] + ((uint32_t)left / 4u) * t->f->guest_bytes;
        else
            off = (uint32_t)top * t->pitch[l] + (uint32_t)left * t->f->guest_bytes;
    }
    wr32(locked_rect, t->pitch[l]);
    wr32(locked_rect + 4, t->mem[fc][l] + off);
    t->locked[fc] = (uint16_t)(t->locked[fc] | (1u << l));
    t->lock_flags[fc][l] = flags;
    return D3D_OK;
}

static uint32_t tex_unlock(D3DTexture *t, uint32_t fc, uint32_t l)
{
    D3DDevice *d = t->dev;
    if (l >= t->levels || !(t->locked[fc] & (1u << l))) return D3DERR_INVALIDCALL;
    t->locked[fc] = (uint16_t)(t->locked[fc] & ~(1u << l));
    if (t->tex != nil && !(t->lock_flags[fc][l] & D3DLOCK_READONLY))
    {
        upload_level(d, t, fc, l, (t->lock_flags[fc][l] & D3DLOCK_DISCARD) != 0);
        if ((t->usage & D3DUSAGE_AUTOGENMIPMAP) && l == 0) generate_mips(d, t);
    }
    return D3D_OK;
}

void rt_com_D3DTexture9_GetDevice(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(tex_obj(ARG(0)));
    get_device(cpu, t->dev);
}

void rt_com_D3DTexture9_SetPriority(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(tex_obj(ARG(0)));
    uint32_t old = t->priority;
    t->priority = ARG(1);
    RET(old);
}

void rt_com_D3DTexture9_GetPriority(_cpu *cpu)
{
    RET(d3d_tex(tex_obj(ARG(0)))->priority);
}

/* PreLoad is a residency hint; Metal textures are always resident. */
void rt_com_D3DTexture9_PreLoad(_cpu *cpu)
{
    (void)tex_obj(ARG(0));
}

void rt_com_D3DTexture9_GetType(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(tex_obj(ARG(0)));
    RET(t->volume ? D3DRTYPE_VOLUMETEXTURE : t->cube ? D3DRTYPE_CUBETEXTURE : D3DRTYPE_TEXTURE);
}

void rt_com_D3DTexture9_SetLOD(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(tex_obj(ARG(0)));
    uint32_t old = t->lod;
    if (t->pool == D3DPOOL_MANAGED) t->lod = ARG(1) < t->levels ? ARG(1) : t->levels - 1u;
    RET(t->pool == D3DPOOL_MANAGED ? old : 0u);
}

void rt_com_D3DTexture9_GetLOD(_cpu *cpu)
{
    RET(d3d_tex(tex_obj(ARG(0)))->lod);
}

void rt_com_D3DTexture9_GetLevelCount(_cpu *cpu)
{
    RET(d3d_tex(tex_obj(ARG(0)))->levels);
}

void rt_com_D3DTexture9_SetAutoGenFilterType(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(tex_obj(ARG(0)));
    uint32_t ft = ARG(1);
    if (!(t->usage & D3DUSAGE_AUTOGENMIPMAP) || ft < 1 || ft > 2)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    t->autogen_filter = ft;
    RET(D3D_OK);
}

void rt_com_D3DTexture9_GetAutoGenFilterType(_cpu *cpu)
{
    RET(d3d_tex(tex_obj(ARG(0)))->autogen_filter);
}

void rt_com_D3DTexture9_GenerateMipSubLevels(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(tex_obj(ARG(0)));
    d3d_lock(t->dev);
    if (t->usage & D3DUSAGE_AUTOGENMIPMAP) generate_mips(t->dev, t);
    d3d_unlock(t->dev);
}

static void write_surface_desc(uint32_t p, uint32_t fmt, uint32_t type, uint32_t usage, uint32_t pool, uint32_t msaa,
                               uint32_t quality, uint32_t w, uint32_t h)
{
    wr32(p, fmt);
    wr32(p + 4, type);
    wr32(p + 8, usage);
    wr32(p + 12, pool);
    wr32(p + 16, msaa);
    wr32(p + 20, quality);
    wr32(p + 24, w);
    wr32(p + 28, h);
}

void rt_com_D3DTexture9_GetLevelDesc(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(tex_obj(ARG(0)));
    uint32_t l = ARG(1), p = ARG(2);
    if (l >= t->levels || p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    write_surface_desc(p, t->fmt, D3DRTYPE_SURFACE, t->usage, t->pool, 0, 0, t->lw[l], t->lh[l]);
    RET(D3D_OK);
}

/* The child surface of face fc, level l (created on first use). */
static uint32_t child_surface(D3DTexture *t, uint32_t fc, uint32_t l, uint32_t out)
{
    rt_com_object *so;
    if (l >= t->levels || out == 0) return D3DERR_INVALIDCALL;
    so = t->surf[fc][l];
    if (so == NULL)
    {
        D3DSurface *s = [D3DSurface new];
        s->dev = t->dev;
        s->container = t->com;
        s->ctex = t;
        s->face = fc;
        s->level = l;
        s->fmt = t->fmt;
        s->f = t->f;
        s->usage = t->usage;
        s->pool = t->pool;
        s->width = t->lw[l];
        s->height = t->lh[l];
        so = make_object(&d3d_surface_class, s);
        s->com = so;
        t->surf[fc][l] = so;
    }
    rt_com_addref(t->com);
    write_iface(so, out);
    return D3D_OK;
}

void rt_com_D3DTexture9_GetSurfaceLevel(_cpu *cpu)
{
    rt_com_object *o = rt_com_this(ARG(0), 0, &d3d_texture_class);
    D3DTexture *t = d3d_tex(o);
    d3d_lock(t->dev);
    RET(child_surface(t, 0, ARG(1), ARG(2)));
    d3d_unlock(t->dev);
}

void rt_com_D3DTexture9_LockRect(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(rt_com_this(ARG(0), 0, &d3d_texture_class));
    d3d_lock(t->dev);
    RET(tex_lock(t, 0, ARG(1), ARG(2), ARG(3), ARG(4)));
    d3d_unlock(t->dev);
}

void rt_com_D3DTexture9_UnlockRect(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(rt_com_this(ARG(0), 0, &d3d_texture_class));
    d3d_lock(t->dev);
    RET(tex_unlock(t, 0, ARG(1)));
    d3d_unlock(t->dev);
}

/* Every unlock uploads its level, so there is no dirty region to keep. */
void rt_com_D3DTexture9_AddDirtyRect(_cpu *cpu)
{
    (void)rt_com_this(ARG(0), 0, &d3d_texture_class);
    RET(D3D_OK);
}

void rt_com_D3DCubeTexture9_GetCubeMapSurface(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(rt_com_this(ARG(0), 0, &d3d_cube_class));
    uint32_t fc = ARG(1);
    d3d_lock(t->dev);
    RET(fc < 6 ? child_surface(t, fc, ARG(2), ARG(3)) : D3DERR_INVALIDCALL);
    d3d_unlock(t->dev);
}

void rt_com_D3DCubeTexture9_LockRect(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(rt_com_this(ARG(0), 0, &d3d_cube_class));
    uint32_t fc = ARG(1);
    d3d_lock(t->dev);
    RET(fc < 6 ? tex_lock(t, fc, ARG(2), ARG(3), ARG(4), ARG(5)) : D3DERR_INVALIDCALL);
    d3d_unlock(t->dev);
}

void rt_com_D3DCubeTexture9_UnlockRect(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(rt_com_this(ARG(0), 0, &d3d_cube_class));
    uint32_t fc = ARG(1);
    d3d_lock(t->dev);
    RET(fc < 6 ? tex_unlock(t, fc, ARG(2)) : D3DERR_INVALIDCALL);
    d3d_unlock(t->dev);
}

void rt_com_D3DCubeTexture9_AddDirtyRect(_cpu *cpu)
{
    (void)rt_com_this(ARG(0), 0, &d3d_cube_class);
    RET(ARG(1) < 6 ? D3D_OK : D3DERR_INVALIDCALL);
}

/* GetLevelDesc(UINT level, D3DVOLUME_DESC *desc): Format, Type, Usage,
   Pool, Width, Height, Depth. */
void rt_com_D3DVolumeTexture9_GetLevelDesc(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(rt_com_this(ARG(0), 0, &d3d_volume_class));
    uint32_t l = ARG(1), p = ARG(2);
    if (l >= t->levels || p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    wr32(p, t->fmt);
    wr32(p + 4, D3DRTYPE_VOLUME);
    wr32(p + 8, t->usage);
    wr32(p + 12, t->pool);
    wr32(p + 16, t->lw[l]);
    wr32(p + 20, t->lh[l]);
    wr32(p + 24, t->ld[l]);
    RET(D3D_OK);
}

/* LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags).
   D3DLOCKED_BOX: RowPitch, SlicePitch, pBits. D3DBOX: Left, Top, Right,
   Bottom, Front, Back. */
static uint32_t volume_lock(D3DTexture *t, uint32_t l, uint32_t locked_box, uint32_t box, uint32_t flags)
{
    uint32_t off = 0;
    if (l >= t->levels || locked_box == 0) return D3DERR_INVALIDCALL;
    if (t->pool == D3DPOOL_DEFAULT && !(t->usage & D3DUSAGE_DYNAMIC)) return D3DERR_INVALIDCALL;
    if (t->locked[0] & (1u << l)) return D3DERR_INVALIDCALL;
    if (t->mem[0][l] == 0)
    {
        t->mem[0][l] = rt_heap_calloc(1, t->lsize[l]);
        if (t->mem[0][l] == 0) return D3DERR_OUTOFVIDEOMEMORY;
    }
    if (box != 0)
    {
        uint32_t left = rd32(box), top = rd32(box + 4), right = rd32(box + 8), bottom = rd32(box + 12),
                 front = rd32(box + 16), back = rd32(box + 20);
        if (right <= left || bottom <= top || back <= front || right > t->lw[l] || bottom > t->lh[l] || back > t->ld[l])
            return D3DERR_INVALIDCALL;
        off = front * t->slice_pitch[l];
        if (t->f->block)
            off += (top / 4u) * t->pitch[l] + (left / 4u) * t->f->guest_bytes;
        else
            off += top * t->pitch[l] + left * t->f->guest_bytes;
    }
    wr32(locked_box, t->pitch[l]);
    wr32(locked_box + 4, t->slice_pitch[l]);
    wr32(locked_box + 8, t->mem[0][l] + off);
    t->locked[0] = (uint16_t)(t->locked[0] | (1u << l));
    t->lock_flags[0][l] = flags;
    return D3D_OK;
}

void rt_com_D3DVolumeTexture9_LockBox(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(rt_com_this(ARG(0), 0, &d3d_volume_class));
    d3d_lock(t->dev);
    RET(volume_lock(t, ARG(1), ARG(2), ARG(3), ARG(4)));
    d3d_unlock(t->dev);
}

void rt_com_D3DVolumeTexture9_UnlockBox(_cpu *cpu)
{
    D3DTexture *t = d3d_tex(rt_com_this(ARG(0), 0, &d3d_volume_class));
    d3d_lock(t->dev);
    RET(tex_unlock(t, 0, ARG(1)));
    d3d_unlock(t->dev);
}

/* Every unlock uploads its level, so there is no dirty region to keep. */
void rt_com_D3DVolumeTexture9_AddDirtyBox(_cpu *cpu)
{
    (void)rt_com_this(ARG(0), 0, &d3d_volume_class);
    RET(D3D_OK);
}

/* ---------------------------------------------------------------- surfaces */

static uint32_t msaa_samples(uint32_t type)
{
    if (type == 1) return 4; /* D3DMULTISAMPLE_NONMASKABLE */
    return type;
}

uint32_t d3d_create_surface(D3DDevice *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool,
                            uint32_t msaa, uint32_t quality, bool lockable, rt_com_object **out_obj)
{
    const d3d_format *f = d3d_format_find(fmt);
    D3DSurface *s;
    uint32_t samples = msaa_samples(msaa);
    if (w == 0 || h == 0 || w > 16384u || h > 16384u) return D3DERR_INVALIDCALL;
    if (f == NULL)
    {
        rt_log("D3D9: surface format 0x%08x is not supported", fmt);
        return D3DERR_INVALIDCALL;
    }
    if ((usage & D3DUSAGE_RENDERTARGET) && !f->renderable) return D3DERR_INVALIDCALL;
    if ((usage & D3DUSAGE_DEPTHSTENCIL) && !f->depth) return D3DERR_INVALIDCALL;
    if (samples > 1 && ![d->mtl supportsTextureSampleCount:samples]) return D3DERR_INVALIDCALL;
    if (f->block && (![d->mtl supportsBCTextureCompression])) return D3DERR_INVALIDCALL;
    s = [D3DSurface new];
    s->dev = d;
    s->fmt = fmt;
    s->f = f;
    s->usage = usage;
    s->pool = pool;
    s->width = w;
    s->height = h;
    s->msaa = msaa;
    s->msaa_quality = quality;
    s->lockable = lockable;
    level_layout(f, w, h, &s->pitch, &(uint32_t){ 0 });
    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH)
    {
        uint32_t pitch, rows;
        level_layout(f, w, h, &pitch, &rows);
        s->mem = rt_heap_calloc(1, pitch * rows);
        if (s->mem == 0) rt_fatal("out of guest memory (system-memory surface)");
    }
    else
    {
        s->tex = [d->mtl newTextureWithDescriptor:texture_desc(d, f, false, w, h, 1, usage, samples)];
        if (s->tex == nil) return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out_obj = make_object(&d3d_surface_class, s);
    s->com = *out_obj;
    return D3D_OK;
}

static void surface_destroy(rt_com_object *o)
{
    D3DSurface *s = (__bridge_transfer D3DSurface *)o->data;
    if (s->mem != 0) rt_heap_free(s->mem);
}

rt_com_object *d3d_surface_from_ptr(uint32_t p)
{
    return rt_com_this(p, 0, &d3d_surface_class);
}

id<MTLTexture> d3d_surface_texture(D3DSurface *s, NSUInteger *slice, NSUInteger *level)
{
    if (s->ctex != nil)
    {
        *slice = s->face;
        *level = s->level;
        return s->ctex->tex;
    }
    *slice = 0;
    *level = 0;
    return s->tex;
}

void d3d_use_surface(D3DSurface *s)
{
    if (s->ctex != nil)
        s->ctex->last_use = s->dev->serial;
    else
        s->last_use = s->dev->serial;
}

static uint64_t surface_last_use(D3DSurface *s)
{
    return s->ctex != nil ? s->ctex->last_use : s->last_use;
}

/* Copy texture pixels into guest memory (rows of pitch bytes). */
static void read_pixels(D3DDevice *d, id<MTLTexture> tex, NSUInteger slice, NSUInteger level, const d3d_format *f,
                        uint32_t w, uint32_t h, uint32_t dst, uint32_t pitch, uint64_t last_use)
{
    uint32_t mp = metal_pitch(f, w), rows = f->block ? (h + 3u) / 4u : h, y;
    if (f->conv != D3DCONV_NONE || f->depth) rt_fatal("D3D9: reading back format 0x%x is not supported", f->d3d);
    if (tex.storageMode == MTLStorageModePrivate)
    {
        id<MTLBuffer> b = [d->mtl newBufferWithLength:(NSUInteger)mp * rows options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> be;
        if (b == nil) rt_fatal("D3D9: out of memory (read back)");
        d3d_end_pass(d);
        @autoreleasepool
        {
            be = [d->cb blitCommandEncoder];
            [be copyFromTexture:tex sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:mp
        destinationBytesPerImage:(NSUInteger)mp * rows];
            [be endEncoding];
        }
        d3d_wait_serial(d, d->serial);
        for (y = 0; y < rows; y++)
            memcpy(G2H(dst + y * pitch), (const uint8_t *)b.contents + (size_t)y * mp, mp);
    }
    else
    {
        if (last_use != 0) d3d_wait_serial(d, last_use);
        [tex getBytes:G2H(dst) bytesPerRow:pitch bytesPerImage:(NSUInteger)pitch * rows
           fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level slice:slice];
    }
}

/* Write guest rows into a texture region. */
static void write_pixels(D3DDevice *d, id<MTLTexture> tex, NSUInteger slice, NSUInteger level, const d3d_format *f,
                         uint32_t x, uint32_t y0, uint32_t w, uint32_t h, const uint8_t *src, uint32_t pitch,
                         uint64_t last_use)
{
    uint32_t mp = metal_pitch(f, w), rows = f->block ? (h + 3u) / 4u : h;
    uint8_t *conv = NULL;
    if (f->depth) rt_fatal("D3D9: writing depth data from the CPU is not supported");
    if (f->conv != D3DCONV_NONE)
    {
        conv = malloc((size_t)mp * rows);
        if (conv == NULL) rt_fatal("out of host memory (texture conversion)");
        convert_rows(f, src, pitch, conv, mp, w, rows);
        src = conv;
        pitch = mp;
    }
    if (tex.storageMode == MTLStorageModePrivate)
    {
        uint32_t off, r;
        id<MTLBuffer> b;
        id<MTLBlitCommandEncoder> be;
        uint8_t *tmp = malloc((size_t)mp * rows);
        if (tmp == NULL) rt_fatal("out of host memory (upload)");
        for (r = 0; r < rows; r++)
            memcpy(tmp + (size_t)r * mp, src + (size_t)r * pitch, mp);
        b = d3d_upload(d, tmp, mp * rows, &off);
        free(tmp);
        d3d_end_pass(d);
        @autoreleasepool
        {
            be = [d->cb blitCommandEncoder];
            [be copyFromBuffer:b sourceOffset:off sourceBytesPerRow:mp sourceBytesPerImage:(NSUInteger)mp * rows
                    sourceSize:MTLSizeMake(w, h, 1) toTexture:tex destinationSlice:slice destinationLevel:level
             destinationOrigin:MTLOriginMake(x, y0, 0)];
            [be endEncoding];
        }
    }
    else
    {
        if (last_use != 0) d3d_wait_serial(d, last_use);
        [tex replaceRegion:MTLRegionMake2D(x, y0, w, h) mipmapLevel:level slice:slice withBytes:src
               bytesPerRow:pitch bytesPerImage:(NSUInteger)pitch * rows];
    }
    free(conv);
}

void rt_com_D3DSurface9_QueryInterface(_cpu *cpu)
{
    rt_com_object *o = d3d_surface_from_ptr(ARG(0));
    D3DSurface *s = d3d_surf(o);
    rt_guid iid = rt_guid_read(ARG(1));
    uint32_t ppv = ARG(2);
    if (s->container == NULL)
    {
        RET(rt_com_query(o, &iid, ppv));
        return;
    }
    if (ppv == 0)
    {
        RET(E_POINTER);
        return;
    }
    if (!rt_guid_eq(&iid, &rt_IID_IUnknown) && !rt_guid_eq(&iid, &IID_IDirect3DResource9) &&
        !rt_guid_eq(&iid, &IID_IDirect3DSurface9))
    {
        char text[39];
        rt_guid_format(&iid, text, true, false);
        rt_fatal("QueryInterface of a texture surface for interface %s, which the runtime does not implement", text);
    }
    rt_com_addref(s->container);
    wr32(ppv, rt_com_ptr(o, 0));
    RET(S_OK);
}

/* A surface of a texture shares the reference count of the texture. */
void rt_com_D3DSurface9_AddRef(_cpu *cpu)
{
    rt_com_object *o = d3d_surface_from_ptr(ARG(0));
    D3DSurface *s = d3d_surf(o);
    rt_com_object *t = s->container != NULL ? s->container : o;
    rt_com_addref(t);
    RET(t->refs);
}

void rt_com_D3DSurface9_Release(_cpu *cpu)
{
    rt_com_object *o = d3d_surface_from_ptr(ARG(0));
    D3DSurface *s = d3d_surf(o);
    RET(rt_com_release(s->container != NULL ? s->container : o));
}

void rt_com_D3DSurface9_GetDevice(_cpu *cpu)
{
    get_device(cpu, d3d_surf(d3d_surface_from_ptr(ARG(0)))->dev);
}

void rt_com_D3DSurface9_SetPriority(_cpu *cpu)
{
    (void)d3d_surface_from_ptr(ARG(0));
    RET(0);
}

void rt_com_D3DSurface9_GetPriority(_cpu *cpu)
{
    (void)d3d_surface_from_ptr(ARG(0));
    RET(0);
}

void rt_com_D3DSurface9_PreLoad(_cpu *cpu)
{
    (void)d3d_surface_from_ptr(ARG(0));
}

void rt_com_D3DSurface9_GetType(_cpu *cpu)
{
    (void)d3d_surface_from_ptr(ARG(0));
    RET(D3DRTYPE_SURFACE);
}

void rt_com_D3DSurface9_GetContainer(_cpu *cpu)
{
    D3DSurface *s = d3d_surf(d3d_surface_from_ptr(ARG(0)));
    rt_guid iid = rt_guid_read(ARG(1));
    uint32_t ppv = ARG(2);
    if (s->container != NULL)
        RET(rt_com_query(s->container, &iid, ppv));
    else
    {
        /* A standalone surface: the container is the device. */
        if (rt_guid_eq(&iid, &rt_IID_IUnknown) || s->dev->com->cls->nifaces == 0 ||
            rt_guid_eq(&iid, s->dev->com->cls->ifaces[0].iid))
            RET(rt_com_query(s->dev->com, &iid, ppv));
        else
            RET(E_NOINTERFACE);
    }
}

void rt_com_D3DSurface9_GetDesc(_cpu *cpu)
{
    D3DSurface *s = d3d_surf(d3d_surface_from_ptr(ARG(0)));
    uint32_t p = ARG(1);
    if (p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    write_surface_desc(p, s->fmt, D3DRTYPE_SURFACE, s->usage, s->pool, s->msaa, s->msaa_quality, s->width, s->height);
    RET(D3D_OK);
}

void rt_com_D3DSurface9_LockRect(_cpu *cpu)
{
    D3DSurface *s = d3d_surf(d3d_surface_from_ptr(ARG(0)));
    D3DDevice *d = s->dev;
    uint32_t lr = ARG(1), rect = ARG(2), flags = ARG(3), off = 0, rows;
    d3d_lock(d);
    if (s->ctex != nil)
    {
        RET(tex_lock(s->ctex, s->face, s->level, lr, rect, flags));
        d3d_unlock(d);
        return;
    }
    if (lr == 0 || s->locked || s->msaa > 0 || s->f->depth ||
        ((s->usage & D3DUSAGE_RENDERTARGET) && !s->lockable))
    {
        RET(D3DERR_INVALIDCALL);
        d3d_unlock(d);
        return;
    }
    level_layout(s->f, s->width, s->height, &s->pitch, &rows);
    if (s->mem == 0)
    {
        s->mem = rt_heap_calloc(1, s->pitch * rows);
        if (s->mem == 0)
        {
            RET(D3DERR_OUTOFVIDEOMEMORY);
            d3d_unlock(d);
            return;
        }
    }
    if (s->tex != nil && !(flags & D3DLOCK_DISCARD))
        read_pixels(d, s->tex, 0, 0, s->f, s->width, s->height, s->mem, s->pitch, s->last_use);
    if (rect != 0)
    {
        uint32_t left = rd32(rect), top = rd32(rect + 4);
        off = s->f->block ? (top / 4u) * s->pitch + (left / 4u) * s->f->guest_bytes
                          : top * s->pitch + left * s->f->guest_bytes;
    }
    wr32(lr, s->pitch);
    wr32(lr + 4, s->mem + off);
    s->locked = true;
    s->lock_flags = flags;
    RET(D3D_OK);
    d3d_unlock(d);
}

void rt_com_D3DSurface9_UnlockRect(_cpu *cpu)
{
    D3DSurface *s = d3d_surf(d3d_surface_from_ptr(ARG(0)));
    D3DDevice *d = s->dev;
    d3d_lock(d);
    if (s->ctex != nil)
    {
        RET(tex_unlock(s->ctex, s->face, s->level));
        d3d_unlock(d);
        return;
    }
    if (!s->locked)
    {
        RET(D3DERR_INVALIDCALL);
        d3d_unlock(d);
        return;
    }
    s->locked = false;
    if (s->tex != nil && !(s->lock_flags & D3DLOCK_READONLY))
        write_pixels(d, s->tex, 0, 0, s->f, 0, 0, s->width, s->height, G2H(s->mem), s->pitch, s->last_use);
    RET(D3D_OK);
    d3d_unlock(d);
}

/* The guest memory and pitch of a system-memory surface or texture level. */
static bool sysmem_of(D3DSurface *s, uint32_t *mem, uint32_t *pitch)
{
    if (s->pool != D3DPOOL_SYSTEMMEM && s->pool != D3DPOOL_SCRATCH) return false;
    if (s->ctex != nil)
    {
        *mem = s->ctex->mem[s->face][s->level];
        *pitch = s->ctex->pitch[s->level];
    }
    else
    {
        uint32_t rows;
        level_layout(s->f, s->width, s->height, pitch, &rows);
        *mem = s->mem;
    }
    return *mem != 0;
}

uint32_t d3d_readback(D3DDevice *d, D3DSurface *src, D3DSurface *dst)
{
    uint32_t mem, pitch;
    NSUInteger slice, level;
    id<MTLTexture> tex = d3d_surface_texture(src, &slice, &level);
    if (tex == nil || src->msaa > 0 || src->width != dst->width || src->height != dst->height || src->fmt != dst->fmt ||
        !sysmem_of(dst, &mem, &pitch))
        return D3DERR_INVALIDCALL;
    read_pixels(d, tex, slice, level, src->f, src->width, src->height, mem, pitch, surface_last_use(src));
    return D3D_OK;
}

uint32_t d3d_update_surface(D3DDevice *d, D3DSurface *src, uint32_t src_rect, D3DSurface *dst, uint32_t dst_point)
{
    uint32_t mem, pitch, x0 = 0, y0 = 0, w = src->width, h = src->height, dx = 0, dy = 0, off;
    NSUInteger slice, level;
    id<MTLTexture> tex = d3d_surface_texture(dst, &slice, &level);
    if (tex == nil || dst->pool != D3DPOOL_DEFAULT || src->fmt != dst->fmt || !sysmem_of(src, &mem, &pitch))
        return D3DERR_INVALIDCALL;
    if (src_rect != 0)
    {
        x0 = rd32(src_rect);
        y0 = rd32(src_rect + 4);
        w = rd32(src_rect + 8) - x0;
        h = rd32(src_rect + 12) - y0;
    }
    if (dst_point != 0)
    {
        dx = rd32(dst_point);
        dy = rd32(dst_point + 4);
    }
    if (x0 + w > src->width || y0 + h > src->height || dx + w > dst->width || dy + h > dst->height || w == 0 || h == 0)
        return D3DERR_INVALIDCALL;
    off = src->f->block ? (y0 / 4u) * pitch + (x0 / 4u) * src->f->guest_bytes : y0 * pitch + x0 * src->f->guest_bytes;
    write_pixels(d, tex, slice, level, dst->f, dx, dy, w, h, G2H(mem + off), pitch, surface_last_use(dst));
    if (dst->ctex != nil && (dst->ctex->usage & D3DUSAGE_AUTOGENMIPMAP) && dst->level == 0) generate_mips(d, dst->ctex);
    return D3D_OK;
}

uint32_t d3d_update_texture(D3DDevice *d, D3DTexture *src, D3DTexture *dst)
{
    uint32_t faces = src->cube ? 6u : 1u, fc, l, n;
    if (src->volume || dst->volume) rt_fatal("D3D9: UpdateTexture with a volume texture is not supported");
    if ((src->pool != D3DPOOL_SYSTEMMEM) || dst->pool != D3DPOOL_DEFAULT || src->cube != dst->cube ||
        src->fmt != dst->fmt || dst->tex == nil)
        return D3DERR_INVALIDCALL;
    /* Levels are matched from the top level whose size is the top level of
       the destination. */
    if (src->width != dst->width || src->height != dst->height) return D3DERR_INVALIDCALL;
    n = (dst->usage & D3DUSAGE_AUTOGENMIPMAP) ? 1u : (dst->levels < src->levels ? dst->levels : src->levels);
    for (fc = 0; fc < faces; fc++)
        for (l = 0; l < n; l++)
            write_pixels(d, dst->tex, fc, l, dst->f, 0, 0, dst->lw[l], dst->lh[l], G2H(src->mem[fc][l]), src->pitch[l],
                         dst->last_use);
    if (dst->usage & D3DUSAGE_AUTOGENMIPMAP) generate_mips(d, dst);
    return D3D_OK;
}

/* ---------------------------------------------------------------- buffers */

static rt_com_object *buf_obj(uint32_t p)
{
    rt_com_object *o = rt_com_this(p, 0, NULL);
    if (o->cls != &d3d_vb_class && o->cls != &d3d_ib_class)
        rt_fatal("0x%08x is not a runtime vertex or index buffer (%s)", p, o->cls->name);
    return o;
}

uint32_t d3d_create_buffer(D3DDevice *d, bool index, uint32_t size, uint32_t usage, uint32_t format_or_fvf,
                           uint32_t pool, uint32_t out)
{
    D3DBuffer *b;
    rt_com_object *o;
    if (out == 0 || size == 0 || size > (512u << 20) || pool > D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    if (index && format_or_fvf != D3DFMT_INDEX16 && format_or_fvf != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;
    b = [D3DBuffer new];
    b->dev = d;
    b->index = index;
    b->size = size;
    b->usage = usage;
    b->pool = pool;
    b->format_or_fvf = format_or_fvf;
    b->retired = [NSMutableArray new];
    b->buf = [d->mtl newBufferWithLength:size options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
    if (b->buf == nil) return D3DERR_OUTOFVIDEOMEMORY;
    o = make_object(index ? &d3d_ib_class : &d3d_vb_class, b);
    b->com = o;
    write_iface(o, out);
    return D3D_OK;
}

static void buffer_destroy(rt_com_object *o)
{
    D3DBuffer *b = (__bridge_transfer D3DBuffer *)o->data;
    if (b->staging != 0) rt_heap_free(b->staging);
}

void rt_com_D3DVertexBuffer9_GetDevice(_cpu *cpu)
{
    get_device(cpu, d3d_buf(buf_obj(ARG(0)))->dev);
}

void rt_com_D3DVertexBuffer9_SetPriority(_cpu *cpu)
{
    (void)buf_obj(ARG(0));
    RET(0);
}

void rt_com_D3DVertexBuffer9_GetPriority(_cpu *cpu)
{
    (void)buf_obj(ARG(0));
    RET(0);
}

void rt_com_D3DVertexBuffer9_PreLoad(_cpu *cpu)
{
    (void)buf_obj(ARG(0));
}

void rt_com_D3DVertexBuffer9_GetType(_cpu *cpu)
{
    RET(d3d_buf(buf_obj(ARG(0)))->index ? D3DRTYPE_INDEXBUFFER : D3DRTYPE_VERTEXBUFFER);
}

/* HRESULT Lock(UINT offset, UINT size, void **data, DWORD flags) */
void rt_com_D3DVertexBuffer9_Lock(_cpu *cpu)
{
    D3DBuffer *b = d3d_buf(buf_obj(ARG(0)));
    D3DDevice *d = b->dev;
    uint32_t off = ARG(1), size = ARG(2), out = ARG(3), flags = ARG(4);
    d3d_lock(d);
    if (size == 0 && off == 0) size = b->size;
    if (out == 0 || b->locked || off > b->size || size > b->size - off || size == 0)
    {
        RET(D3DERR_INVALIDCALL);
        d3d_unlock(d);
        return;
    }
    if ((flags & D3DLOCK_DISCARD) && b->last_use != 0 && !d3d_serial_done(d, b->last_use))
    {
        id<MTLBuffer> nb = nil;
        NSUInteger limit = BUFFER_RETIRED_BYTES / b->size;
        if (limit < MAX_RETIRED) limit = MAX_RETIRED;
        if (limit > BUFFER_RETIRED_MAX) limit = BUFFER_RETIRED_MAX;
        /* The list is in serial order (a rename appends the current
           last_use, which only grows), so only the oldest entry can be
           the first to complete. */
        if (b->retired.count > 0 && d3d_serial_done(d, b->retired[0]->serial))
        {
            nb = b->retired[0]->buf;
            [b->retired removeObjectAtIndex:0];
        }
        if (nb == nil && b->retired.count >= limit)
        {
            d3d_wait_serial(d, b->retired[0]->serial);
            nb = b->retired[0]->buf;
            [b->retired removeObjectAtIndex:0];
        }
        if (nb == nil)
        {
            nb = [d->mtl newBufferWithLength:b->size
                                     options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
            if (nb == nil) rt_fatal("D3D9: out of video memory (dynamic buffer)");
        }
        [b->retired addObject:d3d_retired_new(b->buf, nil, b->last_use)];
        b->buf = nb;
        b->last_use = 0;
    }
    else if (!(flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY)) && b->last_use != 0 &&
             !d3d_serial_done(d, b->last_use))
        d3d_wait_serial(d, b->last_use);
    if (b->staging_size < size)
    {
        if (b->staging != 0) rt_heap_free(b->staging);
        b->staging = rt_heap_alloc(size);
        b->staging_size = b->staging ? size : 0;
        if (b->staging == 0)
        {
            RET(D3DERR_OUTOFVIDEOMEMORY);
            d3d_unlock(d);
            return;
        }
    }
    if (!(flags & D3DLOCK_DISCARD)) memcpy(G2H(b->staging), (const uint8_t *)b->buf.contents + off, size);
    b->locked = true;
    b->lock_off = off;
    b->lock_size = size;
    b->lock_flags = flags;
    wr32(out, b->staging);
    RET(D3D_OK);
    d3d_unlock(d);
}

void rt_com_D3DVertexBuffer9_Unlock(_cpu *cpu)
{
    D3DBuffer *b = d3d_buf(buf_obj(ARG(0)));
    D3DDevice *d = b->dev;
    d3d_lock(d);
    if (!b->locked)
    {
        RET(D3DERR_INVALIDCALL);
        d3d_unlock(d);
        return;
    }
    b->locked = false;
    if (!(b->lock_flags & D3DLOCK_READONLY))
        memcpy((uint8_t *)b->buf.contents + b->lock_off, G2H(b->staging), b->lock_size);
    /* Static buffers do not keep their staging memory. */
    if (!(b->usage & D3DUSAGE_DYNAMIC))
    {
        rt_heap_free(b->staging);
        b->staging = 0;
        b->staging_size = 0;
    }
    RET(D3D_OK);
    d3d_unlock(d);
}

void rt_com_D3DVertexBuffer9_GetDesc(_cpu *cpu)
{
    D3DBuffer *b = d3d_buf(rt_com_this(ARG(0), 0, &d3d_vb_class));
    uint32_t p = ARG(1);
    if (p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    wr32(p, 100u /* D3DFMT_VERTEXDATA */);
    wr32(p + 4, D3DRTYPE_VERTEXBUFFER);
    wr32(p + 8, b->usage);
    wr32(p + 12, b->pool);
    wr32(p + 16, b->size);
    wr32(p + 20, b->format_or_fvf);
    RET(D3D_OK);
}

void rt_com_D3DIndexBuffer9_GetDesc(_cpu *cpu)
{
    D3DBuffer *b = d3d_buf(rt_com_this(ARG(0), 0, &d3d_ib_class));
    uint32_t p = ARG(1);
    if (p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    wr32(p, b->format_or_fvf);
    wr32(p + 4, D3DRTYPE_INDEXBUFFER);
    wr32(p + 8, b->usage);
    wr32(p + 12, b->pool);
    wr32(p + 16, b->size);
    RET(D3D_OK);
}

/* ---------------------------------------------------------------- vertex declarations */

uint32_t d3d_create_decl(D3DDevice *d, const d3d_velem *el, uint32_t n, uint32_t fvf, rt_com_object **out_obj)
{
    D3DDecl *dc = [D3DDecl new];
    uint32_t i;
    if (n > 64) return D3DERR_INVALIDCALL;
    for (i = 0; i < n; i++)
    {
        if (el[i].stream >= D3D_MAX_STREAMS || el[i].type > 16 || el[i].usage > 13 || el[i].method != 0)
            return D3DERR_INVALIDCALL;
        dc->el[i] = el[i];
    }
    dc->dev = d;
    dc->n = n;
    dc->fvf = fvf;
    dc->id = ++d->next_decl_id;
    *out_obj = make_object(&d3d_decl_class, dc);
    dc->com = *out_obj;
    return D3D_OK;
}

void rt_com_D3DVertexDeclaration9_GetDevice(_cpu *cpu)
{
    get_device(cpu, d3d_decl(rt_com_this(ARG(0), 0, &d3d_decl_class))->dev);
}

/* HRESULT GetDeclaration(D3DVERTEXELEMENT9 *elements, UINT *count) */
void rt_com_D3DVertexDeclaration9_GetDeclaration(_cpu *cpu)
{
    D3DDecl *dc = d3d_decl(rt_com_this(ARG(0), 0, &d3d_decl_class));
    uint32_t p = ARG(1), cnt = ARG(2), i;
    if (cnt == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (p != 0)
    {
        for (i = 0; i < dc->n; i++)
        {
            wr16(p + 8 * i, dc->el[i].stream);
            wr16(p + 8 * i + 2, dc->el[i].offset);
            wr8(p + 8 * i + 4, dc->el[i].type);
            wr8(p + 8 * i + 5, dc->el[i].method);
            wr8(p + 8 * i + 6, dc->el[i].usage);
            wr8(p + 8 * i + 7, dc->el[i].index);
        }
        /* D3DDECL_END */
        wr16(p + 8 * i, 0xFF);
        wr16(p + 8 * i + 2, 0);
        wr8(p + 8 * i + 4, 17);
        wr8(p + 8 * i + 5, 0);
        wr8(p + 8 * i + 6, 0);
        wr8(p + 8 * i + 7, 0);
    }
    wr32(cnt, dc->n + 1u);
    RET(D3D_OK);
}

/* ---------------------------------------------------------------- shaders */

uint32_t d3d_create_shader(D3DDevice *d, bool ps, uint32_t function, uint32_t out)
{
    D3DShader *s;
    char err[256];
    rt_com_object *o;
    uint32_t max;
    if (function == 0 || out == 0) return D3DERR_INVALIDCALL;
    s = [D3DShader new];
    /* The token stream ends at the end token; read at most 256 KiB. The
       readable length is the longest committed range from the start (a
       binary search: a halving search can give a shorter length than the
       shader when the shader is near the end of its memory block). */
    max = 65536u;
    if (!rt_va_is_committed(function, max * 4u))
    {
        uint32_t lo = 0u, hi = max; /* lo dwords are readable, hi are not */
        while (hi - lo > 1u)
        {
            uint32_t mid = lo + (hi - lo) / 2u;
            if (rt_va_is_committed(function, mid * 4u))
                lo = mid;
            else
                hi = mid;
        }
        if (lo < 2u) return D3DERR_INVALIDCALL;
        max = lo;
    }
    if (!d3d_shader_parse(&s->sh, G2H(function), max, err, sizeof err))
        rt_fatal("D3D9: %s shader is not supported: %s", ps ? "pixel" : "vertex", err);
    if (s->sh.is_ps != ps) rt_fatal("D3D9: Create%sShader with a %s shader", ps ? "Pixel" : "Vertex", s->sh.is_ps ? "pixel" : "vertex");
    s->dev = d;
    s->id = ++d->next_shader_id;
    if (s->id > 0xFFFFu) rt_fatal("D3D9: more than 65535 shaders");
    s->ps_funcs = [NSMutableDictionary new];
    o = make_object(ps ? &d3d_ps_class : &d3d_vs_class, s);
    s->com = o;
    write_iface(o, out);
    return D3D_OK;
}

static void shader_destroy(rt_com_object *o)
{
    D3DShader *s = (__bridge_transfer D3DShader *)o->data;
    d3d_shader_free(&s->sh);
}

static rt_com_object *shader_obj(uint32_t p)
{
    rt_com_object *o = rt_com_this(p, 0, NULL);
    if (o->cls != &d3d_vs_class && o->cls != &d3d_ps_class)
        rt_fatal("0x%08x is not a runtime shader (%s)", p, o->cls->name);
    return o;
}

void rt_com_D3DVertexShader9_GetDevice(_cpu *cpu)
{
    get_device(cpu, d3d_shader_obj(shader_obj(ARG(0)))->dev);
}

/* HRESULT GetFunction(void *data, UINT *size) */
void rt_com_D3DVertexShader9_GetFunction(_cpu *cpu)
{
    D3DShader *s = d3d_shader_obj(shader_obj(ARG(0)));
    uint32_t p = ARG(1), sz = ARG(2), bytes = s->sh.ntokens * 4u;
    if (sz == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (p != 0)
    {
        if (rd32(sz) < bytes)
        {
            RET(D3DERR_INVALIDCALL);
            return;
        }
        memcpy(G2H(p), s->sh.tokens, bytes);
    }
    wr32(sz, bytes);
    RET(D3D_OK);
}

/* ---------------------------------------------------------------- queries */

#define D3DQUERYTYPE_EVENT 8u
#define D3DISSUE_END 1u
#define D3DISSUE_BEGIN 2u
#define D3DGETDATA_FLUSH 1u

uint32_t d3d_create_query(D3DDevice *d, uint32_t type, uint32_t out)
{
    D3DQuery *q;
    rt_com_object *o;
    /* Only event queries are supported; the device reports the others as
       not available, which is a valid D3D9 answer. */
    if (type != D3DQUERYTYPE_EVENT) return D3DERR_NOTAVAILABLE;
    if (out == 0) return D3D_OK;
    q = [D3DQuery new];
    q->dev = d;
    q->type = type;
    o = make_object(&d3d_query_class, q);
    q->com = o;
    write_iface(o, out);
    return D3D_OK;
}

void rt_com_D3DQuery9_GetDevice(_cpu *cpu)
{
    get_device(cpu, ((__bridge D3DQuery *)rt_com_this(ARG(0), 0, &d3d_query_class)->data)->dev);
}

void rt_com_D3DQuery9_GetType(_cpu *cpu)
{
    RET(((__bridge D3DQuery *)rt_com_this(ARG(0), 0, &d3d_query_class)->data)->type);
}

void rt_com_D3DQuery9_GetDataSize(_cpu *cpu)
{
    (void)rt_com_this(ARG(0), 0, &d3d_query_class);
    RET(4); /* BOOL */
}

void rt_com_D3DQuery9_Issue(_cpu *cpu)
{
    D3DQuery *q = (__bridge D3DQuery *)rt_com_this(ARG(0), 0, &d3d_query_class)->data;
    uint32_t flags = ARG(1);
    if (flags != D3DISSUE_END)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(q->dev);
    q->serial = q->dev->serial;
    q->issued = true;
    d3d_unlock(q->dev);
    RET(D3D_OK);
}

/* HRESULT GetData(void *data, DWORD size, DWORD flags): S_OK when the GPU
   has passed the event, S_FALSE before. */
void rt_com_D3DQuery9_GetData(_cpu *cpu)
{
    D3DQuery *q = (__bridge D3DQuery *)rt_com_this(ARG(0), 0, &d3d_query_class)->data;
    uint32_t p = ARG(1), size = ARG(2);
    D3DDevice *d = q->dev;
    bool done;
    d3d_lock(d);
    if (!q->issued)
        done = true;
    else
    {
        /* The event is in the command buffer that is being recorded: commit
           it so that the event can complete. */
        if (q->serial == d->serial) d3d_flush(d);
        done = d3d_serial_done(d, q->serial);
    }
    d3d_unlock(d);
    if (done && p != 0 && size >= 4) wr32(p, 1);
    RET(done ? S_OK : S_FALSE);
}
