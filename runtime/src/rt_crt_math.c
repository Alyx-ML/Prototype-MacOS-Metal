/*
 * MSVCR80 math functions.
 *
 * The _CI functions ("compiler intrinsic" forms) take their arguments on
 * the x87 register stack and return the result in st(0). The other
 * functions are __cdecl with double arguments on the stack and the result
 * in st(0). Errors follow the Microsoft runtime: a domain or range error
 * calls the __setusermatherr handler (if set) and otherwise sets errno.
 */
#include "rt_crt.h"
#include "rt_thread.h"

#include <float.h>
#include <math.h>

#define EDOM_ 33u
#define ERANGE_ 34u

enum { M_DOMAIN = 1, M_SING = 2, M_OVERFLOW = 3, M_UNDERFLOW = 4, M_TLOSS = 5 };

/* Guest strings for the function names in struct _exception. */
static uint32_t name_guest(const char *name)
{
    static struct { const char *name; uint32_t g; } cache[16];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    uint32_t g = 0;
    int i;
    if (pthread_mutex_lock(&lock) != 0) rt_fatal("lock failed");
    for (i = 0; i < 16 && cache[i].name != NULL; i++)
        if (strcmp(cache[i].name, name) == 0) g = cache[i].g;
    if (g == 0)
    {
        size_t n = strlen(name) + 1;
        g = rt_heap_alloc((uint32_t)n);
        if (g == 0) rt_fatal("no guest memory");
        memcpy(G2H(g), name, n);
        if (i < 16)
        {
            cache[i].name = name;
            cache[i].g = g;
        }
    }
    if (pthread_mutex_unlock(&lock) != 0) rt_fatal("unlock failed");
    return g;
}

/* Report a math error. Returns the result to use. */
static double math_error(_cpu *cpu, int type, const char *name, double a1, double a2, double ret)
{
    uint32_t handler = rt_crt_matherr_handler();
    if (handler != 0)
    {
        /* struct _exception: int type; char *name; double arg1, arg2, retval
           (offsets 0, 4, 8, 16, 24). */
        uint32_t saved = esp, e;
        esp = (esp - 64u) & ~15u;
        e = esp;
        wr32(e + 0, (uint32_t)type);
        wr32(e + 4, name_guest(name));
        memcpy(G2H(e + 8), &a1, 8);
        memcpy(G2H(e + 16), &a2, 8);
        memcpy(G2H(e + 24), &ret, 8);
        esp = e - 16u;
        uint32_t handled = rt_call_cdecl(cpu, handler, 1, e);
        if (handled != 0) memcpy(&ret, G2H(e + 24), 8);
        esp = saved;
        if (handled != 0) return ret;
    }
    rt_crt_set_errno((type == M_DOMAIN || type == M_SING) ? EDOM_ : ERANGE_);
    return ret;
}

static double ind_nan(void)
{
    /* The x87 "indefinite" NaN: sign set, quiet. */
    return -__builtin_nan("");
}

/* ---------------------------------------------------------------- _CI functions */

void rt__CIsqrt(_cpu *cpu)
{
    double x = rt_fpu_pop(cpu), r;
    if (x < 0.0) r = math_error(cpu, M_DOMAIN, "sqrt", x, 0.0, ind_nan());
    else r = sqrt(x);
    RETF(r);
}

void rt__CIacos(_cpu *cpu)
{
    double x = rt_fpu_pop(cpu), r;
    if (x < -1.0 || x > 1.0) r = math_error(cpu, M_DOMAIN, "acos", x, 0.0, ind_nan());
    else r = acos(x);
    RETF(r);
}

void rt__CIasin(_cpu *cpu)
{
    double x = rt_fpu_pop(cpu), r;
    if (x < -1.0 || x > 1.0) r = math_error(cpu, M_DOMAIN, "asin", x, 0.0, ind_nan());
    else r = asin(x);
    RETF(r);
}

/* The x87 fcos and fsin instructions do not accept |x| >= 2^63: total loss
   of precision. */
void rt__CIcos(_cpu *cpu)
{
    double x = rt_fpu_pop(cpu), r;
    if (isinf(x) || fabs(x) >= 9223372036854775808.0) r = math_error(cpu, M_TLOSS, "cos", x, 0.0, ind_nan());
    else r = cos(x);
    RETF(r);
}

void rt__CIcosh(_cpu *cpu)
{
    double x = rt_fpu_pop(cpu), r = cosh(x);
    if (isinf(r) && !isinf(x)) r = math_error(cpu, M_OVERFLOW, "cosh", x, 0.0, HUGE_VAL);
    RETF(r);
}

void rt__CIsinh(_cpu *cpu)
{
    double x = rt_fpu_pop(cpu), r = sinh(x);
    if (isinf(r) && !isinf(x)) r = math_error(cpu, M_OVERFLOW, "sinh", x, 0.0, x > 0 ? HUGE_VAL : -HUGE_VAL);
    RETF(r);
}

void rt__CItanh(_cpu *cpu)
{
    double x = rt_fpu_pop(cpu);
    RETF(tanh(x));
}

/* _CIfmod and _CIpow: st(1) is the first argument, st(0) the second. */
void rt__CIfmod(_cpu *cpu)
{
    double y = rt_fpu_pop(cpu), x = rt_fpu_pop(cpu), r;
    if (y == 0.0 || isinf(x)) r = math_error(cpu, M_DOMAIN, "fmod", x, y, ind_nan());
    else r = fmod(x, y);
    RETF(r);
}

void rt__CIpow(_cpu *cpu)
{
    double y = rt_fpu_pop(cpu), x = rt_fpu_pop(cpu), r;
    if (isnan(x) || isnan(y))
    {
        r = pow(x, y);
    }
    else if (x == 0.0 && y < 0.0)
    {
        r = math_error(cpu, M_SING, "pow", x, y, HUGE_VAL);
    }
    else if (x < 0.0 && y != floor(y) && !isinf(y))
    {
        r = math_error(cpu, M_DOMAIN, "pow", x, y, ind_nan());
    }
    else
    {
        r = pow(x, y);
        if (isinf(r) && !isinf(x) && !isinf(y)) r = math_error(cpu, M_OVERFLOW, "pow", x, y, r);
        else if (r == 0.0 && x != 0.0 && !isinf(y) && fabs(x) != 1.0) r = math_error(cpu, M_UNDERFLOW, "pow", x, y, r);
    }
    RETF(r);
}

/* ---------------------------------------------------------------- __cdecl functions */

void rt_ceil(_cpu *cpu) { RETF(ceil(ARGF(0))); }
void rt_floor(_cpu *cpu) { RETF(floor(ARGF(0))); }

/* frexp(x, int *exp) */
void rt_frexp(_cpu *cpu)
{
    int e = 0;
    double r = frexp(ARGF(0), &e);
    wr32(ARG(2), (uint32_t)e);
    RETF(r);
}

/* ldexp(x, int exp) */
void rt_ldexp(_cpu *cpu)
{
    double x = ARGF(0), r = ldexp(x, (int)(int32_t)ARG(2));
    if (isinf(r) && !isinf(x)) r = math_error(cpu, M_OVERFLOW, "ldexp", x, (double)(int32_t)ARG(2), r);
    RETF(r);
}

/* modf(x, double *ip) */
void rt_modf(_cpu *cpu)
{
    double ip, r = modf(ARGF(0), &ip);
    memcpy(G2H(ARG(2)), &ip, 8);
    RETF(r);
}

void rt__isnan(_cpu *cpu)
{
    RET(isnan(ARGF(0)) ? 1u : 0u);
}
