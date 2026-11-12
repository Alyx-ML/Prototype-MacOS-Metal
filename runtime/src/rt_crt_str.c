/*
 * MSVCR80 memory, string, character class, number conversion, time and
 * random number functions. The behavior is that of the Microsoft C
 * runtime 8.0 in the "C" locale with code page 1252.
 */
#include "rt_crt.h"
#include "rt_thread.h"

#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wctype.h>
#include <xlocale.h>

#define EINVAL_ 22u
#define ERANGE_ 34u
#define EILSEQ_ 42u
#define ENOMEM_ 12u
#define STRUNCATE_ 80u
#define TRUNCATE_ 0xFFFFFFFFu

static size_t gstrlen(uint32_t s) { return strlen((const char *)G2H(s)); }
static size_t gwcslen(uint32_t s)
{
    size_t n = 0;
    while (rd16(s + 2u * (uint32_t)n) != 0) n++;
    return n;
}

/* The invalid parameter handler of the C runtime. No handler can be set
   (_set_invalid_parameter_handler is not imported), so the default handler
   ends the process. */
_Noreturn void rt_crt_invalid_parameter(const char *function)
{
    rt_log("invalid parameter passed to the C runtime function %s", function);
    rt_process_exit(0xC0000417u);
}

/* ---------------------------------------------------------------- memory */

void rt_malloc(_cpu *cpu)
{
    uint32_t n = ARG(0), p;
    if (n > 0xFFFFFFE0u)
    {
        rt_crt_set_errno(ENOMEM_);
        RET(0);
        return;
    }
    p = rt_heap_alloc(n == 0 ? 1u : n);
    if (p == 0) rt_crt_set_errno(ENOMEM_);
    RET(p);
}

void rt_free(_cpu *cpu)
{
    if (ARG(0) != 0) rt_heap_free(ARG(0));
}

void rt_realloc(_cpu *cpu)
{
    uint32_t p = ARG(0), n = ARG(1), r;
    if (p == 0)
    {
        r = rt_heap_alloc(n == 0 ? 1u : n);
        if (r == 0) rt_crt_set_errno(ENOMEM_);
        RET(r);
        return;
    }
    if (n == 0)
    {
        rt_heap_free(p);
        RET(0);
        return;
    }
    r = rt_heap_realloc(p, n);
    if (r == 0) rt_crt_set_errno(ENOMEM_);
    RET(r);
}

/* operator new: the default new handler is not set, so a failure throws
   std::bad_alloc. The game code has no C++ catch handler (the C++ frame
   handler is not imported), so the exception ends the process. */
void rt_operator_new(_cpu *cpu)
{
    uint32_t n = ARG(0), p;
    p = rt_heap_alloc(n == 0 ? 1u : n);
    if (p == 0) rt_fatal("operator new(%u): no guest memory (std::bad_alloc without a handler)", n);
    RET(p);
}

void rt_operator_delete(_cpu *cpu)
{
    if (ARG(0) != 0) rt_heap_free(ARG(0));
}

void rt_operator_delete_array(_cpu *cpu)
{
    if (ARG(0) != 0) rt_heap_free(ARG(0));
}

void rt_memchr(_cpu *cpu)
{
    uint32_t s = ARG(0), n = ARG(2);
    const void *r;
    if (n == 0)
    {
        RET(0);
        return;
    }
    r = memchr(G2H(s), (int)(uint8_t)ARG(1), n);
    RET(r ? s + (uint32_t)((const uint8_t *)r - (const uint8_t *)G2H(s)) : 0u);
}

void rt_memcpy(_cpu *cpu)
{
    /* The Microsoft memcpy handles overlapping buffers like memmove. */
    if (ARG(2) != 0) memmove(G2H(ARG(0)), G2H(ARG(1)), ARG(2));
    RET(ARG(0));
}

void rt_memmove(_cpu *cpu)
{
    if (ARG(2) != 0) memmove(G2H(ARG(0)), G2H(ARG(1)), ARG(2));
    RET(ARG(0));
}

void rt_memset(_cpu *cpu)
{
    if (ARG(2) != 0) memset(G2H(ARG(0)), (int)(uint8_t)ARG(1), ARG(2));
    RET(ARG(0));
}

void rt_memcpy_s(_cpu *cpu)
{
    uint32_t dst = ARG(0), size = ARG(1), src = ARG(2), n = ARG(3);
    if (n == 0)
    {
        RET(0);
        return;
    }
    if (dst == 0) rt_crt_invalid_parameter("memcpy_s");
    if (src == 0 || size < n)
    {
        memset(G2H(dst), 0, size);
        rt_crt_invalid_parameter("memcpy_s");
    }
    memmove(G2H(dst), G2H(src), n);
    RET(0);
}

void rt_memmove_s(_cpu *cpu)
{
    uint32_t dst = ARG(0), size = ARG(1), src = ARG(2), n = ARG(3);
    if (n == 0)
    {
        RET(0);
        return;
    }
    if (dst == 0 || src == 0 || size < n) rt_crt_invalid_parameter("memmove_s");
    memmove(G2H(dst), G2H(src), n);
    RET(0);
}

