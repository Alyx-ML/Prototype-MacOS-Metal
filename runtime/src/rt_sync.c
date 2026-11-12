/*
 * KERNEL32 critical sections, interlocked operations, thread local storage
 * (TLS) and time functions.
 */
#include "rt_object.h"
#include "rt_thread.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

/* ---------------------------------------------------------------- critical sections */

/*
 * Guest CRITICAL_SECTION (24 bytes):
 *   +0  DebugInfo       +4  LockCount     +8  RecursionCount
 *   +12 OwningThread    +16 LockSemaphore +20 SpinCount
 * LockSemaphore holds the index of a host recursive mutex. RecursionCount
 * and OwningThread are kept current for guest code that reads them.
 *
 * Windows DeleteCriticalSection does not fail when a thread owns the
 * critical section or waits for it (the state of the waiting threads is
 * then undefined). A host mutex that is locked or has a waiter cannot be
 * destroyed. DeleteCriticalSection then marks the entry as deleted, and
 * the LeaveCriticalSection that makes the mutex free destroys it and frees
 * the index.
 */
#define CS_CHUNK 256u
#define CS_CHUNKS 256u
#define CS_MAX (CS_CHUNK * CS_CHUNKS)

typedef struct cs_entry {
    pthread_mutex_t m;
    uint32_t guest; /* guest address of the CRITICAL_SECTION, 0 if free */
    _Atomic(bool) deleted; /* deleted while in use; the mutex is not yet destroyed */
} cs_entry;

static pthread_mutex_t cs_table_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(cs_entry *) cs_chunks[CS_CHUNKS];
static uint32_t cs_next = 1; /* index 0 is never used */
static uint32_t *cs_free_list;
static uint32_t cs_free_count, cs_free_cap;

static cs_entry *cs_entry_at(uint32_t idx)
{
    cs_entry *chunk;
    if (idx == 0 || idx >= CS_MAX) return NULL;
    chunk = atomic_load_explicit(&cs_chunks[idx / CS_CHUNK], memory_order_acquire);
    if (chunk == NULL) return NULL;
    return &chunk[idx % CS_CHUNK];
}

static cs_entry *cs_lookup(uint32_t cs)
{
    cs_entry *e;
    if (cs == 0) rt_fatal("critical section function with a NULL pointer");
    e = cs_entry_at(rd32(cs + 16));
    if (e == NULL || e->guest != cs) rt_fatal("use of an uninitialized critical section at 0x%08x", cs);
    return e;
}

void rt_InitializeCriticalSection(_cpu *cpu)
{
    uint32_t cs = ARG(0), idx;
    cs_entry *e;
    pthread_mutexattr_t a;

    if (pthread_mutex_lock(&cs_table_lock) != 0) rt_fatal("critical section table lock failed");
    if (cs_free_count > 0)
    {
        idx = cs_free_list[--cs_free_count];
    }
    else
    {
        if (cs_next >= CS_MAX) rt_fatal("more than %u critical sections", CS_MAX - 1u);
        idx = cs_next++;
        if (atomic_load_explicit(&cs_chunks[idx / CS_CHUNK], memory_order_relaxed) == NULL)
        {
            cs_entry *chunk = calloc(CS_CHUNK, sizeof(cs_entry));
            if (chunk == NULL) rt_fatal("out of host memory for critical sections");
            atomic_store_explicit(&cs_chunks[idx / CS_CHUNK], chunk, memory_order_release);
        }
    }
    e = cs_entry_at(idx);
    if (pthread_mutexattr_init(&a) != 0) rt_fatal("pthread_mutexattr_init failed");
    if (pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE) != 0) rt_fatal("pthread_mutexattr_settype failed");
    if (pthread_mutex_init(&e->m, &a) != 0) rt_fatal("pthread_mutex_init failed");
    pthread_mutexattr_destroy(&a);
    e->guest = cs;
    if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");

    wr32(cs + 0, 0);
    wr32(cs + 4, 0xFFFFFFFFu);
    wr32(cs + 8, 0);
    wr32(cs + 12, 0);
    wr32(cs + 16, idx);
    wr32(cs + 20, 0);
}

