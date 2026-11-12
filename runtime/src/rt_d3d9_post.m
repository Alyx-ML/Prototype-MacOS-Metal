/*
 * Post-processing of the presented frame: the Mac enhancement settings
 * (rt_enh.h) that change the complete image.
 *
 * d3d_present (rt_d3d9_draw.m) calls d3d_post_process with the resolved back
 * buffer before it draws the frame into the drawable. The steps, each one
 * only if its setting is on:
 *   1. SMAA: morphological anti-aliasing in three passes at the back buffer
 *      size. This is SMAA 1x with the settings of the SMAA "ultra" preset
 *      (threshold 0.05, search lengths of 32 pixels orthogonal and 16
 *      diagonal, corner rounding 25): color edge detection with local
 *      contrast adaptation and depth predication,
 *      diagonal pattern detection, a search along each edge for its two
 *      ends and the crossing edges there, the coverage area of the
 *      reconstructed edge line for each pixel, and corner detection. The
 *      orthogonal and the diagonal area textures are calculated once on the
 *      CPU with the method of the reference script AreaTex.py
 *      (ortho_texture, diag_texture), with the subsample offsets of SMAA 1x
 *      and of the two frames of SMAA T2x. The depth predication uses the scene depth
 *      of the frame (d3d_ao_camera); a frame without a scene camera (for
 *      example a menu) has the edge detection without it. UNCONFIRMED: the
 *      depth buffer does not change between the scene and the present (the
 *      HUD draws of the game do not use the depth).
 *   2. MetalFX spatial upscaling from the back buffer size to the size of
 *      the image in the drawable, if the drawable image is larger.
 *   3. Contrast-adaptive sharpening (the method of AMD FidelityFX CAS) at the
 *      size of the result of step 2.
 * The result is the texture that d3d_present samples.
 *
 * SMAA T2x (setting value 2): d3d_post_t2x runs at the bloom composite of
 * the scene (before the HUD) in place of step 1. The scene draws have a
 * jitter of a quarter pixel that alternates each frame
 * (d3d_post_t2x_jitter). The SMAA passes use the subsample set of the
 * frame; the blend keeps the velocity weight in alpha; the resolve mixes
 * the frame with the last frame at the position from the camera motion.
 * A frame without the resolve gets step 1 at the present.
 *
 * The SMAA diagonal detection and the diagonal area calculation are ports
 * of SMAA.hlsl and Scripts/AreaTex.py of the SMAA reference
 * implementation (github.com/iryoku/smaa), which is available under this
 * license:
 *   Copyright (C) 2013 Jorge Jimenez (jorge@iryoku.com)
 *   Copyright (C) 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)
 *   Copyright (C) 2013 Belen Masia (bmasia@unizar.es)
 *   Copyright (C) 2013 Fernando Navarro (fernandn@microsoft.com)
 *   Copyright (C) 2013 Diego Gutierrez (diegog@unizar.es)
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *   The above copyright notice and this permission notice shall be
 *   included in all copies or substantial portions of the Software. As
 *   clarification, there is no requirement that the copyright notice and
 *   permission be included in binary distributions of the Software.
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * HDR: d3d_post_hdr_begin sets the layer for extended dynamic range (EDR)
 * output: a 16-bit float drawable in the extended linear sRGB color space.
 * The present shader then expands the highlights up to the EDR headroom of
 * the display. The layer properties change on the main thread; the present
 * shader follows the pixel format of each drawable.
 *
 * Tone mapping: d3d_post_gt7 gives the constants of the GT7 tone mapping
 * (RT_ENH_TONEMAP). The scene pass of rt_d3d9_draw.m applies it after the
 * game's bloom composite and before the HUD. With HDR output the back
 * buffer is then a 16-bit float texture (D3DSurface.hdr): the scene pass
 * also expands the highlights to the EDR headroom and writes values above
 * 1, and the present shader only decodes the frame, so the HUD stays at
 * the SDR white. A frame without the scene pass (for example a menu) gets
 * the EDR expansion and GT7 in the present shader. The input of the
 * post-processing steps can then have values above 1: the sharpening
 * result is not clipped at 1. UNCONFIRMED: the MetalFX spatial scaler in
 * the perceptual mode with values above 1.
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
    "static int2 clampc(int2 p, int2 sz) { return clamp(p, int2(0), sz - 1); }\n"
    /* Pass 1: edges (SMAAColorEdgeDetectionPS). r: edge on the left side of
       the pixel; g: edge on the top side. The difference of two pixels is
       the largest difference of their color channels. SMAA threshold 0.05
       (the "ultra" preset) and local contrast adaptation factor 2.
       Depth predication (smaa_edges_pred, smaa_edges_pred_ms): the SMAA
       values predication threshold 0.01, scale 2 and strength 0.4 change
       the threshold of each side to 0.1, or to 0.06 where the depth also
       has an edge. The depth value is the view depth (pr.y = P22,
       pr.z = P23: view depth = P23 / (z - P22)), and its edge test is
       relative: |difference| / view depth of the pixel. */
    "static float view_depth(float z, float4 pr)\n"
    "{\n"
    "    float den = z - pr.y;\n"
    "    return abs(pr.z / (abs(den) < 1e-7 ? 1e-7 : den));\n"
    "}\n"
    "static float2 pred_threshold(float3 v)\n"
    "{\n"
    "    float2 rel = abs(v.xx - v.yz) / max(v.x, 1e-6);\n"
    "    return 2.0 * 0.05 * (1.0 - 0.4 * step(float2(0.01), rel));\n"
    "}\n"
    "static float CD(texture2d<float> t, int2 p, int2 sz, float3 c)\n"
    "{\n"
    "    float3 d = abs(c - t.read(uint2(clampc(p, sz))).rgb);\n"
    "    return max(max(d.r, d.g), d.b);\n"
    "}\n"
    "static float4 edges_at(texture2d<float> src, int2 p, float2 thr)\n"
    "{\n"
    "    int2 sz = int2(src.get_width(), src.get_height());\n"
    "    float3 c = src.read(uint2(clampc(p, sz))).rgb;\n"
    "    float2 dlt = float2(CD(src, p + int2(-1, 0), sz, c), CD(src, p + int2(0, -1), sz, c));\n"
    "    float2 e = step(thr, dlt);\n"
    "    if (e.x + e.y == 0.0) return float4(0.0);\n"
    "    float2 m = max(dlt, float2(CD(src, p + int2(1, 0), sz, c), CD(src, p + int2(0, 1), sz, c)));\n"
    "    m = max(m, float2(CD(src, p + int2(-2, 0), sz, c), CD(src, p + int2(0, -2), sz, c)));\n"
    "    e *= step(max(m.x, m.y), 2.0 * dlt);\n"
    "    return float4(e, 0.0, 0.0);\n"
    "}\n"
    "fragment float4 smaa_edges(PV in [[stage_in]], texture2d<float> src [[texture(0)]])\n"
    "{\n"
    "    return edges_at(src, int2(in.pos.xy), float2(0.05));\n"
    "}\n"
    "fragment float4 smaa_edges_pred(PV in [[stage_in]], texture2d<float> src [[texture(0)]],\n"
    "    depth2d<float> dep [[texture(1)]], constant float4 &pr [[buffer(0)]])\n"
    "{\n"
    "    int2 p = int2(in.pos.xy), sz = int2(dep.get_width(), dep.get_height());\n"
    "    float3 v = float3(view_depth(dep.read(uint2(clampc(p, sz))), pr),\n"
    "        view_depth(dep.read(uint2(clampc(p + int2(-1, 0), sz))), pr),\n"
    "        view_depth(dep.read(uint2(clampc(p + int2(0, -1), sz))), pr));\n"
    "    return edges_at(src, p, pred_threshold(v));\n"
    "}\n"
    "fragment float4 smaa_edges_pred_ms(PV in [[stage_in]], texture2d<float> src [[texture(0)]],\n"
    "    depth2d_ms<float> dep [[texture(1)]], constant float4 &pr [[buffer(0)]])\n"
    "{\n"
    "    int2 p = int2(in.pos.xy), sz = int2(dep.get_width(), dep.get_height());\n"
    "    float3 v = float3(view_depth(dep.read(uint2(clampc(p, sz)), 0), pr),\n"
    "        view_depth(dep.read(uint2(clampc(p + int2(-1, 0), sz)), 0), pr),\n"
    "        view_depth(dep.read(uint2(clampc(p + int2(0, -1), sz)), 0), pr));\n"
    "    return edges_at(src, p, pred_threshold(v));\n"
    "}\n"
    "static float E(texture2d<float> t, int2 p, int2 sz, int ch)\n"
    "{\n"
    "    if (p.x < 0 || p.y < 0 || p.x >= sz.x || p.y >= sz.y) return 0.0;\n"
    "    float4 v = t.read(uint2(p));\n"
    "    return ch == 0 ? v.r : v.g;\n"
    "}\n"
    /* The orthogonal area texture (ortho_texture): for edge pattern n
       (crossing edges at the first end on the side of this pixel = 1, on the
       other side = 4; at the last end 2 and 8), the distances left and right
       to the two ends and the subsample set, the two areas (r: below the
       edge, g: above it). */
    "#define ORTHO_SIZE 33\n"
    "static float2 area_ortho(texture2d<float> area, int n, int left, int right, int set)\n"
    "{\n"
    "    return area.read(uint2(n * ORTHO_SIZE + left, set * ORTHO_SIZE + right)).rg;\n"
    "}\n"
    "#define MAX_SEARCH 32\n"
    /* The corner factors of the two weights of a run (see the corner
       detection below). dd: distance to the first and the last pixel of the
       run. ce: the continued crossing edges at the first and the last end on
       the side of this pixel (xy, for the first weight) and of the other
       pixel (zw, for the second weight). */
    "static float2 corner(float2 dd, float4 ce)\n"
    "{\n"
    "    float2 lr = step(dd.xy, dd.yx);\n"
    "    float2 r = 0.75 * lr / (lr.x + lr.y);\n"
    "    return saturate(float2(1.0 - r.x * ce.x - r.y * ce.y, 1.0 - r.x * ce.z - r.y * ce.w));\n"
    "}\n"
    /* Diagonal detection (SMAASearchDiag1, SMAASearchDiag2, SMAAAreaDiag
       and SMAACalculateDiagWeights of the SMAA reference, with point reads
       in place of the bilinear reads; MAX_SEARCH_DIAG is the "ultra"
       value). search_diag goes along a diagonal while each pixel has both
       edges of a diagonal step (two: the left edge of the next pixel and
       the top edge). It returns the steps and 1 when it found no end. The
       area texture is the diagonal part of the SMAA area texture
       (diag_area_texture), 20 x 20 distances for each of the 4 x 4
       crossing edge cases. */
    "#define MAX_SEARCH_DIAG 16\n"
    "static float2 search_diag(texture2d<float> edges, int2 p, int2 dir, int2 sz, bool two, thread float2 &end)\n"
    "{\n"
    "    float z = -1.0, w = 1.0;\n"
    "    int2 q = p;\n"
    "    end = float2(0.0);\n"
    "    while (z < float(MAX_SEARCH_DIAG - 1) && w > 0.9) {\n"
    "        q += dir;\n"
    "        z += 1.0;\n"
    "        end = float2(E(edges, two ? q + int2(1, 0) : q, sz, 0), E(edges, q, sz, 1));\n"
    "        w = dot(end, float2(0.5));\n"
    "    }\n"
    "    return float2(z, w);\n"
    "}\n"
    "static float2 area_diag(texture2d<float> area, float2 dist, float2 cc, int set)\n"
    "{\n"
    "    return area.read(uint2(20.0 * cc + dist) + uint2(0, 80 * set)).rg;\n"
    "}\n"
    "static float2 diag_weights(texture2d<float> edges, texture2d<float> area, int2 p, int2 sz, float2 e, int2 set)\n"
    "{\n"
    "    float2 w = float2(0.0), end;\n"
    "    float4 d;\n"
    "    if (e.x > 0.5) {\n"
    "        d.xz = search_diag(edges, p, int2(-1, 1), sz, false, end);\n"
    "        d.x += float(end.y > 0.9);\n"
    "    } else\n"
    "        d.xz = float2(0.0);\n"
    "    d.yw = search_diag(edges, p, int2(1, -1), sz, false, end);\n"
    "    if (d.x + d.y > 2.0) {\n"
    "        int2 a = p + int2(-int(d.x), int(d.x)), b = p + int2(int(d.y), -int(d.y));\n"
    "        float4 c = float4(E(edges, a + int2(-1, 0), sz, 1), E(edges, a, sz, 0), E(edges, b + int2(1, 0), sz, 1),\n"
    "                          E(edges, b + int2(1, -1), sz, 0));\n"
    "        float2 cc = select(2.0 * c.xz + c.yw, float2(0.0), d.zw >= 0.9);\n"
    "        w += area_diag(area, d.xy, cc, set.x);\n"
    "    }\n"
    "    d.xz = search_diag(edges, p, int2(-1, -1), sz, true, end);\n"
    "    if (E(edges, p + int2(1, 0), sz, 0) > 0.5) {\n"
    "        d.yw = search_diag(edges, p, int2(1, 1), sz, true, end);\n"
    "        d.y += float(end.y > 0.9);\n"
    "    } else\n"
    "        d.yw = float2(0.0);\n"
    "    if (d.x + d.y > 2.0) {\n"
    "        int2 a = p - int2(int(d.x)), b = p + int2(int(d.y));\n"
    "        float4 c = float4(E(edges, a + int2(-1, 0), sz, 1), E(edges, a + int2(0, -1), sz, 0),\n"
    "                          E(edges, b + int2(1, 0), sz, 1), E(edges, b + int2(1, 0), sz, 0));\n"
    "        float2 cc = select(2.0 * c.xz + c.yw, float2(0.0), d.zw >= 0.9);\n"
    "        w += area_diag(area, d.xy, cc, set.y).yx;\n"
    "    }\n"
    "    return w;\n"
    "}\n"
    /* Pass 2: blend weights.
       Edge on the top side (horizontal run along x, crossing edges are left
       edges): r = part of the pixel above that this pixel takes, g = part of
       this pixel that the pixel above takes.
       Edge on the left side (vertical run along y, crossing edges are top
       edges): b = part of the left pixel that this pixel takes, a = part of
       this pixel that the left pixel takes.
       The areas come from the orthogonal area texture (SMAAArea), with the
       exact distances (the reference stores the square roots of 16
       distances and filters between them).
       si: the subsample sets of SMAA T2x (@SUBSAMPLE_INDICES of the
       reference): x vertical, y horizontal, z and w the two diagonal
       directions; all 0 for SMAA 1x.
       Corner detection (SMAA corner rounding 25): when the crossing edge at
       an end continues for one more pixel away from the run, the end is a
       corner of a shape and not a step of a line. The weight of that side
       then decreases by 0.75 of the weight at the nearer end (0.375 at each
       end for a pixel at the middle of the run), so corners stay sharp. */
    "fragment float4 smaa_weights(PV in [[stage_in]], texture2d<float> edges [[texture(0)]],\n"
    "    texture2d<float> area [[texture(1)]], texture2d<float> ortho [[texture(2)]], constant int4 &si [[buffer(0)]])\n"
    "{\n"
    "    int2 sz = int2(edges.get_width(), edges.get_height());\n"
    "    int2 p = int2(in.pos.xy);\n"
    "    float2 e = edges.read(uint2(p)).rg;\n"
    "    float4 w = float4(0.0);\n"
    /* A diagonal has priority: it skips the horizontal and the vertical
       runs of the pixel. */
    "    if (e.y > 0.5) {\n"
    "        float2 dw = diag_weights(edges, area, p, sz, e, si.zw);\n"
    "        if (dw.x + dw.y > 0.0) return float4(dw, 0.0, 0.0);\n"
    "    }\n"
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
    "        int n = (okl ? int(lo1 > 0.5) | int(up1 > 0.5) << 2 : 0) | (okr ? int(lo2 > 0.5) << 1 | int(up2 > 0.5) << 3 : 0);\n"
    "        float2 ar = area_ortho(ortho, n, p.x - xl, xr - p.x, si.y);\n"
    "        float2 cf = corner(float2(float(p.x - xl), float(xr - p.x)),\n"
    "            float4(E(edges, int2(xl, p.y + 1), sz, 0), E(edges, int2(xr + 1, p.y + 1), sz, 0),\n"
    "                   E(edges, int2(xl, p.y - 2), sz, 0), E(edges, int2(xr + 1, p.y - 2), sz, 0)));\n"
    "        w.r = ar.x * cf.x; w.g = ar.y * cf.y;\n"
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
    "        int n = (okt ? int(cu1 > 0.5) | int(le1 > 0.5) << 2 : 0) | (okb ? int(cu2 > 0.5) << 1 | int(le2 > 0.5) << 3 : 0);\n"
    "        float2 ar = area_ortho(ortho, n, p.y - yt, yb - p.y, si.x);\n"
    "        float2 cf = corner(float2(float(p.y - yt), float(yb - p.y)),\n"
    "            float4(E(edges, int2(p.x + 1, yt), sz, 1), E(edges, int2(p.x + 1, yb + 1), sz, 1),\n"
    "                   E(edges, int2(p.x - 2, yt), sz, 1), E(edges, int2(p.x - 2, yb + 1), sz, 1)));\n"
    "        w.b = ar.x * cf.x; w.a = ar.y * cf.y;\n"
    "    }\n"
    "    return w;\n"
    "}\n"
    /* Pass 3: blend (SMAANeighborhoodBlendingPS). The pixel blends along one
       axis only: horizontal when the largest left or right weight is larger
       than the largest top or bottom weight, else vertical. The two weights
       of that axis are normalized; each one mixes the pixel with the
       neighbor on its side by that weight (the reference does this with a
       bilinear read at the weight offset, which gives the same mix). */
    "struct BW { bool any; int2 d; float2 o, w; };\n"
    "static BW blend_weights(texture2d<float> wt, int2 p, int2 sz)\n"
    "{\n"
    "    BW r;\n"
    "    float4 w = wt.read(uint2(p));\n"
    "    float4 a = float4(p.x + 1 < sz.x ? wt.read(uint2(p + int2(1, 0))).a : 0.0,\n"
    "                      p.y + 1 < sz.y ? wt.read(uint2(p + int2(0, 1))).g : 0.0, w.b, w.r);\n"
    "    bool h = max(a.x, a.z) > max(a.y, a.w);\n"
    "    r.any = dot(a, float4(1.0)) >= 1e-5;\n"
    "    r.o = h ? a.xz : a.yw;\n"
    "    r.w = r.any ? r.o / (r.o.x + r.o.y) : float2(0.0);\n"
    "    r.d = h ? int2(1, 0) : int2(0, 1);\n"
    "    return r;\n"
    "}\n"
    "fragment float4 smaa_blend(PV in [[stage_in]], texture2d<float> src [[texture(0)]],\n"
    "    texture2d<float> wt [[texture(1)]])\n"
    "{\n"
    "    int2 sz = int2(src.get_width(), src.get_height());\n"
    "    int2 p = int2(in.pos.xy);\n"
    "    float4 c = src.read(uint2(p));\n"
    "    BW b = blend_weights(wt, p, sz);\n"
    "    if (!b.any) return c;\n"
    "    float4 n1 = src.read(uint2(clampc(p + b.d, sz))), n2 = src.read(uint2(clampc(p - b.d, sz)));\n"
    "    return b.w.x * mix(c, n1, b.o.x) + b.w.y * mix(c, n2, b.o.y);\n"
    "}\n"
    /* SMAA T2x (rt_d3d9_post.m d3d_post_t2x). smaa_blend_t2x: the blend
       with SMAA_REPROJECTION: alpha = sqrt(5 * |velocity|), with the
       velocity blended as the color. t2x_velocity: the screen motion of each
       pixel from the previous frame (texture coordinates, current minus
       previous), from the depth and the camera of the two frames (inv:
       clip to world of this frame, prev: world to clip of the previous
       frame). A point behind the previous camera gets a velocity outside
       the screen. t2x_resolve: SMAAResolvePS with SMAA_REPROJECTION and the
       weight scale 30, and a clamp of the previous color to the range of
       the 3 x 3 pixels of the current frame (not in the reference: the game
       has no velocity for moving objects, and the clamp limits their
       ghosting). hist 0: no previous frame. */
    "fragment float4 smaa_blend_t2x(PV in [[stage_in]], texture2d<float> src [[texture(0)]],\n"
    "    texture2d<float> wt [[texture(1)]], texture2d<float> vel [[texture(2)]])\n"
    "{\n"
    "    int2 sz = int2(src.get_width(), src.get_height());\n"
    "    int2 p = int2(in.pos.xy);\n"
    "    float4 c = src.read(uint2(p));\n"
    "    float2 v = vel.read(uint2(p)).xy;\n"
    "    BW b = blend_weights(wt, p, sz);\n"
    "    if (b.any) {\n"
    "        float4 n1 = src.read(uint2(clampc(p + b.d, sz))), n2 = src.read(uint2(clampc(p - b.d, sz)));\n"
    "        float2 v1 = vel.read(uint2(clampc(p + b.d, sz))).xy, v2 = vel.read(uint2(clampc(p - b.d, sz))).xy;\n"
    "        c = b.w.x * mix(c, n1, b.o.x) + b.w.y * mix(c, n2, b.o.y);\n"
    "        v = b.w.x * mix(v, v1, b.o.x) + b.w.y * mix(v, v2, b.o.y);\n"
    "    }\n"
    "    return float4(c.rgb, sqrt(5.0 * length(v)));\n"
    "}\n"
    "struct T2XV { float4x4 inv, prev; };\n"
    "static float4 velocity_of(float z, int2 p, int2 sz, constant T2XV &m)\n"
    "{\n"
    "    float2 uv = (float2(p) + 0.5) / float2(sz);\n"
    "    float4 wp = m.inv * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);\n"
    "    float4 cp = m.prev * (wp / wp.w);\n"
    "    if (!(cp.w > 1e-6)) return float4(2.0, 2.0, 0.0, 0.0);\n"
    "    float2 q = cp.xy / cp.w;\n"
    "    return float4(uv - float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5), 0.0, 0.0);\n"
    "}\n"
    "fragment float4 t2x_velocity(PV in [[stage_in]], depth2d<float> dep [[texture(0)]], constant T2XV &m [[buffer(0)]])\n"
    "{\n"
    "    int2 p = int2(in.pos.xy), sz = int2(dep.get_width(), dep.get_height());\n"
    "    return velocity_of(dep.read(uint2(p)), p, sz, m);\n"
    "}\n"
    "fragment float4 t2x_velocity_ms(PV in [[stage_in]], depth2d_ms<float> dep [[texture(0)]], constant T2XV &m [[buffer(0)]])\n"
    "{\n"
    "    int2 p = int2(in.pos.xy), sz = int2(dep.get_width(), dep.get_height());\n"
    "    return velocity_of(dep.read(uint2(p), 0), p, sz, m);\n"
    "}\n"
    "fragment float4 t2x_resolve(PV in [[stage_in]], texture2d<float> cur [[texture(0)]],\n"
    "    texture2d<float> prev [[texture(1)]], texture2d<float> vel [[texture(2)]], constant uint &hist [[buffer(0)]])\n"
    "{\n"
    "    constexpr sampler ps(filter::nearest, address::clamp_to_edge);\n"
    "    int2 sz = int2(cur.get_width(), cur.get_height());\n"
    "    int2 p = int2(in.pos.xy);\n"
    "    float4 c = cur.read(uint2(p));\n"
    "    float2 uv = (float2(p) + 0.5) / float2(sz) - vel.read(uint2(p)).xy;\n"
    "    if (hist == 0u || any(uv < 0.0) || any(uv > 1.0)) return float4(c.rgb, 1.0);\n"
    "    float4 pr = prev.sample(ps, uv);\n"
    "    float delta = abs(c.a * c.a - pr.a * pr.a) / 5.0;\n"
    "    float k = 0.5 * saturate(1.0 - sqrt(delta) * 30.0);\n"
    "    float3 mn = c.rgb, mx = c.rgb;\n"
    "    for (int y = -1; y <= 1; y++)\n"
    "        for (int x = -1; x <= 1; x++) {\n"
    "            float3 n = cur.read(uint2(clampc(p + int2(x, y), sz))).rgb;\n"
    "            mn = min(mn, n);\n"
    "            mx = max(mx, n);\n"
    "        }\n"
    "    return float4(mix(c.rgb, clamp(pr.rgb, mn, mx), k), 1.0);\n"
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
    "    return float4(max(r, 0.0), e4.a);\n"
    "}\n";

