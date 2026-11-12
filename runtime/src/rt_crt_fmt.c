/*
 * Formatted output and input of the Microsoft C runtime 8.0
 * (printf and scanf families) and USER32 wsprintfA/wvsprintfA.
 *
 * Output rules of the Microsoft runtime that differ from the C standard
 * library of macOS and that this engine follows:
 *   - floating point numbers have at most 17 significant digits; more
 *     digits are zeros; rounding is half up on the exact value;
 *   - %e and %g exponents have at least 3 digits;
 *   - infinity and NaN are written as 1.#INF, 1.#QNAN, 1.#SNAN and 1.#IND
 *     with the digits after the point rounded like other digits;
 *   - %p writes 8 upper-case hexadecimal digits;
 *   - %n is an invalid parameter unless _set_printf_count_output enabled it;
 *   - an unknown conversion is an invalid parameter.
 */
#include "rt_crt.h"
#include "rt_crt_file.h"
#include "rt_thread.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define EILSEQ_ 42u
#define EINVAL_ 22u
#define TRUNCATE_ 0xFFFFFFFFu

static bool count_output_enabled;

void rt__set_printf_count_output(_cpu *cpu)
{
    bool prev = count_output_enabled;
    count_output_enabled = ARG(0) != 0;
    RET(prev ? 1u : 0u);
}

/* ---------------------------------------------------------------- output sink */

typedef struct sink {
    /* Memory output: buf (guest address), limit (maximum characters, not
       counting the terminator; UINT32_MAX for no limit). */
    uint32_t buf;
    uint32_t limit;
    /* File output (when file is not 0). */
    uint32_t file;
    uint32_t count;      /* characters produced (also past the limit) */
    bool overflow;       /* a character was dropped at the limit */
    bool error;          /* conversion error (EILSEQ) or file error */
} sink;

static void out_char(sink *s, char c)
{
    if (s->file != 0)
    {
        if (!s->error && rt_crt_file_putc(s->file, (uint8_t)c) < 0) s->error = true;
    }
    else if (s->count < s->limit)
    {
        wr8(s->buf + s->count, (uint8_t)c);
    }
    else
    {
        s->overflow = true;
    }
    s->count++;
}

static void out_n(sink *s, const char *p, size_t n)
{
    while (n--) out_char(s, *p++);
}

static void out_pad(sink *s, char c, int n)
{
    while (n-- > 0) out_char(s, c);
}

/* ---------------------------------------------------------------- arguments */

typedef struct args {
    uint32_t ap; /* guest va_list position */
} args;

static uint32_t arg32(args *a)
{
    uint32_t v = rd32(a->ap);
    a->ap += 4;
    return v;
}

static uint64_t arg64(args *a)
{
    uint64_t v = rd64(a->ap);
    a->ap += 8;
    return v;
}

static double argf(args *a)
{
    double v = rdf64(a->ap);
    a->ap += 8;
    return v;
}

/* ---------------------------------------------------------------- floating point digits */

/* Decimal digits of |x| (finite, nonzero): up to 40 significant digits
   (correctly rounded by the host) and the decimal exponent of the first
   digit. */
static void exact_digits(double x, char *d, int *exp10)
{
    char buf[64];
    char *e;
    int i, n = 0;
    snprintf(buf, sizeof(buf), "%.39e", fabs(x));
    for (i = 0; buf[i] && buf[i] != 'e'; i++)
        if (buf[i] >= '0' && buf[i] <= '9') d[n++] = buf[i];
    d[n] = 0;
    e = strchr(buf, 'e');
    *exp10 = (int)strtol(e + 1, NULL, 10);
}

/* Round the digit string to keep digits (0 <= keep), half up, with at most
   17 significant digits; the digits after the kept ones are zeros. d has
   room for 41 digits. Returns the new exponent (a carry can add a digit). */
static int round_digits(char *d, int keep, int exp10)
{
    int limit = keep < 17 ? keep : 17, i;
    size_t len = strlen(d);
    if (limit < 0)
    {
        /* Nothing kept: the value rounds to 0 or to 1 unit at the position. */
        memset(d, '0', len);
        return exp10;
    }
    if ((size_t)limit < len)
    {
        bool up = d[limit] >= '5';
        for (i = limit; (size_t)i < len; i++) d[i] = '0';
        if (up)
        {
            for (i = limit - 1; i >= 0; i--)
            {
                if (d[i] == '9')
                {
                    d[i] = '0';
                }
                else
                {
                    d[i]++;
                    break;
                }
            }
            if (i < 0)
            {
                memmove(d + 1, d, len);
                d[0] = '1';
                d[len] = 0;
                exp10++;
            }
        }
    }
    return exp10;
}

static char digit_at(const char *d, int i)
{
    if (i < 0 || (size_t)i >= strlen(d)) return '0';
    return d[i];
}

/* The special values: text "1#INF" and so on, written with the digits
   after the point rounded at the precision like the Microsoft runtime. */
static const char *special_text(double x)
{
    uint64_t bits;
    memcpy(&bits, &x, 8);
    if (isinf(x)) return "1#INF";
    if (bits == UINT64_C(0xFFF8000000000000)) return "1#IND";
    if ((bits & UINT64_C(0x0008000000000000)) == 0) return "1#SNAN";
    return "1#QNAN";
}