/* qsort: the algorithm of the Microsoft C runtime 8.0 (quicksort with a
   median of three, an explicit stack, and selection sort for 8 or fewer
   elements), so that elements that compare equal end in the same order. */
static void gswap(uint32_t a, uint32_t b, uint32_t width)
{
    uint8_t *p = G2H(a), *q = G2H(b);
    if (a == b) return;
    while (width--)
    {
        uint8_t t = *p;
        *p++ = *q;
        *q++ = t;
    }
}

static int32_t gcomp(_cpu *cpu, uint32_t fn, uint32_t a, uint32_t b)
{
    return (int32_t)rt_call_cdecl(cpu, fn, 2, a, b);
}

static void shortsort(_cpu *cpu, uint32_t lo, uint32_t hi, uint32_t width, uint32_t comp)
{
    while (hi > lo)
    {
        uint32_t max = lo, p;
        for (p = lo + width; p <= hi; p += width)
            if (gcomp(cpu, comp, p, max) > 0) max = p;
        gswap(max, hi, width);
        hi -= width;
    }
}

void rt_qsort(_cpu *cpu)
{
    uint32_t base = ARG(0), num = ARG(1), width = ARG(2), comp = ARG(3);
    uint32_t lo, hi, mid, loguy, higuy, size;
    uint32_t lostk[30], histk[30];
    int stkptr = 0;

    if (num < 2 || width == 0) return;
    if ((uint64_t)width * num > 0x80000000u) rt_crt_invalid_parameter("qsort");
    lo = base;
    hi = base + width * (num - 1u);
recurse:
    size = (hi - lo) / width + 1u;
    if (size <= 8u)
    {
        shortsort(cpu, lo, hi, width, comp);
    }
    else
    {
        mid = lo + (size / 2u) * width;
        if (gcomp(cpu, comp, lo, mid) > 0) gswap(lo, mid, width);
        if (gcomp(cpu, comp, lo, hi) > 0) gswap(lo, hi, width);
        if (gcomp(cpu, comp, mid, hi) > 0) gswap(mid, hi, width);
        loguy = lo;
        higuy = hi;
        for (;;)
        {
            if (mid > loguy)
            {
                do { loguy += width; } while (loguy < mid && gcomp(cpu, comp, loguy, mid) <= 0);
            }
            if (mid <= loguy)
            {
                do { loguy += width; } while (loguy <= hi && gcomp(cpu, comp, loguy, mid) <= 0);
            }
            do { higuy -= width; } while (higuy > mid && gcomp(cpu, comp, higuy, mid) > 0);
            if (higuy < loguy) break;
            gswap(loguy, higuy, width);
            if (mid == higuy) mid = loguy;
        }
        higuy += width;
        if (mid < higuy)
        {
            do { higuy -= width; } while (higuy > mid && gcomp(cpu, comp, higuy, mid) == 0);
        }
        if (mid >= higuy)
        {
            do { higuy -= width; } while (higuy > lo && gcomp(cpu, comp, higuy, mid) == 0);
        }
        if (higuy - lo >= hi - loguy)
        {
            if (lo < higuy)
            {
                lostk[stkptr] = lo;
                histk[stkptr] = higuy;
                ++stkptr;
            }
            if (loguy < hi)
            {
                lo = loguy;
                goto recurse;
            }
        }
        else
        {
            if (loguy < hi)
            {
                lostk[stkptr] = loguy;
                histk[stkptr] = hi;
                ++stkptr;
            }
            if (lo < higuy)
            {
                hi = higuy;
                goto recurse;
            }
        }
    }
    --stkptr;
    if (stkptr >= 0)
    {
        lo = lostk[stkptr];
        hi = histk[stkptr];
        goto recurse;
    }
}

/* ---------------------------------------------------------------- character classes */

/* The "C" locale: ASCII classes only; characters 128 to 255 have no class. */
static int c_class(int32_t c, int (*fn)(int))
{
    if (c < 0 || c > 127) return 0;
    return fn((int)c) ? 1 : 0;
}

/* The Microsoft functions return the class bit, not 1. The game uses
   them only as truth values; the runtime returns the bit values of the
   Microsoft table for exactness. */
#define CT_UPPER 0x1
#define CT_LOWER 0x2
#define CT_DIGIT 0x4
#define CT_SPACE 0x8
#define CT_PUNCT 0x10
#define CT_CONTROL 0x20
#define CT_BLANK 0x40
#define CT_HEX 0x80
#define CT_ALPHA (0x0100 | CT_UPPER | CT_LOWER)

