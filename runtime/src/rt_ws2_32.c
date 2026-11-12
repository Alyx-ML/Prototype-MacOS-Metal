/*
 * WS2_32 (Windows Sockets 2) on BSD sockets.
 *
 * The engine uses sockets for the Massive AdClient (in-game advertising:
 * DNS lookups and HTTP requests). A guest SOCKET is a handle of the kernel
 * object table (type OBJ_SOCKET) for one host socket.
 *
 * Supported: IPv4 and IPv6 TCP and UDP sockets, blocking and non-blocking
 * mode (FIONBIO), select and getaddrinfo. WSAAsyncGetHostByName and
 * WSACancelAsyncRequest are imports of kind "unreachable" (no translated
 * code calls them; see runtime/imports.spec).
 * Windows behavior that the runtime copies:
 *   - Error codes are WSA codes (WSAEWOULDBLOCK and so on). WSAGetLastError
 *     is the thread's last error value, as GetLastError.
 *   - A non-blocking connect that fails is reported in the exception set
 *     of select, not in the write set. getsockopt(SO_ERROR) then gives the
 *     error.
 *   - Sending on a closed connection gives WSAECONNRESET or WSAESHUTDOWN,
 *     not a signal.
 * Socket options, ioctl commands and address families that the runtime
 * does not translate stop the process with a message.
 */
#include "rt.h"
#include "rt_object.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define INVALID_SOCKET 0xFFFFFFFFu
#define SOCKET_ERROR 0xFFFFFFFFu

#define WSAEINTR 10004u
#define WSAEBADF 10009u
#define WSAEACCES 10013u
#define WSAEFAULT 10014u
#define WSAEINVAL 10022u
#define WSAEMFILE 10024u
#define WSAEWOULDBLOCK 10035u
#define WSAEINPROGRESS 10036u
#define WSAEALREADY 10037u
#define WSAENOTSOCK 10038u
#define WSAEDESTADDRREQ 10039u
#define WSAEMSGSIZE 10040u
#define WSAEPROTOTYPE 10041u
#define WSAENOPROTOOPT 10042u
#define WSAEPROTONOSUPPORT 10043u
#define WSAESOCKTNOSUPPORT 10044u
#define WSAEOPNOTSUPP 10045u
#define WSAEAFNOSUPPORT 10047u
#define WSAEADDRINUSE 10048u
#define WSAEADDRNOTAVAIL 10049u
#define WSAENETDOWN 10050u
#define WSAENETUNREACH 10051u
#define WSAENETRESET 10052u
#define WSAECONNABORTED 10053u
#define WSAECONNRESET 10054u
#define WSAENOBUFS 10055u
#define WSAEISCONN 10056u
#define WSAENOTCONN 10057u
#define WSAESHUTDOWN 10058u
#define WSAETIMEDOUT 10060u
#define WSAECONNREFUSED 10061u
#define WSAEHOSTDOWN 10064u
#define WSAEHOSTUNREACH 10065u
#define WSAVERNOTSUPPORTED 10092u
#define WSANOTINITIALISED 10093u
#define WSATYPE_NOT_FOUND 10109u
#define WSAHOST_NOT_FOUND 11001u
#define WSATRY_AGAIN 11002u
#define WSANO_RECOVERY 11003u
#define WSANO_DATA 11004u
#define WSA_NOT_ENOUGH_MEMORY 8u

/* Windows constants. */
#define W_AF_INET 2u
#define W_AF_INET6 23u
#define W_SOL_SOCKET 0xFFFFu
#define W_SO_REUSEADDR 0x0004u
#define W_SO_KEEPALIVE 0x0008u
#define W_SO_BROADCAST 0x0020u
#define W_SO_LINGER 0x0080u
#define W_SO_DONTLINGER 0xFF7Fu
#define W_SO_SNDBUF 0x1001u
#define W_SO_RCVBUF 0x1002u
#define W_SO_SNDTIMEO 0x1005u
#define W_SO_RCVTIMEO 0x1006u
#define W_SO_ERROR 0x1007u
#define W_SO_TYPE 0x1008u
#define W_IPPROTO_TCP 6u
#define W_TCP_NODELAY 1u
#define W_FIONBIO 0x8004667Eu
#define W_FIONREAD 0x4004667Fu
#define W_MSG_OOB 1u
#define W_MSG_PEEK 2u
#define W_MSG_DONTROUTE 4u
#define W_FD_SETSIZE 64u

typedef struct wsock {
    int fd;
    int type;              /* SOCK_STREAM or SOCK_DGRAM */
    bool nonblocking;
    bool connect_pending;  /* non-blocking connect in progress */
    uint32_t error;        /* SO_ERROR value (WSA code) */
} wsock;

static pthread_mutex_t ws_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t startup_count;

static void lock(void)
{
    if (pthread_mutex_lock(&ws_lock) != 0) rt_fatal("Winsock lock failed");
}

static void unlock(void)
{
    if (pthread_mutex_unlock(&ws_lock) != 0) rt_fatal("Winsock unlock failed");
}

static bool started(void)
{
    bool r;
    lock();
    r = startup_count > 0;
    unlock();
    if (!r) rt_set_last_error(WSANOTINITIALISED);
    return r;
}

