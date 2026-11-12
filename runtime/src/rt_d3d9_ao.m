/*
 * Ambient occlusion for the Mac enhancement setting RT_ENH_AO (rt_enh.h):
 * a Metal port of the XeGTAO method (ground-truth ambient occlusion,
 * Jimenez et al. 2016, in the XeGTAO form of Intel).
 *
 * XeGTAO is available under this license:
 *
 *   MIT License
 *
 *   Copyright (C) 2016-2021, Intel Corporation
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The camera. The game gives no projection matrix to the shaders. The
 * vertex shaders of the scene have p3dWorldViewProjectionMatrix (WVP) and
 * p3dWorldMatrix (W) in their constant tables (d3d_shader.ct_wvp,
 * ct_world). d3d_ao_capture calculates VP = WVP * inverse(W) at each
 * scene draw into the full-size depth buffer. For a perspective projection
 * P and a view matrix V with orthonormal rotation, the rows of VP give:
 *   P00 = |row0.xyz|, P11 = |row1.xyz|, row3 = the view z axis,
 *   P22 = dot(row2.xyz, row3.xyz), P23 = row2.w - P22 * row3.w.
 * The view depth of a depth value z is P23 / (z - P22).
 * The register layout of the matrices (one register for each row or for
 * each column of the coefficients) comes from the class in the constant
 * table and the form of the multiplication in the shader. The capture
 * tries the two layouts and keeps the one that gives an affine W and a
 * unit row 3 of VP. A draw that gives no such matrix (orthographic
 * projection, a shadow map, a full-screen pass) gives no camera.
 *
 * The passes, at the composite draw of the frame (d3d_ao_apply):
 *   1. View depth of each pixel (mip 0) and four smaller mip levels with
 *      the XeGTAO depth filter, in one dispatch (threadgroup memory).
 *   2. The XeGTAO main pass: the normal from the depths, then for each
 *      slice direction the two horizon angles and the visibility integral.
 *      Slices and steps per slice: low 1 x 2, medium 2 x 2, high 3 x 3
 *      (the XeGTAO quality presets low, medium and high). The noise is the
 *      Hilbert curve index with the R2 sequence; the game has no temporal
 *      filter, so the noise does not change from frame to frame.
 *   3. Two passes of the XeGTAO edge-aware denoise filter. The second
 *      pass is a fragment shader that multiplies the scene texture
 *      (p3dFrameBufferCurrent of the composite shader) in place by the
 *      visibility with the GTAO multi-bounce approximation (the scene
 *      color is the albedo), so the result needs no texture of its own.
 * The main pass uses half precision for the slice directions, the angles
 * and the horizon cosines (the half-precision option of XeGTAO).
 * XeGTAO default settings: effect radius 0.5 world units with the radius
 * multiplier 1.457, falloff range 0.615, sample distribution power 2,
 * final value power 2.2, depth mip sampling offset 3.30, denoise blur beta
 * 1.2. One world unit of the game is assumed to be one meter
 * (UNCONFIRMED).
 *
 * Limits: the occlusion darkens the lit scene, not only the ambient light
 * (the game has no separate ambient term). A frame whose scene texture or
 * depth buffer does not have the expected size gets no occlusion, and the
 * log gets one message. This code has not run (UNCONFIRMED).
 */
#include "rt_d3d9_int.h"
#include "rt_enh.h"

#include <math.h>

#define AO_MIPS 5u
#define AO_SKY_DEPTH 1.0e6f

