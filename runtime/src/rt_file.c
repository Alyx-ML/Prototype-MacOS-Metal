/* The emulated file system and the KERNEL32, SHELL32 and SHLWAPI file
   functions. See rt_file.h for the guest directory layout. */
#include "rt_file.h"
#include "rt_module.h"
#include "rt_object.h"
#include "rt_thread.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <unistd.h>

#define ERROR_INVALID_FUNCTION 1u
#define ERROR_WRITE_PROTECT 19u
#define ERROR_SHARING_VIOLATION 32u
#define ERROR_BAD_NETPATH 53u
#define ERROR_DISK_FULL 112u
#define ERROR_INVALID_NAME 123u
#define ERROR_DIR_NOT_EMPTY 145u
#define ERROR_NEGATIVE_SEEK 131u
#define ERROR_FILENAME_EXCED_RANGE 206u
#define ERROR_DIRECTORY 267u

#define INVALID_HANDLE_VALUE 0xFFFFFFFFu

typedef struct fs_mount {
    const char *guest; /* normalized, for example "C:\\Prototype" */
    char host[RT_HOST_PATH];
} fs_mount;

#define NUM_MOUNTS 3
static fs_mount mounts[NUM_MOUNTS] = {
    { "C:\\Prototype", "" },
    { "C:\\Users\\Player", "" },
    { "C:\\ProgramData", "" },
};

/* Directories above the mounts. They exist and contain only the mounts. */
static const char *const virtual_dirs[] = { "C:\\", "C:\\Users" };

static pthread_mutex_t cwd_lock = PTHREAD_MUTEX_INITIALIZER;
static char cwd[RT_MAX_PATH] = RT_GUEST_GAME_DIR;

/* ---------------------------------------------------------------- setup */

static void make_dirs(const char *path)
{
    char buf[RT_HOST_PATH];
    size_t i, n = strlen(path);
    if (n >= sizeof(buf)) rt_fatal("host path too long: %s", path);
    memcpy(buf, path, n + 1);
    for (i = 1; i <= n; i++)
    {
        if (buf[i] == '/' || buf[i] == 0)
        {
            char c = buf[i];
            buf[i] = 0;
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) rt_fatal("cannot create directory %s (%s)", buf, strerror(errno));
            buf[i] = c;
        }
    }
}

static void set_mount(int i, const char *host, bool create)
{
    struct stat st;
    size_t n = strlen(host);
    if (n == 0 || n >= RT_HOST_PATH - 1) rt_fatal("bad host directory \"%s\"", host);
    memcpy(mounts[i].host, host, n + 1);
    while (n > 1 && mounts[i].host[n - 1] == '/') mounts[i].host[--n] = 0;
    if (create) make_dirs(mounts[i].host);
    if (stat(mounts[i].host, &st) != 0 || !S_ISDIR(st.st_mode)) rt_fatal("host directory %s does not exist", mounts[i].host);
}

void rt_fs_init(const char *game_dir, const char *user_dir, const char *programdata_dir)
{
    char sub[RT_HOST_PATH];
    static const char *const user_subdirs[] = { "Documents", "AppData/Roaming", "AppData/Local", "AppData/Local/Temp" };
    size_t i;
    set_mount(0, game_dir, false);
    set_mount(1, user_dir, true);
    set_mount(2, programdata_dir, true);
    for (i = 0; i < sizeof(user_subdirs) / sizeof(user_subdirs[0]); i++)
    {
        if ((size_t)snprintf(sub, sizeof(sub), "%s/%s", mounts[1].host, user_subdirs[i]) >= sizeof(sub))
            rt_fatal("host path too long");
        make_dirs(sub);
    }
}

/* ---------------------------------------------------------------- paths */

uint32_t rt_errno_to_win32(int e, uint32_t not_found)
{
    switch (e)
    {
    case ENOENT: return not_found;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EISDIR: return ERROR_ACCESS_DENIED;
    case EROFS: return ERROR_WRITE_PROTECT;
    case EEXIST: return ERROR_FILE_EXISTS;
    case EMFILE:
    case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case ENOSPC:
    case EDQUOT: return ERROR_DISK_FULL;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EBUSY:
    case ETXTBSY: return ERROR_SHARING_VIOLATION;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    default: return ERROR_ACCESS_DENIED;
    }
}

static bool strieq_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
    {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        if (a[i] == 0) return true;
    }
    return true;
}

static void get_cwd(char *out, size_t n)
{
    if (pthread_mutex_lock(&cwd_lock) != 0) rt_fatal("cwd lock failed");
    snprintf(out, n, "%s", cwd);
    if (pthread_mutex_unlock(&cwd_lock) != 0) rt_fatal("cwd unlock failed");
}

uint32_t rt_path_full(const char *in, char *out, size_t n)
{
    char tmp[RT_MAX_PATH * 2 + 8], base[RT_MAX_PATH];
    char *comp[RT_MAX_PATH];
    size_t i, len = strlen(in), ncomp = 0, pos;
    char drive;
    char *p, *s;

    if (len == 0) return ERROR_PATH_NOT_FOUND;
    if (len >= RT_MAX_PATH) return ERROR_FILENAME_EXCED_RANGE;
    memcpy(tmp, in, len + 1);
    for (i = 0; i < len; i++)
        if (tmp[i] == '/') tmp[i] = '\\';
    p = tmp;
    if (p[0] == '\\' && p[1] == '\\')
    {
        if ((p[2] == '?' || p[2] == '.') && p[3] == '\\' && isalpha((unsigned char)p[4]) && p[5] == ':')
            p += 4; /* \\?\C:\... */
        else
            return ERROR_BAD_NETPATH;
    }

    get_cwd(base, sizeof(base));
    if (isalpha((unsigned char)p[0]) && p[1] == ':')
    {
        drive = (char)toupper((unsigned char)p[0]);
        p += 2;
        if (*p != '\\')
        {
            /* Drive-relative path: relative to the current directory of
               that drive. Only drive C has a current directory. */
            if (drive != 'C') return ERROR_PATH_NOT_FOUND;
            if (strlen(base) + 1 + strlen(p) >= sizeof(tmp) - 8) return ERROR_FILENAME_EXCED_RANGE;
            char joined[sizeof(tmp)];
            snprintf(joined, sizeof(joined), "%s\\%s", base + 2, p);
            memcpy(tmp, joined, strlen(joined) + 1);
            p = tmp;
        }
    }
    else if (p[0] == '\\')
    {
        drive = base[0];
    }
    else
    {
        char joined[sizeof(tmp)];
        drive = base[0];
        if (strlen(base) + 1 + strlen(p) >= sizeof(joined)) return ERROR_FILENAME_EXCED_RANGE;
        snprintf(joined, sizeof(joined), "%s\\%s", base + 2, p);
        memcpy(tmp, joined, strlen(joined) + 1);
        p = tmp;
    }

    /* p is now "\..." relative to the drive root. */
    for (s = strtok_r(p, "\\", &p); s != NULL; s = strtok_r(NULL, "\\", &p))
    {
        size_t cl = strlen(s);
        if (strcmp(s, ".") == 0) continue;
        if (strcmp(s, "..") == 0)
        {
            if (ncomp > 0) ncomp--;
            continue;
        }
        /* Windows removes trailing dots and spaces from a component. */
        while (cl > 0 && (s[cl - 1] == '.' || s[cl - 1] == ' ')) s[--cl] = 0;
        if (cl == 0) continue;
        for (i = 0; i < cl; i++)
        {
            unsigned char c = (unsigned char)s[i];
            if (c < 32 || strchr("<>:\"|?*", c) != NULL) return ERROR_INVALID_NAME;
        }
        comp[ncomp++] = s;
    }
    pos = 0;
    if (n < 4) return ERROR_FILENAME_EXCED_RANGE;
    out[pos++] = drive;
    out[pos++] = ':';
    out[pos++] = '\\';
    for (i = 0; i < ncomp; i++)
    {
        size_t cl = strlen(comp[i]);
        if (pos + cl + 2 > n || pos + cl + 1 > RT_MAX_PATH) return ERROR_FILENAME_EXCED_RANGE;
        if (i > 0) out[pos++] = '\\';
        memcpy(out + pos, comp[i], cl);
        pos += cl;
    }
    out[pos] = 0;
    return 0;
}

