/*
 * ADVAPI32 registry functions.
 *
 * The registry is a tree in host memory. Key and value names are
 * case-insensitive; subkeys are kept in alphabetical order, as on Windows.
 * Keys that are not volatile are saved to the registry file after each
 * change and loaded at start (rt_registry_init).
 *
 * File format (UTF-8 text, one entry for each line):
 *   K<TAB><key path>
 *   V<TAB><key path><TAB><value name><TAB><type><TAB><data in hex>
 * The key path starts with the root name (HKEY_LOCAL_MACHINE and so on).
 *
 * The first start makes the values that the Steam installation script of
 * the game (10150_install.vdf) writes, with the paths of the emulated file
 * system and the English language values.
 */
#include "rt.h"
#include "rt_file.h"
#include "rt_module.h"
#include "rt_object.h"
#include "rt_registry.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#define HKCR 0x80000000u
#define HKCU 0x80000001u
#define HKLM 0x80000002u
#define HKU 0x80000003u

#define REG_SZ 1u
#define REG_EXPAND_SZ 2u
#define REG_BINARY 3u
#define REG_DWORD 4u
#define REG_MULTI_SZ 7u

#define REG_OPTION_VOLATILE 1u
#define MAX_KEY_NAME 255u
#define MAX_VALUE_NAME 16383u
#define MAX_VALUE_DATA 0x100000u

typedef struct reg_value {
    char *name;
    uint32_t type;
    uint8_t *data;
    uint32_t size;
} reg_value;

typedef struct reg_key reg_key;
struct reg_key {
    char *name;
    reg_key *parent;
    bool is_volatile;
    reg_key **children; /* sorted by name, case-insensitive */
    size_t nchildren, cchildren;
    reg_value *values;
    size_t nvalues, cvalues;
};

static const char *const root_names[4] = { "HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_USERS" };
static reg_key roots[4];
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static char reg_file[RT_HOST_PATH];
static bool loading;

static void lock(void)
{
    if (pthread_mutex_lock(&reg_lock) != 0) rt_fatal("registry lock failed");
}

static void unlock(void)
{
    if (pthread_mutex_unlock(&reg_lock) != 0) rt_fatal("registry unlock failed");
}

static char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (d == NULL) rt_fatal("out of host memory");
    return d;
}

static void *xgrow(void *p, size_t *cap, size_t elem)
{
    size_t n = *cap ? *cap * 2 : 8;
    void *q;
    if (n > SIZE_MAX / elem) rt_fatal("registry too large");
    q = realloc(p, n * elem);
    if (q == NULL) rt_fatal("out of host memory");
    *cap = n;
    return q;
}

/* ---------------------------------------------------------------- tree */

/* Index of the child with the name, or the insertion position (found is
   false). */
