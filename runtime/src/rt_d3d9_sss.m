/*
 * Screen-space contact shadows for the Mac enhancement setting
 * RT_ENH_CONTACT (rt_enh.h): a Metal implementation of the method of Bend
 * Studio ("Inside Bend: Screen Space Shadows", Days Gone).
 *
 * This file is derived from bend_sss_cpu.h and bend_sss_gpu.h of Bend
 * Studio, which are available under this license:
 *
 *   Copyright 2023 Sony Interactive Entertainment.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 * Changes from the original: the HLSL shader is written again in the Metal
 * shading language; the dispatch list function is written again in C; the
 * depth reads use texture reads with a bounds check (the result outside
 * the texture is the far depth) instead of a border sampler; the debug
 * outputs are not included; the early-out writes 1 (no shadow) for pixels
 * at the far depth; the result fades out between 40 and 80 world units of
 * view distance; a multisampled depth buffer is read at sample 0.
 *
 * The method. Each pixel looks along the screen-space line from the pixel
 * to the light for a depth that is nearer than the line from the pixel to
 * the light. The dispatches cover the screen in wavefronts of 64 pixels
 * that point to the light, so the 64 threads of a group share the depth
 * samples of one line in threadgroup memory. Settings: 60 samples (the
 * shadow length in pixels), 4 hard-shadow samples, 8 fade-out samples,
 * surface thickness 0.005, bilinear threshold 0.02, shadow contrast 4 (the
 * recommended starting values of Bend Studio; UNCONFIRMED for this game),
 * bilinear sampling offset mode on (smoother shadow edges), early-out on.
 * One world unit is assumed to be one meter (UNCONFIRMED).
 *
 * The light. The pixel shaders that read the shadow map have the matrix
 * p3dShadowMapMatrix (d3d_shader.ct_shadow). Its depth row is the
 * direction in which the shadow depth increases, the direction of the
 * sunlight. The matrix applies to world positions or to object positions;
 * d3d_sss_draw finds which from two draws with different world matrices
 * (the world-space direction is the same for all draws of a frame). The
 * vector to the light is the negative direction. Its projection with the
 * view-projection matrix of the camera (d3d_ao_camera) gives the light
 * position on the screen.
 *
 * Use. The contact shadows are calculated before the first draw of a frame
 * that reads the shadow map into a full-size target, after the game's depth
 * pass. The pixel shader variants of these draws multiply each shadow map
 * comparison by the contact shadow of the pixel (d3d_shader_msl_ps,
 * contact), so the contact shadows darken only the sunlight. A frame
 * without a camera or a light direction has no contact shadows; the log
 * gets one message. This code has not run (UNCONFIRMED).
 */
#include "rt_d3d9_int.h"
#include "rt_enh.h"

#include <math.h>

#define SSS_WAVE 64
#define SSS_MAX_DISPATCH 8