static const char ao_src[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    /* All members are scalars, so the layout agrees with ao_consts. */
    "struct AOC {\n"
    "    float sx, sy, px, py;\n"     /* viewport size and pixel size */
    "    float mulx, muly, addx, addy;\n" /* screen position (0..1) to view xy at depth 1 */
    "    float mpx, mpy, da, db;\n"   /* mul * pixel size; P22, P23 */
    "    float radius, falloff, power, mipoff;\n"
    "    float beta, pad;\n"
    "    uint slices, steps;\n"
    "};\n"
    "#define SKY 1.0e6\n"
    "static float view_z(float z, constant AOC &c)\n"
    "{\n"
    "    if (z >= 1.0) return SKY;\n"
    "    float den = z - c.da;\n"
    "    float w = abs(den) < 1e-12 ? SKY : c.db / den;\n"
    "    return clamp(w, 1e-4, SKY);\n"
    "}\n"
    /* XeGTAO depth mip filter: the mean of the four depths, with less
       weight for depths that are much nearer than the farthest one. */
    "static float depth_filter(float d0, float d1, float d2, float d3, constant AOC &c)\n"
    "{\n"
    "    float mx = max(max(d0, d1), max(d2, d3));\n"
    "    float er = 0.75 * c.radius, fr = c.falloff * er, ff = er * (1.0 - c.falloff);\n"
    "    float fm = -1.0 / fr, fa = ff / fr + 1.0;\n"
    "    float w0 = saturate((mx - d0) * fm + fa), w1 = saturate((mx - d1) * fm + fa);\n"
    "    float w2 = saturate((mx - d2) * fm + fa), w3 = saturate((mx - d3) * fm + fa);\n"
    "    return (w0 * d0 + w1 * d1 + w2 * d2 + w3 * d3) / (w0 + w1 + w2 + w3);\n"
    "}\n"
    /* All five depth mips in one dispatch, as the XeGTAO prefilter: a
       group of 8x8 threads covers 16x16 pixels of mip 0. Each thread
       converts 2x2 depths and writes one pixel of mip 1; threadgroup
       memory gives mips 2 to 4. Every thread reaches every barrier;
       only the writes check the bounds. */
    "static void prefilter(float d0, float d1, float d2, float d3, uint2 gid, uint2 lid,\n"
    "    texture2d<float, access::write> m0, texture2d<float, access::write> m1,\n"
    "    texture2d<float, access::write> m2, texture2d<float, access::write> m3,\n"
    "    texture2d<float, access::write> m4, threadgroup float (&sm)[8][8], constant AOC &c)\n"
    "{\n"
    "    uint2 b = gid * 2;\n"
    "    uint2 s0 = uint2(m0.get_width(), m0.get_height());\n"
    "    if (b.x < s0.x && b.y < s0.y) m0.write(float4(d0), b);\n"
    "    if (b.x + 1 < s0.x && b.y < s0.y) m0.write(float4(d1), b + uint2(1, 0));\n"
    "    if (b.x < s0.x && b.y + 1 < s0.y) m0.write(float4(d2), b + uint2(0, 1));\n"
    "    if (b.x + 1 < s0.x && b.y + 1 < s0.y) m0.write(float4(d3), b + uint2(1, 1));\n"
    "    float v = depth_filter(d0, d1, d2, d3, c);\n"
    "    if (gid.x < m1.get_width() && gid.y < m1.get_height()) m1.write(float4(v), gid);\n"
    "    sm[lid.y][lid.x] = v;\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    if ((lid.x & 1) == 0 && (lid.y & 1) == 0) {\n"
    "        v = depth_filter(sm[lid.y][lid.x], sm[lid.y][lid.x + 1], sm[lid.y + 1][lid.x], sm[lid.y + 1][lid.x + 1], c);\n"
    "        uint2 q = gid / 2;\n"
    "        if (q.x < m2.get_width() && q.y < m2.get_height()) m2.write(float4(v), q);\n"
    "    }\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    if ((lid.x & 1) == 0 && (lid.y & 1) == 0) sm[lid.y][lid.x] = v;\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    if ((lid.x & 3) == 0 && (lid.y & 3) == 0) {\n"
    "        v = depth_filter(sm[lid.y][lid.x], sm[lid.y][lid.x + 2], sm[lid.y + 2][lid.x], sm[lid.y + 2][lid.x + 2], c);\n"
    "        uint2 q = gid / 4;\n"
    "        if (q.x < m3.get_width() && q.y < m3.get_height()) m3.write(float4(v), q);\n"
    "    }\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    if ((lid.x & 3) == 0 && (lid.y & 3) == 0) sm[lid.y][lid.x] = v;\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    if (lid.x == 0 && lid.y == 0) {\n"
    "        v = depth_filter(sm[0][0], sm[0][4], sm[4][0], sm[4][4], c);\n"
    "        uint2 q = gid / 8;\n"
    "        if (q.x < m4.get_width() && q.y < m4.get_height()) m4.write(float4(v), q);\n"
    "    }\n"
    "}\n"
    "kernel void ao_prefilter(depth2d<float, access::read> dt [[texture(0)]],\n"
    "    texture2d<float, access::write> m0 [[texture(1)]], texture2d<float, access::write> m1 [[texture(2)]],\n"
    "    texture2d<float, access::write> m2 [[texture(3)]], texture2d<float, access::write> m3 [[texture(4)]],\n"
    "    texture2d<float, access::write> m4 [[texture(5)]], constant AOC &c [[buffer(0)]],\n"
    "    uint2 gid [[thread_position_in_grid]], uint2 lid [[thread_position_in_threadgroup]])\n"
    "{\n"
    "    threadgroup float sm[8][8];\n"
    "    uint2 mx = uint2(dt.get_width() - 1, dt.get_height() - 1), b = gid * 2;\n"
    "    float d0 = view_z(dt.read(min(b, mx)), c), d1 = view_z(dt.read(min(b + uint2(1, 0), mx)), c);\n"
    "    float d2 = view_z(dt.read(min(b + uint2(0, 1), mx)), c), d3 = view_z(dt.read(min(b + uint2(1, 1), mx)), c);\n"
    "    prefilter(d0, d1, d2, d3, gid, lid, m0, m1, m2, m3, m4, sm, c);\n"
    "}\n"
    "kernel void ao_prefilter_ms(depth2d_ms<float, access::read> dt [[texture(0)]],\n"
    "    texture2d<float, access::write> m0 [[texture(1)]], texture2d<float, access::write> m1 [[texture(2)]],\n"
    "    texture2d<float, access::write> m2 [[texture(3)]], texture2d<float, access::write> m3 [[texture(4)]],\n"
    "    texture2d<float, access::write> m4 [[texture(5)]], constant AOC &c [[buffer(0)]],\n"
    "    uint2 gid [[thread_position_in_grid]], uint2 lid [[thread_position_in_threadgroup]])\n"
    "{\n"
    "    threadgroup float sm[8][8];\n"
    "    uint2 mx = uint2(dt.get_width() - 1, dt.get_height() - 1), b = gid * 2;\n"
    "    float d0 = view_z(dt.read(min(b, mx), 0), c), d1 = view_z(dt.read(min(b + uint2(1, 0), mx), 0), c);\n"
    "    float d2 = view_z(dt.read(min(b + uint2(0, 1), mx), 0), c), d3 = view_z(dt.read(min(b + uint2(1, 1), mx), 0), c);\n"
    "    prefilter(d0, d1, d2, d3, gid, lid, m0, m1, m2, m3, m4, sm, c);\n"
    "}\n"
    "static float3 vpos(float2 sp, float z, constant AOC &c)\n"
    "{\n"
    "    return float3((float2(c.mulx, c.muly) * sp + float2(c.addx, c.addy)) * z, z);\n"
    "}\n"
    /* Edges (1 = no edge) on the left, right, top and bottom sides. */
    "static float4 edges_of(float z, float l, float r, float t, float b)\n"
    "{\n"
    "    float4 e = float4(l, r, t, b) - z;\n"
    "    float slr = (e.y - e.x) * 0.5, stb = (e.w - e.z) * 0.5;\n"
    "    float4 es = e + float4(slr, -slr, stb, -stb);\n"
    "    e = min(abs(e), abs(es));\n"
    "    return saturate(1.25 - e / (z * 0.011));\n"
    "}\n"
    "static uint hilbert(uint x, uint y)\n"
    "{\n"
    "    uint index = 0;\n"
    "    for (uint lvl = 32; lvl > 0; lvl /= 2) {\n"
    "        uint rx = (x & lvl) > 0 ? 1 : 0, ry = (y & lvl) > 0 ? 1 : 0;\n"
    "        index += lvl * lvl * ((3 * rx) ^ ry);\n"
    "        if (ry == 0) {\n"
    "            if (rx == 1) { x = 63 - x; y = 63 - y; }\n"
    "            uint t = x; x = y; y = t;\n"
    "        }\n"
    "    }\n"
    "    return index;\n"
    "}\n"
    "kernel void ao_main(texture2d<float> dz [[texture(0)]], texture2d<float, access::write> ao [[texture(1)]],\n"
    "    texture2d<float, access::write> eo [[texture(2)]], constant AOC &c [[buffer(0)]],\n"
    "    uint2 p [[thread_position_in_grid]])\n"
    "{\n"
    "    constexpr sampler pc(filter::nearest, mip_filter::nearest, address::clamp_to_edge);\n"
    "    if (p.x >= ao.get_width() || p.y >= ao.get_height()) return;\n"
    "    int2 ip = int2(p), mx = int2(dz.get_width() - 1, dz.get_height() - 1);\n"
    "    float zc = dz.read(p).r;\n"
    "    float zl = dz.read(uint2(clamp(ip + int2(-1, 0), int2(0), mx))).r;\n"
    "    float zr = dz.read(uint2(clamp(ip + int2(1, 0), int2(0), mx))).r;\n"
    "    float zt = dz.read(uint2(clamp(ip + int2(0, -1), int2(0), mx))).r;\n"
    "    float zb = dz.read(uint2(clamp(ip + int2(0, 1), int2(0), mx))).r;\n"
    "    float4 e = edges_of(zc, zl, zr, zt, zb);\n"
    "    eo.write(e, p);\n"
    "    if (zc >= SKY * 0.5) { ao.write(float4(1.0), p); return; }\n"
    "    float2 px = float2(c.px, c.py);\n"
    "    float2 sp = (float2(p) + 0.5) * px;\n"
    "    float3 C = vpos(sp, zc, c);\n"
    "    float3 nl = normalize(vpos(sp + float2(-px.x, 0.0), zl, c) - C);\n"
    "    float3 nr = normalize(vpos(sp + float2(px.x, 0.0), zr, c) - C);\n"
    "    float3 nt = normalize(vpos(sp + float2(0.0, -px.y), zt, c) - C);\n"
    "    float3 nb = normalize(vpos(sp + float2(0.0, px.y), zb, c) - C);\n"
    "    float4 acc = saturate(float4(e.x * e.z, e.z * e.y, e.y * e.w, e.w * e.x) + 0.01);\n"
    "    float3 N = normalize(acc.x * cross(nl, nt) + acc.y * cross(nt, nr) + acc.z * cross(nr, nb) +\n"
    "                         acc.w * cross(nb, nl));\n"
    /* Move the center a little toward the camera (depth precision). */
    "    C *= 0.9999;\n"
    "    float3 V = normalize(-C);\n"
    "    float sr = c.radius / (c.mpx * C.z);\n"
    "    float vis = saturate((10.0 - sr) / 100.0) * 0.5;\n"
    "    float mins = 1.3 / sr;\n"
    "    float fr = c.falloff * c.radius, ff = c.radius * (1.0 - c.falloff);\n"
    "    float fm = -1.0 / fr, fa = ff / fr + 1.0;\n"
    "    uint hi = hilbert(p.x % 64, p.y % 64);\n"
    "    float nslice = fract(0.5 + float(hi) * 0.75487766624669276);\n"
    "    float nsample = fract(0.5 + float(hi) * 0.56984029099805326);\n"
    /* Half precision for the directions, the angles and the horizon
       cosines, as XE_GTAO_USE_HALF_FLOAT_PRECISION of XeGTAO; positions,
       distances and sample offsets stay in float. */
    "    half3 Vh = half3(V), Nh = half3(N);\n"
    "    for (uint s = 0; s < c.slices; s++) {\n"
    "        half phi = half((float(s) + nslice) / float(c.slices)) * M_PI_H;\n"
    "        half cp = cos(phi), spi = sin(phi);\n"
    "        float2 om = float2(float(cp), -float(spi)) * sr;\n"
    "        half3 dv = half3(cp, spi, 0.0h);\n"
    "        half3 odv = dv - dot(dv, Vh) * Vh;\n"
    "        half3 ax = normalize(cross(odv, Vh));\n"
    "        half3 pn = Nh - ax * dot(Nh, ax);\n"
    "        half sgn = sign(dot(odv, pn));\n"
    "        half pl = length(pn);\n"
    "        half cn = saturate(dot(pn, Vh) / max(pl, 1e-4h));\n"
    "        half n = sgn * acos(cn);\n"
    "        half lo0 = cos(n + M_PI_2_H), lo1 = cos(n - M_PI_2_H);\n"
    "        half h0c = lo0, h1c = lo1;\n"
    "        for (uint j = 0; j < c.steps; j++) {\n"
    "            float sn = fract(nsample + float(s + j * c.steps) * 0.6180339887498948);\n"
    "            float st = (float(j) + sn) / float(c.steps);\n"
    "            st = st * st + mins;\n" /* sample distribution power 2 */
    "            float2 so = st * om;\n"
    "            float mip = clamp(log2(length(so)) - c.mipoff, 0.0, 4.0);\n"
    "            so = round(so) * px;\n"
    "            float2 s0 = sp + so, s1 = sp - so;\n"
    "            float3 d0 = vpos(s0, dz.sample(pc, s0, level(mip)).r, c) - C;\n"
    "            float3 d1 = vpos(s1, dz.sample(pc, s1, level(mip)).r, c) - C;\n"
    "            float l0 = length(d0), l1 = length(d1);\n"
    "            half w0 = half(saturate(l0 * fm + fa)), w1 = half(saturate(l1 * fm + fa));\n"
    "            half c0 = l0 > 0.0 ? half(dot(d0 / l0, V)) : lo0, c1 = l1 > 0.0 ? half(dot(d1 / l1, V)) : lo1;\n"
    "            h0c = max(h0c, mix(lo0, c0, w0));\n"
    "            h1c = max(h1c, mix(lo1, c1, w1));\n"
    "        }\n"
    "        pl = mix(pl, 1.0h, 0.05h);\n"
    "        half h0 = -acos(clamp(h1c, -1.0h, 1.0h)), h1 = acos(clamp(h0c, -1.0h, 1.0h));\n"
    "        h0 = n + clamp(h0 - n, -M_PI_2_H, M_PI_2_H);\n"
    "        h1 = n + clamp(h1 - n, -M_PI_2_H, M_PI_2_H);\n"
    "        half i0 = (cn + 2.0h * h0 * sin(n) - cos(2.0h * h0 - n)) * 0.25h;\n"
    "        half i1 = (cn + 2.0h * h1 * sin(n) - cos(2.0h * h1 - n)) * 0.25h;\n"
    "        vis += float(pl * (i0 + i1));\n"
    "    }\n"
    "    vis /= float(c.slices);\n"
    "    vis = pow(saturate(vis), c.power);\n"
    "    ao.write(float4(max(0.03, vis)), p);\n"
    "}\n"
    /* XeGTAO denoise: an edge-aware 3x3 filter. beta is the weight of the
       center pixel. */
    "static float4 rd(texture2d<float, access::read> t, int2 q, int2 mx) { return t.read(uint2(clamp(q, int2(0), mx))); }\n"
    "static float denoise(texture2d<float, access::read> a, texture2d<float, access::read> e, int2 ip, float beta)\n"
    "{\n"
    "    int2 mx = int2(a.get_width() - 1, a.get_height() - 1);\n"
    "    float4 ec = rd(e, ip, mx);\n"
    "    float4 el = rd(e, ip + int2(-1, 0), mx), er = rd(e, ip + int2(1, 0), mx);\n"
    "    float4 et = rd(e, ip + int2(0, -1), mx), eb = rd(e, ip + int2(0, 1), mx);\n"
    "    float edginess = saturate(4.0 - 2.5 - (ec.x + ec.y + ec.z + ec.w)) / (4.0 - 2.5) * 0.5;\n"
    "    ec = saturate(ec + edginess);\n"
    "    ec *= float4(el.y, er.x, et.w, eb.z);\n"
    "    const float dw = 0.425;\n"
    "    float wtl = dw * (ec.x * el.z + ec.z * et.x), wtr = dw * (ec.z * et.y + ec.y * er.z);\n"
    "    float wbl = dw * (ec.w * eb.x + ec.x * el.w), wbr = dw * (ec.y * er.w + ec.w * eb.y);\n"
    "    float sum = beta * rd(a, ip, mx).r;\n"
    "    sum += ec.x * rd(a, ip + int2(-1, 0), mx).r + ec.y * rd(a, ip + int2(1, 0), mx).r;\n"
    "    sum += ec.z * rd(a, ip + int2(0, -1), mx).r + ec.w * rd(a, ip + int2(0, 1), mx).r;\n"
    "    sum += wtl * rd(a, ip + int2(-1, -1), mx).r + wtr * rd(a, ip + int2(1, -1), mx).r;\n"
    "    sum += wbl * rd(a, ip + int2(-1, 1), mx).r + wbr * rd(a, ip + int2(1, 1), mx).r;\n"
    "    float ws = beta + ec.x + ec.y + ec.z + ec.w + wtl + wtr + wbl + wbr;\n"
    "    return sum / ws;\n"
    "}\n"
    "kernel void ao_denoise(texture2d<float, access::read> a [[texture(0)]],\n"
    "    texture2d<float, access::read> e [[texture(1)]], texture2d<float, access::write> o [[texture(2)]],\n"
    "    constant float &beta [[buffer(0)]], uint2 p [[thread_position_in_grid]])\n"
    "{\n"
    "    if (p.x >= o.get_width() || p.y >= o.get_height()) return;\n"
    "    o.write(float4(denoise(a, e, int2(p), beta)), p);\n"
    "}\n"
    "struct PV { float4 pos [[position]]; };\n"
    "vertex PV ao_vs(uint vid [[vertex_id]])\n"
    "{\n"
    "    float2 q = float2(float((vid << 1) & 2u), float(vid & 2u));\n"
    "    PV o;\n"
    "    o.pos = float4(q * 2.0 - 1.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n"
    /* The last denoise pass multiplies the scene in place (framebuffer
       fetch), so the final visibility needs no texture of its own. The
       multi-bounce approximation of GTAO (Jimenez et al. 2016) uses the
       scene color as the albedo: light surfaces get back part of the light
       that the occlusion takes away, so they do not go grey. */
    "fragment float4 ao_fs(PV in [[stage_in]], texture2d<float, access::read> a [[texture(0)]],\n"
    "    texture2d<float, access::read> e [[texture(1)]], constant float &beta [[buffer(0)]],\n"
    "    float4 dst [[color(0)]])\n"
    "{\n"
    "    float v = denoise(a, e, int2(in.pos.xy), beta);\n"
    "    float3 al = saturate(dst.rgb);\n"
    "    float3 ka = 2.0404 * al - 0.3324, kb = -4.7951 * al + 0.6417, kc = 2.7552 * al + 0.6903;\n"
    "    float3 m = max(float3(v), ((v * ka + kb) * v + kc) * v);\n"
    "    return float4(dst.rgb * m, dst.a);\n"
    "}\n";

typedef struct ao_consts {
    float sx, sy, px, py;
    float mulx, muly, addx, addy;
    float mpx, mpy, da, db;
    float radius, falloff, power, mipoff;
    float beta, pad;
    uint32_t slices, steps;
} ao_consts;

@interface D3DAO : NSObject
{
@public
    id<MTLLibrary> lib;
    id<MTLComputePipelineState> prefilter, prefilter_ms, main_pso, denoise;
    id<MTLRenderPipelineState> apply_pso;
    MTLPixelFormat apply_fmt;
    id<MTLTexture> depths, mip[AO_MIPS], ao0, ao1, edges;
    /* The camera of the frame (d3d_ao_capture). */
    bool have;
    uint32_t frame;
    /* present_count + 1 of the last frame with the occlusion pass. The
       camera stays available after the pass for d3d_ao_camera (the SMAA
       depth predication at the present). */
    uint32_t applied;
    float p00, p11, p22, p23;
    float vp[4][4]; /* view-projection rows, scaled so that |row3.xyz| = 1 */
    D3DSurface *ds;
    id<MTLTexture> depth;
    bool logged_start, logged_scene;
}
@end

@implementation D3DAO
@end

static id<MTLComputePipelineState> ao_kernel(D3DDevice *d, D3DAO *a, const char *name)
{
    NSError *err = nil;
    id<MTLFunction> f = [a->lib newFunctionWithName:@(name)];
    id<MTLComputePipelineState> s;
    if (f == nil) rt_fatal("D3D9: ambient occlusion function %s is missing", name);
    s = [d->mtl newComputePipelineStateWithFunction:f error:&err];
    if (s == nil) rt_fatal("D3D9: ambient occlusion pipeline %s creation failed: %s", name, err.localizedDescription.UTF8String);
    return s;
}

static D3DAO *ao_of(D3DDevice *d)
{
    if (d->ao == nil)
    {
        D3DAO *a = [D3DAO new];
        NSError *err = nil;
        a->lib = [d->mtl newLibraryWithSource:@(ao_src) options:[MTLCompileOptions new] error:&err];
        if (a->lib == nil)
            rt_fatal("D3D9: Metal compile of the ambient occlusion shaders failed: %s", err.localizedDescription.UTF8String);
        a->prefilter = ao_kernel(d, a, "ao_prefilter");
        a->prefilter_ms = ao_kernel(d, a, "ao_prefilter_ms");
        a->main_pso = ao_kernel(d, a, "ao_main");
        a->denoise = ao_kernel(d, a, "ao_denoise");
        d->ao = a;
    }
    return (D3DAO *)d->ao;
}

/* ---------------------------------------------------------------- camera */

/* The coefficient matrix m (clip = m * position) of the 4 registers r: one
   register for each row of m, or with transpose one for each column. */
static void coef(float m[4][4], const float (*r)[4], bool transpose)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) m[i][j] = transpose ? r[j][i] : r[i][j];
}

