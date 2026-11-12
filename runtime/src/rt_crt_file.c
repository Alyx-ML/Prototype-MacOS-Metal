/*
 * MSVCR80 stream functions (fopen, fread, fprintf output and so on) and
 * _stat64.
 *
 * Behavior of the Microsoft runtime that this file follows:
 *   - text mode (the default unless _fmode is _O_BINARY or the mode has
 *     'b'): on input "\r\n" becomes "\n" and Ctrl+Z (0x1A) is the end of
 *     the file; on output "\n" becomes "\r\n";
 *   - a read directly after a write (or a write directly after a read that
 *     is not at the end of the file) without fflush, fseek or rewind sets
 *     the error flag and fails;
 *   - fflush on an input stream discards the buffered input;
 *   - feof returns 0x10 (_IOEOF) and ferror returns 0x20 (_IOERR).
 *
 * The standard streams use the host descriptors 0, 1 and 2. They do not
 * translate "\n" because the host console uses LF line ends. stderr has no
 * buffer: each operation writes its data at the end of the operation.
 * Sharing modes are not enforced (as in rt_file.c).
 */
#include "rt_crt.h"
#include "rt_crt_file.h"
#include "rt_file.h"
#include "rt_thread.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NSTREAM 512u
#define BUFSZ 4096u
#define IOB_SIZE 32u

/* struct _iobuf fields. */
#define F_PTR 0u
#define F_CNT 4u
#define F_BASE 8u
#define F_FLAG 12u
#define F_FILE 16u

/* _flag bits. */
#define IOREAD 0x01u
#define IOWRT 0x02u
#define IOEOF 0x10u
#define IOERR 0x20u
#define IORW 0x80u

/* MSVC errno values. */
#define M_ENOENT 2u
#define M_EBADF 9u
#define M_EAGAIN 11u
#define M_EACCES 13u
#define M_EINVAL 22u
#define M_EMFILE 24u
#define M_EDEADLK 36u
#define M_ENAMETOOLONG 38u
#define M_ENOLCK 39u
#define M_ENOSYS 40u
#define M_ENOTEMPTY 41u
#define M_EILSEQ 42u

enum { OP_NONE, OP_READ, OP_WRITE };

typedef struct stream {
    _Atomic uint32_t guest; /* guest FILE, 0 if the slot is free */
    pthread_mutex_t lock;   /* recursive */
    int fd;
    bool can_read, can_write, text, std, unbuffered, commit, delete_on_close;
    bool eof, err;
    int op;
    uint8_t buf[BUFSZ];
    size_t rpos, rlen; /* read buffer */
    off_t buf_off;     /* file offset of buf[0] while reading */
    size_t wlen;       /* bytes in the write buffer */
    int unget;         /* pushed back character, -1 if none */
    char host[RT_HOST_PATH];
} stream;

static stream *streams;
static uint32_t iob_base;
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

/* ---------------------------------------------------------------- errno */

static uint32_t msvc_errno(int e)
{
    /* The host values 1 to 34 are the same as the Microsoft values, except
       11 (EDEADLK on the host, EAGAIN in the Microsoft runtime). */
    switch (e)
    {
    case EDEADLK: return M_EDEADLK;
    case EAGAIN: return M_EAGAIN;
    case ENAMETOOLONG: return M_ENAMETOOLONG;
    case ENOTEMPTY: return M_ENOTEMPTY;
    case ENOLCK: return M_ENOLCK;
    case ENOSYS: return M_ENOSYS;
    case EILSEQ: return M_EILSEQ;
    default: break;
    }
    if (e >= 1 && e <= 34) return (uint32_t)e;
    /* The Microsoft runtime maps unknown errors to EINVAL (_dosmaperr). */
    return M_EINVAL;
}

static uint32_t win32_to_errno(uint32_t w)
{
    switch (w)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return M_ENOENT;
    case ERROR_ACCESS_DENIED: return M_EACCES;
    case ERROR_TOO_MANY_OPEN_FILES: return M_EMFILE;
    default: return M_EINVAL;
    }
}