static void cs_free_entry(cs_entry *e, uint32_t idx);

void rt_DeleteCriticalSection(_cpu *cpu)
{
    uint32_t cs = ARG(0);
    cs_entry *e;
    uint32_t k;
    bool zero = cs != 0;
    /* Windows does nothing for a critical section that is all zero (never
       initialized): it has no debug data and no semaphore to release. The
       engine deletes such a section at exit. */
    for (k = 0; zero && k < 24u; k += 4u)
        if (rd32(cs + k) != 0) zero = false;
    if (zero) return;
    e = cs_lookup(cs);
    uint32_t idx = rd32(cs + 16);
    int r;

    if (pthread_mutex_lock(&cs_table_lock) != 0) rt_fatal("critical section table lock failed");
    if (atomic_load_explicit(&e->deleted, memory_order_relaxed))
    {
        if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");
        rt_fatal("DeleteCriticalSection of the critical section at 0x%08x, which is already deleted", cs);
    }
    r = pthread_mutex_destroy(&e->m);
    if (r == EBUSY)
    {
        /* The section is in use (see the comment at the top). The owner
           can release the mutex before it sees the mark. The fences make
           sure that the owner sees the mark (and destroys the mutex) or
           the second destroy here finds the mutex free. */
        atomic_store_explicit(&e->deleted, true, memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);
        r = pthread_mutex_destroy(&e->m);
        if (r == 0)
        {
            cs_free_entry(e, idx);
            if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");
            wr32(cs + 16, 0);
            return;
        }
        if (r != EBUSY) rt_fatal("DeleteCriticalSection failed (%d)", r);
        if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");
        rt_log("DeleteCriticalSection of the critical section at 0x%08x while it is in use (owner thread %u, "
               "recursion %u, calling thread %u, return address 0x%08x); the host mutex is destroyed after the "
               "last LeaveCriticalSection",
               cs, rd32(cs + 12), rd32(cs + 8), rt_self()->tid, rd32(esp));
        return;
    }
    if (r != 0) rt_fatal("DeleteCriticalSection failed (%d)", r);
    cs_free_entry(e, idx);
    if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");
    wr32(cs + 16, 0);
}

/* After a LeaveCriticalSection of a deleted section: destroy the host mutex
   if it is free, and free the index. */
static void cs_reap(cs_entry *e, uint32_t cs, uint32_t idx)
{
    int r;
    if (pthread_mutex_lock(&cs_table_lock) != 0) rt_fatal("critical section table lock failed");
    if (atomic_load_explicit(&e->deleted, memory_order_relaxed) && e->guest == cs)
    {
        r = pthread_mutex_destroy(&e->m);
        if (r == 0)
        {
            cs_free_entry(e, idx);
            /* The guest can have initialized the memory again. */
            if (rd32(cs + 16) == idx) wr32(cs + 16, 0);
        }
        else if (r != EBUSY)
            rt_fatal("destruction of the deleted critical section at 0x%08x failed (%d)", cs, r);
    }
    if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");
}

/* Free entry idx. The table lock is held. */
static void cs_free_entry(cs_entry *e, uint32_t idx)
{
    atomic_store_explicit(&e->deleted, false, memory_order_relaxed);
    e->guest = 0;
    if (cs_free_count == cs_free_cap)
    {
        uint32_t cap = cs_free_cap ? cs_free_cap * 2u : 64u;
        uint32_t *n = realloc(cs_free_list, (size_t)cap * sizeof(uint32_t));
        if (n == NULL) rt_fatal("out of host memory for critical sections");
        cs_free_list = n;
        cs_free_cap = cap;
    }
    cs_free_list[cs_free_count++] = idx;
}

static void cs_owned(uint32_t cs)
{
    wr32(cs + 8, rd32(cs + 8) + 1u);
    wr32(cs + 12, rt_self()->tid);
}

void rt_EnterCriticalSection(_cpu *cpu)
{
    uint32_t cs = ARG(0);
    int r = pthread_mutex_lock(&cs_lookup(cs)->m);
    if (r != 0) rt_fatal("EnterCriticalSection failed (%d)", r);
    cs_owned(cs);
}

