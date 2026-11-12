/*
 * Direct3D 9 shader bytecode to Metal Shading Language (see
 * rt_d3d9_shader.h).
 *
 * Token format (D3D9 DDK, d3d9types.h):
 *   version      0xFFFE#### (vertex) or 0xFFFF#### (pixel), major.minor
 *   instruction  bits 0-15 opcode, 16-23 opcode control, 24-27 length in
 *                dwords (shader model 2 and later), 28 predicated
 *   parameter    bits 0-10 register number, 28-30 and 11-12 register
 *                type, 13 relative addressing (an address token follows);
 *                destination: 16-19 write mask, 20-23 result modifier;
 *                source: 16-23 swizzle, 24-27 source modifier
 *   comment      0xFFFE with the length in bits 16-30
 *   end          0x0000FFFF
 *
 * Shader model 1 is not supported (the game has none).
 */
#include "rt_d3d9_shader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 65536u
#define MAX_SOURCE (4u << 20)
#define MAX_LABELS 64
#define MAX_CALL_DEPTH 4

/* Opcodes. */
enum {
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT,
    OP_SGE, OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3, OP_M3x2,
    OP_CALL, OP_CALLNZ, OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN, OP_ABS, OP_NRM,
    OP_SINCOS, OP_REP, OP_ENDREP, OP_IF, OP_IFC, OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC, OP_MOVA, OP_DEFB,
    OP_DEFI,
    OP_TEXKILL = 65, OP_TEX = 66, OP_EXPP = 78, OP_LOGP = 79, OP_DEF = 81, OP_CMP = 88, OP_DP2ADD = 90,
    OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93, OP_SETP = 94, OP_TEXLDL = 95, OP_BREAKP = 96,
    OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF
};

/* Register types. */
enum {
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, /* RT_TEXTURE in pixel shaders */
    RT_RASTOUT = 4, RT_ATTROUT = 5, RT_OUTPUT = 6,        /* RT_TEXCRDOUT before vs_3_0 */
    RT_CONSTINT = 7, RT_COLOROUT = 8, RT_DEPTHOUT = 9, RT_SAMPLER = 10, RT_CONST2 = 11, RT_CONST3 = 12,
    RT_CONST4 = 13, RT_CONSTBOOL = 14, RT_LOOP = 15, RT_TEMPFLOAT16 = 16, RT_MISCTYPE = 17, RT_LABEL = 18,
    RT_PREDICATE = 19
};

#define REL_BIT (1u << 13)
#define DST_SAT (1u << 20)
#define DST_CENTROID (4u << 20)

static uint32_t rtype(uint32_t p) { return ((p >> 28) & 7u) | ((p >> 8) & 0x18u); }
static uint32_t rnum(uint32_t p) { return p & 0x7FFu; }
static uint32_t wmask(uint32_t p) { return (p >> 16) & 0xFu; }

static const char *usage_name(uint32_t u)
{
    static const char *const n[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",
                                     "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT",
                                     "COLOR", "FOG", "DEPTH", "SAMPLE" };
    return u < sizeof n / sizeof n[0] ? n[u] : NULL;
}

void d3dsh_text_free(d3dsh_text *t)
{
    free(t->s);
    memset(t, 0, sizeof *t);
}

void d3d_shader_free(d3d_shader *sh)
{
    free(sh->tokens);
    memset(sh, 0, sizeof *sh);
}

/* ---------------------------------------------------------------- instruction walk */

typedef struct inst {
    uint32_t op, ctl, pos, n; /* pos: index of the instruction token; n: parameter dwords */
    bool pred;
    const uint32_t *p;
} inst;

/* Read the instruction at *pos. Returns false at the end token or on a
   format error (err set). Comments are skipped. */
static bool next_inst(const uint32_t *tk, uint32_t ntk, uint32_t *pos, inst *in, bool *end, char *err, size_t errn)
{
    *end = false;
    for (;;)
    {
        uint32_t t, op;
        if (*pos >= ntk)
        {
            snprintf(err, errn, "shader has no end token");
            return false;
        }
        t = tk[*pos];
        op = t & 0xFFFFu;
        if (op == OP_END)
        {
            *end = true;
            return false;
        }
        if (op == OP_COMMENT)
        {
            uint32_t len = (t >> 16) & 0x7FFFu;
            if (len > ntk - *pos - 1u)
            {
                snprintf(err, errn, "comment extends past the end of the shader");
                return false;
            }
            *pos += 1u + len;
            continue;
        }
        in->op = op;
        in->ctl = (t >> 16) & 0xFFu;
        in->pred = (t & (1u << 28)) != 0;
        in->n = (t >> 24) & 0xFu;
        in->pos = *pos;
        if (in->n > ntk - *pos - 1u)
        {
            snprintf(err, errn, "instruction at token %u extends past the end of the shader", *pos);
            return false;
        }
        in->p = tk + *pos + 1u;
        *pos += 1u + in->n;
        return true;
    }
}

/* ---------------------------------------------------------------- parse */

static d3dsh_io *add_io(d3dsh_io *arr, int *n, uint32_t usage, uint32_t index, uint32_t reg, uint32_t mask,
                        bool centroid, char *err, size_t errn)
{
    d3dsh_io *e;
    if (*n >= D3DSH_MAX_IO)
    {
        snprintf(err, errn, "more than %d input or output declarations", D3DSH_MAX_IO);
        return NULL;
    }
    e = &arr[(*n)++];
    e->usage = (uint8_t)usage;
    e->index = (uint8_t)index;
    e->reg = (uint8_t)reg;
    e->mask = (uint8_t)mask;
    e->centroid = centroid;
    return e;
}

static bool find_io(const d3dsh_io *arr, int n, uint32_t usage, uint32_t index)
{
    int i;
    for (i = 0; i < n; i++)
        if (arr[i].usage == usage && arr[i].index == index) return true;
    return false;
}

/* Read the constant table of a "CTAB" comment: b points to the table (after
   the fourcc), n is its size in bytes. The D3DX layout: a header with the
   constant count at byte 12 and the offset of the constant entries at byte
   16; entries of 20 bytes (name offset, register set u16, register index
   u16, register count u16, reserved u16, type offset, default offset); a
   type starts with its class u16. All offsets are from the start of the
   table. Entries with an offset outside the table are ignored. */
static uint32_t ct_u32(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }
static uint32_t ct_u16(const uint8_t *b) { uint16_t v; memcpy(&v, b, 2); return v; }

static void ctab_read(d3d_shader *sh, const uint8_t *b, uint32_t n)
{
    uint32_t cnt, info, i;
    int fb = -1, bloom_s = -1;
    bool bloomed = false, strength = false, cutoff = false, falloff = false, blur_o = false, blur_w = false;
    if (n < 20u) return;
    cnt = ct_u32(b + 12);
    info = ct_u32(b + 16);
    if (info > n || cnt > (n - info) / 20u) return;
    for (i = 0; i < cnt; i++)
    {
        const uint8_t *e = b + info + 20u * i;
        uint32_t name = ct_u32(e), set = ct_u16(e + 4), reg = ct_u16(e + 6), count = ct_u16(e + 8), type = ct_u32(e + 12);
        uint32_t cls = 0;
        const char *nm;
        if (name >= n || memchr(b + name, 0, n - name) == NULL) continue;
        nm = (const char *)(b + name);
        if (type <= n - 2u) cls = ct_u16(b + type);
        if (set == 2u /* float */ && count == 4u && reg + 4u <= D3DSH_VS_CONST_F &&
            (cls == D3DSH_CLASS_MATRIX_ROWS || cls == D3DSH_CLASS_MATRIX_COLUMNS))
        {
            if (strcmp(nm, "p3dWorldViewProjectionMatrix") == 0)
            {
                sh->ct_wvp = (int16_t)reg;
                sh->ct_wvp_class = (uint8_t)cls;
            }
            else if (strcmp(nm, "p3dWorldMatrix") == 0)
            {
                sh->ct_world = (int16_t)reg;
                sh->ct_world_class = (uint8_t)cls;
            }
            else if (strcmp(nm, "p3dShadowMapMatrix") == 0)
            {
                sh->ct_shadow = (int16_t)reg;
                sh->ct_shadow_class = (uint8_t)cls;
            }
        }
        else if (set == 3u /* sampler */ && strcmp(nm, "p3dFrameBufferCurrent") == 0 && reg < D3DSH_MAX_SAMPLERS)
            fb = (int)reg;
        else if (set == 3u && strcmp(nm, "BloomedScene") == 0 && reg < D3DSH_MAX_SAMPLERS)
        {
            bloomed = true;
            bloom_s = (int)reg;
        }
        else if (set == 2u && strcmp(nm, "BloomStrength") == 0)
            strength = true;
        else if (set == 2u && strcmp(nm, "Cutoff") == 0)
            cutoff = true;
        else if (set == 2u && strcmp(nm, "FalloffPower") == 0)
            falloff = true;
        else if (set == 2u && strcmp(nm, "BlurOffset1") == 0)
            blur_o = true;
        else if (set == 2u && strcmp(nm, "BlurWeight1") == 0)
            blur_w = true;
    }
    if (!sh->is_ps || fb < 0) return;
    if (bloomed && strength)
    {
        sh->ct_scene_sampler = (int8_t)fb;
        sh->ct_bloom_sampler = (int8_t)bloom_s;
    }
    else if (cutoff && falloff)
        sh->ct_role = D3DSH_ROLE_BLOOM_BRIGHT;
    else if (blur_o && blur_w)
        sh->ct_role = D3DSH_ROLE_BLOOM_BLUR;
}

