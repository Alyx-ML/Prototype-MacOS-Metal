/*
 * Unit tests of WS2_32 (rt_ws2_32.c): startup, byte order, inet_addr,
 * loopback TCP and UDP, non-blocking connect with select, socket options
 * and getaddrinfo. The expected values are the documented Windows Sockets
 * results (WSA error codes, fd_set and ADDRINFOA layouts).
 *
 * The tests use only the loopback interface (127.0.0.1) and names that the
 * system resolves without a network ("localhost" and numeric addresses).
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

void rt_ws_WSAStartup(_cpu *cpu);
void rt_ws_WSACleanup(_cpu *cpu);
void rt_ws_WSAGetLastError(_cpu *cpu);
void rt_ws_htonl(_cpu *cpu);
void rt_ws_ntohl(_cpu *cpu);
void rt_ws_htons(_cpu *cpu);
void rt_ws_ntohs(_cpu *cpu);
void rt_ws_inet_addr(_cpu *cpu);
void rt_ws_socket(_cpu *cpu);
void rt_ws_closesocket(_cpu *cpu);
void rt_ws_bind(_cpu *cpu);
void rt_ws_listen(_cpu *cpu);
void rt_ws_accept(_cpu *cpu);
void rt_ws_connect(_cpu *cpu);
void rt_ws_getpeername(_cpu *cpu);
void rt_ws_getsockname(_cpu *cpu);
void rt_ws_send(_cpu *cpu);
void rt_ws_sendto(_cpu *cpu);
void rt_ws_recv(_cpu *cpu);
void rt_ws_recvfrom(_cpu *cpu);
void rt_ws_shutdown(_cpu *cpu);
void rt_ws_ioctlsocket(_cpu *cpu);
void rt_ws_setsockopt(_cpu *cpu);
void rt_ws_getsockopt(_cpu *cpu);
void rt_ws___WSAFDIsSet(_cpu *cpu);
void rt_ws_select(_cpu *cpu);
void rt_ws_getaddrinfo(_cpu *cpu);
void rt_ws_freeaddrinfo(_cpu *cpu);

#define AF_INET_W 2u
#define SOCK_STREAM_W 1u
#define SOCK_DGRAM_W 2u
#define INVALID_SOCKET_W 0xFFFFFFFFu
#define SOCKET_ERROR_W 0xFFFFFFFFu
#define FIONBIO_W 0x8004667Eu
#define FIONREAD_W 0x4004667Fu
#define SOL_SOCKET_W 0xFFFFu
#define SO_ERROR_W 0x1007u
#define SO_TYPE_W 0x1008u
#define SO_REUSEADDR_W 0x0004u
#define SO_RCVTIMEO_W 0x1006u
#define IPPROTO_TCP_W 6u
#define TCP_NODELAY_W 1u
#define MSG_PEEK_W 2u

static uint32_t wsa_error(void)
{
    argv_t a = { .n = 0 };
    return callv(rt_ws_WSAGetLastError, &a);
}

static uint32_t call5(void (*fn)(_cpu *), uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    argv_t a = { .n = 0 };
    a32(&a, a0);
    a32(&a, a1);
    a32(&a, a2);
    a32(&a, a3);
    a32(&a, a4);
    return callv(fn, &a);
}

static uint32_t call6(void (*fn)(_cpu *), uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    argv_t a = { .n = 0 };
    a32(&a, a0);
    a32(&a, a1);
    a32(&a, a2);
    a32(&a, a3);
    a32(&a, a4);
    a32(&a, a5);
    return callv(fn, &a);
}

/* A guest sockaddr_in for 127.0.0.1 and port (host byte order). */
static uint32_t loopback(uint16_t port)
{
    uint32_t g = gbuf(16);
    wr16(g, AF_INET_W);
    wr8(g + 2, (uint8_t)(port >> 8));
    wr8(g + 3, (uint8_t)port);
    wr32(g + 4, 0x0100007Fu);
    return g;
}

/* The port (host byte order) of a guest sockaddr_in. */
static uint16_t port_of(uint32_t g)
{
    return (uint16_t)((rd8(g + 2) << 8) | rd8(g + 3));
}