void rt_TryEnterCriticalSection(_cpu *cpu)
{
    uint32_t cs = ARG(0);
    int r = pthread_mutex_trylock(&cs_lookup(cs)->m);
    if (r == EBUSY)
    {
        RET(0);
        return;
    }
    if (r != 0) rt_fatal("TryEnterCriticalSection failed (%d)", r);
    cs_owned(cs);
    RET(1);
}

void rt_LeaveCriticalSection(_cpu *cpu)
{
    uint32_t cs = ARG(0);
    cs_entry *e = cs_lookup(cs);
    uint32_t rec = rd32(cs + 8);
    int r;
    if (rec == 0 || rd32(cs + 12) != rt_self()->tid)
        rt_fatal("LeaveCriticalSection of a critical section that the thread does not own");
    wr32(cs + 8, rec - 1u);
    if (rec == 1u) wr32(cs + 12, 0);
    r = pthread_mutex_unlock(&e->m);
    if (r != 0) rt_fatal("LeaveCriticalSection failed (%d)", r);
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&e->deleted, memory_order_relaxed)) cs_reap(e, cs, rd32(cs + 16));
}

/* ---------------------------------------------------------------- interlocked */

static _Atomic(uint32_t) *atomic32(uint32_t g)
{
    if (g == 0 || (g & 3u) != 0) rt_fatal("interlocked operation on a bad address 0x%08x", g);
    return (_Atomic(uint32_t) *)G2H(g);
}

void rt_InterlockedCompareExchange(_cpu *cpu)
{
    uint32_t expected = ARG(2);
    atomic_compare_exchange_strong(atomic32(ARG(0)), &expected, ARG(1));
    RET(expected);
}

void rt_InterlockedExchange(_cpu *cpu)
{
    RET(atomic_exchange(atomic32(ARG(0)), ARG(1)));
}

void rt_InterlockedDecrement(_cpu *cpu)
{
    RET(atomic_fetch_sub(atomic32(ARG(0)), 1u) - 1u);
}

/* ---------------------------------------------------------------- TLS */

static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tls_used;

#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu

void rt_TlsAlloc(_cpu *cpu)
{
    uint32_t i;
    if (pthread_mutex_lock(&tls_lock) != 0) rt_fatal("TLS lock failed");
    for (i = 0; i < TLS_SLOT_COUNT; i++)
    {
        if ((tls_used & (UINT64_C(1) << i)) == 0)
        {
            tls_used |= UINT64_C(1) << i;
            break;
        }
    }
    if (pthread_mutex_unlock(&tls_lock) != 0) rt_fatal("TLS unlock failed");
    if (i == TLS_SLOT_COUNT)
    {
        rt_set_last_error(ERROR_NO_MORE_ITEMS);
        RET(TLS_OUT_OF_INDEXES);
        return;
    }
    /* A new slot is zero in every thread (TlsFree cleared it). */
    RET(i);
}

static void clear_slot(rt_thread *t, void *ctx)
{
    uint32_t i = *(uint32_t *)ctx;
    wr32(t->teb + TEB_TLS_SLOTS + 4u * i, 0);
}

void rt_TlsFree(_cpu *cpu)
{
    uint32_t i = ARG(0);
    bool ok;
    if (pthread_mutex_lock(&tls_lock) != 0) rt_fatal("TLS lock failed");
    ok = i < TLS_SLOT_COUNT && (tls_used & (UINT64_C(1) << i)) != 0;
    if (ok)
    {
        tls_used &= ~(UINT64_C(1) << i);
        rt_thread_foreach(clear_slot, &i);
    }
    if (pthread_mutex_unlock(&tls_lock) != 0) rt_fatal("TLS unlock failed");
    if (!ok) rt_set_last_error(ERROR_INVALID_PARAMETER);
    RET(ok ? 1 : 0);
}

void rt_TlsGetValue(_cpu *cpu)
{
    uint32_t i = ARG(0);
    rt_thread *t = rt_self();
    if (i >= TLS_SLOT_COUNT)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    rt_set_last_error(ERROR_SUCCESS);
    RET(rd32(t->teb + TEB_TLS_SLOTS + 4u * i));
}