static uint32_t wsa_error(int e)
{
    switch (e)
    {
    case EINTR: return WSAEINTR;
    case EBADF: return WSAENOTSOCK;
    case EACCES: case EPERM: return WSAEACCES;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: case ENFILE: return WSAEMFILE;
    case EAGAIN: return WSAEWOULDBLOCK;
    case EINPROGRESS: return WSAEWOULDBLOCK; /* Windows reports a pending connect as WSAEWOULDBLOCK */
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return WSAEDESTADDRREQ;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EPROTOTYPE: return WSAEPROTOTYPE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case ESOCKTNOSUPPORT: return WSAESOCKTNOSUPPORT;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ENETRESET: return WSAENETRESET;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: case EPIPE: return WSAECONNRESET;
    case ENOBUFS: case ENOMEM: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ESHUTDOWN: return WSAESHUTDOWN;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case EHOSTDOWN: return WSAEHOSTDOWN;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    default:
        rt_log("Winsock: host error %d (%s) has no WSA code; reported as WSAEINVAL", e, strerror(e));
        return WSAEINVAL;
    }
}

static uint32_t fail(uint32_t code)
{
    rt_set_last_error(code);
    return SOCKET_ERROR;
}

static uint32_t fail_errno(void)
{
    return fail(wsa_error(errno));
}

/* The socket of handle s with one new reference, or NULL (error set). */
static rt_object *sock_get(uint32_t s)
{
    rt_object *o;
    if (!started()) return NULL;
    o = rt_handle_get(s, OBJ_SOCKET);
    if (o == NULL) rt_set_last_error(WSAENOTSOCK);
    return o;
}

static void sock_destroy(rt_object *o)
{
    wsock *w = o->data;
    if (w != NULL)
    {
        if (close(w->fd) != 0) rt_log("Winsock: close of socket fd %d failed: %s", w->fd, strerror(errno));
        free(w);
    }
}

/* ---------------------------------------------------------------- addresses */

/* Guest sockaddr to host sockaddr. Returns false (error set) if the
   address is not valid. */
static bool addr_in(uint32_t g, int32_t len, struct sockaddr_storage *h, socklen_t *hlen)
{
    uint16_t fam;
    memset(h, 0, sizeof *h);
    if (g == 0 || len < 2)
    {
        rt_set_last_error(WSAEFAULT);
        return false;
    }
    fam = rd16(g);
    if (fam == W_AF_INET)
    {
        struct sockaddr_in *a = (struct sockaddr_in *)h;
        if (len < 16)
        {
            rt_set_last_error(WSAEFAULT);
            return false;
        }
        a->sin_len = sizeof *a;
        a->sin_family = AF_INET;
        memcpy(&a->sin_port, G2H(g + 2), 2);
        memcpy(&a->sin_addr, G2H(g + 4), 4);
        *hlen = sizeof *a;
        return true;
    }
    if (fam == W_AF_INET6)
    {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)h;
        if (len < 24)
        {
            rt_set_last_error(WSAEFAULT);
            return false;
        }
        a->sin6_len = sizeof *a;
        a->sin6_family = AF_INET6;
        memcpy(&a->sin6_port, G2H(g + 2), 2);
        memcpy(&a->sin6_flowinfo, G2H(g + 4), 4);
        memcpy(&a->sin6_addr, G2H(g + 8), 16);
        if (len >= 28) memcpy(&a->sin6_scope_id, G2H(g + 24), 4);
        *hlen = sizeof *a;
        return true;
    }
    rt_set_last_error(WSAEAFNOSUPPORT);
    return false;
}

/* Windows form of a host sockaddr in buf (28 bytes). Returns the length
   (16 for IPv4, 28 for IPv6). */
static uint32_t addr_windows(const struct sockaddr *h, uint8_t buf[28])
{
    memset(buf, 0, 28);
    if (h->sa_family == AF_INET)
    {
        const struct sockaddr_in *a = (const struct sockaddr_in *)h;
        uint16_t fam = W_AF_INET;
        memcpy(buf, &fam, 2);
        memcpy(buf + 2, &a->sin_port, 2);
        memcpy(buf + 4, &a->sin_addr, 4);
        return 16;
    }
    if (h->sa_family == AF_INET6)
    {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)h;
        uint16_t fam = W_AF_INET6;
        memcpy(buf, &fam, 2);
        memcpy(buf + 2, &a->sin6_port, 2);
        memcpy(buf + 4, &a->sin6_flowinfo, 4);
        memcpy(buf + 8, &a->sin6_addr, 16);
        memcpy(buf + 24, &a->sin6_scope_id, 4);
        return 28;
    }
    rt_fatal("Winsock: host address family %d has no Windows form", h->sa_family);
}

/* Host sockaddr to the guest sockaddr at g, whose capacity is the int at
   plen (updated to the length). Returns false (WSAEFAULT) if the buffer
   is too small. */
static bool addr_out(const struct sockaddr *h, uint32_t g, uint32_t plen)
{
    uint8_t buf[28];
    uint32_t n = addr_windows(h, buf);
    if (g == 0) return true;
    if (plen == 0 || (int32_t)rd32(plen) < (int32_t)n)
    {
        rt_set_last_error(WSAEFAULT);
        return false;
    }
    memcpy(G2H(g), buf, n);
    wr32(plen, n);
    return true;
}

/* ---------------------------------------------------------------- startup */