static bool is_virtual_dir(const char *full)
{
    size_t i;
    for (i = 0; i < sizeof(virtual_dirs) / sizeof(virtual_dirs[0]); i++)
        if (strlen(full) == strlen(virtual_dirs[i]) && strieq_n(full, virtual_dirs[i], strlen(full))) return true;
    return false;
}

/* Find the host name of component c in host directory dir (case-insensitive).
   Returns true and writes the name if it exists. */
static bool find_component(const char *dir, const char *c, char *name, size_t n)
{
    char probe[RT_HOST_PATH];
    struct stat st;
    DIR *d;
    struct dirent *e;
    bool found = false;

    if ((size_t)snprintf(probe, sizeof(probe), "%s/%s", dir, c) < sizeof(probe) && lstat(probe, &st) == 0)
    {
        snprintf(name, n, "%s", c);
        return true;
    }
    d = opendir(dir);
    if (d == NULL) return false;
    while ((e = readdir(d)) != NULL)
    {
        if (strcasecmp(e->d_name, c) == 0)
        {
            snprintf(name, n, "%s", e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

uint32_t rt_path_host(const char *guest, char *host, size_t n, bool *exists)
{
    char full[RT_MAX_PATH];
    uint32_t err = rt_path_full(guest, full, sizeof(full));
    const fs_mount *m = NULL;
    size_t i, mlen = 0;
    char rest[RT_MAX_PATH];
    char *s, *save;
    size_t pos;

    if (err != 0) return err;
    *exists = false;
    for (i = 0; i < NUM_MOUNTS; i++)
    {
        size_t l = strlen(mounts[i].guest);
        if (strieq_n(full, mounts[i].guest, l) && (full[l] == 0 || full[l] == '\\') && l > mlen)
        {
            m = &mounts[i];
            mlen = l;
        }
    }
    if (m == NULL) return is_virtual_dir(full) ? ERROR_ACCESS_DENIED : ERROR_PATH_NOT_FOUND;

    pos = strlen(m->host);
    if (pos + 1 > n) return ERROR_FILENAME_EXCED_RANGE;
    memcpy(host, m->host, pos + 1);
    snprintf(rest, sizeof(rest), "%s", full + mlen);
    *exists = true;
    for (s = strtok_r(rest, "\\", &save); s != NULL; s = strtok_r(NULL, "\\", &save))
    {
        char name[256];
        bool last = (save == NULL || *save == 0);
        if (!*exists) return ERROR_PATH_NOT_FOUND; /* a missing directory before this component */
        if (find_component(host, s, name, sizeof(name)))
        {
            struct stat st;
            if (pos + 1 + strlen(name) + 1 > n) return ERROR_FILENAME_EXCED_RANGE;
            host[pos++] = '/';
            memcpy(host + pos, name, strlen(name) + 1);
            pos += strlen(name);
            if (!last && (stat(host, &st) != 0 || !S_ISDIR(st.st_mode))) return ERROR_PATH_NOT_FOUND;
        }
        else
        {
            if (pos + 1 + strlen(s) + 1 > n) return ERROR_FILENAME_EXCED_RANGE;
            host[pos++] = '/';
            memcpy(host + pos, s, strlen(s) + 1);
            pos += strlen(s);
            *exists = false;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- attributes and times */

#define FILE_ATTRIBUTE_READONLY 0x01u
#define FILE_ATTRIBUTE_HIDDEN 0x02u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_ARCHIVE 0x20u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu

#define FILETIME_UNIX_EPOCH UINT64_C(116444736000000000)

uint64_t rt_filetime_from_timespec(struct timespec ts)
{
    if (ts.tv_sec < -11644473600LL) return 0;
    return (uint64_t)((int64_t)ts.tv_sec * 10000000 + 116444736000000000LL) + (uint64_t)ts.tv_nsec / 100u;
}

uint32_t rt_file_attributes(const struct stat *st, const char *name)
{
    uint32_t a = 0;
    if (S_ISDIR(st->st_mode)) a |= FILE_ATTRIBUTE_DIRECTORY;
    else a |= FILE_ATTRIBUTE_ARCHIVE;
    if ((st->st_mode & S_IWUSR) == 0) a |= FILE_ATTRIBUTE_READONLY;
    if (name != NULL && name[0] == '.' && strcmp(name, ".") != 0 && strcmp(name, "..") != 0) a |= FILE_ATTRIBUTE_HIDDEN;
    return a;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Read a guest path argument. Returns false and sets the last error if
   the string is missing or too long. */
static bool path_arg(uint32_t g, char *buf, size_t n)
{
    if (g == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return false;
    }
    if (rt_gstr(buf, n, g) != 0)
    {
        rt_set_last_error(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }
    return true;
}

void rt_GetFileAttributesA(_cpu *cpu)
{
    char gp[RT_MAX_PATH], full[RT_MAX_PATH], host[RT_HOST_PATH];
    struct stat st;
    bool exists;
    uint32_t err;

    if (!path_arg(ARG(0), gp, sizeof(gp)))
    {
        RET(INVALID_FILE_ATTRIBUTES);
        return;
    }
    if (rt_path_full(gp, full, sizeof(full)) == 0 && is_virtual_dir(full))
    {
        RET(FILE_ATTRIBUTE_DIRECTORY);
        return;
    }
    err = rt_path_host(gp, host, sizeof(host), &exists);
    if (err == 0 && !exists) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && stat(host, &st) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(INVALID_FILE_ATTRIBUTES);
        return;
    }
    RET(rt_file_attributes(&st, base_name(host)));
}

void rt_SetFileAttributesA(_cpu *cpu)
{
    char gp[RT_MAX_PATH], host[RT_HOST_PATH];
    struct stat st;
    bool exists;
    uint32_t err, attr = ARG(1);
    mode_t mode;

    if (!path_arg(ARG(0), gp, sizeof(gp)))
    {
        RET(0);
        return;
    }
    err = rt_path_host(gp, host, sizeof(host), &exists);
    if (err == 0 && !exists) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && stat(host, &st) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err == 0)
    {
        /* Only the read-only attribute has a host equivalent. Hidden,
           system and archive attributes are not stored. */
        mode = st.st_mode & 07777;
        if (attr & FILE_ATTRIBUTE_READONLY) mode &= (mode_t)~(S_IWUSR | S_IWGRP | S_IWOTH);
        else mode |= S_IWUSR;
        if (chmod(host, mode) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    }
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

/* ---------------------------------------------------------------- file handles */

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_FLAG_OVERLAPPED 0x40000000u
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u

typedef struct file_data {
    int fd;
    bool is_dir;
    bool delete_on_close;
    char host[RT_HOST_PATH];
} file_data;

static void file_destroy(rt_object *o)
{
    file_data *f = o->data;
    if (f->fd >= 0 && close(f->fd) != 0) rt_log("close of %s failed (%s)", f->host, strerror(errno));
    if (f->delete_on_close && unlink(f->host) != 0) rt_log("delete on close of %s failed (%s)", f->host, strerror(errno));
    free(f);
}

int rt_file_fd(uint32_t h)
{
    rt_object *o = rt_handle_get(h, OBJ_FILE);
    int fd;
    if (o == NULL) return -1;
    fd = ((file_data *)o->data)->fd;
    rt_object_unref(o);
    return fd;
}

void rt_CreateFileA(_cpu *cpu)
{
    char gp[RT_MAX_PATH], host[RT_HOST_PATH];
    uint32_t access = ARG(1), disp = ARG(4), flags = ARG(5);
    bool exists;
    uint32_t err, last = ERROR_SUCCESS;
    int oflags = O_CLOEXEC, fd;
    struct stat st;
    rt_object *o;
    file_data *f;
    uint32_t h;

    if (!path_arg(ARG(0), gp, sizeof(gp)))
    {
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    if (strncmp(gp, "\\\\.\\", 4) == 0 && !(isalpha((unsigned char)gp[4]) && gp[5] == ':' && gp[6] != 0))
    {
        /* A device (\\.\name). The emulated system has no devices. */
        rt_log("CreateFileA(\"%s\"): no such device in the emulated system", gp);
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    if (flags & FILE_FLAG_OVERLAPPED) rt_fatal("CreateFileA(\"%s\") with FILE_FLAG_OVERLAPPED is not supported", gp);
    if (ARG(6) != 0) rt_fatal("CreateFileA with a template file is not supported");

    err = rt_path_host(gp, host, sizeof(host), &exists);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    if (exists && stat(host, &st) == 0 && S_ISDIR(st.st_mode))
    {
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS) || (disp != OPEN_EXISTING && disp != OPEN_ALWAYS))
        {
            rt_set_last_error(ERROR_ACCESS_DENIED);
            RET(INVALID_HANDLE_VALUE);
            return;
        }
        oflags |= O_RDONLY | O_DIRECTORY;
    }
    else
    {
        /* GENERIC_ALL 0x10000000; FILE_READ_DATA 1; FILE_WRITE_DATA 2;
           FILE_APPEND_DATA 4. Other rights (attributes, DELETE,
           SYNCHRONIZE) need no host access mode. */
        bool rd = (access & (GENERIC_READ | 0x10000000u | 0x1u)) != 0;
        bool wrt = (access & (GENERIC_WRITE | 0x10000000u | 0x2u | 0x4u)) != 0;
        if (rd && wrt) oflags |= O_RDWR;
        else if (wrt) oflags |= O_WRONLY;
        else oflags |= O_RDONLY;
        switch (disp)
        {
        case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; if (exists) last = ERROR_ALREADY_EXISTS; break;
        case OPEN_EXISTING: break;
        case OPEN_ALWAYS: oflags |= O_CREAT; if (exists) last = ERROR_ALREADY_EXISTS; break;
        case TRUNCATE_EXISTING:
            if ((oflags & O_ACCMODE) == O_RDONLY)
            {
                rt_set_last_error(ERROR_INVALID_PARAMETER);
                RET(INVALID_HANDLE_VALUE);
                return;
            }
            oflags |= O_TRUNC;
            break;
        default:
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(INVALID_HANDLE_VALUE);
            return;
        }
        if (!exists && (disp == OPEN_EXISTING || disp == TRUNCATE_EXISTING))
        {
            rt_set_last_error(ERROR_FILE_NOT_FOUND);
            RET(INVALID_HANDLE_VALUE);
            return;
        }
    }
    fd = open(host, oflags, 0644);
    if (fd < 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND));
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    f = calloc(1, sizeof(*f));
    if (f == NULL) rt_fatal("out of host memory for a file");
    f->fd = fd;
    f->is_dir = (oflags & O_DIRECTORY) != 0;
    f->delete_on_close = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
    snprintf(f->host, sizeof(f->host), "%s", host);
    if ((flags & 0x1u) && (oflags & O_CREAT) && !exists)
    {
        /* FILE_ATTRIBUTE_READONLY on a new file. */
        if (fchmod(fd, 0444) != 0) rt_log("fchmod of %s failed (%s)", host, strerror(errno));
    }
    o = rt_object_new(OBJ_FILE);
    o->data = f;
    o->destroy = file_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    rt_set_last_error(last);
    RET(h);
}

static file_data *file_arg(uint32_t h, rt_object **o)
{
    *o = rt_handle_get(h, OBJ_FILE);
    if (*o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return (*o)->data;
}

/* OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent. With a
   handle without FILE_FLAG_OVERLAPPED the operation is synchronous at the
   given offset. */
static bool seek_overlapped(int fd, uint32_t ov)
{
    off_t off;
    if (ov == 0) return true;
    off = (off_t)(((uint64_t)rd32(ov + 12) << 32) | rd32(ov + 8));
    return lseek(fd, off, SEEK_SET) >= 0;
}

static void finish_overlapped(uint32_t ov, uint32_t n, uint32_t status)
{
    if (ov == 0) return;
    wr32(ov + 0, status);
    wr32(ov + 4, n);
    if (rd32(ov + 16) != 0)
    {
        rt_object *e = rt_handle_get(rd32(ov + 16), OBJ_EVENT);
        if (e != NULL)
        {
            rt_obj_lock();
            e->signaled = true;
            rt_obj_signal(e, e->manual_reset ? 0u : 1u);
            rt_obj_unlock();
            rt_object_unref(e);
        }
    }
}

void rt_ReadFile(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    uint32_t buf = ARG(1), n = ARG(2), out = ARG(3), ov = ARG(4), done = 0;
    bool ok = true;

    if (f == NULL)
    {
        RET(0);
        return;
    }
    if (out != 0) wr32(out, 0);
    if (n > 0 && !rt_va_is_committed(buf, n))
    {
        rt_object_unref(o);
        rt_set_last_error(998u); /* ERROR_NOACCESS */
        RET(0);
        return;
    }
    if (!seek_overlapped(f->fd, ov))
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_PARAMETER));
        ok = false;
    }
    while (ok && done < n)
    {
        ssize_t r = read(f->fd, (uint8_t *)G2H(buf) + done, n - done);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
            ok = false;
            break;
        }
        if (r == 0) break;
        done += (uint32_t)r;
    }
    rt_object_unref(o);
    if (out != 0) wr32(out, done);
    if (ok && ov != 0 && done == 0 && n > 0)
    {
        /* With an OVERLAPPED structure, a read at the end of the file fails
           with ERROR_HANDLE_EOF. */
        finish_overlapped(ov, 0, 0xC0000011u);
        rt_set_last_error(ERROR_HANDLE_EOF);
        RET(0);
        return;
    }
    if (ok) finish_overlapped(ov, done, 0);
    RET(ok ? 1 : 0);
}

void rt_WriteFile(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    uint32_t buf = ARG(1), n = ARG(2), out = ARG(3), ov = ARG(4), done = 0;
    bool ok = true;

    if (f == NULL)
    {
        RET(0);
        return;
    }
    if (out != 0) wr32(out, 0);
    if (n > 0 && !rt_va_is_committed(buf, n))
    {
        rt_object_unref(o);
        rt_set_last_error(998u);
        RET(0);
        return;
    }
    if (!seek_overlapped(f->fd, ov))
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_PARAMETER));
        ok = false;
    }
    while (ok && done < n)
    {
        ssize_t r = write(f->fd, (const uint8_t *)G2H(buf) + done, n - done);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
            ok = false;
            break;
        }
        done += (uint32_t)r;
    }
    rt_object_unref(o);
    if (out != 0) wr32(out, done);
    if (ok) finish_overlapped(ov, done, 0);
    RET(ok ? 1 : 0);
}

void rt_SetFilePointer(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    int32_t lo = (int32_t)ARG(1);
    uint32_t hi_ptr = ARG(2), method = ARG(3);
    int64_t dist;
    off_t r;
    int whence;

    if (f == NULL)
    {
        RET(0xFFFFFFFFu);
        return;
    }
    dist = hi_ptr ? (int64_t)(((uint64_t)rd32(hi_ptr) << 32) | (uint32_t)lo) : (int64_t)lo;
    switch (method)
    {
    case 0: whence = SEEK_SET; break;
    case 1: whence = SEEK_CUR; break;
    case 2: whence = SEEK_END; break;
    default:
        rt_object_unref(o);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0xFFFFFFFFu);
        return;
    }
    {
        off_t cur = lseek(f->fd, 0, whence == SEEK_SET ? SEEK_SET : whence);
        if (cur < 0 || (int64_t)cur + dist < 0)
        {
            rt_object_unref(o);
            rt_set_last_error(cur < 0 ? rt_errno_to_win32(errno, ERROR_INVALID_HANDLE) : ERROR_NEGATIVE_SEEK);
            RET(0xFFFFFFFFu);
            return;
        }
        r = lseek(f->fd, (off_t)((int64_t)cur + dist), SEEK_SET);
    }
    rt_object_unref(o);
    if (r < 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0xFFFFFFFFu);
        return;
    }
    if (hi_ptr) wr32(hi_ptr, (uint32_t)((uint64_t)r >> 32));
    else if ((uint64_t)r > 0xFFFFFFFFu)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0xFFFFFFFFu);
        return;
    }
    rt_set_last_error(ERROR_SUCCESS);
    RET((uint32_t)r);
}