/* The bound port of socket s. */
static uint16_t bound_port(uint32_t s)
{
    uint32_t a = gbuf(16), n = gbuf(4);
    wr32(n, 16);
    check_u32("getsockname", call3(rt_ws_getsockname, s, a, n), 0);
    check_u32("getsockname length", rd32(n), 16);
    return port_of(a);
}

/* A guest fd_set with the given sockets. */
static uint32_t fdset(int n, const uint32_t *s)
{
    uint32_t g = gbuf(4 + 64 * 4);
    int i;
    wr32(g, (uint32_t)n);
    for (i = 0; i < n; i++) wr32(g + 4 + 4 * (uint32_t)i, s[i]);
    return g;
}

static uint32_t timeval(uint32_t sec, uint32_t usec)
{
    uint32_t g = gbuf(8);
    wr32(g, sec);
    wr32(g + 4, usec);
    return g;
}

static uint32_t sockopt_int(uint32_t s, uint32_t level, uint32_t name)
{
    uint32_t v = gbuf(4), n = gbuf(4);
    wr32(n, 4);
    check_u32("getsockopt", call5(rt_ws_getsockopt, s, level, name, v, n), 0);
    check_u32("getsockopt length", rd32(n), 4);
    return rd32(v);
}

static void test_startup(void)
{
    uint32_t data = gbuf(400);
    check_u32("socket before WSAStartup", call3(rt_ws_socket, AF_INET_W, SOCK_STREAM_W, 0), INVALID_SOCKET_W);
    check_u32("socket before WSAStartup: error", wsa_error(), 10093);
    check_u32("WSACleanup before WSAStartup", call1(rt_ws_WSACleanup, 0), SOCKET_ERROR_W);
    check_u32("WSACleanup before WSAStartup: error", wsa_error(), 10093);
    check_u32("WSAStartup 0.0", call2(rt_ws_WSAStartup, 0x0000, data), 10092);
    check_u32("WSAStartup NULL data", call2(rt_ws_WSAStartup, 0x0202, 0), 10014);
    check_u32("WSAStartup 2.2", call2(rt_ws_WSAStartup, 0x0202, data), 0);
    check_u32("WSAStartup 2.2 wVersion", rd16(data), 0x0202);
    check_u32("WSAStartup 2.2 wHighVersion", rd16(data + 2), 0x0202);
    check_str("WSAStartup szDescription", data + 4, "WinSock 2.0");
    check_str("WSAStartup szSystemStatus", data + 261, "Running");
    check_u32("WSAStartup 3.3", call2(rt_ws_WSAStartup, 0x0303, data), 0);
    check_u32("WSAStartup 3.3 wVersion", rd16(data), 0x0202);
    check_u32("WSAStartup 1.1", call2(rt_ws_WSAStartup, 0x0101, data), 0);
    check_u32("WSAStartup 1.1 wVersion", rd16(data), 0x0101);
    check_u32("WSAStartup 1.1 iMaxSockets", rd16(data + 390), 32767);
    check_u32("WSACleanup 1", call1(rt_ws_WSACleanup, 0), 0);
    check_u32("WSACleanup 2", call1(rt_ws_WSACleanup, 0), 0);
    /* One startup is still active for the other tests. */
}

static void test_byte_order(void)
{
    check_u32("htonl", call1(rt_ws_htonl, 0x12345678u), 0x78563412u);
    check_u32("ntohl", call1(rt_ws_ntohl, 0x78563412u), 0x12345678u);
    check_u32("htons", call1(rt_ws_htons, 0x1234u), 0x3412u);
    check_u32("ntohs", call1(rt_ws_ntohs, 0xAABB3412u), 0x1234u);
    check_u32("inet_addr 127.0.0.1", call1(rt_ws_inet_addr, gs("127.0.0.1")), 0x0100007Fu);
    check_u32("inet_addr 10.1.2.3", call1(rt_ws_inet_addr, gs("10.1.2.3")), 0x0302010Au);
    check_u32("inet_addr 0x7f.1 (a.b form)", call1(rt_ws_inet_addr, gs("0x7f.1")), 0x0100007Fu);
    check_u32("inet_addr empty", call1(rt_ws_inet_addr, gs("")), 0);
    check_u32("inet_addr text", call1(rt_ws_inet_addr, gs("not an address")), 0xFFFFFFFFu);
    check_u32("inet_addr 256.0.0.1", call1(rt_ws_inet_addr, gs("256.0.0.1")), 0xFFFFFFFFu);
    check_u32("inet_addr NULL", call1(rt_ws_inet_addr, 0), 0xFFFFFFFFu);
}

