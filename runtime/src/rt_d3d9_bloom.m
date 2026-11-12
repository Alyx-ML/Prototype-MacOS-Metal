/*
 * Dual Kawase bloom for the Mac enhancement setting RT_ENH_BLOOM
 * (rt_enh.h). The method is the dual filter of Marius Bjørge ("Bandwidth-
 * Efficient Rendering", SIGGRAPH 2015): a chain of half-size passes down,
 * then a chain of double-size passes up. Each down pass reads five
 * bilinear samples (the center with weight 4 and four diagonal corners);
 * each up pass reads eight bilinear samples (four on the axes with weight 1
 * and four on the diagonals with weight 2). The result is a wide, smooth
 * blur with a small number of texture reads.
 *
 * The game's bloom (the shader names come from the constant tables):
 *   1. bright pass (D3DSH_ROLE_BLOOM_BRIGHT): the scene into a quarter-size
 *      target (640x400 for a 2560x1600 scene);
 *   2. glow sprites into the same size of target;
 *   3. an 11-tap blur (D3DSH_ROLE_BLOOM_BLUR) in four draws;
 *   4. the composite (ct_bloom_sampler): scene + BloomedScene *
 *      BloomStrength.
 * With the setting on, d3d_bloom_draw keeps the render target of the
 * bright pass, skips the draws of the blur shader and, before the
 * composite, writes the dual Kawase blur of the bright-pass target into
 * the BloomedScene texture of the composite. The composite then adds it
 * with the game's BloomStrength. Five levels down (1/2 to 1/32 of the
 * bright-pass size) and five levels up; the intermediate textures are
 * RGBA16Float.
 *
 * Changes to the plain dual filter:
 *   - The first down pass is the 13-tap filter of Jorge Jimenez ("Next
 *     Generation Post Processing in Call of Duty: Advanced Warfare",
 *     SIGGRAPH 2014): five boxes of four bilinear samples, each box
 *     weighted by 1 / (1 + luma) (the Karis average). A small, very bright
 *     pixel thus cannot make the glow flicker.
 *   - The up passes add: each level is its own blur plus the up-sampled
 *     levels below it (additive blend into the level). The output is the
 *     sum of the five levels divided by five, so the energy is the same as
 *     one level, and the glow falls off with distance from a light like a
 *     sum of blurs of increasing size.
 *
 * This code has not run (UNCONFIRMED): that the glow sprites go into the
 * bright-pass target, that the blur draws write only the BloomedScene
 * texture, and how the brightness compares with the game's blur.
 */
#include "rt_d3d9_int.h"
#include "rt_enh.h"

#define BLOOM_LEVELS 5