void rt_GetFileSize(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    struct stat st;
    int r;
    if (f == NULL)
    {
        RET(0xFFFFFFFFu);
        return;
    }
    r = fstat(f->fd, &st);
    rt_object_unref(o);
    if (r != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0xFFFFFFFFu);
        return;
    }
    if (ARG(1) != 0) wr32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    rt_set_last_error(ERROR_SUCCESS);
    RET((uint32_t)st.st_size);
}

void rt_GetFileTime(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    struct stat st;
    int r;
    if (f == NULL)
    {
        RET(0);
        return;
    }
    r = fstat(f->fd, &st);
    rt_object_unref(o);
    if (r != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0);
        return;
    }
    if (ARG(1) != 0) wr64(ARG(1), rt_filetime_from_timespec(st.st_birthtimespec));
    if (ARG(2) != 0) wr64(ARG(2), rt_filetime_from_timespec(st.st_atimespec));
    if (ARG(3) != 0) wr64(ARG(3), rt_filetime_from_timespec(st.st_mtimespec));
    RET(1);
}

void rt_FlushFileBuffers(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    int r;
    if (f == NULL)
    {
        RET(0);
        return;
    }
    r = fsync(f->fd);
    rt_object_unref(o);
    if (r != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0);
        return;
    }
    RET(1);
}