@interface D3DPost : NSObject
{
@public
    id<MTLLibrary> lib;
    id<MTLRenderPipelineState> edges_pso, edges_pred_pso, edges_pred_ms_pso, weights_pso, blend_pso, cas_pso;
    bool pred_logged;
    MTLPixelFormat pso_format;
    id<MTLTexture> edges, weights, smaa_out, cas_out, fx_out, area_diag, area_ortho;
    /* SMAA T2x (d3d_post_t2x). t2x_col: the blended color with the
       velocity weight in alpha of the even and the odd frame; t2x_vel: the
       velocity of this frame. prev_vp: the camera of the last resolved frame,
       prev_frame: present_count + 1 of that frame (0 = none). live: the last
       frame had the resolve, so this frame has the jitter. */
    id<MTLRenderPipelineState> blend_t2x_pso, vel_pso, vel_ms_pso, resolve_pso;
    MTLPixelFormat t2x_format;
    id<MTLTexture> t2x_col[2], t2x_vel;
    float prev_vp[4][4];
    uint32_t prev_frame;
    bool t2x_live, t2x_logged, t2x_skip_logged;
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

/* The subsample offsets of the area textures (SUBSAMPLE_OFFSETS_ORTHO and
   SUBSAMPLE_OFFSETS_DIAG of AreaTex.py): set 0 for SMAA 1x, sets 1 and 2
   for the two frames of SMAA T2x. */
#define AREA_SETS 3
static const double ortho_offsets[AREA_SETS] = { 0.0, -0.25, 0.25 };
static const double diag_offsets[AREA_SETS][2] = { { 0.0, 0.0 }, { 0.25, -0.25 }, { -0.25, 0.25 } };

/* The orthogonal SMAA area texture: a port of areaortho, area and
   smootharea of AreaTex.py. Texel (33 * pattern + left, 33 * set + right)
   has the two areas (r, g) of the pattern for the distances left and right
   (0 to 32, not compressed) and the subsample set, as 8-bit values (the
   script truncates 255 * area). */
#define ORTHO_SIZE 33
#define ORTHO_SMOOTH 32.0

/* The area under the line p1 -> p2 for the pixel x .. x + 1 (area of
   areaortho). */
static void ortho_area1(double p1x, double p1y, double p2x, double p2y, double x, double r[2])
{
    double dx = p2x - p1x, dy = p2y - p1y, x1 = x, x2 = x + 1.0;
    double y1 = p1y + dy * (x1 - p1x) / dx, y2 = p1y + dy * (x2 - p1x) / dx, a;
    r[0] = r[1] = 0.0;
    if (!((x1 >= p1x && x1 < p2x) || (x2 > p1x && x2 <= p2x))) return;
    if (copysign(1.0, y1) == copysign(1.0, y2) || fabs(y1) < 1e-4 || fabs(y2) < 1e-4)
    {
        a = (y1 + y2) / 2.0;
        if (a < 0.0)
            r[0] = fabs(a);
        else
            r[1] = fabs(a);
    }
    else
    {
        double xc = -p1y * dx / dy + p1x, ip, fr = modf(xc, &ip);
        double a1 = xc > p1x ? y1 * fr / 2.0 : 0.0, a2 = xc < p2x ? y2 * (1.0 - fr) / 2.0 : 0.0;
        a = fabs(a1) > fabs(a2) ? a1 : -a2;
        r[0] = a < 0.0 ? fabs(a1) : fabs(a2);
        r[1] = a < 0.0 ? fabs(a2) : fabs(a1);
    }
}

/* The sum of the two halves of a U pattern after smootharea. */
static void ortho_smooth(double d, const double a1[2], const double a2[2], double r[2])
{
    double p = fmin(fmax(d / ORTHO_SMOOTH, 0.0), 1.0);
    int k;
    for (k = 0; k < 2; k++)
    {
        double b1 = sqrt(a1[k] * 2.0) * 0.5, b2 = sqrt(a2[k] * 2.0) * 0.5;
        r[k] = b1 + (a1[k] - b1) * p + b2 + (a2[k] - b2) * p;
    }
}

/* The mean of the full Z line and the two L halves (Z patterns 6 and 9 with
   an offset). */
static void ortho_z(double d, double ya, double yb, int left, double r[2])
{
    double a1[2], a2[2], a3[2];
    ortho_area1(0.0, ya, d, yb, left, a1);
    ortho_area1(0.0, ya, d / 2.0, 0.0, left, a2);
    ortho_area1(d / 2.0, 0.0, d, yb, left, a3);
    r[0] = (a1[0] + a2[0] + a3[0]) / 2.0;
    r[1] = (a1[1] + a2[1] + a3[1]) / 2.0;
}

static void ortho_area(int pattern, int left, int right, double offset, double r[2])
{
    double d = (double)(left + right + 1), o1 = 0.5 + offset, o2 = 0.5 + offset - 1.0, a1[2], a2[2];
    r[0] = r[1] = 0.0;
    switch (pattern)
    {
    case 1:
        if (left <= right) ortho_area1(0.0, o2, d / 2.0, 0.0, left, r);
        break;
    case 2:
        if (left >= right) ortho_area1(d / 2.0, 0.0, d, o2, left, r);
        break;
    case 3:
        ortho_area1(0.0, o2, d / 2.0, 0.0, left, a1);
        ortho_area1(d / 2.0, 0.0, d, o2, left, a2);
        ortho_smooth(d, a1, a2, r);
        break;
    case 4:
        if (left <= right) ortho_area1(0.0, o1, d / 2.0, 0.0, left, r);
        break;
    case 6:
        if (fabs(offset) > 0.0)
            ortho_z(d, o1, o2, left, r);
        else
            ortho_area1(0.0, o1, d, o2, left, r);
        break;
    case 7:
    case 14:
        ortho_area1(0.0, o1, d, o2, left, r);
        break;
    case 8:
        if (left >= right) ortho_area1(d / 2.0, 0.0, d, o1, left, r);
        break;
    case 9:
        if (fabs(offset) > 0.0)
            ortho_z(d, o2, o1, left, r);
        else
            ortho_area1(0.0, o2, d, o1, left, r);
        break;
    case 11:
    case 13:
        ortho_area1(0.0, o2, d, o1, left, r);
        break;
    case 12:
        ortho_area1(0.0, o1, d / 2.0, 0.0, left, a1);
        ortho_area1(d / 2.0, 0.0, d, o1, left, a2);
        ortho_smooth(d, a1, a2, r);
        break;
    default: /* 0, 5, 10, 15: no area */
        break;
    }
}

static id<MTLTexture> ortho_texture(D3DDevice *d, D3DPost *p)
{
    enum { W = 16 * ORTHO_SIZE, H = AREA_SETS * ORTHO_SIZE };
    static uint8_t data[W * H * 2];
    MTLTextureDescriptor *td;
    int set, pat, left, right;
    if (p->area_ortho != nil) return p->area_ortho;
    for (set = 0; set < AREA_SETS; set++)
        for (pat = 0; pat < 16; pat++)
            for (left = 0; left < ORTHO_SIZE; left++)
                for (right = 0; right < ORTHO_SIZE; right++)
                {
                    double r[2];
                    size_t i = ((size_t)(set * ORTHO_SIZE + right) * W + (size_t)(pat * ORTHO_SIZE + left)) * 2u;
                    ortho_area(pat, left, right, ortho_offsets[set], r);
                    data[i] = (uint8_t)(255.0 * fmin(r[0], 1.0));
                    data[i + 1] = (uint8_t)(255.0 * fmin(r[1], 1.0));
                }
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead;
    p->area_ortho = [d->mtl newTextureWithDescriptor:td];
    if (p->area_ortho == nil) rt_fatal("D3D9: out of video memory (SMAA orthogonal area texture)");
    [p->area_ortho replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:data bytesPerRow:W * 2];
    rt_log("D3D9: SMAA orthogonal area texture %ux%u (%d subsample sets)", (unsigned)W, (unsigned)H, AREA_SETS);
    return p->area_ortho;
}

/* The diagonal SMAA area texture: a port of areadiag, area, area1, tex2ddiag
   and tex4ddiag of AreaTex.py, for the three subsample sets stacked
   vertically (80 rows each). Texel (20 * ex + left, 80 * set + 20 * ey +
   right) has the two areas of the diagonal pattern with the crossing edge
   case (ex, ey) and the distances left and right, as 8-bit values (the
   script truncates 255 * area). The offset of the set moves each end of
   the line that has a crossing edge. The areas come from 30 x 30 samples of
   each pixel (the same sampling as the script). */
#define DIAG_SIZE 20
#define DIAG_SAMPLES 30

/* The part of the samples of the pixel at (px, py) on the positive side of
   the line p1 -> p2 (all samples when p1 = p2). */
static double diag_area1(const double p1[2], const double p2[2], double px, double py)
{
    double xm, ym, a, b, n = 0.0;
    int x, y;
    if (p1[0] == p2[0] && p1[1] == p2[1]) return 1.0;
    xm = (p1[0] + p2[0]) / 2.0;
    ym = (p1[1] + p2[1]) / 2.0;
    a = p2[1] - p1[1];
    b = p1[0] - p2[0];
    for (x = 0; x < DIAG_SAMPLES; x++)
        for (y = 0; y < DIAG_SAMPLES; y++)
        {
            double sx = px + (double)x / (double)(DIAG_SAMPLES - 1), sy = py + (double)y / (double)(DIAG_SAMPLES - 1);
            if (a * (sx - xm) + b * (sy - ym) > 0.0) n += 1.0;
        }
    return n / (double)(DIAG_SAMPLES * DIAG_SAMPLES);
}

static void diag_area_texture(uint8_t *out)
{
    /* The crossing edge case (ex, ey) of each pattern (edgesdiag). */
    static const uint8_t edges[16][2] = { { 0, 0 }, { 1, 0 }, { 0, 2 }, { 1, 2 }, { 2, 0 }, { 3, 0 }, { 2, 2 }, { 3, 2 },
                                          { 0, 1 }, { 1, 1 }, { 0, 3 }, { 1, 3 }, { 2, 1 }, { 3, 1 }, { 2, 3 }, { 3, 3 } };
    /* Each pattern is the mean of two lines: start p1, and end p2 + (d, d).
       A pattern with one line has it twice. */
    static const uint8_t lines[16][2][4] = {
        { { 1, 1, 1, 1 }, { 1, 0, 1, 0 } }, { { 1, 0, 0, 0 }, { 1, 0, 1, 0 } }, { { 0, 0, 1, 0 }, { 1, 0, 1, 0 } },
        { { 1, 0, 1, 0 }, { 1, 0, 1, 0 } }, { { 1, 1, 0, 0 }, { 1, 1, 1, 0 } }, { { 1, 1, 0, 0 }, { 1, 0, 1, 0 } },
        { { 1, 1, 1, 0 }, { 1, 1, 1, 0 } }, { { 1, 1, 1, 0 }, { 1, 0, 1, 0 } }, { { 0, 0, 1, 1 }, { 1, 0, 1, 1 } },
        { { 1, 0, 1, 1 }, { 1, 0, 1, 1 } }, { { 0, 0, 1, 1 }, { 1, 0, 1, 0 } }, { { 1, 0, 1, 1 }, { 1, 0, 1, 0 } },
        { { 1, 1, 1, 1 }, { 1, 1, 1, 1 } }, { { 1, 1, 1, 1 }, { 1, 0, 1, 1 } }, { { 1, 1, 1, 1 }, { 1, 1, 1, 0 } },
        { { 1, 1, 1, 1 }, { 1, 0, 1, 0 } } };
    int set, pat, left, right, k;
    for (set = 0; set < AREA_SETS; set++)
        for (pat = 0; pat < 16; pat++)
            for (left = 0; left < DIAG_SIZE; left++)
                for (right = 0; right < DIAG_SIZE; right++)
                {
                    double dd = (double)(left + right + 1), r[2] = { 0.0, 0.0 };
                    const double *off = diag_offsets[set];
                    size_t i = ((size_t)(4 * DIAG_SIZE * set + DIAG_SIZE * edges[pat][1] + right) * (4u * DIAG_SIZE) +
                                (size_t)(DIAG_SIZE * edges[pat][0] + left)) * 2u;
                    for (k = 0; k < 2; k++)
                    {
                        const uint8_t *l = lines[pat][k];
                        double p1[2] = { l[0], l[1] }, p2[2] = { l[2] + dd, l[3] + dd };
                        if (edges[pat][0] > 0)
                        {
                            p1[0] += off[0];
                            p1[1] += off[1];
                        }
                        if (edges[pat][1] > 0)
                        {
                            p2[0] += off[0];
                            p2[1] += off[1];
                        }
                        r[0] += 1.0 - diag_area1(p1, p2, 1.0 + left, 0.0 + left);
                        r[1] += diag_area1(p1, p2, 1.0 + left, 1.0 + left);
                    }
                    out[i] = (uint8_t)(255.0 * (r[0] / 2.0));
                    out[i + 1] = (uint8_t)(255.0 * (r[1] / 2.0));
                }
}

static id<MTLTexture> diag_texture(D3DDevice *d, D3DPost *p)
{
    enum { W = 4 * DIAG_SIZE, H = AREA_SETS * W };
    static uint8_t data[W * H * 2];
    MTLTextureDescriptor *td;
    if (p->area_diag != nil) return p->area_diag;
    diag_area_texture(data);
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead;
    p->area_diag = [d->mtl newTextureWithDescriptor:td];
    if (p->area_diag == nil) rt_fatal("D3D9: out of video memory (SMAA diagonal area texture)");
    [p->area_diag replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:data bytesPerRow:W * 2];
    rt_log("D3D9: SMAA diagonal area texture %ux%u (%d subsample sets)", (unsigned)W, (unsigned)H, AREA_SETS);
    return p->area_diag;
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
                      id<MTLTexture> t2, const void *bytes, NSUInteger nbytes, const char *what)
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
    if (t2 != nil) [e setFragmentTexture:t2 atIndex:2];
    if (bytes != NULL) [e setFragmentBytes:bytes length:nbytes atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

static void ensure_psos(D3DDevice *d, D3DPost *p, MTLPixelFormat fmt)
{
    if (p->edges_pso != nil && p->pso_format == fmt) return;
    p->edges_pso = post_pso(d, p, "smaa_edges", MTLPixelFormatRG8Unorm);
    p->edges_pred_pso = post_pso(d, p, "smaa_edges_pred", MTLPixelFormatRG8Unorm);
    p->edges_pred_ms_pso = post_pso(d, p, "smaa_edges_pred_ms", MTLPixelFormatRG8Unorm);
    p->weights_pso = post_pso(d, p, "smaa_weights", MTLPixelFormatRGBA8Unorm);
    p->blend_pso = post_pso(d, p, "smaa_blend", fmt);
    p->cas_pso = post_pso(d, p, "cas_fs", fmt);
    p->pso_format = fmt;
}

/* The pipelines of SMAA T2x for the back buffer format fmt. The resolve
   writes only the color channels of the back buffer. */
static void ensure_t2x_psos(D3DDevice *d, D3DPost *p, MTLPixelFormat fmt)
{
    MTLRenderPipelineDescriptor *pd;
    NSError *err = nil;
    if (p->resolve_pso != nil && p->t2x_format == fmt) return;
    p->blend_t2x_pso = post_pso(d, p, "smaa_blend_t2x", fmt);
    p->vel_pso = post_pso(d, p, "t2x_velocity", MTLPixelFormatRG16Float);
    p->vel_ms_pso = post_pso(d, p, "t2x_velocity_ms", MTLPixelFormatRG16Float);
    pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [p->lib newFunctionWithName:@"post_vs"];
    pd.fragmentFunction = [p->lib newFunctionWithName:@"t2x_resolve"];
    if (pd.vertexFunction == nil || pd.fragmentFunction == nil) rt_fatal("D3D9: post-processing function t2x_resolve is missing");
    pd.colorAttachments[0].pixelFormat = fmt;
    pd.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
    p->resolve_pso = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
    if (p->resolve_pso == nil)
        rt_fatal("D3D9: post-processing pipeline t2x_resolve creation failed: %s", err.localizedDescription.UTF8String);
    p->t2x_format = fmt;
}

/* The inverse of the 4 x 4 matrix m (cofactors); false if m has no
   inverse. */
static bool invert4(const double m[16], double r[16])
{
    double inv[16], det;
    int i;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] -
             m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] +
             m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] -
             m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] +
              m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] +
             m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] -
             m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] +
             m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] -
              m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] -
             m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] +
             m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] -
              m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] +
              m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] +
             m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] -
             m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] +
              m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(fabs(det) > 1e-30) || !isfinite(det)) return false;
    for (i = 0; i < 16; i++) r[i] = inv[i] / det;
    return true;
}