static void test_tcp(void)
{
    uint32_t ls, cs, ss, one = gbuf(4), buf = gbuf(64), peer = gbuf(16), plen = gbuf(4);
    uint32_t r, rset, wset, eset, set[1];
    uint16_t port;

    check_u32("socket bad family", call3(rt_ws_socket, 99, SOCK_STREAM_W, 0), INVALID_SOCKET_W);
    check_u32("socket bad family: error", wsa_error(), 10047);
    check_u32("socket bad type", call3(rt_ws_socket, AF_INET_W, 3, 0), INVALID_SOCKET_W);
    check_u32("socket bad type: error", wsa_error(), 10044);

    ls = call3(rt_ws_socket, AF_INET_W, SOCK_STREAM_W, 6);
    if (ls == INVALID_SOCKET_W)
    {
        check_u32("listening socket", wsa_error(), 0);
        return;
    }
    check_u32("SO_TYPE stream", sockopt_int(ls, SOL_SOCKET_W, SO_TYPE_W), SOCK_STREAM_W);
    wr32(one, 1);
    check_u32("setsockopt SO_REUSEADDR", call5(rt_ws_setsockopt, ls, SOL_SOCKET_W, SO_REUSEADDR_W, one, 4), 0);
    check_u32("SO_REUSEADDR value", sockopt_int(ls, SOL_SOCKET_W, SO_REUSEADDR_W), 1);
    r = call3(rt_ws_bind, ls, loopback(0), 16);
    check_u32("bind 127.0.0.1:0", r, 0);
    if (r != 0)
    {
        printf("  bind error %u (the sandbox can block local binding)\n", wsa_error());
        call1(rt_ws_closesocket, ls);
        return;
    }
    check_u32("bind short address", call3(rt_ws_bind, ls, loopback(0), 8), SOCKET_ERROR_W);
    check_u32("bind short address: error", wsa_error(), 10014);
    check_u32("listen", call2(rt_ws_listen, ls, 5), 0);
    port = bound_port(ls);
    check_u32("bound port is not 0", port != 0, 1);

    /* Non-blocking connect: WSAEWOULDBLOCK, then the socket is writable. */
    cs = call3(rt_ws_socket, AF_INET_W, SOCK_STREAM_W, 0);
    wr32(one, 1);
    check_u32("FIONBIO 1", call3(rt_ws_ioctlsocket, cs, FIONBIO_W, one), 0);
    r = call3(rt_ws_connect, cs, loopback(port), 16);
    if (r == SOCKET_ERROR_W)
        check_u32("non-blocking connect: error", wsa_error(), 10035);
    else
        check_u32("non-blocking connect", r, 0);
    set[0] = cs;
    wset = fdset(1, set);
    eset = fdset(1, set);
    check_u32("select connect", call5(rt_ws_select, 0, 0, wset, eset, timeval(5, 0)), 1);
    check_u32("select connect: write set count", rd32(wset), 1);
    check_u32("select connect: write set socket", rd32(wset + 4), cs);
    check_u32("select connect: except set count", rd32(eset), 0);
    check_u32("__WSAFDIsSet write", call2(rt_ws___WSAFDIsSet, cs, wset), 1);
    check_u32("__WSAFDIsSet except", call2(rt_ws___WSAFDIsSet, cs, eset), 0);
    check_u32("SO_ERROR after connect", sockopt_int(cs, SOL_SOCKET_W, SO_ERROR_W), 0);

    wr32(plen, 16);
    ss = call3(rt_ws_accept, ls, peer, plen);
    check_u32("accept", ss != INVALID_SOCKET_W, 1);
    check_u32("accept address length", rd32(plen), 16);
    check_u32("accept address family", rd16(peer), AF_INET_W);
    check_u32("accept address", rd32(peer + 4), 0x0100007Fu);
    check_u32("accept peer port", port_of(peer), bound_port(cs));
    wr32(plen, 16);
    check_u32("getpeername", call3(rt_ws_getpeername, cs, peer, plen), 0);
    check_u32("getpeername port", port_of(peer), port);
    wr32(plen, 8);
    check_u32("getpeername small buffer", call3(rt_ws_getpeername, cs, peer, plen), SOCKET_ERROR_W);
    check_u32("getpeername small buffer: error", wsa_error(), 10014);

    wr8(one, 1);
    check_u32("setsockopt TCP_NODELAY (1-byte BOOL)", call5(rt_ws_setsockopt, cs, IPPROTO_TCP_W, TCP_NODELAY_W, one, 1), 0);
    check_u32("TCP_NODELAY value", sockopt_int(cs, IPPROTO_TCP_W, TCP_NODELAY_W), 1);
    wr32(one, 2500);
    check_u32("setsockopt SO_RCVTIMEO", call5(rt_ws_setsockopt, ss, SOL_SOCKET_W, SO_RCVTIMEO_W, one, 4), 0);

    /* Data from the client to the server. */
    memcpy(G2H(buf), "hello", 5);
    check_u32("send", call4(rt_ws_send, cs, buf, 5, 0), 5);
    set[0] = ss;
    rset = fdset(1, set);
    check_u32("select read", call5(rt_ws_select, 0, rset, 0, 0, timeval(5, 0)), 1);
    check_u32("select read: set socket", rd32(rset + 4), ss);
    check_u32("FIONREAD", call3(rt_ws_ioctlsocket, ss, FIONREAD_W, one), 0);
    check_u32("FIONREAD value", rd32(one), 5);
    memset(G2H(buf), 0, 64);
    check_u32("recv MSG_PEEK", call4(rt_ws_recv, ss, buf, 64, MSG_PEEK_W), 5);
    check_bytes("recv MSG_PEEK data", G2H(buf), 5, "hello", 5);
    memset(G2H(buf), 0, 64);
    check_u32("recv", call4(rt_ws_recv, ss, buf, 64, 0), 5);
    check_bytes("recv data", G2H(buf), 5, "hello", 5);
    check_u32("recv unsupported flag", call4(rt_ws_recv, ss, buf, 64, 0x100), SOCKET_ERROR_W);
    check_u32("recv unsupported flag: error", wsa_error(), 10045);

    /* The accepted socket is blocking; the client socket is non-blocking. */
    check_u32("recv no data (non-blocking)", call4(rt_ws_recv, cs, buf, 64, 0), SOCKET_ERROR_W);
    check_u32("recv no data: error", wsa_error(), 10035);
    wr32(one, 0);
    check_u32("FIONBIO 0", call3(rt_ws_ioctlsocket, cs, FIONBIO_W, one), 0);

    /* Graceful close: recv gives 0. */
    check_u32("shutdown bad how", call2(rt_ws_shutdown, cs, 3), SOCKET_ERROR_W);
    check_u32("shutdown bad how: error", wsa_error(), 10022);
    check_u32("shutdown SD_SEND", call2(rt_ws_shutdown, cs, 1), 0);
    check_u32("recv after shutdown", call4(rt_ws_recv, ss, buf, 64, 0), 0);

    check_u32("closesocket client", call1(rt_ws_closesocket, cs), 0);
    check_u32("closesocket client again", call1(rt_ws_closesocket, cs), SOCKET_ERROR_W);
    check_u32("closesocket client again: error", wsa_error(), 10038);
    /* Send to a connection that the peer closed: an error, not a signal. */
    memcpy(G2H(buf), "x", 1);
    r = call4(rt_ws_send, ss, buf, 1, 0);
    if (r != SOCKET_ERROR_W)
    {
        int i;
        for (i = 0; i < 50 && r != SOCKET_ERROR_W; i++) r = call4(rt_ws_send, ss, buf, 1, 0);
    }
    check_u32("send to closed peer", r, SOCKET_ERROR_W);
    check_u32("send to closed peer: error", wsa_error(), 10054);
    check_u32("closesocket server", call1(rt_ws_closesocket, ss), 0);

    /* Non-blocking connect to a closed port: the exception set and
       SO_ERROR WSAECONNREFUSED. */
    check_u32("closesocket listener", call1(rt_ws_closesocket, ls), 0);
    cs = call3(rt_ws_socket, AF_INET_W, SOCK_STREAM_W, 0);
    wr32(one, 1);
    check_u32("FIONBIO 1 (refused)", call3(rt_ws_ioctlsocket, cs, FIONBIO_W, one), 0);
    r = call3(rt_ws_connect, cs, loopback(port), 16);
    if (r == SOCKET_ERROR_W && wsa_error() == 10061)
    {
        /* The system can refuse a loopback connect at once. */
        check_u32("refused connect at once", 1, 1);
    }
    else
    {
        check_u32("refused connect: pending", r, SOCKET_ERROR_W);
        check_u32("refused connect: pending error", wsa_error(), 10035);
        set[0] = cs;
        wset = fdset(1, set);
        eset = fdset(1, set);
        check_u32("select refused", call5(rt_ws_select, 0, 0, wset, eset, timeval(5, 0)), 1);
        check_u32("select refused: write set count", rd32(wset), 0);
        check_u32("select refused: except set count", rd32(eset), 1);
        check_u32("SO_ERROR refused", sockopt_int(cs, SOL_SOCKET_W, SO_ERROR_W), 10061);
        check_u32("SO_ERROR refused, read again", sockopt_int(cs, SOL_SOCKET_W, SO_ERROR_W), 0);
    }
    check_u32("closesocket refused", call1(rt_ws_closesocket, cs), 0);
}