/* int WSAStartup(WORD version, LPWSADATA) */
void rt_ws_WSAStartup(_cpu *cpu)
{
    uint32_t req = ARG(0) & 0xFFFFu, data = ARG(1), ver;
    uint8_t major = (uint8_t)req, minor = (uint8_t)(req >> 8);
    if (major < 1)
    {
        RET(WSAVERNOTSUPPORTED);
        return;
    }
    if (data == 0)
    {
        RET(WSAEFAULT);
        return;
    }
    /* The highest version is 2.2. */
    if (major > 2 || (major == 2 && minor > 2))
        ver = 0x0202u;
    else
        ver = req;
    memset(G2H(data), 0, 400);
    wr16(data, (uint16_t)ver);
    wr16(data + 2, 0x0202u);
    memcpy(G2H(data + 4), "WinSock 2.0", 12);
    memcpy(G2H(data + 261), "Running", 8);
    if (major < 2)
    {
        wr16(data + 390, 32767); /* iMaxSockets, used by Windows Sockets 1.1 only */
        wr16(data + 392, 65467); /* iMaxUdpDg */
    }
    lock();
    if (startup_count == UINT32_MAX) rt_fatal("WSAStartup count overflow");
    startup_count++;
    unlock();
    RET(0);
}

/* int WSACleanup(void) */
void rt_ws_WSACleanup(_cpu *cpu)
{
    lock();
    if (startup_count == 0)
    {
        unlock();
        RET(fail(WSANOTINITIALISED));
        return;
    }
    startup_count--;
    unlock();
    RET(0);
}

/* int WSAGetLastError(void) */
void rt_ws_WSAGetLastError(_cpu *cpu)
{
    RET(rt_get_last_error());
}

/* ---------------------------------------------------------------- byte order and text addresses */

void rt_ws_htonl(_cpu *cpu) { RET(htonl(ARG(0))); }
void rt_ws_ntohl(_cpu *cpu) { RET(ntohl(ARG(0))); }
void rt_ws_htons(_cpu *cpu) { RET(htons((uint16_t)ARG(0))); }
void rt_ws_ntohs(_cpu *cpu) { RET(ntohs((uint16_t)ARG(0))); }

/* unsigned long inet_addr(const char *): the forms a.b.c.d, a.b.c, a.b
   and a, with decimal, octal and hexadecimal parts. An empty string gives
   INADDR_ANY (Windows XP behavior); text that is not an address gives
   INADDR_NONE. */
void rt_ws_inet_addr(_cpu *cpu)
{
    char text[256];
    struct in_addr a;
    size_t i;
    if (ARG(0) == 0 || rt_gstr(text, sizeof text, ARG(0)) != 0)
    {
        RET(0xFFFFFFFFu);
        return;
    }
    for (i = 0; text[i] == ' '; i++)
        ;
    if (text[i] == 0)
    {
        RET(0);
        return;
    }
    if (inet_aton(text, &a) == 0)
    {
        RET(0xFFFFFFFFu);
        return;
    }
    RET(a.s_addr);
}

/* ---------------------------------------------------------------- sockets */

/* SOCKET socket(int af, int type, int protocol) */
void rt_ws_socket(_cpu *cpu)
{
    uint32_t af = ARG(0), type = ARG(1), proto = ARG(2), h;
    int fd, haf, one = 1;
    wsock *w;
    rt_object *o;
    if (!started())
    {
        RET(INVALID_SOCKET);
        return;
    }
    if (af == W_AF_INET)
        haf = AF_INET;
    else if (af == W_AF_INET6)
        haf = AF_INET6;
    else
    {
        RET(fail(WSAEAFNOSUPPORT));
        return;
    }
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
    {
        RET(fail(WSAESOCKTNOSUPPORT));
        return;
    }
    if (proto != 0 && proto != IPPROTO_TCP && proto != IPPROTO_UDP)
    {
        RET(fail(WSAEPROTONOSUPPORT));
        return;
    }
    fd = socket(haf, (int)type, (int)proto);
    if (fd < 0)
    {
        RET(fail_errno());
        return;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0)
    {
        int e = errno;
        close(fd);
        RET(fail(wsa_error(e)));
        return;
    }
    w = calloc(1, sizeof *w);
    if (w == NULL) rt_fatal("out of host memory");
    w->fd = fd;
    w->type = (int)type;
    o = rt_object_new(OBJ_SOCKET);
    o->data = w;
    o->destroy = sock_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o); /* closes the socket if there is no handle */
    if (h == 0)
    {
        RET(fail(WSAEMFILE));
        return;
    }
    RET(h);
}

/* int closesocket(SOCKET) */
void rt_ws_closesocket(_cpu *cpu)
{
    rt_object *o = sock_get(ARG(0));
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    rt_object_unref(o);
    if (!rt_handle_close(ARG(0)))
    {
        RET(fail(WSAENOTSOCK));
        return;
    }
    RET(0);
}

/* int bind(SOCKET, const struct sockaddr *, int) */
void rt_ws_bind(_cpu *cpu)
{
    struct sockaddr_storage a;
    socklen_t n;
    rt_object *o = sock_get(ARG(0));
    uint32_t r = 0;
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    if (!addr_in(ARG(1), (int32_t)ARG(2), &a, &n))
        r = SOCKET_ERROR;
    else if (bind(((wsock *)o->data)->fd, (struct sockaddr *)&a, n) != 0)
        r = fail_errno();
    rt_object_unref(o);
    RET(r);
}

/* int listen(SOCKET, int backlog) */
void rt_ws_listen(_cpu *cpu)
{
    rt_object *o = sock_get(ARG(0));
    int backlog = (int)ARG(1);
    uint32_t r = 0;
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    if (backlog < 0) backlog = 0;
    if (backlog > SOMAXCONN) backlog = SOMAXCONN; /* Windows SOMAXCONN is 0x7FFFFFFF: the system maximum */
    if (listen(((wsock *)o->data)->fd, backlog) != 0) r = fail_errno();
    rt_object_unref(o);
    RET(r);
}

