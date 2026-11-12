/* Kernel objects, the handle table, and waits on objects. */
#include "rt_object.h"
#include "rt_thread.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_HANDLES 65536u

static pthread_mutex_t obj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t obj_cond = PTHREAD_COND_INITIALIZER;
static rt_object *handles[MAX_HANDLES];
static uint32_t next_free_hint;

void rt_obj_lock(void)
{
    int r = pthread_mutex_lock(&obj_lock);
    if (r != 0) rt_fatal("object lock failed (%d)", r);
}

void rt_obj_unlock(void)
{
    int r = pthread_mutex_unlock(&obj_lock);
    if (r != 0) rt_fatal("object unlock failed (%d)", r);
}

/* A thread in wait_objects. Each waiter has its own condition variable,
   and one node on the wait list of each object that it waits for. A
   signal wakes only the waiters of the signaled object. */
typedef struct waiter waiter;
struct waiter {
    pthread_cond_t cv;
    bool woken;
    waiter *prev, *next; /* list of all waiters (all_waiters) */
};
struct rt_wait_node {
    waiter *w;
    rt_wait_node *prev, *next;
};
static waiter *all_waiters;

static void wake(waiter *w)
{
    int r;
    w->woken = true;
    r = pthread_cond_signal(&w->cv);
    if (r != 0) rt_fatal("object signal failed (%d)", r);
}

void rt_obj_broadcast(void)
{
    waiter *w;
    int r = pthread_cond_broadcast(&obj_cond);
    if (r != 0) rt_fatal("object broadcast failed (%d)", r);
    for (w = all_waiters; w != NULL; w = w->next) wake(w);
}

void rt_obj_signal(rt_object *o, uint32_t n)
{
    rt_wait_node *x;
    uint32_t k = 0;
    for (x = o->wait_head; x != NULL && (n == 0 || k < n); x = x->next)
    {
        if (x->w->woken) continue;
        wake(x->w);
        k++;
    }
}

void rt_obj_wait(void)
{
    int r = pthread_cond_wait(&obj_cond, &obj_lock);
    if (r != 0) rt_fatal("condition wait failed (%d)", r);
}

rt_object *rt_object_new(rt_object_type type)
{
    rt_object *o = calloc(1, sizeof(*o));
    if (o == NULL) rt_fatal("out of host memory for a kernel object");
    o->type = type;
    o->refs = 1;
    return o;
}

void rt_object_ref(rt_object *o)
{
    rt_obj_lock();
    if (o->refs <= 0 || o->refs == INT32_MAX) rt_fatal("bad reference count %d of an object", o->refs);
    o->refs++;
    rt_obj_unlock();
}

/* Named events, mutexes and semaphores (protected by the object lock).
   The game is one process, so a name identifies an object in this process
   only. An entry is removed when the last reference of its object goes. */
#define MAX_NAMED 256u
static rt_object *named[MAX_NAMED];

void rt_object_unref(rt_object *o)
{
    int left;
    uint32_t i;
    rt_obj_lock();
    if (o->refs <= 0) rt_fatal("release of an object without references");
    left = --o->refs;
    if (left == 0 && o->name != NULL)
    {
        for (i = 0; i < MAX_NAMED; i++)
            if (named[i] == o) named[i] = NULL;
    }
    rt_obj_unlock();
    if (left == 0)
    {
        if (o->destroy != NULL) o->destroy(o);
        free(o->name);
        free(o);
    }
}

uint32_t rt_handle_new(rt_object *o)
{
    uint32_t i, n;
    rt_obj_lock();
    for (n = 0; n < MAX_HANDLES; n++)
    {
        i = (next_free_hint + n) % MAX_HANDLES;
        if (i == 0) continue; /* handle 0 is invalid */
        if (handles[i] == NULL)
        {
            if (o->refs == INT32_MAX) rt_fatal("too many references to an object");
            o->refs++;
            handles[i] = o;
            next_free_hint = i + 1;
            rt_obj_unlock();
            return i * 4u;
        }
    }
    rt_obj_unlock();
    return 0;
}

static uint32_t handle_index(uint32_t h)
{
    if (h == 0 || (h & 3u) != 0) return 0;
    if (h / 4u >= MAX_HANDLES) return 0;
    return h / 4u;
}

rt_object *rt_handle_get(uint32_t h, rt_object_type type)
{
    uint32_t i = handle_index(h);
    rt_object *o;
    if (i == 0) return NULL;
    rt_obj_lock();
    o = handles[i];
    if (o != NULL && (type == 0 || o->type == type))
    {
        o->refs++;
    }
    else
    {
        o = NULL;
    }
    rt_obj_unlock();
    return o;
}