/* Make the digit string for a special value with prec digits after the
   point: "1" then "#INF..." padded with '0' and rounded with the character
   rule (a next character >= '5' adds one to the previous character). */
static void special_digits(const char *text, int prec, char *out, size_t n)
{
    const char *frac = text + 1;
    size_t fl = strlen(frac);
    int i;
    char lead = '1';
    if ((size_t)prec + 3 > n) prec = (int)n - 3;
    for (i = 0; i < prec; i++) out[i] = (size_t)i < fl ? frac[i] : '0';
    out[prec] = 0;
    {
        char next = (size_t)prec < fl ? frac[prec] : '0';
        if (next >= '5')
        {
            if (prec > 0) out[prec - 1]++;
            else lead++;
        }
    }
    memmove(out + 1, out, (size_t)prec + 1);
    out[0] = lead;
}

typedef struct spec {
    bool minus, plus, space, zero, alt;
    int width;
    int prec; /* -1: none */
    char size; /* 'h', 'l', 'L', 'q' (64-bit), 'I' (pointer size), 'w' */
    char type;
} spec;

/* Write sign, prefix and body with the width rules. body_zero_pad: '0'
   flag applies (numbers). */
static void emit(sink *s, const spec *sp, const char *sign, const char *prefix, const char *body, size_t blen, bool numeric)
{
    size_t sl = strlen(sign), pl = strlen(prefix);
    int pad = sp->width - (int)(sl + pl + blen);
    if (!sp->minus && !(numeric && sp->zero)) out_pad(s, ' ', pad);
    out_n(s, sign, sl);
    out_n(s, prefix, pl);
    if (!sp->minus && numeric && sp->zero) out_pad(s, '0', pad);
    out_n(s, body, blen);
    if (sp->minus) out_pad(s, ' ', pad);
}

static void fmt_float(sink *s, const spec *sp, double x)
{
    char body[512];
    size_t bl = 0;
    const char *sign = "";
    int prec = sp->prec < 0 ? 6 : sp->prec;
    char t = sp->type;
    bool neg = signbit(x) != 0;

    if (neg) sign = "-";
    else if (sp->plus) sign = "+";
    else if (sp->space) sign = " ";

    if (prec > 400) prec = 400;

    if (isinf(x) || isnan(x))
    {
        /* Specials use the digits "1#INF": %f and %e write "1.#INF00"
           (%e adds "e+000"); %g writes the significant digits. */
        char d[420];
        const char *text = special_text(x);
        if (t == 'g' || t == 'G')
        {
            int p = prec == 0 ? 1 : prec;
            special_digits(text, p - 1, d, sizeof(d));
            bl = (size_t)snprintf(body, sizeof(body), "%c%s%s", d[0], (p > 1 || sp->alt) ? "." : "", d + 1);
            if (!sp->alt)
            {
                while (bl > 2 && body[bl - 1] == '0') bl--;
                if (bl > 0 && body[bl - 1] == '.') bl--;
            }
        }
        else
        {
            special_digits(text, prec, d, sizeof(d));
            bl = (size_t)snprintf(body, sizeof(body), "%c%s%s", d[0], (prec > 0 || sp->alt) ? "." : "", d + 1);
            if (t == 'e' || t == 'E') bl += (size_t)snprintf(body + bl, sizeof(body) - bl, "%c+000", t);
        }
        emit(s, sp, sign, "", body, bl, false);
        return;
    }

    {
        char d[48];
        int e10 = 0, i;
        bool zero = (x == 0.0);
        if (zero)
        {
            memset(d, '0', 40);
            d[40] = 0;
        }
        else
        {
            exact_digits(x, d, &e10);
        }

        if (t == 'g' || t == 'G')
        {
            int p = prec == 0 ? 1 : prec, xe;
            char d2[48];
            memcpy(d2, d, sizeof(d2));
            xe = zero ? 0 : round_digits(d2, p, e10);
            if (xe < -4 || xe >= p)
            {
                t = (t == 'g') ? 'e' : 'E';
                prec = p - 1;
            }
            else
            {
                t = 'f';
                prec = p - 1 - xe;
            }
            /* Write as e or f, then remove trailing zeros (no '#'). */
            char tmpbody[512];
            size_t tl = 0;
            if (t == 'f')
            {
                int keep = zero ? 0 : e10 + 1 + prec;
                int ne = zero ? 0 : round_digits(d, keep, e10);
                int intdigits = ne >= 0 ? ne + 1 : 1;
                for (i = 0; i < intdigits && tl < sizeof(tmpbody) - 2; i++)
                    tmpbody[tl++] = (ne >= 0) ? digit_at(d, i) : '0';
                if (prec > 0 || sp->alt) tmpbody[tl++] = '.';
                for (i = 0; i < prec && tl < sizeof(tmpbody) - 2; i++)
                    tmpbody[tl++] = digit_at(d, ne + 1 + i);
            }
            else
            {
                int ne = zero ? 0 : round_digits(d, prec + 1, e10);
                tmpbody[tl++] = digit_at(d, 0);
                if (prec > 0 || sp->alt) tmpbody[tl++] = '.';
                for (i = 0; i < prec && tl < sizeof(tmpbody) - 12; i++) tmpbody[tl++] = digit_at(d, 1 + i);
                tl += (size_t)snprintf(tmpbody + tl, sizeof(tmpbody) - tl, "%c%c%03d", t, ne < 0 ? '-' : '+', abs(ne));
            }
            if (!sp->alt)
            {
                /* Remove trailing zeros of the fraction (before the exponent). */
                char *ep = memchr(tmpbody, t, tl);
                size_t mant = ep ? (size_t)(ep - tmpbody) : tl;
                if (memchr(tmpbody, '.', mant) != NULL)
                {
                    size_t m2 = mant;
                    while (m2 > 0 && tmpbody[m2 - 1] == '0') m2--;
                    if (m2 > 0 && tmpbody[m2 - 1] == '.') m2--;
                    memmove(tmpbody + m2, tmpbody + mant, tl - mant);
                    tl -= mant - m2;
                }
            }
            memcpy(body, tmpbody, tl);
            bl = tl;
        }
        else if (t == 'f')
        {
            int keep = zero ? 0 : e10 + 1 + prec;
            int ne = zero ? 0 : round_digits(d, keep, e10);
            if (!zero && keep < 0)
            {
                ne = e10;
                memset(d, '0', 40);
            }
            int intdigits = ne >= 0 ? ne + 1 : 1;
            if (intdigits > 320) intdigits = 320;
            for (i = 0; i < intdigits; i++) body[bl++] = (ne >= 0) ? digit_at(d, i) : '0';
            if (prec > 0 || sp->alt) body[bl++] = '.';
            for (i = 0; i < prec && bl < sizeof(body) - 1; i++)
                body[bl++] = (ne >= 0) ? digit_at(d, ne + 1 + i) : (i < -ne - 1 ? '0' : digit_at(d, i + ne + 1));
        }
        else if (t == 'e' || t == 'E')
        {
            int ne = zero ? 0 : round_digits(d, prec + 1, e10);
            body[bl++] = digit_at(d, 0);
            if (prec > 0 || sp->alt) body[bl++] = '.';
            for (i = 0; i < prec && bl < sizeof(body) - 12; i++) body[bl++] = digit_at(d, 1 + i);
            bl += (size_t)snprintf(body + bl, sizeof(body) - bl, "%c%c%03d", t, ne < 0 ? '-' : '+', abs(ne));
        }
        else
        {
            rt_fatal("printf conversion %%%c is not supported", t);
        }
    }
    emit(s, sp, sign, "", body, bl, true);
}