void rt_isalnum(_cpu *cpu)
{
    int32_t c = (int32_t)ARG(0);
    RET(c_class(c, isalnum) ? (uint32_t)((isdigit((int)c) ? CT_DIGIT : 0) | (isupper((int)c) ? (0x100 | CT_UPPER) : 0) |
                                         (islower((int)c) ? (0x100 | CT_LOWER) : 0)) : 0u);
}
void rt_isalpha(_cpu *cpu)
{
    int32_t c = (int32_t)ARG(0);
    RET(c_class(c, isalpha) ? (uint32_t)(0x100 | (isupper((int)c) ? CT_UPPER : CT_LOWER)) : 0u);
}
void rt_iscntrl(_cpu *cpu) { RET(c_class((int32_t)ARG(0), iscntrl) ? CT_CONTROL : 0); }
void rt_isdigit(_cpu *cpu) { RET(c_class((int32_t)ARG(0), isdigit) ? CT_DIGIT : 0); }
void rt_islower(_cpu *cpu) { RET(c_class((int32_t)ARG(0), islower) ? CT_LOWER : 0); }
void rt_isupper(_cpu *cpu) { RET(c_class((int32_t)ARG(0), isupper) ? CT_UPPER : 0); }
void rt_ispunct(_cpu *cpu) { RET(c_class((int32_t)ARG(0), ispunct) ? CT_PUNCT : 0); }
void rt_isspace(_cpu *cpu) { RET(c_class((int32_t)ARG(0), isspace) ? CT_SPACE : 0); }
void rt_isxdigit(_cpu *cpu)
{
    int32_t c = (int32_t)ARG(0);
    RET(c_class(c, isxdigit) ? (uint32_t)(CT_HEX | (isdigit((int)c) ? CT_DIGIT : 0)) : 0u);
}

void rt_tolower(_cpu *cpu)
{
    int32_t c = (int32_t)ARG(0);
    RET((c >= 'A' && c <= 'Z') ? c + 32 : c);
}

void rt_toupper(_cpu *cpu)
{
    int32_t c = (int32_t)ARG(0);
    RET((c >= 'a' && c <= 'z') ? c - 32 : c);
}

/* Wide character classes: Unicode classes (the Microsoft runtime uses the
   Unicode character type table for wide characters). */
static locale_t wide_locale;
static pthread_once_t wide_once = PTHREAD_ONCE_INIT;

static void init_wide_locale(void)
{
    wide_locale = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", NULL);
    if (wide_locale == NULL) rt_fatal("the UTF-8 locale is not available");
}

static locale_t wloc(void)
{
    if (pthread_once(&wide_once, init_wide_locale) != 0) rt_fatal("pthread_once failed");
    return wide_locale;
}

void rt_iswalnum(_cpu *cpu)
{
    uint32_t c = ARG(0) & 0xFFFFu;
    RET(iswalnum_l((wint_t)c, wloc()) ? 1u : 0u);
}

void rt_iswspace(_cpu *cpu)
{
    uint32_t c = ARG(0) & 0xFFFFu;
    RET(iswspace_l((wint_t)c, wloc()) ? CT_SPACE : 0u);
}

/* towlower and towupper in the "C" locale change only A-Z and a-z. */
void rt_towlower(_cpu *cpu)
{
    uint32_t c = ARG(0) & 0xFFFFu;
    RET((c >= 'A' && c <= 'Z') ? c + 32u : c);
}

void rt_towupper(_cpu *cpu)
{
    uint32_t c = ARG(0) & 0xFFFFu;
    RET((c >= 'a' && c <= 'z') ? c - 32u : c);
}

/* Code page 1252 has no lead bytes. */
void rt__ismbblead(_cpu *cpu)
{
    RET(0);
}

/* ---------------------------------------------------------------- strings */

