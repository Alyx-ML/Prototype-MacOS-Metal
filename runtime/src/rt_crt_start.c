/*
 * MSVCR80 startup, exit, locks, pointer encoding, errno and floating point
 * control.
 */
#include "rt_crt.h"
#include "rt_thread.h"

#include <stdlib.h>

/* ---------------------------------------------------------------- data imports */

/* The guest code reads these objects through their import address. They
   are in the program image, so they are in the guest window. */
uint32_t _acmdln_asm2c;         /* char *_acmdln */
uint32_t _adjust_fdiv_asm2c;    /* int _adjust_fdiv: 0, no Pentium FDIV defect */
double _HUGE_asm2c = __builtin_inf();

/* ---------------------------------------------------------------- command line */

static uint32_t cmdline;        /* guest string */
static uint32_t g_argc, g_argv, g_envp;
static uint32_t fmode_addr, commode_addr;
static uint32_t app_type;
static uint32_t matherr_handler;

static uint32_t guest_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t g;
    if (n > 0x10000000u) rt_fatal("string too long");
    g = rt_heap_alloc((uint32_t)n);
    if (g == 0) rt_fatal("no guest memory for a string");
    memcpy(G2H(g), s, n);
    return g;
}

/* Split a Windows command line into arguments with the rules of the
   Microsoft C runtime (parse_cmdline in stdargv.c). Returns the number of
   arguments; when out is not NULL, writes NUL-terminated copies. */
static size_t parse_cmdline(const char *cmd, char **out, char *buf)
{
    const char *p = cmd;
    size_t argc = 0, pos = 0;
    bool inquote = false;

    /* Program name: quotes delimit; no backslash rules. */
    if (out) out[argc] = buf + pos;
    argc++;
    if (*p == '"')
    {
        p++;
        while (*p && *p != '"')
        {
            if (buf) buf[pos] = *p;
            pos++;
            p++;
        }
        if (*p == '"') p++;
    }
    else
    {
        while (*p && *p != ' ' && *p != '\t')
        {
            if (buf) buf[pos] = *p;
            pos++;
            p++;
        }
    }
    if (buf) buf[pos] = 0;
    pos++;

    for (;;)
    {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0) break;
        if (out) out[argc] = buf + pos;
        argc++;
        for (;;)
        {
            bool copy = true;
            unsigned slashes = 0;
            while (*p == '\\')
            {
                p++;
                slashes++;
            }
            if (*p == '"')
            {
                if (slashes % 2 == 0)
                {
                    if (inquote && p[1] == '"')
                        p++; /* "" inside quotes is one quote */
                    else
                    {
                        copy = false;
                        inquote = !inquote;
                    }
                }
                slashes /= 2;
            }
            while (slashes--)
            {
                if (buf) buf[pos] = '\\';
                pos++;
            }
            if (*p == 0 || (!inquote && (*p == ' ' || *p == '\t'))) break;
            if (copy)
            {
                if (buf) buf[pos] = *p;
                pos++;
            }
            p++;
        }
        if (buf) buf[pos] = 0;
        pos++;
    }
    return argc;
}

/* The emulated environment. The values agree with the virtual file system
   (rt_file.c). */
static const char *const environment[] = {
    "ALLUSERSPROFILE=C:\\ProgramData",
    "APPDATA=C:\\Users\\Player\\AppData\\Roaming",
    "COMPUTERNAME=MAC",
    "HOMEDRIVE=C:",
    "HOMEPATH=\\Users\\Player",
    "LOCALAPPDATA=C:\\Users\\Player\\AppData\\Local",
    "NUMBER_OF_PROCESSORS=4",
    "OS=Windows_NT",
    "PROCESSOR_ARCHITECTURE=x86",
    "ProgramFiles=C:\\Program Files",
    "SystemDrive=C:",
    "SystemRoot=C:\\Windows",
    "TEMP=C:\\Users\\Player\\AppData\\Local\\Temp",
    "TMP=C:\\Users\\Player\\AppData\\Local\\Temp",
    "USERNAME=Player",
    "USERPROFILE=C:\\Users\\Player",
    "windir=C:\\Windows",
};