void rt_SetEndOfFile(_cpu *cpu)
{
    rt_object *o;
    file_data *f = file_arg(ARG(0), &o);
    off_t pos;
    int r = -1;
    if (f == NULL)
    {
        RET(0);
        return;
    }
    pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos >= 0) r = ftruncate(f->fd, pos);
    rt_object_unref(o);
    if (r != 0)
    {
        rt_set_last_error(rt_errno_to_win32(errno, ERROR_INVALID_HANDLE));
        RET(0);
        return;
    }
    RET(1);
}

/* Devices are not part of the emulated system, so no handle accepts a
   device control code. */
void rt_DeviceIoControl(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), 0);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_object_unref(o);
    if (ARG(6) != 0) wr32(ARG(6), 0);
    rt_set_last_error(ERROR_INVALID_FUNCTION);
    RET(0);
}

/* ---------------------------------------------------------------- directories and names */

static uint32_t host_of_arg(uint32_t g, char *host, size_t n, bool *exists)
{
    char gp[RT_MAX_PATH];
    if (!path_arg(g, gp, sizeof(gp))) return rt_get_last_error();
    return rt_path_host(gp, host, n, exists);
}

void rt_CreateDirectoryA(_cpu *cpu)
{
    char host[RT_HOST_PATH], gp[RT_MAX_PATH], full[RT_MAX_PATH];
    bool exists;
    uint32_t err;

    if (ARG(0) != 0 && rt_gstr(gp, sizeof(gp), ARG(0)) == 0 && rt_path_full(gp, full, sizeof(full)) == 0 && is_virtual_dir(full))
    {
        rt_set_last_error(ERROR_ALREADY_EXISTS);
        RET(0);
        return;
    }
    err = host_of_arg(ARG(0), host, sizeof(host), &exists);
    if (err == 0 && exists) err = ERROR_ALREADY_EXISTS;
    if (err == 0 && mkdir(host, 0755) != 0) err = rt_errno_to_win32(errno, ERROR_PATH_NOT_FOUND);
    if (err == ERROR_FILE_EXISTS) err = ERROR_ALREADY_EXISTS;
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

void rt_RemoveDirectoryA(_cpu *cpu)
{
    char host[RT_HOST_PATH];
    bool exists;
    uint32_t err = host_of_arg(ARG(0), host, sizeof(host), &exists);
    if (err == 0 && !exists) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && rmdir(host) != 0) err = (errno == ENOTDIR) ? ERROR_DIRECTORY : rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

void rt_DeleteFileA(_cpu *cpu)
{
    char host[RT_HOST_PATH];
    bool exists;
    struct stat st;
    uint32_t err = host_of_arg(ARG(0), host, sizeof(host), &exists);
    if (err == 0 && !exists) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && stat(host, &st) == 0)
    {
        if (S_ISDIR(st.st_mode)) err = ERROR_ACCESS_DENIED;
        else if ((st.st_mode & S_IWUSR) == 0) err = ERROR_ACCESS_DENIED; /* read-only attribute */
    }
    if (err == 0 && unlink(host) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

void rt_MoveFileA(_cpu *cpu)
{
    char from[RT_HOST_PATH], to[RT_HOST_PATH];
    bool e1, e2;
    uint32_t err = host_of_arg(ARG(0), from, sizeof(from), &e1);
    if (err == 0) err = host_of_arg(ARG(1), to, sizeof(to), &e2);
    if (err == 0 && !e1) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && e2 && strcasecmp(from, to) != 0) err = ERROR_ALREADY_EXISTS;
    if (err == 0 && rename(from, to) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

void rt_CopyFileA(_cpu *cpu)
{
    char from[RT_HOST_PATH], to[RT_HOST_PATH];
    bool e1, e2;
    uint32_t err = host_of_arg(ARG(0), from, sizeof(from), &e1);
    int in = -1, out = -1;
    struct stat st;

    if (err == 0) err = host_of_arg(ARG(1), to, sizeof(to), &e2);
    if (err == 0 && !e1) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && e2 && ARG(2) != 0) err = ERROR_FILE_EXISTS;
    if (err == 0 && (in = open(from, O_RDONLY | O_CLOEXEC)) < 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err == 0 && fstat(in, &st) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (err == 0 && S_ISDIR(st.st_mode)) err = ERROR_ACCESS_DENIED;
    if (err == 0 && (out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777)) < 0)
        err = rt_errno_to_win32(errno, ERROR_PATH_NOT_FOUND);
    if (err == 0)
    {
        static char buf[65536];
        static pthread_mutex_t copy_lock = PTHREAD_MUTEX_INITIALIZER;
        if (pthread_mutex_lock(&copy_lock) != 0) rt_fatal("copy lock failed");
        for (;;)
        {
            ssize_t r = read(in, buf, sizeof(buf)), w = 0;
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) { err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND); break; }
            if (r == 0) break;
            while (w < r)
            {
                ssize_t k = write(out, buf + w, (size_t)(r - w));
                if (k < 0 && errno == EINTR) continue;
                if (k < 0) { err = rt_errno_to_win32(errno, ERROR_DISK_FULL); break; }
                w += k;
            }
            if (err != 0) break;
        }
        if (pthread_mutex_unlock(&copy_lock) != 0) rt_fatal("copy unlock failed");
    }
    if (in >= 0) close(in);
    if (out >= 0 && close(out) != 0 && err == 0) err = rt_errno_to_win32(errno, ERROR_DISK_FULL);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    RET(1);
}