static int lower_ascii(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

void rt__stricmp(_cpu *cpu)
{
    const uint8_t *a = G2H(ARG(0)), *b = G2H(ARG(1));
    int f, l;
    if (a == NULL || b == NULL) rt_crt_invalid_parameter("_stricmp");
    do
    {
        f = lower_ascii(*a++);
        l = lower_ascii(*b++);
    } while (f && f == l);
    RET((uint32_t)(f - l));
}

void rt__strnicmp(_cpu *cpu)
{
    const uint8_t *a = G2H(ARG(0)), *b = G2H(ARG(1));
    uint32_t n = ARG(2);
    int f = 0, l = 0;
    if (n == 0)
    {
        RET(0);
        return;
    }
    if (a == NULL || b == NULL) rt_crt_invalid_parameter("_strnicmp");
    do
    {
        f = lower_ascii(*a++);
        l = lower_ascii(*b++);
    } while (--n && f && f == l);
    RET((uint32_t)(f - l));
}

void rt__strlwr(_cpu *cpu)
{
    uint8_t *s = G2H(ARG(0));
    if (s == NULL) rt_crt_invalid_parameter("_strlwr");
    for (; *s; s++) *s = (uint8_t)lower_ascii(*s);
    RET(ARG(0));
}

void rt__strupr(_cpu *cpu)
{
    uint8_t *s = G2H(ARG(0));
    if (s == NULL) rt_crt_invalid_parameter("_strupr");
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (uint8_t)(*s - 32);
    RET(ARG(0));
}

void rt_strchr(_cpu *cpu)
{
    uint32_t s = ARG(0);
    const char *r = strchr(G2H(s), (int)(char)ARG(1));
    RET(r ? s + (uint32_t)(r - (const char *)G2H(s)) : 0u);
}

void rt_strrchr(_cpu *cpu)
{
    uint32_t s = ARG(0);
    const char *r = strrchr(G2H(s), (int)(char)ARG(1));
    RET(r ? s + (uint32_t)(r - (const char *)G2H(s)) : 0u);
}

void rt_strstr(_cpu *cpu)
{
    uint32_t s = ARG(0);
    const char *r = strstr(G2H(s), G2H(ARG(1)));
    RET(r ? s + (uint32_t)(r - (const char *)G2H(s)) : 0u);
}

void rt_strpbrk(_cpu *cpu)
{
    uint32_t s = ARG(0);
    const char *r = strpbrk(G2H(s), G2H(ARG(1)));
    RET(r ? s + (uint32_t)(r - (const char *)G2H(s)) : 0u);
}

void rt_strspn(_cpu *cpu) { RET((uint32_t)strspn(G2H(ARG(0)), G2H(ARG(1)))); }
void rt_strcspn(_cpu *cpu) { RET((uint32_t)strcspn(G2H(ARG(0)), G2H(ARG(1)))); }

/* strcoll in the "C" locale compares like strcmp; the Microsoft strcmp
   returns -1, 0 or 1. */
void rt_strcoll(_cpu *cpu)
{
    int r = strcmp(G2H(ARG(0)), G2H(ARG(1)));
    RET((uint32_t)(r < 0 ? -1 : (r > 0 ? 1 : 0)));
}

void rt_strncmp(_cpu *cpu)
{
    const uint8_t *a = G2H(ARG(0)), *b = G2H(ARG(1));
    uint32_t n = ARG(2);
    if (n == 0)
    {
        RET(0);
        return;
    }
    while (--n && *a && *a == *b)
    {
        a++;
        b++;
    }
    RET((uint32_t)((int)*a - (int)*b));
}

void rt_strncpy(_cpu *cpu)
{
    uint8_t *d = G2H(ARG(0));
    const uint8_t *s = G2H(ARG(1));
    uint32_t n = ARG(2);
    while (n && (*d++ = *s++) != 0) n--;
    if (n)
        while (--n) *d++ = 0;
    RET(ARG(0));
}

void rt_strncat(_cpu *cpu)
{
    uint32_t dst = ARG(0);
    char *d = (char *)G2H(dst) + gstrlen(dst);
    const char *s = G2H(ARG(1));
    uint32_t n = ARG(2);
    while (n--)
    {
        if ((*d++ = *s++) == 0)
        {
            RET(dst);
            return;
        }
    }
    *d = 0;
    RET(dst);
}

void rt_strnlen(_cpu *cpu)
{
    RET((uint32_t)strnlen(G2H(ARG(0)), ARG(1)));
}

/* strcpy_s(dst, size, src) */
void rt_strcpy_s(_cpu *cpu)
{
    uint32_t dst = ARG(0), size = ARG(1), src = ARG(2), avail;
    char *p;
    const char *s;
    if (dst == 0 || size == 0) rt_crt_invalid_parameter("strcpy_s");
    p = G2H(dst);
    if (src == 0)
    {
        p[0] = 0;
        rt_crt_invalid_parameter("strcpy_s");
    }
    s = G2H(src);
    avail = size;
    while ((*p++ = *s++) != 0 && --avail > 0) {}
    if (avail == 0)
    {
        ((char *)G2H(dst))[0] = 0;
        rt_crt_invalid_parameter("strcpy_s");
    }
    RET(0);
}

void rt_strcat_s(_cpu *cpu)
{
    uint32_t dst = ARG(0), size = ARG(1), src = ARG(2), avail;
    char *p;
    const char *s;
    if (dst == 0 || size == 0) rt_crt_invalid_parameter("strcat_s");
    p = G2H(dst);
    if (src == 0)
    {
        p[0] = 0;
        rt_crt_invalid_parameter("strcat_s");
    }
    s = G2H(src);
    avail = size;
    while (avail > 0 && *p != 0)
    {
        p++;
        avail--;
    }
    if (avail == 0)
    {
        ((char *)G2H(dst))[0] = 0;
        rt_crt_invalid_parameter("strcat_s");
    }
    while ((*p++ = *s++) != 0 && --avail > 0) {}
    if (avail == 0)
    {
        ((char *)G2H(dst))[0] = 0;
        rt_crt_invalid_parameter("strcat_s");
    }
    RET(0);
}

/* strncpy_s(dst, size, src, count) */
void rt_strncpy_s(_cpu *cpu)
{
    uint32_t dst = ARG(0), size = ARG(1), src = ARG(2), count = ARG(3), avail;
    char *p;
    const char *s;
    if (count == 0 && dst == 0 && size == 0)
    {
        RET(0);
        return;
    }
    if (dst == 0 || size == 0) rt_crt_invalid_parameter("strncpy_s");
    p = G2H(dst);
    if (count == 0)
    {
        p[0] = 0;
        RET(0);
        return;
    }
    if (src == 0)
    {
        p[0] = 0;
        rt_crt_invalid_parameter("strncpy_s");
    }
    s = G2H(src);
    avail = size;
    if (count == TRUNCATE_)
    {
        while ((*p++ = *s++) != 0 && --avail > 0) {}
    }
    else
    {
        while ((*p++ = *s++) != 0 && --avail > 0 && --count > 0) {}
        if (count == 0) *p = 0;
    }
    if (avail == 0)
    {
        if (count == TRUNCATE_)
        {
            ((char *)G2H(dst))[size - 1u] = 0;
            RET(STRUNCATE_);
            return;
        }
        ((char *)G2H(dst))[0] = 0;
        rt_crt_invalid_parameter("strncpy_s");
    }
    RET(0);
}

void rt_wcscpy_s(_cpu *cpu)
{
    uint32_t dst = ARG(0), size = ARG(1), src = ARG(2), avail, i = 0;
    uint16_t c;
    if (dst == 0 || size == 0) rt_crt_invalid_parameter("wcscpy_s");
    if (src == 0)
    {
        wr16(dst, 0);
        rt_crt_invalid_parameter("wcscpy_s");
    }
    avail = size;
    do
    {
        c = rd16(src + 2u * i);
        wr16(dst + 2u * i, c);
        i++;
    } while (c != 0 && --avail > 0);
    if (avail == 0)
    {
        wr16(dst, 0);
        rt_crt_invalid_parameter("wcscpy_s");
    }
    RET(0);
}

void rt_wcscat_s(_cpu *cpu)
{
    uint32_t dst = ARG(0), size = ARG(1), src = ARG(2), avail, i = 0, j = 0;
    uint16_t c;
    if (dst == 0 || size == 0) rt_crt_invalid_parameter("wcscat_s");
    if (src == 0)
    {
        wr16(dst, 0);
        rt_crt_invalid_parameter("wcscat_s");
    }
    avail = size;
    while (avail > 0 && rd16(dst + 2u * i) != 0)
    {
        i++;
        avail--;
    }
    if (avail == 0)
    {
        wr16(dst, 0);
        rt_crt_invalid_parameter("wcscat_s");
    }
    do
    {
        c = rd16(src + 2u * j++);
        wr16(dst + 2u * i++, c);
    } while (c != 0 && --avail > 0);
    if (avail == 0)
    {
        wr16(dst, 0);
        rt_crt_invalid_parameter("wcscat_s");
    }
    RET(0);
}

void rt_wcsstr(_cpu *cpu)
{
    uint32_t s = ARG(0), f = ARG(1);
    size_t n = gwcslen(f), i, k;
    if (n == 0)
    {
        RET(s);
        return;
    }
    for (i = 0; rd16(s + 2u * (uint32_t)i) != 0; i++)
    {
        for (k = 0; k < n && rd16(s + 2u * (uint32_t)(i + k)) == rd16(f + 2u * (uint32_t)k); k++) {}
        if (k == n)
        {
            RET(s + 2u * (uint32_t)i);
            return;
        }
    }
    RET(0);
}

/* wcsrtombs_s(&ret, dst, size, &src, count, state) in the "C" locale:
   characters above 255 cannot be converted. */
void rt_wcsrtombs_s(_cpu *cpu)
{
    uint32_t pret = ARG(0), dst = ARG(1), size = ARG(2), psrc = ARG(3), count = ARG(4);
    uint32_t src, n = 0, i = 0;

    if (pret != 0) wr32(pret, 0xFFFFFFFFu);
    if ((dst == 0 && size > 0) || (dst != 0 && size == 0) || psrc == 0 || rd32(psrc) == 0)
        rt_crt_invalid_parameter("wcsrtombs_s");
    src = rd32(psrc);
    for (;;)
    {
        uint16_t c = rd16(src + 2u * i);
        if (c > 255u)
        {
            if (dst != 0) wr8(dst, 0);
            rt_crt_set_errno(EILSEQ_);
            RET(EILSEQ_);
            return;
        }
        if (dst != 0 && c != 0 && count != TRUNCATE_ && n >= count) break;
        if (dst != 0 && n >= size)
        {
            if (count == TRUNCATE_)
            {
                wr8(dst + size - 1u, 0);
                if (pret != 0) wr32(pret, size);
                RET(STRUNCATE_);
                return;
            }
            wr8(dst, 0);
            rt_crt_invalid_parameter("wcsrtombs_s");
        }
        if (dst != 0) wr8(dst + n, (uint8_t)c);
        n++;
        i++;
        if (c == 0)
        {
            if (dst != 0) wr32(psrc, 0);
            if (pret != 0) wr32(pret, n);
            RET(0);
            return;
        }
    }
    /* count characters were converted: add the terminator. */
    if (n >= size)
    {
        wr8(dst, 0);
        rt_crt_invalid_parameter("wcsrtombs_s");
    }
    wr8(dst + n, 0);
    wr32(psrc, src + 2u * i);
    if (pret != 0) wr32(pret, n + 1u);
    RET(0);
}

/* strtok: the position is per thread. */
void rt_strtok(_cpu *cpu)
{
    rt_thread *t = rt_self();
    uint32_t s = ARG(0), delim = ARG(1), start;
    const char *d = G2H(delim);
    char *p;

    if (s == 0) s = t->crt_strtok;
    if (s == 0)
    {
        RET(0);
        return;
    }
    p = G2H(s);
    p += strspn(p, d);
    if (*p == 0)
    {
        t->crt_strtok = s + (uint32_t)(p - (char *)G2H(s));
        RET(0);
        return;
    }
    start = s + (uint32_t)(p - (char *)G2H(s));
    p += strcspn(p, d);
    if (*p)
    {
        *p = 0;
        t->crt_strtok = s + (uint32_t)(p + 1 - (char *)G2H(s));
    }
    else
    {
        t->crt_strtok = s + (uint32_t)(p - (char *)G2H(s));
    }
    RET(start);
}

/* strerror: the message table of the Microsoft C runtime. */
static const char *const sys_errlist[] = {
    "No error", "Operation not permitted", "No such file or directory", "No such process",
    "Interrupted function call", "Input/output error", "No such device or address", "Arg list too long",
    "Exec format error", "Bad file descriptor", "No child processes", "Resource temporarily unavailable",
    "Not enough space", "Permission denied", "Bad address", "Unknown error", "Resource device", "File exists",
    "Improper link", "No such device", "Not a directory", "Is a directory", "Invalid argument",
    "Too many open files in system", "Too many open files", "Inappropriate I/O control operation",
    "Unknown error", "File too large", "No space left on device", "Invalid seek", "Read-only file system",
    "Too many links", "Broken pipe", "Domain error", "Result too large", "Unknown error",
    "Resource deadlock avoided", "Unknown error", "Filename too long", "No locks available",
    "Function not implemented", "Directory not empty", "Illegal byte sequence", "Unknown error",
};
#define SYS_NERR (sizeof(sys_errlist) / sizeof(sys_errlist[0]))
static uint32_t errlist_guest[SYS_NERR];
static pthread_once_t errlist_once = PTHREAD_ONCE_INIT;

static void init_errlist(void)
{
    size_t i;
    for (i = 0; i < SYS_NERR; i++)
    {
        size_t n = strlen(sys_errlist[i]) + 1;
        errlist_guest[i] = rt_heap_alloc((uint32_t)n);
        if (errlist_guest[i] == 0) rt_fatal("no guest memory");
        memcpy(G2H(errlist_guest[i]), sys_errlist[i], n);
    }
}

void rt_strerror(_cpu *cpu)
{
    uint32_t e = ARG(0);
    if (pthread_once(&errlist_once, init_errlist) != 0) rt_fatal("pthread_once failed");
    RET(errlist_guest[e < SYS_NERR ? e : SYS_NERR - 1u]);
}

/* localeconv: the "C" locale. struct lconv (Microsoft): 10 string pointers,
   then 8 chars. */
static uint32_t lconv_guest;
static pthread_once_t lconv_once = PTHREAD_ONCE_INIT;

static void init_lconv(void)
{
    uint32_t s;
    int i;
    lconv_guest = rt_heap_calloc(1, 48);
    s = rt_heap_calloc(1, 4);
    if (lconv_guest == 0 || s == 0) rt_fatal("no guest memory");
    wr8(s, '.');
    wr32(lconv_guest + 0, s);
    for (i = 1; i < 10; i++) wr32(lconv_guest + 4u * (uint32_t)i, s + 1u); /* "" */
    for (i = 0; i < 8; i++) wr8(lconv_guest + 40u + (uint32_t)i, 127);   /* CHAR_MAX */
}

void rt_localeconv(_cpu *cpu)
{
    if (pthread_once(&lconv_once, init_lconv) != 0) rt_fatal("pthread_once failed");
    RET(lconv_guest);
}

/* ---------------------------------------------------------------- numbers */

/* Parse an integer as strtol/strtoul/_strtoui64. Returns the value
   (clamped on overflow), the end position and the overflow state. */
static uint64_t parse_int(const char *s, const char **end, int base, uint64_t limit_pos, uint64_t limit_neg,
                          bool is_signed, bool *overflow, bool *negative)
{
    const char *p = s, *start;
    uint64_t v = 0;
    bool any = false;
    *overflow = false;
    *negative = false;
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    if (*p == '-') { *negative = true; p++; }
    else if (*p == '+') p++;
    if (base < 0 || base == 1 || base > 36)
    {
        *end = s;
        return 0;
    }
    if (base == 0)
    {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0') base = 8;
        else base = 10;
    }
    else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
    }
    start = p;
    for (;; p++)
    {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        any = true;
        if (!*overflow)
        {
            if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) *overflow = true;
            else v = v * (uint64_t)base + (uint64_t)d;
        }
    }
    if (!any)
    {
        *end = s;
        *negative = false;
        return 0;
    }
    (void)start;
    *end = p;
    if (is_signed)
    {
        if (!*negative && v > limit_pos) *overflow = true;
        if (*negative && v > limit_neg) *overflow = true;
    }
    else if (v > limit_pos)
    {
        *overflow = true;
    }
    return v;
}