static void test_udp(void)
{
    uint32_t a = call3(rt_ws_socket, AF_INET_W, SOCK_DGRAM_W, 17);
    uint32_t b = call3(rt_ws_socket, AF_INET_W, SOCK_DGRAM_W, 0);
    uint32_t buf = gbuf(64), from = gbuf(16), flen = gbuf(4), r, set[1], rset;
    uint16_t pa, pb;
    check_u32("udp sockets", a != INVALID_SOCKET_W && b != INVALID_SOCKET_W, 1);
    check_u32("SO_TYPE datagram", sockopt_int(a, SOL_SOCKET_W, SO_TYPE_W), SOCK_DGRAM_W);
    r = call3(rt_ws_bind, a, loopback(0), 16);
    check_u32("udp bind a", r, 0);
    if (r != 0) return;
    check_u32("udp bind b", call3(rt_ws_bind, b, loopback(0), 16), 0);
    pa = bound_port(a);
    pb = bound_port(b);
    memcpy(G2H(buf), "datagram-1", 10);
    check_u32("sendto", call6(rt_ws_sendto, b, buf, 10, 0, loopback(pa), 16), 10);
    set[0] = a;
    rset = fdset(1, set);
    check_u32("udp select read", call5(rt_ws_select, 0, rset, 0, 0, timeval(5, 0)), 1);
    memset(G2H(buf), 0, 64);
    wr32(flen, 16);
    check_u32("recvfrom", call6(rt_ws_recvfrom, a, buf, 64, 0, from, flen), 10);
    check_bytes("recvfrom data", G2H(buf), 10, "datagram-1", 10);
    check_u32("recvfrom address length", rd32(flen), 16);
    check_u32("recvfrom port", port_of(from), pb);
    check_u32("recvfrom address", rd32(from + 4), 0x0100007Fu);

    /* A datagram larger than the buffer: the first part and WSAEMSGSIZE. */
    memcpy(G2H(buf), "0123456789", 10);
    check_u32("sendto long", call6(rt_ws_sendto, b, buf, 10, 0, loopback(pa), 16), 10);
    rset = fdset(1, set);
    check_u32("udp select read 2", call5(rt_ws_select, 0, rset, 0, 0, timeval(5, 0)), 1);
    memset(G2H(buf), 0, 64);
    check_u32("recv short buffer", call4(rt_ws_recv, a, buf, 4, 0), SOCKET_ERROR_W);
    check_u32("recv short buffer: error", wsa_error(), 10040);
    check_bytes("recv short buffer: first part", G2H(buf), 5, "0123\0", 5);

    /* select with a timeout and no data. */
    rset = fdset(1, set);
    check_u32("select timeout", call5(rt_ws_select, 0, rset, 0, 0, timeval(0, 20000)), 0);
    check_u32("select timeout: set count", rd32(rset), 0);
    check_u32("closesocket a", call1(rt_ws_closesocket, a), 0);
    check_u32("closesocket b", call1(rt_ws_closesocket, b), 0);
}

