/*
 * KERNEL32 module functions: LoadLibraryA, FreeLibrary, GetModuleHandleA,
 * GetModuleFileNameA, GetProcAddress, and process exit.
 *
 * Modules:
 *   prototypef.exe        the translated program; the handle is the guest
 *                         address of its image header copy.
 *   prototypeenginef.dll  the translated DLL; LoadLibraryA calls its entry
 *                         point (DllMain) with DLL_PROCESS_ATTACH.
 *   system DLLs           the DLLs of imports.spec. They are always loaded.
 *                         GetProcAddress finds the functions that the
 *                         runtime implements. The handle is the guest
 *                         address of a minimal image header.
 * Every other DLL name gives ERROR_MOD_NOT_FOUND: the DLL is not in the
 * emulated system.
 */
#include "rt_module.h"
#include "rt_file.h"
#include "rt_thread.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DLL_PROCESS_DETACH 0u
#define DLL_PROCESS_ATTACH 1u
#define DLL_THREAD_ATTACH 2u
#define DLL_THREAD_DETACH 3u

#define MAX_NAME 64

typedef enum { MOD_EXE, MOD_DLL, MOD_SYSTEM } mod_kind;

typedef struct rt_module {
    char name[MAX_NAME];        /* lower case file name, for example "kernel32.dll" */
    char key[MAX_NAME];         /* imports.spec DLL name, for example "kernel32" */
    mod_kind kind;
    uint32_t base;              /* HMODULE */
    uint32_t entry;             /* guest address of DllMain, 0 if none */
    const rt_export_entry *exports;
    unsigned nexports;
    int refs;                   /* LoadLibrary references (translated DLL) */
    bool attached;              /* DLL_PROCESS_ATTACH done */
} rt_module;

#define MAX_MODULES 32

static pthread_mutex_t loader_lock;
static rt_module modules[MAX_MODULES];
static int nmodules;
static int load_order[MAX_MODULES];
static int nloaded;
static atomic_bool exiting;

static void loader_enter(void)
{
    if (pthread_mutex_lock(&loader_lock) != 0) rt_fatal("loader lock failed");
}

static void loader_leave(void)
{
    if (pthread_mutex_unlock(&loader_lock) != 0) rt_fatal("loader unlock failed");
}

/* A minimal PE header for a system DLL: no sections, no directories. */
static uint32_t make_system_image(void)
{
    uint32_t base = rt_va_reserve(0, RT_GRANULE), pe;
    if (base == 0 || rt_va_commit(base, 0x1000) != 0) rt_fatal("no guest memory for a system module header");
    pe = base + 0x40u;
    wr16(base + 0, 0x5A4D);       /* "MZ" */
    wr32(base + 0x3C, 0x40);      /* e_lfanew */
    wr32(pe + 0, 0x00004550);     /* "PE\0\0" */
    wr16(pe + 4, 0x014C);         /* i386 */
    wr16(pe + 6, 0);              /* NumberOfSections */
    wr16(pe + 20, 0xE0);          /* SizeOfOptionalHeader */
    wr16(pe + 22, 0x2102);        /* executable, 32-bit, DLL */
    wr16(pe + 24, 0x010B);        /* PE32 */
    wr32(pe + 24 + 28, base);     /* ImageBase */
    wr32(pe + 24 + 32, 0x1000);   /* SectionAlignment */
    wr32(pe + 24 + 36, 0x200);    /* FileAlignment */
    wr32(pe + 24 + 56, 0x1000);   /* SizeOfImage */
    wr32(pe + 24 + 60, 0x400);    /* SizeOfHeaders */
    wr32(pe + 24 + 92, 16);       /* NumberOfRvaAndSizes */
    return base;
}

static rt_module *add_module(const char *name, const char *key, mod_kind kind)
{
    rt_module *m;
    if (nmodules >= MAX_MODULES) rt_fatal("too many modules");
    m = &modules[nmodules++];
    if ((size_t)snprintf(m->name, sizeof(m->name), "%s", name) >= sizeof(m->name)) rt_fatal("module name too long");
    if ((size_t)snprintf(m->key, sizeof(m->key), "%s", key) >= sizeof(m->key)) rt_fatal("module key too long");
    m->kind = kind;
    return m;
}

