/*
 * KERNEL32: virtual memory, the process heap handle, system information,
 * locale and code page functions, FormatMessageA and debug output.
 *
 * The emulated system is Windows XP Service Pack 3 (5.1.2600), English
 * (United States), ANSI code page 1252. The game executable is not large
 * address aware, so memory sizes are limited to 2 GiB as on Windows.
 */
#include "rt.h"
#include "rt_thread.h"

#include <mach/mach.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_TOP_DOWN 0x100000u

#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u

#define ERROR_INVALID_ADDRESS 487u
#define ERROR_INVALID_FLAGS 1004u
#define ERROR_NO_UNICODE_TRANSLATION 1113u
#define ERROR_MR_MID_NOT_FOUND 317u

#define GUEST_PAGE 0x1000u
#define LIMIT_2G 0x7FFFFFFFu

/* ---------------------------------------------------------------- virtual memory */

/* LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect)
   Committed memory is always readable and writable: read-only and
   no-access protection are not enforced (the guest code cannot execute
   data, so execute protections need no action). */
void rt_VirtualAlloc(_cpu *cpu)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2), prot = ARG(3);
    uint32_t base;

    if (prot != PAGE_READWRITE && prot != PAGE_READONLY && prot != PAGE_NOACCESS && prot != PAGE_EXECUTE &&
        prot != PAGE_EXECUTE_READ && prot != PAGE_EXECUTE_READWRITE)
        rt_fatal("VirtualAlloc: protection 0x%x is not supported", prot);
    if ((type & ~(MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN)) != 0 || (type & (MEM_COMMIT | MEM_RESERVE)) == 0)
        rt_fatal("VirtualAlloc: allocation type 0x%x is not supported", type);
    if (size == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }

    if ((type & MEM_RESERVE) || addr == 0)
    {
        /* A reservation (MEM_COMMIT alone with a NULL address reserves
           too). The base is rounded down to 64 KiB; the size covers the
           pages of [addr, addr + size). */
        uint32_t start = addr & ~(RT_GRANULE - 1);
        uint64_t end = ((uint64_t)addr + size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
        if (end - start > 0xFFFFFFFFu)
        {
            rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        base = rt_va_reserve(start, (uint32_t)(end - start));
        if (base == 0)
        {
            rt_set_last_error(addr != 0 ? ERROR_INVALID_ADDRESS : ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        if ((type & MEM_COMMIT) && rt_va_commit(base, (uint32_t)(end - start)) != 0)
            rt_fatal("VirtualAlloc: commit of a new reservation failed");
        RET(base);
        return;
    }

    /* Commit in an existing reservation. */
    {
        uint32_t start = addr & ~(GUEST_PAGE - 1);
        uint64_t end = ((uint64_t)addr + size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
        if (end > RT_WINDOW_SIZE || rt_va_commit(start, (uint32_t)(end - start)) != 0)
        {
            rt_set_last_error(ERROR_INVALID_ADDRESS);
            RET(0);
            return;
        }
        RET(start);
    }
}

/* BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type) */
void rt_VirtualFree(_cpu *cpu)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (type == MEM_RELEASE)
    {
        if (size != 0 || rt_va_release(addr) != 0)
        {
            rt_set_last_error(size != 0 ? ERROR_INVALID_PARAMETER : ERROR_INVALID_ADDRESS);
            RET(0);
            return;
        }
        RET(1);
        return;
    }
    if (type == MEM_DECOMMIT)
    {
        uint32_t start = addr & ~(GUEST_PAGE - 1);
        uint64_t end;
        if (size == 0) rt_fatal("VirtualFree: MEM_DECOMMIT of a whole region (size 0) is not supported");
        end = ((uint64_t)addr + size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
        if (end > RT_WINDOW_SIZE || rt_va_decommit(start, (uint32_t)(end - start)) != 0)
        {
            rt_set_last_error(ERROR_INVALID_ADDRESS);
            RET(0);
            return;
        }
        RET(1);
        return;
    }
    rt_set_last_error(ERROR_INVALID_PARAMETER);
    RET(0);
}

/* ---------------------------------------------------------------- process heap */

/* The process heap handle is the guest address of a reserved block, as on
   Windows (a heap handle is the address of the heap). Allocations of the
   process heap are guest heap blocks. */
static uint32_t process_heap;
static pthread_once_t heap_once = PTHREAD_ONCE_INIT;

static void make_process_heap(void)
{
    process_heap = rt_heap_calloc(1, 0x1000);
    if (process_heap == 0) rt_fatal("no guest memory for the process heap handle");
    wr32(rt_peb + 0x18u, process_heap); /* PEB.ProcessHeap */
}

uint32_t rt_process_heap(void)
{
    if (pthread_once(&heap_once, make_process_heap) != 0) rt_fatal("process heap setup failed");
    return process_heap;
}

void rt_GetProcessHeap(_cpu *cpu)
{
    RET(rt_process_heap());
}

/* BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID mem) */
void rt_HeapFree(_cpu *cpu)
{
    uint32_t heap = ARG(0), mem = ARG(2);
    if (heap != rt_process_heap()) rt_fatal("HeapFree: heap 0x%08x is not the process heap", heap);
    if (mem != 0) rt_heap_free(mem);
    RET(1);
}

/* ---------------------------------------------------------------- system information */

static uint64_t host_total_memory(void)
{
    uint64_t v = 0;
    size_t n = sizeof(v);
    if (sysctlbyname("hw.memsize", &v, &n, NULL, 0) != 0) rt_fatal("sysctl hw.memsize failed");
    return v;
}

static uint64_t host_available_memory(void)
{
    vm_statistics64_data_t st;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) rt_fatal("host_page_size failed");
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&st, &count) != KERN_SUCCESS)
        rt_fatal("host_statistics64 failed");
    return ((uint64_t)st.free_count + st.inactive_count + st.purgeable_count) * page;
}

static uint32_t cap2g(uint64_t v)
{
    return v > LIMIT_2G ? LIMIT_2G : (uint32_t)v;
}

/* void GlobalMemoryStatus(LPMEMORYSTATUS) */
void rt_GlobalMemoryStatus(_cpu *cpu)
{
    uint32_t ms = ARG(0);
    uint64_t total = host_total_memory(), avail = host_available_memory();
    uint64_t vfree = rt_va_free_bytes();
    if (avail > total) avail = total;
    wr32(ms + 0, 32);
    wr32(ms + 4, total != 0 ? (uint32_t)(((total - avail) * 100u) / total) : 0u);
    wr32(ms + 8, cap2g(total));
    wr32(ms + 12, cap2g(avail));
    wr32(ms + 16, cap2g(total));
    wr32(ms + 20, cap2g(avail));
    wr32(ms + 24, 0x7FFE0000u); /* user address space of a 32-bit process */
    wr32(ms + 28, cap2g(vfree));
}

/* BOOL GetVersionExA(LPOSVERSIONINFOA): Windows XP SP3. */
void rt_GetVersionExA(_cpu *cpu)
{
    uint32_t vi = ARG(0), size = rd32(vi);
    static const char sp[] = "Service Pack 3";
    if (size != 148u && size != 156u)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    memset(G2H(vi + 4), 0, size - 4u);
    wr32(vi + 4, 5);    /* dwMajorVersion */
    wr32(vi + 8, 1);    /* dwMinorVersion */
    wr32(vi + 12, 2600); /* dwBuildNumber */
    wr32(vi + 16, 2);   /* VER_PLATFORM_WIN32_NT */
    memcpy(G2H(vi + 20), sp, sizeof(sp));
    if (size == 156u)
    {
        wr16(vi + 148, 3);     /* wServicePackMajor */
        wr16(vi + 150, 0);     /* wServicePackMinor */
        wr16(vi + 152, 0x100); /* wSuiteMask: VER_SUITE_SINGLEUSERTS */
        wr8(vi + 154, 1);      /* wProductType: VER_NT_WORKSTATION */
    }
    RET(1);
}

/* void GetStartupInfoA(LPSTARTUPINFOA): no startup information. */
void rt_GetStartupInfoA(_cpu *cpu)
{
    uint32_t si = ARG(0);
    memset(G2H(si), 0, 68);
    wr32(si, 68);
}

void rt_GetLastError(_cpu *cpu)
{
    RET(rt_get_last_error());
}

void rt_OutputDebugStringA(_cpu *cpu)
{
    uint32_t s = ARG(0);
    if (s != 0) rt_log("debug: %s", (const char *)G2H(s));
}

/* lstrlenA and lstrlenW: NULL gives 0. */
void rt_lstrlenA(_cpu *cpu)
{
    uint32_t s = ARG(0);
    RET(s != 0 ? (uint32_t)strlen(G2H(s)) : 0u);
}

void rt_lstrlenW(_cpu *cpu)
{
    uint32_t s = ARG(0), n = 0;
    if (s != 0)
        while (rd16(s + 2u * n) != 0) n++;
    RET(n);
}

/* ---------------------------------------------------------------- locale */

#define LCID_EN_US 0x0409u

void rt_GetACP(_cpu *cpu) { RET(1252); }
void rt_GetThreadLocale(_cpu *cpu) { RET(LCID_EN_US); }
void rt_GetSystemDefaultLangID(_cpu *cpu) { RET(LCID_EN_US); }

static bool lcid_is_en_us(uint32_t lcid)
{
    /* LOCALE_USER_DEFAULT, LOCALE_SYSTEM_DEFAULT and 0x0409 (with any sort
       identifier 0). */
    return lcid == 0x0400u || lcid == 0x0800u || lcid == LCID_EN_US;
}

static const struct { uint32_t type; const char *value; } locale_en_us[] = {
    { 0x0001, "0409" },                       /* LOCALE_ILANGUAGE */
    { 0x0002, "English (United States)" },    /* LOCALE_SLANGUAGE */
    { 0x0003, "ENU" },                        /* LOCALE_SABBREVLANGNAME */
    { 0x0004, "English" },                    /* LOCALE_SNATIVELANGNAME */
    { 0x0005, "1" },                          /* LOCALE_ICOUNTRY */
    { 0x0006, "United States" },              /* LOCALE_SCOUNTRY */
    { 0x0007, "USA" },                        /* LOCALE_SABBREVCTRYNAME */
    { 0x0008, "United States" },              /* LOCALE_SNATIVECTRYNAME */
    { 0x0009, "0409" },                       /* LOCALE_IDEFAULTLANGUAGE */
    { 0x000A, "1" },                          /* LOCALE_IDEFAULTCOUNTRY */
    { 0x000B, "437" },                        /* LOCALE_IDEFAULTCODEPAGE */
    { 0x000C, "," },                          /* LOCALE_SLIST */
    { 0x000D, "1" },                          /* LOCALE_IMEASURE */
    { 0x000E, "." },                          /* LOCALE_SDECIMAL */
    { 0x000F, "," },                          /* LOCALE_STHOUSAND */
    { 0x0010, "3;0" },                        /* LOCALE_SGROUPING */
    { 0x0011, "2" },                          /* LOCALE_IDIGITS */
    { 0x0012, "1" },                          /* LOCALE_ILZERO */
    { 0x0013, "0123456789" },                 /* LOCALE_SNATIVEDIGITS */
    { 0x0014, "$" },                          /* LOCALE_SCURRENCY */
    { 0x0015, "USD" },                        /* LOCALE_SINTLSYMBOL */
    { 0x0016, "." },                          /* LOCALE_SMONDECIMALSEP */
    { 0x0017, "," },                          /* LOCALE_SMONTHOUSANDSEP */
    { 0x0018, "3;0" },                        /* LOCALE_SMONGROUPING */
    { 0x0019, "2" },                          /* LOCALE_ICURRDIGITS */
    { 0x001B, "0" },                          /* LOCALE_ICURRENCY */
    { 0x001C, "0" },                          /* LOCALE_INEGCURR */
    { 0x001D, "/" },                          /* LOCALE_SDATE */
    { 0x001E, ":" },                          /* LOCALE_STIME */
    { 0x001F, "M/d/yyyy" },                   /* LOCALE_SSHORTDATE */
    { 0x0020, "dddd, MMMM dd, yyyy" },        /* LOCALE_SLONGDATE */
    { 0x0021, "0" },                          /* LOCALE_IDATE */
    { 0x0022, "0" },                          /* LOCALE_ILDATE */
    { 0x0023, "0" },                          /* LOCALE_ITIME */
    { 0x0025, "0" },                          /* LOCALE_ICENTURY */
    { 0x0026, "0" },                          /* LOCALE_ITLZERO */
    { 0x0027, "0" },                          /* LOCALE_IDAYLZERO */
    { 0x0028, "AM" },                         /* LOCALE_S1159 */
    { 0x0029, "PM" },                         /* LOCALE_S2359 */
    { 0x0050, "" },                           /* LOCALE_SPOSITIVESIGN */
    { 0x0051, "-" },                          /* LOCALE_SNEGATIVESIGN */
    { 0x0059, "en" },                         /* LOCALE_SISO639LANGNAME */
    { 0x005A, "US" },                         /* LOCALE_SISO3166CTRYNAME */
    { 0x1001, "English" },                    /* LOCALE_SENGLANGUAGE */
    { 0x1002, "United States" },              /* LOCALE_SENGCOUNTRY */
    { 0x1003, "h:mm:ss tt" },                 /* LOCALE_STIMEFORMAT */
    { 0x1004, "1252" },                       /* LOCALE_IDEFAULTANSICODEPAGE */
    { 0x1007, "en-US" },                      /* LOCALE_SNAME (Vista and later) */
    { 0x100C, "0" },                          /* LOCALE_IFIRSTDAYOFWEEK */
    { 0x100D, "0" },                          /* LOCALE_IFIRSTWEEKOFYEAR */
    { 0x1010, "10000" },                      /* LOCALE_IDEFAULTMACCODEPAGE */
};

/* int GetLocaleInfoA(LCID, LCTYPE, LPSTR buf, int n) */
void rt_GetLocaleInfoA(_cpu *cpu)
{
    uint32_t lcid = ARG(0), type = ARG(1), buf = ARG(2);
    int32_t n = (int32_t)ARG(3);
    bool number = (type & 0x20000000u) != 0; /* LOCALE_RETURN_NUMBER */
    uint32_t t = type & ~(0x80000000u | 0x20000000u | 0x40000000u); /* NOUSEROVERRIDE, USE_CP_ACP */
    const char *v = NULL;
    size_t i, len;

    if (!lcid_is_en_us(lcid)) rt_fatal("GetLocaleInfoA: locale 0x%x is not supported (English, United States only)", lcid);
    for (i = 0; i < sizeof(locale_en_us) / sizeof(locale_en_us[0]); i++)
        if (locale_en_us[i].type == t) v = locale_en_us[i].value;
    if (v == NULL) rt_fatal("GetLocaleInfoA: locale information type 0x%x is not supported", type);
    if (n < 0 || (n > 0 && buf == 0))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (number)
    {
        char *end;
        unsigned long nv = strtoul(v, &end, (t == 0x0001 || t == 0x0009) ? 16 : 10);
        if (*v == 0 || *end != 0) rt_fatal("GetLocaleInfoA: type 0x%x has no number value", t);
        if (n == 0)
        {
            RET(2); /* a DWORD is 2 characters of the buffer */
            return;
        }
        if (n < 2)
        {
            rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
            RET(0);
            return;
        }
        wr32(buf, (uint32_t)nv);
        RET(2);
        return;
    }
    len = strlen(v) + 1;
    if (n == 0)
    {
        RET((uint32_t)len);
        return;
    }
    if ((size_t)n < len)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    memcpy(G2H(buf), v, len);
    RET((uint32_t)len);
}

/* ---------------------------------------------------------------- code pages */

/* Windows-1252 0x80-0x9F (0 = not defined; the unassigned bytes map to the
   same C1 control code point as in Windows: 0x81, 0x8D, 0x8F, 0x90, 0x9D). */
static const uint16_t cp1252_80[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

uint16_t rt_cp1252_to_unicode(uint8_t c)
{
    return (c >= 0x80 && c <= 0x9F) ? cp1252_80[c - 0x80] : c;
}

/* Decode one UTF-8 sequence at s[0..n). Returns the length (>= 1) and the
   code point, or -(length of the invalid sequence) for invalid input. */
static int utf8_decode(const uint8_t *s, size_t n, uint32_t *cp)
{
    uint8_t c = s[0];
    int len, i;
    uint32_t v, min;
    if (c < 0x80)
    {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) { len = 2; v = c & 0x1Fu; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; v = c & 0x0Fu; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; v = c & 0x07u; min = 0x10000; }
    else return -1;
    for (i = 1; i < len; i++)
    {
        if ((size_t)i >= n || (s[i] & 0xC0) != 0x80) return -i;
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return -len;
    *cp = v;
    return len;
}

/* int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int srclen,
   LPWSTR dst, int dstlen). Code pages: CP_ACP (0), CP_THREAD_ACP (3; the
   thread locale is en-US, so its ANSI code page is 1252), 1252 and CP_UTF8.
   UTF-8: invalid sequences are removed (the Windows XP behavior), or the
   call fails with MB_ERR_INVALID_CHARS. */
void rt_MultiByteToWideChar(_cpu *cpu)
{
    uint32_t cp = ARG(0), flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t srclen = (int32_t)ARG(3), dstlen = (int32_t)ARG(5);
    const uint8_t *s;
    size_t n, i = 0;
    uint32_t out = 0;
    bool utf8;

    if (cp == 0u || cp == 3u || cp == 1252u) utf8 = false;
    else if (cp == 65001u) utf8 = true;
    else rt_fatal("MultiByteToWideChar: code page %u is not supported", cp);
    if (src == 0 || srclen == 0 || dstlen < 0 || (dstlen > 0 && dst == 0) || (dst != 0 && dst == src))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (utf8 ? (flags & ~0x8u) != 0 : (flags & ~0x9u) != 0)
    {
        if (!utf8 && (flags & 0x6u) != 0)
            rt_fatal("MultiByteToWideChar: flags 0x%x (MB_COMPOSITE or MB_USEGLYPHCHARS) are not supported", flags);
        rt_set_last_error(ERROR_INVALID_FLAGS);
        RET(0);
        return;
    }
    s = G2H(src);
    n = srclen < 0 ? strlen((const char *)s) + 1 : (size_t)srclen;

    while (i < n)
    {
        uint32_t c;
        uint16_t units[2];
        int nu = 1, k;
        if (utf8)
        {
            int r = utf8_decode(s + i, n - i, &c);
            if (r < 0)
            {
                if (flags & 0x8u)
                {
                    rt_set_last_error(ERROR_NO_UNICODE_TRANSLATION);
                    RET(0);
                    return;
                }
                i += (size_t)(-r);
                continue;
            }
            i += (size_t)r;
        }
        else
        {
            c = rt_cp1252_to_unicode(s[i++]);
        }
        if (c >= 0x10000)
        {
            units[0] = (uint16_t)(0xD800u + ((c - 0x10000u) >> 10));
            units[1] = (uint16_t)(0xDC00u + ((c - 0x10000u) & 0x3FFu));
            nu = 2;
        }
        else
        {
            units[0] = (uint16_t)c;
        }
        for (k = 0; k < nu; k++)
        {
            if (dstlen > 0)
            {
                if (out >= (uint32_t)dstlen)
                {
                    rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
                    RET(0);
                    return;
                }
                wr16(dst + 2u * out, units[k]);
            }
            out++;
        }
    }
    RET(out);
}

/* ---------------------------------------------------------------- FormatMessageA */

#define FM_ALLOCATE_BUFFER 0x100u
#define FM_IGNORE_INSERTS 0x200u
#define FM_FROM_STRING 0x400u
#define FM_FROM_HMODULE 0x800u
#define FM_FROM_SYSTEM 0x1000u
#define FM_ARGUMENT_ARRAY 0x2000u
#define FM_MAX_WIDTH_MASK 0xFFu

/* English system messages of the error codes that the runtime sets. */
static const struct { uint32_t id; const char *text; } system_messages[] = {
    { 0, "The operation completed successfully.\r\n" },
    { 2, "The system cannot find the file specified.\r\n" },
    { 3, "The system cannot find the path specified.\r\n" },
    { 4, "The system cannot open the file.\r\n" },
    { 5, "Access is denied.\r\n" },
    { 6, "The handle is invalid.\r\n" },
    { 8, "Not enough storage is available to process this command.\r\n" },
    { 13, "The data is invalid.\r\n" },
    { 14, "Not enough storage is available to complete this operation.\r\n" },
    { 18, "There are no more files.\r\n" },
    { 19, "The media is write protected.\r\n" },
    { 32, "The process cannot access the file because it is being used by another process.\r\n" },
    { 38, "Reached the end of the file.\r\n" },
    { 50, "The request is not supported.\r\n" },
    { 80, "The file exists.\r\n" },
    { 87, "The parameter is incorrect.\r\n" },
    { 122, "The data area passed to a system call is too small.\r\n" },
    { 126, "The specified module could not be found.\r\n" },
    { 127, "The specified procedure could not be found.\r\n" },
    { 183, "Cannot create a file when that file already exists.\r\n" },
    { 206, "The filename or extension is too long.\r\n" },
    { 234, "More data is available.\r\n" },
    { 259, "No more data is available.\r\n" },
    { 487, "Attempt to access invalid address.\r\n" },
    { 1004, "Invalid flags.\r\n" },
    { 1113, "No mapping for the Unicode character exists in the target multi-byte code page.\r\n" },
};

typedef struct fm_out {
    char *buf;
    size_t len, cap;
} fm_out;

static void fm_put(fm_out *o, char c)
{
    if (o->len + 1 >= o->cap)
    {
        size_t ncap = o->cap ? o->cap * 2 : 256;
        char *nb;
        if (ncap > 0x1000000u) rt_fatal("FormatMessageA: message too long");
        nb = realloc(o->buf, ncap);
        if (nb == NULL) rt_fatal("out of host memory");
        o->buf = nb;
        o->cap = ncap;
    }
    o->buf[o->len++] = c;
    o->buf[o->len] = 0;
}

/* DWORD FormatMessageA(DWORD flags, LPCVOID source, DWORD id, DWORD lang,
   LPSTR buf, DWORD size, va_list *args) */
void rt_FormatMessageA(_cpu *cpu)
{
    uint32_t flags = ARG(0), source = ARG(1), id = ARG(2), lang = ARG(3), buf = ARG(4), size = ARG(5), args = ARG(6);
    uint32_t width = flags & FM_MAX_WIDTH_MASK;
    const char *text = NULL;
    fm_out o = { NULL, 0, 0 };
    size_t i;

    if (flags & FM_FROM_HMODULE) rt_fatal("FormatMessageA: FORMAT_MESSAGE_FROM_HMODULE is not supported");
    if (flags & FM_FROM_STRING)
    {
        if (source == 0)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        text = G2H(source);
    }
    else if (flags & FM_FROM_SYSTEM)
    {
        if (lang != 0 && lang != LCID_EN_US && lang != 0x0400u && lang != 0x0800u)
            rt_fatal("FormatMessageA: language 0x%x is not supported", lang);
        for (i = 0; i < sizeof(system_messages) / sizeof(system_messages[0]); i++)
            if (system_messages[i].id == id) text = system_messages[i].text;
        if (text == NULL)
        {
            rt_log("FormatMessageA: no system message text for code %u", id);
            rt_set_last_error(ERROR_MR_MID_NOT_FOUND);
            RET(0);
            return;
        }
    }
    else
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (width != 0 && width != FM_MAX_WIDTH_MASK) rt_fatal("FormatMessageA: line width %u is not supported", width);

    for (const char *p = text; *p; p++)
    {
        if (width == FM_MAX_WIDTH_MASK && (*p == '\r' || *p == '\n'))
        {
            /* Line breaks become spaces; "\r\n" is one break. */
            if (*p == '\r' && p[1] == '\n') p++;
            fm_put(&o, ' ');
            continue;
        }
        if (*p != '%')
        {
            fm_put(&o, *p);
            continue;
        }
        p++;
        if (*p == 0) break;
        if (flags & FM_IGNORE_INSERTS)
        {
            fm_put(&o, '%');
            fm_put(&o, *p);
            continue;
        }
        if (*p >= '1' && *p <= '9')
        {
            unsigned n = (unsigned)(*p - '0');
            uint32_t a;
            if (p[1] >= '0' && p[1] <= '9') n = n * 10u + (unsigned)(*++p - '0');
            if (p[1] == '!')
            {
                const char *e = strchr(p + 2, '!');
                if (e == NULL || !(e - (p + 2) == 1 && p[2] == 's'))
                    rt_fatal("FormatMessageA: insert formats other than !s! are not supported");
                p = e;
            }
            if (args == 0) rt_fatal("FormatMessageA: insert %%%u without arguments", n);
            /* With FORMAT_MESSAGE_ARGUMENT_ARRAY, args is an array of
               values; otherwise a pointer to a va_list. */
            a = (flags & FM_ARGUMENT_ARRAY) ? rd32(args + 4u * (n - 1u)) : rd32(rd32(args) + 4u * (n - 1u));
            if (a == 0) rt_fatal("FormatMessageA: NULL string insert");
            for (const char *q = G2H(a); *q; q++) fm_put(&o, *q);
            continue;
        }
        switch (*p)
        {
        case '0': goto done; /* end of the message, no new line */
        case 'n': fm_put(&o, '\r'); fm_put(&o, '\n'); break;
        case 'r': fm_put(&o, '\r'); break;
        case 't': fm_put(&o, '\t'); break;
        case 'b': fm_put(&o, ' '); break;
        default: fm_put(&o, *p); break; /* %% %. %! and others: the character */
        }
    }
done:
    if (o.buf == NULL)
    {
        /* An empty message: make the buffer for the terminator. */
        fm_put(&o, ' ');
        o.len = 0;
        o.buf[0] = 0;
    }

    if (flags & FM_ALLOCATE_BUFFER)
    {
        uint32_t need = (uint32_t)o.len + 1u, g;
        if (need < size) need = size;
        g = rt_heap_alloc(need);
        if (g == 0)
        {
            free(o.buf);
            rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        memcpy(G2H(g), o.buf, o.len + 1);
        wr32(buf, g);
    }
    else
    {
        if (o.len + 1 > size)
        {
            free(o.buf);
            rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
            RET(0);
            return;
        }
        memcpy(G2H(buf), o.buf, o.len + 1);
    }
    RET((uint32_t)o.len);
    free(o.buf);
}