static void set_end(uint32_t endp, uint32_t s, const char *host_s, const char *end)
{
    if (endp != 0) wr32(endp, s + (uint32_t)(end - host_s));
}

void rt_strtol(_cpu *cpu)
{
    uint32_t s = ARG(0);
    const char *hs = G2H(s), *end;
    bool ovf, neg;
    uint64_t v = parse_int(hs, &end, (int)ARG(2), 0x7FFFFFFFu, 0x80000000u, true, &ovf, &neg);
    set_end(ARG(1), s, hs, end);
    if (ovf)
    {
        rt_crt_set_errno(ERANGE_);
        RET(neg ? 0x80000000u : 0x7FFFFFFFu);
        return;
    }
    RET(neg ? (uint32_t)(0u - (uint32_t)v) : (uint32_t)v);
}

void rt_strtoul(_cpu *cpu)
{
    uint32_t s = ARG(0);
    const char *hs = G2H(s), *end;
    bool ovf, neg;
    uint64_t v = parse_int(hs, &end, (int)ARG(2), 0xFFFFFFFFu, 0, false, &ovf, &neg);
    set_end(ARG(1), s, hs, end);
    if (ovf)
    {
        rt_crt_set_errno(ERANGE_);
        RET(0xFFFFFFFFu);
        return;
    }
    RET(neg ? (uint32_t)(0u - (uint32_t)v) : (uint32_t)v);
}

