/*
 * RPCRT4: UUID creation and text conversion.
 */
#include "rt.h"
#include "rt_com.h"

#include <uuid/uuid.h>

#define RPC_S_OK 0u
#define RPC_S_OUT_OF_MEMORY 14u
#define RPC_S_INVALID_ARG 87u

/* RPC_STATUS UuidToStringA(const UUID *, RPC_CSTR *): lower-case text
   without braces in a new string; a NULL UUID gives the nil UUID. */
void rt_UuidToStringA(_cpu *cpu)
{
    uint32_t pu = ARG(0), pp = ARG(1), s;
    rt_guid g;
    char text[39];
    if (pp == 0)
    {
        RET(RPC_S_INVALID_ARG);
        return;
    }
    if (pu != 0)
        g = rt_guid_read(pu);
    else
        memset(&g, 0, sizeof g);
    rt_guid_format(&g, text, false, false);
    s = rt_heap_alloc(37);
    if (s == 0)
    {
        wr32(pp, 0);
        RET(RPC_S_OUT_OF_MEMORY);
        return;
    }
    memcpy(G2H(s), text, 37);
    wr32(pp, s);
    RET(RPC_S_OK);
}

/* RPC_STATUS UuidCreateSequential(UUID *): a time-based (version 1) UUID
   from uuid_generate_time. The UUID fields Data1, Data2 and Data3 are
   little-endian in memory; uuid_t holds them big-endian. */
void rt_UuidCreateSequential(_cpu *cpu)
{
    uint32_t pu = ARG(0);
    uuid_t u;
    uint8_t *p;
    if (pu == 0)
    {
        RET(RPC_S_INVALID_ARG);
        return;
    }
    uuid_generate_time(u);
    p = G2H(pu);
    p[0] = u[3];
    p[1] = u[2];
    p[2] = u[1];
    p[3] = u[0];
    p[4] = u[5];
    p[5] = u[4];
    p[6] = u[7];
    p[7] = u[6];
    memcpy(p + 8, u + 8, 8);
    RET(RPC_S_OK);
}

/* RPC_STATUS RpcStringFreeA(RPC_CSTR *): frees the string and sets the
   pointer to NULL. */
void rt_RpcStringFreeA(_cpu *cpu)
{
    uint32_t pp = ARG(0);
    if (pp == 0)
    {
        RET(RPC_S_INVALID_ARG);
        return;
    }
    if (rd32(pp) != 0) rt_heap_free(rd32(pp));
    wr32(pp, 0);
    RET(RPC_S_OK);
}