static float len3(const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* The inverse of an affine matrix (row 3 = 0 0 0 1). Returns false for a
   matrix that is not affine or has no inverse. */
static bool affine_inverse(const float m[4][4], float r[4][4])
{
    float det, k;
    int i, j;
    if (fabsf(m[3][0]) > 1e-4f || fabsf(m[3][1]) > 1e-4f || fabsf(m[3][2]) > 1e-4f || fabsf(m[3][3] - 1.0f) > 1e-4f)
        return false;
    r[0][0] = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    r[0][1] = m[0][2] * m[2][1] - m[0][1] * m[2][2];
    r[0][2] = m[0][1] * m[1][2] - m[0][2] * m[1][1];
    r[1][0] = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    r[1][1] = m[0][0] * m[2][2] - m[0][2] * m[2][0];
    r[1][2] = m[0][2] * m[1][0] - m[0][0] * m[1][2];
    r[2][0] = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    r[2][1] = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    r[2][2] = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    det = m[0][0] * r[0][0] + m[0][1] * r[1][0] + m[0][2] * r[2][0];
    if (!(fabsf(det) > 1e-12f)) return false;
    k = 1.0f / det;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) r[i][j] *= k;
    for (i = 0; i < 3; i++) r[i][3] = -(r[i][0] * m[0][3] + r[i][1] * m[1][3] + r[i][2] * m[2][3]);
    r[3][0] = r[3][1] = r[3][2] = 0.0f;
    r[3][3] = 1.0f;
    return true;
}