void rt__strtoui64(_cpu *cpu)
{
    uint32_t s = ARG(0);
    const char *hs = G2H(s), *end;
    bool ovf, neg;
    uint64_t v = parse_int(hs, &end, (int)ARG(2), UINT64_MAX, 0, false, &ovf, &neg);
    set_end(ARG(1), s, hs, end);
    if (ovf)
    {
        rt_crt_set_errno(ERANGE_);
        RET64(UINT64_MAX);
        return;
    }
    RET64(neg ? 0u - v : v);
}

void rt_atoi(_cpu *cpu)
{
    const char *hs = G2H(ARG(0)), *end;
    bool ovf, neg;
    uint64_t v = parse_int(hs, &end, 10, 0x7FFFFFFFu, 0x80000000u, true, &ovf, &neg);
    if (ovf)
    {
        rt_crt_set_errno(ERANGE_);
        RET(neg ? 0x80000000u : 0x7FFFFFFFu);
        return;
    }
    RET(neg ? (uint32_t)(0u - (uint32_t)v) : (uint32_t)v);
}

void rt__itoa_s(_cpu *cpu)
{
    int32_t value = (int32_t)ARG(0);
    uint32_t buf = ARG(1), size = ARG(2), radix = ARG(3);
    char tmp[40];
    size_t n = 0, i;
    uint32_t u;
    bool neg = false;

    if (buf == 0 || size == 0) rt_crt_invalid_parameter("_itoa_s");
    wr8(buf, 0);
    if (radix < 2 || radix > 36) rt_crt_invalid_parameter("_itoa_s");
    if (radix == 10 && value < 0)
    {
        neg = true;
        u = 0u - (uint32_t)value;
    }
    else
    {
        u = (uint32_t)value;
    }
    do
    {
        uint32_t d = u % radix;
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        u /= radix;
    } while (u != 0);
    if (n + (neg ? 1u : 0u) + 1u > size) rt_crt_invalid_parameter("_itoa_s");
    i = 0;
    if (neg) wr8(buf + (uint32_t)i++, '-');
    while (n > 0) wr8(buf + (uint32_t)i++, (uint8_t)tmp[--n]);
    wr8(buf + (uint32_t)i, 0);
    RET(0);
}