bool rt_handle_close(uint32_t h)
{
    uint32_t i = handle_index(h);
    rt_object *o;
    if (i == 0) return false;
    rt_obj_lock();
    o = handles[i];
    handles[i] = NULL;
    if (o != NULL && i < next_free_hint) next_free_hint = i;
    rt_obj_unlock();
    if (o == NULL) return false;
    rt_object_unref(o);
    return true;
}

void rt_obj_thread_exited(rt_object *thread_obj, uint32_t tid, uint32_t exit_code)
{
    uint32_t i;
    rt_obj_lock();
    for (i = 1; i < MAX_HANDLES; i++)
    {
        rt_object *o = handles[i];
        if (o != NULL && o->type == OBJ_MUTEX && o->owner_tid == tid)
        {
            o->owner_tid = 0;
            o->recursion = 0;
            o->abandoned = true;
        }
    }
    if (thread_obj != NULL)
    {
        thread_obj->signaled = true;
        thread_obj->exit_code = exit_code;
    }
    /* Thread exit is rare: wake every waiter (abandoned mutexes too). */
    rt_obj_broadcast();
    rt_obj_unlock();
}

/* ---------------------------------------------------------------- waits */

#define WAIT_OBJECT_0 0u
#define WAIT_ABANDONED_0 0x80u
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE 0xFFFFFFFFu
#define MAXIMUM_WAIT_OBJECTS 64

static bool is_waitable(const rt_object *o)
{
    return o->type == OBJ_EVENT || o->type == OBJ_MUTEX || o->type == OBJ_SEMAPHORE || o->type == OBJ_THREAD;
}

/* Object lock held. */
static bool can_acquire(const rt_object *o, uint32_t tid)
{
    switch (o->type)
    {
    case OBJ_EVENT:
    case OBJ_THREAD:
        return o->signaled;
    case OBJ_MUTEX:
        return o->owner_tid == 0 || o->owner_tid == tid;
    case OBJ_SEMAPHORE:
        return o->count > 0;
    default:
        return false;
    }
}

/* Object lock held. Returns true if the mutex was abandoned. */
static bool acquire(rt_object *o, uint32_t tid)
{
    bool was_abandoned = false;
    switch (o->type)
    {
    case OBJ_EVENT:
        if (!o->manual_reset) o->signaled = false;
        break;
    case OBJ_MUTEX:
        if (o->recursion == UINT32_MAX) rt_fatal("mutex recursion count overflow");
        o->owner_tid = tid;
        o->recursion++;
        was_abandoned = o->abandoned;
        o->abandoned = false;
        break;
    case OBJ_SEMAPHORE:
        o->count--;
        break;
    default:
        break;
    }
    return was_abandoned;
}

static uint64_t now_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* Wait on the condition variable of w until the deadline. Object lock
   held. Returns false when the deadline passed. */
static bool cond_wait_until(waiter *w, bool infinite, uint64_t deadline)
{
    int r;
    if (infinite)
    {
        r = pthread_cond_wait(&w->cv, &obj_lock);
        if (r != 0) rt_fatal("condition wait failed (%d)", r);
        return true;
    }
    uint64_t t = now_ns();
    if (t >= deadline) return false;
    uint64_t rem = deadline - t;
    struct timespec ts = { (time_t)(rem / 1000000000u), (long)(rem % 1000000000u) };
    r = pthread_cond_timedwait_relative_np(&w->cv, &obj_lock, &ts);
    if (r != 0 && r != ETIMEDOUT) rt_fatal("timed condition wait failed (%d)", r);
    return true;
}

/* Object lock held. True if a waiter could take o now (a mutex: if it has
   no owner). */
static bool available(const rt_object *o)
{
    switch (o->type)
    {
    case OBJ_EVENT:
    case OBJ_THREAD:
        return o->signaled;
    case OBJ_MUTEX:
        return o->owner_tid == 0;
    case OBJ_SEMAPHORE:
        return o->count > 0;
    default:
        return false;
    }
}

/* Try to finish the wait. Object lock held. Returns true and sets *result
   when the wait is satisfied. */
static bool try_acquire(rt_object **objs, uint32_t count, bool wait_all, uint32_t tid, uint32_t *result)
{
    uint32_t i;
    if (wait_all)
    {
        bool all = true, ab = false;
        for (i = 0; i < count && all; i++) all = can_acquire(objs[i], tid);
        if (!all) return false;
        for (i = 0; i < count; i++) ab |= acquire(objs[i], tid);
        *result = ab ? WAIT_ABANDONED_0 : WAIT_OBJECT_0;
        return true;
    }
    for (i = 0; i < count; i++)
    {
        if (can_acquire(objs[i], tid))
        {
            *result = (acquire(objs[i], tid) ? WAIT_ABANDONED_0 : WAIT_OBJECT_0) + i;
            return true;
        }
    }
    return false;
}