/* SOCKET accept(SOCKET, struct sockaddr *, int *) */
void rt_ws_accept(_cpu *cpu)
{
    struct sockaddr_storage a;
    socklen_t n = sizeof a;
    rt_object *o = sock_get(ARG(0)), *no;
    wsock *w, *nw;
    int fd, one = 1;
    uint32_t h;
    bool nonblocking;
    if (o == NULL)
    {
        RET(INVALID_SOCKET);
        return;
    }
    w = o->data;
    fd = accept(w->fd, (struct sockaddr *)&a, &n);
    nonblocking = w->nonblocking;
    if (fd < 0)
    {
        uint32_t e = wsa_error(errno);
        rt_object_unref(o);
        RET(fail(e));
        return;
    }
    rt_object_unref(o);
    /* The new socket has the properties of the listening socket
       (non-blocking mode included), as on Windows. */
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0 ||
        fcntl(fd, F_SETFL, nonblocking ? O_NONBLOCK : 0) != 0)
    {
        int e = errno;
        close(fd);
        RET(fail(wsa_error(e)));
        return;
    }
    if (ARG(1) != 0 && !addr_out((struct sockaddr *)&a, ARG(1), ARG(2)))
    {
        close(fd);
        RET(INVALID_SOCKET);
        return;
    }
    nw = calloc(1, sizeof *nw);
    if (nw == NULL) rt_fatal("out of host memory");
    nw->fd = fd;
    nw->type = SOCK_STREAM;
    nw->nonblocking = nonblocking;
    no = rt_object_new(OBJ_SOCKET);
    no->data = nw;
    no->destroy = sock_destroy;
    h = rt_handle_new(no);
    rt_object_unref(no);
    if (h == 0)
    {
        RET(fail(WSAEMFILE));
        return;
    }
    RET(h);
}

/* int connect(SOCKET, const struct sockaddr *, int) */
void rt_ws_connect(_cpu *cpu)
{
    struct sockaddr_storage a;
    socklen_t n;
    rt_object *o = sock_get(ARG(0));
    wsock *w;
    uint32_t r = 0;
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    w = o->data;
    if (!addr_in(ARG(1), (int32_t)ARG(2), &a, &n))
    {
        r = SOCKET_ERROR;
    }
    else if (connect(w->fd, (struct sockaddr *)&a, n) != 0)
    {
        int e = errno;
        lock();
        if (e == EINPROGRESS)
        {
            w->connect_pending = true;
            w->error = 0;
        }
        unlock();
        r = fail(wsa_error(e));
    }
    rt_object_unref(o);
    RET(r);
}

static uint32_t name_query(_cpu *cpu, bool peer)
{
    struct sockaddr_storage a;
    socklen_t n = sizeof a;
    rt_object *o = sock_get(ARG(0));
    int rc;
    uint32_t r = 0;
    if (o == NULL) return SOCKET_ERROR;
    rc = peer ? getpeername(((wsock *)o->data)->fd, (struct sockaddr *)&a, &n)
              : getsockname(((wsock *)o->data)->fd, (struct sockaddr *)&a, &n);
    if (rc != 0)
        r = fail_errno();
    else if (ARG(1) == 0 || !addr_out((struct sockaddr *)&a, ARG(1), ARG(2)))
        r = fail(WSAEFAULT);
    rt_object_unref(o);
    return r;
}

/* int getpeername(SOCKET, struct sockaddr *, int *) */
void rt_ws_getpeername(_cpu *cpu) { RET(name_query(cpu, true)); }
/* int getsockname(SOCKET, struct sockaddr *, int *) */
void rt_ws_getsockname(_cpu *cpu) { RET(name_query(cpu, false)); }

static int host_msg_flags(uint32_t f, bool *ok)
{
    int h = 0;
    *ok = (f & ~(W_MSG_OOB | W_MSG_PEEK | W_MSG_DONTROUTE)) == 0;
    if (f & W_MSG_OOB) h |= MSG_OOB;
    if (f & W_MSG_PEEK) h |= MSG_PEEK;
    if (f & W_MSG_DONTROUTE) h |= MSG_DONTROUTE;
    return h;
}

/* Check a guest buffer for a transfer of len bytes. */
static bool buffer_ok(uint32_t buf, int32_t len)
{
    if (len < 0) return false;
    if (len == 0) return true;
    return buf != 0 && rt_va_is_committed(buf, (uint32_t)len);
}

static uint32_t do_send(_cpu *cpu, bool to)
{
    uint32_t buf = ARG(1), flags = ARG(3);
    int32_t len = (int32_t)ARG(2);
    struct sockaddr_storage a;
    socklen_t n = 0;
    rt_object *o = sock_get(ARG(0));
    bool ok;
    int hf, e;
    ssize_t r;
    if (o == NULL) return SOCKET_ERROR;
    hf = host_msg_flags(flags, &ok);
    if (!ok || !buffer_ok(buf, len))
    {
        rt_object_unref(o);
        return fail(ok ? WSAEFAULT : WSAEOPNOTSUPP);
    }
    if (to && ARG(4) != 0 && !addr_in(ARG(4), (int32_t)ARG(5), &a, &n))
    {
        rt_object_unref(o);
        return SOCKET_ERROR;
    }
    if (to && ARG(4) != 0)
        r = sendto(((wsock *)o->data)->fd, G2H(buf), (size_t)len, hf, (struct sockaddr *)&a, n);
    else
        r = send(((wsock *)o->data)->fd, len ? G2H(buf) : "", (size_t)len, hf);
    e = r < 0 ? errno : 0;
    rt_object_unref(o);
    if (r < 0) return fail(wsa_error(e));
    return (uint32_t)r;
}