/* Floating point text in the grammar of the Microsoft runtime:
   [whitespace][sign][digits][.digits][{e|E|d|D}[sign]digits].
   Returns the end of the longest valid prefix and the value. */
static locale_t c_locale;
static pthread_once_t c_once = PTHREAD_ONCE_INIT;

static void init_c_locale(void)
{
    c_locale = newlocale(LC_ALL_MASK, "C", NULL);
    if (c_locale == NULL) rt_fatal("the C locale is not available");
}

double rt_crt_parse_double(const char *s, const char **end, bool *range_error)
{
    char *buf;
    locale_t cloc;
    const char *p = s, *num;
    size_t n = 0;
    bool digits = false;
    double v;

    *range_error = false;
    if (pthread_once(&c_once, init_c_locale) != 0) rt_fatal("pthread_once failed");
    cloc = c_locale;
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    num = p;
    if (*p == '+' || *p == '-') p++;
    while (*p >= '0' && *p <= '9') { p++; digits = true; }
    if (*p == '.')
    {
        p++;
        while (*p >= '0' && *p <= '9') { p++; digits = true; }
    }
    if (!digits)
    {
        *end = s;
        return 0.0;
    }
    if (*p == 'e' || *p == 'E' || *p == 'd' || *p == 'D')
    {
        const char *q = p + 1;
        if (*q == '+' || *q == '-') q++;
        if (*q >= '0' && *q <= '9')
        {
            while (*q >= '0' && *q <= '9') q++;
            p = q;
        }
    }
    *end = p;
    buf = malloc((size_t)(p - num) + 1u);
    if (buf == NULL) rt_fatal("out of host memory");
    for (const char *c = num; c < p; c++) buf[n++] = (*c == 'd' || *c == 'D') ? 'e' : *c;
    buf[n] = 0;
    errno = 0;
    v = strtod_l(buf, NULL, cloc);
    if (errno == ERANGE) *range_error = true;
    free(buf);
    return v;
}