static void fmt_int(sink *s, const spec *sp, uint64_t v, bool is_signed, bool is64)
{
    char body[80];
    size_t bl = 0;
    const char *sign = "", *prefix = "";
    char t = sp->type;
    bool neg = false;
    unsigned base = (t == 'o') ? 8u : (t == 'x' || t == 'X' || t == 'p') ? 16u : 10u;
    bool nonzero;
    const char *digits = (t == 'X' || t == 'p') ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[80];
    size_t tl = 0;
    int prec = sp->prec;

    if (!is64) v = is_signed ? (uint64_t)(int64_t)(int32_t)(uint32_t)v : (uint32_t)v;
    if (sp->size == 'h') v = is_signed ? (uint64_t)(int64_t)(int16_t)(uint16_t)v : (uint16_t)v;
    if (is_signed && (int64_t)v < 0)
    {
        neg = true;
        v = 0u - v;
    }
    nonzero = v != 0;
    while (v != 0)
    {
        tmp[tl++] = digits[v % base];
        v /= base;
    }
    if (t == 'p') prec = 8;
    if (prec < 0) prec = 1;
    if (prec > 64) prec = 64;
    while ((int)tl < prec) tmp[tl++] = '0';
    if (sp->alt && t == 'o' && (tl == 0 || tmp[tl - 1] != '0')) tmp[tl++] = '0';
    while (tl > 0) body[bl++] = tmp[--tl];

    if (is_signed)
    {
        if (neg) sign = "-";
        else if (sp->plus) sign = "+";
        else if (sp->space) sign = " ";
    }
    if (sp->alt && nonzero && (t == 'x' || t == 'X')) prefix = (t == 'x') ? "0x" : "0X";
    spec sp2 = *sp;
    if (sp->prec >= 0) sp2.zero = false; /* a precision disables '0' for integers */
    emit(s, &sp2, sign, prefix, body, bl, true);
}

/* Convert a wide character (C locale, code page 1252 subset 0-255). */
static bool wide_to_char(uint32_t w, char *c)
{
    if (w > 255u) return false;
    *c = (char)w;
    return true;
}