/* int send(SOCKET, const char *, int, int) */
void rt_ws_send(_cpu *cpu) { RET(do_send(cpu, false)); }
/* int sendto(SOCKET, const char *, int, int, const struct sockaddr *, int) */
void rt_ws_sendto(_cpu *cpu) { RET(do_send(cpu, true)); }

static uint32_t do_recv(_cpu *cpu, bool from)
{
    uint32_t buf = ARG(1), flags = ARG(3);
    int32_t len = (int32_t)ARG(2);
    struct sockaddr_storage a;
    socklen_t n = sizeof a;
    rt_object *o = sock_get(ARG(0));
    wsock *w;
    bool ok;
    int hf;
    ssize_t r;
    uint8_t dummy;
    bool dgram, truncated = false;
    int e = 0;
    if (o == NULL) return SOCKET_ERROR;
    w = o->data;
    hf = host_msg_flags(flags, &ok);
    if (!ok || !buffer_ok(buf, len))
    {
        rt_object_unref(o);
        return fail(ok ? WSAEFAULT : WSAEOPNOTSUPP);
    }
    dgram = w->type == SOCK_DGRAM;
    if (dgram)
    {
        /* recvmsg reports a datagram larger than the buffer with MSG_TRUNC
           in msg_flags (macOS does not accept MSG_TRUNC as an input flag). */
        struct iovec iov = { len ? G2H(buf) : &dummy, (size_t)len };
        struct msghdr m;
        memset(&m, 0, sizeof m);
        m.msg_name = &a;
        m.msg_namelen = n;
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        r = recvmsg(w->fd, &m, hf);
        n = m.msg_namelen;
        truncated = r >= 0 && (m.msg_flags & MSG_TRUNC) != 0;
    }
    else if (from)
        r = recvfrom(w->fd, len ? G2H(buf) : &dummy, (size_t)len, hf, (struct sockaddr *)&a, &n);
    else
        r = recv(w->fd, len ? G2H(buf) : &dummy, (size_t)len, hf);
    if (r < 0) e = errno;
    rt_object_unref(o);
    if (r < 0) return fail(wsa_error(e));
    /* A datagram larger than the buffer: Windows copies the first part and
       returns WSAEMSGSIZE. */
    if (truncated) return fail(WSAEMSGSIZE);
    if (from && ARG(4) != 0 && n > 0 && !addr_out((struct sockaddr *)&a, ARG(4), ARG(5))) return fail(WSAEFAULT);
    return (uint32_t)r;
}

/* int recv(SOCKET, char *, int, int) */
void rt_ws_recv(_cpu *cpu) { RET(do_recv(cpu, false)); }
/* int recvfrom(SOCKET, char *, int, int, struct sockaddr *, int *) */
void rt_ws_recvfrom(_cpu *cpu) { RET(do_recv(cpu, true)); }

/* int shutdown(SOCKET, int how): SD_RECEIVE 0, SD_SEND 1, SD_BOTH 2 */
void rt_ws_shutdown(_cpu *cpu)
{
    rt_object *o = sock_get(ARG(0));
    uint32_t r = 0;
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    if (ARG(1) > 2)
        r = fail(WSAEINVAL);
    else if (shutdown(((wsock *)o->data)->fd, (int)ARG(1)) != 0)
        r = fail_errno();
    rt_object_unref(o);
    RET(r);
}

/* int ioctlsocket(SOCKET, long cmd, u_long *arg) */
void rt_ws_ioctlsocket(_cpu *cpu)
{
    rt_object *o = sock_get(ARG(0));
    uint32_t cmd = ARG(1), arg = ARG(2), r = 0;
    wsock *w;
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    w = o->data;
    if (arg == 0)
    {
        r = fail(WSAEFAULT);
    }
    else if (cmd == W_FIONBIO)
    {
        bool nb = rd32(arg) != 0;
        int fl = fcntl(w->fd, F_GETFL);
        if (fl < 0 || fcntl(w->fd, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) != 0)
            r = fail_errno();
        else
            w->nonblocking = nb;
    }
    else if (cmd == W_FIONREAD)
    {
        int avail = 0;
        if (ioctl(w->fd, FIONREAD, &avail) != 0)
            r = fail_errno();
        else
            wr32(arg, (uint32_t)(avail < 0 ? 0 : avail));
    }
    else
    {
        rt_fatal("ioctlsocket command 0x%08x is not supported", cmd);
    }
    rt_object_unref(o);
    RET(r);
}

/* Translate a socket option: level and name. Returns false for options
   that the runtime does not support. */
static bool host_option(uint32_t level, uint32_t name, int *hl, int *hn)
{
    if (level == W_SOL_SOCKET)
    {
        *hl = SOL_SOCKET;
        switch (name)
        {
        case W_SO_REUSEADDR: *hn = SO_REUSEADDR; return true;
        case W_SO_KEEPALIVE: *hn = SO_KEEPALIVE; return true;
        case W_SO_BROADCAST: *hn = SO_BROADCAST; return true;
        case W_SO_LINGER: case W_SO_DONTLINGER: *hn = SO_LINGER; return true;
        case W_SO_SNDBUF: *hn = SO_SNDBUF; return true;
        case W_SO_RCVBUF: *hn = SO_RCVBUF; return true;
        case W_SO_SNDTIMEO: *hn = SO_SNDTIMEO; return true;
        case W_SO_RCVTIMEO: *hn = SO_RCVTIMEO; return true;
        case W_SO_ERROR: *hn = SO_ERROR; return true;
        case W_SO_TYPE: *hn = SO_TYPE; return true;
        default: return false;
        }
    }
    if (level == W_IPPROTO_TCP && name == W_TCP_NODELAY)
    {
        *hl = IPPROTO_TCP;
        *hn = TCP_NODELAY;
        return true;
    }
    return false;
}