void rt_crt_set_command_line(const char *line)
{
    cmdline = guest_strdup(line);
    _acmdln_asm2c = cmdline;
}

static void build_args(void)
{
    const char *cmd = G2H(cmdline);
    size_t argc = parse_cmdline(cmd, NULL, NULL), i, bytes = 0;
    char **ptrs;
    char *tmp;
    uint32_t argv, strings;
    size_t nenv = sizeof(environment) / sizeof(environment[0]);

    /* The parsed strings are never longer than the command line plus one
       terminator for each argument. */
    bytes = strlen(cmd) + argc + 1;
    ptrs = calloc(argc, sizeof(char *));
    tmp = calloc(bytes, 1);
    if (ptrs == NULL || tmp == NULL) rt_fatal("out of host memory for the command line");
    parse_cmdline(cmd, ptrs, tmp);

    argv = rt_heap_calloc((uint32_t)argc + 1u, 4);
    strings = rt_heap_alloc((uint32_t)bytes);
    if (argv == 0 || strings == 0) rt_fatal("no guest memory for the command line");
    memcpy(G2H(strings), tmp, bytes);
    for (i = 0; i < argc; i++) wr32(argv + 4u * (uint32_t)i, strings + (uint32_t)(ptrs[i] - tmp));
    free(ptrs);
    free(tmp);

    g_envp = rt_heap_calloc((uint32_t)nenv + 1u, 4);
    if (g_envp == 0) rt_fatal("no guest memory for the environment");
    for (i = 0; i < nenv; i++) wr32(g_envp + 4u * (uint32_t)i, guest_strdup(environment[i]));
    g_argc = (uint32_t)argc;
    g_argv = argv;
}

/* __getmainargs(&argc, &argv, &envp, dowildcard, &startinfo) */
void rt___getmainargs(_cpu *cpu)
{
    if (g_argv == 0) build_args();
    if (ARG(3) != 0)
    {
        /* Wildcard expansion (setargv.obj) of arguments with * or ?. */
        uint32_t i;
        for (i = 1; i < g_argc; i++)
        {
            const char *a = G2H(rd32(g_argv + 4u * i));
            if (strpbrk(a, "*?") != NULL) rt_fatal("wildcard expansion of command line argument \"%s\" is not supported", a);
        }
    }
    wr32(ARG(0), g_argc);
    wr32(ARG(1), g_argv);
    wr32(ARG(2), g_envp);
    RET(0);
}

void rt___set_app_type(_cpu *cpu)
{
    app_type = ARG(0);
}

/* 1: console program, 2: GUI program (__set_app_type). */
uint32_t rt_crt_app_type(void)
{
    return app_type;
}

static uint32_t crt_int(uint32_t *slot, uint32_t init)
{
    if (*slot == 0)
    {
        *slot = rt_heap_calloc(1, 4);
        if (*slot == 0) rt_fatal("no guest memory");
        wr32(*slot, init);
    }
    return *slot;
}

void rt___p__fmode(_cpu *cpu) { RET(crt_int(&fmode_addr, 0)); }
void rt___p__commode(_cpu *cpu) { RET(crt_int(&commode_addr, 0)); }

uint32_t rt_crt_fmode(void)
{
    return fmode_addr != 0 ? rd32(fmode_addr) : 0u;
}

void rt___setusermatherr(_cpu *cpu)
{
    matherr_handler = ARG(0);
}

uint32_t rt_crt_matherr_handler(void)
{
    return matherr_handler;
}

/* ---------------------------------------------------------------- initializers */

/* _initterm(begin, end): call each non-zero function pointer. */
uint32_t rt__initterm(_cpu *cpu)
{
    uint32_t ret = rd32(esp), p = ARG(0), end = ARG(1);
    esp += 4u; /* the return; __cdecl leaves the arguments to the caller */
    for (; p < end; p += 4u)
    {
        uint32_t fn = rd32(p);
        if (fn != 0) rt_call_cdecl0(cpu, fn);
    }
    return ret;
}

/* _initterm_e(begin, end): stop at the first function that returns
   nonzero, and return that value. */