/* Wait for count objects. Returns the Windows wait result. */
static uint32_t wait_objects(uint32_t count, const uint32_t *hs, bool wait_all, uint32_t ms)
{
    rt_object *objs[MAXIMUM_WAIT_OBJECTS];
    uint32_t i, result = WAIT_FAILED;
    uint32_t tid = rt_self()->tid;
    bool infinite = (ms == INFINITE);
    uint64_t deadline = infinite ? 0 : now_ns() + (uint64_t)ms * 1000000u;

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    for (i = 0; i < count; i++)
    {
        if (hs[i] == RT_PSEUDO_PROCESS || hs[i] == RT_PSEUDO_THREAD)
            rt_fatal("wait on a pseudo handle 0x%x (the current process or thread)", hs[i]);
        objs[i] = rt_handle_get(hs[i], 0);
        if (objs[i] == NULL || !is_waitable(objs[i]))
        {
            uint32_t j;
            if (objs[i] != NULL) rt_object_unref(objs[i]);
            for (j = 0; j < i; j++) rt_object_unref(objs[j]);
            rt_set_last_error(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
    }

    rt_obj_lock();
    if (!try_acquire(objs, count, wait_all, tid, &result))
    {
        if (!infinite && ms == 0)
        {
            result = WAIT_TIMEOUT;
        }
        else
        {
            waiter me;
            rt_wait_node nodes[MAXIMUM_WAIT_OBJECTS];
            bool was_woken = false;
            int r = pthread_cond_init(&me.cv, NULL);
            if (r != 0) rt_fatal("condition variable creation failed (%d)", r);
            me.woken = false;
            me.prev = NULL;
            me.next = all_waiters;
            if (all_waiters != NULL) all_waiters->prev = &me;
            all_waiters = &me;
            for (i = 0; i < count; i++)
            {
                nodes[i].w = &me;
                nodes[i].next = NULL;
                nodes[i].prev = objs[i]->wait_tail;
                if (objs[i]->wait_tail != NULL)
                    objs[i]->wait_tail->next = &nodes[i];
                else
                    objs[i]->wait_head = &nodes[i];
                objs[i]->wait_tail = &nodes[i];
            }
            for (;;)
            {
                bool in_time;
                me.woken = false;
                in_time = cond_wait_until(&me, infinite, deadline);
                was_woken |= me.woken;
                if (try_acquire(objs, count, wait_all, tid, &result)) break;
                if (!in_time)
                {
                    result = WAIT_TIMEOUT;
                    break;
                }
            }
            for (i = 0; i < count; i++)
            {
                if (nodes[i].prev != NULL) nodes[i].prev->next = nodes[i].next; else objs[i]->wait_head = nodes[i].next;
                if (nodes[i].next != NULL) nodes[i].next->prev = nodes[i].prev; else objs[i]->wait_tail = nodes[i].prev;
            }
            if (me.prev != NULL) me.prev->next = me.next; else all_waiters = me.next;
            if (me.next != NULL) me.next->prev = me.prev;
            /* A wakeup that this thread did not use (it took another
               object, its time ran out, or another thread was first) goes
               to the next waiter of each object that is still available. */
            if (was_woken)
                for (i = 0; i < count; i++)
                    if (available(objs[i])) rt_obj_signal(objs[i], 1);
            r = pthread_cond_destroy(&me.cv);
            if (r != 0) rt_fatal("condition variable destruction failed (%d)", r);
        }
    }
    rt_obj_unlock();

    for (i = 0; i < count; i++) rt_object_unref(objs[i]);
    return result;
}

void rt_WaitForSingleObject(_cpu *cpu)
{
    uint32_t h = ARG(0);
    RET(wait_objects(1, &h, false, ARG(1)));
}

void rt_WaitForMultipleObjects(_cpu *cpu)
{
    uint32_t count = ARG(0), arr = ARG(1), wait_all = ARG(2), ms = ARG(3);
    uint32_t hs[MAXIMUM_WAIT_OBJECTS];
    uint32_t i;
    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || arr == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(WAIT_FAILED);
        return;
    }
    for (i = 0; i < count; i++) hs[i] = rd32(arr + 4u * i);
    RET(wait_objects(count, hs, wait_all != 0, ms));
}

/* ---------------------------------------------------------------- creation */

static uint32_t publish(rt_object *o)
{
    uint32_t h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        return 0;
    }
    rt_set_last_error(ERROR_SUCCESS);
    return h;
}

/* CreateXxxA with a name (guest string gname, not 0). If an object with
   the name exists, *existing gets it with one new reference (NULL and
   ERROR_INVALID_HANDLE for an object of another type). Otherwise o (a new
   object) gets the name. Returns false if the call must fail. */