void rt_strtod(_cpu *cpu)
{
    uint32_t s = ARG(0);
    const char *hs = G2H(s), *end;
    bool range;
    double v = rt_crt_parse_double(hs, &end, &range);
    set_end(ARG(1), s, hs, end);
    if (range) rt_crt_set_errno(ERANGE_);
    RETF(v);
}

void rt_atof(_cpu *cpu)
{
    const char *end;
    bool range;
    double v = rt_crt_parse_double(G2H(ARG(0)), &end, &range);
    if (range) rt_crt_set_errno(ERANGE_);
    RETF(v);
}

/* ---------------------------------------------------------------- random numbers */

void rt_srand(_cpu *cpu)
{
    rt_self()->crt_rand = ARG(0);
}

void rt_rand(_cpu *cpu)
{
    rt_thread *t = rt_self();
    t->crt_rand = t->crt_rand * 214013u + 2531011u;
    RET((t->crt_rand >> 16) & 0x7FFFu);
}

/* ---------------------------------------------------------------- time */

/* struct tm (Microsoft): 9 ints. */
static void write_tm(uint32_t g, const struct tm *tm)
{
    wr32(g + 0, (uint32_t)tm->tm_sec);
    wr32(g + 4, (uint32_t)tm->tm_min);
    wr32(g + 8, (uint32_t)tm->tm_hour);
    wr32(g + 12, (uint32_t)tm->tm_mday);
    wr32(g + 16, (uint32_t)tm->tm_mon);
    wr32(g + 20, (uint32_t)tm->tm_year);
    wr32(g + 24, (uint32_t)tm->tm_wday);
    wr32(g + 28, (uint32_t)tm->tm_yday);
    wr32(g + 32, (uint32_t)(tm->tm_isdst > 0 ? 1 : 0));
}

/* The 64-bit time functions accept times from 1970-01-01 to 3000-12-31
   23:59:59 UTC. */
#define MAX_TIME64 INT64_C(32535215999)

void rt__gmtime64(_cpu *cpu)
{
    int64_t t = (int64_t)rd64(ARG(0));
    time_t tt = (time_t)t;
    struct tm tm;
    if (t < 0 || t > MAX_TIME64 || gmtime_r(&tt, &tm) == NULL)
    {
        rt_crt_set_errno(EINVAL_);
        RET(0);
        return;
    }
    write_tm(rt_self()->crt_tm, &tm);
    RET(rt_self()->crt_tm);
}

void rt__localtime64(_cpu *cpu)
{
    int64_t t = (int64_t)rd64(ARG(0));
    time_t tt = (time_t)t;
    struct tm tm;
    if (t < 0 || t > MAX_TIME64 || localtime_r(&tt, &tm) == NULL)
    {
        rt_crt_set_errno(EINVAL_);
        RET(0);
        return;
    }
    write_tm(rt_self()->crt_tm, &tm);
    RET(rt_self()->crt_tm);
}

/* _ftime64(struct __timeb64 *): time (8), millitm (2), timezone (2,
   minutes west of UTC), dstflag (2). */
void rt__ftime64(_cpu *cpu)
{
    uint32_t g = ARG(0);
    struct timespec ts;
    struct tm tm;
    long std_west;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) rt_fatal("clock_gettime failed");
    if (localtime_r(&ts.tv_sec, &tm) == NULL) rt_fatal("localtime_r failed");
    tzset();
    /* timezone is the standard time offset; tm_gmtoff includes daylight
       saving time. */
    std_west = -(tm.tm_gmtoff / 60) + (tm.tm_isdst > 0 ? 60 : 0);
    wr64(g + 0, (uint64_t)(int64_t)ts.tv_sec);
    wr16(g + 8, (uint16_t)(ts.tv_nsec / 1000000L));
    wr16(g + 10, (uint16_t)(int16_t)std_west);
    wr16(g + 12, (uint16_t)(tm.tm_isdst > 0 ? 1 : 0));
}