/* Return a string in a guest buffer with the Win32 size rules: when the
   buffer is too small, the result is the size needed including the
   terminator. */
static uint32_t put_string(uint32_t buf, uint32_t n, const char *s)
{
    size_t len = strlen(s);
    if (len + 1 > n) return (uint32_t)len + 1u;
    memcpy(G2H(buf), s, len + 1);
    return (uint32_t)len;
}

void rt_GetCurrentDirectoryA(_cpu *cpu)
{
    char c[RT_MAX_PATH];
    get_cwd(c, sizeof(c));
    RET(put_string(ARG(1), ARG(0), c));
}

void rt_SetCurrentDirectoryA(_cpu *cpu)
{
    char gp[RT_MAX_PATH], full[RT_MAX_PATH], host[RT_HOST_PATH];
    struct stat st;
    bool exists;
    uint32_t err;
    if (!path_arg(ARG(0), gp, sizeof(gp)))
    {
        RET(0);
        return;
    }
    err = rt_path_full(gp, full, sizeof(full));
    if (err == 0 && !is_virtual_dir(full))
    {
        err = rt_path_host(full, host, sizeof(host), &exists);
        if (err == 0 && !exists) err = ERROR_FILE_NOT_FOUND;
        if (err == 0 && (stat(host, &st) != 0 || !S_ISDIR(st.st_mode))) err = ERROR_DIRECTORY;
    }
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    if (pthread_mutex_lock(&cwd_lock) != 0) rt_fatal("cwd lock failed");
    snprintf(cwd, sizeof(cwd), "%s", full);
    if (pthread_mutex_unlock(&cwd_lock) != 0) rt_fatal("cwd unlock failed");
    RET(1);
}