static const char sss_src[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#define WAVE 64\n"
    "#define SAMPLES 60\n"
    "#define HARD 4\n"
    "#define FADE 8\n"
    "#define READS (SAMPLES / WAVE + 2)\n"
    "struct SSSP {\n"
    "    float4 light;\n"      /* screen x, y (pixels), depth, sign of w */
    "    int2 offset;\n"       /* wave offset of this dispatch (pixels) */
    "    float far_z, near_z;\n"
    "    float thickness, bilinear, contrast, pad;\n"
    "    float p22, p23, fade_start, fade_end;\n" /* view depth = p23 / (z - p22); fade (world units) */
    "};\n"
    "static float dget(depth2d<float, access::read> t, int2 q, float far_z)\n"
    "{\n"
    "    if (q.x < 0 || q.y < 0 || q.x >= int(t.get_width()) || q.y >= int(t.get_height())) return far_z;\n"
    "    return t.read(uint2(q));\n"
    "}\n"
    "static float dget(depth2d_ms<float, access::read> t, int2 q, float far_z)\n"
    "{\n"
    "    if (q.x < 0 || q.y < 0 || q.x >= int(t.get_width()) || q.y >= int(t.get_height())) return far_z;\n"
    "    return t.read(uint2(q), 0);\n"
    "}\n"
    "static int2 isign(int2 v) { return int2(v > 0) - int2(v < 0); }\n"
    "template <typename D>\n"
    "static void sss(D dt, texture2d<float, access::write> out, constant SSSP &p, int3 g, int tid,\n"
    "    threadgroup float *lds)\n"
    "{\n"
    /* The line of this wavefront and the start pixel of this thread. */
    "    int2 xy = g.yz * WAVE + p.offset;\n"
    "    float2 lc = floor(p.light.xy) + 0.5;\n"
    "    float2 lf = p.light.xy - lc;\n"
    "    bool rev = p.light.w > 0.0;\n"
    "    int2 sg = isign(xy);\n"
    "    bool horiz = abs(xy.x + sg.y) < abs(xy.y - sg.x);\n"
    "    int2 ax = horiz ? int2(sg.y, 0) : int2(0, -sg.x);\n"
    "    xy = ax * g.x + xy;\n"
    "    float2 xf = float2(xy);\n"
    "    bool xmaj = abs(xf.x) > abs(xf.y);\n"
    "    float maj = xmaj ? xf.x : xf.y;\n"
    "    float ms = abs(maj), me = abs(maj) - float(WAVE);\n"
    "    float mf = xmaj ? lf.x : lf.y;\n"
    "    mf = maj > 0.0 ? -mf : mf;\n"
    "    float2 sxy = xf + lc;\n"
    "    float2 exy = mix(p.light.xy, sxy, (me + mf) / (ms + mf));\n"
    "    float2 dxy = sxy - exy;\n"
    "    float st = float(uint(tid) ^ (rev ? 0u : uint(WAVE - 1)));\n"
    "    float2 pix = mix(sxy, exy, st / float(WAVE));\n"
    "    float pdist = ms - st + mf;\n"
    /* Depth reads: two depths per sample on the minor axis, interpolated
       unless they are on two sides of an edge. */
    "    float dir = -p.light.w;\n"
    "    float zs = p.near_z > p.far_z ? -1.0 : 1.0;\n"
    "    float samp0 = 0.0, raw0 = 0.0, thick0 = 1.0, dist0 = 1.0;\n"
    "    float2 wxy = floor(pix);\n"
    "    for (int i = 0; i < READS; i++) {\n"
    "        int2 rq = int2(floor(pix));\n"
    "        float minor = xmaj ? pix.y : pix.x;\n"
    "        float bl = fract(minor) - 0.5;\n"
    "        int b = bl > 0.0 ? 1 : -1;\n"
    "        int2 oq = int2(xmaj ? 0 : b, xmaj ? b : 0);\n"
    "        float d0 = dget(dt, rq, p.far_z), d1 = dget(dt, rq + oq, p.far_z);\n"
    "        float thick = abs(p.far_z - d0);\n"
    "        bool point_f = abs(d0 - d1) > thick * p.bilinear;\n"
    /* Bilinear offset mode: the depth between the two texels at the
       position of the line, except at an edge. */
    "        float shadow_d = mix(d0, d1, point_f ? 0.0 : abs(bl));\n"
    "        float sdist = pdist + float(WAVE * i) * dir;\n"
    "        if (i == 0) { samp0 = shadow_d; raw0 = d0; thick0 = thick; dist0 = sdist; }\n"
    /* In this space all light rays are parallel. */
    "        float sd = (shadow_d - p.light.z) / sdist;\n"
    "        if (i != 0) sd = sdist > 0.0 ? sd : 1e10;\n"
    "        lds[i * WAVE + tid] = sd;\n"
    "        pix += dxy * dir;\n"
    "    }\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    int2 w = int2(wxy);\n"
    "    bool inside = w.x >= 0 && w.y >= 0 && w.x < int(out.get_width()) && w.y < int(out.get_height());\n"
    /* Early out: a pixel at the far depth (sky) has no surface to shadow.
       Its depth is still in threadgroup memory for the other pixels. */
    "    if (raw0 >= p.far_z) { if (inside) out.write(float4(1.0), uint2(w)); return; }\n"
    "    float start = (samp0 - p.light.z) / dist0;\n"
    "    float scale = min(dist0 + dir, 1.0 / p.thickness) * dist0 / thick0;\n"
    "    start = start * scale - zs;\n"
    "    int si = tid + 1;\n"
    "    float4 sv = float4(1.0);\n"
    "    float hard = 1.0;\n"
    "    for (int i = 0; i < HARD; i++)\n"
    "        hard = min(hard, abs(start - lds[si + i] * scale));\n"
    "    for (int i = HARD; i < SAMPLES - FADE; i++)\n"
    "        sv[i & 3] = min(sv[i & 3], abs(start - lds[si + i] * scale));\n"
    "    for (int i = SAMPLES - FADE; i < SAMPLES; i++) {\n"
    "        float fo = float(i + 1 - (SAMPLES - FADE)) / float(FADE + 1) * 0.75;\n"
    "        sv[i & 3] = min(sv[i & 3], abs(start - lds[si + i] * scale) + fo);\n"
    "    }\n"
    "    sv = saturate(sv * p.contrast + (1.0 - p.contrast));\n"
    "    hard = saturate(hard * p.contrast + (1.0 - p.contrast));\n"
    "    float r = min(hard, dot(sv, float4(0.25)));\n"
    /* Fade out with the view distance: far away, 60 pixels are a long
       distance in the world. */
    "    float vz = p.p23 / (raw0 - p.p22);\n"
    "    r = mix(r, 1.0, saturate((vz - p.fade_start) / (p.fade_end - p.fade_start)));\n"
    "    if (inside) out.write(float4(r), uint2(w));\n"
    "}\n"
    "kernel void sss_main(depth2d<float, access::read> dt [[texture(0)]],\n"
    "    texture2d<float, access::write> o [[texture(1)]], constant SSSP &p [[buffer(0)]],\n"
    "    uint3 g [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]])\n"
    "{\n"
    "    threadgroup float lds[READS * WAVE];\n"
    "    sss(dt, o, p, int3(g), int(tid), lds);\n"
    "}\n"
    "kernel void sss_main_ms(depth2d_ms<float, access::read> dt [[texture(0)]],\n"
    "    texture2d<float, access::write> o [[texture(1)]], constant SSSP &p [[buffer(0)]],\n"
    "    uint3 g [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]])\n"
    "{\n"
    "    threadgroup float lds[READS * WAVE];\n"
    "    sss(dt, o, p, int3(g), int(tid), lds);\n"
    "}\n";

typedef struct sss_params {
    float light[4];
    int32_t offset[2];
    float far_z, near_z;
    float thickness, bilinear, contrast, pad;
    float p22, p23, fade_start, fade_end;
} sss_params;

typedef struct sss_dispatch {
    int32_t count[3];  /* threadgroups x, y, z */
    int32_t offset[2]; /* wave offset (pixels) */
} sss_dispatch;

@interface D3DSSS : NSObject
{
@public
    id<MTLLibrary> lib;
    id<MTLComputePipelineState> pso, pso_ms;
    id<MTLTexture> out;
    uint32_t frame; /* present_count + 1 of the last calculation, 0 = none */
    /* Space of p3dShadowMapMatrix: -1 not known, 0 world, 1 object. */
    int space;
    bool have_prev;
    float prev_world[3], prev_object[3], prev_w[3];
    bool logged_start, logged_wait, logged_size;
}
@end

@implementation D3DSSS
@end

static D3DSSS *sss_of(D3DDevice *d)
{
    if (d->sss == nil)
    {
        D3DSSS *s = [D3DSSS new];
        NSError *err = nil;
        id<MTLFunction> f, fm;
        s->lib = [d->mtl newLibraryWithSource:@(sss_src) options:[MTLCompileOptions new] error:&err];
        if (s->lib == nil)
            rt_fatal("D3D9: Metal compile of the contact shadow shader failed: %s", err.localizedDescription.UTF8String);
        f = [s->lib newFunctionWithName:@"sss_main"];
        fm = [s->lib newFunctionWithName:@"sss_main_ms"];
        if (f == nil || fm == nil) rt_fatal("D3D9: the contact shadow function is missing");
        s->pso = [d->mtl newComputePipelineStateWithFunction:f error:&err];
        if (s->pso == nil) rt_fatal("D3D9: contact shadow pipeline creation failed: %s", err.localizedDescription.UTF8String);
        s->pso_ms = [d->mtl newComputePipelineStateWithFunction:fm error:&err];
        if (s->pso_ms == nil) rt_fatal("D3D9: contact shadow pipeline creation failed: %s", err.localizedDescription.UTF8String);
        if (s->pso.maxTotalThreadsPerThreadgroup < SSS_WAVE || s->pso_ms.maxTotalThreadsPerThreadgroup < SSS_WAVE)
            rt_fatal("D3D9: the contact shadow shader cannot run %d threads in a group", SSS_WAVE);
        s->space = -1;
        d->sss = s;
    }
    return (D3DSSS *)d->sss;
}

/* ------------------------------------------------------- dispatch list */

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* The dispatches for the light at clip position lp (x, y, z, w) over a
   w x h target. The screen is split into four quadrants around the light;
   each quadrant that is not square is split in two on its long axis. */
static int dispatch_list(const float lp[4], int w, int h, float light_out[4], sss_dispatch *list)
{
    float lw = lp[3], lim = 0.000002f * (float)SSS_WAVE;
    int n = 0, q, lx, ly, bb[4], i;
    if (lw >= 0.0f && lw < lim) lw = lim;
    else if (lw < 0.0f && lw > -lim) lw = -lim;
    light_out[0] = ((lp[0] / lw) * 0.5f + 0.5f) * (float)w;
    light_out[1] = ((lp[1] / lw) * -0.5f + 0.5f) * (float)h;
    light_out[2] = lp[3] == 0.0f ? 0.0f : lp[2] / lp[3];
    light_out[3] = lp[3] > 0.0f ? 1.0f : -1.0f;
    lx = (int)(light_out[0] + 0.5f);
    ly = (int)(light_out[1] + 0.5f);
    /* Inclusive screen bounds relative to the light (y up). */
    bb[0] = 0 - lx;
    bb[1] = -(h - ly);
    bb[2] = w - lx;
    bb[3] = -(0 - ly);
    for (q = 0; q < 4; q++)
    {
        bool vert = q == 0 || q == 3;
        int r0 = imax(0, (q & 1) ? bb[0] : -bb[2]) / SSS_WAVE;
        int r1 = imax(0, (q & 2) ? bb[1] : -bb[3]) / SSS_WAVE;
        int r2 = imax(0, ((q & 1) ? bb[2] : -bb[0]) + SSS_WAVE * (vert ? 1 : 2) - 1) / SSS_WAVE;
        int r3 = imax(0, ((q & 2) ? bb[3] : -bb[1]) + SSS_WAVE * (vert ? 2 : 1) - 1) / SSS_WAVE;
        sss_dispatch *a, *b;
        int diag;
        if (r2 - r0 <= 0 || r3 - r1 <= 0) continue;
        a = &list[n++];
        a->count[0] = SSS_WAVE;
        a->count[1] = r2 - r0;
        a->count[2] = r3 - r1;
        a->offset[0] = ((q & 1) ? r0 : -r2) + ((q == 2 || q == 3) ? 1 : 0);
        a->offset[1] = ((q & 2) ? -r3 : r1) + ((q == 1 || q == 3) ? 1 : 0);
        /* The far corner of the quadrant: where the diagonal from the light
           meets the bounds. */
        diag = bb[0] - bb[1];
        if (q == 1) diag = bb[2] + bb[1];
        if (q == 2) diag = -bb[0] - bb[3];
        if (q == 3) diag = -bb[2] + bb[3];
        diag = (diag + SSS_WAVE - 1) / SSS_WAVE;
        if (diag <= 0) continue;
        b = &list[n++];
        *b = *a;
        if (q == 0)
        {
            b->count[2] = imin(a->count[2], diag);
            a->count[2] -= b->count[2];
            b->offset[1] = a->offset[1] + a->count[2];
            b->offset[0]--;
            b->count[1]++;
        }
        else if (q == 1)
        {
            b->count[1] = imin(a->count[1], diag);
            a->count[1] -= b->count[1];
            b->offset[0] = a->offset[0] + a->count[1];
            b->count[2]++;
        }
        else if (q == 2)
        {
            b->count[1] = imin(a->count[1], diag);
            a->count[1] -= b->count[1];
            a->offset[0] += b->count[1];
            b->count[2]++;
            b->offset[1]--;
        }
        else
        {
            b->count[2] = imin(a->count[2], diag);
            a->count[2] -= b->count[2];
            a->offset[1] += b->count[2];
            b->count[1]++;
        }
        /* Remove an empty part (the last entry takes its place). */
        if (b->count[1] <= 0 || b->count[2] <= 0) *b = list[--n];
        if (a->count[1] <= 0 || a->count[2] <= 0) *a = list[--n];
    }
    for (i = 0; i < n; i++)
    {
        list[i].offset[0] *= SSS_WAVE;
        list[i].offset[1] *= SSS_WAVE;
    }
    return n;
}

/* ------------------------------------------------------------- light */

static void mat_of(float m[4][4], const float (*r)[4], bool transpose)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) m[i][j] = transpose ? r[j][i] : r[i][j];
}