uint32_t rt__initterm_e(_cpu *cpu)
{
    uint32_t ret = rd32(esp), p = ARG(0), end = ARG(1), r = 0;
    esp += 4u;
    for (; p < end && r == 0; p += 4u)
    {
        uint32_t fn = rd32(p);
        if (fn != 0) r = rt_call_cdecl0(cpu, fn);
    }
    eax = r;
    return ret;
}

/* ---------------------------------------------------------------- exit */

/* Pointer encoding. The C runtime uses EncodePointer when it exists
   (Windows XP SP2 and later) and the plain value otherwise. This runtime
   uses the plain value. */
void rt__encode_pointer(_cpu *cpu) { RET(ARG(0)); }
void rt__decode_pointer(_cpu *cpu) { RET(ARG(0)); }
void rt__encoded_null(_cpu *cpu) { RET(0); }

#define LOCK_COUNT 36u
#define EXIT_LOCK1 8u

static pthread_mutex_t crt_locks[LOCK_COUNT];
static pthread_once_t crt_locks_once = PTHREAD_ONCE_INIT;

static void init_crt_locks(void)
{
    pthread_mutexattr_t a;
    uint32_t i;
    if (pthread_mutexattr_init(&a) != 0) rt_fatal("pthread_mutexattr_init failed");
    if (pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE) != 0) rt_fatal("pthread_mutexattr_settype failed");
    for (i = 0; i < LOCK_COUNT; i++)
        if (pthread_mutex_init(&crt_locks[i], &a) != 0) rt_fatal("pthread_mutex_init failed");
    pthread_mutexattr_destroy(&a);
}

void rt_crt_lock(uint32_t n)
{
    if (pthread_once(&crt_locks_once, init_crt_locks) != 0) rt_fatal("pthread_once failed");
    if (n >= LOCK_COUNT) rt_fatal("_lock(%u): bad lock number", n);
    if (pthread_mutex_lock(&crt_locks[n]) != 0) rt_fatal("_lock failed");
}

void rt_crt_unlock(uint32_t n)
{
    if (n >= LOCK_COUNT) rt_fatal("_unlock(%u): bad lock number", n);
    if (pthread_mutex_unlock(&crt_locks[n]) != 0) rt_fatal("_unlock failed");
}

void rt__lock(_cpu *cpu) { rt_crt_lock(ARG(0)); }
void rt__unlock(_cpu *cpu) { rt_crt_unlock(ARG(0)); }

/* The process atexit table (the program's _onexit and atexit). */
static uint32_t *onexit_table;
static size_t onexit_count, onexit_cap;
static bool onexit_done;

void rt__onexit(_cpu *cpu)
{
    uint32_t fn = ARG(0);
    rt_crt_lock(EXIT_LOCK1);
    if (onexit_count == onexit_cap)
    {
        size_t cap = onexit_cap ? onexit_cap * 2u : 32u;
        uint32_t *n = realloc(onexit_table, cap * sizeof(uint32_t));
        if (n == NULL)
        {
            rt_crt_unlock(EXIT_LOCK1);
            RET(0);
            return;
        }
        onexit_table = n;
        onexit_cap = cap;
    }
    onexit_table[onexit_count++] = fn;
    rt_crt_unlock(EXIT_LOCK1);
    RET(fn);
}

/* __dllonexit(fn, &begin, &end): add fn to the table of a DLL. The table is
   a guest heap block; begin and end point into it. */
void rt___dllonexit(_cpu *cpu)
{
    uint32_t fn = ARG(0), pbegin = ARG(1), pend = ARG(2);
    uint32_t begin, end, size, used;

    rt_crt_lock(EXIT_LOCK1);
    begin = rd32(pbegin);
    end = rd32(pend);
    if (end < begin || begin == 0)
    {
        rt_crt_unlock(EXIT_LOCK1);
        RET(0);
        return;
    }
    used = end - begin;
    size = rt_heap_size(begin);
    if (size < used + 4u)
    {
        uint32_t grow = size > 2048u ? 2048u : size;
        uint32_t nb = rt_heap_realloc(begin, size + (grow < 16u ? 16u : grow));
        if (nb == 0)
        {
            nb = rt_heap_realloc(begin, used + 16u);
            if (nb == 0)
            {
                rt_crt_unlock(EXIT_LOCK1);
                RET(0);
                return;
            }
        }
        begin = nb;
        end = begin + used;
    }
    wr32(end, fn);
    end += 4u;
    wr32(pbegin, begin);
    wr32(pend, end);
    rt_crt_unlock(EXIT_LOCK1);
    RET(fn);
}