void rt_GetFullPathNameA(_cpu *cpu)
{
    char gp[RT_MAX_PATH], full[RT_MAX_PATH];
    uint32_t buf = ARG(2), n = ARG(1), part = ARG(3), err, r;
    size_t len;
    if (!path_arg(ARG(0), gp, sizeof(gp)))
    {
        RET(0);
        return;
    }
    len = strlen(gp);
    err = rt_path_full(gp, full, sizeof(full));
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    /* A trailing separator in the input stays in the result. */
    if (len > 0 && (gp[len - 1] == '\\' || gp[len - 1] == '/') && strlen(full) > 3 && strlen(full) + 1 < sizeof(full))
        strcat(full, "\\");
    r = put_string(buf, n, full);
    if (r < n && part != 0)
    {
        const char *s = strrchr(full, '\\');
        wr32(part, (s != NULL && s[1] != 0) ? buf + (uint32_t)(s + 1 - full) : 0);
    }
    RET(r);
}

/* ---------------------------------------------------------------- find files */

typedef struct find_entry {
    char name[256];
    struct stat st;
    bool virtual_dir;
} find_entry;

typedef struct find_data {
    find_entry *e;
    size_t n, next;
} find_data;

static void find_destroy(rt_object *o)
{
    find_data *f = o->data;
    free(f->e);
    free(f);
}

bool rt_wildcard_match(const char *pat, const char *name)
{
    /* "*.*" and "*" match every name; "x.*" matches "x" (no extension). */
    if (strcmp(pat, "*.*") == 0 || strcmp(pat, "*") == 0) return true;
    while (*pat)
    {
        if (*pat == '*')
        {
            pat++;
            if (*pat == 0) return true;
            for (; *name; name++)
                if (rt_wildcard_match(pat, name)) return true;
            return strcmp(pat, ".*") == 0 || strcmp(pat, ".") == 0 || rt_wildcard_match(pat, name);
        }
        if (*name == 0) return strcmp(pat, ".*") == 0 || strcmp(pat, ".") == 0;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*name)) return false;
        pat++;
        name++;
    }
    return *name == 0;
}

static bool find_add(find_data *f, size_t *cap, const char *name, const struct stat *st, bool vdir)
{
    if (f->n == *cap)
    {
        size_t c = *cap ? *cap * 2u : 32u;
        find_entry *ne = realloc(f->e, c * sizeof(find_entry));
        if (ne == NULL) return false;
        f->e = ne;
        *cap = c;
    }
    snprintf(f->e[f->n].name, sizeof(f->e[f->n].name), "%s", name);
    if (st != NULL) f->e[f->n].st = *st;
    else memset(&f->e[f->n].st, 0, sizeof(struct stat));
    f->e[f->n].virtual_dir = vdir;
    f->n++;
    return true;
}

/* WIN32_FIND_DATAA (320 bytes). */
static void write_find_data(uint32_t g, const find_entry *e)
{
    memset(G2H(g), 0, 320);
    if (e->virtual_dir)
    {
        wr32(g + 0, FILE_ATTRIBUTE_DIRECTORY);
    }
    else
    {
        wr32(g + 0, rt_file_attributes(&e->st, e->name));
        wr64(g + 4, rt_filetime_from_timespec(e->st.st_birthtimespec));
        wr64(g + 12, rt_filetime_from_timespec(e->st.st_atimespec));
        wr64(g + 20, rt_filetime_from_timespec(e->st.st_mtimespec));
        if (!S_ISDIR(e->st.st_mode))
        {
            wr32(g + 28, (uint32_t)((uint64_t)e->st.st_size >> 32));
            wr32(g + 32, (uint32_t)e->st.st_size);
        }
    }
    memcpy(G2H(g + 44), e->name, strlen(e->name) + 1);
}

void rt_FindFirstFileA(_cpu *cpu)
{
    char gp[RT_MAX_PATH], full[RT_MAX_PATH], dir_guest[RT_MAX_PATH], host[RT_HOST_PATH];
    const char *pattern;
    char *slash;
    find_data *f;
    size_t cap = 0, i;
    bool exists;
    uint32_t err, h;
    rt_object *o;

    if (!path_arg(ARG(0), gp, sizeof(gp)))
    {
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    /* Split into the directory and the last component (the pattern). The
       pattern can have wildcards; rt_path_full does not accept them. */
    for (i = 0; gp[i]; i++)
        if (gp[i] == '/') gp[i] = '\\';
    slash = strrchr(gp, '\\');
    if (slash == NULL && gp[0] != 0 && gp[1] == ':')
    {
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    if (slash != NULL)
    {
        *slash = 0;
        pattern = slash + 1;
        if (gp[0] == 0) snprintf(dir_guest, sizeof(dir_guest), "\\");
        else if (gp[1] == ':' && gp[2] == 0) snprintf(dir_guest, sizeof(dir_guest), "%s\\", gp);
        else snprintf(dir_guest, sizeof(dir_guest), "%s", gp);
    }
    else
    {
        pattern = gp;
        snprintf(dir_guest, sizeof(dir_guest), ".");
    }
    if (*pattern == 0)
    {
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    err = rt_path_full(dir_guest, full, sizeof(full));
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    f = calloc(1, sizeof(*f));
    if (f == NULL) rt_fatal("out of host memory for a find handle");

    if (is_virtual_dir(full))
    {
        /* The children of a directory above the mounts are the next
           components of the mount paths. */
        size_t fl = strlen(full);
        for (i = 0; i < NUM_MOUNTS; i++)
        {
            const char *mg = mounts[i].guest;
            char child[64];
            size_t k = 0;
            if (!strieq_n(mg, full, fl)) continue;
            const char *rest = mg + fl;
            if (*rest == '\\') rest++;
            while (rest[k] && rest[k] != '\\' && k + 1 < sizeof(child)) { child[k] = rest[k]; k++; }
            child[k] = 0;
            bool dup = false;
            for (size_t j = 0; j < f->n; j++) dup |= strcasecmp(f->e[j].name, child) == 0;
            if (k > 0 && !dup && rt_wildcard_match(pattern, child) && !find_add(f, &cap, child, NULL, true))
                rt_fatal("out of host memory");
        }
    }
    else
    {
        err = rt_path_host(full, host, sizeof(host), &exists);
        if (err == 0 && !exists) err = ERROR_PATH_NOT_FOUND;
        if (err == 0)
        {
            DIR *d = opendir(host);
            struct dirent *de;
            if (d == NULL) err = rt_errno_to_win32(errno, ERROR_PATH_NOT_FOUND);
            else
            {
                while ((de = readdir(d)) != NULL)
                {
                    char p[RT_HOST_PATH];
                    struct stat st;
                    if (!rt_wildcard_match(pattern, de->d_name)) continue;
                    if ((size_t)snprintf(p, sizeof(p), "%s/%s", host, de->d_name) >= sizeof(p)) continue;
                    if (stat(p, &st) != 0) continue;
                    if (!find_add(f, &cap, de->d_name, &st, false)) rt_fatal("out of host memory");
                }
                closedir(d);
            }
        }
    }
    if (err == 0 && f->n == 0) err = ERROR_FILE_NOT_FOUND;
    if (err != 0)
    {
        free(f->e);
        free(f);
        rt_set_last_error(err);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    write_find_data(ARG(1), &f->e[0]);
    f->next = 1;
    o = rt_object_new(OBJ_FIND);
    o->data = f;
    o->destroy = find_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    rt_set_last_error(ERROR_SUCCESS);
    RET(h);
}

void rt_FindNextFileA(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_FIND);
    find_data *f;
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    f = o->data;
    if (f->next >= f->n)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_NO_MORE_FILES);
        RET(0);
        return;
    }
    write_find_data(ARG(1), &f->e[f->next++]);
    rt_object_unref(o);
    RET(1);
}

void rt_FindClose(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_FIND);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_object_unref(o);
    rt_handle_close(ARG(0));
    RET(1);
}