static size_t find_child(const reg_key *k, const char *name, bool *found)
{
    size_t lo = 0, hi = k->nchildren;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        int c = strcasecmp(k->children[mid]->name, name);
        if (c == 0)
        {
            *found = true;
            return mid;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    *found = false;
    return lo;
}

static reg_key *child(reg_key *k, const char *name, bool create, bool is_volatile, bool *created)
{
    bool found;
    size_t i = find_child(k, name, &found);
    reg_key *c;
    if (found) return k->children[i];
    if (!create) return NULL;
    c = calloc(1, sizeof(*c));
    if (c == NULL) rt_fatal("out of host memory");
    c->name = xstrdup(name);
    c->parent = k;
    c->is_volatile = is_volatile || k->is_volatile;
    if (k->nchildren == k->cchildren) k->children = xgrow(k->children, &k->cchildren, sizeof(reg_key *));
    memmove(k->children + i + 1, k->children + i, (k->nchildren - i) * sizeof(reg_key *));
    k->children[i] = c;
    k->nchildren++;
    if (created != NULL) *created = true;
    return c;
}

static reg_value *find_value(reg_key *k, const char *name)
{
    size_t i;
    for (i = 0; i < k->nvalues; i++)
        if (strcasecmp(k->values[i].name, name) == 0) return &k->values[i];
    return NULL;
}

static void set_value(reg_key *k, const char *name, uint32_t type, const void *data, uint32_t size)
{
    reg_value *v = find_value(k, name);
    uint8_t *copy = malloc(size ? size : 1);
    if (copy == NULL) rt_fatal("out of host memory");
    if (size) memcpy(copy, data, size);
    if (v == NULL)
    {
        if (k->nvalues == k->cvalues) k->values = xgrow(k->values, &k->cvalues, sizeof(reg_value));
        v = &k->values[k->nvalues++];
        v->name = xstrdup(name);
    }
    else
    {
        free(v->data);
    }
    v->type = type;
    v->data = copy;
    v->size = size;
}

/* Walk a backslash-separated path from k. Empty components are skipped.
   Returns NULL if a key is missing and create is false, or if a component
   is longer than 255 characters (*bad_name). */
static reg_key *walk(reg_key *k, const char *path, bool create, bool is_volatile, bool *created, bool *bad_name)
{
    char comp[MAX_KEY_NAME + 1];
    *bad_name = false;
    while (k != NULL && *path)
    {
        size_t n = 0;
        while (*path == '\\') path++;
        if (*path == 0) break;
        while (path[n] && path[n] != '\\') n++;
        if (n > MAX_KEY_NAME)
        {
            *bad_name = true;
            return NULL;
        }
        memcpy(comp, path, n);
        comp[n] = 0;
        path += n;
        k = child(k, comp, create, is_volatile, created);
    }
    return k;
}

/* ---------------------------------------------------------------- file */

static void write_path(FILE *f, const reg_key *k)
{
    if (k->parent == NULL)
    {
        fputs(root_names[k - roots], f);
        return;
    }
    write_path(f, k->parent);
    fputc('\\', f);
    fputs(k->name, f);
}

static void write_key(FILE *f, const reg_key *k)
{
    size_t i;
    uint32_t j;
    if (k->is_volatile) return;
    if (k->parent != NULL)
    {
        fputs("K\t", f);
        write_path(f, k);
        fputc('\n', f);
    }
    for (i = 0; i < k->nvalues; i++)
    {
        fputs("V\t", f);
        write_path(f, k);
        fprintf(f, "\t%s\t%u\t", k->values[i].name, k->values[i].type);
        for (j = 0; j < k->values[i].size; j++) fprintf(f, "%02x", k->values[i].data[j]);
        fputc('\n', f);
    }
    for (i = 0; i < k->nchildren; i++) write_key(f, k->children[i]);
}

/* Save the registry (atomic replacement of the file). The lock is held. */
static void save(void)
{
    char tmp[RT_HOST_PATH + 8];
    FILE *f;
    int i;
    if (loading || reg_file[0] == 0) return;
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", reg_file) >= sizeof(tmp)) rt_fatal("registry path too long");
    f = fopen(tmp, "w");
    if (f == NULL) rt_fatal("cannot write the registry file %s: %s", tmp, strerror(errno));
    for (i = 0; i < 4; i++) write_key(f, &roots[i]);
    if (ferror(f) != 0 || fclose(f) != 0) rt_fatal("write of the registry file %s failed", tmp);
    if (rename(tmp, reg_file) != 0) rt_fatal("cannot replace the registry file %s: %s", reg_file, strerror(errno));
}

static reg_key *root_by_name(const char *name, size_t n)
{
    int i;
    for (i = 0; i < 4; i++)
        if (strlen(root_names[i]) == n && strncmp(root_names[i], name, n) == 0) return &roots[i];
    return NULL;
}

static reg_key *key_from_path(const char *path, size_t n)
{
    const char *bs = memchr(path, '\\', n);
    size_t rl = bs ? (size_t)(bs - path) : n;
    reg_key *r = root_by_name(path, rl);
    char *rest;
    reg_key *k;
    bool bad;
    if (r == NULL) return NULL;
    if (bs == NULL) return r;
    rest = malloc(n - rl);
    if (rest == NULL) rt_fatal("out of host memory");
    memcpy(rest, bs + 1, n - rl - 1);
    rest[n - rl - 1] = 0;
    k = walk(r, rest, true, false, NULL, &bad);
    free(rest);
    return k;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void load(FILE *f)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    unsigned lineno = 0;
    while ((len = getline(&line, &cap, f)) > 0)
    {
        char *fields[6];
        int nf = 0;
        char *p = line;
        lineno++;
        if (line[len - 1] == '\n') line[--len] = 0;
        if (len == 0) continue;
        fields[nf++] = p;
        while (nf < 6 && (p = strchr(p, '\t')) != NULL)
        {
            *p++ = 0;
            fields[nf++] = p;
        }
        if (strcmp(fields[0], "K") == 0 && nf == 2)
        {
            if (key_from_path(fields[1], strlen(fields[1])) == NULL) rt_fatal("registry file %s line %u: bad key", reg_file, lineno);
        }
        else if (strcmp(fields[0], "V") == 0 && nf == 5)
        {
            reg_key *k = key_from_path(fields[1], strlen(fields[1]));
            char *end;
            unsigned long type = strtoul(fields[3], &end, 10);
            size_t hl = strlen(fields[4]), i;
            uint8_t *data;
            if (k == NULL || *end != 0 || type > 0xFFFFFFFFul || (hl & 1) != 0 || hl / 2 > MAX_VALUE_DATA)
                rt_fatal("registry file %s line %u: bad value", reg_file, lineno);
            data = malloc(hl / 2 + 1);
            if (data == NULL) rt_fatal("out of host memory");
            for (i = 0; i < hl / 2; i++)
            {
                int h = hexval(fields[4][2 * i]), l = hexval(fields[4][2 * i + 1]);
                if (h < 0 || l < 0) rt_fatal("registry file %s line %u: bad data", reg_file, lineno);
                data[i] = (uint8_t)(h * 16 + l);
            }
            set_value(k, fields[2], (uint32_t)type, data, (uint32_t)(hl / 2));
            free(data);
        }
        else
        {
            rt_fatal("registry file %s line %u: bad entry", reg_file, lineno);
        }
    }
    free(line);
    if (ferror(f) != 0) rt_fatal("read of the registry file %s failed", reg_file);
}

static void set_sz(reg_key *k, const char *name, const char *v)
{
    set_value(k, name, REG_SZ, v, (uint32_t)strlen(v) + 1u);
}

void rt_registry_init(const char *path)
{
    FILE *f;
    if ((size_t)snprintf(reg_file, sizeof(reg_file), "%s", path) >= sizeof(reg_file)) rt_fatal("registry path too long");
    lock();
    loading = true;
    f = fopen(reg_file, "r");
    if (f != NULL)
    {
        load(f);
        fclose(f);
    }
    else if (errno != ENOENT)
    {
        rt_fatal("cannot read the registry file %s: %s", reg_file, strerror(errno));
    }
    else
    {
        /* First start: the values of the Steam installation script. */
        bool bad;
        reg_key *k = walk(&roots[2], "SOFTWARE\\Activision\\Prototype", true, false, NULL, &bad);
        set_sz(k, "EXEString", RT_GUEST_GAME_DIR "\\Prototypef.exe");
        set_sz(k, "InstallExePath", RT_GUEST_GAME_DIR);
        set_sz(k, "IntVersion", "26.000000");
        set_sz(k, "Path", RT_GUEST_GAME_DIR);
        set_sz(k, "Version", "1.000000");
        set_sz(k, "LanguageCode", "ENU");
        set_sz(k, "LCID", "1033");
        set_sz(k, "FE_Language", "69");
    }
    loading = false;
    save();
    unlock();
}

/* ---------------------------------------------------------------- handles */

uint32_t rt_registry_get_sz(uint32_t root, const char *path, const char *name, char *out, size_t n)
{
    reg_key *k;
    reg_value *v;
    bool bad;
    uint32_t r = ERROR_FILE_NOT_FOUND;
    if (root < HKCR || root > HKU) rt_fatal("rt_registry_get_sz: 0x%08x is not a root key", root);
    lock();
    k = walk(&roots[root - HKCR], path, false, false, NULL, &bad);
    v = k != NULL ? find_value(k, name) : NULL;
    if (v != NULL && v->type == REG_SZ)
    {
        /* The stored data normally ends with a NUL; the copy always does. */
        size_t len = strnlen((const char *)v->data, v->size);
        if (len + 1 > n)
        {
            r = ERROR_MORE_DATA;
        }
        else
        {
            memcpy(out, v->data, len);
            out[len] = 0;
            r = ERROR_SUCCESS;
        }
    }
    unlock();
    return r;
}

static void key_destroy(rt_object *o)
{
    (void)o; /* keys are never deleted (RegDeleteKey is not imported) */
}

/* The key of a handle, or NULL. The registry lock is held. */
static reg_key *key_of(uint32_t h)
{
    rt_object *o;
    reg_key *k;
    if (h >= HKCR && h <= HKU) return &roots[h - HKCR];
    o = rt_handle_get(h, OBJ_REGKEY);
    if (o == NULL) return NULL;
    k = o->data;
    rt_object_unref(o);
    return k;
}

static uint32_t new_handle(reg_key *k, uint32_t *out)
{
    rt_object *o = rt_object_new(OBJ_REGKEY);
    uint32_t h;
    o->data = k;
    o->destroy = key_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0) return ERROR_NOT_ENOUGH_MEMORY;
    *out = h;
    return ERROR_SUCCESS;
}