static bool name_object(uint32_t gname, rt_object *o, rt_object **existing, const char *what)
{
    char buf[260];
    uint32_t i, free_slot = MAX_NAMED;
    *existing = NULL;
    if (rt_gstr(buf, sizeof(buf), gname) != 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return false;
    }
    rt_obj_lock();
    for (i = 0; i < MAX_NAMED; i++)
    {
        if (named[i] == NULL)
        {
            if (free_slot == MAX_NAMED) free_slot = i;
        }
        else if (strcmp(named[i]->name, buf) == 0)
        {
            if (named[i]->type != o->type)
            {
                rt_obj_unlock();
                rt_set_last_error(ERROR_INVALID_HANDLE);
                return false;
            }
            if (named[i]->refs == INT32_MAX) rt_fatal("too many references to an object");
            named[i]->refs++;
            *existing = named[i];
            rt_obj_unlock();
            return true;
        }
    }
    if (free_slot == MAX_NAMED) rt_fatal("%s: more than %u named objects", what, MAX_NAMED);
    o->name = strdup(buf);
    if (o->name == NULL) rt_fatal("out of host memory (object name)");
    named[free_slot] = o;
    rt_obj_unlock();
    return true;
}

/* Finish CreateXxxA: the handle of the new object o, or of the existing
   object with the same name (ERROR_ALREADY_EXISTS). */
static uint32_t create_named(uint32_t gname, rt_object *o, const char *what)
{
    rt_object *existing;
    uint32_t h;
    if (gname == 0) return publish(o);
    if (!name_object(gname, o, &existing, what))
    {
        rt_object_unref(o);
        return 0;
    }
    if (existing == NULL) return publish(o);
    rt_object_unref(o);
    h = publish(existing);
    if (h != 0) rt_set_last_error(ERROR_ALREADY_EXISTS);
    return h;
}

void rt_CreateEventA(_cpu *cpu)
{
    rt_object *o;
    o = rt_object_new(OBJ_EVENT);
    o->manual_reset = ARG(1) != 0;
    o->signaled = ARG(2) != 0;
    RET(create_named(ARG(3), o, "CreateEventA"));
}

void rt_CreateMutexA(_cpu *cpu)
{
    rt_object *o;
    o = rt_object_new(OBJ_MUTEX);
    /* For an existing named mutex, Windows ignores the initial owner flag;
       the new object with the owner is released again. */
    if (ARG(1) != 0)
    {
        o->owner_tid = rt_self()->tid;
        o->recursion = 1;
    }
    RET(create_named(ARG(2), o, "CreateMutexA"));
}

void rt_CreateSemaphoreA(_cpu *cpu)
{
    int32_t initial = (int32_t)ARG(1), maximum = (int32_t)ARG(2);
    rt_object *o;
    if (maximum <= 0 || initial < 0 || initial > maximum)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    o = rt_object_new(OBJ_SEMAPHORE);
    o->count = initial;
    o->max_count = maximum;
    RET(create_named(ARG(3), o, "CreateSemaphoreA"));
}

static void set_event_state(_cpu *cpu, bool state)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_EVENT);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_obj_lock();
    o->signaled = state;
    if (state) rt_obj_signal(o, o->manual_reset ? 0u : 1u);
    rt_obj_unlock();
    rt_object_unref(o);
    RET(1);
}

void rt_SetEvent(_cpu *cpu) { set_event_state(cpu, true); }
void rt_ResetEvent(_cpu *cpu) { set_event_state(cpu, false); }

void rt_ReleaseSemaphore(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_SEMAPHORE);
    int32_t n = (int32_t)ARG(1);
    uint32_t prev_ptr = ARG(2);
    bool ok;
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_obj_lock();
    ok = n > 0 && o->count <= o->max_count - n;
    if (ok)
    {
        if (prev_ptr != 0) wr32(prev_ptr, (uint32_t)o->count);
        o->count += n;
        rt_obj_signal(o, (uint32_t)n);
    }
    rt_obj_unlock();
    rt_object_unref(o);
    if (!ok) rt_set_last_error(298u); /* ERROR_TOO_MANY_POSTS */
    RET(ok ? 1 : 0);
}

/* ReleaseMutex is not imported; mutexes are released only through thread
   exit (abandonment). CloseHandle works for every object type. */
void rt_CloseHandle(_cpu *cpu)
{
    uint32_t h = ARG(0);
    if (h == RT_PSEUDO_PROCESS || h == RT_PSEUDO_THREAD)
    {
        RET(1);
        return;
    }
    if (!rt_handle_close(h))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    RET(1);
}