static void run_onexit(_cpu *cpu)
{
    rt_crt_lock(EXIT_LOCK1);
    if (!onexit_done)
    {
        onexit_done = true;
        while (onexit_count > 0)
        {
            uint32_t fn = onexit_table[--onexit_count];
            if (fn != 0) rt_call_cdecl0(cpu, fn);
        }
    }
    rt_crt_unlock(EXIT_LOCK1);
}

void rt_exit(_cpu *cpu)
{
    uint32_t code = ARG(0);
    run_onexit(cpu);
    rt_crt_flush_all();
    rt_process_exit(code);
}

void rt__exit(_cpu *cpu)
{
    rt_process_exit(ARG(0));
}

void rt__cexit(_cpu *cpu)
{
    run_onexit(cpu);
    rt_crt_flush_all();
}

/* ---------------------------------------------------------------- errno, locale */

void rt__errno(_cpu *cpu)
{
    RET(rt_self()->crt_errno_addr);
}

void rt_crt_set_errno(uint32_t e)
{
    wr32(rt_self()->crt_errno_addr, e);
}

/* _configthreadlocale: per-thread locale mode. Only the "C" locale exists,
   so the mode has no other effect. */
static _Thread_local uint32_t locale_mode = 2u; /* _DISABLE_PER_THREAD_LOCALE */

void rt__configthreadlocale(_cpu *cpu)
{
    int32_t mode = (int32_t)ARG(0);
    uint32_t prev = locale_mode;
    if (mode == 1 || mode == 2)
        locale_mode = (uint32_t)mode;
    else if (mode == -1)
        locale_mode = 2u;
    else if (mode != 0)
    {
        rt_crt_set_errno(22); /* EINVAL */
        RET(0xFFFFFFFFu);
        return;
    }
    RET(prev);
}

void rt__malloc_crt(_cpu *cpu)
{
    RET(rt_heap_alloc(ARG(0)));
}

/* ---------------------------------------------------------------- floating point control */

/* Abstract control word of the C runtime. */
#define EM_INEXACT 0x00000001u
#define EM_UNDERFLOW 0x00000002u
#define EM_OVERFLOW 0x00000004u
#define EM_ZERODIVIDE 0x00000008u
#define EM_INVALID 0x00000010u
#define EM_DENORMAL 0x00080000u
#define MCW_EM 0x0008001Fu
#define MCW_IC 0x00040000u
#define MCW_RC 0x00000300u
#define MCW_PC 0x00030000u
#define MCW_DN 0x03000000u

static uint32_t x87_to_abstract(uint32_t cw)
{
    uint32_t a = 0;
    if (cw & 0x01u) a |= EM_INVALID;
    if (cw & 0x02u) a |= EM_DENORMAL;
    if (cw & 0x04u) a |= EM_ZERODIVIDE;
    if (cw & 0x08u) a |= EM_OVERFLOW;
    if (cw & 0x10u) a |= EM_UNDERFLOW;
    if (cw & 0x20u) a |= EM_INEXACT;
    a |= ((cw >> 10) & 3u) << 8;          /* rounding: same encoding */
    switch ((cw >> 8) & 3u)
    {
    case 0: a |= 0x00020000u; break;      /* _PC_24 */
    case 2: a |= 0x00010000u; break;      /* _PC_53 */
    default: break;                       /* _PC_64 */
    }
    if (cw & 0x1000u) a |= 0x00040000u;   /* _IC_AFFINE */
    return a;
}