void rt_TlsSetValue(_cpu *cpu)
{
    uint32_t i = ARG(0);
    rt_thread *t = rt_self();
    if (i >= TLS_SLOT_COUNT)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    wr32(t->teb + TEB_TLS_SLOTS + 4u * i, ARG(1));
    RET(1);
}

/* ---------------------------------------------------------------- time */

/* 100 ns intervals between 1601-01-01 and 1970-01-01. */
#define FILETIME_UNIX_EPOCH UINT64_C(116444736000000000)

uint64_t rt_filetime_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) rt_fatal("clock_gettime failed");
    return FILETIME_UNIX_EPOCH + (uint64_t)ts.tv_sec * 10000000u + (uint64_t)ts.tv_nsec / 100u;
}

static uint64_t uptime_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void rt_GetTickCount(_cpu *cpu)
{
    RET((uint32_t)(uptime_ns() / 1000000u));
}

void rt_timeGetTime(_cpu *cpu)
{
    RET((uint32_t)(uptime_ns() / 1000000u));
}

/* QueryPerformanceFrequency: 10 MHz, the value of current Windows
   versions. */
#define QPC_FREQUENCY UINT64_C(10000000)

void rt_QueryPerformanceCounter(_cpu *cpu)
{
    if (ARG(0) == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    wr64(ARG(0), uptime_ns() / 100u);
    RET(1);
}

void rt_QueryPerformanceFrequency(_cpu *cpu)
{
    if (ARG(0) == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    wr64(ARG(0), QPC_FREQUENCY);
    RET(1);
}

void rt_GetSystemTimeAsFileTime(_cpu *cpu)
{
    wr64(ARG(0), rt_filetime_now());
}

/* SYSTEMTIME: wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond,
   wMilliseconds (16-bit each). */
void rt_write_systemtime(uint32_t g, const struct tm *tm, uint32_t ms)
{
    wr16(g + 0, (uint16_t)(tm->tm_year + 1900));
    wr16(g + 2, (uint16_t)(tm->tm_mon + 1));
    wr16(g + 4, (uint16_t)tm->tm_wday);
    wr16(g + 6, (uint16_t)tm->tm_mday);
    wr16(g + 8, (uint16_t)tm->tm_hour);
    wr16(g + 10, (uint16_t)tm->tm_min);
    wr16(g + 12, (uint16_t)tm->tm_sec);
    wr16(g + 14, (uint16_t)ms);
}

void rt_GetLocalTime(_cpu *cpu)
{
    struct timespec ts;
    struct tm tm;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) rt_fatal("clock_gettime failed");
    if (localtime_r(&ts.tv_sec, &tm) == NULL) rt_fatal("localtime_r failed");
    rt_write_systemtime(ARG(0), &tm, (uint32_t)(ts.tv_nsec / 1000000L));
}

static void write_wname(uint32_t g, const char *s)
{
    uint32_t i;
    for (i = 0; i < 31u && s != NULL && s[i] != 0; i++) wr16(g + 2u * i, (uint16_t)(uint8_t)s[i]);
    for (; i < 32u; i++) wr16(g + 2u * i, 0);
}

static long gmtoff_at(time_t t)
{
    struct tm tm;
    if (localtime_r(&t, &tm) == NULL) rt_fatal("localtime_r failed");
    return tm.tm_gmtoff;
}

/* Find the time in (a, b] where the UTC offset changes (to the minute). */
static time_t find_transition(time_t a, time_t b)
{
    long off_a = gmtoff_at(a);
    while (b - a > 60)
    {
        time_t m = a + (b - a) / 2;
        if (gmtoff_at(m) == off_a) a = m;
        else b = m;
    }
    return b;
}

/* TIME_ZONE_INFORMATION (172 bytes):
   +0 Bias, +4 StandardName[32], +68 StandardDate, +84 StandardBias,
   +88 DaylightName[32], +152 DaylightDate, +168 DaylightBias.
   The transition dates of the current year are absolute dates (wYear is
   not 0). */
void rt_GetTimeZoneInformation(_cpu *cpu)
{
    uint32_t g = ARG(0);
    time_t now = time(NULL), year_start, year_end, t1 = 0, t2 = 0;
    struct tm tm;
    long std_off, dst_off, off_start, off_mid;
    uint32_t result;
    int transitions = 0;

    if (g == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0xFFFFFFFFu);
        return;
    }
    memset(G2H(g), 0, 172);
    tzset();
    if (localtime_r(&now, &tm) == NULL) rt_fatal("localtime_r failed");
    struct tm jan = { .tm_year = tm.tm_year, .tm_mon = 0, .tm_mday = 1, .tm_isdst = -1 };
    struct tm dec = { .tm_year = tm.tm_year + 1, .tm_mon = 0, .tm_mday = 1, .tm_isdst = -1 };
    year_start = mktime(&jan);
    year_end = mktime(&dec);
    if (year_start == (time_t)-1 || year_end == (time_t)-1) rt_fatal("mktime failed");

    off_start = gmtoff_at(year_start);
    /* Find up to two offset changes in the year, by days. */
    {
        time_t d, prev = year_start;
        long prev_off = off_start;
        for (d = year_start + 86400; d <= year_end; d += 86400)
        {
            long o = gmtoff_at(d);
            if (o != prev_off)
            {
                time_t tr = find_transition(prev, d);
                if (transitions == 0) t1 = tr;
                else if (transitions == 1) t2 = tr;
                transitions++;
                prev_off = o;
            }
            prev = d;
        }
    }
    if (transitions == 0)
    {
        std_off = dst_off = off_start;
    }
    else
    {
        off_mid = gmtoff_at(t1);
        /* Standard time has the smaller offset. */
        std_off = off_start < off_mid ? off_start : off_mid;
        dst_off = off_start < off_mid ? off_mid : off_start;
    }

    wr32(g + 0, (uint32_t)(int32_t)(-(std_off / 60)));
    write_wname(g + 4, tzname[0]);
    write_wname(g + 88, tzname[1]);
    wr32(g + 84, 0);
    if (transitions >= 2)
    {
        time_t to_dst, to_std;
        struct tm a;
        /* The local time of a change is given in the time before the change. */
        if (gmtoff_at(t1) == dst_off) { to_dst = t1; to_std = t2; }
        else { to_dst = t2; to_std = t1; }
        time_t lt = to_dst - 1;
        if (localtime_r(&lt, &a) == NULL) rt_fatal("localtime_r failed");
        a.tm_sec += 1;
        if (a.tm_sec == 60) { a.tm_sec = 0; a.tm_min++; }
        if (a.tm_min == 60) { a.tm_min = 0; a.tm_hour++; }
        rt_write_systemtime(g + 152, &a, 0);
        lt = to_std - 1;
        if (localtime_r(&lt, &a) == NULL) rt_fatal("localtime_r failed");
        a.tm_sec += 1;
        if (a.tm_sec == 60) { a.tm_sec = 0; a.tm_min++; }
        if (a.tm_min == 60) { a.tm_min = 0; a.tm_hour++; }
        rt_write_systemtime(g + 68, &a, 0);
        wr32(g + 168, (uint32_t)(int32_t)(-((dst_off - std_off) / 60)));
        result = (gmtoff_at(now) == dst_off) ? 2u : 1u; /* TIME_ZONE_ID_DAYLIGHT / STANDARD */
    }
    else if (transitions == 1)
    {
        /* A permanent change of the zone during this year (no daylight
           saving). Report the current offset as standard time. */
        wr32(g + 0, (uint32_t)(int32_t)(-(gmtoff_at(now) / 60)));
        result = 0u; /* TIME_ZONE_ID_UNKNOWN */
    }
    else
    {
        result = 0u;
    }
    RET(result);
}

void rt_CompareFileTime(_cpu *cpu)
{
    uint64_t a = rd64(ARG(0)), b = rd64(ARG(1));
    RET(a < b ? 0xFFFFFFFFu : (a > b ? 1u : 0u));
}