/* P00, P11, P22 and P23 of the projection in wvp * inverse(w), or false if
   the matrices do not give a perspective projection. */
static bool projection_of(const float wvp[4][4], const float w[4][4], float out[4], float vp_out[4][4])
{
    float inv[4][4], vp[4][4], l3, k, c0, c1;
    int i, j, n;
    if (!affine_inverse(w, inv)) return false;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
        {
            float s = 0.0f;
            for (n = 0; n < 4; n++) s += wvp[i][n] * inv[n][j];
            vp[i][j] = s;
        }
    l3 = len3(vp[3]);
    if (!(l3 > 1e-6f) || !isfinite(l3)) return false;
    k = 1.0f / l3;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) vp[i][j] *= k;
    /* Row 3 must be the view z axis (unit length after the scale, so the
       view rotation is orthonormal when |row0| and |row1| agree with it),
       rows 0 and 1 must be perpendicular to it (no off-center projection). */
    c0 = len3(vp[0]);
    c1 = len3(vp[1]);
    if (!(c0 > 1e-4f) || !(c1 > 1e-4f)) return false;
    if (fabsf(dot3(vp[0], vp[3])) > 0.01f * c0 || fabsf(dot3(vp[1], vp[3])) > 0.01f * c1) return false;
    out[0] = c0;
    out[1] = c1;
    out[2] = dot3(vp[2], vp[3]);
    out[3] = vp[2][3] - out[2] * vp[3][3];
    if (!(fabsf(out[3]) > 1e-9f) || !isfinite(out[2]) || !isfinite(out[3])) return false;
    memcpy(vp_out, vp, sizeof vp);
    return true;
}