static bool affine(const float m[4][4])
{
    return fabsf(m[3][0]) < 1e-4f && fabsf(m[3][1]) < 1e-4f && fabsf(m[3][2]) < 1e-4f && fabsf(m[3][3] - 1.0f) < 1e-4f;
}

static bool norm3(float v[3])
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(l > 1e-12f) || !isfinite(l)) return false;
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
    return true;
}

static bool same3(const float a[3], const float b[3], float eps)
{
    return fabsf(a[0] - b[0]) < eps && fabsf(a[1] - b[1]) < eps && fabsf(a[2] - b[2]) < eps;
}

/* The two candidates for the world-space vector to the light: from the
   shadow matrix s as a world-space matrix, and as an object-space matrix
   (s * inverse(w), w = world matrix). The direction of the light is the
   gradient of the shadow depth, row 2 of the world-space matrix; with an
   object-space matrix, row2_world = row2_object * inverse(w), which is
   the transpose of the inverse 3x3 part applied to row 2. */
static bool light_candidates(const float s[4][4], const float w[4][4], float world[3], float object[3])
{
    float a = w[0][0], b = w[0][1], c = w[0][2], d = w[1][0], e = w[1][1], f = w[1][2], g = w[2][0], h = w[2][1],
          k = w[2][2];
    float inv[3][3], det;
    int i, j;
    inv[0][0] = e * k - f * h;
    inv[0][1] = c * h - b * k;
    inv[0][2] = b * f - c * e;
    inv[1][0] = f * g - d * k;
    inv[1][1] = a * k - c * g;
    inv[1][2] = c * d - a * f;
    inv[2][0] = d * h - e * g;
    inv[2][1] = b * g - a * h;
    inv[2][2] = a * e - b * d;
    det = a * inv[0][0] + b * inv[1][0] + c * inv[2][0];
    if (!(fabsf(det) > 1e-12f)) return false;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) inv[i][j] /= det;
    for (j = 0; j < 3; j++)
    {
        world[j] = -s[2][j];
        object[j] = -(s[2][0] * inv[0][j] + s[2][1] * inv[1][j] + s[2][2] * inv[2][j]);
    }
    return norm3(world) && norm3(object);
}