/* ---------------------------------------------------------------- exports of the translated DLL */

/* Upper limits for the export directory of the DLL file. */
#define MAX_PE_FILE (256u << 20)
#define MAX_EXPORTS 65536u
#define MAX_EXPORT_NAME 1024u
#define MAX_SECTIONS 96u

typedef struct pe_view {
    const uint8_t *d;
    size_t n;
    unsigned nsec;
    struct { uint32_t va, size, ptr; } sec[MAX_SECTIONS];
} pe_view;

static bool pe_u16(const pe_view *p, size_t off, uint16_t *v)
{
    if (off > p->n || p->n - off < 2) return false;
    memcpy(v, p->d + off, 2);
    return true;
}

static bool pe_u32(const pe_view *p, size_t off, uint32_t *v)
{
    if (off > p->n || p->n - off < 4) return false;
    memcpy(v, p->d + off, 4);
    return true;
}

/* The file offset of an RVA, as in tools/gen_exports.py: the section that
   holds the RVA (virtual size or raw size, the larger). */
static bool pe_offset(const pe_view *p, uint32_t rva, size_t *off)
{
    unsigned i;
    for (i = 0; i < p->nsec; i++)
    {
        uint32_t va = p->sec[i].va;
        if (rva >= va && rva - va < p->sec[i].size)
        {
            uint64_t o = (uint64_t)p->sec[i].ptr + (rva - va);
            if (o >= p->n) return false;
            *off = (size_t)o;
            return true;
        }
    }
    return false;
}

/* The symbol that SRW makes for an export name (SymbolName in
   SRW/SRW_loader.c, tools/importspec.py symbol_name). */
static bool export_symbol(const char *name, char *out, size_t n)
{
    const unsigned char *s;
    size_t len = 0;
    bool plain = true;
    for (s = (const unsigned char *)name; *s; s++)
        if (!(isascii(*s) && (isalnum(*s) || *s == '_'))) plain = false;
    if (plain)
    {
        if (strncmp(name, "X_", 2) == 0) rt_fatal("the export %s starts with the reserved prefix X_", name);
        return (size_t)snprintf(out, n, "%s", name) < n;
    }
    if (n < 3) return false;
    out[len++] = 'X';
    out[len++] = '_';
    for (s = (const unsigned char *)name; *s; s++)
    {
        char tmp[5];
        int w;
        if (isascii(*s) && isalnum(*s))
            w = snprintf(tmp, sizeof tmp, "%c", *s);
        else if (*s == '_')
            w = snprintf(tmp, sizeof tmp, "__");
        else if (*s == '?')
            w = snprintf(tmp, sizeof tmp, "_Q");
        else if (*s == '@')
            w = snprintf(tmp, sizeof tmp, "_A");
        else if (*s == '$')
            w = snprintf(tmp, sizeof tmp, "_D");
        else
            w = snprintf(tmp, sizeof tmp, "_x%02x", *s);
        if (w <= 0 || (size_t)w >= n - len) return false;
        memcpy(out + len, tmp, (size_t)w);
        len += (size_t)w;
    }
    out[len] = 0;
    return true;
}

static uint8_t *read_host_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long end;
    if (f == NULL) rt_fatal("cannot open %s: %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
        rt_fatal("cannot get the size of %s", path);
    if ((unsigned long)end > MAX_PE_FILE) rt_fatal("%s is larger than %u bytes", path, MAX_PE_FILE);
    buf = malloc(end > 0 ? (size_t)end : 1);
    if (buf == NULL) rt_fatal("out of host memory");
    if (fread(buf, 1, (size_t)end, f) != (size_t)end) rt_fatal("read of %s failed", path);
    fclose(f);
    *size = (size_t)end;
    return buf;
}

/* Make the export table of the translated DLL. The export directory of
   the DLL file gives the names and the ordinals. dlsym finds the symbol of
   each export in the image that holds the runtime (the translated code is
   in the same image). Forwarded exports and exports without a name are not
   supported, as in tools/gen_exports.py. */
