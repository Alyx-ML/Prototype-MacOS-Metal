/*
 * Post-processing of the presented frame: the Mac enhancement settings
 * (rt_enh.h) that change the complete image.
 *
 * d3d_present (rt_d3d9_draw.m) calls d3d_post_process with the resolved back
 * buffer before it draws the frame into the drawable. The steps, each one
 * only if its setting is on:
 *   1. SMAA: morphological anti-aliasing in three passes at the back buffer
 *      size. This is SMAA 1x with the settings of the SMAA "low" preset (no
 *      diagonal pattern detection and no corner detection): luma edge
 *      detection with local contrast adaptation, a search along each edge
 *      for its two ends and the crossing edges there, and the coverage area
 *      of the reconstructed edge line for each pixel. The area is calculated
 *      in the shader (the reference implementation reads the same values
 *      from a precalculated texture).
 *   2. MetalFX spatial upscaling from the back buffer size to the size of
 *      the image in the drawable, if the drawable image is larger.
 *   3. Contrast-adaptive sharpening (the method of AMD FidelityFX CAS) at the
 *      size of the result of step 2.
 * The result is the texture that d3d_present samples.
 *
 * HDR: d3d_post_hdr_begin sets the layer for extended dynamic range (EDR)
 * output: a 16-bit float drawable in the extended linear sRGB color space.
 * The present shader then expands the highlights up to the EDR headroom of
 * the display. The layer properties change on the main thread; the present
 * shader follows the pixel format of each drawable.
 */
#include "rt_d3d9_int.h"
#include "rt_enh.h"

#import <AppKit/AppKit.h>
#import <MetalFX/MetalFX.h>

#include <math.h>
#include <stdatomic.h>