/* Find the constant table in the comments of a parsed token stream. */
static void ctab_scan(d3d_shader *sh)
{
    uint32_t pos = 1;
    while (pos < sh->ntokens)
    {
        uint32_t t = sh->tokens[pos], op = t & 0xFFFFu, len;
        if (op == OP_END) return;
        if (op == OP_COMMENT)
        {
            len = (t >> 16) & 0x7FFFu;
            if (len > sh->ntokens - pos - 1u) return;
            if (len >= 2u && sh->tokens[pos + 1u] == 0x42415443u /* "CTAB" */)
                ctab_read(sh, (const uint8_t *)(sh->tokens + pos + 2u), (len - 1u) * 4u);
        }
        else
            len = (t >> 24) & 0xFu;
        pos += 1u + len;
    }
}

bool d3d_shader_parse(d3d_shader *sh, const uint32_t *tokens, uint32_t max_tokens, char *err, size_t errn)
{
    uint32_t pos = 1, ntk, major, minor;
    inst in;
    bool end = false;
    memset(sh, 0, sizeof *sh);
    if (max_tokens < 2u)
    {
        snprintf(err, errn, "shader is too short");
        return false;
    }
    if (max_tokens > MAX_TOKENS) max_tokens = MAX_TOKENS;
    if ((tokens[0] >> 16) == 0xFFFEu)
        sh->is_ps = false;
    else if ((tokens[0] >> 16) == 0xFFFFu)
        sh->is_ps = true;
    else
    {
        snprintf(err, errn, "bad version token 0x%08x", tokens[0]);
        return false;
    }
    major = (tokens[0] >> 8) & 0xFFu;
    minor = tokens[0] & 0xFFu;
    sh->version = major << 8 | minor;
    if (major < 2 || major > 3)
    {
        snprintf(err, errn, "%s_%u_%u is not supported (only shader models 2 and 3)", sh->is_ps ? "ps" : "vs", major,
                 minor);
        return false;
    }
    /* Find the end token, so that the copy has the exact length. */
    ntk = max_tokens;
    while (next_inst(tokens, ntk, &pos, &in, &end, err, errn))
    {
    }
    if (!end) return false;
    ntk = pos + 1u;
    sh->tokens = malloc(sizeof(uint32_t) * ntk);
    if (sh->tokens == NULL)
    {
        snprintf(err, errn, "out of memory");
        return false;
    }
    memcpy(sh->tokens, tokens, sizeof(uint32_t) * ntk);
    sh->ntokens = ntk;
    sh->ct_wvp = -1;
    sh->ct_world = -1;
    sh->ct_shadow = -1;
    sh->ct_scene_sampler = -1;
    sh->ct_bloom_sampler = -1;
    ctab_scan(sh);

    pos = 1;
    while (next_inst(sh->tokens, ntk, &pos, &in, &end, err, errn))
    {
        if (in.op != OP_DCL && in.op != OP_DEF && in.op != OP_DEFI && in.op != OP_DEFB)
        {
            /* Float constant reads. Parameter 0 is the destination, except
               for the instructions that have only sources. A source with
               REL_BIT is followed by its address token. */
            uint32_t limit = sh->is_ps ? D3DSH_PS_CONST_F : D3DSH_VS_CONST_F, i;
            bool no_dst = in.op == OP_TEXKILL || in.op == OP_IF || in.op == OP_IFC || in.op == OP_REP ||
                          in.op == OP_LOOP || in.op == OP_BREAKC || in.op == OP_BREAKP || in.op == OP_CALL ||
                          in.op == OP_CALLNZ || in.op == OP_LABEL;
            for (i = no_dst ? 0u : 1u; i < in.n; i++)
            {
                uint32_t tok = in.p[i];
                if (rtype(tok) == RT_CONST)
                {
                    if (tok & REL_BIT)
                        sh->nconst_f = (uint16_t)limit;
                    else if (rnum(tok) < limit && rnum(tok) + 1u > sh->nconst_f)
                        sh->nconst_f = (uint16_t)(rnum(tok) + 1u);
                }
                if (tok & REL_BIT) i++;
            }
        }
        if (in.op == OP_DCL)
        {
            uint32_t u, r, t, reg;
            if (in.n < 2u) goto bad_len;
            u = in.p[0];
            r = in.p[1];
            t = rtype(r);
            reg = rnum(r);
            if (t == RT_SAMPLER)
            {
                uint32_t st = (u >> 27) & 0xFu;
                if (reg >= D3DSH_MAX_SAMPLERS || (st != D3DSTT_2D && st != D3DSTT_CUBE && st != D3DSTT_VOLUME))
                {
                    snprintf(err, errn, "bad sampler declaration s%u type %u", reg, st);
                    goto fail;
                }
                sh->sampler_type[reg] = (uint8_t)st;
            }
            else if (t == RT_INPUT || (sh->is_ps && t == RT_ADDR /* t# */))
            {
                uint32_t usage = u & 0x1Fu, index = (u >> 16) & 0xFu;
                if (sh->version < 0x300 && sh->is_ps)
                {
                    usage = t == RT_INPUT ? D3DDECLUSAGE_COLOR : D3DDECLUSAGE_TEXCOORD;
                    index = reg;
                }
                if (usage_name(usage) == NULL || reg >= D3DSH_MAX_IO)
                {
                    snprintf(err, errn, "bad input declaration (usage %u, register %u)", usage, reg);
                    goto fail;
                }
                if (sh->is_ps && sh->version < 0x300 && t == RT_ADDR) reg += 8u; /* t# after v# */
                if (add_io(sh->inputs, &sh->ninputs, usage, index, reg, wmask(r), (r & DST_CENTROID) != 0, err,
                           errn) == NULL)
                    goto fail;
            }
            else if (t == RT_OUTPUT && !sh->is_ps && sh->version >= 0x300)
            {
                uint32_t usage = u & 0x1Fu, index = (u >> 16) & 0xFu;
                if (usage_name(usage) == NULL || reg >= 12u)
                {
                    snprintf(err, errn, "bad output declaration (usage %u, register %u)", usage, reg);
                    goto fail;
                }
                if (find_io(sh->outputs, sh->noutputs, usage, index))
                {
                    snprintf(err, errn, "output %s%u is declared twice", usage_name(usage), index);
                    goto fail;
                }
                if (add_io(sh->outputs, &sh->noutputs, usage, index, reg, wmask(r), false, err, errn) == NULL)
                    goto fail;
            }
            else if (t == RT_MISCTYPE)
            {
                if (reg == 0) sh->uses_vpos = true;
                else if (reg == 1) sh->uses_vface = true;
                else
                {
                    snprintf(err, errn, "bad misc register %u", reg);
                    goto fail;
                }
            }
            else
            {
                snprintf(err, errn, "unsupported declaration of register type %u", t);
                goto fail;
            }
        }
        else if (in.op == OP_DEF || in.op == OP_DEFI)
        {
            d3dsh_def *d;
            uint32_t reg;
            if (in.n < 5u) goto bad_len;
            reg = rnum(in.p[0]);
            if (in.op == OP_DEF)
            {
                if (reg >= (sh->is_ps ? D3DSH_PS_CONST_F : D3DSH_VS_CONST_F) || sh->ndef_f >= D3DSH_MAX_DEFS)
                {
                    snprintf(err, errn, "bad def c%u", reg);
                    goto fail;
                }
                d = &sh->def_f[sh->ndef_f++];
            }
            else
            {
                if (reg >= D3DSH_CONST_I || sh->ndef_i >= D3DSH_CONST_I)
                {
                    snprintf(err, errn, "bad defi i%u", reg);
                    goto fail;
                }
                d = &sh->def_i[sh->ndef_i++];
            }
            d->reg = (uint16_t)reg;
            memcpy(d->v, in.p + 1, 16);
        }
        else if (in.op == OP_DEFB)
        {
            uint32_t reg;
            if (in.n < 2u) goto bad_len;
            reg = rnum(in.p[0]);
            if (reg >= D3DSH_CONST_B || sh->ndef_b >= D3DSH_CONST_B)
            {
                snprintf(err, errn, "bad defb b%u", reg);
                goto fail;
            }
            sh->def_b[sh->ndef_b].reg = (uint16_t)reg;
            sh->def_b[sh->ndef_b].v[0] = in.p[1];
            sh->ndef_b++;
        }
        else if (in.n > 0u && in.op != OP_TEXKILL && in.op != OP_IF && in.op != OP_IFC && in.op != OP_REP &&
                 in.op != OP_LOOP && in.op != OP_BREAKC && in.op != OP_BREAKP && in.op != OP_CALL &&
                 in.op != OP_CALLNZ && in.op != OP_LABEL)
        {
            uint32_t d = in.p[0], t = rtype(d), reg = rnum(d);
            if (t == RT_COLOROUT && reg < 4u) sh->color_outputs = (uint8_t)(sh->color_outputs | (1u << reg));
            if (t == RT_DEPTHOUT) sh->writes_depth = true;
            if (!sh->is_ps && sh->version < 0x300)
            {
                /* vs_2_x outputs have no declarations. */
                uint32_t usage = 0xFF, index = 0;
                if (t == RT_RASTOUT && reg <= 2u)
                {
                    static const uint8_t ru[3] = { D3DDECLUSAGE_POSITION, D3DDECLUSAGE_FOG, D3DDECLUSAGE_PSIZE };
                    usage = ru[reg];
                }
                else if (t == RT_ATTROUT && reg < 2u)
                {
                    usage = D3DDECLUSAGE_COLOR;
                    index = reg;
                }
                else if (t == RT_OUTPUT && reg < 8u)
                {
                    usage = D3DDECLUSAGE_TEXCOORD;
                    index = reg;
                }
                if (usage != 0xFF && !find_io(sh->outputs, sh->noutputs, usage, index))
                {
                    /* reg: 0..2 rasterizer outputs, 4..5 colors, 8..15 texture coordinates. */
                    uint32_t r = t == RT_RASTOUT ? reg : (t == RT_ATTROUT ? 4u + reg : 8u + reg);
                    if (add_io(sh->outputs, &sh->noutputs, usage, index, r, 0xF, false, err, errn) == NULL) goto fail;
                }
            }
        }
    }
    if (!end) goto fail;
    if (!sh->is_ps && !find_io(sh->outputs, sh->noutputs, D3DDECLUSAGE_POSITION, 0))
    {
        snprintf(err, errn, "vertex shader writes no position");
        goto fail;
    }
    return true;
bad_len:
    snprintf(err, errn, "instruction %u at token %u is too short", in.op, in.pos);
fail:
    d3d_shader_free(sh);
    return false;
}