static void load_exports(rt_module *m, const char *dll_path)
{
    pe_view p;
    uint8_t *file;
    uint32_t pe, exp_rva, exp_size, base, nfunc, nname, afunc, aname, aord, idx;
    uint16_t nsec, opt_size, magic;
    size_t opt, e, off;
    const char **names;
    rt_export_entry *table;
    unsigned i, count = 0;

    memset(&p, 0, sizeof p);
    file = read_host_file(dll_path, &p.n);
    p.d = file;
    if (p.n < 0x40 || file[0] != 'M' || file[1] != 'Z' || !pe_u32(&p, 0x3C, &pe) || !pe_u32(&p, pe, &exp_rva) ||
        exp_rva != 0x00004550u)
        rt_fatal("%s is not a PE file", dll_path);
    if (!pe_u16(&p, (size_t)pe + 6, &nsec) || !pe_u16(&p, (size_t)pe + 20, &opt_size)) rt_fatal("%s: bad PE header", dll_path);
    opt = (size_t)pe + 24;
    if (!pe_u16(&p, opt, &magic) || magic != 0x10B) rt_fatal("%s is not a 32-bit PE file", dll_path);
    if (nsec > MAX_SECTIONS) rt_fatal("%s has %u sections", dll_path, nsec);
    for (i = 0; i < nsec; i++)
    {
        size_t o = opt + opt_size + 40u * i;
        uint32_t vsize, rsize;
        if (!pe_u32(&p, o + 8, &vsize) || !pe_u32(&p, o + 12, &p.sec[i].va) || !pe_u32(&p, o + 16, &rsize) ||
            !pe_u32(&p, o + 20, &p.sec[i].ptr))
            rt_fatal("%s: bad section table", dll_path);
        p.sec[i].size = vsize > rsize ? vsize : rsize;
    }
    p.nsec = nsec;
    if (!pe_u32(&p, opt + 96, &exp_rva) || !pe_u32(&p, opt + 100, &exp_size)) rt_fatal("%s: bad data directory", dll_path);
    if (exp_rva == 0)
    {
        free(file);
        return;
    }
    if (!pe_offset(&p, exp_rva, &e) || !pe_u32(&p, e + 16, &base) || !pe_u32(&p, e + 20, &nfunc) ||
        !pe_u32(&p, e + 24, &nname) || !pe_u32(&p, e + 28, &afunc) || !pe_u32(&p, e + 32, &aname) ||
        !pe_u32(&p, e + 36, &aord))
        rt_fatal("%s: bad export directory", dll_path);
    if (nfunc > MAX_EXPORTS || nname > nfunc) rt_fatal("%s: %u exports with %u names is not supported", dll_path, nfunc, nname);
    names = calloc(nfunc > 0 ? nfunc : 1, sizeof *names);
    table = calloc(nfunc > 0 ? nfunc : 1, sizeof *table);
    if (names == NULL || table == NULL) rt_fatal("out of host memory");
    for (i = 0; i < nname; i++)
    {
        uint32_t nrva;
        uint16_t ord;
        size_t name_off, len;
        if (!pe_offset(&p, aname, &off) || !pe_u32(&p, off + 4u * i, &nrva) || !pe_offset(&p, aord, &off) ||
            !pe_u16(&p, off + 2u * i, &ord) || !pe_offset(&p, nrva, &name_off))
            rt_fatal("%s: bad export name table", dll_path);
        if (ord >= nfunc) rt_fatal("%s: export name %u has the bad index %u", dll_path, i, ord);
        len = strnlen((const char *)file + name_off, p.n - name_off);
        if (len == p.n - name_off || len > MAX_EXPORT_NAME) rt_fatal("%s: bad export name at RVA 0x%x", dll_path, nrva);
        names[ord] = (const char *)file + name_off;
    }
    for (idx = 0; idx < nfunc; idx++)
    {
        uint32_t frva;
        char sym[4 * MAX_EXPORT_NAME + 3];
        char *name;
        void *addr;
        if (!pe_offset(&p, afunc, &off) || !pe_u32(&p, off + 4u * idx, &frva)) rt_fatal("%s: bad export address table", dll_path);
        if (frva == 0) continue;
        if (frva >= exp_rva && frva - exp_rva < exp_size)
            rt_fatal("%s: the forwarded export at ordinal %u is not supported", dll_path, base + idx);
        if (names[idx] == NULL) rt_fatal("%s: the export without a name at ordinal %u is not supported", dll_path, base + idx);
        if (base > UINT32_MAX - idx) rt_fatal("%s: bad export ordinal base %u", dll_path, base);
        if (!export_symbol(names[idx], sym, sizeof sym)) rt_fatal("the symbol name of the export %s is too long", names[idx]);
        addr = dlsym(RTLD_SELF, sym);
        if (addr == NULL) rt_fatal("the export %s of %s (symbol %s) is not in the translated code", names[idx], m->name, sym);
        name = strdup(names[idx]);
        if (name == NULL) rt_fatal("out of host memory");
        table[count].name = name;
        table[count].ordinal = base + idx;
        table[count].addr = (char *)addr;
        count++;
    }
    free(names);
    free(file);
    m->exports = table;
    m->nexports = count;
}