bool d3d_post_t2x_jitter(D3DDevice *d, float j[2])
{
    D3DPost *p = (D3DPost *)d->post;
    float s;
    if (rt_enh_get(RT_ENH_SMAA) != 2 || p == nil || !p->t2x_live || d->t2x_frame == d->present_count + 1u) return false;
    /* SMAA T2x jitters (0.25, -0.25) and (-0.25, 0.25) pixels with y up:
       (0.25, 0.25) and (-0.25, -0.25) with y down. */
    s = (d->present_count & 1u) == 0 ? 0.25f : -0.25f;
    j[0] = s;
    j[1] = s;
    return true;
}

bool d3d_post_t2x(D3DDevice *d)
{
    const MTLTextureUsage rt_read = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    D3DPost *p;
    D3DSurface *bb;
    id<MTLTexture> src, depth = nil, e, w, vel, col, prev;
    float vpm[4][4], pz[2];
    double vpd[16], inv[16];
    struct
    {
        float inv[16], prev[16];
    } m;
    int32_t si[4];
    uint32_t hist, cur = d->present_count & 1u;
    int set, i, j;
    MTLRenderPassDescriptor *rp;
    id<MTLRenderCommandEncoder> enc;
    if (rt_enh_get(RT_ENH_SMAA) != 2 || d->t2x_frame == d->present_count + 1u || d->backbuffer == NULL ||
        d->rt[0] != d->backbuffer)
        return false;
    bb = d3d_surf(d->backbuffer);
    src = bb->tex;
    if (src == nil || src.sampleCount != 1 || bb->scale != 1u) return false;
    if (!d3d_ao_camera(d, vpm, pz, &depth) || depth.width != src.width || depth.height != src.height) return false;
    /* The camera of the depth: clip = vp * (x, y, z, 1), rows of vpm. */
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) vpd[i * 4 + j] = vpm[i][j];
    if (!invert4(vpd, inv)) return false;
    p = post_of(d);
    ensure_psos(d, p, src.pixelFormat);
    ensure_t2x_psos(d, p, src.pixelFormat);
    d3d_end_pass(d);
    e = post_tex(d, &p->edges, src.width, src.height, MTLPixelFormatRG8Unorm, rt_read);
    w = post_tex(d, &p->weights, src.width, src.height, MTLPixelFormatRGBA8Unorm, rt_read);
    vel = post_tex(d, &p->t2x_vel, src.width, src.height, MTLPixelFormatRG16Float, rt_read);
    col = post_tex(d, &p->t2x_col[cur], src.width, src.height, src.pixelFormat, rt_read);
    prev = post_tex(d, &p->t2x_col[cur ^ 1u], src.width, src.height, src.pixelFormat, rt_read);
    /* The subsample set of this frame: 1 and 2 for the two jitters, 0 when
       this frame has no jitter (the first frame of T2x). */
    set = p->t2x_live ? (int)cur + 1 : 0;
    si[0] = si[1] = si[2] = set;
    si[3] = 0;
    {
        const float pr[4] = { 1.0f, pz[0], pz[1], 0.0f };
        post_pass(d, depth.sampleCount > 1 ? p->edges_pred_ms_pso : p->edges_pred_pso, e, src, depth, nil, pr, sizeof pr,
                  "SMAA T2x edges");
    }
    post_pass(d, p->weights_pso, w, e, diag_texture(d, p), ortho_texture(d, p), si, sizeof si, "SMAA T2x weights");
    /* MSL float4x4 is column-major: element (row i, column j) at j * 4 + i. */
    hist = p->prev_frame == d->present_count ? 1u : 0u;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
        {
            m.inv[j * 4 + i] = (float)inv[i * 4 + j];
            m.prev[j * 4 + i] = hist ? p->prev_vp[i][j] : vpm[i][j];
        }
    post_pass(d, depth.sampleCount > 1 ? p->vel_ms_pso : p->vel_pso, vel, depth, nil, nil, &m, sizeof m, "SMAA T2x velocity");
    post_pass(d, p->blend_t2x_pso, col, src, w, vel, NULL, 0, "SMAA T2x blend");
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = src;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    enc = [d->cb renderCommandEncoderWithDescriptor:rp];
    if (enc == nil) rt_fatal("D3D9: SMAA T2x resolve encoder creation failed");
    enc.label = @"SMAA T2x resolve";
    [enc setRenderPipelineState:p->resolve_pso];
    [enc setFragmentTexture:col atIndex:0];
    [enc setFragmentTexture:prev atIndex:1];
    [enc setFragmentTexture:vel atIndex:2];
    [enc setFragmentBytes:&hist length:sizeof hist atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    memcpy(p->prev_vp, vpm, sizeof vpm);
    p->prev_frame = d->present_count + 1u;
    d->t2x_frame = d->present_count + 1u;
    if (!p->t2x_logged)
    {
        rt_log("D3D9: SMAA T2x at the scene composite %lux%lu (subsample set %d, %s)", (unsigned long)src.width,
               (unsigned long)src.height, set, hist ? "with the previous frame" : "first frame");
        p->t2x_logged = true;
    }
    return true;
}