/* The world-space vector to the light from the constants of this draw, or
   false when it is not known yet. */
static bool light_vector(D3DDevice *d, D3DSSS *s, const D3DShader *vs, const D3DShader *ps, float out[3])
{
    float sm[4][4], wm[4][4], world[3], object[3];
    bool found = false;
    int tr;
    if (ps->sh.ct_shadow < 0 || vs->sh.ct_world < 0) return false;
    for (tr = 0; tr < 2 && !found; tr++)
    {
        mat_of(sm, (const float (*)[4])d->ps_c[ps->sh.ct_shadow],
               (ps->sh.ct_shadow_class == D3DSH_CLASS_MATRIX_ROWS) != (tr == 1));
        mat_of(wm, (const float (*)[4])d->vs_c[vs->sh.ct_world],
               (vs->sh.ct_world_class == D3DSH_CLASS_MATRIX_ROWS) != (tr == 1));
        found = affine(sm) && affine(wm);
    }
    if (!found || !light_candidates(sm, wm, world, object)) return false;
    if (s->space < 0)
    {
        float wrow[3] = { wm[0][0], wm[1][1], wm[2][2] };
        /* Two draws with different world matrices decide the space. */
        if (s->have_prev && !same3(wrow, s->prev_w, 1e-3f))
        {
            bool sw = same3(world, s->prev_world, 1e-4f), so = same3(object, s->prev_object, 1e-4f);
            if (sw != so)
            {
                s->space = sw ? 0 : 1;
                rt_log("D3D9: contact shadows: p3dShadowMapMatrix is a %s-space matrix", sw ? "world" : "object");
            }
        }
        memcpy(s->prev_world, world, sizeof world);
        memcpy(s->prev_object, object, sizeof object);
        memcpy(s->prev_w, wrow, sizeof wrow);
        s->have_prev = true;
        if (s->space < 0) return false;
    }
    memcpy(out, s->space == 0 ? world : object, sizeof world);
    return true;
}