/* int setsockopt(SOCKET, int level, int name, const char *value, int len) */
void rt_ws_setsockopt(_cpu *cpu)
{
    uint32_t level = ARG(1), name = ARG(2), val = ARG(3), r = 0;
    int32_t len = (int32_t)ARG(4);
    rt_object *o;
    wsock *w;
    int hl, hn, rc, iv;
    if (!host_option(level, name, &hl, &hn) || name == W_SO_ERROR || name == W_SO_TYPE)
        rt_fatal("setsockopt level 0x%x option 0x%x is not supported", level, name);
    o = sock_get(ARG(0));
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    w = o->data;
    if (val == 0 || len < 0 || !rt_va_is_committed(val, (uint32_t)len))
    {
        rt_object_unref(o);
        RET(fail(WSAEFAULT));
        return;
    }
    if (name == W_SO_LINGER && level == W_SOL_SOCKET)
    {
        /* struct linger { u_short l_onoff; u_short l_linger; } */
        struct linger lg;
        if (len < 4)
        {
            rt_object_unref(o);
            RET(fail(WSAEFAULT));
            return;
        }
        lg.l_onoff = rd16(val) != 0;
        lg.l_linger = rd16(val + 2);
        rc = setsockopt(w->fd, hl, hn, &lg, sizeof lg);
    }
    else if (name == W_SO_DONTLINGER && level == W_SOL_SOCKET)
    {
        struct linger lg = { 0, 0 };
        lg.l_onoff = len >= 1 && rd8(val) == 0; /* SO_DONTLINGER true: linger off */
        rc = setsockopt(w->fd, hl, hn, &lg, sizeof lg);
    }
    else if ((name == W_SO_SNDTIMEO || name == W_SO_RCVTIMEO) && level == W_SOL_SOCKET)
    {
        /* Windows: a DWORD in milliseconds. */
        struct timeval tv;
        uint32_t ms;
        if (len < 4)
        {
            rt_object_unref(o);
            RET(fail(WSAEFAULT));
            return;
        }
        ms = rd32(val);
        tv.tv_sec = (time_t)(ms / 1000u);
        tv.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
        rc = setsockopt(w->fd, hl, hn, &tv, sizeof tv);
    }
    else
    {
        /* Boolean and integer options: Windows accepts a one-byte BOOL. */
        if (len >= 4)
            iv = (int)rd32(val);
        else if (len >= 1)
            iv = rd8(val);
        else
        {
            rt_object_unref(o);
            RET(fail(WSAEFAULT));
            return;
        }
        rc = setsockopt(w->fd, hl, hn, &iv, sizeof iv);
    }
    if (rc != 0) r = fail_errno();
    rt_object_unref(o);
    RET(r);
}

/* int getsockopt(SOCKET, int level, int name, char *value, int *len) */
void rt_ws_getsockopt(_cpu *cpu)
{
    uint32_t level = ARG(1), name = ARG(2), val = ARG(3), plen = ARG(4), r = 0, out;
    rt_object *o;
    wsock *w;
    int hl, hn, iv = 0;
    socklen_t n = sizeof iv;
    if (!host_option(level, name, &hl, &hn) || name == W_SO_LINGER || name == W_SO_DONTLINGER ||
        name == W_SO_SNDTIMEO || name == W_SO_RCVTIMEO)
        rt_fatal("getsockopt level 0x%x option 0x%x is not supported", level, name);
    o = sock_get(ARG(0));
    if (o == NULL)
    {
        RET(SOCKET_ERROR);
        return;
    }
    w = o->data;
    if (val == 0 || plen == 0 || (int32_t)rd32(plen) < 4)
    {
        rt_object_unref(o);
        RET(fail(WSAEFAULT));
        return;
    }
    if (name == W_SO_ERROR && level == W_SOL_SOCKET)
    {
        lock();
        out = w->error;
        w->error = 0;
        unlock();
        if (out == 0)
        {
            if (getsockopt(w->fd, SOL_SOCKET, SO_ERROR, &iv, &n) != 0)
            {
                r = fail_errno();
                rt_object_unref(o);
                RET(r);
                return;
            }
            out = iv != 0 ? wsa_error(iv) : 0;
        }
    }
    else
    {
        if (getsockopt(w->fd, hl, hn, &iv, &n) != 0)
        {
            r = fail_errno();
            rt_object_unref(o);
            RET(r);
            return;
        }
        out = (uint32_t)iv;
        if (hn == SO_REUSEADDR || hn == SO_KEEPALIVE || hn == SO_BROADCAST || hn == TCP_NODELAY) out = iv != 0;
    }
    wr32(val, out);
    wr32(plen, 4);
    rt_object_unref(o);
    RET(0);
}

/* ---------------------------------------------------------------- select */

/* int __WSAFDIsSet(SOCKET, fd_set *) */
void rt_ws___WSAFDIsSet(_cpu *cpu)
{
    uint32_t s = ARG(0), set = ARG(1), n, i;
    if (set == 0)
    {
        RET(0);
        return;
    }
    n = rd32(set);
    if (n > W_FD_SETSIZE) n = W_FD_SETSIZE;
    for (i = 0; i < n; i++)
    {
        if (rd32(set + 4 + 4 * i) == s)
        {
            RET(1);
            return;
        }
    }
    RET(0);
}