static void fmt_string(sink *s, const spec *sp, uint32_t g, bool wide)
{
    static const char null_text[] = "(null)";
    size_t len, i;
    int prec = sp->prec;

    if (g == 0)
    {
        len = 6;
        if (prec >= 0 && (size_t)prec < len) len = (size_t)prec;
        emit(s, sp, "", "", null_text, len, false);
        return;
    }
    if (!wide)
    {
        const char *p = G2H(g);
        len = (prec >= 0) ? strnlen(p, (size_t)prec) : strlen(p);
        emit(s, sp, "", "", p, len, false);
        return;
    }
    /* Wide: count and convert. */
    for (len = 0; (prec < 0 || len < (size_t)prec) && rd16(g + 2u * (uint32_t)len) != 0; len++) {}
    for (i = 0; i < len; i++)
    {
        char c;
        if (!wide_to_char(rd16(g + 2u * (uint32_t)i), &c))
        {
            s->error = true;
            return;
        }
    }
    {
        int pad = sp->width - (int)len;
        if (!sp->minus) out_pad(s, ' ', pad);
        for (i = 0; i < len; i++)
        {
            char c;
            wide_to_char(rd16(g + 2u * (uint32_t)i), &c);
            out_char(s, c);
        }
        if (sp->minus) out_pad(s, ' ', pad);
    }
}

/* wsprintf mode: Windows USER32 formatting (no floating point, %S is a
   wide string, %ls/%hs). */