void d3d_ao_capture(D3DDevice *d, D3DShader *vs)
{
    D3DSurface *ds, *bb;
    D3DAO *a;
    id<MTLTexture> t;
    NSUInteger slice, level;
    float vpf[6], proj[4];
    int tr;
    bool ok = false;
    float vpm[4][4];
    /* The contact shadows (rt_d3d9_sss.m) and the SMAA depth predication
       (rt_d3d9_post.m) also use the camera. */
    if ((rt_enh_get(RT_ENH_AO) == 0 && rt_enh_get(RT_ENH_CONTACT) == 0 && rt_enh_get(RT_ENH_SMAA) == 0) ||
        vs->sh.ct_wvp < 0 || vs->sh.ct_world < 0)
        return;
    if (d->ds == NULL || d->backbuffer == NULL || !d->rs[RS_ZENABLE] || !d->rs[RS_ZWRITEENABLE]) return;
    ds = d3d_surf(d->ds);
    bb = d3d_surf(d->backbuffer);
    if (ds->width != bb->width || ds->height != bb->height || ds->scale != 1 || !ds->f->depth) return;
    memcpy(vpf, d->viewport, sizeof vpf);
    if (d->viewport[0] != 0 || d->viewport[1] != 0 || d->viewport[2] != ds->width || d->viewport[3] != ds->height ||
        vpf[4] != 0.0f || vpf[5] != 1.0f)
        return;
    for (tr = 0; tr < 2 && !ok; tr++)
    {
        float wvp[4][4], w[4][4];
        coef(wvp, (const float (*)[4])d->vs_c[vs->sh.ct_wvp],
             (vs->sh.ct_wvp_class == D3DSH_CLASS_MATRIX_ROWS) != (tr == 1));
        coef(w, (const float (*)[4])d->vs_c[vs->sh.ct_world],
             (vs->sh.ct_world_class == D3DSH_CLASS_MATRIX_ROWS) != (tr == 1));
        ok = projection_of(wvp, w, proj, vpm);
    }
    if (!ok) return;
    t = d3d_surface_texture(ds, &slice, &level);
    if (t == nil || slice != 0 || level != 0) return;
    a = ao_of(d);
    a->have = true;
    a->frame = d->present_count;
    a->p00 = proj[0];
    a->p11 = proj[1];
    a->p22 = proj[2];
    a->p23 = proj[3];
    memcpy(a->vp, vpm, sizeof vpm);
    a->ds = ds;
    a->depth = t;
}