#define SEL_READ 1
#define SEL_WRITE 2
#define SEL_EXCEPT 4

/* int select(int nfds, fd_set *read, fd_set *write, fd_set *except,
   const struct timeval *timeout) */
void rt_ws_select(_cpu *cpu)
{
    uint32_t sets[3] = { ARG(1), ARG(2), ARG(3) }, tv = ARG(4);
    struct pollfd pfd[3 * W_FD_SETSIZE];
    rt_object *objs[3 * W_FD_SETSIZE];
    uint32_t handles[3 * W_FD_SETSIZE];
    int kind[3 * W_FD_SETSIZE];
    uint8_t ready[3 * W_FD_SETSIZE];
    uint32_t count = 0, k, i, total = 0;
    int timeout_ms, rc;
    if (!started())
    {
        RET(SOCKET_ERROR);
        return;
    }
    for (k = 0; k < 3; k++)
    {
        uint32_t n;
        if (sets[k] == 0) continue;
        n = rd32(sets[k]);
        if (n > W_FD_SETSIZE)
        {
            for (i = 0; i < count; i++) rt_object_unref(objs[i]);
            RET(fail(WSAEINVAL));
            return;
        }
        for (i = 0; i < n; i++)
        {
            uint32_t h = rd32(sets[k] + 4 + 4 * i);
            rt_object *o = rt_handle_get(h, OBJ_SOCKET);
            wsock *w;
            if (o == NULL)
            {
                uint32_t j;
                for (j = 0; j < count; j++) rt_object_unref(objs[j]);
                RET(fail(WSAENOTSOCK));
                return;
            }
            w = o->data;
            objs[count] = o;
            handles[count] = h;
            kind[count] = 1 << k;
            pfd[count].fd = w->fd;
            pfd[count].events = (short)(k == 0 ? POLLIN : k == 1 ? POLLOUT : POLLPRI);
            if (k == 2 && w->connect_pending) pfd[count].events |= POLLOUT;
            pfd[count].revents = 0;
            ready[count] = 0;
            count++;
        }
    }
    if (count == 0)
    {
        RET(fail(WSAEINVAL)); /* Windows: all sets empty */
        return;
    }
    if (tv == 0)
    {
        timeout_ms = -1;
    }
    else
    {
        int64_t ms = (int64_t)(int32_t)rd32(tv) * 1000 + ((int64_t)(int32_t)rd32(tv + 4) + 999) / 1000;
        if (ms < 0) ms = 0;
        timeout_ms = ms > INT32_MAX ? INT32_MAX : (int)ms;
    }
    do
        rc = poll(pfd, count, timeout_ms);
    while (rc < 0 && errno == EINTR && timeout_ms < 0);
    if (rc < 0)
    {
        uint32_t e = wsa_error(errno);
        for (i = 0; i < count; i++) rt_object_unref(objs[i]);
        RET(fail(e));
        return;
    }
    for (i = 0; i < count; i++)
    {
        wsock *w = objs[i]->data;
        short re = pfd[i].revents;
        if (re == 0) continue;
        if (w->connect_pending && (kind[i] == SEL_WRITE || kind[i] == SEL_EXCEPT) && (re & (POLLOUT | POLLERR | POLLHUP)))
        {
            /* The end of a non-blocking connect: success goes to the write
               set, failure to the exception set. */
            int err = 0;
            socklen_t n = sizeof err;
            if (getsockopt(w->fd, SOL_SOCKET, SO_ERROR, &err, &n) != 0) err = errno;
            lock();
            if (w->connect_pending)
            {
                w->connect_pending = false;
                w->error = err != 0 ? wsa_error(err) : 0;
            }
            unlock();
            if (err == 0 && kind[i] == SEL_WRITE) ready[i] = 1;
            if (err != 0 && kind[i] == SEL_EXCEPT) ready[i] = 1;
            /* The other set of the same socket in the same call. */
            {
                uint32_t j;
                for (j = 0; j < count; j++)
                {
                    if (objs[j] != objs[i] || j == i) continue;
                    if (err == 0 && kind[j] == SEL_WRITE) ready[j] = 1;
                    if (err != 0 && kind[j] == SEL_EXCEPT) ready[j] = 1;
                }
            }
            continue;
        }
        if (kind[i] == SEL_READ && (re & (POLLIN | POLLHUP | POLLERR))) ready[i] = 1;
        if (kind[i] == SEL_WRITE && (re & (POLLOUT | POLLERR | POLLHUP))) ready[i] = 1;
        if (kind[i] == SEL_EXCEPT && (re & POLLPRI)) ready[i] = 1;
    }
    /* Write back the sets: each set keeps only its ready sockets. */
    {
        uint32_t idx = 0;
        for (k = 0; k < 3; k++)
        {
            uint32_t n, kept = 0;
            if (sets[k] == 0) continue;
            n = rd32(sets[k]);
            for (i = 0; i < n; i++, idx++)
            {
                if (ready[idx])
                {
                    wr32(sets[k] + 4 + 4 * kept, handles[idx]);
                    kept++;
                }
            }
            wr32(sets[k], kept);
            total += kept;
        }
    }
    for (i = 0; i < count; i++) rt_object_unref(objs[i]);
    RET(total);
}

/* ---------------------------------------------------------------- name resolution */

