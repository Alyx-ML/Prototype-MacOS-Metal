/*
 * Direct3D 9 on Metal: render passes, pipelines and drawing (Clear, the
 * Draw* methods, StretchRect, ColorFill, Present). See rt_d3d9_int.h.
 *
 * Render passes: a pass starts at the first draw or partial clear after a
 * render target change and ends at the next render target change, blit,
 * read back or present. A clear of the whole render target is kept as a
 * pending load action for the next pass.
 *
 * All encoder state (pipeline, depth-stencil state, viewport, scissor,
 * rasterizer state, constants, textures, samplers, vertex buffers) is set
 * again for each draw. The pipelines, depth-stencil states and samplers are
 * cached by their D3D9 state (D3DStateCache: a hash table with fixed-size
 * byte keys, so that a lookup does not allocate).
 *
 * Mapping notes:
 *   - D3D9 pixel centers are at integer coordinates; the vertex shader
 *     moves positions by half a pixel (vs_misc.xy).
 *   - The constant depth bias (D3DRS_DEPTHBIAS) is added to z/w in the
 *     vertex shader (vs_misc.z), which gives the same depth offset for every
 *     depth format. The slope-scaled bias uses the Metal slope scale.
 *   - Front faces are clockwise, as in D3D9. UNCONFIRMED: that the Metal
 *     window-space winding is the same as the D3D9 winding for the same
 *     vertices (both map NDC +y to the top of the target).
 */
#import "rt_d3d9_int.h"
#include "rt_enh.h"
#include "rt_window.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PIPELINES 4096u
#define MAX_DS_STATES 1024u
#define MAX_SAMPLERS 1024u
#define MAX_PS_VARIANTS 64u
/* Depth-stencil keys: 12 words for draws, 2 words (zero-padded) for clears.
   The first word of a clear key (0xC1EA....) differs from a draw key (0 or 1). */
#define DS_KEY_WORDS 12u
#define NULL_VERTEX_BUFFER 20u /* Metal buffer index of the zero vertex data */

#define D3DPT_POINTLIST 1u
#define D3DPT_LINELIST 2u
#define D3DPT_LINESTRIP 3u
#define D3DPT_TRIANGLELIST 4u
#define D3DPT_TRIANGLESTRIP 5u
#define D3DPT_TRIANGLEFAN 6u

#define D3DSTREAMSOURCE_INDEXEDDATA (1u << 30)
#define D3DSTREAMSOURCE_INSTANCEDATA (2u << 30)

/* ---------------------------------------------------------------- built-in library */

