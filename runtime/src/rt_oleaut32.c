/*
 * OLEAUT32: BSTR strings, VARIANT values and error information objects.
 *
 * BSTR: a guest heap block [byte length][UTF-16 characters][NUL]. The
 * BSTR pointer is the block address + 4.
 *
 * VariantChangeType supports these types as source and target: VT_EMPTY,
 * VT_NULL, VT_I1, VT_I2, VT_I4, VT_I8, VT_UI1, VT_UI2, VT_UI4, VT_UI8,
 * VT_INT, VT_UINT, VT_R4, VT_R8, VT_BOOL and VT_BSTR, also with VT_BYREF on
 * the source. The rules are those of the OLE Automation conversion
 * functions (VarXxxFromYyy): round to nearest with ties to even,
 * DISP_E_OVERFLOW out of range, DISP_E_TYPEMISMATCH for text that is not a
 * number. Text is parsed and formatted with the English (United States)
 * rules of the emulated system. Other types (VT_DATE, VT_CY, VT_DECIMAL,
 * VT_DISPATCH, VT_UNKNOWN, VT_ERROR, arrays) stop the process with a
 * message.
 */
#include "rt.h"
#include "rt_com.h"
#include "rt_com_thunks.h"
#include "rt_thread.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

/* ---------------------------------------------------------------- BSTR */

#define BSTR_MAX_CHARS 0x3FFFFFF0u

uint32_t rt_bstr_alloc_utf16(const uint16_t *s, uint32_t nchars)
{
    uint32_t blk, bytes;
    if (nchars > BSTR_MAX_CHARS) return 0;
    bytes = 2u * nchars;
    blk = rt_heap_alloc(bytes + 6u);
    if (blk == 0) return 0;
    wr32(blk, bytes);
    if (s != NULL)
        memcpy(G2H(blk + 4), s, bytes);
    else
        memset(G2H(blk + 4), 0, bytes);
    wr16(blk + 4 + bytes, 0);
    return blk + 4;
}

uint32_t rt_bstr_alloc_ascii(const char *s)
{
    size_t n = strlen(s), i;
    uint32_t b;
    if (n > BSTR_MAX_CHARS) return 0;
    b = rt_bstr_alloc_utf16(NULL, (uint32_t)n);
    if (b == 0) return 0;
    for (i = 0; i < n; i++) wr16(b + 2u * (uint32_t)i, (uint8_t)s[i]);
    return b;
}

void rt_bstr_free(uint32_t b)
{
    if (b == 0) return;
    if (b < 4) rt_fatal("SysFreeString of invalid BSTR 0x%08x", b);
    rt_heap_free(b - 4);
}

uint32_t rt_bstr_len(uint32_t b)
{
    return b != 0 ? rd32(b - 4) / 2u : 0;
}

/* Number of characters of a NUL-terminated guest UTF-16 string. */
static uint32_t wlen(uint32_t s)
{
    uint32_t n = 0;
    while (rd16(s + 2u * n) != 0)
    {
        if (n == BSTR_MAX_CHARS) rt_fatal("UTF-16 string at 0x%08x is not terminated", s);
        n++;
    }
    return n;
}

/* BSTR SysAllocString(const OLECHAR *) */
void rt_SysAllocString(_cpu *cpu)
{
    uint32_t s = ARG(0);
    if (s == 0)
    {
        RET(0);
        return;
    }
    RET(rt_bstr_alloc_utf16(G2H(s), wlen(s)));
}

/* BSTR SysAllocStringLen(const OLECHAR *, UINT len): a NULL source gives
   len characters of zero. */
void rt_SysAllocStringLen(_cpu *cpu)
{
    uint32_t s = ARG(0), n = ARG(1);
    RET(rt_bstr_alloc_utf16(s != 0 ? G2H(s) : NULL, n));
}

/* void SysFreeString(BSTR) */
void rt_SysFreeString(_cpu *cpu)
{
    rt_bstr_free(ARG(0));
}

/* ---------------------------------------------------------------- VARIANT */

/* void VariantInit(VARIANTARG *) */
void rt_VariantInit(_cpu *cpu)
{
    if (ARG(0) != 0) wr16(ARG(0), VT_EMPTY);
}