/* ---------------------------------------------------------------- table */

static void init_streams(void)
{
    pthread_mutexattr_t a;
    uint32_t i;
    streams = calloc(NSTREAM, sizeof(stream));
    if (streams == NULL) rt_fatal("out of host memory for the stream table");
    if (pthread_mutexattr_init(&a) != 0 || pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE) != 0)
        rt_fatal("mutex attribute setup failed");
    for (i = 0; i < NSTREAM; i++)
    {
        if (pthread_mutex_init(&streams[i].lock, &a) != 0) rt_fatal("mutex setup failed");
        streams[i].fd = -1;
        streams[i].unget = -1;
    }
    pthread_mutexattr_destroy(&a);

    /* stdin, stdout, stderr: _iob[0..2]. */
    iob_base = rt_heap_calloc(3, IOB_SIZE);
    if (iob_base == 0) rt_fatal("no guest memory for the standard streams");
    for (i = 0; i < 3; i++)
    {
        stream *s = &streams[i];
        uint32_t g = iob_base + i * IOB_SIZE;
        s->fd = (int)i;
        s->can_read = (i == 0);
        s->can_write = (i != 0);
        s->std = true;
        s->unbuffered = (i == 2);
        snprintf(s->host, sizeof(s->host), "%s", i == 0 ? "<stdin>" : i == 1 ? "<stdout>" : "<stderr>");
        wr32(g + F_FLAG, i == 0 ? IOREAD : IOWRT);
        wr32(g + F_FILE, i);
        atomic_store(&s->guest, g);
    }
}

static void ensure_init(void)
{
    if (pthread_once(&init_once, init_streams) != 0) rt_fatal("stream table setup failed");
}

/* The stream of a guest FILE pointer. A pointer that is not an open stream
   is a guest defect (the Microsoft runtime has undefined behavior). */
static stream *get(uint32_t f, const char *fname)
{
    uint32_t idx;
    ensure_init();
    if (f == 0) rt_crt_invalid_parameter(fname);
    idx = rd32(f + F_FILE);
    if (idx >= NSTREAM || atomic_load(&streams[idx].guest) != f)
        rt_fatal("%s: FILE 0x%08x is not an open stream", fname, f);
    return &streams[idx];
}

static void lock_stream(stream *s)
{
    if (pthread_mutex_lock(&s->lock) != 0) rt_fatal("stream lock failed");
}

static void update_flag(stream *s)
{
    uint32_t g = atomic_load(&s->guest), fl = 0;
    if (g == 0) return;
    if (s->can_read && s->can_write) fl |= IORW;
    if (s->op == OP_READ || (s->can_read && !s->can_write)) fl |= IOREAD;
    if (s->op == OP_WRITE || (s->can_write && !s->can_read)) fl |= IOWRT;
    if (s->eof) fl |= IOEOF;
    if (s->err) fl |= IOERR;
    wr32(g + F_FLAG, fl);
    wr32(g + F_CNT, 0);
}

/* ---------------------------------------------------------------- buffers */