/* ---------------------------------------------------------------- OpenFile */

#define OF_READ 0x0000u
#define OF_WRITE 0x0001u
#define OF_READWRITE 0x0002u
#define OF_PARSE 0x0100u
#define OF_DELETE 0x0200u
#define OF_CREATE 0x1000u
#define OF_EXIST 0x4000u
#define HFILE_ERROR 0xFFFFFFFFu

/* OpenFile(name, OFSTRUCT*, style). OFSTRUCT: cBytes, fFixedDisk,
   nErrCode (16-bit), 2 reserved words, szPathName[128]. */
void rt_OpenFile(_cpu *cpu)
{
    char gp[RT_MAX_PATH], full[RT_MAX_PATH], host[RT_HOST_PATH];
    uint32_t of = ARG(1), style = ARG(2), err;
    bool exists;
    int fd, oflags = O_CLOEXEC;
    rt_object *o;
    file_data *f;
    uint32_t h;

    if (!path_arg(ARG(0), gp, sizeof(gp)) || of == 0)
    {
        RET(HFILE_ERROR);
        return;
    }
    memset(G2H(of), 0, 136);
    wr8(of + 0, 136);
    wr8(of + 1, 1);
    err = rt_path_full(gp, full, sizeof(full));
    if (err == 0 && strlen(full) < 128) memcpy(G2H(of + 8), full, strlen(full) + 1);
    if (err == 0 && (style & OF_PARSE))
    {
        RET(0);
        return;
    }
    if (err == 0) err = rt_path_host(full, host, sizeof(host), &exists);
    if (err == 0 && !exists && !(style & OF_CREATE)) err = ERROR_FILE_NOT_FOUND;
    if (err == 0 && (style & OF_DELETE))
    {
        if (unlink(host) != 0) err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
        if (err == 0)
        {
            RET(1);
            return;
        }
    }
    if (err != 0)
    {
        wr16(of + 2, (uint16_t)err);
        rt_set_last_error(err);
        RET(HFILE_ERROR);
        return;
    }
    if (style & OF_EXIST)
    {
        RET(1);
        return;
    }
    if (style & OF_READWRITE) oflags |= O_RDWR;
    else if (style & OF_WRITE) oflags |= O_WRONLY;
    else oflags |= O_RDONLY;
    if (style & OF_CREATE) oflags |= O_CREAT | O_TRUNC | ((oflags & O_ACCMODE) == O_RDONLY ? O_RDWR : 0);
    fd = open(host, oflags, 0644);
    if (fd < 0)
    {
        err = rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
        wr16(of + 2, (uint16_t)err);
        rt_set_last_error(err);
        RET(HFILE_ERROR);
        return;
    }
    f = calloc(1, sizeof(*f));
    if (f == NULL) rt_fatal("out of host memory for a file");
    f->fd = fd;
    snprintf(f->host, sizeof(f->host), "%s", host);
    o = rt_object_new(OBJ_FILE);
    o->data = f;
    o->destroy = file_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(HFILE_ERROR);
        return;
    }
    RET(h);
}

/* ---------------------------------------------------------------- volumes */

static bool is_c_root(uint32_t g)
{
    char buf[8];
    if (g == 0) return true; /* the root of the current drive */
    if (rt_gstr(buf, sizeof(buf), g) != 0) return false;
    return (buf[0] == 'C' || buf[0] == 'c') && buf[1] == ':' && (buf[2] == '\\' || buf[2] == '/') && buf[3] == 0;
}

void rt_GetDriveTypeA(_cpu *cpu)
{
    RET(is_c_root(ARG(0)) ? 3u : 1u); /* DRIVE_FIXED, DRIVE_NO_ROOT_DIR */
}

void rt_GetLogicalDrives(_cpu *cpu)
{
    RET(1u << 2); /* C: */
}

/* GetVolumeInformationA(root, name, name_size, serial, max_component,
   flags, fs_name, fs_name_size). */
void rt_GetVolumeInformationA(_cpu *cpu)
{
    if (!is_c_root(ARG(0)))
    {
        rt_set_last_error(ERROR_PATH_NOT_FOUND);
        RET(0);
        return;
    }
    if (ARG(1) != 0 && put_string(ARG(1), ARG(2), "") >= ARG(2))
    {
        rt_set_last_error(ERROR_MORE_DATA);
        RET(0);
        return;
    }
    if (ARG(3) != 0) wr32(ARG(3), 0x50524F54u);
    if (ARG(4) != 0) wr32(ARG(4), 255);
    if (ARG(5) != 0) wr32(ARG(5), 0x00000006u); /* FILE_CASE_PRESERVED_NAMES, FILE_UNICODE_ON_DISK */
    if (ARG(6) != 0 && put_string(ARG(6), ARG(7), "NTFS") >= ARG(7))
    {
        rt_set_last_error(ERROR_MORE_DATA);
        RET(0);
        return;
    }
    RET(1);
}