/* ---------------------------------------------------------------- code generation */

typedef struct gen {
    const d3d_shader *sh;
    d3dsh_text *out;
    char *err;
    size_t errn;
    bool failed;
    int indent, loop_depth, call_depth;
    uint16_t depth_compare;
    uint32_t pcss; /* PCSS level for the comparison samplers (0 = off) */
    bool contact;  /* multiply comparison results by the contact shadow */
    uint32_t labels[MAX_LABELS]; /* token position of each label instruction, 0 = none */
} gen;

static void fail(gen *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(gen *g, const char *fmt, ...)
{
    va_list ap;
    if (g->failed) return;
    g->failed = true;
    va_start(ap, fmt);
    vsnprintf(g->err, g->errn, fmt, ap);
    va_end(ap);
}

static void emit(gen *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit(gen *g, const char *fmt, ...)
{
    va_list ap;
    d3dsh_text *t = g->out;
    int n;
    if (g->failed) return;
    for (;;)
    {
        size_t avail = t->cap - t->len;
        int pad = g->indent * 4;
        if (t->s != NULL && avail > (size_t)pad + 1u)
        {
            memset(t->s + t->len, ' ', (size_t)pad);
            va_start(ap, fmt);
            n = vsnprintf(t->s + t->len + (size_t)pad, avail - (size_t)pad, fmt, ap);
            va_end(ap);
            if (n < 0)
            {
                fail(g, "text format error");
                return;
            }
            if ((size_t)n < avail - (size_t)pad)
            {
                t->len += (size_t)pad + (size_t)n;
                return;
            }
        }
        {
            size_t nc = t->cap ? t->cap * 2u : 16384u;
            char *ns;
            if (nc > MAX_SOURCE)
            {
                fail(g, "generated source is larger than %u bytes", MAX_SOURCE);
                return;
            }
            ns = realloc(t->s, nc);
            if (ns == NULL)
            {
                fail(g, "out of memory");
                return;
            }
            t->s = ns;
            t->cap = nc;
            t->s[t->len] = 0;
        }
    }
}

/* Small expression buffers. */
typedef struct expr {
    char s[384];
} expr;

static void ex_set(gen *g, expr *e, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void ex_set(gen *g, expr *e, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(e->s, sizeof e->s, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof e->s) fail(g, "expression too long");
}

static const char comp[4] = { 'x', 'y', 'z', 'w' };

/* Source parameter reader: *i is the index into in->p; returns the token
   and the relative address token (0 if none). */
static bool read_src(gen *g, const inst *in, uint32_t *i, uint32_t *tok, uint32_t *rel)
{
    if (*i >= in->n)
    {
        fail(g, "instruction %u at token %u has too few parameters", in->op, in->pos);
        return false;
    }
    *tok = in->p[(*i)++];
    *rel = 0;
    if (*tok & REL_BIT)
    {
        if (*i >= in->n)
        {
            fail(g, "relative address token missing at token %u", in->pos);
            return false;
        }
        *rel = in->p[(*i)++];
    }
    return true;
}

/* Name of a register read as float4 (no swizzle or modifier). */
static void reg_name(gen *g, uint32_t tok, uint32_t rel, expr *e)
{
    const d3d_shader *sh = g->sh;
    uint32_t t = rtype(tok), n = rnum(tok);
    switch (t)
    {
    case RT_TEMP: ex_set(g, e, "r%u", n); break;
    case RT_INPUT:
        if (rel) fail(g, "relative addressing of input registers is not supported");
        ex_set(g, e, "v%u", n);
        break;
    case RT_ADDR:
        if (sh->is_ps)
            ex_set(g, e, "v%u", n + 8u); /* t# (ps_2_x) */
        else
            ex_set(g, e, "float4(a0)");
        break;
    case RT_CONST: {
        uint32_t limit = sh->is_ps ? D3DSH_PS_CONST_F : D3DSH_VS_CONST_F;
        if (rel)
        {
            uint32_t rt = rtype(rel), sel = (rel >> 16) & 3u;
            if (sh->nconst_f != limit)
                fail(g, "internal error: the constant scan missed relative addressing at c%u", n);
            if (rt == RT_ADDR)
                ex_set(g, e, "c[clamp(a0.%c + %u, 0, %u)]", comp[sel], n, limit - 1u);
            else if (rt == RT_LOOP)
                ex_set(g, e, "c[clamp(aL + %u, 0, %u)]", n, limit - 1u);
            else
                fail(g, "relative address register type %u is not supported", rt);
        }
        else
        {
            if (n >= limit) fail(g, "constant c%u is out of range", n);
            if (n >= sh->nconst_f) fail(g, "internal error: the constant scan missed c%u", n);
            ex_set(g, e, "c[%u]", n);
        }
        break;
    }
    case RT_CONSTINT:
        if (n >= D3DSH_CONST_I) fail(g, "constant i%u is out of range", n);
        ex_set(g, e, "float4(ci[%u])", n);
        break;
    case RT_CONSTBOOL:
        if (n >= D3DSH_CONST_B) fail(g, "constant b%u is out of range", n);
        ex_set(g, e, "float4(cb[%u] != 0u ? 1.0 : 0.0)", n);
        break;
    case RT_LOOP: ex_set(g, e, "float4(float(aL))"); break;
    case RT_MISCTYPE:
        if (n == 0) ex_set(g, e, "vpos");
        else if (n == 1) ex_set(g, e, "float4(vface)");
        else fail(g, "misc register %u", n);
        break;
    case RT_TEMPFLOAT16: ex_set(g, e, "r%u", n); break;
    default: fail(g, "source register type %u is not supported", t); break;
    }
}

/* Full source expression with swizzle and modifier (float4). */
static void src_expr(gen *g, uint32_t tok, uint32_t rel, expr *e)
{
    expr base;
    uint32_t sw = (tok >> 16) & 0xFFu, mod = (tok >> 24) & 0xFu;
    char swz[6] = "";
    reg_name(g, tok, rel, &base);
    if (sw != 0xE4u)
    {
        swz[0] = '.';
        swz[1] = comp[sw & 3u];
        swz[2] = comp[(sw >> 2) & 3u];
        swz[3] = comp[(sw >> 4) & 3u];
        swz[4] = comp[(sw >> 6) & 3u];
        swz[5] = 0;
    }
    switch (mod)
    {
    case 0: ex_set(g, e, "%s%s", base.s, swz); break;
    case 1: ex_set(g, e, "(-%s%s)", base.s, swz); break;
    case 11: ex_set(g, e, "abs(%s%s)", base.s, swz); break;
    case 12: ex_set(g, e, "(-abs(%s%s))", base.s, swz); break;
    default: fail(g, "source modifier %u is not supported", mod); break;
    }
}

static bool next_src(gen *g, const inst *in, uint32_t *i, expr *e)
{
    uint32_t tok, rel;
    if (!read_src(g, in, i, &tok, &rel)) return false;
    src_expr(g, tok, rel, e);
    return !g->failed;
}

/* Destination register name (float4 lvalue) and its write mask. */
static void dst_name(gen *g, uint32_t tok, expr *e)
{
    const d3d_shader *sh = g->sh;
    uint32_t t = rtype(tok), n = rnum(tok);
    if (tok & REL_BIT) fail(g, "relative addressing of a destination is not supported");
    switch (t)
    {
    case RT_TEMP:
    case RT_TEMPFLOAT16: ex_set(g, e, "r%u", n); break;
    case RT_ADDR:
        if (sh->is_ps) fail(g, "pixel shader writes a texture register");
        ex_set(g, e, "a0");
        break;
    case RT_RASTOUT: ex_set(g, e, "orast%u", n); break;
    case RT_ATTROUT: ex_set(g, e, "oattr%u", n); break;
    case RT_OUTPUT: ex_set(g, e, sh->version >= 0x300 ? "o%u" : "otex%u", n); break;
    case RT_COLOROUT:
        if (n >= 4u) fail(g, "color output %u", n);
        ex_set(g, e, "oC%u", n);
        break;
    case RT_DEPTHOUT: ex_set(g, e, "oDepth"); break;
    case RT_PREDICATE: ex_set(g, e, "p0"); break;
    default: fail(g, "destination register type %u is not supported", t); break;
    }
}

static void mask_str(uint32_t m, char out[6])
{
    int k = 0, c;
    out[k++] = '.';
    for (c = 0; c < 4; c++)
        if (m & (1u << c)) out[k++] = comp[c];
    out[k] = 0;
}

/* Assign a float4 expression to the destination of instruction in. */
static void assign(gen *g, const inst *in, uint32_t dtok, uint32_t ptok, const char *value)
{
    expr d;
    char m[6];
    uint32_t mask = wmask(dtok), t = rtype(dtok);
    bool sat = (dtok & DST_SAT) != 0;
    if ((dtok >> 24) & 0xFu) fail(g, "destination shift is not supported");
    dst_name(g, dtok, &d);
    if (mask == 0) return;
    mask_str(mask, m);
    if (t == RT_DEPTHOUT)
    {
        emit(g, "oDepth = %s(%s).x;\n", sat ? "saturate" : "", value);
        return;
    }
    if (t == RT_ADDR)
    {
        emit(g, "a0%s = int4(floor((%s) + 0.5))%s;\n", m, value, m);
        return;
    }
    if (t == RT_PREDICATE)
    {
        fail(g, "predicate written by instruction %u", in->op);
        return;
    }
    if (in->pred)
    {
        expr p;
        uint32_t sw = (ptok >> 16) & 0xFFu;
        char ps[6];
        bool notp = ((ptok >> 24) & 0xFu) == 13u;
        ps[0] = '.';
        ps[1] = comp[sw & 3u];
        ps[2] = comp[(sw >> 2) & 3u];
        ps[3] = comp[(sw >> 4) & 3u];
        ps[4] = comp[(sw >> 6) & 3u];
        ps[5] = 0;
        ex_set(g, &p, "%sp0%s", notp ? "!" : "", ps);
        emit(g, "%s%s = select(%s, %s(%s), %s)%s;\n", d.s, m, d.s, sat ? "saturate" : "", value, p.s, m);
        return;
    }
    if (mask == 0xFu)
        emit(g, "%s = %s(%s);\n", d.s, sat ? "saturate" : "", value);
    else
        emit(g, "%s%s = %s(%s)%s;\n", d.s, m, sat ? "saturate" : "", value, m);
}

static const char *cmp_op(gen *g, uint32_t ctl)
{
    switch (ctl)
    {
    case 1: return ">";
    case 2: return "==";
    case 3: return ">=";
    case 4: return "<";
    case 5: return "!=";
    case 6: return "<=";
    default: fail(g, "comparison %u", ctl); return "==";
    }
}

/* Texture sample. coord: float4 expression; stok: sampler token. */
static void tex_sample(gen *g, const inst *in, uint32_t dtok, uint32_t ptok, const char *coord, uint32_t stok,
                       const char *grad_x, const char *grad_y)
{
    uint32_t s = rnum(stok), st, sw = (stok >> 16) & 0xFFu;
    bool proj = in->op == OP_TEX && in->ctl == 1, bias = in->op == OP_TEX && in->ctl == 2;
    bool lod = in->op == OP_TEXLDL, grad = in->op == OP_TEXLDD;
    const char *tp = g->sh->is_ps ? "" : "v";
    expr c, v, r;
    char swz[6] = "";
    if (rtype(stok) != RT_SAMPLER || s >= D3DSH_MAX_SAMPLERS || g->sh->sampler_type[s] == 0)
    {
        fail(g, "texture instruction at token %u uses an undeclared sampler", in->pos);
        return;
    }
    st = g->sh->sampler_type[s];
    if (sw != 0xE4u)
    {
        swz[0] = '.';
        swz[1] = comp[sw & 3u];
        swz[2] = comp[(sw >> 2) & 3u];
        swz[3] = comp[(sw >> 4) & 3u];
        swz[4] = comp[(sw >> 6) & 3u];
        swz[5] = 0;
    }
    if (st == D3DSTT_2D)
        ex_set(g, &c, proj ? "((%s).xy / (%s).w)" : "(%s).xy", coord, coord);
    else
        ex_set(g, &c, proj ? "((%s).xyz / (%s).w)" : "(%s).xyz", coord, coord);
    if (g->depth_compare & (1u << s))
    {
        if (st != D3DSTT_2D || lod || grad || bias)
        {
            fail(g, "depth comparison sampler s%u is not a plain 2D sample", s);
            return;
        }
        ex_set(g, &r, proj ? "((%s).z / (%s).w)" : "(%s).z", coord, coord);
        if (g->pcss != 0)
            ex_set(g, &v, "float4(d3d_pcss(%st%u, %ss%u, %s, %s)%s)%s", tp, s, tp, s, c.s, r.s,
                   g->contact ? " * d3d_csv" : "", swz);
        else
            ex_set(g, &v, "float4(%st%u.sample_compare(%ss%u, %s, %s)%s)%s", tp, s, tp, s, c.s, r.s,
                   g->contact ? " * d3d_csv" : "", swz);
    }
    else if (lod)
        ex_set(g, &v, "%st%u.sample(%ss%u, %s, level((%s).w))%s", tp, s, tp, s, c.s, coord, swz);
    else if (bias)
        ex_set(g, &v, "%st%u.sample(%ss%u, %s, bias((%s).w))%s", tp, s, tp, s, c.s, coord, swz);
    else if (grad)
    {
        const char *gf = st == D3DSTT_2D ? "gradient2d" : (st == D3DSTT_CUBE ? "gradientcube" : "gradient3d");
        const char *gc = st == D3DSTT_2D ? "xy" : "xyz";
        ex_set(g, &v, "%st%u.sample(%ss%u, %s, %s((%s).%s, (%s).%s))%s", tp, s, tp, s, c.s, gf, grad_x, gc, grad_y,
               gc, swz);
    }
    else
    {
        if (!g->sh->is_ps)
        {
            fail(g, "vertex shader texture instruction without a level (texldl is required)");
            return;
        }
        ex_set(g, &v, "t%u.sample(s%u, %s)%s", s, s, c.s, swz);
    }
    assign(g, in, dtok, ptok, v.s);
}

static uint32_t matrix_rows(uint32_t op)
{
    switch (op)
    {
    case OP_M4x4: return 4;
    case OP_M4x3: return 3;
    case OP_M3x4: return 4;
    case OP_M3x3: return 3;
    case OP_M3x2: return 2;
    default: return 0;
    }
}

static void gen_block(gen *g, uint32_t pos);

static void gen_inst(gen *g, const inst *in)
{
    uint32_t i = 0, dtok = 0, ptok = 0;
    expr a, b, c, v;
    switch (in->op)
    {
    case OP_NOP:
    case OP_DCL:
    case OP_DEF:
    case OP_DEFI:
    case OP_DEFB: return;
    case OP_PHASE: fail(g, "phase instruction (ps_1_4) is not supported"); return;
    case OP_ELSE:
        g->indent--;
        emit(g, "} else {\n");
        g->indent++;
        return;
    case OP_ENDIF:
    case OP_ENDREP:
        g->indent--;
        emit(g, "}\n");
        if (in->op == OP_ENDREP) g->loop_depth--;
        return;
    case OP_ENDLOOP:
        g->indent--;
        emit(g, "}\n");
        emit(g, "aL = aL_save%d;\n", g->loop_depth);
        g->indent--;
        emit(g, "}\n");
        g->loop_depth--;
        return;
    case OP_BREAK: emit(g, "break;\n"); return;
    case OP_IF: {
        uint32_t tok, rel;
        if (!read_src(g, in, &i, &tok, &rel)) return;
        if (rtype(tok) == RT_CONSTBOOL)
        {
            bool notb = ((tok >> 24) & 0xFu) == 13u;
            if (rnum(tok) >= D3DSH_CONST_B) fail(g, "b%u out of range", rnum(tok));
            emit(g, "if (cb[%u] %s 0u) {\n", rnum(tok), notb ? "==" : "!=");
        }
        else if (rtype(tok) == RT_PREDICATE)
        {
            bool notp = ((tok >> 24) & 0xFu) == 13u;
            emit(g, "if (%sp0.%c) {\n", notp ? "!" : "", comp[(tok >> 16) & 3u]);
        }
        else
            fail(g, "if on register type %u", rtype(tok));
        g->indent++;
        return;
    }
    case OP_IFC:
    case OP_BREAKC: {
        const char *op = cmp_op(g, in->ctl);
        if (!next_src(g, in, &i, &a) || !next_src(g, in, &i, &b)) return;
        if (in->op == OP_IFC)
        {
            emit(g, "if ((%s).x %s (%s).x) {\n", a.s, op, b.s);
            g->indent++;
        }
        else
            emit(g, "if ((%s).x %s (%s).x) break;\n", a.s, op, b.s);
        return;
    }
    case OP_BREAKP: {
        uint32_t tok, rel;
        if (!read_src(g, in, &i, &tok, &rel)) return;
        emit(g, "if (%sp0.%c) break;\n", ((tok >> 24) & 0xFu) == 13u ? "!" : "", comp[(tok >> 16) & 3u]);
        return;
    }
    case OP_REP: {
        uint32_t tok, rel;
        if (!read_src(g, in, &i, &tok, &rel)) return;
        if (rtype(tok) != RT_CONSTINT || rnum(tok) >= D3DSH_CONST_I)
        {
            fail(g, "rep on a register that is not i#");
            return;
        }
        g->loop_depth++;
        emit(g, "for (int rep%d = 0; rep%d < min(ci[%u].x, 255); rep%d++) {\n", g->loop_depth, g->loop_depth,
             rnum(tok), g->loop_depth);
        g->indent++;
        return;
    }
    case OP_LOOP: {
        uint32_t ltok, lrel, tok, rel, n;
        if (!read_src(g, in, &i, &ltok, &lrel) || !read_src(g, in, &i, &tok, &rel)) return;
        if (rtype(tok) != RT_CONSTINT || rnum(tok) >= D3DSH_CONST_I)
        {
            fail(g, "loop on a register that is not i#");
            return;
        }
        n = rnum(tok);
        g->loop_depth++;
        emit(g, "{\n");
        g->indent++;
        emit(g, "int aL_save%d = aL;\n", g->loop_depth);
        emit(g, "aL = ci[%u].y;\n", n);
        emit(g, "for (int lp%d = 0; lp%d < min(ci[%u].x, 255); lp%d++, aL += ci[%u].z) {\n", g->loop_depth,
             g->loop_depth, n, g->loop_depth, n);
        g->indent++;
        return;
    }
    case OP_CALL:
    case OP_CALLNZ: {
        uint32_t ltok, lrel, l;
        if (!read_src(g, in, &i, &ltok, &lrel)) return;
        l = rnum(ltok);
        if (l >= MAX_LABELS || g->labels[l] == 0)
        {
            fail(g, "call of an undefined label %u", l);
            return;
        }
        if (g->call_depth >= MAX_CALL_DEPTH)
        {
            fail(g, "call depth is more than %d", MAX_CALL_DEPTH);
            return;
        }
        if (in->op == OP_CALLNZ)
        {
            uint32_t tok, rel;
            if (!read_src(g, in, &i, &tok, &rel)) return;
            if (rtype(tok) == RT_CONSTBOOL)
                emit(g, "if (cb[%u] %s 0u) {\n", rnum(tok) & 15u, ((tok >> 24) & 0xFu) == 13u ? "==" : "!=");
            else if (rtype(tok) == RT_PREDICATE)
                emit(g, "if (%sp0.%c) {\n", ((tok >> 24) & 0xFu) == 13u ? "!" : "", comp[(tok >> 16) & 3u]);
            else
            {
                fail(g, "callnz on register type %u", rtype(tok));
                return;
            }
        }
        else
            emit(g, "{\n");
        g->indent++;
        g->call_depth++;
        gen_block(g, g->labels[l]);
        g->call_depth--;
        g->indent--;
        emit(g, "}\n");
        return;
    }
    case OP_TEXKILL: {
        uint32_t t = in->n > 0 ? in->p[0] : 0, m = wmask(t), k;
        expr r;
        char cond[96] = "";
        size_t len = 0;
        if (in->n < 1)
        {
            fail(g, "texkill without operand");
            return;
        }
        if (rtype(t) == RT_TEMP)
            ex_set(g, &r, "r%u", rnum(t));
        else if (rtype(t) == RT_ADDR && g->sh->is_ps)
            ex_set(g, &r, "v%u", rnum(t) + 8u);
        else if (rtype(t) == RT_INPUT)
            ex_set(g, &r, "v%u", rnum(t));
        else
        {
            fail(g, "texkill on register type %u", rtype(t));
            return;
        }
        if (m == 0) m = 0xF;
        for (k = 0; k < 4; k++)
        {
            if (!(m & (1u << k))) continue;
            len += (size_t)snprintf(cond + len, sizeof cond - len, "%s%s.%c < 0.0", len ? " || " : "", r.s, comp[k]);
        }
        emit(g, "if (%s) discard_fragment();\n", cond);
        return;
    }
    case OP_SETP: {
        expr d;
        char m[6];
        uint32_t dt;
        if (in->n < 1) return;
        dt = in->p[i++];
        if (rtype(dt) != RT_PREDICATE)
        {
            fail(g, "setp destination is not p0");
            return;
        }
        if (!next_src(g, in, &i, &a) || !next_src(g, in, &i, &b)) return;
        mask_str(wmask(dt), m);
        dst_name(g, dt, &d);
        emit(g, "p0%s = ((%s) %s (%s))%s;\n", m, a.s, cmp_op(g, in->ctl), b.s, m);
        return;
    }
    default: break;
    }

    /* Instructions with a destination. */
    if (in->n < 1)
    {
        fail(g, "instruction %u at token %u has no destination", in->op, in->pos);
        return;
    }
    dtok = in->p[i++];
    if (in->pred)
    {
        if (i >= in->n)
        {
            fail(g, "predicate token missing");
            return;
        }
        ptok = in->p[i++];
    }
    switch (in->op)
    {
    case OP_MOV:
    case OP_MOVA:
        if (next_src(g, in, &i, &a)) assign(g, in, dtok, ptok, a.s);
        return;
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_MIN:
    case OP_MAX:
    case OP_SLT:
    case OP_SGE:
    case OP_DP3:
    case OP_DP4:
    case OP_CRS:
    case OP_DST:
        if (!next_src(g, in, &i, &a) || !next_src(g, in, &i, &b)) return;
        switch (in->op)
        {
        case OP_ADD: ex_set(g, &v, "%s + %s", a.s, b.s); break;
        case OP_SUB: ex_set(g, &v, "%s - %s", a.s, b.s); break;
        case OP_MUL: ex_set(g, &v, "%s * %s", a.s, b.s); break;
        case OP_MIN: ex_set(g, &v, "min(%s, %s)", a.s, b.s); break;
        case OP_MAX: ex_set(g, &v, "max(%s, %s)", a.s, b.s); break;
        case OP_SLT: ex_set(g, &v, "select(float4(0.0), float4(1.0), %s < %s)", a.s, b.s); break;
        case OP_SGE: ex_set(g, &v, "select(float4(0.0), float4(1.0), %s >= %s)", a.s, b.s); break;
        case OP_DP3: ex_set(g, &v, "float4(dot((%s).xyz, (%s).xyz))", a.s, b.s); break;
        case OP_DP4: ex_set(g, &v, "float4(dot(%s, %s))", a.s, b.s); break;
        case OP_CRS: ex_set(g, &v, "float4(cross((%s).xyz, (%s).xyz), 0.0)", a.s, b.s); break;
        default: ex_set(g, &v, "float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a.s, b.s, a.s, b.s); break;
        }
        assign(g, in, dtok, ptok, v.s);
        return;
    case OP_MAD:
    case OP_LRP:
    case OP_CMP:
    case OP_DP2ADD:
        if (!next_src(g, in, &i, &a) || !next_src(g, in, &i, &b) || !next_src(g, in, &i, &c)) return;
        if (in->op == OP_MAD)
            ex_set(g, &v, "fma(%s, %s, %s)", a.s, b.s, c.s);
        else if (in->op == OP_LRP)
            ex_set(g, &v, "mix(%s, %s, %s)", c.s, b.s, a.s);
        else if (in->op == OP_CMP)
            ex_set(g, &v, "select(%s, %s, %s >= 0.0)", c.s, b.s, a.s);
        else
            ex_set(g, &v, "float4(dot((%s).xy, (%s).xy) + (%s).x)", a.s, b.s, c.s);
        assign(g, in, dtok, ptok, v.s);
        return;
    case OP_RCP:
    case OP_RSQ:
    case OP_EXP:
    case OP_EXPP:
    case OP_LOG:
    case OP_LOGP:
    case OP_FRC:
    case OP_ABS:
    case OP_NRM:
    case OP_SGN:
    case OP_LIT:
    case OP_DSX:
    case OP_DSY:
    case OP_SINCOS:
        if (!next_src(g, in, &i, &a)) return;
        switch (in->op)
        {
        case OP_RCP: ex_set(g, &v, "float4(1.0 / (%s).x)", a.s); break;
        case OP_RSQ: ex_set(g, &v, "float4(rsqrt(abs((%s).x)))", a.s); break;
        case OP_EXP:
        case OP_EXPP: ex_set(g, &v, "float4(exp2((%s).x))", a.s); break;
        case OP_LOG:
        case OP_LOGP: ex_set(g, &v, "float4(log2(abs((%s).x)))", a.s); break;
        case OP_FRC: ex_set(g, &v, "fract(%s)", a.s); break;
        case OP_ABS: ex_set(g, &v, "abs(%s)", a.s); break;
        case OP_NRM: ex_set(g, &v, "((%s) * rsqrt(dot((%s).xyz, (%s).xyz)))", a.s, a.s, a.s); break;
        case OP_SGN: ex_set(g, &v, "sign(%s)", a.s); break;
        case OP_LIT: ex_set(g, &v, "d3d_lit(%s)", a.s); break;
        case OP_DSX: ex_set(g, &v, "dfdx(%s)", a.s); break;
        case OP_DSY: ex_set(g, &v, "dfdy(%s)", a.s); break;
        default: ex_set(g, &v, "float4(cos((%s).x), sin((%s).x), 0.0, 0.0)", a.s, a.s); break;
        }
        assign(g, in, dtok, ptok, v.s);
        return;
    case OP_POW:
        if (!next_src(g, in, &i, &a) || !next_src(g, in, &i, &b)) return;
        ex_set(g, &v, "float4(pow(abs((%s).x), (%s).x))", a.s, b.s);
        assign(g, in, dtok, ptok, v.s);
        return;
    case OP_M4x4:
    case OP_M4x3:
    case OP_M3x4:
    case OP_M3x3:
    case OP_M3x2: {
        uint32_t mtok, mrel, rows = matrix_rows(in->op), k;
        bool four = in->op == OP_M4x4 || in->op == OP_M4x3;
        char parts[4][384];
        if (!next_src(g, in, &i, &a) || !read_src(g, in, &i, &mtok, &mrel)) return;
        for (k = 0; k < 4; k++)
        {
            if (k < rows)
            {
                uint32_t t2 = (mtok & ~0x7FFu) | ((rnum(mtok) + k) & 0x7FFu);
                expr m;
                src_expr(g, t2, mrel, &m);
                snprintf(parts[k], sizeof parts[k], four ? "dot(%s, %s)" : "dot((%s).xyz, (%s).xyz)", a.s, m.s);
            }
            else
                snprintf(parts[k], sizeof parts[k], "0.0");
        }
        ex_set(g, &v, "float4(%s, %s, %s, %s)", parts[0], parts[1], parts[2], parts[3]);
        assign(g, in, dtok, ptok, v.s);
        return;
    }
    case OP_TEX:
    case OP_TEXLDL: {
        uint32_t stok, srel;
        if (!next_src(g, in, &i, &a) || !read_src(g, in, &i, &stok, &srel)) return;
        tex_sample(g, in, dtok, ptok, a.s, stok, NULL, NULL);
        return;
    }
    case OP_TEXLDD: {
        uint32_t stok, srel;
        if (!next_src(g, in, &i, &a) || !read_src(g, in, &i, &stok, &srel) || !next_src(g, in, &i, &b) ||
            !next_src(g, in, &i, &c))
            return;
        tex_sample(g, in, dtok, ptok, a.s, stok, b.s, c.s);
        return;
    }
    default: fail(g, "opcode %u at token %u is not supported", in->op, in->pos); return;
    }
}

/* Emit instructions from token position pos until ret or the end token. */
static void gen_block(gen *g, uint32_t pos)
{
    inst in;
    bool end;
    while (!g->failed && next_inst(g->sh->tokens, g->sh->ntokens, &pos, &in, &end, g->err, g->errn))
    {
        if (in.op == OP_RET || in.op == OP_LABEL) return;
        gen_inst(g, &in);
    }
    if (!end && !g->failed) fail(g, "shader token stream is malformed");
}

static bool gen_init(gen *g, const d3d_shader *sh, d3dsh_text *out, char *err, size_t errn)
{
    uint32_t pos = 1;
    inst in;
    bool end;
    memset(g, 0, sizeof *g);
    g->sh = sh;
    g->out = out;
    g->err = err;
    g->errn = errn;
    memset(out, 0, sizeof *out);
    while (next_inst(sh->tokens, sh->ntokens, &pos, &in, &end, err, errn))
    {
        if (in.op == OP_LABEL && in.n >= 1)
        {
            uint32_t l = rnum(in.p[0]);
            if (l >= MAX_LABELS)
            {
                snprintf(err, errn, "label %u is out of range", l);
                return false;
            }
            g->labels[l] = pos; /* body starts after the label instruction */
        }
    }
    emit(g, "#include <metal_stdlib>\nusing namespace metal;\n\n");
    emit(g, "static inline float4 d3d_lit(float4 s)\n{\n"
            "    float4 d = float4(1.0, 0.0, 0.0, 1.0);\n"
            "    if (s.x > 0.0) {\n"
            "        d.y = s.x;\n"
            "        if (s.y > 0.0) d.z = pow(s.y, clamp(s.w, -128.0, 128.0));\n"
            "    }\n"
            "    return d;\n}\n\n");
    /* D3DCMPFUNC: 1 never, 2 less, 3 equal, 4 less-equal, 5 greater,
       6 not-equal, 7 greater-equal, 8 always. */
    emit(g, "static inline bool d3d_alpha(int f, float a, float r)\n{\n"
            "    switch (f) {\n"
            "    case 1: return false;\n"
            "    case 2: return a < r;\n"
            "    case 3: return a == r;\n"
            "    case 4: return a <= r;\n"
            "    case 5: return a > r;\n"
            "    case 6: return a != r;\n"
            "    case 7: return a >= r;\n"
            "    default: return true;\n"
            "    }\n}\n\n");
    return !g->failed;
}

static void declare_temps(gen *g)
{
    int k;
    for (k = 0; k < 32; k++)
        emit(g, "float4 r%d = float4(0.0);\n", k);
    emit(g, "int4 a0 = int4(0);\n");
    emit(g, "int aL = 0;\n");
    emit(g, "bool4 p0 = bool4(false);\n");
}

static bool finish(gen *g, d3dsh_text *out)
{
    if (g->failed)
    {
        d3dsh_text_free(out);
        return false;
    }
    return true;
}

/* Name of the local variable that holds output entry e of a vertex shader. */
static void vs_out_name(gen *g, const d3d_shader *vs, const d3dsh_io *e, expr *x)
{
    if (vs->version >= 0x300)
        ex_set(g, x, "o%u", e->reg);
    else if (e->reg < 4u)
        ex_set(g, x, "orast%u", e->reg);
    else if (e->reg < 8u)
        ex_set(g, x, "oattr%u", e->reg - 4u);
    else
        ex_set(g, x, "otex%u", e->reg - 8u);
}

static bool is_special_vs_output(const d3dsh_io *e)
{
    return (e->usage == D3DDECLUSAGE_POSITION || e->usage == D3DDECLUSAGE_PSIZE) && e->index == 0;
}

static void texture_params(gen *g, const d3d_shader *sh, uint16_t depth_compare)
{
    uint32_t s;
    const char *tp = sh->is_ps ? "" : "v";
    for (s = 0; s < D3DSH_MAX_SAMPLERS; s++)
    {
        const char *type;
        switch (sh->sampler_type[s])
        {
        case 0: continue;
        case D3DSTT_2D: type = (depth_compare & (1u << s)) ? "depth2d<float>" : "texture2d<float>"; break;
        case D3DSTT_CUBE: type = "texturecube<float>"; break;
        default: type = "texture3d<float>"; break;
        }
        emit(g, ",\n    %s %st%u [[texture(%u)]], sampler %ss%u [[sampler(%u)]]", type, tp, s, s, tp, s, s);
    }
}

bool d3d_shader_msl_vs(const d3d_shader *vs, d3dsh_text *out, char *err, size_t errn)
{
    gen g;
    int k;
    const d3dsh_io *pos_out = NULL, *psize_out = NULL;
    expr x;
    if (vs->is_ps)
    {
        snprintf(err, errn, "not a vertex shader");
        return false;
    }
    if (!gen_init(&g, vs, out, err, errn)) return finish(&g, out);
    for (k = 0; k < D3DSH_MAX_SAMPLERS; k++)
        if (vs->sampler_type[k] != 0 && k >= 4) fail(&g, "vertex sampler s%d (only s0 to s3)", k);

    if (vs->ninputs > 0)
    {
        emit(&g, "struct VIn {\n");
        for (k = 0; k < vs->ninputs; k++)
            emit(&g, "    float4 a%u [[attribute(%u)]];\n", vs->inputs[k].reg, vs->inputs[k].reg);
        emit(&g, "};\n\n");
    }
    emit(&g, "struct VOut {\n    float4 pos [[position]];\n");
    for (k = 0; k < vs->noutputs; k++)
    {
        const d3dsh_io *e = &vs->outputs[k];
        if (e->usage == D3DDECLUSAGE_POSITION && e->index == 0)
            pos_out = e;
        else if (e->usage == D3DDECLUSAGE_PSIZE && e->index == 0)
        {
            psize_out = e;
            emit(&g, "    float psize [[point_size]];\n");
        }
        else
            emit(&g, "    float4 %s%u [[user(%s%u)]];\n", usage_name(e->usage), e->index, usage_name(e->usage),
                 e->index);
    }
    emit(&g, "};\n\n");
    emit(&g, "vertex VOut vs_main(");
    if (vs->ninputs > 0) emit(&g, "VIn vin [[stage_in]],\n    ");
    emit(&g, "constant float4 *c [[buffer(16)]], constant int4 *ci [[buffer(17)]], constant uint *cb [[buffer(18)]],\n"
             "    constant float4 &vs_misc [[buffer(19)]]");
    texture_params(&g, vs, 0);
    emit(&g, ")\n{\n");
    g.indent = 1;
    for (k = 0; k < vs->ninputs; k++)
        emit(&g, "float4 v%u = vin.a%u;\n", vs->inputs[k].reg, vs->inputs[k].reg);
    declare_temps(&g);
    if (vs->version >= 0x300)
    {
        for (k = 0; k < 12; k++)
            emit(&g, "float4 o%d = float4(0.0);\n", k);
    }
    else
    {
        for (k = 0; k < 3; k++)
            emit(&g, "float4 orast%d = float4(0.0);\n", k);
        for (k = 0; k < 2; k++)
            emit(&g, "float4 oattr%d = float4(0.0);\n", k);
        for (k = 0; k < 8; k++)
            emit(&g, "float4 otex%d = float4(0.0);\n", k);
    }
    gen_block(&g, 1);
    emit(&g, "VOut out;\n");
    if (pos_out == NULL)
        fail(&g, "vertex shader writes no position");
    else
    {
        vs_out_name(&g, vs, pos_out, &x);
        emit(&g, "out.pos = %s;\n", x.s);
        /* D3D9 pixel centers are at integer coordinates. */
        emit(&g, "out.pos.xy += vs_misc.xy * out.pos.w;\n");
        /* Constant depth bias (D3DRS_DEPTHBIAS): z/w changes by vs_misc.z. */
        emit(&g, "out.pos.z += vs_misc.z * out.pos.w;\n");
    }
    if (psize_out != NULL)
    {
        vs_out_name(&g, vs, psize_out, &x);
        emit(&g, "out.psize = %s.x;\n", x.s);
    }
    for (k = 0; k < vs->noutputs; k++)
    {
        const d3dsh_io *e = &vs->outputs[k];
        if (is_special_vs_output(e)) continue;
        vs_out_name(&g, vs, e, &x);
        /* Colors of shader model 2 are clamped to 0..1. */
        if (vs->version < 0x300 && e->usage == D3DDECLUSAGE_COLOR)
            emit(&g, "out.%s%u = saturate(%s);\n", usage_name(e->usage), e->index, x.s);
        else
            emit(&g, "out.%s%u = %s;\n", usage_name(e->usage), e->index, x.s);
    }
    emit(&g, "return out;\n");
    g.indent = 0;
    emit(&g, "}\n");
    return finish(&g, out);
}

/* Percentage-closer soft shadows (PCSS, the method of NVIDIA, Randima
   Fernando 2005) for a comparison sample of the shadow map:
     1. Blocker search: gathers of the depth (four texels in each fetch)
        in a disk of the largest penumbra radius. The mean depth of the
        texels that are nearer to the light than the receiver is the
        blocker depth.
     2. Penumbra estimate: the radius is proportional to the distance
        from the receiver to the blocker in the depth range of the map,
        between one texel and the largest radius.
     3. Filtering: comparison samples (with the hardware 2x2 filter of the
        game's sampler) in a disk of that radius.
   The game's shadow map is an atlas of four cascades in a 2x2 grid of the
   2048x2048 texture (from the four p3dShadowMapChildBox constants; the
   grid layout is UNCONFIRMED). The samples are clamped to the grid cell of
   the center sample, so a filter does not read a different cascade.
   PCSS_K (penumbra radius in texture coordinates for each unit of depth
   difference) and the largest radius are tuned values, not values from
   the game (UNCONFIRMED visual result). With no blocker the result is the
   single comparison sample of the game. */
static void emit_pcss(gen *g)
{
    static const char *const disk =
        "constant float2 d3d_pcss_disk[16] = {\n"
        "    float2(-0.9420, -0.3991), float2(0.9456, -0.7689), float2(-0.0942, -0.9294), float2(0.3450, 0.2939),\n"
        "    float2(-0.9159, 0.4577), float2(-0.8154, -0.8791), float2(-0.3828, 0.2768), float2(0.9748, 0.7565),\n"
        "    float2(0.4432, -0.9751), float2(0.5374, -0.4737), float2(-0.2650, -0.4189), float2(0.7920, 0.1909),\n"
        "    float2(-0.2419, 0.9971), float2(-0.8141, 0.9144), float2(0.1998, 0.7864), float2(0.1438, -0.1410)\n"
        "};\n";
    /* Level 1: 4 blocker gathers (16 texels) and 8 filter samples, radius
       up to 4 texels of a 2048 map. Level 2: 8 gathers (32 texels) and 16
       filter samples, up to 8 texels. A gather costs one fetch, so the
       search reads twice the texels of point samples with half the
       fetches. */
    uint32_t taps = g->pcss >= 2 ? 16u : 8u, gathers = taps / 2u;
    float maxr = (g->pcss >= 2 ? 8.0f : 4.0f) / 2048.0f;
    emit(g, "%s", disk);
    emit(g, "static inline float d3d_pcss(depth2d<float> t, sampler s, float2 uv, float z)\n{\n"
            "    constexpr sampler pt(filter::nearest, address::clamp_to_edge);\n"
            "    const float PCSS_K = 0.25, maxr = %.9g;\n"
            "    float2 ts = 1.0 / float2(t.get_width(), t.get_height());\n"
            "    float2 lo = floor(saturate(uv) * 2.0) * 0.5;\n"
            "    float2 hi = min(lo + 0.5, float2(1.0)) - ts * 0.5;\n"
            "    lo += ts * 0.5;\n"
            /* A gather reads the 2x2 texels around its point: one more
               half texel keeps them in the cell. */
            "    float2 glo = lo + ts * 0.5, ghi = hi - ts * 0.5;\n"
            "    float bsum = 0.0, bn = 0.0;\n"
            "    for (int i = 0; i < %u; i++) {\n"
            "        float4 d = t.gather(pt, clamp(uv + d3d_pcss_disk[i] * maxr, glo, ghi));\n"
            "        float4 b = select(float4(0.0), float4(1.0), d < z);\n"
            "        bsum += dot(d, b); bn += b.x + b.y + b.z + b.w;\n"
            "    }\n"
            "    if (bn == 0.0) return t.sample_compare(s, uv, z);\n"
            "    float pen = clamp((z - bsum / bn) * PCSS_K, ts.x, maxr);\n"
            "    float sum = 0.0;\n"
            "    for (int i = 0; i < %u; i++) sum += t.sample_compare(s, clamp(uv + d3d_pcss_disk[i] * pen, lo, hi), z);\n"
            "    return sum * %.9g;\n"
            "}\n\n",
         (double)maxr, gathers, taps, 1.0 / (double)taps);
}

bool d3d_shader_msl_ps(const d3d_shader *ps, const d3dsh_io *vs_outputs, int nvs, uint16_t depth_compare,
                       uint32_t pcss, bool contact, d3dsh_text *out, char *err, size_t errn)
{
    gen g;
    int k, nmembers = 0;
    bool need_pos;
    if (!ps->is_ps)
    {
        snprintf(err, errn, "not a pixel shader");
        return false;
    }
    if (!gen_init(&g, ps, out, err, errn)) return finish(&g, out);
    g.depth_compare = depth_compare;
    g.pcss = depth_compare != 0 ? pcss : 0u;
    if (g.pcss != 0) emit_pcss(&g);
    g.contact = depth_compare != 0 && contact;
    need_pos = ps->uses_vpos || ps->writes_depth || g.contact;

    for (k = 0; k < ps->ninputs; k++)
    {
        const d3dsh_io *e = &ps->inputs[k];
        if (find_io(vs_outputs, nvs, e->usage, e->index) && !is_special_vs_output(e))
        {
            if (nmembers++ == 0) emit(&g, "struct PIn {\n");
            emit(&g, "    float4 %s%u [[user(%s%u)%s]];\n", usage_name(e->usage), e->index, usage_name(e->usage),
                 e->index, e->centroid ? ", centroid_perspective" : "");
        }
    }
    if (nmembers > 0) emit(&g, "};\n\n");
    emit(&g, "struct POut {\n");
    for (k = 0; k < 4; k++)
        if (ps->color_outputs & (1u << k)) emit(&g, "    float4 oC%d [[color(%d)]];\n", k, k);
    if (ps->writes_depth) emit(&g, "    float oDepth [[depth(any)]];\n");
    if (ps->color_outputs == 0 && !ps->writes_depth) fail(&g, "pixel shader writes no output");
    emit(&g, "};\n\n");

    emit(&g, "fragment POut ps_main(");
    if (nmembers > 0) emit(&g, "PIn pin [[stage_in]], ");
    if (need_pos) emit(&g, "float4 fpos [[position]], ");
    if (ps->uses_vface) emit(&g, "bool ffront [[front_facing]], ");
    emit(&g, "\n    constant float4 *c [[buffer(16)]], constant int4 *ci [[buffer(17)]], constant uint *cb [[buffer(18)]],\n"
             "    constant float4 &ps_misc [[buffer(19)]]");
    texture_params(&g, ps, depth_compare);
    if (g.contact)
        emit(&g, ",\n    texture2d<float, access::read> d3d_cs [[texture(%u)]]", D3DSH_CONTACT_TEXTURE);
    emit(&g, ")\n{\n");
    g.indent = 1;
    /* The contact shadow of this pixel (1 = lit). */
    if (g.contact) emit(&g, "float d3d_csv = d3d_cs.read(uint2(fpos.xy)).r;\n");
    if (ps->uses_vpos) emit(&g, "float4 vpos = float4(fpos.xy - 0.5, fpos.z, fpos.w);\n");
    if (ps->uses_vface) emit(&g, "float vface = ffront ? 1.0 : -1.0;\n");
    for (k = 0; k < ps->ninputs; k++)
    {
        const d3dsh_io *e = &ps->inputs[k];
        if (find_io(vs_outputs, nvs, e->usage, e->index) && !is_special_vs_output(e))
            emit(&g, "float4 v%u = pin.%s%u;\n", e->reg, usage_name(e->usage), e->index);
        else
            /* The vertex shader does not write this input; D3D9 leaves it
               undefined. */
            emit(&g, "float4 v%u = float4(0.0);\n", e->reg);
    }
    declare_temps(&g);
    for (k = 0; k < 4; k++)
        emit(&g, "float4 oC%d = float4(0.0);\n", k);
    if (ps->writes_depth) emit(&g, "float oDepth = fpos.z;\n");
    gen_block(&g, 1);
    if (ps->color_outputs & 1u)
        emit(&g, "if (ps_misc.x != 0.0 && !d3d_alpha(int(ps_misc.x), oC0.a, ps_misc.y)) discard_fragment();\n");
    emit(&g, "POut out;\n");
    for (k = 0; k < 4; k++)
        if (ps->color_outputs & (1u << k)) emit(&g, "out.oC%d = oC%d;\n", k, k);
    if (ps->writes_depth) emit(&g, "out.oDepth = oDepth;\n");
    emit(&g, "return out;\n");
    g.indent = 0;
    emit(&g, "}\n");
    return finish(&g, out);
}