static const char *gstr_or_empty(uint32_t g)
{
    return g != 0 ? (const char *)G2H(g) : "";
}

static uint32_t open_key(uint32_t hkey, uint32_t gsub, uint32_t phk)
{
    reg_key *k;
    bool bad;
    uint32_t r, h = 0;
    if (phk == 0) return ERROR_INVALID_PARAMETER;
    lock();
    k = key_of(hkey);
    if (k == NULL)
    {
        unlock();
        return ERROR_INVALID_HANDLE;
    }
    k = walk(k, gstr_or_empty(gsub), false, false, NULL, &bad);
    if (k == NULL)
    {
        unlock();
        return bad ? ERROR_INVALID_PARAMETER : ERROR_FILE_NOT_FOUND;
    }
    r = new_handle(k, &h);
    unlock();
    if (r == ERROR_SUCCESS) wr32(phk, h);
    return r;
}

/* LONG RegOpenKeyA(HKEY, LPCSTR sub, PHKEY): a NULL or empty name gives
   the same handle. */
void rt_RegOpenKeyA(_cpu *cpu)
{
    uint32_t hkey = ARG(0), sub = ARG(1), phk = ARG(2);
    if (phk != 0 && (sub == 0 || rd8(sub) == 0))
    {
        lock();
        if (key_of(hkey) == NULL)
        {
            unlock();
            RET(ERROR_INVALID_HANDLE);
            return;
        }
        unlock();
        wr32(phk, hkey);
        RET(ERROR_SUCCESS);
        return;
    }
    RET(open_key(hkey, sub, phk));
}