static const char bloom_src[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct BV { float4 pos [[position]]; float2 uv; };\n"
    "vertex BV bloom_vs(uint vid [[vertex_id]])\n"
    "{\n"
    "    float2 q = float2(float((vid << 1) & 2u), float(vid & 2u));\n"
    "    BV o;\n"
    "    o.pos = float4(q * 2.0 - 1.0, 0.0, 1.0);\n"
    "    o.uv = float2(q.x, 1.0 - q.y);\n"
    "    return o;\n"
    "}\n"
    "constexpr sampler bs(filter::linear, address::clamp_to_edge);\n"
    /* hp: half a texel of the output (bloom_down, bloom_up) or one texel of
       the input (bloom_down13); scale: the multiplier of the result. */
    "struct BP { float2 hp; float scale; float pad; };\n"
    "static float karis(float4 c) { return 1.0 / (1.0 + dot(c.rgb, float3(0.2126, 0.7152, 0.0722))); }\n"
    "fragment float4 bloom_down13(BV in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "    constant BP &p [[buffer(0)]])\n"
    "{\n"
    "    float2 uv = in.uv, o = p.hp;\n"
    "    float4 a = t.sample(bs, uv + o * float2(-2.0, -2.0)), b = t.sample(bs, uv + o * float2(0.0, -2.0));\n"
    "    float4 c = t.sample(bs, uv + o * float2(2.0, -2.0)), d = t.sample(bs, uv + o * float2(-1.0, -1.0));\n"
    "    float4 e = t.sample(bs, uv + o * float2(1.0, -1.0)), f = t.sample(bs, uv + o * float2(-2.0, 0.0));\n"
    "    float4 g = t.sample(bs, uv), h = t.sample(bs, uv + o * float2(2.0, 0.0));\n"
    "    float4 i = t.sample(bs, uv + o * float2(-1.0, 1.0)), j = t.sample(bs, uv + o * float2(1.0, 1.0));\n"
    "    float4 k = t.sample(bs, uv + o * float2(-2.0, 2.0)), l = t.sample(bs, uv + o * float2(0.0, 2.0));\n"
    "    float4 m = t.sample(bs, uv + o * float2(2.0, 2.0));\n"
    "    float4 b0 = (d + e + i + j) * 0.25, b1 = (a + b + f + g) * 0.25, b2 = (b + c + g + h) * 0.25;\n"
    "    float4 b3 = (f + g + k + l) * 0.25, b4 = (g + h + l + m) * 0.25;\n"
    "    float w0 = 0.5 * karis(b0), w1 = 0.125 * karis(b1), w2 = 0.125 * karis(b2);\n"
    "    float w3 = 0.125 * karis(b3), w4 = 0.125 * karis(b4);\n"
    "    return (b0 * w0 + b1 * w1 + b2 * w2 + b3 * w3 + b4 * w4) / (w0 + w1 + w2 + w3 + w4);\n"
    "}\n"
    "fragment float4 bloom_down(BV in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "    constant BP &p [[buffer(0)]])\n"
    "{\n"
    "    float2 uv = in.uv, hp = p.hp;\n"
    "    float4 s = t.sample(bs, uv) * 4.0;\n"
    "    s += t.sample(bs, uv - hp);\n"
    "    s += t.sample(bs, uv + hp);\n"
    "    s += t.sample(bs, uv + float2(hp.x, -hp.y));\n"
    "    s += t.sample(bs, uv - float2(hp.x, -hp.y));\n"
    "    return s / 8.0;\n"
    "}\n"
    "fragment float4 bloom_up(BV in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "    constant BP &p [[buffer(0)]])\n"
    "{\n"
    "    float2 uv = in.uv, hp = p.hp;\n"
    "    float4 s = t.sample(bs, uv + float2(-hp.x * 2.0, 0.0));\n"
    "    s += t.sample(bs, uv + float2(-hp.x, hp.y)) * 2.0;\n"
    "    s += t.sample(bs, uv + float2(0.0, hp.y * 2.0));\n"
    "    s += t.sample(bs, uv + float2(hp.x, hp.y)) * 2.0;\n"
    "    s += t.sample(bs, uv + float2(hp.x * 2.0, 0.0));\n"
    "    s += t.sample(bs, uv + float2(hp.x, -hp.y)) * 2.0;\n"
    "    s += t.sample(bs, uv + float2(0.0, -hp.y * 2.0));\n"
    "    s += t.sample(bs, uv + float2(-hp.x, -hp.y)) * 2.0;\n"
    "    return s * (p.scale / 12.0);\n"
    "}\n";

@interface D3DBloom : NSObject
{
@public
    id<MTLLibrary> lib;
    id<MTLFunction> vs, down13_fs, down_fs, up_fs;
    id<MTLRenderPipelineState> down13, down, up_add, up_out;
    MTLPixelFormat up_out_fmt;
    id<MTLTexture> level[BLOOM_LEVELS];
    id<MTLTexture> bright; /* bright-pass target of this frame */
    D3DSurface *bright_surf;
    uint32_t frame;        /* present_count + 1 of the bright pass, 0 = none */
    bool logged_start, logged_target;
}
@end

@implementation D3DBloom
@end