static bool scalar_type(uint32_t vt)
{
    switch (vt)
    {
    case VT_EMPTY: case VT_NULL: case VT_I1: case VT_I2: case VT_I4: case VT_I8:
    case VT_UI1: case VT_UI2: case VT_UI4: case VT_UI8: case VT_INT: case VT_UINT:
    case VT_R4: case VT_R8: case VT_BOOL: case VT_CY: case VT_DATE: case VT_ERROR:
    case VT_DECIMAL:
        return true;
    default:
        return false;
    }
}

static uint32_t variant_clear(_cpu *cpu, uint32_t v)
{
    uint32_t vt = rd16(v);
    if ((vt & VT_BYREF) == 0)
    {
        if ((vt & VT_ARRAY) != 0) rt_fatal("VariantClear of a SAFEARRAY (vt 0x%04x) is not supported", vt);
        if (vt == VT_BSTR)
        {
            rt_bstr_free(rd32(v + 8));
        }
        else if (vt == VT_DISPATCH || vt == VT_UNKNOWN)
        {
            uint32_t unk = rd32(v + 8);
            if (unk != 0) rt_com_guest_release(cpu, unk);
        }
        else if (!scalar_type(vt))
        {
            if (vt == 36u /* VT_RECORD */ || vt == VT_VARIANT) rt_fatal("VariantClear of vt 0x%04x is not supported", vt);
            return DISP_E_BADVARTYPE;
        }
    }
    wr16(v, VT_EMPTY);
    return S_OK;
}