static int do_format(sink *s, const char *fmt, args *a, bool wsprintf_mode, const char *fname)
{
    const char *p = fmt;
    while (*p)
    {
        spec sp;
        if (*p != '%')
        {
            out_char(s, *p++);
            continue;
        }
        p++;
        if (*p == '%')
        {
            out_char(s, *p++);
            continue;
        }
        memset(&sp, 0, sizeof(sp));
        sp.prec = -1;
        for (;; p++)
        {
            if (*p == '-') sp.minus = true;
            else if (*p == '+' && !wsprintf_mode) sp.plus = true;
            else if (*p == ' ' && !wsprintf_mode) sp.space = true;
            else if (*p == '0') sp.zero = true;
            else if (*p == '#') sp.alt = true;
            else break;
        }
        if (*p == '*' && !wsprintf_mode)
        {
            int32_t w = (int32_t)arg32(a);
            if (w < 0)
            {
                sp.minus = true;
                w = -w;
            }
            sp.width = w > 100000 ? 100000 : w;
            p++;
        }
        else
        {
            while (*p >= '0' && *p <= '9')
            {
                if (sp.width < 100000) sp.width = sp.width * 10 + (*p - '0');
                p++;
            }
        }
        if (*p == '.')
        {
            p++;
            sp.prec = 0;
            if (*p == '*' && !wsprintf_mode)
            {
                int32_t pr = (int32_t)arg32(a);
                sp.prec = pr < 0 ? -1 : (pr > 100000 ? 100000 : pr);
                p++;
            }
            else
            {
                while (*p >= '0' && *p <= '9')
                {
                    if (sp.prec < 100000) sp.prec = sp.prec * 10 + (*p - '0');
                    p++;
                }
            }
        }
        /* Size. */
        if (*p == 'h') { sp.size = 'h'; p++; }
        else if (*p == 'l')
        {
            p++;
            if (*p == 'l' && !wsprintf_mode) { sp.size = 'q'; p++; }
            else sp.size = 'l';
        }
        else if (*p == 'L' && !wsprintf_mode) { sp.size = 'L'; p++; }
        else if (*p == 'w' && !wsprintf_mode) { sp.size = 'l'; p++; }
        else if (*p == 'I' && !wsprintf_mode)
        {
            if (p[1] == '6' && p[2] == '4') { sp.size = 'q'; p += 3; }
            else if (p[1] == '3' && p[2] == '2') { sp.size = 0; p += 3; }
            else { sp.size = 'I'; p++; }
        }
        sp.type = *p;
        if (sp.type == 0) break;
        p++;
        switch (sp.type)
        {
        case 'd':
        case 'i':
            if (sp.size == 'q') fmt_int(s, &sp, arg64(a), true, true);
            else fmt_int(s, &sp, arg32(a), true, false);
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            if (wsprintf_mode && sp.type == 'o') goto invalid;
            if (sp.size == 'q') fmt_int(s, &sp, arg64(a), false, true);
            else fmt_int(s, &sp, arg32(a), false, false);
            break;
        case 'p':
            if (wsprintf_mode) goto invalid;
            sp.alt = false;
            fmt_int(s, &sp, arg32(a), false, false);
            break;
        case 'c':
        case 'C':
        {
            uint32_t v = arg32(a);
            bool wide = (sp.type == 'c') ? (sp.size == 'l') : (sp.size != 'h');
            char c;
            if (wide)
            {
                if (!wide_to_char(v & 0xFFFFu, &c))
                {
                    s->error = true;
                    break;
                }
            }
            else
            {
                c = (char)v;
            }
            emit(s, &sp, "", "", &c, 1, false);
            break;
        }
        case 's':
        case 'S':
        {
            uint32_t g = arg32(a);
            bool wide = (sp.type == 's') ? (sp.size == 'l') : (sp.size != 'h');
            fmt_string(s, &sp, g, wide);
            break;
        }
        case 'Z':
        {
            /* ANSI_STRING / UNICODE_STRING: Length, MaximumLength, Buffer. */
            uint32_t g;
            if (wsprintf_mode) goto invalid;
            g = arg32(a);
            if (g == 0 || rd32(g + 4) == 0)
            {
                fmt_string(s, &sp, 0, false);
            }
            else
            {
                spec sp2 = sp;
                uint32_t len = rd16(g);
                if (sp.size == 'l') len /= 2u;
                if (sp2.prec < 0 || (uint32_t)sp2.prec > len) sp2.prec = (int)len;
                fmt_string(s, &sp2, rd32(g + 4), sp.size == 'l');
            }
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G':
            if (wsprintf_mode) goto invalid;
            fmt_float(s, &sp, argf(a));
            break;
        case 'n':
            if (wsprintf_mode) goto invalid;
            if (!count_output_enabled) rt_crt_invalid_parameter(fname);
            {
                uint32_t g = arg32(a);
                if (sp.size == 'h') wr16(g, (uint16_t)s->count);
                else wr32(g, s->count);
            }
            break;
        default:
        invalid:
            if (wsprintf_mode)
            {
                /* wsprintf writes an unknown conversion character as text. */
                out_char(s, sp.type);
                break;
            }
            rt_crt_invalid_parameter(fname);
        }
        if (s->error) return -1;
    }
    return s->error ? -1 : (int)s->count;
}

/* ---------------------------------------------------------------- printf family */

static const char *fmt_arg(uint32_t g, const char *fname)
{
    if (g == 0) rt_crt_invalid_parameter(fname);
    return G2H(g);
}

/* Write to a buffer with at most limit characters plus the terminator
   rules of _vsnprintf: returns the length if it fits (terminated when
   there is room), -1 if not. */
static int32_t vsnprintf_core(uint32_t buf, uint32_t count, const char *fmt, uint32_t ap, const char *fname)
{
    sink s = { .buf = buf, .limit = count };
    args a = { ap };
    int r = do_format(&s, fmt, &a, false, fname);
    if (r < 0)
    {
        rt_crt_set_errno(EILSEQ_);
        return -1;
    }
    if (s.count < count)
    {
        wr8(buf + s.count, 0);
        return (int32_t)s.count;
    }
    if (s.count == count) return (int32_t)count; /* fits exactly, no terminator */
    return -1;
}

void rt__vsnprintf(_cpu *cpu)
{
    uint32_t buf = ARG(0), count = ARG(1);
    const char *fmt = fmt_arg(ARG(2), "_vsnprintf");
    if (buf == 0 && count != 0) rt_crt_invalid_parameter("_vsnprintf");
    RET((uint32_t)vsnprintf_core(buf, count, fmt, ARG(3), "_vsnprintf"));
}

void rt__snprintf(_cpu *cpu)
{
    uint32_t buf = ARG(0), count = ARG(1);
    const char *fmt = fmt_arg(ARG(2), "_snprintf");
    if (buf == 0 && count != 0) rt_crt_invalid_parameter("_snprintf");
    RET((uint32_t)vsnprintf_core(buf, count, fmt, ARGADR(3), "_snprintf"));
}

/* _vsnprintf_s(buf, size, count, fmt, ap) */
static int32_t vsnprintf_s_core(uint32_t buf, uint32_t size, uint32_t count, const char *fmt, uint32_t ap, const char *fname)
{
    sink s;
    args a = { ap };
    int r;
    uint32_t limit;

    if (count == 0 && buf == 0 && size == 0) return 0;
    if (buf == 0 || size == 0) rt_crt_invalid_parameter(fname);
    limit = size - 1u;
    if (count != TRUNCATE_ && count < limit) limit = count;
    memset(&s, 0, sizeof(s));
    s.buf = buf;
    s.limit = limit;
    r = do_format(&s, fmt, &a, false, fname);
    if (r < 0)
    {
        wr8(buf, 0);
        rt_crt_set_errno(EILSEQ_);
        return -1;
    }
    if (s.count <= limit)
    {
        wr8(buf + s.count, 0);
        return (int32_t)s.count;
    }
    /* Truncated. */
    if (count == TRUNCATE_ || count < size)
    {
        wr8(buf + limit, 0);
        return -1;
    }
    wr8(buf, 0);
    rt_crt_invalid_parameter(fname);
}

void rt__vsnprintf_s(_cpu *cpu)
{
    const char *fmt = fmt_arg(ARG(3), "_vsnprintf_s");
    RET((uint32_t)vsnprintf_s_core(ARG(0), ARG(1), ARG(2), fmt, ARG(4), "_vsnprintf_s"));
}

void rt__snprintf_s(_cpu *cpu)
{
    const char *fmt = fmt_arg(ARG(3), "_snprintf_s");
    RET((uint32_t)vsnprintf_s_core(ARG(0), ARG(1), ARG(2), fmt, ARGADR(4), "_snprintf_s"));
}

void rt_sprintf(_cpu *cpu)
{
    uint32_t buf = ARG(0);
    const char *fmt = fmt_arg(ARG(1), "sprintf");
    sink s = { .buf = buf, .limit = UINT32_MAX };
    args a = { ARGADR(2) };
    int r;
    if (buf == 0) rt_crt_invalid_parameter("sprintf");
    r = do_format(&s, fmt, &a, false, "sprintf");
    if (r < 0)
    {
        rt_crt_set_errno(EILSEQ_);
        RET(0xFFFFFFFFu);
        return;
    }
    wr8(buf + s.count, 0);
    RET(s.count);
}

void rt_sprintf_s(_cpu *cpu)
{
    uint32_t buf = ARG(0), size = ARG(1);
    const char *fmt = fmt_arg(ARG(2), "sprintf_s");
    sink s;
    args a = { ARGADR(3) };
    int r;
    if (buf == 0 || size == 0) rt_crt_invalid_parameter("sprintf_s");
    memset(&s, 0, sizeof(s));
    s.buf = buf;
    s.limit = size - 1u;
    r = do_format(&s, fmt, &a, false, "sprintf_s");
    if (r < 0)
    {
        wr8(buf, 0);
        rt_crt_set_errno(EILSEQ_);
        RET(0xFFFFFFFFu);
        return;
    }
    if (s.count > size - 1u)
    {
        wr8(buf, 0);
        rt_crt_invalid_parameter("sprintf_s");
    }
    wr8(buf + s.count, 0);
    RET(s.count);
}

void rt_fprintf(_cpu *cpu)
{
    uint32_t f = ARG(0);
    const char *fmt = fmt_arg(ARG(1), "fprintf");
    sink s;
    args a = { ARGADR(2) };
    int r;
    if (f == 0) rt_crt_invalid_parameter("fprintf");
    memset(&s, 0, sizeof(s));
    s.file = f;
    rt_crt_file_lock(f);
    r = do_format(&s, fmt, &a, false, "fprintf");
    rt_crt_file_unlock(f);
    RET(r < 0 ? 0xFFFFFFFFu : (uint32_t)r);
}

/* USER32 wsprintfA (cdecl, variable arguments) and wvsprintfA (stdcall).
   The output buffer has a limit of 1024 characters. */
static uint32_t wsprintf_core(uint32_t buf, const char *fmt, uint32_t ap)
{
    sink s = { .buf = buf, .limit = 1023u };
    args a = { ap };
    do_format(&s, fmt, &a, true, "wsprintfA");
    if (s.count > 1023u) s.count = 1023u;
    wr8(buf + s.count, 0);
    return s.count;
}

void rt_wsprintfA(_cpu *cpu)
{
    if (ARG(0) == 0 || ARG(1) == 0)
    {
        RET(0);
        return;
    }
    RET(wsprintf_core(ARG(0), G2H(ARG(1)), ARGADR(2)));
}

void rt_wvsprintfA(_cpu *cpu)
{
    if (ARG(0) == 0 || ARG(1) == 0)
    {
        RET(0);
        return;
    }
    RET(wsprintf_core(ARG(0), G2H(ARG(1)), ARG(2)));
}

/* ---------------------------------------------------------------- scanf family */

typedef struct input {
    uint32_t s;      /* guest string */
    bool wide;
    uint32_t pos;
} input;

static int in_peek(const input *in)
{
    uint32_t c = in->wide ? rd16(in->s + 2u * in->pos) : rd8(in->s + in->pos);
    return c == 0 ? -1 : (int)c;
}

static void in_next(input *in)
{
    in->pos++;
}

static bool is_ws(int c)
{
    return c == ' ' || (c >= 9 && c <= 13);
}

static void store_int(uint32_t g, char size, uint64_t v)
{
    if (size == 'h') wr16(g, (uint16_t)v);
    else if (size == 'q') wr64(g, v);
    else if (size == 'H') wr8(g, (uint8_t)v); /* hh */
    else wr32(g, (uint32_t)v);
}

/* The scanf engine. secure: the _s form with buffer sizes after %s, %c
   and %[ pointers. Returns the number of assigned fields, or -1 (EOF) for
   an input failure before the first conversion. */
static int do_scan(input *in, const char *fmt, args *a, bool secure, const char *fname)
{
    const char *p = fmt;
    int assigned = 0;
    bool any_conversion = false;

    while (*p)
    {
        if (is_ws((unsigned char)*p))
        {
            while (is_ws((unsigned char)*p)) p++;
            while (is_ws(in_peek(in))) in_next(in);
            continue;
        }
        if (*p != '%' || p[1] == '%')
        {
            int c;
            if (*p == '%') p++;
            c = in_peek(in);
            if (c < 0) return (assigned == 0 && !any_conversion) ? -1 : assigned;
            if (c != (unsigned char)*p) return assigned;
            in_next(in);
            p++;
            continue;
        }
        p++;
        bool suppress = false;
        int width = 0;
        char size = 0;
        if (*p == '*') { suppress = true; p++; }
        while (*p >= '0' && *p <= '9') { if (width < 100000) width = width * 10 + (*p - '0'); p++; }
        if (*p == 'h') { p++; size = 'h'; if (*p == 'h') { size = 'H'; p++; } }
        else if (*p == 'l') { p++; size = 'l'; if (*p == 'l') { size = 'q'; p++; } }
        else if (*p == 'L') { size = 'L'; p++; }
        else if (*p == 'I' && p[1] == '6' && p[2] == '4') { size = 'q'; p += 3; }
        else if (*p == 'I' && p[1] == '3' && p[2] == '2') { size = 0; p += 3; }
        else if (*p == 'I') { size = 0; p++; }
        else if (*p == 'w') { size = 'l'; p++; }
        char t = *p;
        if (t == 0) rt_crt_invalid_parameter(fname);
        p++;

        if (t != 'c' && t != 'C' && t != '[' && t != 'n')
            while (is_ws(in_peek(in))) in_next(in);
        if (t != 'n' && in_peek(in) < 0) return (assigned == 0 && !any_conversion) ? -1 : assigned;

        switch (t)
        {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p':
        {
            char buf[80];
            int n = 0, maxw = width > 0 ? width : 79, base;
            int c = in_peek(in);
            if (maxw > 79) maxw = 79;
            if (c == '+' || c == '-') { buf[n++] = (char)c; in_next(in); }
            base = (t == 'd' || t == 'u') ? 10 : (t == 'o') ? 8 : (t == 'i') ? 0 : 16;
            if ((base == 0 || base == 16) && n < maxw && in_peek(in) == '0')
            {
                buf[n++] = '0';
                in_next(in);
                if (n < maxw && (in_peek(in) == 'x' || in_peek(in) == 'X'))
                {
                    buf[n++] = 'x';
                    in_next(in);
                    base = 16;
                }
                else if (base == 0)
                {
                    base = 8;
                }
            }
            if (base == 0) base = 10;
            while (n < maxw)
            {
                c = in_peek(in);
                bool ok = (base == 8) ? (c >= '0' && c <= '7') : (base == 10) ? (c >= '0' && c <= '9')
                          : ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
                if (!ok) break;
                buf[n++] = (char)c;
                in_next(in);
            }
            buf[n] = 0;
            {
                char *end;
                bool neg = buf[0] == '-';
                const char *digits = buf + ((buf[0] == '-' || buf[0] == '+') ? 1 : 0);
                if (base == 16 && digits[0] == '0' && (digits[1] == 'x')) digits += 2;
                if (*digits == 0 && !(base == 8 && strchr(buf, '0') != NULL) && !(base == 16 && strstr(buf, "0x") != NULL))
                    return assigned;
                uint64_t v = strtoull(*digits ? digits : "0", &end, base);
                if (neg) v = 0u - v;
                any_conversion = true;
                if (!suppress)
                {
                    store_int(arg32(a), t == 'p' ? 0 : size, v);
                    assigned++;
                }
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G':
        {
            /* [sign] digits [. digits] [e|E [sign] digits]: every accepted
               character is consumed (as in the Microsoft runtime, "1e" is
               read completely and gives 1). */
            char buf[512];
            int n = 0, maxw = width > 0 ? width : 511, c;
            bool digits = false;
            const char *end;
            bool range;
            if (maxw > 511) maxw = 511;
            c = in_peek(in);
            if (n < maxw && (c == '+' || c == '-')) { buf[n++] = (char)c; in_next(in); }
            while (n < maxw && (c = in_peek(in)) >= '0' && c <= '9') { buf[n++] = (char)c; in_next(in); digits = true; }
            if (n < maxw && in_peek(in) == '.')
            {
                buf[n++] = '.';
                in_next(in);
                while (n < maxw && (c = in_peek(in)) >= '0' && c <= '9') { buf[n++] = (char)c; in_next(in); digits = true; }
            }
            if (digits && n < maxw && ((c = in_peek(in)) == 'e' || c == 'E'))
            {
                buf[n++] = 'e';
                in_next(in);
                c = in_peek(in);
                if (n < maxw && (c == '+' || c == '-')) { buf[n++] = (char)c; in_next(in); }
                while (n < maxw && (c = in_peek(in)) >= '0' && c <= '9') { buf[n++] = (char)c; in_next(in); }
            }
            buf[n] = 0;
            if (!digits) return assigned;
            double v = rt_crt_parse_double(buf, &end, &range);
            any_conversion = true;
            if (!suppress)
            {
                uint32_t g = arg32(a);
                if (size == 'l' || size == 'L') memcpy(G2H(g), &v, 8);
                else { float f = (float)v; memcpy(G2H(g), &f, 4); }
                assigned++;
            }
            break;
        }
        case 's': case 'S': case 'c': case 'C':
        {
            bool to_wide = (t == 's' || t == 'c') ? (size == 'l') : (size != 'h');
            bool is_c = (t == 'c' || t == 'C');
            uint32_t dst = 0, cap = UINT32_MAX, n = 0;
            int maxw = width > 0 ? width : (is_c ? 1 : INT32_MAX);
            if (!suppress)
            {
                dst = arg32(a);
                if (secure) cap = arg32(a);
            }
            while ((int)n < maxw)
            {
                int c = in_peek(in);
                if (c < 0 || (!is_c && is_ws(c))) break;
                if (!suppress)
                {
                    if (n + (is_c ? 0u : 1u) >= cap)
                    {
                        if (to_wide) wr16(dst, 0); else wr8(dst, 0);
                        return assigned; /* the buffer is too small: the field fails */
                    }
                    if (to_wide) wr16(dst + 2u * n, (uint16_t)c);
                    else
                    {
                        char ch;
                        if (in->wide && !wide_to_char((uint32_t)c, &ch)) return assigned;
                        wr8(dst + n, in->wide ? (uint8_t)ch : (uint8_t)c);
                    }
                }
                n++;
                in_next(in);
            }
            if (n == 0) return assigned;
            if (is_c && (int)n < maxw && width > 0) return assigned;
            any_conversion = true;
            if (!suppress)
            {
                if (!is_c)
                {
                    if (to_wide) wr16(dst + 2u * n, 0); else wr8(dst + n, 0);
                }
                assigned++;
            }
            break;
        }
        case '[':
        {
            bool set[256] = { false };
            bool invert = false;
            uint32_t dst = 0, cap = UINT32_MAX, n = 0;
            int maxw = width > 0 ? width : INT32_MAX;
            if (*p == '^') { invert = true; p++; }
            if (*p == ']') { set[']'] = true; p++; }
            while (*p && *p != ']')
            {
                unsigned char a1 = (unsigned char)*p;
                if (p[1] == '-' && p[2] && p[2] != ']')
                {
                    unsigned char b1 = (unsigned char)p[2], lo = a1 < b1 ? a1 : b1, hi = a1 < b1 ? b1 : a1;
                    for (unsigned k = lo; k <= hi; k++) set[k] = true;
                    p += 3;
                }
                else
                {
                    set[a1] = true;
                    p++;
                }
            }
            if (*p != ']') rt_crt_invalid_parameter(fname);
            p++;
            if (!suppress)
            {
                dst = arg32(a);
                if (secure) cap = arg32(a);
            }
            while ((int)n < maxw)
            {
                int c = in_peek(in);
                bool member;
                if (c < 0) break;
                member = c < 256 ? set[c] : false;
                if (member == invert) break;
                if (!suppress)
                {
                    if (n + 1u >= cap)
                    {
                        if (size == 'l') wr16(dst, 0); else wr8(dst, 0);
                        return assigned;
                    }
                    if (size == 'l') wr16(dst + 2u * n, (uint16_t)c);
                    else wr8(dst + n, (uint8_t)c);
                }
                n++;
                in_next(in);
            }
            if (n == 0) return assigned;
            any_conversion = true;
            if (!suppress)
            {
                if (size == 'l') wr16(dst + 2u * n, 0); else wr8(dst + n, 0);
                assigned++;
            }
            break;
        }
        case 'n':
            if (!suppress) store_int(arg32(a), size, in->pos);
            break;
        default:
            rt_crt_invalid_parameter(fname);
        }
    }
    return assigned;
}

void rt_sscanf(_cpu *cpu)
{
    input in = { ARG(0), false, 0 };
    args a = { ARGADR(2) };
    if (ARG(0) == 0 || ARG(1) == 0) rt_crt_invalid_parameter("sscanf");
    RET((uint32_t)do_scan(&in, G2H(ARG(1)), &a, false, "sscanf"));
}

void rt_sscanf_s(_cpu *cpu)
{
    input in = { ARG(0), false, 0 };
    args a = { ARGADR(2) };
    if (ARG(0) == 0 || ARG(1) == 0) rt_crt_invalid_parameter("sscanf_s");
    RET((uint32_t)do_scan(&in, G2H(ARG(1)), &a, true, "sscanf_s"));
}

/* swscanf(wide input, wide format, ...): the format is converted to
   single-byte characters (the conversion characters are ASCII). */
void rt_swscanf(_cpu *cpu)
{
    input in = { ARG(0), true, 0 };
    args a = { ARGADR(2) };
    uint32_t f = ARG(1), n = 0;
    char *fmt;
    int r;
    if (ARG(0) == 0 || f == 0) rt_crt_invalid_parameter("swscanf");
    while (rd16(f + 2u * n) != 0) n++;
    fmt = malloc((size_t)n + 1u);
    if (fmt == NULL) rt_fatal("out of host memory");
    for (uint32_t i = 0; i < n; i++)
    {
        uint16_t c = rd16(f + 2u * i);
        if (c > 127u)
        {
            /* A literal non-ASCII character in the format: match it exactly
               is not possible with the single-byte copy. */
            free(fmt);
            rt_fatal("swscanf format with non-ASCII characters is not supported");
        }
        fmt[i] = (char)c;
    }
    fmt[n] = 0;
    /* In swscanf, %s, %c and %[ store wide characters (as %ls). */
    {
        size_t len = strlen(fmt), extra = 0;
        for (size_t i = 0; i < len; i++)
            if (fmt[i] == '%') extra++;
        char *f2 = malloc(len + 2u * extra + 1u);
        size_t j = 0;
        if (f2 == NULL) rt_fatal("out of host memory");
        for (size_t i = 0; i < len; i++)
        {
            f2[j++] = fmt[i];
            if (fmt[i] == '%' && i + 1 < len && fmt[i + 1] != '%')
            {
                size_t k = i + 1;
                while (k < len && (fmt[k] == '*' || (fmt[k] >= '0' && fmt[k] <= '9'))) f2[j++] = fmt[k++];
                if (k < len && (fmt[k] == 's' || fmt[k] == 'c' || fmt[k] == '['))
                {
                    f2[j++] = 'l';
                }
                else if (k < len && (fmt[k] == 'S' || fmt[k] == 'C'))
                {
                    /* In the wide function %S and %C are single-byte. */
                    f2[j++] = 'h';
                    f2[j++] = (char)(fmt[k] == 'S' ? 's' : 'c');
                    k++;
                }
                i = k - 1;
            }
            else if (fmt[i] == '%' && i + 1 < len)
            {
                f2[j++] = fmt[++i];
            }
        }
        f2[j] = 0;
        r = do_scan(&in, f2, &a, false, "swscanf");
        free(f2);
    }
    free(fmt);
    RET((uint32_t)r);
}