static bool write_all(stream *s, const uint8_t *p, size_t n)
{
    while (n > 0)
    {
        ssize_t w = write(s->fd, p, n);
        if (w < 0)
        {
            if (errno == EINTR) continue;
            rt_crt_set_errno(msvc_errno(errno));
            s->err = true;
            return false;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return true;
}

static bool flush_write(stream *s)
{
    bool ok = true;
    if (s->op == OP_WRITE && s->wlen > 0)
    {
        ok = write_all(s, s->buf, s->wlen);
        s->wlen = 0;
    }
    return ok;
}

/* Discard the read buffer. The descriptor position stays where the last
   read left it (the Microsoft runtime behaves the same in fflush). */
static void discard_read(stream *s)
{
    if (s->op == OP_READ)
    {
        s->buf_off += (off_t)s->rlen;
        s->rpos = s->rlen = 0;
    }
}

/* Set the read position of the descriptor to the logical read position. */
static bool sync_read_position(stream *s)
{
    if (s->op == OP_READ && s->rpos < s->rlen)
    {
        if (lseek(s->fd, s->buf_off + (off_t)s->rpos, SEEK_SET) < 0)
        {
            rt_crt_set_errno(msvc_errno(errno));
            s->err = true;
            return false;
        }
    }
    s->rpos = s->rlen = 0;
    return true;
}

static bool begin_read(stream *s)
{
    if (!s->can_read || s->op == OP_WRITE)
    {
        s->err = true;
        return false;
    }
    if (s->op != OP_READ)
    {
        off_t pos = s->std ? 0 : lseek(s->fd, 0, SEEK_CUR);
        if (pos < 0) pos = 0; /* a pipe or a terminal has no position */
        s->buf_off = pos;
        s->rpos = s->rlen = 0;
        s->op = OP_READ;
    }
    return true;
}

static bool begin_write(stream *s)
{
    if (!s->can_write)
    {
        s->err = true;
        return false;
    }
    if (s->op == OP_READ)
    {
        if (!s->eof && (s->rpos < s->rlen || s->unget >= 0))
        {
            s->err = true;
            return false;
        }
        if (!sync_read_position(s)) return false;
        s->eof = false;
        s->op = OP_NONE;
    }
    if (s->op != OP_WRITE)
    {
        s->wlen = 0;
        s->op = OP_WRITE;
    }
    return true;
}

static bool fill(stream *s)
{
    ssize_t n;
    s->buf_off += (off_t)s->rlen;
    s->rpos = s->rlen = 0;
    do
    {
        n = read(s->fd, s->buf, BUFSZ);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
    {
        rt_crt_set_errno(msvc_errno(errno));
        s->err = true;
        return false;
    }
    if (n == 0)
    {
        s->eof = true;
        return false;
    }
    s->rlen = (size_t)n;
    return true;
}

static int raw_getc(stream *s)
{
    if (s->rpos >= s->rlen && !fill(s)) return -1;
    return s->buf[s->rpos++];
}

/* Read one character with the translation of the stream. Call after
   begin_read. Returns -1 at the end of the file or after an error. */
static int stream_getc(stream *s)
{
    int c;
    if (s->unget >= 0)
    {
        c = s->unget;
        s->unget = -1;
        return c;
    }
    if (s->eof) return -1;
    c = raw_getc(s);
    if (c < 0 || !s->text) return c;
    if (c == 0x1A)
    {
        /* Ctrl+Z ends a text file; the character stays unread. */
        s->rpos--;
        s->eof = true;
        return -1;
    }
    if (c == '\r')
    {
        int n = raw_getc(s);
        if (n == '\n') return '\n';
        if (n >= 0) s->rpos--;
        else if (!s->err) s->eof = false; /* the '\r' itself is still a character */
    }
    return c;
}

static bool raw_putc(stream *s, uint8_t c)
{
    if (s->wlen == BUFSZ && !flush_write(s)) return false;
    s->buf[s->wlen++] = c;
    return true;
}

static bool stream_putc(stream *s, uint8_t c)
{
    if (s->text && c == '\n' && !raw_putc(s, '\r')) return false;
    return raw_putc(s, c);
}

/* The end of a public operation: write unbuffered output, publish the
   flags and unlock. */
static void end_op(stream *s)
{
    if (s->unbuffered) flush_write(s);
    update_flag(s);
    if (pthread_mutex_unlock(&s->lock) != 0) rt_fatal("stream unlock failed");
}

/* ---------------------------------------------------------------- interface for rt_crt_fmt.c */

void rt_crt_file_lock(uint32_t file)
{
    lock_stream(get(file, "stream lock"));
}

void rt_crt_file_unlock(uint32_t file)
{
    end_op(get(file, "stream unlock"));
}

int rt_crt_file_putc(uint32_t file, uint8_t c)
{
    stream *s = get(file, "fprintf");
    int r;
    lock_stream(s);
    r = (begin_write(s) && stream_putc(s, c)) ? 0 : -1;
    if (pthread_mutex_unlock(&s->lock) != 0) rt_fatal("stream unlock failed");
    return r;
}

/* ---------------------------------------------------------------- open and close */

void rt___iob_func(_cpu *cpu)
{
    ensure_init();
    RET(iob_base);
}

/* Open a stream. Returns 0 and the guest FILE, or an MSVC errno value. */
static uint32_t open_stream(uint32_t gname, uint32_t gmode, const char *fname, uint32_t *out)
{
    char name[RT_MAX_PATH], host[RT_HOST_PATH];
    const char *m;
    int oflags, fd;
    bool plus = false, text = (rt_crt_fmode() & 0x8000u) == 0, commit = false, del = false, exists;
    bool seen_tb = false;
    uint32_t err, idx, g;
    struct stat st;

    ensure_init();
    *out = 0;
    if (gname == 0 || gmode == 0) rt_crt_invalid_parameter(fname);
    m = G2H(gmode);
    switch (*m)
    {
    case 'r': oflags = O_RDONLY; break;
    case 'w': oflags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': oflags = O_WRONLY | O_CREAT | O_APPEND; break;
    default: rt_crt_invalid_parameter(fname);
    }
    for (m++; *m; m++)
    {
        switch (*m)
        {
        case '+':
            if (plus) rt_crt_invalid_parameter(fname);
            plus = true;
            oflags = (oflags & ~O_ACCMODE) | O_RDWR;
            break;
        case 'b':
        case 't':
            if (seen_tb) rt_crt_invalid_parameter(fname);
            seen_tb = true;
            text = (*m == 't');
            break;
        case 'c': commit = true; break;
        case 'n': commit = false; break;
        case 'D': del = true; break;
        case 'S': /* sequential access hint */
        case 'R': /* random access hint */
        case 'T': /* temporary file hint (no disk flush wanted) */
        case 'N': /* not inherited: the port starts no child processes */
        case ' ':
            break;
        case ',':
            rt_fatal("%s: encoded file modes (\",ccs=\") are not supported", fname);
        default:
            rt_crt_invalid_parameter(fname);
        }
    }

    if (rt_gstr(name, sizeof(name), gname) != 0) return M_ENOENT;
    /* UNCONFIRMED: an empty name in MSVCR80 fopen gives EINVAL without a
       call to the invalid parameter handler. */
    if (name[0] == 0) return M_EINVAL;
    err = rt_path_host(name, host, sizeof(host), &exists);
    if (err != 0) return win32_to_errno(err);
    if (!exists && !(oflags & O_CREAT)) return M_ENOENT;

    fd = open(host, oflags | O_CLOEXEC, 0666);
    if (fd < 0) return msvc_errno(errno);
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode))
    {
        uint32_t e = S_ISDIR(st.st_mode) ? M_EACCES : msvc_errno(errno);
        close(fd);
        return e;
    }

    g = rt_heap_calloc(1, IOB_SIZE);
    if (g == 0)
    {
        close(fd);
        return 12u; /* ENOMEM */
    }
    if (pthread_mutex_lock(&table_lock) != 0) rt_fatal("lock failed");
    for (idx = 3; idx < NSTREAM && atomic_load(&streams[idx].guest) != 0; idx++) {}
    if (idx == NSTREAM)
    {
        pthread_mutex_unlock(&table_lock);
        close(fd);
        rt_heap_free(g);
        return M_EMFILE;
    }
    {
        stream *s = &streams[idx];
        s->fd = fd;
        s->can_read = (oflags & O_ACCMODE) != O_WRONLY;
        s->can_write = (oflags & O_ACCMODE) != O_RDONLY;
        s->text = text;
        s->std = false;
        s->unbuffered = false;
        s->commit = commit;
        s->delete_on_close = del;
        s->eof = s->err = false;
        s->op = OP_NONE;
        s->rpos = s->rlen = s->wlen = 0;
        s->buf_off = 0;
        s->unget = -1;
        snprintf(s->host, sizeof(s->host), "%s", host);
        wr32(g + F_FILE, idx);
        atomic_store(&s->guest, g);
        update_flag(s);
    }
    if (pthread_mutex_unlock(&table_lock) != 0) rt_fatal("unlock failed");
    *out = g;
    return 0;
}

void rt_fopen(_cpu *cpu)
{
    uint32_t f, e = open_stream(ARG(0), ARG(1), "fopen", &f);
    if (e != 0) rt_crt_set_errno(e);
    RET(f);
}

void rt_fopen_s(_cpu *cpu)
{
    uint32_t pf = ARG(0), f, e;
    if (pf == 0) rt_crt_invalid_parameter("fopen_s");
    e = open_stream(ARG(1), ARG(2), "fopen_s", &f);
    wr32(pf, f);
    if (e != 0) rt_crt_set_errno(e);
    RET(e);
}

void rt_fclose(_cpu *cpu)
{
    uint32_t f = ARG(0);
    stream *s = get(f, "fclose");
    bool ok;
    lock_stream(s);
    ok = flush_write(s);
    if (!s->std)
    {
        if (close(s->fd) != 0)
        {
            rt_crt_set_errno(msvc_errno(errno));
            ok = false;
        }
        if (s->delete_on_close && unlink(s->host) != 0)
            rt_log("fclose: delete of %s failed (%s)", s->host, strerror(errno));
    }
    if (s->err) ok = false;
    s->fd = -1;
    s->op = OP_NONE;
    if (pthread_mutex_lock(&table_lock) != 0) rt_fatal("lock failed");
    atomic_store(&s->guest, 0);
    if (pthread_mutex_unlock(&table_lock) != 0) rt_fatal("unlock failed");
    if (pthread_mutex_unlock(&s->lock) != 0) rt_fatal("stream unlock failed");
    if (!s->std) rt_heap_free(f);
    else wr32(f + F_FLAG, 0);
    RET(ok ? 0u : 0xFFFFFFFFu);
}

/* ---------------------------------------------------------------- flush */

static bool flush_stream(stream *s)
{
    bool ok = flush_write(s);
    if (s->op == OP_READ)
    {
        discard_read(s);
        s->unget = -1;
    }
    if (s->can_read && s->can_write) s->op = OP_NONE;
    if (ok && s->commit && !s->std && fsync(s->fd) != 0)
    {
        rt_crt_set_errno(msvc_errno(errno));
        ok = false;
    }
    return ok;
}

void rt_fflush(_cpu *cpu)
{
    uint32_t f = ARG(0);
    bool ok = true;
    if (f == 0)
    {
        uint32_t i;
        ensure_init();
        for (i = 0; i < NSTREAM; i++)
        {
            stream *s = &streams[i];
            if (atomic_load(&s->guest) == 0) continue;
            lock_stream(s);
            if (atomic_load(&s->guest) != 0 && s->op == OP_WRITE && !flush_stream(s)) ok = false;
            end_op(s);
        }
    }
    else
    {
        stream *s = get(f, "fflush");
        lock_stream(s);
        ok = flush_stream(s);
        end_op(s);
    }
    RET(ok ? 0u : 0xFFFFFFFFu);
}

void rt_crt_flush_all(void)
{
    uint32_t i;
    if (streams == NULL) return;
    for (i = 0; i < NSTREAM; i++)
    {
        stream *s = &streams[i];
        if (atomic_load(&s->guest) == 0) continue;
        /* Other threads are suspended at this point; a stream that one of
           them holds cannot be written. */
        if (pthread_mutex_trylock(&s->lock) != 0)
        {
            rt_log("exit: stream %s is locked by a suspended thread; its buffered output is lost", s->host);
            continue;
        }
        flush_write(s);
        pthread_mutex_unlock(&s->lock);
    }
}

/* ---------------------------------------------------------------- characters and blocks */

void rt_fputc(_cpu *cpu)
{
    stream *s = get(ARG(1), "fputc");
    uint8_t c = (uint8_t)ARG(0);
    bool ok;
    lock_stream(s);
    ok = begin_write(s) && stream_putc(s, c);
    end_op(s);
    RET(ok ? (uint32_t)c : 0xFFFFFFFFu);
}

void rt_fputs(_cpu *cpu)
{
    uint32_t str = ARG(0);
    stream *s;
    bool ok;
    if (str == 0) rt_crt_invalid_parameter("fputs");
    s = get(ARG(1), "fputs");
    lock_stream(s);
    ok = begin_write(s);
    for (const char *p = G2H(str); ok && *p; p++) ok = stream_putc(s, (uint8_t)*p);
    end_op(s);
    RET(ok ? 0u : 0xFFFFFFFFu);
}

void rt_getc(_cpu *cpu)
{
    stream *s = get(ARG(0), "getc");
    int c = -1;
    lock_stream(s);
    if (begin_read(s)) c = stream_getc(s);
    end_op(s);
    RET((uint32_t)c);
}

void rt_ungetc(_cpu *cpu)
{
    int32_t c = (int32_t)ARG(0);
    stream *s = get(ARG(1), "ungetc");
    uint32_t r = 0xFFFFFFFFu;
    lock_stream(s);
    if (c != -1 && s->unget < 0 && begin_read(s))
    {
        s->unget = c & 0xFF;
        s->eof = false;
        r = (uint32_t)(c & 0xFF);
    }
    end_op(s);
    RET(r);
}

void rt_fgets(_cpu *cpu)
{
    uint32_t buf = ARG(0), n = 0;
    int32_t count = (int32_t)ARG(1);
    stream *s;
    uint32_t r = buf;
    if ((buf == 0 && count != 0) || count < 0) rt_crt_invalid_parameter("fgets");
    s = get(ARG(2), "fgets");
    if (count == 0)
    {
        RET(0);
        return;
    }
    lock_stream(s);
    if (!begin_read(s))
    {
        r = 0;
    }
    else
    {
        while (n + 1u < (uint32_t)count)
        {
            int c = stream_getc(s);
            if (c < 0)
            {
                if (n == 0) r = 0;
                break;
            }
            wr8(buf + n++, (uint8_t)c);
            if (c == '\n') break;
        }
        if (r != 0) wr8(buf + n, 0);
    }
    end_op(s);
    RET(r);
}

/* Total byte count of size * count, or invalid parameter on overflow. */
static uint32_t block_bytes(uint32_t size, uint32_t count, const char *fname)
{
    uint64_t t = (uint64_t)size * count;
    if (t > 0xFFFFFFFFu) rt_crt_invalid_parameter(fname);
    return (uint32_t)t;
}

void rt_fread(_cpu *cpu)
{
    uint32_t dst = ARG(0), size = ARG(1), count = ARG(2), total, got = 0;
    stream *s;
    if (size == 0 || count == 0)
    {
        RET(0);
        return;
    }
    total = block_bytes(size, count, "fread");
    if (dst == 0) rt_crt_invalid_parameter("fread");
    s = get(ARG(3), "fread");
    lock_stream(s);
    if (begin_read(s))
    {
        if (s->unget >= 0 && got < total)
        {
            wr8(dst + got++, (uint8_t)s->unget);
            s->unget = -1;
        }
        if (s->text)
        {
            while (got < total)
            {
                int c = stream_getc(s);
                if (c < 0) break;
                wr8(dst + got++, (uint8_t)c);
            }
        }
        else
        {
            while (got < total && !s->eof)
            {
                size_t avail = s->rlen - s->rpos, want = total - got;
                if (avail > 0)
                {
                    size_t k = avail < want ? avail : want;
                    memcpy(G2H(dst + got), s->buf + s->rpos, k);
                    s->rpos += k;
                    got += (uint32_t)k;
                }
                else if (want >= BUFSZ)
                {
                    /* Large read: directly into the guest buffer. */
                    ssize_t n;
                    s->buf_off += (off_t)s->rlen;
                    s->rpos = s->rlen = 0;
                    do
                    {
                        n = read(s->fd, G2H(dst + got), want);
                    } while (n < 0 && errno == EINTR);
                    if (n < 0)
                    {
                        rt_crt_set_errno(msvc_errno(errno));
                        s->err = true;
                        break;
                    }
                    if (n == 0)
                    {
                        s->eof = true;
                        break;
                    }
                    s->buf_off += (off_t)n;
                    got += (uint32_t)n;
                }
                else if (!fill(s))
                {
                    break;
                }
            }
        }
    }
    end_op(s);
    RET(got / size);
}

void rt_fwrite(_cpu *cpu)
{
    uint32_t src = ARG(0), size = ARG(1), count = ARG(2), total, put = 0;
    stream *s;
    if (size == 0 || count == 0)
    {
        RET(0);
        return;
    }
    total = block_bytes(size, count, "fwrite");
    if (src == 0) rt_crt_invalid_parameter("fwrite");
    s = get(ARG(3), "fwrite");
    lock_stream(s);
    if (begin_write(s))
    {
        if (s->text)
        {
            while (put < total && stream_putc(s, rd8(src + put))) put++;
        }
        else if (total >= BUFSZ)
        {
            if (flush_write(s) && write_all(s, G2H(src), total)) put = total;
        }
        else
        {
            const uint8_t *p = G2H(src);
            while (put < total && raw_putc(s, p[put])) put++;
        }
    }
    end_op(s);
    RET(put / size);
}

/* ---------------------------------------------------------------- position */

/* The logical position, or -1 (errno set). */
static int64_t stream_tell(stream *s)
{
    off_t pos;
    if (s->op == OP_READ)
        return (int64_t)(s->buf_off + (off_t)s->rpos) - (s->unget >= 0 ? 1 : 0);
    pos = lseek(s->fd, 0, SEEK_CUR);
    if (pos < 0)
    {
        rt_crt_set_errno(msvc_errno(errno));
        return -1;
    }
    if (s->op == OP_WRITE) pos += (off_t)s->wlen;
    return (int64_t)pos;
}

void rt_ftell(_cpu *cpu)
{
    stream *s = get(ARG(0), "ftell");
    int64_t p;
    lock_stream(s);
    p = stream_tell(s);
    end_op(s);
    if (p > 0x7FFFFFFF)
    {
        rt_crt_set_errno(M_EINVAL);
        p = -1;
    }
    RET((uint32_t)(int32_t)p);
}

static bool stream_seek(stream *s, int64_t off, int whence)
{
    off_t r;
    if (whence == SEEK_CUR)
    {
        int64_t cur = stream_tell(s);
        if (cur < 0) return false;
        off += cur;
        whence = SEEK_SET;
    }
    if (!flush_write(s)) return false;
    s->rpos = s->rlen = 0;
    s->unget = -1;
    s->eof = false;
    if (s->can_read && s->can_write) s->op = OP_NONE;
    else if (s->op == OP_READ) s->op = OP_NONE;
    if (whence == SEEK_SET && off < 0)
    {
        rt_crt_set_errno(M_EINVAL);
        return false;
    }
    r = lseek(s->fd, (off_t)off, whence);
    if (r < 0)
    {
        rt_crt_set_errno(msvc_errno(errno));
        return false;
    }
    return true;
}

void rt_fseek(_cpu *cpu)
{
    int32_t off = (int32_t)ARG(1);
    uint32_t whence = ARG(2);
    stream *s;
    bool ok;
    if (whence > 2u) rt_crt_invalid_parameter("fseek");
    s = get(ARG(0), "fseek");
    lock_stream(s);
    ok = stream_seek(s, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    end_op(s);
    RET(ok ? 0u : 0xFFFFFFFFu);
}

void rt_rewind(_cpu *cpu)
{
    stream *s = get(ARG(0), "rewind");
    lock_stream(s);
    stream_seek(s, 0, SEEK_SET);
    s->err = false;
    s->eof = false;
    end_op(s);
}

void rt_feof(_cpu *cpu)
{
    stream *s = get(ARG(0), "feof");
    bool e;
    lock_stream(s);
    e = s->eof && s->unget < 0;
    end_op(s);
    RET(e ? IOEOF : 0u);
}

void rt_ferror(_cpu *cpu)
{
    stream *s = get(ARG(0), "ferror");
    bool e;
    lock_stream(s);
    e = s->err;
    end_op(s);
    RET(e ? IOERR : 0u);
}

/* ---------------------------------------------------------------- _stat64 */

/* struct _stat64 (56 bytes): st_dev 0, st_ino 4, st_mode 6, st_nlink 8,
   st_uid 10, st_gid 12, st_rdev 16, st_size 24, st_atime 32, st_mtime 40,
   st_ctime 48. */
static void write_stat(uint32_t b, uint16_t mode, uint64_t size, int64_t at, int64_t mt, int64_t ct)
{
    memset(G2H(b), 0, 56);
    wr32(b + 0, 2);  /* drive C: */
    wr16(b + 6, mode);
    wr16(b + 8, 1);
    wr32(b + 16, 2);
    wr64(b + 24, size);
    wr64(b + 32, (uint64_t)at);
    wr64(b + 40, (uint64_t)mt);
    wr64(b + 48, (uint64_t)ct);
}

static bool has_exec_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL && (strcasecmp(dot, ".exe") == 0 || strcasecmp(dot, ".com") == 0 ||
                           strcasecmp(dot, ".bat") == 0 || strcasecmp(dot, ".cmd") == 0);
}

static uint16_t replicate_mode(uint16_t m)
{
    return (uint16_t)(m | ((m & 0700u) >> 3) | ((m & 0700u) >> 6));
}

void rt__stat64(_cpu *cpu)
{
    uint32_t gpath = ARG(0), b = ARG(1), err;
    char name[RT_MAX_PATH], full[RT_MAX_PATH], host[RT_HOST_PATH];
    bool exists;
    struct stat st;

    if (gpath == 0 || b == 0) rt_crt_invalid_parameter("_stat64");
    if (rt_gstr(name, sizeof(name), gpath) != 0 || strpbrk(name, "*?") != NULL)
    {
        rt_crt_set_errno(M_ENOENT);
        RET(0xFFFFFFFFu);
        return;
    }
    err = rt_path_full(name, full, sizeof(full));
    if (err == 0) err = rt_path_host(full, host, sizeof(host), &exists);
    if (err == ERROR_ACCESS_DENIED)
    {
        /* A virtual directory ("C:\", "C:\Users"). The Microsoft runtime
           gives root directories the time 1980-01-01 00:00:00 local time. */
        struct tm t = { .tm_year = 80, .tm_mon = 0, .tm_mday = 1, .tm_isdst = -1 };
        int64_t tt = (int64_t)mktime(&t);
        write_stat(b, replicate_mode(0x4000u | 0x100u | 0x80u | 0x40u), 0, tt, tt, tt);
        RET(0);
        return;
    }
    if (err != 0 || !exists || stat(host, &st) != 0)
    {
        rt_crt_set_errno(err != 0 ? win32_to_errno(err) : !exists ? M_ENOENT : msvc_errno(errno));
        RET(0xFFFFFFFFu);
        return;
    }
    {
        uint16_t mode = 0x100u; /* _S_IREAD */
        if (st.st_mode & S_IWUSR) mode |= 0x80u;
        if (S_ISDIR(st.st_mode)) mode |= 0x4000u | 0x40u;
        else
        {
            mode |= 0x8000u;
            if (has_exec_extension(full)) mode |= 0x40u;
        }
        write_stat(b, replicate_mode(mode), S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size,
                   (int64_t)st.st_atimespec.tv_sec, (int64_t)st.st_mtimespec.tv_sec,
                   (int64_t)st.st_birthtimespec.tv_sec);
    }
    RET(0);
}