static id<MTLRenderPipelineState> bloom_pipeline(D3DDevice *d, D3DBloom *b, id<MTLFunction> fs, MTLPixelFormat fmt,
                                                 bool add)
{
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    NSError *err = nil;
    id<MTLRenderPipelineState> p;
    pd.vertexFunction = b->vs;
    pd.fragmentFunction = fs;
    pd.colorAttachments[0].pixelFormat = fmt;
    if (add)
    {
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        pd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
    }
    p = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
    if (p == nil) rt_fatal("D3D9: Kawase bloom pipeline creation failed: %s", err.localizedDescription.UTF8String);
    return p;
}

static D3DBloom *bloom_of(D3DDevice *d)
{
    if (d->bloom == nil)
    {
        D3DBloom *b = [D3DBloom new];
        NSError *err = nil;
        b->lib = [d->mtl newLibraryWithSource:@(bloom_src) options:[MTLCompileOptions new] error:&err];
        if (b->lib == nil)
            rt_fatal("D3D9: Metal compile of the Kawase bloom shader failed: %s", err.localizedDescription.UTF8String);
        b->vs = [b->lib newFunctionWithName:@"bloom_vs"];
        b->down13_fs = [b->lib newFunctionWithName:@"bloom_down13"];
        b->down_fs = [b->lib newFunctionWithName:@"bloom_down"];
        b->up_fs = [b->lib newFunctionWithName:@"bloom_up"];
        if (b->vs == nil || b->down13_fs == nil || b->down_fs == nil || b->up_fs == nil)
            rt_fatal("D3D9: a Kawase bloom function is missing");
        b->down13 = bloom_pipeline(d, b, b->down13_fs, MTLPixelFormatRGBA16Float, false);
        b->down = bloom_pipeline(d, b, b->down_fs, MTLPixelFormatRGBA16Float, false);
        b->up_add = bloom_pipeline(d, b, b->up_fs, MTLPixelFormatRGBA16Float, true);
        d->bloom = b;
    }
    return (D3DBloom *)d->bloom;
}

static void ensure_levels(D3DDevice *d, D3DBloom *b, NSUInteger w, NSUInteger h)
{
    int i;
    for (i = 0; i < BLOOM_LEVELS; i++)
    {
        NSUInteger lw = w >> (i + 1), lh = h >> (i + 1);
        MTLTextureDescriptor *td;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        if (b->level[i] != nil && b->level[i].width == lw && b->level[i].height == lh) continue;
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                width:lw
                                                               height:lh
                                                            mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        b->level[i] = [d->mtl newTextureWithDescriptor:td];
        if (b->level[i] == nil)
            rt_fatal("D3D9: out of video memory (bloom texture %lux%lu)", (unsigned long)lw, (unsigned long)lh);
    }
}

/* One full-screen pass from src into dst (struct BP of the shader). add:
   the pass adds to the content of dst. */
static void bloom_pass(D3DDevice *d, id<MTLRenderPipelineState> pso, id<MTLTexture> src, id<MTLTexture> dst, float hpx,
                       float hpy, float scale, bool add)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    id<MTLRenderCommandEncoder> re;
    float hp[4] = { hpx, hpy, scale, 0.0f };
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = add ? MTLLoadActionLoad : MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    re = [d->cb renderCommandEncoderWithDescriptor:rp];
    if (re == nil) rt_fatal("D3D9: Kawase bloom encoder creation failed");
    re.label = @"Kawase bloom";
    [re setRenderPipelineState:pso];
    [re setFragmentTexture:src atIndex:0];
    [re setFragmentBytes:hp length:sizeof hp atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re endEncoding];
}