static const char post_src[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct PV { float4 pos [[position]]; };\n"
    "vertex PV post_vs(uint vid [[vertex_id]])\n"
    "{\n"
    "    float2 q = float2(float((vid << 1) & 2u), float(vid & 2u));\n"
    "    PV o;\n"
    "    o.pos = float4(q * 2.0 - 1.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n"
    "static float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }\n"
    "static int2 clampc(int2 p, int2 sz) { return clamp(p, int2(0), sz - 1); }\n"
    "static float L(texture2d<float> t, int2 p, int2 sz) { return luma(t.read(uint2(clampc(p, sz))).rgb); }\n"
    /* Pass 1: edges. r: edge on the left side of the pixel; g: edge on the
       top side. SMAA threshold 0.1 and local contrast adaptation factor 2. */
    "fragment float4 smaa_edges(PV in [[stage_in]], texture2d<float> src [[texture(0)]])\n"
    "{\n"
    "    int2 sz = int2(src.get_width(), src.get_height());\n"
    "    int2 p = int2(in.pos.xy);\n"
    "    float c = L(src, p, sz);\n"
    "    float dl = abs(c - L(src, p + int2(-1, 0), sz));\n"
    "    float dt = abs(c - L(src, p + int2(0, -1), sz));\n"
    "    float2 e = step(float2(0.1), float2(dl, dt));\n"
    "    if (e.x + e.y == 0.0) return float4(0.0);\n"
    "    float dr = abs(c - L(src, p + int2(1, 0), sz));\n"
    "    float db = abs(c - L(src, p + int2(0, 1), sz));\n"
    "    float dll = abs(L(src, p + int2(-1, 0), sz) - L(src, p + int2(-2, 0), sz));\n"
    "    float dtt = abs(L(src, p + int2(0, -1), sz) - L(src, p + int2(0, -2), sz));\n"
    "    float m = max(max(max(dl, dt), max(dr, db)), max(dll, dtt));\n"
    "    e *= step(m, 2.0 * float2(dl, dt));\n"
    "    return float4(e, 0.0, 0.0);\n"
    "}\n"
    "static float E(texture2d<float> t, int2 p, int2 sz, int ch)\n"
    "{\n"
    "    if (p.x < 0 || p.y < 0 || p.x >= sz.x || p.y >= sz.y) return 0.0;\n"
    "    float4 v = t.read(uint2(p));\n"
    "    return ch == 0 ? v.r : v.g;\n"
    "}\n"
    /* The integral of the reconstructed edge line over the pixel span [a, b]
       of an edge run of length n. The run is split at its middle: the first
       half has a line from height h1 at the start to 0 at the middle, the
       second half a line from 0 at the middle to h2 at the end. The result
       gives the area above the edge (positive) and below it (negative). */
    "static float2 run_area(float a, float b, float n, float h1, float h2)\n"
    "{\n"
    "    float m = 0.5 * n;\n"
    "    float2 r = float2(0.0);\n"
    "    float a1 = a, b1 = min(b, m);\n"
    "    if (b1 > a1) { float v = h1 * ((b1 - a1) - (b1 * b1 - a1 * a1) / n); if (v > 0.0) r.x += v; else r.y -= v; }\n"
    "    float a2 = max(a, m), b2 = b;\n"
    "    if (b2 > a2) { float v = h2 * ((b2 * b2 - a2 * a2) / n - (b2 - a2)); if (v > 0.0) r.x += v; else r.y -= v; }\n"
    "    return r;\n"
    "}\n"
    "#define MAX_SEARCH 32\n"
    /* Pass 2: blend weights.
       Edge on the top side (horizontal run along x, crossing edges are left
       edges): r = part of the pixel above that this pixel takes, g = part of
       this pixel that the pixel above takes.
       Edge on the left side (vertical run along y, crossing edges are top
       edges): b = part of the left pixel that this pixel takes, a = part of
       this pixel that the left pixel takes.
       A crossing edge on the side of the other pixel at an end of the run
       gives the height +0.5 there, a crossing edge on the side of this pixel
       gives -0.5, both or none give 0. */
    "fragment float4 smaa_weights(PV in [[stage_in]], texture2d<float> edges [[texture(0)]])\n"
    "{\n"
    "    int2 sz = int2(edges.get_width(), edges.get_height());\n"
    "    int2 p = int2(in.pos.xy);\n"
    "    float2 e = edges.read(uint2(p)).rg;\n"
    "    float4 w = float4(0.0);\n"
    "    if (e.y > 0.5) {\n"
    "        int xl = p.x, xr = p.x, i;\n"
    "        bool okl = true, okr = true;\n"
    "        for (i = 0; i < MAX_SEARCH; i++) {\n"
    "            int2 q = int2(xl, p.y);\n"
    "            if (E(edges, q, sz, 0) > 0.5 || E(edges, q + int2(0, -1), sz, 0) > 0.5) break;\n"
    "            if (E(edges, q + int2(-1, 0), sz, 1) < 0.5) break;\n"
    "            xl--;\n"
    "        }\n"
    "        if (i == MAX_SEARCH) okl = false;\n"
    "        for (i = 0; i < MAX_SEARCH; i++) {\n"
    "            int2 q = int2(xr + 1, p.y);\n"
    "            if (E(edges, q, sz, 0) > 0.5 || E(edges, q + int2(0, -1), sz, 0) > 0.5) break;\n"
    "            if (E(edges, q, sz, 1) < 0.5) break;\n"
    "            xr++;\n"
    "        }\n"
    "        if (i == MAX_SEARCH) okr = false;\n"
    "        float up1 = E(edges, int2(xl, p.y - 1), sz, 0), lo1 = E(edges, int2(xl, p.y), sz, 0);\n"
    "        float up2 = E(edges, int2(xr + 1, p.y - 1), sz, 0), lo2 = E(edges, int2(xr + 1, p.y), sz, 0);\n"
    "        float h1 = okl ? 0.5 * (step(0.5, up1) - step(0.5, lo1)) : 0.0;\n"
    "        float h2 = okr ? 0.5 * (step(0.5, up2) - step(0.5, lo2)) : 0.0;\n"
    "        float n = float(xr - xl + 1), x0 = float(p.x - xl);\n"
    "        float2 ar = run_area(x0, x0 + 1.0, n, h1, h2);\n"
    "        w.r = ar.y; w.g = ar.x;\n"
    "    }\n"
    "    if (e.x > 0.5) {\n"
    "        int yt = p.y, yb = p.y, i;\n"
    "        bool okt = true, okb = true;\n"
    "        for (i = 0; i < MAX_SEARCH; i++) {\n"
    "            int2 q = int2(p.x, yt);\n"
    "            if (E(edges, q, sz, 1) > 0.5 || E(edges, q + int2(-1, 0), sz, 1) > 0.5) break;\n"
    "            if (E(edges, q + int2(0, -1), sz, 0) < 0.5) break;\n"
    "            yt--;\n"
    "        }\n"
    "        if (i == MAX_SEARCH) okt = false;\n"
    "        for (i = 0; i < MAX_SEARCH; i++) {\n"
    "            int2 q = int2(p.x, yb + 1);\n"
    "            if (E(edges, q, sz, 1) > 0.5 || E(edges, q + int2(-1, 0), sz, 1) > 0.5) break;\n"
    "            if (E(edges, q, sz, 0) < 0.5) break;\n"
    "            yb++;\n"
    "        }\n"
    "        if (i == MAX_SEARCH) okb = false;\n"
    "        float le1 = E(edges, int2(p.x - 1, yt), sz, 1), cu1 = E(edges, int2(p.x, yt), sz, 1);\n"
    "        float le2 = E(edges, int2(p.x - 1, yb + 1), sz, 1), cu2 = E(edges, int2(p.x, yb + 1), sz, 1);\n"
    "        float h1 = okt ? 0.5 * (step(0.5, le1) - step(0.5, cu1)) : 0.0;\n"
    "        float h2 = okb ? 0.5 * (step(0.5, le2) - step(0.5, cu2)) : 0.0;\n"
    "        float n = float(yb - yt + 1), y0 = float(p.y - yt);\n"
    "        float2 ar = run_area(y0, y0 + 1.0, n, h1, h2);\n"
    "        w.b = ar.y; w.a = ar.x;\n"
    "    }\n"
    "    return w;\n"
    "}\n"
    /* Pass 3: blend each pixel with its four neighbors. */
    "fragment float4 smaa_blend(PV in [[stage_in]], texture2d<float> src [[texture(0)]],\n"
    "    texture2d<float> wt [[texture(1)]])\n"
    "{\n"
    "    int2 sz = int2(src.get_width(), src.get_height());\n"
    "    int2 p = int2(in.pos.xy);\n"
    "    float4 c = src.read(uint2(p));\n"
    "    float4 w = wt.read(uint2(p));\n"
    "    float fu = w.r, fl = w.b;\n"
    "    float fd = p.y + 1 < sz.y ? wt.read(uint2(p + int2(0, 1))).g : 0.0;\n"
    "    float fr = p.x + 1 < sz.x ? wt.read(uint2(p + int2(1, 0))).a : 0.0;\n"
    "    float s = fu + fd + fl + fr;\n"
    "    if (s <= 0.0) return c;\n"
    "    if (s > 1.0) { float k = 1.0 / s; fu *= k; fd *= k; fl *= k; fr *= k; s = 1.0; }\n"
    "    float4 r = c * (1.0 - s);\n"
    "    r += fu * src.read(uint2(clampc(p + int2(0, -1), sz)));\n"
    "    r += fd * src.read(uint2(clampc(p + int2(0, 1), sz)));\n"
    "    r += fl * src.read(uint2(clampc(p + int2(-1, 0), sz)));\n"
    "    r += fr * src.read(uint2(clampc(p + int2(1, 0), sz)));\n"
    "    return r;\n"
    "}\n"
    /* Contrast-adaptive sharpening: the sharpening amount of each pixel
       depends on the local contrast of its 3x3 neighborhood, so edges with
       high contrast do not get halos. */
    "fragment float4 cas_fs(PV in [[stage_in]], texture2d<float> src [[texture(0)]], constant float &sharp [[buffer(0)]])\n"
    "{\n"
    "    int2 sz = int2(src.get_width(), src.get_height());\n"
    "    int2 p = int2(in.pos.xy);\n"
    "    float3 a = src.read(uint2(clampc(p + int2(-1, -1), sz))).rgb;\n"
    "    float3 b = src.read(uint2(clampc(p + int2(0, -1), sz))).rgb;\n"
    "    float3 c = src.read(uint2(clampc(p + int2(1, -1), sz))).rgb;\n"
    "    float3 d = src.read(uint2(clampc(p + int2(-1, 0), sz))).rgb;\n"
    "    float4 e4 = src.read(uint2(p));\n"
    "    float3 e = e4.rgb;\n"
    "    float3 f = src.read(uint2(clampc(p + int2(1, 0), sz))).rgb;\n"
    "    float3 g = src.read(uint2(clampc(p + int2(-1, 1), sz))).rgb;\n"
    "    float3 h = src.read(uint2(clampc(p + int2(0, 1), sz))).rgb;\n"
    "    float3 i = src.read(uint2(clampc(p + int2(1, 1), sz))).rgb;\n"
    "    float3 mn = min(min(min(d, e), min(f, b)), h);\n"
    "    mn += min(mn, min(min(a, c), min(g, i)));\n"
    "    float3 mx = max(max(max(d, e), max(f, b)), h);\n"
    "    mx += max(mx, max(max(a, c), max(g, i)));\n"
    "    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));\n"
    "    float3 w = amp * (-1.0 / mix(8.0, 5.0, sharp));\n"
    "    float3 r = ((b + d + f + h) * w + e) / (1.0 + 4.0 * w);\n"
    "    return float4(saturate(r), e4.a);\n"
    "}\n";

@interface D3DPost : NSObject
{
@public
    id<MTLLibrary> lib;
    id<MTLRenderPipelineState> edges_pso, weights_pso, blend_pso, cas_pso;
    MTLPixelFormat pso_format;
    id<MTLTexture> edges, weights, smaa_out, cas_out, fx_out;
    id<MTLFXSpatialScaler> scaler;
    bool fx_failed_logged;
    bool hdr_requested;
    bool hdr_no_headroom_logged;
    _Atomic float headroom;
    uint32_t headroom_frames;
}
@end

@implementation D3DPost
@end

static D3DPost *post_of(D3DDevice *d)
{
    if (d->post == nil)
    {
        D3DPost *p = [D3DPost new];
        NSError *err = nil;
        p->lib = [d->mtl newLibraryWithSource:@(post_src) options:[MTLCompileOptions new] error:&err];
        if (p->lib == nil) rt_fatal("D3D9: Metal compile of the post-processing shaders failed: %s", err.localizedDescription.UTF8String);
        atomic_store(&p->headroom, 1.0f);
        d->post = p;
    }
    return (D3DPost *)d->post;
}

static id<MTLRenderPipelineState> post_pso(D3DDevice *d, D3DPost *p, const char *fs, MTLPixelFormat fmt)
{
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    NSError *err = nil;
    id<MTLRenderPipelineState> s;
    pd.vertexFunction = [p->lib newFunctionWithName:@"post_vs"];
    pd.fragmentFunction = [p->lib newFunctionWithName:@(fs)];
    if (pd.vertexFunction == nil || pd.fragmentFunction == nil) rt_fatal("D3D9: post-processing function %s is missing", fs);
    pd.colorAttachments[0].pixelFormat = fmt;
    s = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
    if (s == nil) rt_fatal("D3D9: post-processing pipeline %s creation failed: %s", fs, err.localizedDescription.UTF8String);
    return s;
}

/* A private render target texture of a size and format; the texture in
   *slot is used again when it agrees. */
static id<MTLTexture> post_tex(D3DDevice *d, id<MTLTexture> __strong *slot, NSUInteger w, NSUInteger h, MTLPixelFormat fmt,
                               MTLTextureUsage usage)
{
    id<MTLTexture> t = *slot;
    if (t != nil && t.width == w && t.height == h && t.pixelFormat == fmt && (t.usage & usage) == usage) return t;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = usage;
    t = [d->mtl newTextureWithDescriptor:td];
    if (t == nil) rt_fatal("D3D9: out of video memory (post-processing texture %lux%lu)", (unsigned long)w, (unsigned long)h);
    *slot = t;
    return t;
}

static void post_pass(D3DDevice *d, id<MTLRenderPipelineState> pso, id<MTLTexture> dst, id<MTLTexture> t0, id<MTLTexture> t1,
                      const void *bytes, NSUInteger nbytes, const char *what)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    id<MTLRenderCommandEncoder> e;
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    e = [d->cb renderCommandEncoderWithDescriptor:rp];
    if (e == nil) rt_fatal("D3D9: %s encoder creation failed", what);
    e.label = @(what);
    [e setRenderPipelineState:pso];
    [e setFragmentTexture:t0 atIndex:0];
    if (t1 != nil) [e setFragmentTexture:t1 atIndex:1];
    if (bytes != NULL) [e setFragmentBytes:bytes length:nbytes atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

static void ensure_psos(D3DDevice *d, D3DPost *p, MTLPixelFormat fmt)
{
    if (p->edges_pso != nil && p->pso_format == fmt) return;
    p->edges_pso = post_pso(d, p, "smaa_edges", MTLPixelFormatRG8Unorm);
    p->weights_pso = post_pso(d, p, "smaa_weights", MTLPixelFormatRGBA8Unorm);
    p->blend_pso = post_pso(d, p, "smaa_blend", fmt);
    p->cas_pso = post_pso(d, p, "cas_fs", fmt);
    p->pso_format = fmt;
}

static id<MTLTexture> metalfx(D3DDevice *d, D3DPost *p, id<MTLTexture> src, uint32_t out_w, uint32_t out_h)
{
    MTLFXSpatialScalerDescriptor *sd;
    id<MTLTexture> out;
    if (p->scaler == nil || p->scaler.inputWidth != src.width || p->scaler.inputHeight != src.height ||
        p->scaler.outputWidth != out_w || p->scaler.outputHeight != out_h || p->scaler.colorTextureFormat != src.pixelFormat)
    {
        if (![MTLFXSpatialScalerDescriptor supportsDevice:d->mtl])
            rt_fatal("MetalFX upscaling is on, but this GPU does not support the MetalFX spatial scaler; switch "
                     "\"MetalFX Upscaling\" off in the Video options");
        sd = [MTLFXSpatialScalerDescriptor new];
        sd.inputWidth = src.width;
        sd.inputHeight = src.height;
        sd.outputWidth = out_w;
        sd.outputHeight = out_h;
        sd.colorTextureFormat = src.pixelFormat;
        sd.outputTextureFormat = src.pixelFormat;
        sd.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
        p->scaler = [sd newSpatialScalerWithDevice:d->mtl];
        if (p->scaler == nil)
            rt_fatal("D3D9: MetalFX spatial scaler creation failed (%lux%lu to %ux%u, pixel format %lu)",
                     (unsigned long)src.width, (unsigned long)src.height, out_w, out_h, (unsigned long)src.pixelFormat);
        rt_log("D3D9: MetalFX spatial upscaling %lux%lu to %ux%u", (unsigned long)src.width, (unsigned long)src.height,
               out_w, out_h);
    }
    if ((src.usage & p->scaler.colorTextureUsage) != p->scaler.colorTextureUsage)
        rt_fatal("D3D9: the MetalFX input texture does not have the usage that the scaler needs");
    out = post_tex(d, &p->fx_out, out_w, out_h, src.pixelFormat, p->scaler.outputTextureUsage | MTLTextureUsageShaderRead);
    p->scaler.colorTexture = src;
    p->scaler.outputTexture = out;
    [p->scaler encodeToCommandBuffer:d->cb];
    return out;
}

id<MTLTexture> d3d_post_process(D3DDevice *d, id<MTLTexture> src, uint32_t out_w, uint32_t out_h)
{
    int smaa = rt_enh_get(RT_ENH_SMAA), fx = rt_enh_get(RT_ENH_METALFX), sharpen = rt_enh_get(RT_ENH_SHARPEN);
    const MTLTextureUsage rt_read = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    D3DPost *p;
    if (!smaa && !fx && !sharpen) return src;
    p = post_of(d);
    ensure_psos(d, p, src.pixelFormat);
    if (smaa)
    {
        id<MTLTexture> e = post_tex(d, &p->edges, src.width, src.height, MTLPixelFormatRG8Unorm, rt_read);
        id<MTLTexture> w = post_tex(d, &p->weights, src.width, src.height, MTLPixelFormatRGBA8Unorm, rt_read);
        id<MTLTexture> o = post_tex(d, &p->smaa_out, src.width, src.height, src.pixelFormat, rt_read);
        post_pass(d, p->edges_pso, e, src, nil, NULL, 0, "SMAA edges");
        post_pass(d, p->weights_pso, w, e, nil, NULL, 0, "SMAA weights");
        post_pass(d, p->blend_pso, o, src, w, NULL, 0, "SMAA blend");
        src = o;
    }
    if (fx && out_w > src.width && out_h > src.height) src = metalfx(d, p, src, out_w, out_h);
    if (sharpen)
    {
        static const float amount[4] = { 0.0f, 0.3f, 0.6f, 1.0f };
        float s = amount[sharpen];
        id<MTLTexture> o = post_tex(d, &p->cas_out, src.width, src.height, src.pixelFormat, rt_read);
        post_pass(d, p->cas_pso, o, src, nil, &s, sizeof s, "sharpening");
        src = o;
    }
    return src;
}

/* The EDR headroom of the screen of the game window (main thread). */
static float screen_headroom(CAMetalLayer *layer)
{
    NSScreen *s = nil;
    id delegate = layer.delegate;
    if ([delegate isKindOfClass:[NSView class]]) s = ((NSView *)delegate).window.screen;
    if (s == nil) s = NSScreen.mainScreen;
    return s == nil ? 1.0f : (float)s.maximumExtendedDynamicRangeColorComponentValue;
}

float d3d_post_hdr_begin(D3DDevice *d)
{
    D3DPost *p = post_of(d);
    bool want = rt_enh_get(RT_ENH_HDR) != 0;
    CAMetalLayer *layer = d->layer;
    if (want != p->hdr_requested)
    {
        p->hdr_requested = want;
        p->headroom_frames = 0;
        dispatch_async(dispatch_get_main_queue(), ^{
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(want ? kCGColorSpaceExtendedLinearSRGB : kCGColorSpaceSRGB);
            if (cs == NULL) rt_fatal("D3D9: the color space for %s output cannot be created", want ? "HDR" : "SDR");
            layer.wantsExtendedDynamicRangeContent = want;
            layer.pixelFormat = want ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
            layer.colorspace = cs;
            CGColorSpaceRelease(cs);
            atomic_store(&p->headroom, screen_headroom(layer));
        });
        rt_log("D3D9: HDR output %s", want ? "on" : "off");
    }
    else if (want && ++p->headroom_frames >= 120)
    {
        /* The headroom changes with the display brightness. */
        p->headroom_frames = 0;
        dispatch_async(dispatch_get_main_queue(), ^{
            atomic_store(&p->headroom, screen_headroom(layer));
        });
    }
    if (want && !p->hdr_no_headroom_logged && p->headroom_frames == 60 && atomic_load(&p->headroom) <= 1.0f)
    {
        rt_log("D3D9: HDR output is on, but the display has no extended dynamic range headroom; highlights are not "
               "expanded");
        p->hdr_no_headroom_logged = true;
    }
    return atomic_load(&p->headroom);
}
