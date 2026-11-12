/*
 * Direct3D 9 shader bytecode (vs_2_0 to vs_3_0, ps_2_0 to ps_3_0) to Metal
 * Shading Language (MSL) translation.
 *
 * The translator reads the D3D9 token stream once (d3d_shader_parse) and
 * keeps the reflection data: the declared inputs and outputs, the sampler
 * types, the constants that the shader defines (def, defi, defb) and the
 * constant ranges that it reads. MSL source is then made for one pipeline
 * variant (d3d_shader_msl_vs, d3d_shader_msl_ps).
 *
 * Metal resource binding of the generated functions:
 *   vertex   buffer 0..15  vertex streams (stage_in)
 *            buffer 16     float4 c[256]   (float constants, defs applied)
 *            buffer 17     int4 i[16]
 *            buffer 18     uint b[16]
 *            buffer 19     float4 vs_misc: xy = half-pixel offset (in clip
 *                          units per w), z = constant depth bias (added to
 *                          z/w), w = 0
 *   fragment buffer 16     float4 c[224]
 *            buffer 17     int4 i[16]
 *            buffer 18     uint b[16]
 *            buffer 19     float4 ps_misc: x = alpha test function
 *                          (D3DCMPFUNC, 0 = off), y = alpha reference (0..1)
 *            texture n / sampler n  sampler register s#n
 *
 * Varyings are matched by D3D semantic: a vs_3_0 output and a ps_3_0
 * input with the same usage and usage index use the same user() name.
 */
#ifndef RT_D3D9_SHADER_H
#define RT_D3D9_SHADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D3DSH_MAX_IO 16
#define D3DSH_MAX_SAMPLERS 16
#define D3DSH_MAX_DEFS 256
#define D3DSH_VS_CONST_F 256
#define D3DSH_PS_CONST_F 224
#define D3DSH_CONST_I 16
#define D3DSH_CONST_B 16

/* D3DDECLUSAGE */
#define D3DDECLUSAGE_POSITION 0u
#define D3DDECLUSAGE_BLENDWEIGHT 1u
#define D3DDECLUSAGE_BLENDINDICES 2u
#define D3DDECLUSAGE_NORMAL 3u
#define D3DDECLUSAGE_PSIZE 4u
#define D3DDECLUSAGE_TEXCOORD 5u
#define D3DDECLUSAGE_TANGENT 6u
#define D3DDECLUSAGE_BINORMAL 7u
#define D3DDECLUSAGE_TESSFACTOR 8u
#define D3DDECLUSAGE_POSITIONT 9u
#define D3DDECLUSAGE_COLOR 10u
#define D3DDECLUSAGE_FOG 11u
#define D3DDECLUSAGE_DEPTH 12u
#define D3DDECLUSAGE_SAMPLE 13u

/* Sampler texture types (D3DSAMPLER_TEXTURE_TYPE >> 27). */
#define D3DSTT_UNKNOWN 0u
#define D3DSTT_2D 2u
#define D3DSTT_CUBE 3u
#define D3DSTT_VOLUME 4u

typedef struct d3dsh_io {
    uint8_t usage, index, reg, mask;
    bool centroid;
} d3dsh_io;

typedef struct d3dsh_def {
    uint16_t reg;
    uint32_t v[4]; /* bit patterns (float for def, int for defi, bool in v[0] for defb) */
} d3dsh_def;

typedef struct d3d_shader {
    bool is_ps;
    uint32_t version; /* major << 8 | minor */
    uint32_t *tokens; /* host copy of the token stream (owned) */
    uint32_t ntokens;
    int ninputs, noutputs;
    d3dsh_io inputs[D3DSH_MAX_IO];
    d3dsh_io outputs[D3DSH_MAX_IO];
    uint8_t sampler_type[D3DSH_MAX_SAMPLERS]; /* 0 = not used */
    int ndef_f, ndef_i, ndef_b;
    d3dsh_def def_f[D3DSH_MAX_DEFS], def_i[D3DSH_CONST_I], def_b[D3DSH_CONST_B];
    bool uses_vpos, uses_vface, writes_depth;
    /* Float constant registers that the shader can read: c0 to
       c(nconst_f - 1). Relative addressing gives the full range. The
       draw sends only these registers. */
    uint16_t nconst_f;
    uint8_t color_outputs; /* bit n: oC#n written */
} d3d_shader;

/* Dynamic text buffer for the generated source. */
typedef struct d3dsh_text {
    char *s;
    size_t len, cap;
} d3dsh_text;

void d3dsh_text_free(d3dsh_text *t);

/* Parse a shader. tokens points to host memory with at most max_tokens
   dwords (the end token must be inside). On failure returns false and
   writes a message to err. On success sh owns a copy of the tokens. */
bool d3d_shader_parse(d3d_shader *sh, const uint32_t *tokens, uint32_t max_tokens, char *err, size_t errn);
void d3d_shader_free(d3d_shader *sh);

/* Vertex shader MSL (function "vs_main"). */
bool d3d_shader_msl_vs(const d3d_shader *vs, d3dsh_text *out, char *err, size_t errn);

/* Pixel shader MSL (function "ps_main") for one pipeline variant:
   vs_outputs/nvs are the outputs of the vertex shader (inputs that the
   vertex shader does not write read 0); depth_compare bit n makes sampler
   n a comparison sampler on a depth texture (the result is the compare
   result, as with a D3D9 depth texture). */
bool d3d_shader_msl_ps(const d3d_shader *ps, const d3dsh_io *vs_outputs, int nvs, uint16_t depth_compare,
                       d3dsh_text *out, char *err, size_t errn);

#endif