/* ------------------------------------------------------------- passes */

id<MTLTexture> d3d_sss_draw(D3DDevice *d, D3DShader *vs, D3DShader *ps)
{
    D3DSSS *s;
    D3DSurface *rt;
    id<MTLTexture> depth;
    float vp[4][4], lin[2], l[3], lp[4];
    sss_params p;
    sss_dispatch list[SSS_MAX_DISPATCH];
    int n, i, j;
    NSUInteger w, h;
    if (rt_enh_get(RT_ENH_CONTACT) == 0 || d->rt[0] == NULL) return nil;
    rt = d3d_surf(d->rt[0]);
    if (rt->scale != 1) return nil;
    s = sss_of(d);
    /* The contact shadow texture applies only to draws into a target of its
       size (the size of the scene depth buffer). */
    if (s->frame == d->present_count + 1u)
        return s->out.width == rt->width && s->out.height == rt->height ? s->out : nil;
    if (!light_vector(d, s, vs, ps, l) || !d3d_ao_camera(d, vp, lin, &depth))
    {
        if (!s->logged_wait)
            rt_log("D3D9: contact shadows: the light direction or the camera of the frame is not known yet; the frame "
                   "has no contact shadows");
        s->logged_wait = true;
        return nil;
    }
    w = depth.width;
    h = depth.height;
    if (w != rt->width || h != rt->height)
    {
        if (!s->logged_size)
            rt_log("D3D9: contact shadows: a shadow-map draw into a %ux%u target, but the scene depth is %lux%lu; the "
                   "draw has no contact shadows",
                   rt->width, rt->height, (unsigned long)w, (unsigned long)h);
        s->logged_size = true;
        return nil;
    }
    /* The light at infinity: (l, 0) through the view-projection matrix. */
    for (i = 0; i < 4; i++) lp[i] = vp[i][0] * l[0] + vp[i][1] * l[1] + vp[i][2] * l[2];
    memset(&p, 0, sizeof p);
    n = dispatch_list(lp, (int)w, (int)h, p.light, list);
    p.far_z = 1.0f;
    p.near_z = 0.0f;
    p.thickness = 0.005f;
    p.bilinear = 0.02f;
    p.contrast = 4.0f;
    p.p22 = lin[0];
    p.p23 = lin[1];
    p.fade_start = 40.0f;
    p.fade_end = 80.0f;
    if (s->out == nil || s->out.width != w || s->out.height != h)
    {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                       width:w
                                                                                      height:h
                                                                                   mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        s->out = [d->mtl newTextureWithDescriptor:td];
        if (s->out == nil) rt_fatal("D3D9: out of video memory (contact shadow texture %lux%lu)", (unsigned long)w, (unsigned long)h);
    }
    if (!s->logged_start)
    {
        rt_log("D3D9: Bend contact shadows %lux%lu, %d dispatches for the first frame", (unsigned long)w, (unsigned long)h, n);
        s->logged_start = true;
    }
    @autoreleasepool
    {
        id<MTLComputeCommandEncoder> ce;
        d3d_end_pass(d);
        ce = [d->cb computeCommandEncoder];
        if (ce == nil) rt_fatal("D3D9: contact shadow compute encoder creation failed");
        ce.label = @"Bend contact shadows";
        [ce setComputePipelineState:depth.sampleCount > 1 ? s->pso_ms : s->pso];
        [ce setTexture:depth atIndex:0];
        [ce setTexture:s->out atIndex:1];
        for (j = 0; j < n; j++)
        {
            p.offset[0] = list[j].offset[0];
            p.offset[1] = list[j].offset[1];
            [ce setBytes:&p length:sizeof p atIndex:0];
            [ce dispatchThreadgroups:MTLSizeMake((NSUInteger)list[j].count[0], (NSUInteger)list[j].count[1],
                                                 (NSUInteger)list[j].count[2])
               threadsPerThreadgroup:MTLSizeMake(SSS_WAVE, 1, 1)];
        }
        [ce endEncoding];
    }
    s->frame = d->present_count + 1u;
    return s->out;
}
