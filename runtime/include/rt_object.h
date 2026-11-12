/*
 * Kernel objects and handles.
 *
 * A guest handle is a nonzero multiple of 4 that identifies one entry of the
 * handle table. Each entry points to one object. An object has a reference
 * count: one reference for each handle and one for each runtime user.
 *
 * Waitable objects (events, mutexes, semaphores, threads) use one global
 * lock and one global condition variable. A change of a signal state
 * wakes all waiters, and each waiter checks its own objects again.
 */
#ifndef RT_OBJECT_H
#define RT_OBJECT_H

#include "rt.h"

#include <pthread.h>
#include <stdbool.h>

typedef enum {
    OBJ_EVENT = 1,
    OBJ_MUTEX,
    OBJ_SEMAPHORE,
    OBJ_THREAD,
    OBJ_FILE,
    OBJ_FIND,
    OBJ_REGKEY,
    OBJ_CRYPT,
    OBJ_DEVINFO,
    OBJ_SOCKET,
} rt_object_type;

typedef struct rt_object rt_object;
typedef struct rt_wait_node rt_wait_node;
struct rt_object {
    rt_object_type type;
    int refs;                      /* protected by the object lock */
    void (*destroy)(rt_object *o); /* releases the type-specific data; NULL if none */
    /* Wait state (protected by the object lock). */
    bool signaled;                 /* events, threads */
    bool manual_reset;             /* events */
    bool abandoned;                /* mutexes */
    uint32_t owner_tid;            /* mutexes: owner thread id, 0 if free */
    uint32_t recursion;            /* mutexes */
    int32_t count, max_count;      /* semaphores */
    uint32_t exit_code;            /* threads */
    void *data;                    /* type-specific data */
    char *name;                    /* events, mutexes, semaphores: the object name, or NULL */
    rt_wait_node *wait_head, *wait_tail; /* threads that wait for this object (object lock) */
};

#define RT_PSEUDO_PROCESS 0xFFFFFFFFu
#define RT_PSEUDO_THREAD 0xFFFFFFFEu

/* Allocate an object with one reference. Stops the process when no host
   memory is available. */
rt_object *rt_object_new(rt_object_type type);

/* Add a handle for o. The handle takes one new reference. Returns 0 if the
   handle table is full. */
uint32_t rt_handle_new(rt_object *o);

/* Get the object of handle h with one new reference, or NULL if h is not a
   handle of the given type (type 0: any type). */
rt_object *rt_handle_get(uint32_t h, rt_object_type type);

/* Remove handle h. Returns false if h is not a handle. */
bool rt_handle_close(uint32_t h);

void rt_object_ref(rt_object *o);
void rt_object_unref(rt_object *o);

/* The global object lock and the condition variable for waits. */
void rt_obj_lock(void);
void rt_obj_unlock(void);
/* Wake all waiters. Call with the object lock held. */
void rt_obj_broadcast(void);
/* Wake up to n threads that wait for o (n = 0: all of them), in arrival
   order. Call with the object lock held after o became signaled. */
void rt_obj_signal(rt_object *o, uint32_t n);
/* Wait for the next broadcast. Call with the object lock held. */
void rt_obj_wait(void);

/* Signal a thread object (thread exit) and mark the mutexes owned by tid
   as abandoned. Takes the object lock. */
void rt_obj_thread_exited(rt_object *thread_obj, uint32_t tid, uint32_t exit_code);

#endif