static const char builtin_src[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct QV { float4 pos [[position]]; float2 uv; };\n"
    "static float2 tri(uint vid) { return float2(float((vid << 1) & 2u), float(vid & 2u)); }\n"
    "vertex QV present_vs(uint vid [[vertex_id]])\n"
    "{\n"
    "    float2 q = tri(vid);\n"
    "    QV o;\n"
    "    o.pos = float4(q * 2.0 - 1.0, 0.0, 1.0);\n"
    "    o.uv = float2(q.x, 1.0 - q.y);\n"
    "    return o;\n"
    "}\n"
    /* use_lut: apply the gamma ramp of SetGammaRamp. hdr: the drawable is
       extended linear sRGB (Mac enhancement HDR); the highlights are expanded
       up to the EDR headroom of the display, and SDR white stays at 1.
       src_linear: the source texture is an sRGB format (samples are linear). */
    "struct PresentParams { uint use_lut; uint hdr; float headroom; uint src_linear; };\n"
    "static float3 to_linear(float3 c) { return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045); }\n"
    "static float3 to_srgb(float3 c) { return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308); }\n"
    "fragment float4 present_fs(QV in [[stage_in]], texture2d<float> src [[texture(0)]],\n"
    "    texture2d<float> lut [[texture(1)]], sampler s [[sampler(0)]], constant PresentParams &pp [[buffer(0)]])\n"
    "{\n"
    "    float4 c = src.sample(s, in.uv);\n"
    "    if (pp.src_linear != 0u) c.rgb = to_srgb(saturate(c.rgb));\n"
    "    if (pp.use_lut != 0u) {\n"
    "        constexpr sampler ls(filter::linear, address::clamp_to_edge);\n"
    "        float3 t = saturate(c.rgb) * (255.0 / 256.0) + (0.5 / 256.0);\n"
    "        c.r = lut.sample(ls, float2(t.r, 0.5)).r;\n"
    "        c.g = lut.sample(ls, float2(t.g, 0.5)).g;\n"
    "        c.b = lut.sample(ls, float2(t.b, 0.5)).b;\n"
    "    }\n"
    "    if (pp.hdr != 0u) {\n"
    "        float3 l = to_linear(saturate(c.rgb));\n"
    "        float y = dot(l, float3(0.2126, 0.7152, 0.0722));\n"
    "        float y2 = y * y;\n"
    "        l *= 1.0 + (max(pp.headroom, 1.0) - 1.0) * y2 * y2;\n"
    "        return float4(l, 1.0);\n"
    "    }\n"
    "    return float4(c.rgb, 1.0);\n"
    "}\n"
    "vertex QV blit_vs(uint vid [[vertex_id]], constant float4 &r [[buffer(0)]])\n"
    "{\n"
    "    float2 q = tri(vid);\n"
    "    QV o;\n"
    "    o.pos = float4(q * 2.0 - 1.0, 0.0, 1.0);\n"
    "    o.uv = mix(r.xy, r.zw, float2(q.x, 1.0 - q.y));\n"
    "    return o;\n"
    "}\n"
    "fragment float4 blit_fs(QV in [[stage_in]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]])\n"
    "{\n"
    "    return src.sample(s, in.uv);\n"
    "}\n"
    "struct CV { float4 pos [[position]]; };\n"
    "vertex CV clear_vs(uint vid [[vertex_id]], constant float4 &p [[buffer(0)]])\n"
    "{\n"
    "    float2 q = tri(vid);\n"
    "    CV o;\n"
    "    o.pos = float4(q * 2.0 - 1.0, p.x, 1.0);\n"
    "    return o;\n"
    "}\n";

static id<MTLLibrary> compile_library(D3DDevice *d, NSString *src, const char *what)
{
    NSError *err = nil;
    MTLCompileOptions *o = [MTLCompileOptions new];
    id<MTLLibrary> lib = [d->mtl newLibraryWithSource:src options:o error:&err];
    if (lib == nil)
    {
        rt_log("D3D9: Metal source of %s:\n%s", what, src.UTF8String);
        rt_fatal("D3D9: Metal compile of %s failed: %s", what, err.localizedDescription.UTF8String);
    }
    if (err != nil) rt_log("D3D9: Metal compile of %s: %s", what, err.localizedDescription.UTF8String);
    return lib;
}

static id<MTLFunction> function_of(id<MTLLibrary> lib, const char *name)
{
    id<MTLFunction> f = [lib newFunctionWithName:@(name)];
    if (f == nil) rt_fatal("D3D9: Metal function %s is missing", name);
    return f;
}

void d3d_draw_init(D3DDevice *d)
{
    @autoreleasepool
    {
        NSMutableString *src = [NSMutableString stringWithUTF8String:builtin_src];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        MTLTextureDescriptor *td;
        NSError *err = nil;
        unsigned m, k;
        static const uint8_t black[4] = { 0, 0, 0, 0xFF };
        static const float zero_one[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        /* Clear fragment functions: clear_fs_<mask> writes the clear color to
           the color attachments of mask. */
        for (m = 1; m < 16; m++)
        {
            [src appendFormat:@"struct CO%u {", m];
            for (k = 0; k < 4; k++)
                if (m & (1u << k)) [src appendFormat:@" float4 c%u [[color(%u)]];", k, k];
            [src appendFormat:@" };\nfragment CO%u clear_fs_%u(constant float4 &c [[buffer(0)]])\n{\n    CO%u o;\n", m, m, m];
            for (k = 0; k < 4; k++)
                if (m & (1u << k)) [src appendFormat:@"    o.c%u = c;\n", k];
            [src appendString:@"    return o;\n}\n"];
        }
        d->builtin_lib = compile_library(d, src, "the built-in shaders");

        pd.vertexFunction = function_of(d->builtin_lib, "present_vs");
        pd.fragmentFunction = function_of(d->builtin_lib, "present_fs");
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        d->present_pso = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
        if (d->present_pso == nil)
            rt_fatal("D3D9: present pipeline creation failed: %s", err.localizedDescription.UTF8String);
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        d->present_pso_hdr = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
        if (d->present_pso_hdr == nil)
            rt_fatal("D3D9: HDR present pipeline creation failed: %s", err.localizedDescription.UTF8String);

        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        d->present_linear = [d->mtl newSamplerStateWithDescriptor:sd];
        sd.minFilter = MTLSamplerMinMagFilterNearest;
        sd.magFilter = MTLSamplerMinMagFilterNearest;
        d->present_point = [d->mtl newSamplerStateWithDescriptor:sd];
        if (d->present_linear == nil || d->present_point == nil) rt_fatal("D3D9: sampler creation failed");

        /* A texture that no texture is bound to reads (0, 0, 0, 1).
           UNCONFIRMED: the D3D9 value for an unbound sampler. */
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1
                                                            mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead;
        d->null_tex2d = [d->mtl newTextureWithDescriptor:td];
        td = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:1 mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead;
        d->null_texcube = [d->mtl newTextureWithDescriptor:td];
        td = [MTLTextureDescriptor new];
        td.textureType = MTLTextureType3D;
        td.pixelFormat = MTLPixelFormatBGRA8Unorm;
        td.width = 1;
        td.height = 1;
        td.depth = 1;
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead;
        d->null_tex3d = [d->mtl newTextureWithDescriptor:td];
        if (d->null_tex2d == nil || d->null_texcube == nil || d->null_tex3d == nil)
            rt_fatal("D3D9: texture creation failed");
        [d->null_tex3d replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:black
                         bytesPerRow:4 bytesPerImage:4];
        [d->null_tex2d replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:black bytesPerRow:4];
        for (k = 0; k < 6; k++)
            [d->null_texcube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:k withBytes:black
                               bytesPerRow:4 bytesPerImage:4];
        /* Vertex shader inputs that the declaration does not supply read
           (0, 0, 0, 1). UNCONFIRMED: the D3D9 value. */
        d->null_vertex = [d->mtl newBufferWithBytes:zero_one length:sizeof zero_one options:MTLResourceStorageModeShared];
        if (d->null_vertex == nil) rt_fatal("D3D9: buffer creation failed");
    }
}

/* ---------------------------------------------------------------- render passes */

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static bool is_srgb(MTLPixelFormat pf)
{
    switch (pf)
    {
    case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Unorm_sRGB: return true;
    default: return false;
    }
}

/* The attachment texture of a surface. */
static id<MTLTexture> attach_texture(D3DSurface *s, bool srgb, NSUInteger *slice, NSUInteger *level)
{
    id<MTLTexture> t = d3d_surface_texture(s, slice, level);
    if (t == nil) rt_fatal("D3D9: a system-memory surface is bound as a render target");
    if (srgb && s->f->pf_srgb != MTLPixelFormatInvalid)
    {
        __strong id<MTLTexture> *slot = s->ctex != nil ? &s->ctex->rt_srgb : &s->rt_srgb;
        if (*slot == nil)
        {
            *slot = [t newTextureViewWithPixelFormat:s->f->pf_srgb];
            if (*slot == nil) rt_fatal("D3D9: sRGB render target view creation failed (format 0x%x)", s->fmt);
        }
        t = *slot;
    }
    return t;
}

static bool pending_clear(const D3DDevice *d)
{
    return d->clear_color[0] || d->clear_color[1] || d->clear_color[2] || d->clear_color[3] || d->clear_depth ||
           d->clear_stencil;
}

/* Clear the record of the encoder state (see D3DDevice.es_valid). */
static void enc_state_reset(D3DDevice *d)
{
    uint32_t i;
    d->es_valid = false;
    d->es_pipe = nil;
    d->es_ds = nil;
    for (i = 0; i < D3D_SAMPLERS; i++)
    {
        d->es_tex[i] = nil;
        d->es_smp[i] = nil;
    }
    for (i = 0; i < D3D_MAX_STREAMS; i++)
    {
        d->es_vb[i] = nil;
        d->es_vb_off[i] = 0;
    }
    d->es_null_vb = false;
}

static void begin_pass(D3DDevice *d)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    bool srgb = d->rs[RS_SRGBWRITEENABLE] != 0;
    uint32_t i, w = UINT32_MAX, h = UINT32_MAX, samples = 0, scale;
    NSUInteger slice, level;
    if (d->rt[0] == NULL) rt_fatal("D3D9: render pass without render target 0");
    scale = d3d_surf(d->rt[0])->scale;
    for (i = 0; i < D3D_MAX_RT; i++)
    {
        D3DSurface *s;
        id<MTLTexture> t;
        MTLRenderPassColorAttachmentDescriptor *ca;
        d->pass_color[i] = MTLPixelFormatInvalid;
        if (d->rt[i] == NULL) continue;
        s = d3d_surf(d->rt[i]);
        t = attach_texture(s, srgb, &slice, &level);
        ca = rp.colorAttachments[i];
        ca.texture = t;
        ca.slice = slice;
        ca.level = level;
        if (d->clear_color[i])
        {
            float c[4];
            int k;
            for (k = 0; k < 4; k++) c[k] = is_srgb(t.pixelFormat) && k < 3 ? srgb_to_linear(d->clear_rgba[k]) : d->clear_rgba[k];
            ca.loadAction = MTLLoadActionClear;
            ca.clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
        }
        else
            ca.loadAction = MTLLoadActionLoad;
        ca.storeAction = MTLStoreActionStore;
        if (s->scale != scale)
            rt_fatal("D3D9: render targets of %ux%u (%ux shadow resolution) and %ux%u (%ux) in one pass; set \"Shadow "
                     "Resolution\" to \"Game\" in the Video options",
                     d3d_surf(d->rt[0])->width, d3d_surf(d->rt[0])->height, scale, s->width, s->height, s->scale);
        if (s->width * s->scale < w) w = s->width * s->scale;
        if (s->height * s->scale < h) h = s->height * s->scale;
        if (samples == 0)
            samples = (uint32_t)t.sampleCount;
        else if (samples != t.sampleCount)
            rt_fatal("D3D9: render targets with different sample counts");
        d->pass_color[i] = t.pixelFormat;
        d3d_use_surface(s);
    }
    d->pass_depth = MTLPixelFormatInvalid;
    d->pass_stencil = MTLPixelFormatInvalid;
    if (d->ds != NULL)
    {
        D3DSurface *s = d3d_surf(d->ds);
        id<MTLTexture> t = attach_texture(s, false, &slice, &level);
        if (s->scale != scale)
            rt_fatal("D3D9: a %ux%u render target (%ux shadow resolution) with a %ux%u depth-stencil surface (%ux); set "
                     "\"Shadow Resolution\" to \"Game\" in the Video options",
                     d3d_surf(d->rt[0])->width, d3d_surf(d->rt[0])->height, scale, s->width, s->height, s->scale);
        if (t.sampleCount != samples)
        {
            /* Direct3D 9 draws with a depth-stencil surface whose sample
               count is not the render target's as if no depth-stencil
               surface were set: Wine's d3d9 test test_multisample_mismatch
               (dlls/d3d9/tests/visual.c), run on Windows, draws with
               D3DRS_ZENABLE TRUE into a single-sample target with a
               2-sample depth buffer, and the geometry is drawn. Wine's
               wined3d does not attach the depth-stencil surface in this
               case, also when the render target has more samples (there
               NVIDIA and AMD drivers differ; UNCONFIRMED for this game).
               The pass gets no depth-stencil attachment, so depth and
               stencil tests are off and a pending depth or stencil clear
               has no effect (NVIDIA behavior in the same test). */
            static bool logged;
            if (!logged)
                rt_log("D3D9: the depth-stencil surface (%lu samples) and the render target (%u samples) differ in "
                       "sample count; the pass draws without the depth-stencil surface",
                       (unsigned long)t.sampleCount, samples);
            logged = true;
            goto no_ds;
        }
        rp.depthAttachment.texture = t;
        rp.depthAttachment.slice = slice;
        rp.depthAttachment.level = level;
        rp.depthAttachment.loadAction = d->clear_depth ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = d->clear_z;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        d->pass_depth = t.pixelFormat;
        if (s->f->stencil)
        {
            rp.stencilAttachment.texture = t;
            rp.stencilAttachment.slice = slice;
            rp.stencilAttachment.level = level;
            rp.stencilAttachment.loadAction = d->clear_stencil ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = d->clear_s & 0xFFu;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
            d->pass_stencil = t.pixelFormat;
        }
        /* UNCONFIRMED: Metal accepts a depth attachment that is larger than
           the color attachments (D3D9 allows it); the render area is set to
           the color size. */
        d3d_use_surface(s);
    }
no_ds:
    rp.renderTargetWidth = w;
    rp.renderTargetHeight = h;
    d->enc = [d->cb renderCommandEncoderWithDescriptor:rp];
    if (d->enc == nil) rt_fatal("D3D9: render command encoder creation failed");
    for (i = 0; i < D3D_MAX_RT; i++) d->clear_color[i] = false;
    d->clear_depth = false;
    d->clear_stencil = false;
    d->enc_draws = false;
    d->pass_samples = samples;
    d->pass_w = w;
    d->pass_h = h;
    d->pass_scale = scale;
    d->pass_srgb = srgb;
    enc_state_reset(d);
}

void d3d_end_pass(D3DDevice *d)
{
    @autoreleasepool
    {
        if (d->enc == nil && pending_clear(d)) begin_pass(d);
        if (d->enc != nil)
        {
            [d->enc endEncoding];
            d->enc = nil;
        }
    }
}

/* Start a pass for drawing (the pass must use the current sRGB state). */
static void need_pass(D3DDevice *d)
{
    if (d->enc != nil && d->pass_srgb != (d->rs[RS_SRGBWRITEENABLE] != 0)) d3d_end_pass(d);
    if (d->enc == nil) begin_pass(d);
}

/* ---------------------------------------------------------------- caches */

static void cache_limit(NSMutableDictionary *dict, NSUInteger max, const char *what)
{
    if (dict.count >= max)
    {
        rt_log("D3D9: %s cache is full (%lu entries); it is emptied", what, (unsigned long)dict.count);
        [dict removeAllObjects];
    }
}

@implementation D3DStateCache
- (void)dealloc
{
    uint32_t i;
    for (i = 0; i < count; i++) CFRelease(objs[i]);
    free(slots);
    free(hashes);
    free(keys);
    free(objs);
}
@end

/* key_size must be a multiple of 4; limit is the most entries. */
static D3DStateCache *state_cache_new(uint32_t key_size, uint32_t limit, const char *name)
{
    D3DStateCache *c = [D3DStateCache new];
    uint32_t cap = 1;
    if (key_size == 0 || key_size % 4u != 0 || limit == 0 || limit > (1u << 20) ||
        key_size > (64u << 20) / limit)
        rt_fatal("D3D9: %s cache: bad key size %u or limit %u", name, key_size, limit);
    while (cap < limit * 2u) cap <<= 1;
    c->name = name;
    c->key_size = key_size;
    c->limit = limit;
    c->mask = cap - 1u;
    c->slots = calloc(cap, sizeof *c->slots);
    c->hashes = calloc(limit, sizeof *c->hashes);
    c->keys = calloc(limit, key_size);
    c->objs = calloc(limit, sizeof *c->objs);
    if (c->slots == NULL || c->hashes == NULL || c->keys == NULL || c->objs == NULL)
        rt_fatal("D3D9: out of memory (%s cache)", name);
    return c;
}

static uint64_t key_hash(const uint8_t *k, uint32_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    uint32_t i = 0;
    for (; i + 8u <= n; i += 8u)
    {
        uint64_t w;
        memcpy(&w, k + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    if (i < n)
    {
        uint32_t w;
        memcpy(&w, k + i, 4);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    return h;
}

/* The cached object for key (key_size bytes), or nil. */
static id state_cache_get(D3DStateCache *c, const void *key)
{
    uint64_t h = key_hash(key, c->key_size);
    uint32_t i = (uint32_t)h & c->mask;
    for (;;)
    {
        uint32_t e = c->slots[i];
        if (e == 0) return nil;
        e--;
        if (c->hashes[e] == h && memcmp(c->keys + (size_t)e * c->key_size, key, c->key_size) == 0)
            return (__bridge id)c->objs[e];
        i = (i + 1u) & c->mask;
    }
}

/* Add obj for key (not in the cache). A full cache is emptied first. */
static void state_cache_put(D3DStateCache *c, const void *key, id obj)
{
    uint64_t h = key_hash(key, c->key_size);
    uint32_t i, e;
    if (c->count >= c->limit)
    {
        rt_log("D3D9: %s cache is full (%u entries); it is emptied", c->name, c->count);
        for (e = 0; e < c->count; e++) CFRelease(c->objs[e]);
        memset(c->slots, 0, ((size_t)c->mask + 1u) * sizeof *c->slots);
        c->count = 0;
    }
    e = c->count++;
    c->hashes[e] = h;
    memcpy(c->keys + (size_t)e * c->key_size, key, c->key_size);
    c->objs[e] = (void *)CFBridgingRetain(obj);
    for (i = (uint32_t)h & c->mask; c->slots[i] != 0; i = (i + 1u) & c->mask) {}
    c->slots[i] = e + 1u;
}

/* ---------------------------------------------------------------- state mapping */

static MTLCompareFunction cmp_func(uint32_t f)
{
    switch (f)
    {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    case 8: return MTLCompareFunctionAlways;
    default: rt_fatal("D3D9: compare function %u is not valid", f);
    }
}

static MTLStencilOperation stencil_op(uint32_t op)
{
    switch (op)
    {
    case 1: return MTLStencilOperationKeep;
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: rt_fatal("D3D9: stencil operation %u is not valid", op);
    }
}

static MTLBlendFactor blend_factor(uint32_t b)
{
    switch (b)
    {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    default: rt_fatal("D3D9: blend factor %u is not supported", b);
    }
}

static MTLBlendOperation blend_op(uint32_t op)
{
    switch (op)
    {
    case 1: return MTLBlendOperationAdd;
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: rt_fatal("D3D9: blend operation %u is not valid", op);
    }
}

/* D3DBLEND_BOTHSRCALPHA (12) and D3DBLEND_BOTHINVSRCALPHA (13) set both
   factors from the source factor. */
static void blend_pair(uint32_t src, uint32_t dst, uint32_t *s, uint32_t *t)
{
    if (src == 12)
    {
        *s = 5;
        *t = 6;
    }
    else if (src == 13)
    {
        *s = 6;
        *t = 5;
    }
    else
    {
        *s = src;
        *t = dst;
    }
}

static MTLVertexFormat vertex_format(uint32_t type, uint32_t *size)
{
    switch (type)
    {
    case 0: *size = 4; return MTLVertexFormatFloat;
    case 1: *size = 8; return MTLVertexFormatFloat2;
    case 2: *size = 12; return MTLVertexFormatFloat3;
    case 3: *size = 16; return MTLVertexFormatFloat4;
    case 4: *size = 4; return MTLVertexFormatUChar4Normalized_BGRA;
    /* UNCONFIRMED: Metal converts the integer formats to the float values
       of the float4 shader attribute, as D3D9 does. */
    case 5: *size = 4; return MTLVertexFormatUChar4;
    case 6: *size = 4; return MTLVertexFormatShort2;
    case 7: *size = 8; return MTLVertexFormatShort4;
    case 8: *size = 4; return MTLVertexFormatUChar4Normalized;
    case 9: *size = 4; return MTLVertexFormatShort2Normalized;
    case 10: *size = 8; return MTLVertexFormatShort4Normalized;
    case 11: *size = 4; return MTLVertexFormatUShort2Normalized;
    case 12: *size = 8; return MTLVertexFormatUShort4Normalized;
    case 14: *size = 4; return MTLVertexFormatInt1010102Normalized;
    case 15: *size = 4; return MTLVertexFormatHalf2;
    case 16: *size = 8; return MTLVertexFormatHalf4;
    default: rt_fatal("D3D9: vertex element type %u is not supported", type);
    }
}

/* ---------------------------------------------------------------- shaders */

static id<MTLFunction> vs_function(D3DDevice *d, D3DShader *vs)
{
    d3dsh_text t = { 0 };
    char err[256];
    if (vs->vs_func != nil) return vs->vs_func;
    if (!d3d_shader_msl_vs(&vs->sh, &t, err, sizeof err)) rt_fatal("D3D9: vertex shader translation failed: %s", err);
    @autoreleasepool
    {
        NSString *src = [[NSString alloc] initWithBytes:t.s length:t.len encoding:NSUTF8StringEncoding];
        vs->vs_func = function_of(compile_library(d, src, "a vertex shader"), "vs_main");
    }
    d3dsh_text_free(&t);
    return vs->vs_func;
}

static id<MTLFunction> ps_function(D3DDevice *d, D3DShader *ps, D3DShader *vs, uint16_t dmask)
{
    NSNumber *key = @((vs->id << 16) | dmask);
    id<MTLFunction> f = ps->ps_funcs[key];
    d3dsh_text t = { 0 };
    char err[256];
    if (f != nil) return f;
    if (!d3d_shader_msl_ps(&ps->sh, vs->sh.outputs, vs->sh.noutputs, dmask, &t, err, sizeof err))
        rt_fatal("D3D9: pixel shader translation failed: %s", err);
    @autoreleasepool
    {
        NSString *src = [[NSString alloc] initWithBytes:t.s length:t.len encoding:NSUTF8StringEncoding];
        f = function_of(compile_library(d, src, "a pixel shader"), "ps_main");
    }
    d3dsh_text_free(&t);
    cache_limit(ps->ps_funcs, MAX_PS_VARIANTS, "pixel shader variant");
    ps->ps_funcs[key] = f;
    return f;
}

/* ---------------------------------------------------------------- pipelines */

typedef struct pipe_key {
    uint32_t kind; /* 0 draw, 1 clear, 2 blit */
    uint32_t vs, ps, decl;
    uint32_t stride[D3D_MAX_STREAMS];
    uint32_t step[D3D_MAX_STREAMS]; /* 0 unused, 1 per vertex, 2 per instance (rate in high bits), 3 constant */
    uint32_t color[D3D_MAX_RT], depth, stencil, samples;
    uint32_t blend, src, dst, op, src_a, dst_a, op_a;
    uint32_t wmask[D3D_MAX_RT];
    uint32_t depth_compare;
    uint32_t clear_flags, clear_mask;
} pipe_key;

static bool blendable(MTLPixelFormat pf)
{
    switch (pf)
    {
    case MTLPixelFormatR32Float:
    case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA32Float: return false;
    default: return true;
    }
}

static MTLColorWriteMask write_mask(uint32_t m)
{
    MTLColorWriteMask r = MTLColorWriteMaskNone;
    if (m & 1u) r |= MTLColorWriteMaskRed;
    if (m & 2u) r |= MTLColorWriteMaskGreen;
    if (m & 4u) r |= MTLColorWriteMaskBlue;
    if (m & 8u) r |= MTLColorWriteMaskAlpha;
    return r;
}

static void set_attachments(MTLRenderPipelineDescriptor *pd, const pipe_key *k)
{
    uint32_t i;
    for (i = 0; i < D3D_MAX_RT; i++)
    {
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        ca.pixelFormat = (MTLPixelFormat)k->color[i];
        if (k->color[i] == MTLPixelFormatInvalid) continue;
        ca.writeMask = write_mask(k->wmask[i]);
        /* Blending on a format that cannot blend is off (D3D9 reports
           such formats without D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING).
           UNCONFIRMED: D3D9 hardware ignores the blend state there. */
        if (k->blend && blendable((MTLPixelFormat)k->color[i]))
        {
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = blend_factor(k->src);
            ca.destinationRGBBlendFactor = blend_factor(k->dst);
            ca.rgbBlendOperation = blend_op(k->op);
            ca.sourceAlphaBlendFactor = blend_factor(k->src_a);
            ca.destinationAlphaBlendFactor = blend_factor(k->dst_a);
            ca.alphaBlendOperation = blend_op(k->op_a);
        }
    }
    pd.depthAttachmentPixelFormat = (MTLPixelFormat)k->depth;
    pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k->stencil;
    pd.rasterSampleCount = k->samples ? k->samples : 1;
}

static id<MTLRenderPipelineState> make_pipeline(D3DDevice *d, const pipe_key *k, MTLRenderPipelineDescriptor *pd)
{
    id<MTLRenderPipelineState> p = state_cache_get(d->pipelines, k);
    NSError *err = nil;
    if (p != nil) return p;
    set_attachments(pd, k);
    p = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
    if (p == nil) rt_fatal("D3D9: render pipeline creation failed: %s", err.localizedDescription.UTF8String);
    state_cache_put(d->pipelines, k, p);
    return p;
}

static void pass_key(const D3DDevice *d, pipe_key *k)
{
    uint32_t i;
    for (i = 0; i < D3D_MAX_RT; i++) k->color[i] = (uint32_t)d->pass_color[i];
    k->depth = (uint32_t)d->pass_depth;
    k->stencil = (uint32_t)d->pass_stencil;
    k->samples = d->pass_samples;
}

/* The clear pipeline for the current pass: flags D3DCLEAR_*, mask = color
   attachments to write. */
static id<MTLRenderPipelineState> clear_pipeline(D3DDevice *d, const pipe_key *base, uint32_t flags)
{
    pipe_key k = *base;
    uint32_t i, mask = 0;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    k.kind = 1;
    k.clear_flags = flags;
    for (i = 0; i < D3D_MAX_RT; i++)
    {
        if (k.color[i] != MTLPixelFormatInvalid && (flags & D3DCLEAR_TARGET)) mask |= 1u << i;
        k.wmask[i] = (mask & (1u << i)) ? 0xFu : 0u;
    }
    k.clear_mask = mask;
    pd.vertexFunction = function_of(d->builtin_lib, "clear_vs");
    if (mask != 0)
    {
        char name[32];
        snprintf(name, sizeof name, "clear_fs_%u", mask);
        pd.fragmentFunction = function_of(d->builtin_lib, name);
    }
    return make_pipeline(d, &k, pd);
}

static id<MTLDepthStencilState> ds_state(D3DDevice *d, MTLDepthStencilDescriptor *dd, const void *key, size_t keylen)
{
    uint32_t k[DS_KEY_WORDS];
    id<MTLDepthStencilState> s;
    if (keylen > sizeof k) rt_fatal("D3D9: depth-stencil key of %zu bytes is too large", keylen);
    memset(k, 0, sizeof k);
    memcpy(k, key, keylen);
    s = state_cache_get(d->ds_states, k);
    if (s != nil) return s;
    s = [d->mtl newDepthStencilStateWithDescriptor:dd];
    if (s == nil) rt_fatal("D3D9: depth-stencil state creation failed");
    state_cache_put(d->ds_states, k, s);
    return s;
}

/* ---------------------------------------------------------------- clears */

/* Intersect r with b; returns false if empty. r: left, top, right, bottom. */
static bool clip_rect(int32_t r[4], const int32_t b[4])
{
    if (r[0] < b[0]) r[0] = b[0];
    if (r[1] < b[1]) r[1] = b[1];
    if (r[2] > b[2]) r[2] = b[2];
    if (r[3] > b[3]) r[3] = b[3];
    return r[0] < r[2] && r[1] < r[3];
}

/* Draw clear quads into the current pass. */
static void clear_quads(D3DDevice *d, uint32_t flags, const int32_t (*rects)[4], uint32_t n, const float rgba[4], float z,
                        uint32_t stencil)
{
    pipe_key k;
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    uint32_t i, dkey[2];
    float col[4], zp[4] = { z, 0.0f, 0.0f, 0.0f };
    MTLViewport vp;
    memset(&k, 0, sizeof k);
    pass_key(d, &k);
    [d->enc setRenderPipelineState:clear_pipeline(d, &k, flags)];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = (flags & D3DCLEAR_ZBUFFER) != 0 && d->pass_depth != MTLPixelFormatInvalid;
    if ((flags & D3DCLEAR_STENCIL) && d->pass_stencil != MTLPixelFormatInvalid)
    {
        MTLStencilDescriptor *st = [MTLStencilDescriptor new];
        st.stencilCompareFunction = MTLCompareFunctionAlways;
        st.stencilFailureOperation = MTLStencilOperationReplace;
        st.depthFailureOperation = MTLStencilOperationReplace;
        st.depthStencilPassOperation = MTLStencilOperationReplace;
        st.readMask = 0xFFu;
        st.writeMask = 0xFFu;
        dd.frontFaceStencil = st;
        dd.backFaceStencil = st;
    }
    dkey[0] = 0xC1EA0000u | (dd.depthWriteEnabled ? 1u : 0u) | ((flags & D3DCLEAR_STENCIL) ? 2u : 0u);
    dkey[1] = 0;
    [d->enc setDepthStencilState:ds_state(d, dd, dkey, sizeof dkey)];
    [d->enc setStencilReferenceValue:stencil & 0xFFu];
    for (i = 0; i < 4; i++)
    {
        bool srgb = false;
        uint32_t r;
        for (r = 0; r < D3D_MAX_RT; r++)
            if (d->pass_color[r] != MTLPixelFormatInvalid && is_srgb(d->pass_color[r])) srgb = true;
        /* All color attachments of a pass have the same sRGB state. */
        col[i] = srgb && i < 3 ? srgb_to_linear(rgba[i]) : rgba[i];
    }
    [d->enc setFragmentBytes:col length:sizeof col atIndex:0];
    [d->enc setVertexBytes:zp length:sizeof zp atIndex:0];
    vp.originX = 0;
    vp.originY = 0;
    vp.width = d->pass_w;
    vp.height = d->pass_h;
    vp.znear = 0;
    vp.zfar = 1;
    [d->enc setViewport:vp];
    [d->enc setCullMode:MTLCullModeNone];
    [d->enc setTriangleFillMode:MTLTriangleFillModeFill];
    [d->enc setDepthBias:0 slopeScale:0 clamp:0];
    for (i = 0; i < n; i++)
    {
        MTLScissorRect sr = { (NSUInteger)rects[i][0], (NSUInteger)rects[i][1], (NSUInteger)(rects[i][2] - rects[i][0]),
                              (NSUInteger)(rects[i][3] - rects[i][1]) };
        [d->enc setScissorRect:sr];
        [d->enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    d->enc_draws = true;
    /* The clear quads changed the pipeline, the depth-stencil state, the
       raster state and vertex buffer 0 (setVertexBytes at index 0). */
    enc_state_reset(d);
}

static void color_of(uint32_t c, float rgba[4])
{
    rgba[0] = (float)((c >> 16) & 0xFFu) / 255.0f;
    rgba[1] = (float)((c >> 8) & 0xFFu) / 255.0f;
    rgba[2] = (float)(c & 0xFFu) / 255.0f;
    rgba[3] = (float)(c >> 24) / 255.0f;
}

/* HRESULT Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) */
void rt_com_D3DDevice9_Clear(_cpu *cpu)
{
    D3DDevice *d = d3d_device_of(ARG(0));
    uint32_t count = ARG(1), rects = ARG(2), flags = ARG(3), color = ARG(4), stencil = ARG(6), i, n = 0;
    float z = d3d_f(ARG(5)), rgba[4];
    D3DSurface *rt0, *ds;
    int32_t area[4], (*list)[4];
    bool full;
    d3d_lock(d);
    @autoreleasepool
    {
        rt0 = d->rt[0] != NULL ? d3d_surf(d->rt[0]) : nil;
        ds = d->ds != NULL ? d3d_surf(d->ds) : nil;
        if (rt0 == nil || (count != 0 && rects == 0) || (count == 0 && rects != 0) || (flags & ~7u) != 0 ||
            ((flags & D3DCLEAR_ZBUFFER) && ds == nil) || ((flags & D3DCLEAR_STENCIL) && (ds == nil || !ds->f->stencil)) ||
            count > 0x10000u)
        {
            RET(D3DERR_INVALIDCALL);
            d3d_unlock(d);
            return;
        }
        if (flags == 0)
        {
            RET(D3D_OK);
            d3d_unlock(d);
            return;
        }
        color_of(color, rgba);
        if (z < 0.0f) z = 0.0f;
        if (z > 1.0f) z = 1.0f;
        /* The clear area: the viewport, and the scissor rectangle when the
           scissor test is on. */
        area[0] = (int32_t)d->viewport[0];
        area[1] = (int32_t)d->viewport[1];
        area[2] = (int32_t)(d->viewport[0] + d->viewport[2]);
        area[3] = (int32_t)(d->viewport[1] + d->viewport[3]);
        if (d->rs[RS_SCISSORTESTENABLE] && !clip_rect(area, d->scissor))
        {
            RET(D3D_OK);
            d3d_unlock(d);
            return;
        }
        full = count == 0 && area[0] == 0 && area[1] == 0 && (uint32_t)area[2] == rt0->width &&
               (uint32_t)area[3] == rt0->height;
        for (i = 1; i < D3D_MAX_RT && full; i++)
            if (d->rt[i] != NULL && (d3d_surf(d->rt[i])->width != rt0->width || d3d_surf(d->rt[i])->height != rt0->height))
                full = false;
        if (full && (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && (ds->width != rt0->width || ds->height != rt0->height))
            full = false;
        /* A depth-stencil surface with stencil is one Metal texture: a pass
           load action clears depth and stencil separately, so a clear of only
           one of them is still a load action. */
        if (full)
        {
            if (d->enc != nil) d3d_end_pass(d);
            if (flags & D3DCLEAR_TARGET)
            {
                for (i = 0; i < D3D_MAX_RT; i++) d->clear_color[i] = d->rt[i] != NULL;
                memcpy(d->clear_rgba, rgba, sizeof rgba);
            }
            if (flags & D3DCLEAR_ZBUFFER)
            {
                d->clear_depth = true;
                d->clear_z = z;
            }
            if (flags & D3DCLEAR_STENCIL)
            {
                d->clear_stencil = true;
                d->clear_s = stencil;
            }
            RET(D3D_OK);
            d3d_unlock(d);
            return;
        }
        need_pass(d);
        list = malloc(sizeof *list * (count ? count : 1u));
        if (list == NULL) rt_fatal("out of host memory (Clear)");
        if (count == 0)
        {
            memcpy(list[0], area, sizeof area);
            n = 1;
        }
        else
            for (i = 0; i < count; i++)
            {
                int32_t r[4];
                uint32_t p = rects + 16u * i;
                r[0] = (int32_t)rd32(p);
                r[1] = (int32_t)rd32(p + 4);
                r[2] = (int32_t)rd32(p + 8);
                r[3] = (int32_t)rd32(p + 12);
                if (clip_rect(r, area)) memcpy(list[n++], r, sizeof r);
            }
        {
            int32_t pass_area[4] = { 0, 0, (int32_t)d->pass_w, (int32_t)d->pass_h };
            uint32_t m = 0, k;
            for (i = 0; i < n; i++)
            {
                for (k = 0; k < 4; k++) list[i][k] *= (int32_t)d->pass_scale;
                if (clip_rect(list[i], pass_area)) memcpy(list[m++], list[i], sizeof list[i]);
            }
            n = m;
        }
        if (n > 0) clear_quads(d, flags, (const int32_t(*)[4])list, n, rgba, z, stencil);
        free(list);
    }
    RET(D3D_OK);
    d3d_unlock(d);
}

/* ---------------------------------------------------------------- samplers */

typedef struct samp_key {
    uint32_t min, mag, mip, u, v, w, border, aniso, lodmin, compare;
} samp_key;

static MTLSamplerAddressMode address_mode(uint32_t a, uint32_t border)
{
    switch (a)
    {
    case 1: return MTLSamplerAddressModeRepeat;
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4:
        if (border != 0 && border != 0xFF000000u && border != 0xFFFFFFFFu)
            rt_fatal("D3D9: border color 0x%08x is not supported (only transparent black, black, white)", border);
        return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: rt_fatal("D3D9: texture address mode %u is not valid", a);
    }
}

static MTLSamplerMinMagFilter minmag(uint32_t f)
{
    switch (f)
    {
    case 0: /* D3DTEXF_NONE is not valid for MIN and MAG; D3D9 uses point. UNCONFIRMED */
    case 1: return MTLSamplerMinMagFilterNearest;
    case 2:
    case 3: return MTLSamplerMinMagFilterLinear;
    default: rt_fatal("D3D9: texture filter %u is not supported", f);
    }
}

static id<MTLSamplerState> sampler_for(D3DDevice *d, const uint32_t *ss, uint32_t lod, bool compare)
{
    samp_key k;
    id<MTLSamplerState> s;
    MTLSamplerDescriptor *sd;
    uint32_t lmin;
    if (ss[SS_MIPMAPLODBIAS] != 0 && d3d_f(ss[SS_MIPMAPLODBIAS]) != 0.0f)
        rt_fatal("D3D9: D3DSAMP_MIPMAPLODBIAS %g is not supported (Metal samplers have no LOD bias)",
                 (double)d3d_f(ss[SS_MIPMAPLODBIAS]));
    memset(&k, 0, sizeof k);
    k.min = ss[SS_MINFILTER];
    k.mag = ss[SS_MAGFILTER];
    k.mip = ss[SS_MIPFILTER];
    k.u = ss[SS_ADDRESSU];
    k.v = ss[SS_ADDRESSV];
    k.w = ss[SS_ADDRESSW];
    k.border = (k.u == 4 || k.v == 4 || k.w == 4) ? ss[SS_BORDERCOLOR] : 0;
    k.aniso = (k.min == 3 || k.mag == 3) ? ss[SS_MAXANISOTROPY] : 1;
    /* Mac enhancement: 16x anisotropic filtering for the mipmapped textures
       with linear filtering. Point-filtered textures (text, interface) and
       depth comparison samplers (shadows) keep the game setting. */
    if (rt_enh_get(RT_ENH_ANISOTROPY) != 0 && !compare && k.mip != 0 && k.min >= 2 && k.mag >= 2) k.aniso = 16;
    lmin = ss[SS_MAXMIPLEVEL] > lod ? ss[SS_MAXMIPLEVEL] : lod;
    k.lodmin = lmin;
    k.compare = compare;
    s = state_cache_get(d->samplers, &k);
    if (s != nil) return s;
    sd = [MTLSamplerDescriptor new];
    sd.minFilter = minmag(k.min);
    sd.magFilter = minmag(k.mag);
    switch (k.mip)
    {
    case 0: sd.mipFilter = MTLSamplerMipFilterNotMipmapped; break;
    case 1: sd.mipFilter = MTLSamplerMipFilterNearest; break;
    case 2:
    case 3: sd.mipFilter = MTLSamplerMipFilterLinear; break;
    default: rt_fatal("D3D9: mip filter %u is not supported", k.mip);
    }
    sd.sAddressMode = address_mode(k.u, k.border);
    sd.tAddressMode = address_mode(k.v, k.border);
    sd.rAddressMode = address_mode(k.w, k.border);
    sd.borderColor = k.border == 0 ? MTLSamplerBorderColorTransparentBlack
                     : k.border == 0xFF000000u ? MTLSamplerBorderColorOpaqueBlack
                                                : MTLSamplerBorderColorOpaqueWhite;
    sd.maxAnisotropy = k.aniso < 1 ? 1 : (k.aniso > 16 ? 16 : k.aniso);
    sd.lodMinClamp = (float)lmin;
    if (compare) sd.compareFunction = MTLCompareFunctionLessEqual;
    s = [d->mtl newSamplerStateWithDescriptor:sd];
    if (s == nil) rt_fatal("D3D9: sampler state creation failed");
    state_cache_put(d->samplers, &k, s);
    return s;
}

/* The texture bound to sampler slot s for a shader sampler of type st. */
static void bind_texture(D3DDevice *d, uint32_t slot, uint32_t st, bool vertex, uint32_t index)
{
    rt_com_object *o = d->tex[slot];
    id<MTLTexture> view;
    id<MTLSamplerState> smp;
    bool compare = false;
    uint32_t lod = 0;
    if (st == D3DSTT_UNKNOWN) rt_fatal("D3D9: sampler %u has no declared texture type", slot);
    if (o != NULL)
    {
        D3DTexture *t = d3d_tex(o);
        uint32_t tt = t->cube ? D3DSTT_CUBE : t->volume ? D3DSTT_VOLUME : D3DSTT_2D;
        /* Direct3D 9 does not fail a draw whose texture type is not the
           declared sampler type; the samples are undefined. The sampler
           then reads the empty texture of its type: (0, 0, 0, 1).
           UNCONFIRMED: what Windows drivers return. */
        if (st != tt)
        {
            static bool logged;
            if (!logged)
                rt_log("D3D9: sampler %u is a %s sampler but a %s texture is bound; the sampler reads (0, 0, 0, 1)",
                       slot, st == D3DSTT_CUBE ? "cube" : st == D3DSTT_VOLUME ? "volume" : "2D",
                       tt == D3DSTT_CUBE ? "cube" : tt == D3DSTT_VOLUME ? "volume" : "2D");
            logged = true;
            o = NULL;
        }
    }
    if (o == NULL)
        view = st == D3DSTT_CUBE ? d->null_texcube : st == D3DSTT_VOLUME ? d->null_tex3d : d->null_tex2d;
    else
    {
        D3DTexture *t = d3d_tex(o);
        if (t->tex == nil) rt_fatal("D3D9: a system-memory texture is bound to sampler %u", slot);
        if (t->f->depth)
        {
            if (vertex) rt_fatal("D3D9: a depth texture is bound to vertex sampler %u", index);
            compare = true;
        }
        view = d3d_texture_view(t, d->ss[slot][SS_SRGBTEXTURE] != 0);
        lod = t->lod;
        t->last_use = d->serial;
    }
    smp = sampler_for(d, d->ss[slot], lod, compare);
    if (d->es_tex[slot] != view)
    {
        if (vertex) [d->enc setVertexTexture:view atIndex:index];
        else [d->enc setFragmentTexture:view atIndex:index];
        d->es_tex[slot] = view;
    }
    if (d->es_smp[slot] != smp)
    {
        if (vertex) [d->enc setVertexSamplerState:smp atIndex:index];
        else [d->enc setFragmentSamplerState:smp atIndex:index];
        d->es_smp[slot] = smp;
    }
}

/* ---------------------------------------------------------------- draws */

typedef struct draw_info {
    uint32_t type;      /* D3DPRIMITIVETYPE */
    uint32_t prims;
    bool indexed;
    /* Non-indexed: start vertex. Indexed: index buffer, offset, format. */
    uint32_t start;
    id<MTLBuffer> ibuf;
    uint32_t ioff;       /* byte offset of the first index */
    bool idx32;
    int32_t base_vertex;
    /* User pointer vertex data for stream 0 (DrawPrimitiveUP). */
    id<MTLBuffer> up_buf;
    uint32_t up_off, up_stride;
    /* Index data on the host for triangle fans. */
    const uint8_t *ihost;
} draw_info;

static bool prim_count(uint32_t type, uint32_t prims, MTLPrimitiveType *mt, uint32_t *n)
{
    switch (type)
    {
    case D3DPT_POINTLIST: *mt = MTLPrimitiveTypePoint; *n = prims; return true;
    case D3DPT_LINELIST: *mt = MTLPrimitiveTypeLine; *n = prims * 2u; return true;
    case D3DPT_LINESTRIP: *mt = MTLPrimitiveTypeLineStrip; *n = prims + 1u; return true;
    case D3DPT_TRIANGLELIST: *mt = MTLPrimitiveTypeTriangle; *n = prims * 3u; return true;
    case D3DPT_TRIANGLESTRIP: *mt = MTLPrimitiveTypeTriangleStrip; *n = prims + 2u; return true;
    case D3DPT_TRIANGLEFAN: *mt = MTLPrimitiveTypeTriangle; *n = prims + 2u; return true;
    default: return false;
    }
}

/* Render states that the runtime does not implement stop the process when
   a draw uses them. */
static void check_states(D3DDevice *d, const D3DShader *ps, bool points)
{
    uint32_t i;
    if (d->rs[RS_CLIPPLANEENABLE] != 0) rt_fatal("D3D9: user clip planes are not supported (D3DRS_CLIPPLANEENABLE 0x%x)", d->rs[RS_CLIPPLANEENABLE]);
    for (i = 0; i < 8; i++)
        if (d->rs[RS_WRAP0 + i] != 0 || d->rs[RS_WRAP8 + i] != 0) rt_fatal("D3D9: D3DRS_WRAP%u is not supported", i);
    if (d->rs[RS_SHADEMODE] == 1) rt_fatal("D3D9: flat shading is not supported");
    if (d->rs[RS_FILLMODE] == 1) rt_fatal("D3D9: point fill mode is not supported");
    if (d->rs[RS_FOGENABLE] && ps->sh.version < 0x300) rt_fatal("D3D9: fog with a shader model 2 pixel shader is not supported");
    if (d->rs[RS_ZENABLE] == 2) rt_fatal("D3D9: W-buffering is not supported");
    if (points && d->rs[RS_POINTSPRITEENABLE]) rt_fatal("D3D9: point sprites are not supported");
    if (d->npatch_mode != 0 && d3d_f(d->npatch_mode) != 0.0f) rt_fatal("D3D9: N-patches are not supported");
    if (d->pass_samples > 1 && (!d->rs[RS_MULTISAMPLEANTIALIAS] || d->rs[RS_MULTISAMPLEMASK] != 0xFFFFFFFFu))
        rt_fatal("D3D9: D3DRS_MULTISAMPLEANTIALIAS off or a sample mask is not supported");
}

static MTLVertexDescriptor *vertex_desc(D3DDevice *d, const D3DShader *vs, const D3DDecl *dc, const pipe_key *k)
{
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    bool null_used = false;
    int i;
    uint32_t s, e, used = 0; /* bit per stream that an attribute reads */
    (void)d;
    for (i = 0; i < vs->sh.ninputs; i++)
    {
        const d3dsh_io *in = &vs->sh.inputs[i];
        MTLVertexAttributeDescriptor *a = vd.attributes[in->reg];
        for (e = 0; e < dc->n; e++)
            if (dc->el[e].usage == in->usage && dc->el[e].index == in->index) break;
        if (e == dc->n)
        {
            a.format = MTLVertexFormatFloat4;
            a.offset = 0;
            a.bufferIndex = NULL_VERTEX_BUFFER;
            null_used = true;
        }
        else
        {
            uint32_t size;
            a.format = vertex_format(dc->el[e].type, &size);
            if (dc->el[e].offset % 4u) rt_fatal("D3D9: vertex element offset %u is not a multiple of 4", dc->el[e].offset);
            a.offset = dc->el[e].offset;
            a.bufferIndex = dc->el[e].stream;
            if (dc->el[e].stream < 32u) used |= 1u << dc->el[e].stream;
        }
    }
    for (s = 0; s < D3D_MAX_STREAMS; s++)
    {
        MTLVertexBufferLayoutDescriptor *l;
        /* Metal rejects a layout for a buffer that no attribute reads. */
        if (k->step[s] == 0 || !(used & (1u << s))) continue;
        l = vd.layouts[s];
        switch (k->step[s] & 0xFFu)
        {
        case 1:
            l.stepFunction = MTLVertexStepFunctionPerVertex;
            l.stride = k->stride[s];
            break;
        case 2:
            l.stepFunction = MTLVertexStepFunctionPerInstance;
            l.stepRate = k->step[s] >> 8;
            l.stride = k->stride[s];
            break;
        default:
            /* Stride 0: every vertex reads the same data. */
            l.stepFunction = MTLVertexStepFunctionConstant;
            l.stepRate = 0;
            l.stride = 4;
            break;
        }
    }
    if (null_used)
    {
        vd.layouts[NULL_VERTEX_BUFFER].stepFunction = MTLVertexStepFunctionConstant;
        vd.layouts[NULL_VERTEX_BUFFER].stepRate = 0;
        vd.layouts[NULL_VERTEX_BUFFER].stride = 16;
    }
    return vd;
}

static id<MTLDepthStencilState> draw_ds_state(D3DDevice *d)
{
    uint32_t key[DS_KEY_WORDS];
    bool zon = d->rs[RS_ZENABLE] != 0 && d->pass_depth != MTLPixelFormatInvalid;
    bool son = d->rs[RS_STENCILENABLE] != 0 && d->pass_stencil != MTLPixelFormatInvalid;
    bool two = son && d->rs[RS_TWOSIDEDSTENCILMODE] != 0;
    MTLDepthStencilDescriptor *dd;
    memset(key, 0, sizeof key);
    key[0] = zon ? 1u : 0u;
    key[1] = zon ? d->rs[RS_ZFUNC] : 0u;
    key[2] = zon && d->rs[RS_ZWRITEENABLE] ? 1u : 0u;
    if (son)
    {
        key[3] = 1u | (two ? 2u : 0u);
        key[4] = d->rs[RS_STENCILFAIL];
        key[5] = d->rs[RS_STENCILZFAIL];
        key[6] = d->rs[RS_STENCILPASS];
        key[7] = d->rs[RS_STENCILFUNC];
        key[8] = (d->rs[RS_STENCILMASK] & 0xFFu) | (d->rs[RS_STENCILWRITEMASK] & 0xFFu) << 8;
        if (two)
        {
            key[9] = d->rs[RS_CCW_STENCILFAIL] | d->rs[RS_CCW_STENCILZFAIL] << 8;
            key[10] = d->rs[RS_CCW_STENCILPASS] | d->rs[RS_CCW_STENCILFUNC] << 8;
        }
    }
    {
        id<MTLDepthStencilState> st = state_cache_get(d->ds_states, key);
        if (st != nil) return st;
    }
    dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = zon ? cmp_func(key[1]) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = key[2] != 0;
    if (son)
    {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new], *b;
        f.stencilFailureOperation = stencil_op(key[4]);
        f.depthFailureOperation = stencil_op(key[5]);
        f.depthStencilPassOperation = stencil_op(key[6]);
        f.stencilCompareFunction = cmp_func(key[7]);
        f.readMask = key[8] & 0xFFu;
        f.writeMask = (key[8] >> 8) & 0xFFu;
        b = f;
        if (two)
        {
            b = [MTLStencilDescriptor new];
            b.stencilFailureOperation = stencil_op(d->rs[RS_CCW_STENCILFAIL]);
            b.depthFailureOperation = stencil_op(d->rs[RS_CCW_STENCILZFAIL]);
            b.depthStencilPassOperation = stencil_op(d->rs[RS_CCW_STENCILPASS]);
            b.stencilCompareFunction = cmp_func(d->rs[RS_CCW_STENCILFUNC]);
            b.readMask = f.readMask;
            b.writeMask = f.writeMask;
        }
        /* Front faces are clockwise: the D3D9 clockwise stencil state is the
           front state, the D3DRS_CCW_* state is the back state. */
        dd.frontFaceStencil = f;
        dd.backFaceStencil = b;
    }
    return ds_state(d, dd, key, sizeof key);
}

/* Copy shader constants and the defined constants of the shader. Only the
   float registers that the shader can read are sent (d3d_shader.nconst_f;
   the MSL generator stops with an error if the shader reads another one).
   At least one register is sent, because the shader declares the buffer. */
static void bind_constants(D3DDevice *d, const D3DShader *sh, bool ps)
{
    float c[D3DSH_VS_CONST_F][4];
    int32_t ci[D3DSH_CONST_I][4];
    uint32_t cb[D3DSH_CONST_B];
    uint32_t nf = sh->sh.nconst_f;
    int k;
    if (nf > (ps ? D3DSH_PS_CONST_F : D3DSH_VS_CONST_F))
        rt_fatal("D3D9: shader constant count %u is out of range", nf);
    if (nf == 0) nf = 1;
    memcpy(c, ps ? (const void *)d->ps_c : (const void *)d->vs_c, nf * 16u);
    memcpy(ci, ps ? d->ps_i : d->vs_i, sizeof ci);
    memcpy(cb, ps ? d->ps_b : d->vs_b, sizeof cb);
    for (k = 0; k < sh->sh.ndef_f; k++)
        if (sh->sh.def_f[k].reg < nf) memcpy(c[sh->sh.def_f[k].reg], sh->sh.def_f[k].v, 16);
    for (k = 0; k < sh->sh.ndef_i; k++)
        if (sh->sh.def_i[k].reg < D3DSH_CONST_I) memcpy(ci[sh->sh.def_i[k].reg], sh->sh.def_i[k].v, 16);
    for (k = 0; k < sh->sh.ndef_b; k++)
        if (sh->sh.def_b[k].reg < D3DSH_CONST_B) cb[sh->sh.def_b[k].reg] = sh->sh.def_b[k].v[0] ? 1u : 0u;
    if (ps)
    {
        [d->enc setFragmentBytes:c length:nf * 16u atIndex:16];
        [d->enc setFragmentBytes:ci length:sizeof ci atIndex:17];
        [d->enc setFragmentBytes:cb length:sizeof cb atIndex:18];
    }
    else
    {
        [d->enc setVertexBytes:c length:nf * 16u atIndex:16];
        [d->enc setVertexBytes:ci length:sizeof ci atIndex:17];
        [d->enc setVertexBytes:cb length:sizeof cb atIndex:18];
    }
}

/* Encode one draw. Returns D3D_OK or an error for invalid calls. */
static uint32_t do_draw(D3DDevice *d, draw_info *di)
{
    D3DShader *vs, *ps;
    D3DDecl *dc;
    pipe_key k;
    MTLPrimitiveType mt;
    uint32_t n, s, e, i, instances = 1;
    uint16_t dmask = 0;
    MTLRenderPipelineDescriptor *pd;
    MTLViewport vp;
    float vs_misc[4], ps_misc[4];
    bool has_psize = false;
    int j;
    if (di->prims == 0) return D3D_OK;
    if (di->prims > 0x00FFFFFFu || !prim_count(di->type, di->prims, &mt, &n)) return D3DERR_INVALIDCALL;
    if (d->vs == NULL || d->ps == NULL)
        rt_fatal("D3D9: draws without a vertex shader and a pixel shader (fixed-function pipeline) are not supported");
    if (d->decl == NULL) return D3DERR_INVALIDCALL;
    vs = d3d_shader_obj(d->vs);
    ps = d3d_shader_obj(d->ps);
    dc = d3d_decl(d->decl);
    for (j = 0; j < vs->sh.noutputs; j++)
        if (vs->sh.outputs[j].usage == D3DDECLUSAGE_PSIZE && vs->sh.outputs[j].index == 0) has_psize = true;
    if (mt == MTLPrimitiveTypePoint && !has_psize)
        rt_fatal("D3D9: point lists need a vertex shader that writes the point size");
    need_pass(d);
    check_states(d, ps, mt == MTLPrimitiveTypePoint);

    /* Pipeline key. */
    memset(&k, 0, sizeof k);
    k.vs = vs->id;
    k.ps = ps->id;
    k.decl = dc->id;
    if (di->indexed && (d->streams[0].freq & D3DSTREAMSOURCE_INDEXEDDATA)) instances = d->streams[0].freq & 0x3FFFFFFFu;
    if (instances == 0) instances = 1;
    for (e = 0; e < dc->n; e++)
    {
        uint32_t st = dc->el[e].stream, stride, freq;
        if (st == 0 && di->up_buf != nil)
        {
            stride = di->up_stride;
            freq = 0;
        }
        else
        {
            if (d->streams[st].buf == NULL) rt_fatal("D3D9: stream %u has no vertex buffer", st);
            stride = d->streams[st].stride;
            freq = d->streams[st].freq;
        }
        if (stride % 4u) rt_fatal("D3D9: vertex stride %u is not a multiple of 4", stride);
        k.stride[st] = stride;
        if (stride == 0)
            k.step[st] = 3;
        else if (instances > 1 && (freq & D3DSTREAMSOURCE_INSTANCEDATA))
            k.step[st] = 2u | ((freq & 0x3FFFFFFFu) ? (freq & 0x3FFFFFFFu) : 1u) << 8;
        else
            k.step[st] = 1;
    }
    pass_key(d, &k);
    if (d->rs[RS_ALPHABLENDENABLE])
    {
        k.blend = 1;
        blend_pair(d->rs[RS_SRCBLEND], d->rs[RS_DESTBLEND], &k.src, &k.dst);
        k.op = d->rs[RS_BLENDOP];
        if (d->rs[RS_SEPARATEALPHABLENDENABLE])
        {
            blend_pair(d->rs[RS_SRCBLENDALPHA], d->rs[RS_DESTBLENDALPHA], &k.src_a, &k.dst_a);
            k.op_a = d->rs[RS_BLENDOPALPHA];
        }
        else
        {
            k.src_a = k.src;
            k.dst_a = k.dst;
            k.op_a = k.op;
        }
    }
    k.wmask[0] = d->rs[RS_COLORWRITEENABLE] & 0xFu;
    k.wmask[1] = d->rs[RS_COLORWRITEENABLE1] & 0xFu;
    k.wmask[2] = d->rs[RS_COLORWRITEENABLE2] & 0xFu;
    k.wmask[3] = d->rs[RS_COLORWRITEENABLE3] & 0xFu;
    for (s = 0; s < D3D_PS_SAMPLERS; s++)
        if (ps->sh.sampler_type[s] == D3DSTT_2D && d->tex[s] != NULL && d3d_tex(d->tex[s])->f->depth)
            dmask = (uint16_t)(dmask | (1u << s));
    k.depth_compare = dmask;
    {
        id<MTLRenderPipelineState> p = state_cache_get(d->pipelines, &k);
        if (p == nil)
        {
            pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = vs_function(d, vs);
            pd.fragmentFunction = ps_function(d, ps, vs, dmask);
            pd.vertexDescriptor = vertex_desc(d, vs, dc, &k);
            p = make_pipeline(d, &k, pd);
        }
        if (!d->es_valid || d->es_pipe != p)
        {
            [d->enc setRenderPipelineState:p];
            d->es_pipe = p;
        }
    }
    {
        id<MTLDepthStencilState> dss = draw_ds_state(d);
        uint32_t sref = d->rs[RS_STENCILREF] & 0xFFu, cull = d->rs[RS_CULLMODE];
        uint32_t fill = d->rs[RS_FILLMODE] == 2 ? 1u : 0u, slope = d->rs[RS_SLOPESCALEDEPTHBIAS];
        if (cull < 1 || cull > 3) rt_fatal("D3D9: cull mode %u is not valid", cull);
        if (!d->es_valid || d->es_ds != dss)
        {
            [d->enc setDepthStencilState:dss];
            d->es_ds = dss;
        }
        if (!d->es_valid || d->es_sref != sref)
        {
            [d->enc setStencilReferenceValue:sref];
            d->es_sref = sref;
        }
        if (!d->es_valid || d->es_blendf != d->rs[RS_BLENDFACTOR])
        {
            float bf[4];
            color_of(d->rs[RS_BLENDFACTOR], bf);
            [d->enc setBlendColorRed:bf[0] green:bf[1] blue:bf[2] alpha:bf[3]];
            d->es_blendf = d->rs[RS_BLENDFACTOR];
        }
        if (!d->es_valid || d->es_cull != cull)
        {
            /* D3D9 culls clockwise (2) or counterclockwise (3) faces; front
               faces are clockwise. */
            [d->enc setCullMode:cull == 1 ? MTLCullModeNone : cull == 2 ? MTLCullModeFront : MTLCullModeBack];
            d->es_cull = cull;
        }
        if (!d->es_valid)
            [d->enc setFrontFacingWinding:MTLWindingClockwise];
        if (!d->es_valid || d->es_fill != fill)
        {
            [d->enc setTriangleFillMode:fill ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
            d->es_fill = fill;
        }
        if (!d->es_valid || d->es_slope != slope)
        {
            [d->enc setDepthBias:0 slopeScale:d3d_f(slope) clamp:0];
            d->es_slope = slope;
        }
    }
    vp.originX = (double)d->viewport[0] * d->pass_scale;
    vp.originY = (double)d->viewport[1] * d->pass_scale;
    vp.width = (double)d->viewport[2] * d->pass_scale;
    vp.height = (double)d->viewport[3] * d->pass_scale;
    vp.znear = d3d_f(d->viewport[4]);
    vp.zfar = d3d_f(d->viewport[5]);
    if (!d->es_valid || memcmp(&d->es_vp, &vp, sizeof vp) != 0)
    {
        [d->enc setViewport:vp];
        d->es_vp = vp;
    }
    {
        int32_t r[4] = { 0, 0, (int32_t)d->pass_w, (int32_t)d->pass_h };
        int32_t sc[4], si;
        MTLScissorRect sr;
        for (si = 0; si < 4; si++) sc[si] = d->scissor[si] * (int32_t)d->pass_scale;
        if (d->rs[RS_SCISSORTESTENABLE] && !clip_rect(r, sc)) return D3D_OK; /* nothing is drawn */
        if (!d->rs[RS_SCISSORTESTENABLE])
        {
            int32_t full[4] = { 0, 0, (int32_t)d->pass_w, (int32_t)d->pass_h };
            memcpy(r, full, sizeof r);
        }
        sr.x = (NSUInteger)r[0];
        sr.y = (NSUInteger)r[1];
        sr.width = (NSUInteger)(r[2] - r[0]);
        sr.height = (NSUInteger)(r[3] - r[1]);
        if (!d->es_valid || memcmp(&d->es_sr, &sr, sizeof sr) != 0)
        {
            [d->enc setScissorRect:sr];
            d->es_sr = sr;
        }
    }
    vs_misc[0] = d->viewport[2] ? -1.0f / (float)d->viewport[2] : 0.0f;
    vs_misc[1] = d->viewport[3] ? 1.0f / (float)d->viewport[3] : 0.0f;
    vs_misc[2] = d3d_f(d->rs[RS_DEPTHBIAS]);
    vs_misc[3] = 0.0f;
    if (!d->es_valid || memcmp(d->es_vs_misc, vs_misc, sizeof vs_misc) != 0)
    {
        [d->enc setVertexBytes:vs_misc length:sizeof vs_misc atIndex:19];
        memcpy(d->es_vs_misc, vs_misc, sizeof vs_misc);
    }
    ps_misc[0] = d->rs[RS_ALPHATESTENABLE] ? (float)d->rs[RS_ALPHAFUNC] : 0.0f;
    ps_misc[1] = (float)(d->rs[RS_ALPHAREF] & 0xFFu) / 255.0f;
    ps_misc[2] = 0.0f;
    ps_misc[3] = 0.0f;
    if (!d->es_valid || memcmp(d->es_ps_misc, ps_misc, sizeof ps_misc) != 0)
    {
        [d->enc setFragmentBytes:ps_misc length:sizeof ps_misc atIndex:19];
        memcpy(d->es_ps_misc, ps_misc, sizeof ps_misc);
    }
    /* The constant data of a shader depends only on the shader (its defined
       constants and nconst_f) and on the device constants. */
    if (!d->es_valid || d->es_vs_shader != vs->id || d->es_vs_ver != d->vs_const_ver)
    {
        bind_constants(d, vs, false);
        d->es_vs_shader = vs->id;
        d->es_vs_ver = d->vs_const_ver;
    }
    if (!d->es_valid || d->es_ps_shader != ps->id || d->es_ps_ver != d->ps_const_ver)
    {
        bind_constants(d, ps, true);
        d->es_ps_shader = ps->id;
        d->es_ps_ver = d->ps_const_ver;
    }
    for (s = 0; s < D3D_PS_SAMPLERS; s++)
        if (ps->sh.sampler_type[s] != 0) bind_texture(d, s, ps->sh.sampler_type[s], false, s);
    for (s = 0; s < D3D_VS_SAMPLERS; s++)
        if (vs->sh.sampler_type[s] != 0) bind_texture(d, D3D_PS_SAMPLERS + s, vs->sh.sampler_type[s], true, s);

    /* Vertex buffers. */
    for (s = 0; s < D3D_MAX_STREAMS; s++)
    {
        id<MTLBuffer> vb;
        NSUInteger off;
        if (k.step[s] == 0) continue;
        if (s == 0 && di->up_buf != nil)
        {
            vb = di->up_buf;
            off = di->up_off;
        }
        else
        {
            D3DBuffer *b = d3d_buf(d->streams[s].buf);
            if (d->streams[s].offset % 4u) rt_fatal("D3D9: stream %u offset %u is not a multiple of 4", s, d->streams[s].offset);
            vb = b->buf;
            off = d->streams[s].offset;
            b->last_use = d->serial;
        }
        if (d->es_vb[s] != vb)
        {
            [d->enc setVertexBuffer:vb offset:off atIndex:s];
            d->es_vb[s] = vb;
            d->es_vb_off[s] = off;
        }
        else if (d->es_vb_off[s] != off)
        {
            [d->enc setVertexBufferOffset:off atIndex:s];
            d->es_vb_off[s] = off;
        }
    }
    if (!d->es_null_vb)
    {
        [d->enc setVertexBuffer:d->null_vertex offset:0 atIndex:NULL_VERTEX_BUFFER];
        d->es_null_vb = true;
    }

    if (di->type == D3DPT_TRIANGLEFAN)
    {
        /* Metal has no fans: draw a list with generated indices. */
        uint32_t *idx = malloc((size_t)di->prims * 3u * 4u), off;
        id<MTLBuffer> ub;
        if (idx == NULL) rt_fatal("out of host memory (triangle fan)");
        for (i = 0; i < di->prims; i++)
        {
            uint32_t v[3] = { 0, i + 1u, i + 2u }, t;
            for (t = 0; t < 3; t++)
            {
                if (!di->indexed)
                    idx[3 * i + t] = di->start + v[t];
                else if (di->idx32)
                    memcpy(&idx[3 * i + t], di->ihost + 4u * v[t], 4);
                else
                {
                    uint16_t x;
                    memcpy(&x, di->ihost + 2u * v[t], 2);
                    idx[3 * i + t] = x;
                }
            }
        }
        ub = d3d_upload(d, idx, di->prims * 12u, &off);
        free(idx);
        [d->enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:di->prims * 3u indexType:MTLIndexTypeUInt32
                          indexBuffer:ub indexBufferOffset:off instanceCount:instances
                           baseVertex:di->indexed ? di->base_vertex : 0 baseInstance:0];
    }
    else if (di->indexed)
    {
        [d->enc drawIndexedPrimitives:mt indexCount:n indexType:di->idx32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                          indexBuffer:di->ibuf indexBufferOffset:di->ioff instanceCount:instances
                           baseVertex:di->base_vertex baseInstance:0];
    }
    else
        [d->enc drawPrimitives:mt vertexStart:di->start vertexCount:n];
    d->enc_draws = true;
    d->es_valid = true;
    return D3D_OK;
}

/* Index data of an indexed draw from the bound index buffer: index count n
   from index start. */
static uint32_t index_source(D3DDevice *d, draw_info *di, uint32_t start, uint32_t count)
{
    D3DBuffer *b;
    uint32_t isize, off, bytes;
    if (d->ib == NULL) return D3DERR_INVALIDCALL;
    b = d3d_buf(d->ib);
    di->idx32 = b->format_or_fvf == D3DFMT_INDEX32;
    isize = di->idx32 ? 4u : 2u;
    if (start > b->size / isize || count > b->size / isize - start) return D3DERR_INVALIDCALL;
    off = start * isize;
    bytes = count * isize;
    di->ihost = (const uint8_t *)b->buf.contents + off;
    if (off % 4u)
    {
        /* UNCONFIRMED: Metal needs a 4-byte aligned index buffer offset;
           the indices are copied to aligned upload memory. */
        di->ibuf = d3d_upload(d, di->ihost, bytes, &di->ioff);
    }
    else
    {
        di->ibuf = b->buf;
        di->ioff = off;
    }
    b->last_use = d->serial;
    return D3D_OK;
}

/* HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) */
void rt_com_D3DDevice9_DrawPrimitive(_cpu *cpu)
{
    D3DDevice *d = d3d_device_of(ARG(0));
    draw_info di = { 0 };
    di.type = ARG(1);
    di.start = ARG(2);
    di.prims = ARG(3);
    d3d_lock(d);
    @autoreleasepool
    {
        RET(do_draw(d, &di));
    }
    d3d_unlock(d);
}

/* HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base_vertex, UINT min_index, UINT num_vertices,
   UINT start_index, UINT count) */
void rt_com_D3DDevice9_DrawIndexedPrimitive(_cpu *cpu)
{
    D3DDevice *d = d3d_device_of(ARG(0));
    draw_info di = { 0 };
    MTLPrimitiveType mt;
    uint32_t n, r;
    di.type = ARG(1);
    di.base_vertex = (int32_t)ARG(2);
    di.prims = ARG(6);
    di.indexed = true;
    d3d_lock(d);
    @autoreleasepool
    {
        if (di.prims == 0)
            r = D3D_OK;
        else if (di.prims > 0x00FFFFFFu || !prim_count(di.type, di.prims, &mt, &n))
            r = D3DERR_INVALIDCALL;
        else
        {
            r = index_source(d, &di, ARG(5), n);
            if (r == D3D_OK) r = do_draw(d, &di);
        }
        RET(r);
    }
    d3d_unlock(d);
}

/* The stream 0 state after a user pointer draw: no vertex buffer. */
static void reset_stream0(D3DDevice *d)
{
    if (d->streams[0].buf != NULL) rt_com_release(d->streams[0].buf);
    d->streams[0].buf = NULL;
    d->streams[0].offset = 0;
    d->streams[0].stride = 0;
}

/* HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void *data, UINT stride) */
void rt_com_D3DDevice9_DrawPrimitiveUP(_cpu *cpu)
{
    D3DDevice *d = d3d_device_of(ARG(0));
    draw_info di = { 0 };
    MTLPrimitiveType mt;
    uint32_t n, data = ARG(3), stride = ARG(4), r;
    di.type = ARG(1);
    di.prims = ARG(2);
    d3d_lock(d);
    @autoreleasepool
    {
        if (di.prims == 0)
            r = D3D_OK;
        else if (data == 0 || stride == 0 || di.prims > 0x00FFFFFFu || !prim_count(di.type, di.prims, &mt, &n) ||
                 (uint64_t)n * stride > 0x10000000u)
            r = D3DERR_INVALIDCALL;
        else
        {
            di.up_buf = d3d_upload(d, G2H(data), n * stride, &di.up_off);
            di.up_stride = stride;
            r = do_draw(d, &di);
        }
        if (r == D3D_OK) reset_stream0(d);
        RET(r);
    }
    d3d_unlock(d);
}

/* HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT min_index, UINT num_vertices, UINT count,
   const void *indices, D3DFORMAT index_format, const void *vertices, UINT stride) */
void rt_com_D3DDevice9_DrawIndexedPrimitiveUP(_cpu *cpu)
{
    D3DDevice *d = d3d_device_of(ARG(0));
    draw_info di = { 0 };
    MTLPrimitiveType mt;
    uint32_t n, minv = ARG(2), nv = ARG(3), idx = ARG(5), fmt = ARG(6), verts = ARG(7), stride = ARG(8), r;
    di.type = ARG(1);
    di.prims = ARG(4);
    di.indexed = true;
    d3d_lock(d);
    @autoreleasepool
    {
        if (di.prims == 0)
            r = D3D_OK;
        else if (idx == 0 || verts == 0 || stride == 0 || (fmt != D3DFMT_INDEX16 && fmt != D3DFMT_INDEX32) ||
                 di.prims > 0x00FFFFFFu || !prim_count(di.type, di.prims, &mt, &n) ||
                 (uint64_t)(minv + (uint64_t)nv) * stride > 0x10000000u)
            r = D3DERR_INVALIDCALL;
        else
        {
            di.idx32 = fmt == D3DFMT_INDEX32;
            di.ihost = G2H(idx);
            di.ibuf = d3d_upload(d, di.ihost, n * (di.idx32 ? 4u : 2u), &di.ioff);
            di.up_buf = d3d_upload(d, G2H(verts), (minv + nv) * stride, &di.up_off);
            di.up_stride = stride;
            r = do_draw(d, &di);
        }
        if (r == D3D_OK)
        {
            reset_stream0(d);
            if (d->ib != NULL) rt_com_release(d->ib);
            d->ib = NULL;
        }
        RET(r);
    }
    d3d_unlock(d);
}

/* ---------------------------------------------------------------- StretchRect, ColorFill */

static bool read_rect(uint32_t p, uint32_t w, uint32_t h, int32_t r[4])
{
    if (p == 0)
    {
        r[0] = 0;
        r[1] = 0;
        r[2] = (int32_t)w;
        r[3] = (int32_t)h;
        return true;
    }
    r[0] = (int32_t)rd32(p);
    r[1] = (int32_t)rd32(p + 4);
    r[2] = (int32_t)rd32(p + 8);
    r[3] = (int32_t)rd32(p + 12);
    return r[0] >= 0 && r[1] >= 0 && r[0] < r[2] && r[1] < r[3] && (uint32_t)r[2] <= w && (uint32_t)r[3] <= h;
}

/* Resolve a multisampled texture into a single-sample texture of the same
   size and format. */
static void resolve(D3DDevice *d, id<MTLTexture> src, id<MTLTexture> dst, NSUInteger slice, NSUInteger level)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    id<MTLRenderCommandEncoder> e;
    d3d_end_pass(d);
    rp.colorAttachments[0].texture = src;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    rp.colorAttachments[0].resolveTexture = dst;
    rp.colorAttachments[0].resolveSlice = slice;
    rp.colorAttachments[0].resolveLevel = level;
    e = [d->cb renderCommandEncoderWithDescriptor:rp];
    if (e == nil) rt_fatal("D3D9: resolve encoder creation failed");
    [e endEncoding];
}

/* HRESULT StretchRect(IDirect3DSurface9 *src, const RECT *src_rect, IDirect3DSurface9 *dst, const RECT *dst_rect,
   D3DTEXTUREFILTERTYPE filter) */
void rt_com_D3DDevice9_StretchRect(_cpu *cpu)
{
    D3DDevice *d = d3d_device_of(ARG(0));
    uint32_t filter = ARG(5);
    D3DSurface *src, *dst;
    int32_t sr[4], dr[4];
    NSUInteger ss, sl, ds, dl;
    id<MTLTexture> st, dt;
    uint32_t r = D3D_OK;
    if (ARG(1) == 0 || ARG(3) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    src = d3d_surf(d3d_surface_from_ptr(ARG(1)));
    dst = d3d_surf(d3d_surface_from_ptr(ARG(3)));
    d3d_check_unscaled(src, "StretchRect");
    d3d_check_unscaled(dst, "StretchRect");
    d3d_lock(d);
    @autoreleasepool
    {
        st = d3d_surface_texture(src, &ss, &sl);
        dt = d3d_surface_texture(dst, &ds, &dl);
        if (st == nil || dt == nil || src->pool != D3DPOOL_DEFAULT || dst->pool != D3DPOOL_DEFAULT ||
            !read_rect(ARG(2), src->width, src->height, sr) || !read_rect(ARG(4), dst->width, dst->height, dr) ||
            filter > 3 || dst->msaa > 0 || src->f->depth || dst->f->depth)
            r = D3DERR_INVALIDCALL;
        else if (st.sampleCount > 1)
        {
            if (sr[0] != 0 || sr[1] != 0 || (uint32_t)sr[2] != src->width || (uint32_t)sr[3] != src->height ||
                memcmp(sr, dr, sizeof sr) != 0 || src->width != dst->width || src->height != dst->height ||
                st.pixelFormat != dt.pixelFormat)
                rt_fatal("D3D9: StretchRect from a multisampled surface supports only a full resolve of the same size "
                         "and format");
            resolve(d, st, dt, ds, dl);
        }
        else if (sr[2] - sr[0] == dr[2] - dr[0] && sr[3] - sr[1] == dr[3] - dr[1] && st.pixelFormat == dt.pixelFormat)
        {
            id<MTLBlitCommandEncoder> be;
            d3d_end_pass(d);
            be = [d->cb blitCommandEncoder];
            [be copyFromTexture:st sourceSlice:ss sourceLevel:sl
                   sourceOrigin:MTLOriginMake((NSUInteger)sr[0], (NSUInteger)sr[1], 0)
                     sourceSize:MTLSizeMake((NSUInteger)(sr[2] - sr[0]), (NSUInteger)(sr[3] - sr[1]), 1)
                      toTexture:dt destinationSlice:ds destinationLevel:dl
              destinationOrigin:MTLOriginMake((NSUInteger)dr[0], (NSUInteger)dr[1], 0)];
            [be endEncoding];
        }
        else if (!(dt.usage & MTLTextureUsageRenderTarget) || src->f->block || dst->f->block)
            r = D3DERR_INVALIDCALL;
        else
        {
            /* Scaled or converting copy: draw the source rectangle. */
            pipe_key k;
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            id<MTLRenderCommandEncoder> e;
            id<MTLTexture> sv;
            float uv[4];
            MTLViewport vp;
            d3d_end_pass(d);
            memset(&k, 0, sizeof k);
            k.kind = 2;
            k.color[0] = (uint32_t)dt.pixelFormat;
            k.wmask[0] = 0xF;
            k.samples = 1;
            pd.vertexFunction = function_of(d->builtin_lib, "blit_vs");
            pd.fragmentFunction = function_of(d->builtin_lib, "blit_fs");
            sv = [st newTextureViewWithPixelFormat:st.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(sl, 1)
                                            slices:NSMakeRange(ss, 1)];
            if (sv == nil) rt_fatal("D3D9: StretchRect source view creation failed");
            rp.colorAttachments[0].texture = dt;
            rp.colorAttachments[0].slice = ds;
            rp.colorAttachments[0].level = dl;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            e = [d->cb renderCommandEncoderWithDescriptor:rp];
            if (e == nil) rt_fatal("D3D9: StretchRect encoder creation failed");
            [e setRenderPipelineState:make_pipeline(d, &k, pd)];
            uv[0] = (float)sr[0] / (float)src->width;
            uv[1] = (float)sr[1] / (float)src->height;
            uv[2] = (float)sr[2] / (float)src->width;
            uv[3] = (float)sr[3] / (float)src->height;
            [e setVertexBytes:uv length:sizeof uv atIndex:0];
            [e setFragmentTexture:sv atIndex:0];
            [e setFragmentSamplerState:filter >= 2 ? d->present_linear : d->present_point atIndex:0];
            vp.originX = dr[0];
            vp.originY = dr[1];
            vp.width = dr[2] - dr[0];
            vp.height = dr[3] - dr[1];
            vp.znear = 0;
            vp.zfar = 1;
            [e setViewport:vp];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
        }
        if (r == D3D_OK)
        {
            d3d_use_surface(src);
            d3d_use_surface(dst);
        }
    }
    RET(r);
    d3d_unlock(d);
}

/* HRESULT ColorFill(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR color) */
void rt_com_D3DDevice9_ColorFill(_cpu *cpu)
{
    D3DDevice *d = d3d_device_of(ARG(0));
    D3DSurface *s;
    int32_t r[4];
    uint32_t color = ARG(3), res = D3D_OK;
    NSUInteger slice, level;
    id<MTLTexture> t;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    s = d3d_surf(d3d_surface_from_ptr(ARG(1)));
    d3d_check_unscaled(s, "ColorFill");
    d3d_lock(d);
    @autoreleasepool
    {
        t = d3d_surface_texture(s, &slice, &level);
        if (t == nil || s->pool != D3DPOOL_DEFAULT || s->f->depth || s->f->block || !read_rect(ARG(2), s->width, s->height, r))
            res = D3DERR_INVALIDCALL;
        else if (t.usage & MTLTextureUsageRenderTarget)
        {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            id<MTLRenderCommandEncoder> e;
            float rgba[4];
            bool full = r[0] == 0 && r[1] == 0 && (uint32_t)r[2] == s->width && (uint32_t)r[3] == s->height;
            d3d_end_pass(d);
            color_of(color, rgba);
            rp.colorAttachments[0].texture = t;
            rp.colorAttachments[0].slice = slice;
            rp.colorAttachments[0].level = level;
            rp.colorAttachments[0].loadAction = full ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            e = [d->cb renderCommandEncoderWithDescriptor:rp];
            if (e == nil) rt_fatal("D3D9: ColorFill encoder creation failed");
            if (!full)
            {
                pipe_key k;
                MTLScissorRect sr = { (NSUInteger)r[0], (NSUInteger)r[1], (NSUInteger)(r[2] - r[0]),
                                      (NSUInteger)(r[3] - r[1]) };
                float zp[4] = { 0, 0, 0, 0 };
                memset(&k, 0, sizeof k);
                k.color[0] = (uint32_t)t.pixelFormat;
                k.samples = (uint32_t)t.sampleCount;
                [e setRenderPipelineState:clear_pipeline(d, &k, D3DCLEAR_TARGET)];
                [e setFragmentBytes:rgba length:sizeof rgba atIndex:0];
                [e setVertexBytes:zp length:sizeof zp atIndex:0];
                [e setScissorRect:sr];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [e endEncoding];
            d3d_use_surface(s);
        }
        else if (s->f->bytes == 4 && s->f->conv == D3DCONV_NONE &&
                 (s->f->pf == MTLPixelFormatBGRA8Unorm || s->f->pf == MTLPixelFormatRGBA8Unorm))
        {
            /* A plain surface in shared storage: fill on the CPU. */
            uint32_t w = (uint32_t)(r[2] - r[0]), h = (uint32_t)(r[3] - r[1]), x, px = color;
            uint32_t *row = malloc((size_t)w * 4u);
            if (row == NULL) rt_fatal("out of host memory (ColorFill)");
            if (s->f->pf == MTLPixelFormatRGBA8Unorm)
                px = (color & 0xFF00FF00u) | ((color >> 16) & 0xFFu) | ((color & 0xFFu) << 16);
            for (x = 0; x < w; x++) row[x] = px;
            if (s->last_use != 0) d3d_wait_serial(d, s->last_use);
            for (x = 0; x < h; x++)
                [t replaceRegion:MTLRegionMake2D((NSUInteger)r[0], (NSUInteger)r[1] + x, w, 1) mipmapLevel:level
                           slice:slice withBytes:row bytesPerRow:w * 4u bytesPerImage:w * 4u];
            free(row);
        }
        else
            rt_fatal("D3D9: ColorFill of a non-render-target surface of format 0x%x is not supported", s->fmt);
    }
    RET(res);
    d3d_unlock(d);
}

/* ---------------------------------------------------------------- present */

/* Frame statistics: with the environment variable RT_PERF_LOG=1, the
   runtime logs the frame rate, the longest frame time and the process
   CPU time every 5 seconds of presented frames. */
static void perf_frame(void)
{
    static int enabled = -1;
    static double window_start, last, worst;
    static uint32_t frames;
    struct timespec ts;
    double now, cpu_now;
    static double cpu_start;
    if (enabled < 0)
    {
        const char *v = getenv("RT_PERF_LOG");
        enabled = v != NULL && strcmp(v, "1") == 0;
    }
    if (!enabled) return;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) rt_fatal("clock_gettime failed");
    now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) rt_fatal("clock_gettime failed");
    cpu_now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    if (frames == 0 && window_start == 0.0)
    {
        window_start = last = now;
        cpu_start = cpu_now;
        return;
    }
    if (now - last > worst) worst = now - last;
    last = now;
    frames++;
    if (now - window_start >= 5.0)
    {
        double span = now - window_start;
        rt_log("perf: %.1f fps, longest frame %.1f ms, CPU %.0f%% of one core", (double)frames / span, worst * 1000.0,
               (cpu_now - cpu_start) / span * 100.0);
        window_start = now;
        cpu_start = cpu_now;
        frames = 0;
        worst = 0.0;
    }
}

void d3d_present(D3DDevice *d)
{
    @autoreleasepool
    {
        D3DSurface *bb = d3d_surf(d->backbuffer);
        id<MTLTexture> src = bb->tex;
        CGSize bounds, size;
        CGFloat scale;
        float headroom;
        id<CAMetalDrawable> dr;
        uint32_t slot;
        uint64_t old;
        d3d_end_pass(d);
        if (src.sampleCount > 1)
        {
            if (d->present_resolve == nil || d->present_resolve.width != src.width ||
                d->present_resolve.height != src.height || d->present_resolve.pixelFormat != src.pixelFormat)
            {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
                                                                                               width:src.width
                                                                                              height:src.height
                                                                                           mipmapped:NO];
                td.storageMode = MTLStorageModePrivate;
                td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
                d->present_resolve = [d->mtl newTextureWithDescriptor:td];
                if (d->present_resolve == nil) rt_fatal("D3D9: out of video memory (back buffer resolve)");
            }
            resolve(d, src, d->present_resolve, 0, 0);
            src = d->present_resolve;
        }
        bounds = d->layer.bounds.size;
        scale = d->layer.contentsScale;
        size = CGSizeMake(floor(bounds.width * scale), floor(bounds.height * scale));
        dr = nil;
        headroom = 1.0f;
        if (size.width >= 1 && size.height >= 1 && !rt_cocoa_headless())
        {
            headroom = d3d_post_hdr_begin(d);
            if (!CGSizeEqualToSize(d->layer.drawableSize, size)) d->layer.drawableSize = size;
            dr = [d->layer nextDrawable];
        }
        if (dr != nil)
        {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            id<MTLRenderCommandEncoder> e;
            MTLViewport vp;
            struct { uint32_t use_lut, hdr; float headroom; uint32_t src_linear; } pp_args = { 0, 0, 1.0f, 0 };
            double dw = size.width, dh = size.height, bw = bb->width, bh = bb->height;
            MTLPixelFormat dfmt = dr.texture.pixelFormat;
            if (d->pp[PP_WINDOWED])
            {
                /* Windowed: the back buffer fills the client area. */
                vp.originX = 0;
                vp.originY = 0;
                vp.width = dw;
                vp.height = dh;
            }
            else
            {
                /* Full screen: the window covers the display at its own mode;
                   the back buffer is scaled with its aspect ratio kept. */
                double sc = fmin(dw / bw, dh / bh);
                vp.width = floor(bw * sc);
                vp.height = floor(bh * sc);
                vp.originX = floor((dw - vp.width) / 2);
                vp.originY = floor((dh - vp.height) / 2);
                pp_args.use_lut = d->gamma_set && d->gamma_tex != nil;
            }
            vp.znear = 0;
            vp.zfar = 1;
            /* Mac enhancements: SMAA, MetalFX upscaling, sharpening. */
            src = d3d_post_process(d, src, (uint32_t)vp.width, (uint32_t)vp.height);
            if (dfmt != MTLPixelFormatBGRA8Unorm && dfmt != MTLPixelFormatRGBA16Float)
                rt_fatal("D3D9: the drawable has the pixel format %lu (expected BGRA8Unorm or RGBA16Float)", (unsigned long)dfmt);
            pp_args.hdr = dfmt == MTLPixelFormatRGBA16Float;
            pp_args.headroom = headroom;
            pp_args.src_linear = src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB || src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB;
            rp.colorAttachments[0].texture = dr.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            e = [d->cb renderCommandEncoderWithDescriptor:rp];
            if (e == nil) rt_fatal("D3D9: present encoder creation failed");
            [e setRenderPipelineState:pp_args.hdr ? d->present_pso_hdr : d->present_pso];
            [e setViewport:vp];
            [e setFragmentTexture:src atIndex:0];
            [e setFragmentTexture:pp_args.use_lut ? d->gamma_tex : d->null_tex2d atIndex:1];
            [e setFragmentSamplerState:(vp.width == (double)src.width && vp.height == (double)src.height) ? d->present_point : d->present_linear atIndex:0];
            [e setFragmentBytes:&pp_args length:sizeof pp_args atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
            [d->cb presentDrawable:dr];
        }
        else
        {
            static bool logged;
            if (!logged && !rt_cocoa_headless())
                rt_log("D3D9: Present: the window has no drawable (zero size); the frame is not shown");
            logged = true;
        }
        bb->last_use = d->serial;
        /* At most D3D_FRAMES_IN_FLIGHT frames are queued. */
        slot = d->present_count % D3D_FRAMES_IN_FLIGHT;
        old = d->present_serial[slot];
        d->present_serial[slot] = d->serial;
        d->present_count++;
        d3d_flush(d);
        if (old != 0) d3d_wait_serial(d, old);
        perf_frame();
    }
}

void d3d_init_caches(D3DDevice *d)
{
    d->pipelines = state_cache_new(sizeof(pipe_key), MAX_PIPELINES, "pipeline");
    d->ds_states = state_cache_new(DS_KEY_WORDS * 4u, MAX_DS_STATES, "depth-stencil state");
    d->samplers = state_cache_new(sizeof(samp_key), MAX_SAMPLERS, "sampler");
}