static void bloom_apply(D3DDevice *d, D3DBloom *b, D3DShader *ps)
{
    rt_com_object *o = ps->sh.ct_bloom_sampler >= 0 ? d->tex[ps->sh.ct_bloom_sampler] : NULL;
    D3DTexture *t = o != NULL && d3d_is_texture(o) ? d3d_tex(o) : nil;
    id<MTLTexture> dst = t != nil && !t->cube && !t->volume ? t->tex : nil, src = b->bright;
    NSUInteger w, h;
    int i;
    b->frame = 0;
    b->bright = nil;
    if (dst == nil || dst.sampleCount != 1 || (dst.usage & MTLTextureUsageRenderTarget) == 0 || dst.width < 32 ||
        dst.height < 32 || src == nil)
    {
        if (!b->logged_target)
            rt_log("D3D9: Kawase bloom: the BloomedScene texture of the composite is not a render target; the frame "
                   "has no bloom");
        b->logged_target = true;
        b->bright_surf = nil;
        return;
    }
    w = dst.width;
    h = dst.height;
    if (!b->logged_start)
    {
        rt_log("D3D9: dual Kawase bloom %lux%lu from %lux%lu, %d levels, 13-tap Karis first pass, additive up", (unsigned long)w, (unsigned long)h,
               (unsigned long)src.width, (unsigned long)src.height, BLOOM_LEVELS);
        b->logged_start = true;
    }
    @autoreleasepool
    {
        d3d_end_pass(d);
        ensure_levels(d, b, w, h);
        if (b->up_out == nil || b->up_out_fmt != dst.pixelFormat)
        {
            b->up_out = bloom_pipeline(d, b, b->up_fs, dst.pixelFormat, false);
            b->up_out_fmt = dst.pixelFormat;
        }
        bloom_pass(d, b->down13, src, b->level[0], 1.0f / (float)src.width, 1.0f / (float)src.height, 1.0f, false);
        for (i = 1; i < BLOOM_LEVELS; i++)
        {
            id<MTLTexture> out = b->level[i];
            bloom_pass(d, b->down, b->level[i - 1], out, 0.5f / (float)out.width, 0.5f / (float)out.height, 1.0f,
                       false);
        }
        /* Up: each level adds the up-sampled sum of the levels below it;
           the last pass writes the sum of all levels / BLOOM_LEVELS. */
        for (i = BLOOM_LEVELS - 2; i >= -1; i--)
        {
            id<MTLTexture> out = i >= 0 ? b->level[i] : dst;
            bloom_pass(d, i >= 0 ? b->up_add : b->up_out, b->level[i + 1], out, 0.5f / (float)out.width,
                       0.5f / (float)out.height, i >= 0 ? 1.0f : 1.0f / (float)BLOOM_LEVELS, i >= 0);
        }
    }
    t->last_use = d->serial;
    if (b->bright_surf != nil) d3d_use_surface(b->bright_surf);
    b->bright_surf = nil;
}

bool d3d_bloom_draw(D3DDevice *d, D3DShader *ps)
{
    D3DBloom *b;
    if (ps == nil || rt_enh_get(RT_ENH_BLOOM) == 0) return true;
    if (ps->sh.ct_role == D3DSH_ROLE_BLOOM_BRIGHT)
    {
        D3DSurface *s = d->rt[0] != NULL ? d3d_surf(d->rt[0]) : nil;
        NSUInteger slice = 0, lvl = 0;
        id<MTLTexture> t = s != nil ? d3d_surface_texture(s, &slice, &lvl) : nil;
        b = bloom_of(d);
        /* Only a plain 2D texture of one level can be the source. */
        if (t == nil || t.sampleCount != 1 || t.textureType != MTLTextureType2D || t.mipmapLevelCount != 1 ||
            (t.usage & MTLTextureUsageShaderRead) == 0)
        {
            b->frame = 0;
            b->bright = nil;
            b->bright_surf = nil;
            return true;
        }
        b->bright = t;
        b->bright_surf = s;
        b->frame = d->present_count + 1u;
        return true;
    }
    if (d->bloom == nil) return true;
    b = (D3DBloom *)d->bloom;
    if (b->frame != d->present_count + 1u) return true;
    if (ps->sh.ct_role == D3DSH_ROLE_BLOOM_BLUR) return false;
    if (ps->sh.ct_bloom_sampler >= 0) bloom_apply(d, b, ps);
    return true;
}