void d3d_post_frame_end(D3DDevice *d)
{
    D3DPost *p = (D3DPost *)d->post;
    bool t2x = rt_enh_get(RT_ENH_SMAA) == 2;
    if (p == nil) return;
    p->t2x_live = t2x && d->t2x_frame == d->present_count + 1u;
    if (t2x && !p->t2x_live && !p->t2x_skip_logged)
    {
        rt_log("D3D9: SMAA T2x: a frame without the scene composite and the scene camera gets SMAA 1x at the present");
        p->t2x_skip_logged = true;
    }
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
    /* SMAA T2x: a frame with the resolve at the scene composite
       (d3d_post_t2x) has its anti-aliasing. */
    if (smaa == 2 && d->t2x_frame == d->present_count + 1u) smaa = 0;
    if (!smaa && !fx && !sharpen) return src;
    if (smaa)
    {
        static const int32_t si1x[4] = { 0, 0, 0, 0 };
        id<MTLTexture> e = post_tex(d, &p->edges, src.width, src.height, MTLPixelFormatRG8Unorm, rt_read);
        id<MTLTexture> w = post_tex(d, &p->weights, src.width, src.height, MTLPixelFormatRGBA8Unorm, rt_read);
        id<MTLTexture> o = post_tex(d, &p->smaa_out, src.width, src.height, src.pixelFormat, rt_read);
        float vpm[4][4], pz[2];
        id<MTLTexture> depth = nil;
        if (d3d_ao_camera(d, vpm, pz, &depth) && depth.width == src.width && depth.height == src.height)
        {
            const float pr[4] = { 1.0f, pz[0], pz[1], 0.0f };
            if (!p->pred_logged)
            {
                rt_log("D3D9: SMAA depth predication with the scene depth %lux%lu", (unsigned long)depth.width,
                       (unsigned long)depth.height);
                p->pred_logged = true;
            }
            post_pass(d, depth.sampleCount > 1 ? p->edges_pred_ms_pso : p->edges_pred_pso, e, src, depth, nil, pr, sizeof pr,
                      "SMAA edges");
        }
        else
            post_pass(d, p->edges_pso, e, src, nil, nil, NULL, 0, "SMAA edges");
        post_pass(d, p->weights_pso, w, e, diag_texture(d, p), ortho_texture(d, p), si1x, sizeof si1x, "SMAA weights");
        post_pass(d, p->blend_pso, o, src, w, nil, NULL, 0, "SMAA blend");
        src = o;
    }
    if (fx && out_w > src.width && out_h > src.height) src = metalfx(d, p, src, out_w, out_h);
    if (sharpen)
    {
        static const float amount[4] = { 0.0f, 0.3f, 0.6f, 1.0f };
        float s = amount[sharpen];
        id<MTLTexture> o = post_tex(d, &p->cas_out, src.width, src.height, src.pixelFormat, rt_read);
        post_pass(d, p->cas_pso, o, src, nil, nil, &s, sizeof s, "sharpening");
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

float d3d_post_headroom(D3DDevice *d)
{
    return atomic_load(&post_of(d)->headroom);
}

/* GT7 tone mapping constants (RT_ENH_TONEMAP) for the scene pass of
   rt_d3d9_draw.m. Units: 1.0 = 100 nits. The game's white (1.0 in the
   frame) is the SDR paper white of GT7, 250 nits, so the shader multiplies
   the linear frame by 1 / out_scale = 2.5 before the curve and by
   out_scale after it. The peak is the paper white. The shoulder of the
   curve: k = (linear - 1) / (alpha - 1), ka = peak * (linear + k),
   kb = -peak * k * exp(linear / k), kc = -1 / (k * peak), with
   alpha 0.25 and linear 0.444. peak_ucs is the ICtCp intensity of a grey
   at the peak.
   SDR (the scene pass, hdr false): the peak is the paper white. expand is
   the highlight expansion at white: the input x of the shoulder that gives
   0.98 of the peak, divided by the paper white.
   HDR (hdr true): the input is the frame after the EDR expansion (white =
   headroom), and the peak is the paper white times the headroom. expand
   is the EDR expansion at white, the headroom (gt7_scene_fs uses it; the
   present shader uses its own headroom value, which is the same). */
bool d3d_post_gt7(d3d_gt7_params *g, bool hdr, float headroom)
{
    const double alpha = 0.25, lin = 0.444, white = 2.5, white_out = 0.98;
    const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    double peak, k, y, ka, kb, kc, xw;
    memset(g, 0, sizeof *g);
    if (rt_enh_get(RT_ENH_TONEMAP) == 0) return false;
    peak = white * (hdr && headroom > 1.0f ? (double)headroom : 1.0);
    k = (lin - 1.0) / (alpha - 1.0);
    y = pow(peak * 100.0 / 10000.0, m1);
    ka = peak * lin + peak * k;
    kb = -peak * k * exp(lin / k);
    kc = -1.0 / (k * peak);
    xw = log((white_out * peak - ka) / kb) / kc;
    g->peak = (float)peak;
    g->peak_ucs = (float)pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
    g->out_scale = (float)(1.0 / white);
    g->ka = (float)ka;
    g->kb = (float)kb;
    g->kc = (float)kc;
    g->expand = (float)(hdr ? (headroom > 1.0f ? (double)headroom : 1.0) : xw > white ? xw / white : 1.0);
    return true;
}