void rt_GetDiskFreeSpaceExA(_cpu *cpu)
{
    char host[RT_HOST_PATH], gp[RT_MAX_PATH];
    bool exists;
    struct statfs sf;
    uint32_t err;
    uint64_t bs;

    if (ARG(0) == 0) get_cwd(gp, sizeof(gp));
    else if (rt_gstr(gp, sizeof(gp), ARG(0)) != 0)
    {
        rt_set_last_error(ERROR_FILENAME_EXCED_RANGE);
        RET(0);
        return;
    }
    err = rt_path_host(gp, host, sizeof(host), &exists);
    if (err == ERROR_ACCESS_DENIED) /* a directory above the mounts: use the user directory */
    {
        snprintf(host, sizeof(host), "%s", mounts[1].host);
        err = 0;
        exists = true;
    }
    if (err == 0 && !exists) err = ERROR_PATH_NOT_FOUND;
    if (err == 0 && statfs(host, &sf) != 0) err = rt_errno_to_win32(errno, ERROR_PATH_NOT_FOUND);
    if (err != 0)
    {
        rt_set_last_error(err);
        RET(0);
        return;
    }
    bs = sf.f_bsize;
    if (ARG(1) != 0) wr64(ARG(1), (uint64_t)sf.f_bavail * bs);
    if (ARG(2) != 0) wr64(ARG(2), (uint64_t)sf.f_blocks * bs);
    if (ARG(3) != 0) wr64(ARG(3), (uint64_t)sf.f_bfree * bs);
    RET(1);
}

/* ---------------------------------------------------------------- shell folders */

#define CSIDL_PERSONAL 0x05u
#define CSIDL_APPDATA 0x1Au
#define CSIDL_LOCAL_APPDATA 0x1Cu
#define CSIDL_COMMON_APPDATA 0x23u
#define CSIDL_PROFILE 0x28u
#define CSIDL_COMMON_DOCUMENTS 0x2Eu
#define CSIDL_FLAG_CREATE 0x8000u
#define S_OK 0u
#define E_INVALIDARG 0x80070057u
#define E_FAIL 0x80004005u

/* SHGetFolderPathA(hwnd, csidl, token, flags, path[MAX_PATH]). */
void rt_SHGetFolderPathA(_cpu *cpu)
{
    uint32_t csidl = ARG(1) & 0xFFu, out = ARG(4);
    const char *p;
    switch (csidl)
    {
    case CSIDL_PERSONAL: p = "C:\\Users\\Player\\Documents"; break;
    case CSIDL_APPDATA: p = "C:\\Users\\Player\\AppData\\Roaming"; break;
    case CSIDL_LOCAL_APPDATA: p = "C:\\Users\\Player\\AppData\\Local"; break;
    case CSIDL_PROFILE: p = "C:\\Users\\Player"; break;
    case CSIDL_COMMON_APPDATA: p = "C:\\ProgramData"; break;
    case CSIDL_COMMON_DOCUMENTS: p = "C:\\ProgramData\\Documents"; break;
    default:
        rt_log("SHGetFolderPathA: folder 0x%x is not in the emulated system", ARG(1));
        if (out != 0) wr8(out, 0);
        RET(E_INVALIDARG);
        return;
    }
    if (out == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    if (ARG(1) & CSIDL_FLAG_CREATE)
    {
        char host[RT_HOST_PATH];
        bool exists;
        if (rt_path_host(p, host, sizeof(host), &exists) != 0)
        {
            RET(E_FAIL);
            return;
        }
        if (!exists) make_dirs(host);
    }
    memcpy(G2H(out), p, strlen(p) + 1);
    RET(S_OK);
}

/* PathRelativePathToA(out, from, from_attr, to, to_attr): the relative
   path from "from" to "to". Both must be on the same drive. */
static size_t split_components(char *path, char **comp, size_t max)
{
    size_t n = 0;
    char *save, *c;
    for (c = strtok_r(path + 3, "\\", &save); c != NULL && n < max; c = strtok_r(NULL, "\\", &save)) comp[n++] = c;
    return n;
}

void rt_PathRelativePathToA(_cpu *cpu)
{
    char from_in[RT_MAX_PATH], to_in[RT_MAX_PATH], from[RT_MAX_PATH], to[RT_MAX_PATH], res[RT_MAX_PATH];
    char *fc[RT_MAX_PATH / 2], *tc[RT_MAX_PATH / 2];
    uint32_t out = ARG(0);
    size_t nf, nt, k = 0, i, pos = 0;

    if (out == 0 || ARG(1) == 0 || ARG(3) == 0 || rt_gstr(from_in, sizeof(from_in), ARG(1)) != 0 ||
        rt_gstr(to_in, sizeof(to_in), ARG(3)) != 0 || rt_path_full(from_in, from, sizeof(from)) != 0 ||
        rt_path_full(to_in, to, sizeof(to)) != 0)
    {
        if (out != 0) wr8(out, 0);
        RET(0);
        return;
    }
    if (toupper((unsigned char)from[0]) != toupper((unsigned char)to[0]))
    {
        wr8(out, 0);
        RET(0);
        return;
    }
    nf = split_components(from, fc, sizeof(fc) / sizeof(fc[0]));
    nt = split_components(to, tc, sizeof(tc) / sizeof(tc[0]));
    /* A file as "from": its directory is the start. */
    if (!(ARG(2) & FILE_ATTRIBUTE_DIRECTORY) && nf > 0) nf--;
    while (k < nf && k < nt && strcasecmp(fc[k], tc[k]) == 0) k++;
    if (nf == k)
    {
        res[pos++] = '.';
    }
    else
    {
        for (i = k; i < nf; i++)
        {
            if (pos + 3 >= sizeof(res)) { RET(0); return; }
            if (i > k) res[pos++] = '\\';
            res[pos++] = '.';
            res[pos++] = '.';
        }
    }
    for (i = k; i < nt; i++)
    {
        size_t l = strlen(tc[i]);
        if (pos + 1 + l + 1 > sizeof(res)) { RET(0); return; }
        res[pos++] = '\\';
        memcpy(res + pos, tc[i], l);
        pos += l;
    }
    res[pos] = 0;
    memcpy(G2H(out), res, pos + 1);
    RET(1);
}