void rt_modules_init(const char *game_dir)
{
    char dll_path[RT_HOST_PATH];
    pthread_mutexattr_t a;
    unsigned i;
    rt_module *m;

    if (pthread_mutexattr_init(&a) != 0) rt_fatal("pthread_mutexattr_init failed");
    if (pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE) != 0) rt_fatal("pthread_mutexattr_settype failed");
    if (pthread_mutex_init(&loader_lock, &a) != 0) rt_fatal("pthread_mutex_init failed");
    pthread_mutexattr_destroy(&a);

    m = add_module("prototypef.exe", "prototypef", MOD_EXE);
    m->base = H2G(launcher_image);
    m->attached = true;

    m = add_module("prototypeenginef.dll", "prototypeenginef", MOD_DLL);
    m->base = H2G(engine_image);
    m->entry = H2G((const void *)&engine_entry);
    if ((size_t)snprintf(dll_path, sizeof dll_path, "%s/%s", game_dir, m->name) >= sizeof dll_path)
        rt_fatal("the game directory path is too long");
    load_exports(m, dll_path);

    /* System DLLs: every DLL of the import table, and ntdll.dll (always
       loaded on Windows). */
    for (i = 0; i < rt_import_table_count; i++)
    {
        const char *dll = rt_import_table[i].dll;
        char name[MAX_NAME];
        int j;
        bool found = false;
        for (j = 0; j < nmodules && !found; j++) found = strcmp(modules[j].key, dll) == 0;
        if (found) continue;
        if ((size_t)snprintf(name, sizeof(name), "%s.dll", dll) >= sizeof(name)) rt_fatal("DLL name too long");
        m = add_module(name, dll, MOD_SYSTEM);
        m->base = make_system_image();
        m->attached = true;
    }
    m = add_module("ntdll.dll", "ntdll", MOD_SYSTEM);
    m->base = make_system_image();
    m->attached = true;
}

uint32_t rt_exe_module(void)
{
    return modules[0].base;
}

/* Normalize a DLL name: the file name part, lower case, ".dll" added when
   the name has no extension. Returns false if the name is too long. */
static bool normalize(const char *in, char *out, size_t n)
{
    const char *p = in, *s;
    size_t len, i;
    bool has_dot = false;
    for (s = in; *s; s++)
    {
        if (*s == '\\' || *s == '/' || *s == ':') p = s + 1;
    }
    len = strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '.'))
    {
        /* A trailing dot means "no extension"; Windows removes it. */
        if (p[len - 1] == '.') has_dot = true;
        len--;
    }
    if (len + 5 > n) return false;
    for (i = 0; i < len; i++)
    {
        out[i] = (char)tolower((unsigned char)p[i]);
        if (p[i] == '.') has_dot = true;
    }
    out[len] = 0;
    if (!has_dot) memcpy(out + len, ".dll", 5);
    return true;
}

static rt_module *find_by_name(const char *norm)
{
    int i;
    for (i = 0; i < nmodules; i++)
        if (strcmp(modules[i].name, norm) == 0) return &modules[i];
    return NULL;
}