/* HRESULT VariantClear(VARIANTARG *) */
void rt_VariantClear(_cpu *cpu)
{
    if (ARG(0) == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    RET(variant_clear(cpu, ARG(0)));
}

/* Intermediate value of a conversion. */
typedef enum { V_EMPTY, V_NULL, V_INT, V_DBL, V_BOOL, V_STR } vkind;
typedef struct {
    vkind kind;
    __int128 i;      /* V_INT */
    double d;        /* V_DBL */
    bool b;          /* V_BOOL */
    uint16_t *s;     /* V_STR (host copy, malloc) */
    uint32_t slen;
} value;

static void value_free(value *x)
{
    free(x->s);
    x->s = NULL;
}

/* Read the source value. addr is the address of the data (the variant +8,
   or the target of a VT_BYREF pointer). */
static void read_value(uint32_t vt, uint32_t addr, value *x)
{
    memset(x, 0, sizeof *x);
    switch (vt)
    {
    case VT_EMPTY: x->kind = V_EMPTY; return;
    case VT_NULL: x->kind = V_NULL; return;
    case VT_I1: x->kind = V_INT; x->i = (int8_t)rd8(addr); return;
    case VT_UI1: x->kind = V_INT; x->i = rd8(addr); return;
    case VT_I2: x->kind = V_INT; x->i = (int16_t)rd16(addr); return;
    case VT_UI2: x->kind = V_INT; x->i = rd16(addr); return;
    case VT_I4: case VT_INT: x->kind = V_INT; x->i = (int32_t)rd32(addr); return;
    case VT_UI4: case VT_UINT: x->kind = V_INT; x->i = rd32(addr); return;
    case VT_I8: x->kind = V_INT; x->i = (int64_t)rd64(addr); return;
    case VT_UI8: x->kind = V_INT; x->i = rd64(addr); return;
    case VT_R4:
    {
        float f;
        uint32_t bits = rd32(addr);
        memcpy(&f, &bits, 4);
        x->kind = V_DBL;
        x->d = f;
        return;
    }
    case VT_R8: x->kind = V_DBL; x->d = rdf64(addr); return;
    case VT_BOOL: x->kind = V_BOOL; x->b = rd16(addr) != 0; return;
    case VT_BSTR:
    {
        uint32_t b = rd32(addr), n = rt_bstr_len(b);
        x->kind = V_STR;
        x->slen = n;
        x->s = malloc(2u * (size_t)n + 2u);
        if (x->s == NULL) rt_fatal("out of host memory");
        if (n) memcpy(x->s, G2H(b), 2u * (size_t)n);
        x->s[n] = 0;
        return;
    }
    default:
        rt_fatal("VariantChangeType from vt 0x%04x is not supported", vt);
    }
}

/* Integer range of a target type. */
static bool int_range(uint32_t vt, __int128 *lo, __int128 *hi)
{
    switch (vt)
    {
    case VT_I1: *lo = INT8_MIN; *hi = INT8_MAX; return true;
    case VT_UI1: *lo = 0; *hi = UINT8_MAX; return true;
    case VT_I2: *lo = INT16_MIN; *hi = INT16_MAX; return true;
    case VT_UI2: *lo = 0; *hi = UINT16_MAX; return true;
    case VT_I4: case VT_INT: *lo = INT32_MIN; *hi = INT32_MAX; return true;
    case VT_UI4: case VT_UINT: *lo = 0; *hi = UINT32_MAX; return true;
    case VT_I8: *lo = INT64_MIN; *hi = INT64_MAX; return true;
    case VT_UI8: *lo = 0; *hi = UINT64_MAX; return true;
    default: return false;
    }
}

/* Parse English number text: spaces, sign, digits, '.', digits, exponent,
   spaces. Thousands separators are allowed in the integer part. Returns
   false if the text is not a number. */
static bool parse_number(const value *x, value *out)
{
    char buf[128];
    size_t n = 0, i = 0;
    bool digits = false, frac = false;
    while (i < x->slen && (x->s[i] == ' ' || x->s[i] == '\t')) i++;
    if (i < x->slen && (x->s[i] == '+' || x->s[i] == '-')) buf[n++] = (char)x->s[i++];
    while (i < x->slen && ((x->s[i] >= '0' && x->s[i] <= '9') || (x->s[i] == ',' && digits)))
    {
        if (x->s[i] != ',')
        {
            if (n + 1 >= sizeof buf) return false;
            buf[n++] = (char)x->s[i];
            digits = true;
        }
        i++;
    }
    if (i < x->slen && x->s[i] == '.')
    {
        frac = true;
        buf[n++] = '.';
        i++;
        while (i < x->slen && x->s[i] >= '0' && x->s[i] <= '9')
        {
            if (n + 1 >= sizeof buf) return false;
            buf[n++] = (char)x->s[i++];
            digits = true;
        }
    }
    if (!digits) return false;
    if (i < x->slen && (x->s[i] == 'e' || x->s[i] == 'E'))
    {
        bool edigits = false;
        frac = true;
        buf[n++] = 'e';
        i++;
        if (i < x->slen && (x->s[i] == '+' || x->s[i] == '-')) buf[n++] = (char)x->s[i++];
        while (i < x->slen && x->s[i] >= '0' && x->s[i] <= '9')
        {
            if (n + 1 >= sizeof buf) return false;
            buf[n++] = (char)x->s[i++];
            edigits = true;
        }
        if (!edigits) return false;
    }
    while (i < x->slen && (x->s[i] == ' ' || x->s[i] == '\t')) i++;
    if (i != x->slen) return false;
    buf[n] = 0;
    memset(out, 0, sizeof *out);
    if (!frac)
    {
        /* Exact integer (up to the UI8 and I8 ranges). */
        __int128 v = 0;
        bool neg = buf[0] == '-';
        const char *p = buf + (buf[0] == '-' || buf[0] == '+');
        for (; *p; p++)
        {
            v = v * 10 + (*p - '0');
            if (v > ((__int128)1 << 80)) break;
        }
        out->kind = V_INT;
        out->i = neg ? -v : v;
        return true;
    }
    out->kind = V_DBL;
    out->d = strtod(buf, NULL);
    return true;
}

static uint32_t to_int(const value *x, uint32_t vt, __int128 *out)
{
    __int128 lo, hi, v;
    value num;
    (void)int_range(vt, &lo, &hi);
    switch (x->kind)
    {
    case V_EMPTY: v = 0; break;
    case V_NULL: return DISP_E_TYPEMISMATCH;
    case V_BOOL:
        /* VARIANT_TRUE (-1) keeps its bit pattern in the unsigned types. */
        *out = x->b ? (lo < 0 ? -1 : hi) : 0;
        return S_OK;
    case V_INT: v = x->i; break;
    case V_DBL:
    {
        double r = rint(x->d); /* the default rounding mode: nearest, ties to even */
        /* NaN and values far outside the range; the exact check follows. */
        if (isnan(r) || r < (double)lo - 1.0 || r > (double)hi + 1.0) return DISP_E_OVERFLOW;
        v = (__int128)r;
        break;
    }
    case V_STR:
        if (!parse_number(x, &num)) return DISP_E_TYPEMISMATCH;
        return to_int(&num, vt, out);
    default:
        return DISP_E_TYPEMISMATCH;
    }
    if (v < lo || v > hi) return DISP_E_OVERFLOW;
    *out = v;
    return S_OK;
}

static uint32_t to_double(const value *x, double *out)
{
    value num;
    switch (x->kind)
    {
    case V_EMPTY: *out = 0; return S_OK;
    case V_NULL: return DISP_E_TYPEMISMATCH;
    case V_BOOL: *out = x->b ? -1.0 : 0.0; return S_OK;
    case V_INT: *out = (double)x->i; return S_OK;
    case V_DBL: *out = x->d; return S_OK;
    case V_STR:
        if (!parse_number(x, &num)) return DISP_E_TYPEMISMATCH;
        return to_double(&num, out);
    }
    return DISP_E_TYPEMISMATCH;
}

static uint32_t to_bool(const value *x, bool *out)
{
    static const char *const names[2] = { "false", "true" };
    value num;
    double d;
    int k;
    switch (x->kind)
    {
    case V_EMPTY: *out = false; return S_OK;
    case V_NULL: return DISP_E_TYPEMISMATCH;
    case V_BOOL: *out = x->b; return S_OK;
    case V_INT: *out = x->i != 0; return S_OK;
    case V_DBL: *out = x->d != 0.0; return S_OK;
    case V_STR:
        for (k = 0; k < 2; k++)
        {
            size_t n = strlen(names[k]), i;
            bool eq = x->slen == n;
            for (i = 0; eq && i < n; i++)
            {
                uint16_t c = x->s[i];
                if (c >= 'A' && c <= 'Z') c = (uint16_t)(c + 32);
                eq = c == (uint8_t)names[k][i];
            }
            if (eq)
            {
                *out = k == 1;
                return S_OK;
            }
        }
        if (!parse_number(x, &num)) return DISP_E_TYPEMISMATCH;
        if (to_double(&num, &d) != S_OK) return DISP_E_TYPEMISMATCH;
        *out = d != 0.0;
        return S_OK;
    }
    return DISP_E_TYPEMISMATCH;
}

#define VARIANT_ALPHABOOL 0x2u

/* Format a value as text. */
static uint32_t to_text(const value *x, uint32_t flags, uint32_t src_vt, char *buf, size_t n)
{
    switch (x->kind)
    {
    case V_EMPTY: buf[0] = 0; return S_OK;
    case V_NULL: return DISP_E_TYPEMISMATCH;
    case V_BOOL:
        if (flags & VARIANT_ALPHABOOL)
            snprintf(buf, n, "%s", x->b ? "True" : "False");
        else
            snprintf(buf, n, "%d", x->b ? -1 : 0);
        return S_OK;
    case V_INT:
        if (x->i < 0)
            snprintf(buf, n, "%lld", (long long)x->i);
        else
            snprintf(buf, n, "%llu", (unsigned long long)x->i);
        return S_OK;
    case V_DBL:
        /* VarBstrFromR4 uses 7 and VarBstrFromR8 15 significant digits. */
        snprintf(buf, n, src_vt == VT_R4 ? "%.7G" : "%.15G", x->d);
        return S_OK;
    case V_STR:
        return S_OK; /* not used: text is copied directly */
    }
    return DISP_E_TYPEMISMATCH;
}

static bool supported_target(uint32_t vt)
{
    switch (vt)
    {
    case VT_EMPTY: case VT_NULL: case VT_I1: case VT_I2: case VT_I4: case VT_I8:
    case VT_UI1: case VT_UI2: case VT_UI4: case VT_UI8: case VT_INT: case VT_UINT:
    case VT_R4: case VT_R8: case VT_BOOL: case VT_BSTR:
        return true;
    default:
        return false;
    }
}

/* Convert x to type vt into the cleared variant dst. */
static uint32_t coerce(uint32_t dst, const value *x, uint32_t src_vt, uint32_t flags, uint32_t vt)
{
    __int128 iv;
    double d;
    bool b;
    uint32_t r;
    switch (vt)
    {
    case VT_EMPTY:
        return S_OK;
    case VT_NULL:
        if (x->kind != V_NULL) return DISP_E_TYPEMISMATCH;
        wr16(dst, VT_NULL);
        return S_OK;
    case VT_R4:
    {
        float f;
        uint32_t bits;
        if ((r = to_double(x, &d)) != S_OK) return r;
        if (isfinite(d) && fabs(d) > FLT_MAX) return DISP_E_OVERFLOW;
        f = (float)d;
        memcpy(&bits, &f, 4);
        wr32(dst + 8, bits);
        break;
    }
    case VT_R8:
        if ((r = to_double(x, &d)) != S_OK) return r;
        wr64(dst + 8, 0);
        memcpy(G2H(dst + 8), &d, 8);
        break;
    case VT_BOOL:
        if ((r = to_bool(x, &b)) != S_OK) return r;
        wr16(dst + 8, b ? 0xFFFFu : 0u);
        break;
    case VT_BSTR:
    {
        uint32_t bs;
        if (x->kind == V_STR)
        {
            bs = rt_bstr_alloc_utf16(x->s, x->slen);
        }
        else
        {
            char text[64];
            if ((r = to_text(x, flags, src_vt, text, sizeof text)) != S_OK) return r;
            bs = rt_bstr_alloc_ascii(text);
        }
        if (bs == 0) return E_OUTOFMEMORY;
        wr32(dst + 8, bs);
        break;
    }
    default:
        if ((r = to_int(x, vt, &iv)) != S_OK) return r;
        switch (vt)
        {
        case VT_I1: case VT_UI1: wr8(dst + 8, (uint8_t)iv); break;
        case VT_I2: case VT_UI2: wr16(dst + 8, (uint16_t)iv); break;
        case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: wr32(dst + 8, (uint32_t)iv); break;
        default: wr64(dst + 8, (uint64_t)iv); break;
        }
        break;
    }
    wr16(dst, (uint16_t)vt);
    return S_OK;
}

/* HRESULT VariantChangeType(VARIANTARG *dst, const VARIANTARG *src,
   USHORT flags, VARTYPE vt) */
void rt_VariantChangeType(_cpu *cpu)
{
    uint32_t dst = ARG(0), src = ARG(1), flags = ARG(2) & 0xFFFFu, vt = ARG(3) & 0xFFFFu;
    uint32_t svt, addr, r;
    value x;
    if (dst == 0 || src == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    if (!supported_target(vt)) rt_fatal("VariantChangeType to vt 0x%04x is not supported", vt);
    svt = rd16(src);
    addr = src + 8;
    if (svt & VT_BYREF)
    {
        svt &= ~VT_BYREF;
        addr = rd32(src + 8);
        if (addr == 0) rt_fatal("VariantChangeType of a VT_BYREF variant with a NULL pointer");
    }
    read_value(svt, addr, &x); /* a copy: dst can be src */
    r = variant_clear(cpu, dst);
    if (r == S_OK) r = coerce(dst, &x, svt, flags, vt);
    value_free(&x);
    RET(r);
}

/* ---------------------------------------------------------------- error information */

static const rt_guid IID_ICreateErrorInfo = { 0x22F03340, 0x547D, 0x101B, { 0x8E, 0x65, 0x08, 0x00, 0x2B, 0x2B, 0xD1, 0x19 } };
static const rt_guid IID_IErrorInfo = { 0x1CF2B120, 0x547D, 0x101B, { 0x8E, 0x65, 0x08, 0x00, 0x2B, 0x2B, 0xD1, 0x19 } };

typedef struct errinfo {
    rt_guid guid;
    uint16_t *text[3]; /* source, description, help file (host copies) */
    uint32_t len[3];
    uint32_t help_context;
} errinfo;

static void errinfo_destroy(rt_com_object *o)
{
    errinfo *e = o->data;
    int i;
    for (i = 0; i < 3; i++) free(e->text[i]);
    free(e);
}

static const rt_com_iface errinfo_ifaces[] = {
    { &IID_ICreateErrorInfo, 0 },
    { &IID_IErrorInfo, 1 },
};

static const rt_com_class errinfo_class = {
    "ErrorInfo", 2, { &rt_vt_CreateErrorInfo, &rt_vt_ErrorInfo, NULL, NULL },
    errinfo_ifaces, 2, errinfo_destroy
};

static errinfo *ei(uint32_t this_ptr, int slot)
{
    return rt_com_this(this_ptr, slot, &errinfo_class)->data;
}

static void set_text(errinfo *e, int k, uint32_t s)
{
    uint32_t n = s != 0 ? wlen(s) : 0;
    uint16_t *copy = malloc(2u * (size_t)n + 2u);
    if (copy == NULL) rt_fatal("out of host memory");
    if (n) memcpy(copy, G2H(s), 2u * (size_t)n);
    copy[n] = 0;
    free(e->text[k]);
    e->text[k] = copy;
    e->len[k] = n;
}

void rt_com_CreateErrorInfo_SetGUID(_cpu *cpu)
{
    errinfo *e = ei(ARG(0), 0);
    if (ARG(1) == 0) rt_fatal("ICreateErrorInfo::SetGUID with a NULL GUID");
    e->guid = rt_guid_read(ARG(1));
    RET(S_OK);
}

void rt_com_CreateErrorInfo_SetSource(_cpu *cpu) { set_text(ei(ARG(0), 0), 0, ARG(1)); RET(S_OK); }
void rt_com_CreateErrorInfo_SetDescription(_cpu *cpu) { set_text(ei(ARG(0), 0), 1, ARG(1)); RET(S_OK); }
void rt_com_CreateErrorInfo_SetHelpFile(_cpu *cpu) { set_text(ei(ARG(0), 0), 2, ARG(1)); RET(S_OK); }
void rt_com_CreateErrorInfo_SetHelpContext(_cpu *cpu) { ei(ARG(0), 0)->help_context = ARG(1); RET(S_OK); }

void rt_com_ErrorInfo_GetGUID(_cpu *cpu)
{
    errinfo *e = ei(ARG(0), 1);
    if (ARG(1) == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    rt_guid_write(ARG(1), &e->guid);
    RET(S_OK);
}

static void get_text(_cpu *cpu, int k)
{
    errinfo *e = ei(ARG(0), 1);
    uint32_t b = 0;
    if (ARG(1) == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    if (e->text[k] != NULL)
    {
        b = rt_bstr_alloc_utf16(e->text[k], e->len[k]);
        if (b == 0)
        {
            RET(E_OUTOFMEMORY);
            return;
        }
    }
    wr32(ARG(1), b);
    RET(S_OK);
}

void rt_com_ErrorInfo_GetSource(_cpu *cpu) { get_text(cpu, 0); }
void rt_com_ErrorInfo_GetDescription(_cpu *cpu) { get_text(cpu, 1); }
void rt_com_ErrorInfo_GetHelpFile(_cpu *cpu) { get_text(cpu, 2); }

void rt_com_ErrorInfo_GetHelpContext(_cpu *cpu)
{
    errinfo *e = ei(ARG(0), 1);
    if (ARG(1) == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    wr32(ARG(1), e->help_context);
    RET(S_OK);
}

/* HRESULT CreateErrorInfo(ICreateErrorInfo **) */
void rt_CreateErrorInfo(_cpu *cpu)
{
    errinfo *e;
    rt_com_object *o;
    if (ARG(0) == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    e = calloc(1, sizeof *e);
    if (e == NULL) rt_fatal("out of host memory");
    o = rt_com_create(&errinfo_class, e);
    if (o == NULL)
    {
        free(e);
        wr32(ARG(0), 0);
        RET(E_OUTOFMEMORY);
        return;
    }
    wr32(ARG(0), rt_com_ptr(o, 0));
    RET(S_OK);
}

/* The error information object of the thread (an IErrorInfo pointer with
   one reference, or 0). */
static _Thread_local uint32_t thread_error_info;

/* HRESULT SetErrorInfo(ULONG reserved, IErrorInfo *) */
void rt_SetErrorInfo(_cpu *cpu)
{
    uint32_t p = ARG(1), old;
    if (ARG(0) != 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    if (p != 0)
    {
        uint32_t vt = rd32(p);
        rt_call(cpu, rd32(vt + 4), true, 0, 1, (const uint32_t[]){ p }); /* AddRef */
    }
    old = thread_error_info;
    thread_error_info = p;
    if (old != 0) rt_com_guest_release(cpu, old);
    RET(S_OK);
}

/* HRESULT GetErrorInfo(ULONG reserved, IErrorInfo **): moves the reference
   to the caller. S_FALSE if there is no error information. */
void rt_GetErrorInfo(_cpu *cpu)
{
    uint32_t pp = ARG(1);
    if (ARG(0) != 0 || pp == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    wr32(pp, thread_error_info);
    RET(thread_error_info != 0 ? S_OK : S_FALSE);
    thread_error_info = 0;
}

/* Called by CoUninitialize when the thread leaves COM: COM releases the
   error information object of the thread. */
void rt_oleaut_thread_uninit(_cpu *cpu)
{
    uint32_t old = thread_error_info;
    thread_error_info = 0;
    if (old != 0) rt_com_guest_release(cpu, old);
}