static uint32_t eai_to_wsa(int e)
{
    switch (e)
    {
    case EAI_AGAIN: return WSATRY_AGAIN;
    case EAI_BADFLAGS: return WSAEINVAL;
    case EAI_FAIL: return WSANO_RECOVERY;
    case EAI_FAMILY: return WSAEAFNOSUPPORT;
    case EAI_MEMORY: return WSA_NOT_ENOUGH_MEMORY;
    case EAI_NONAME: return WSAHOST_NOT_FOUND;
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
    case EAI_NODATA: return WSANO_DATA;
#endif
    case EAI_SERVICE: return WSATYPE_NOT_FOUND;
    case EAI_SOCKTYPE: return WSAESOCKTNOSUPPORT;
    case EAI_SYSTEM: return wsa_error(errno);
    default: return WSANO_RECOVERY;
    }
}

/* Free a guest addrinfo list (32-byte nodes). */
static void free_guest_ai(uint32_t ai)
{
    while (ai != 0)
    {
        uint32_t next = rd32(ai + 28);
        if (rd32(ai + 20) != 0) rt_heap_free(rd32(ai + 20)); /* ai_canonname */
        if (rd32(ai + 24) != 0) rt_heap_free(rd32(ai + 24)); /* ai_addr */
        rt_heap_free(ai);
        ai = next;
    }
}

/* int getaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA *hints,
   PADDRINFOA *result)
   ADDRINFOA (32 bytes): ai_flags, ai_family, ai_socktype, ai_protocol,
   ai_addrlen, ai_canonname, ai_addr, ai_next. */
void rt_ws_getaddrinfo(_cpu *cpu)
{
    char node[1025], service[64];
    struct addrinfo hints, *res = NULL, *p;
    uint32_t gh = ARG(2), pres = ARG(3), head = 0, tail = 0;
    int rc;
    if (pres == 0)
    {
        rt_set_last_error(WSAEINVAL);
        RET(WSAEINVAL);
        return;
    }
    wr32(pres, 0);
    if ((ARG(0) != 0 && rt_gstr(node, sizeof node, ARG(0)) != 0) ||
        (ARG(1) != 0 && rt_gstr(service, sizeof service, ARG(1)) != 0))
    {
        rt_set_last_error(WSAEINVAL);
        RET(WSAEINVAL);
        return;
    }
    memset(&hints, 0, sizeof hints);
    if (gh != 0)
    {
        uint32_t fam = rd32(gh + 4), flags = rd32(gh);
        if (flags & ~0x7u) rt_fatal("getaddrinfo hint flags 0x%x are not supported", flags);
        hints.ai_flags = (int)flags; /* AI_PASSIVE 1, AI_CANONNAME 2, AI_NUMERICHOST 4: same values */
        if (fam == 0)
            hints.ai_family = AF_UNSPEC;
        else if (fam == W_AF_INET)
            hints.ai_family = AF_INET;
        else if (fam == W_AF_INET6)
            hints.ai_family = AF_INET6;
        else
        {
            rt_set_last_error(WSAEAFNOSUPPORT);
            RET(WSAEAFNOSUPPORT);
            return;
        }
        hints.ai_socktype = (int)rd32(gh + 8);
        hints.ai_protocol = (int)rd32(gh + 12);
    }
    rc = getaddrinfo(ARG(0) != 0 ? node : NULL, ARG(1) != 0 ? service : NULL, gh != 0 ? &hints : NULL, &res);
    if (rc != 0)
    {
        uint32_t e = eai_to_wsa(rc);
        rt_set_last_error(e);
        RET(e);
        return;
    }
    for (p = res; p != NULL; p = p->ai_next)
    {
        uint32_t ai, addr, len, fam, cn = 0;
        size_t cn_len = 0;
        if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
        if (p->ai_canonname != NULL)
        {
            cn_len = strlen(p->ai_canonname) + 1;
            if (cn_len > 1025) rt_fatal("getaddrinfo: canonical name is longer than 1024 bytes");
        }
        ai = rt_heap_calloc(1, 32);
        len = p->ai_family == AF_INET ? 16u : 28u;
        addr = rt_heap_calloc(1, len);
        if (cn_len != 0) cn = rt_heap_alloc((uint32_t)cn_len);
        if (ai == 0 || addr == 0 || (cn_len != 0 && cn == 0))
        {
            if (ai) rt_heap_free(ai);
            if (addr) rt_heap_free(addr);
            if (cn) rt_heap_free(cn);
            free_guest_ai(head);
            freeaddrinfo(res);
            rt_set_last_error(WSA_NOT_ENOUGH_MEMORY);
            RET(WSA_NOT_ENOUGH_MEMORY);
            return;
        }
        fam = p->ai_family == AF_INET ? W_AF_INET : W_AF_INET6;
        wr32(ai, (uint32_t)p->ai_flags & 0x7u);
        wr32(ai + 4, fam);
        wr32(ai + 8, (uint32_t)p->ai_socktype);
        wr32(ai + 12, (uint32_t)p->ai_protocol);
        wr32(ai + 16, len);
        wr32(ai + 24, addr);
        {
            uint8_t buf[28];
            memcpy(G2H(addr), buf, addr_windows(p->ai_addr, buf));
        }
        if (cn != 0)
        {
            memcpy(G2H(cn), p->ai_canonname, cn_len);
            wr32(ai + 20, cn);
        }
        if (head == 0)
            head = ai;
        else
            wr32(tail + 28, ai);
        tail = ai;
    }
    freeaddrinfo(res);
    if (head == 0)
    {
        rt_set_last_error(WSANO_DATA);
        RET(WSANO_DATA);
        return;
    }
    wr32(pres, head);
    RET(0);
}

/* void freeaddrinfo(PADDRINFOA) */
void rt_ws_freeaddrinfo(_cpu *cpu)
{
    free_guest_ai(ARG(0));
}