static rt_module *find_by_handle(uint32_t h)
{
    int i;
    if (h == 0) return &modules[0];
    for (i = 0; i < nmodules; i++)
        if (modules[i].base == h) return &modules[i];
    return NULL;
}

/* Is the translated DLL loaded? */
static bool is_loaded(const rt_module *m)
{
    return m->kind != MOD_DLL || m->refs > 0;
}

void rt_LoadLibraryA(_cpu *cpu)
{
    char buf[260], norm[MAX_NAME];
    rt_module *m;

    if (ARG(0) == 0 || rt_gstr(buf, sizeof(buf), ARG(0)) != 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (!normalize(buf, norm, sizeof(norm)) || (m = find_by_name(norm)) == NULL || m->kind == MOD_EXE)
    {
        rt_log("LoadLibraryA(\"%s\"): the DLL is not in the emulated system", buf);
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
        RET(0);
        return;
    }
    loader_enter();
    if (m->kind == MOD_DLL)
    {
        if (m->refs == INT32_MAX) rt_fatal("too many references to %s", m->name);
        m->refs++;
        if (!m->attached)
        {
            uint32_t ok;
            m->attached = true;
            load_order[nloaded++] = (int)(m - modules);
            ok = rt_call_std(cpu, m->entry, 3, m->base, DLL_PROCESS_ATTACH, 0);
            if (ok == 0)
            {
                /* DllMain failed: Windows calls it with DLL_PROCESS_DETACH
                   and unloads the DLL. */
                rt_call_std(cpu, m->entry, 3, m->base, DLL_PROCESS_DETACH, 0);
                m->attached = false;
                m->refs = 0;
                nloaded--;
                loader_leave();
                rt_set_last_error(1114u); /* ERROR_DLL_INIT_FAILED */
                RET(0);
                return;
            }
        }
    }
    loader_leave();
    rt_set_last_error(ERROR_SUCCESS);
    RET(m->base);
}

void rt_FreeLibrary(_cpu *cpu)
{
    rt_module *m = find_by_handle(ARG(0));
    if (ARG(0) == 0 || m == NULL || !is_loaded(m))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    loader_enter();
    if (m->kind == MOD_DLL)
    {
        m->refs--;
        if (m->refs == 0 && m->attached)
        {
            int i;
            rt_call_std(cpu, m->entry, 3, m->base, DLL_PROCESS_DETACH, 0);
            m->attached = false;
            for (i = 0; i < nloaded; i++)
            {
                if (load_order[i] == (int)(m - modules))
                {
                    memmove(&load_order[i], &load_order[i + 1], (size_t)(nloaded - i - 1) * sizeof(int));
                    nloaded--;
                    break;
                }
            }
        }
    }
    loader_leave();
    RET(1);
}

void rt_GetModuleHandleA(_cpu *cpu)
{
    char buf[260], norm[MAX_NAME];
    rt_module *m;
    if (ARG(0) == 0)
    {
        RET(modules[0].base);
        return;
    }
    if (rt_gstr(buf, sizeof(buf), ARG(0)) != 0 || !normalize(buf, norm, sizeof(norm)) ||
        (m = find_by_name(norm)) == NULL || !is_loaded(m))
    {
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
        RET(0);
        return;
    }
    RET(m->base);
}

/* Copy src into the guest buffer (size n) with Windows XP truncation
   rules: the result is truncated and not terminated when the buffer is too
   small, and the return value is n. */
void rt_GetModuleFileNameA(_cpu *cpu)
{
    rt_module *m = find_by_handle(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2);
    char path[300];
    size_t len;

    if (m == NULL || !is_loaded(m))
    {
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
        RET(0);
        return;
    }
    if (m->kind == MOD_SYSTEM)
        snprintf(path, sizeof(path), "%s\\%s", RT_GUEST_SYSTEM_DIR, m->name);
    else
        snprintf(path, sizeof(path), "%s\\%s", RT_GUEST_GAME_DIR, m->name);
    len = strlen(path);
    if (n == 0)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    if (len + 1 > n)
    {
        memcpy(G2H(buf), path, n);
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(n);
        return;
    }
    memcpy(G2H(buf), path, len + 1);
    rt_set_last_error(ERROR_SUCCESS);
    RET((uint32_t)len);
}

void rt_GetProcAddress(_cpu *cpu)
{
    rt_module *m = find_by_handle(ARG(0));
    uint32_t name = ARG(1);
    char buf[256];
    bool by_ordinal = (name >> 16) == 0;
    unsigned i;

    if (ARG(0) == 0 || m == NULL || !is_loaded(m))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (!by_ordinal && rt_gstr(buf, sizeof(buf), name) != 0)
    {
        rt_set_last_error(ERROR_PROC_NOT_FOUND);
        RET(0);
        return;
    }
    if (m->kind == MOD_SYSTEM)
    {
        for (i = 0; i < rt_import_table_count; i++)
        {
            const rt_import_entry *e = &rt_import_table[i];
            if (strcmp(e->dll, m->key) != 0) continue;
            if (by_ordinal ? (e->name == NULL && e->ordinal == name) : (e->name != NULL && strcmp(e->name, buf) == 0))
            {
                RET(H2G(e->addr));
                return;
            }
        }
    }
    else
    {
        for (i = 0; i < m->nexports; i++)
        {
            const rt_export_entry *e = &m->exports[i];
            if (by_ordinal ? e->ordinal == name : strcmp(e->name, buf) == 0)
            {
                RET(H2G(e->addr));
                return;
            }
        }
    }
    if (by_ordinal)
        rt_log("GetProcAddress(%s, #%u): no such function in the emulated system", m->name, name);
    else
        rt_log("GetProcAddress(%s, \"%s\"): no such function in the emulated system", m->name, buf);
    rt_set_last_error(ERROR_PROC_NOT_FOUND);
    RET(0);
}

/* ---------------------------------------------------------------- thread notifications */

static void notify_threads(_cpu *cpu, uint32_t reason)
{
    int i;
    loader_enter();
    for (i = 0; i < nloaded; i++)
    {
        rt_module *m = &modules[load_order[i]];
        if (m->attached && m->entry != 0) rt_call_std(cpu, m->entry, 3, m->base, reason, 0);
    }
    loader_leave();
}

void rt_module_thread_attach(_cpu *cpu)
{
    notify_threads(cpu, DLL_THREAD_ATTACH);
}

void rt_module_thread_detach(_cpu *cpu)
{
    if (atomic_load(&exiting)) return;
    notify_threads(cpu, DLL_THREAD_DETACH);
}

/* ---------------------------------------------------------------- process exit */

static void suspend_other(rt_thread *t, void *ctx)
{
    rt_thread *self = ctx;
    if (t == self) return;
    kern_return_t kr = thread_suspend(pthread_mach_thread_np(t->pt));
    if (kr != KERN_SUCCESS) rt_log("thread_suspend of thread %u failed (%d)", t->tid, kr);
}

_Noreturn void rt_process_exit(uint32_t code)
{
    rt_thread *self = rt_self_or_null();
    int i;

    if (atomic_exchange(&exiting, true))
    {
        /* A second exit during the exit notifications ends the process. */
        fflush(stderr);
        _exit((int)code);
    }
    if (self != NULL)
    {
        /* ExitProcess ends all other threads before the DLL notifications. */
        rt_thread_foreach(suspend_other, self);
        loader_enter();
        for (i = nloaded - 1; i >= 0; i--)
        {
            rt_module *m = &modules[load_order[i]];
            if (m->attached && m->entry != 0)
            {
                m->attached = false;
                rt_call_std(self->cpu, m->entry, 3, m->base, DLL_PROCESS_DETACH, 1);
            }
        }
        loader_leave();
    }
    rt_crt_flush_all();
    fflush(stdout);
    fflush(stderr);
    _exit((int)code);
}

void rt_TerminateProcess(_cpu *cpu)
{
    if (ARG(0) != 0xFFFFFFFFu) rt_fatal("TerminateProcess of another process");
    fflush(stderr);
    _exit((int)ARG(1));
}