/* ---------------------------------------------------------------- passes */

static id<MTLTexture> ao_tex(D3DDevice *d, id<MTLTexture> __strong *slot, NSUInteger w, NSUInteger h, MTLPixelFormat fmt,
                             NSUInteger mips)
{
    id<MTLTexture> t = *slot;
    MTLTextureDescriptor *td;
    if (t != nil && t.width == w && t.height == h && t.pixelFormat == fmt && t.mipmapLevelCount == mips) return t;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    td.mipmapLevelCount = mips;
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    t = [d->mtl newTextureWithDescriptor:td];
    if (t == nil) rt_fatal("D3D9: out of video memory (ambient occlusion texture %lux%lu)", (unsigned long)w, (unsigned long)h);
    *slot = t;
    return t;
}

/* w and h are at least 16, so each of the AO_MIPS levels exists. */
static void ensure_textures(D3DDevice *d, D3DAO *a, NSUInteger w, NSUInteger h)
{
    NSUInteger i;
    if (a->depths == nil || a->depths.width != w || a->depths.height != h)
    {
        ao_tex(d, &a->depths, w, h, MTLPixelFormatR32Float, AO_MIPS);
        for (i = 0; i < AO_MIPS; i++)
        {
            a->mip[i] = [a->depths newTextureViewWithPixelFormat:MTLPixelFormatR32Float
                                                      textureType:MTLTextureType2D
                                                           levels:NSMakeRange(i, 1)
                                                           slices:NSMakeRange(0, 1)];
            if (a->mip[i] == nil) rt_fatal("D3D9: ambient occlusion depth mip view creation failed");
        }
    }
    ao_tex(d, &a->ao0, w, h, MTLPixelFormatR16Float, 1);
    ao_tex(d, &a->ao1, w, h, MTLPixelFormatR16Float, 1);
    ao_tex(d, &a->edges, w, h, MTLPixelFormatRGBA8Unorm, 1);
}