static uint32_t abstract_to_x87(uint32_t a, uint32_t cw)
{
    cw &= ~0x1F3Fu;
    if (a & EM_INVALID) cw |= 0x01u;
    if (a & EM_DENORMAL) cw |= 0x02u;
    if (a & EM_ZERODIVIDE) cw |= 0x04u;
    if (a & EM_OVERFLOW) cw |= 0x08u;
    if (a & EM_UNDERFLOW) cw |= 0x10u;
    if (a & EM_INEXACT) cw |= 0x20u;
    cw |= ((a >> 8) & 3u) << 10;
    switch (a & MCW_PC)
    {
    case 0x00020000u: break;              /* 24 bits: 00 */
    case 0x00010000u: cw |= 0x200u; break;
    default: cw |= 0x300u; break;
    }
    if (a & 0x00040000u) cw |= 0x1000u;
    return cw;
}

static uint32_t mxcsr_to_abstract(uint32_t m)
{
    uint32_t a = 0;
    if (m & 0x0080u) a |= EM_INVALID;
    if (m & 0x0100u) a |= EM_DENORMAL;
    if (m & 0x0200u) a |= EM_ZERODIVIDE;
    if (m & 0x0400u) a |= EM_OVERFLOW;
    if (m & 0x0800u) a |= EM_UNDERFLOW;
    if (m & 0x1000u) a |= EM_INEXACT;
    a |= ((m >> 13) & 3u) << 8;
    if ((m & 0x8040u) == 0x8040u) a |= 0x01000000u; /* _DN_FLUSH */
    else if (m & 0x8000u) a |= 0x03000000u;         /* _DN_FLUSH_OPERANDS_SAVE_RESULTS */
    else if (m & 0x0040u) a |= 0x02000000u;         /* _DN_SAVE_OPERANDS_FLUSH_RESULTS */
    return a;
}

static uint32_t abstract_to_mxcsr(uint32_t a, uint32_t m)
{
    m &= ~0xFFC0u;
    if (a & EM_INVALID) m |= 0x0080u;
    if (a & EM_DENORMAL) m |= 0x0100u;
    if (a & EM_ZERODIVIDE) m |= 0x0200u;
    if (a & EM_OVERFLOW) m |= 0x0400u;
    if (a & EM_UNDERFLOW) m |= 0x0800u;
    if (a & EM_INEXACT) m |= 0x1000u;
    m |= ((a >> 8) & 3u) << 13;
    switch (a & MCW_DN)
    {
    case 0x01000000u: m |= 0x8040u; break;
    case 0x02000000u: m |= 0x0040u; break;
    case 0x03000000u: m |= 0x8000u; break;
    default: break;
    }
    return m;
}

/* _controlfp_s(&current, new, mask). On x86 it changes the x87 control
   word and the SSE control register. The translated code computes x87
   operations in double precision and SSE operations with the host
   defaults; the stored control words are the values that the guest reads
   back. */
void rt__controlfp_s(_cpu *cpu)
{
    uint32_t cur_ptr = ARG(0), nv = ARG(1), mask = ARG(2);
    uint32_t valid = MCW_EM | MCW_IC | MCW_RC | MCW_PC | MCW_DN;
    mask &= ~EM_DENORMAL;
    if ((mask & ~valid) != 0 || ((nv & mask) & ~valid) != 0)
    {
        if (cur_ptr != 0) wr32(cur_ptr, x87_to_abstract(cpu->_st_cw));
        rt_crt_set_errno(22);
        RET(22);
        return;
    }
    if (mask != 0)
    {
        uint32_t a = (x87_to_abstract(cpu->_st_cw) & ~mask) | (nv & mask);
        uint32_t am = (mxcsr_to_abstract(cpu->_mxcsr) & ~mask) | (nv & mask);
        cpu->_st_cw = abstract_to_x87(a, cpu->_st_cw);
        cpu->_mxcsr = abstract_to_mxcsr(am, cpu->_mxcsr);
    }
    if (cur_ptr != 0) wr32(cur_ptr, x87_to_abstract(cpu->_st_cw) | (mxcsr_to_abstract(cpu->_mxcsr) & MCW_DN));
    RET(0);
}