/* LONG RegOpenKeyExA(HKEY, LPCSTR sub, DWORD options, REGSAM, PHKEY) */
void rt_RegOpenKeyExA(_cpu *cpu)
{
    RET(open_key(ARG(0), ARG(1), ARG(4)));
}

/* LONG RegCreateKeyExA(HKEY, LPCSTR sub, DWORD reserved, LPSTR class,
   DWORD options, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD disp) */
void rt_RegCreateKeyExA(_cpu *cpu)
{
    uint32_t hkey = ARG(0), sub = ARG(1), options = ARG(4), phk = ARG(7), disp = ARG(8);
    reg_key *k;
    bool created = false, bad;
    uint32_t r, h = 0;
    if (sub == 0 || phk == 0 || (options & ~REG_OPTION_VOLATILE) != 0)
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    lock();
    k = key_of(hkey);
    if (k == NULL)
    {
        unlock();
        RET(ERROR_INVALID_HANDLE);
        return;
    }
    k = walk(k, G2H(sub), true, (options & REG_OPTION_VOLATILE) != 0, &created, &bad);
    if (k == NULL)
    {
        unlock();
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    if (created) save();
    r = new_handle(k, &h);
    unlock();
    if (r == ERROR_SUCCESS)
    {
        wr32(phk, h);
        if (disp != 0) wr32(disp, created ? 1u : 2u); /* REG_CREATED_NEW_KEY, REG_OPENED_EXISTING_KEY */
    }
    RET(r);
}

/* LONG RegCloseKey(HKEY) */
void rt_RegCloseKey(_cpu *cpu)
{
    uint32_t h = ARG(0);
    if (h >= HKCR && h <= HKU)
    {
        RET(ERROR_SUCCESS);
        return;
    }
    RET(rt_handle_close(h) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
}

/* LONG RegEnumKeyA(HKEY, DWORD index, LPSTR name, DWORD cch) */
void rt_RegEnumKeyA(_cpu *cpu)
{
    uint32_t hkey = ARG(0), index = ARG(1), name = ARG(2), cch = ARG(3);
    reg_key *k;
    size_t len;
    lock();
    k = key_of(hkey);
    if (k == NULL)
    {
        unlock();
        RET(ERROR_INVALID_HANDLE);
        return;
    }
    if (index >= k->nchildren)
    {
        unlock();
        RET(ERROR_NO_MORE_ITEMS);
        return;
    }
    len = strlen(k->children[index]->name);
    if (name == 0 || len + 1 > cch)
    {
        unlock();
        RET(ERROR_MORE_DATA);
        return;
    }
    memcpy(G2H(name), k->children[index]->name, len + 1);
    unlock();
    RET(ERROR_SUCCESS);
}

/* LONG RegQueryValueExA(HKEY, LPCSTR name, LPDWORD reserved, LPDWORD type,
   LPBYTE data, LPDWORD cb) */
void rt_RegQueryValueExA(_cpu *cpu)
{
    uint32_t hkey = ARG(0), name = ARG(1), reserved = ARG(2), ptype = ARG(3), data = ARG(4), pcb = ARG(5);
    reg_key *k;
    reg_value *v;
    uint32_t r = ERROR_SUCCESS;
    if (reserved != 0 || (data != 0 && pcb == 0))
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    lock();
    k = key_of(hkey);
    if (k == NULL)
    {
        unlock();
        RET(ERROR_INVALID_HANDLE);
        return;
    }
    v = find_value(k, gstr_or_empty(name));
    if (v == NULL)
    {
        unlock();
        RET(ERROR_FILE_NOT_FOUND);
        return;
    }
    if (ptype != 0) wr32(ptype, v->type);
    if (data != 0)
    {
        if (rd32(pcb) < v->size) r = ERROR_MORE_DATA;
        else memcpy(G2H(data), v->data, v->size);
    }
    if (pcb != 0) wr32(pcb, v->size);
    unlock();
    RET(r);
}

/* LONG RegSetValueExA(HKEY, LPCSTR name, DWORD reserved, DWORD type,
   const BYTE *data, DWORD cb) */
void rt_RegSetValueExA(_cpu *cpu)
{
    uint32_t hkey = ARG(0), name = ARG(1), type = ARG(3), data = ARG(4), cb = ARG(5);
    reg_key *k;
    const char *vn = gstr_or_empty(name);
    if ((data == 0 && cb != 0) || cb > MAX_VALUE_DATA || strlen(vn) > MAX_VALUE_NAME)
    {
        RET(ERROR_INVALID_PARAMETER);
        return;
    }
    lock();
    k = key_of(hkey);
    if (k == NULL)
    {
        unlock();
        RET(ERROR_INVALID_HANDLE);
        return;
    }
    set_value(k, vn, type, cb ? G2H(data) : NULL, cb);
    if (!k->is_volatile) save();
    unlock();
    RET(ERROR_SUCCESS);
}