static void test_select_errors(void)
{
    uint32_t set[1] = { 0x12345678u };
    check_u32("select no sets", call5(rt_ws_select, 0, 0, 0, 0, timeval(0, 0)), SOCKET_ERROR_W);
    check_u32("select no sets: error", wsa_error(), 10022);
    check_u32("select empty set", call5(rt_ws_select, 0, fdset(0, set), 0, 0, timeval(0, 0)), SOCKET_ERROR_W);
    check_u32("select empty set: error", wsa_error(), 10022);
    check_u32("select bad socket", call5(rt_ws_select, 0, fdset(1, set), 0, 0, timeval(0, 0)), SOCKET_ERROR_W);
    check_u32("select bad socket: error", wsa_error(), 10038);
    check_u32("__WSAFDIsSet NULL set", call2(rt_ws___WSAFDIsSet, 1, 0), 0);
    check_u32("send bad socket", call4(rt_ws_send, 0x12345678u, gbuf(4), 1, 0), SOCKET_ERROR_W);
    check_u32("send bad socket: error", wsa_error(), 10038);
}

static void test_getaddrinfo(void)
{
    uint32_t hints = gbuf(32), res = gbuf(4), ai, addr;
    int found = 0;

    wr32(hints, 4);              /* AI_NUMERICHOST */
    wr32(hints + 4, AF_INET_W);
    wr32(hints + 8, SOCK_STREAM_W);
    check_u32("getaddrinfo numeric", call4(rt_ws_getaddrinfo, gs("127.0.0.1"), gs("80"), hints, res), 0);
    ai = rd32(res);
    check_u32("getaddrinfo numeric: result", ai != 0, 1);
    if (ai != 0)
    {
        check_u32("ai_family", rd32(ai + 4), AF_INET_W);
        check_u32("ai_socktype", rd32(ai + 8), SOCK_STREAM_W);
        check_u32("ai_addrlen", rd32(ai + 16), 16);
        addr = rd32(ai + 24);
        check_u32("ai_addr family", rd16(addr), AF_INET_W);
        check_u32("ai_addr port", port_of(addr), 80);
        check_u32("ai_addr address", rd32(addr + 4), 0x0100007Fu);
        check_u32("ai_next", rd32(ai + 28), 0);
        call1(rt_ws_freeaddrinfo, ai);
    }

    check_u32("getaddrinfo numeric host, text name", call4(rt_ws_getaddrinfo, gs("no-such-host.invalid"), 0, hints, res), 11001);
    check_u32("getaddrinfo numeric host, text name: last error", wsa_error(), 11001);
    check_u32("getaddrinfo numeric host, text name: result", rd32(res), 0);

    wr32(hints, 0);
    check_u32("getaddrinfo localhost", call4(rt_ws_getaddrinfo, gs("localhost"), 0, hints, res), 0);
    for (ai = rd32(res); ai != 0; ai = rd32(ai + 28))
        if (rd32(ai + 4) == AF_INET_W && rd32(rd32(ai + 24) + 4) == 0x0100007Fu) found = 1;
    check_u32("getaddrinfo localhost: 127.0.0.1", (uint32_t)found, 1);
    call1(rt_ws_freeaddrinfo, rd32(res));

    wr32(hints + 4, 99);
    check_u32("getaddrinfo bad family", call4(rt_ws_getaddrinfo, gs("localhost"), 0, hints, res), 10047);
    check_u32("getaddrinfo NULL result", call4(rt_ws_getaddrinfo, gs("localhost"), 0, 0, 0), 10022);
}

int main(void)
{
    harness_init();
    test_startup();
    test_byte_order();
    test_tcp();
    test_udp();
    test_select_errors();
    test_getaddrinfo();
    check_u32("final WSACleanup", call1(rt_ws_WSACleanup, 0), 0);
    return harness_report();
}