static void dispatch(id<MTLComputeCommandEncoder> e, id<MTLComputePipelineState> pso, NSUInteger w, NSUInteger h)
{
    [e setComputePipelineState:pso];
    [e dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
}

static id<MTLRenderPipelineState> apply_pipeline(D3DDevice *d, D3DAO *a, MTLPixelFormat fmt)
{
    MTLRenderPipelineDescriptor *pd;
    NSError *err = nil;
    if (a->apply_pso != nil && a->apply_fmt == fmt) return a->apply_pso;
    pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [a->lib newFunctionWithName:@"ao_vs"];
    pd.fragmentFunction = [a->lib newFunctionWithName:@"ao_fs"];
    if (pd.vertexFunction == nil || pd.fragmentFunction == nil) rt_fatal("D3D9: ambient occlusion apply function is missing");
    pd.colorAttachments[0].pixelFormat = fmt;
    /* scene.rgb = scene.rgb * multi-bounce visibility (ao_fs reads the
       scene pixel); alpha does not change. */
    pd.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
    a->apply_pso = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
    if (a->apply_pso == nil)
        rt_fatal("D3D9: ambient occlusion apply pipeline creation failed: %s", err.localizedDescription.UTF8String);
    a->apply_fmt = fmt;
    return a->apply_pso;
}

void d3d_ao_apply(D3DDevice *d, D3DShader *ps)
{
    static const uint32_t slices[4] = { 0, 1, 2, 3 }, steps[4] = { 0, 2, 2, 3 };
    int q = rt_enh_get(RT_ENH_AO);
    D3DAO *a;
    rt_com_object *o;
    D3DTexture *st;
    id<MTLTexture> scene, depth;
    id<MTLComputeCommandEncoder> ce;
    id<MTLRenderCommandEncoder> re;
    MTLRenderPassDescriptor *rp;
    ao_consts c;
    NSUInteger w, h;
    uint32_t i;
    float beta;
    if (q <= 0 || d->ao == nil) return;
    if (q > 3) q = 3;
    a = (D3DAO *)d->ao;
    if (!a->have || a->frame != d->present_count || a->applied == d->present_count + 1u) return;
    a->applied = d->present_count + 1u;
    depth = a->depth;
    o = d->tex[ps->sh.ct_scene_sampler];
    st = o != NULL ? d3d_tex(o) : nil;
    scene = st != nil && !st->cube && !st->volume ? st->tex : nil;
    w = depth.width;
    h = depth.height;
    if (scene == nil || scene.width != w || scene.height != h || w < 16 || h < 16 || scene.sampleCount != 1 ||
        (scene.usage & MTLTextureUsageRenderTarget) == 0)
    {
        if (!a->logged_scene)
            rt_log("D3D9: ambient occlusion: the scene texture of the composite pass is not a %lux%lu render target; the "
                   "frame has no ambient occlusion",
                   (unsigned long)w, (unsigned long)h);
        a->logged_scene = true;
        a->ds = nil;
        return;
    }
    if (!a->logged_start)
    {
        rt_log("D3D9: XeGTAO ambient occlusion %lux%lu, %u slices x %u steps", (unsigned long)w, (unsigned long)h,
               slices[q], steps[q]);
        a->logged_start = true;
    }
    @autoreleasepool
    {
        d3d_end_pass(d);
        ensure_textures(d, a, w, h);
        memset(&c, 0, sizeof c);
        c.sx = (float)w;
        c.sy = (float)h;
        c.px = 1.0f / (float)w;
        c.py = 1.0f / (float)h;
        c.mulx = 2.0f / a->p00;
        c.muly = -2.0f / a->p11;
        c.addx = -1.0f / a->p00;
        c.addy = 1.0f / a->p11;
        c.mpx = c.mulx * c.px;
        c.mpy = c.muly * c.py;
        c.da = a->p22;
        c.db = a->p23;
        c.radius = 0.5f * 1.457f;
        c.falloff = 0.615f;
        c.power = 2.2f;
        c.mipoff = 3.30f;
        c.beta = 1.2f;
        c.slices = slices[q];
        c.steps = steps[q];

        ce = [d->cb computeCommandEncoder];
        if (ce == nil) rt_fatal("D3D9: ambient occlusion compute encoder creation failed");
        ce.label = @"XeGTAO";
        [ce setBytes:&c length:sizeof c atIndex:0];
        [ce setTexture:depth atIndex:0];
        for (i = 0; i < AO_MIPS; i++) [ce setTexture:a->mip[i] atIndex:1 + i];
        /* Whole groups: every thread of a group takes part in its barriers. */
        [ce setComputePipelineState:depth.sampleCount > 1 ? a->prefilter_ms : a->prefilter];
        [ce dispatchThreadgroups:MTLSizeMake((w + 15) / 16, (h + 15) / 16, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [ce setTexture:a->depths atIndex:0];
        [ce setTexture:a->ao0 atIndex:1];
        [ce setTexture:a->edges atIndex:2];
        dispatch(ce, a->main_pso, w, h);
        /* Two denoise passes: the first with less weight on the center. The
           second is the fragment shader of the multiply into the scene. */
        beta = c.beta / 5.0f;
        [ce setBytes:&beta length:sizeof beta atIndex:0];
        [ce setTexture:a->ao0 atIndex:0];
        [ce setTexture:a->edges atIndex:1];
        [ce setTexture:a->ao1 atIndex:2];
        dispatch(ce, a->denoise, w, h);
        [ce endEncoding];
        beta = c.beta;

        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = scene;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        re = [d->cb renderCommandEncoderWithDescriptor:rp];
        if (re == nil) rt_fatal("D3D9: ambient occlusion apply encoder creation failed");
        re.label = @"XeGTAO apply";
        [re setRenderPipelineState:apply_pipeline(d, a, scene.pixelFormat)];
        [re setFragmentTexture:a->ao1 atIndex:0];
        [re setFragmentTexture:a->edges atIndex:1];
        [re setFragmentBytes:&beta length:sizeof beta atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [re endEncoding];
    }
    st->last_use = d->serial;
    if (a->ds != nil) d3d_use_surface(a->ds);
    a->ds = nil;
}

bool d3d_ao_camera(D3DDevice *d, float vp[4][4], float p22_p23[2], id<MTLTexture> __strong *depth)
{
    D3DAO *a = (D3DAO *)d->ao;
    if (a == nil || !a->have || a->frame != d->present_count || a->depth == nil) return false;
    memcpy(vp, a->vp, sizeof a->vp);
    p22_p23[0] = a->p22;
    p22_p23[1] = a->p23;
    *depth = a->depth;
    return true;
}
