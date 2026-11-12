/*
 * KERNEL32 threads and fibers.
 *
 * Each guest thread is one POSIX thread with its own guest stack and TEB.
 *
 * Fibers: the translated code makes every guest call, jump and return a
 * tail call, so the native stack does not grow with guest calls. Only a
 * call from the runtime into guest code (rt_call) adds native frames. A
 * fiber switch saves and loads the guest registers and the TEB fields of
 * the fiber, and continues at the saved guest address. The switch is
 * correct only if the target fiber was suspended at the same native call
 * depth. The runtime checks this and stops the process otherwise.
 */
#include "rt_object.h"
#include "rt_thread.h"

#include <errno.h>
#include <mach/mach.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define CREATE_SUSPENDED 0x4u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000u
#define STILL_ACTIVE 259u
#define DEFAULT_STACK 0x100000u
#define NATIVE_STACK_SIZE (8u * 1024u * 1024u)

typedef struct thread_ctl {
    uint32_t start, param, stack_size, tid;
    uint32_t suspend_count;   /* protected by the object lock */
    bool running;             /* the native thread exists (object lock) */
    mach_port_t port;         /* mach thread port of the native thread */
    pthread_t pt;
    rt_thread *state;         /* guest state, NULL before start and after exit */
} thread_ctl;

static void thread_ctl_destroy(rt_object *o)
{
    free(o->data);
}

/* The thread object of the calling thread or NULL for the main thread. */
static thread_ctl *ctl_of(rt_object *o)
{
    return (thread_ctl *)o->data;
}

_Noreturn void rt_thread_exit(uint32_t code)
{
    rt_thread *t = rt_self();
    if (!t->exit_jmp_set) rt_fatal("ExitThread on a thread without an exit point");
    t->exit_code = code;
    longjmp(t->exit_jmp, 1);
}

static void *thread_main(void *arg)
{
    rt_object *o = arg;
    thread_ctl *c = ctl_of(o);
    rt_thread *t;
    uint32_t code;

    /* A thread created suspended waits for ResumeThread here. */
    rt_obj_lock();
    while (c->suspend_count > 0) rt_obj_wait();
    rt_obj_unlock();

    t = rt_thread_create_state(c->stack_size, c->tid);
    t->obj = o;
    rt_obj_lock();
    c->state = t;
    rt_obj_unlock();

    t->exit_jmp_set = true;
    if (setjmp(t->exit_jmp) == 0)
    {
        rt_module_thread_attach(t->cpu);
        code = rt_call_thread_start(t->cpu, c->start, c->param);
        t->exit_code = code;
    }
    /* ExitThread continues here with the native stack of thread_main. */
    t->exit_jmp_set = false;
    code = t->exit_code;
    /* ExitThread can leave nested runtime-to-guest calls; longjmp removed
       their native frames. */
    t->call_depth = 0;
    t->exit_jmp_set = true;
    if (setjmp(t->exit_jmp) == 0)
    {
        rt_module_thread_detach(t->cpu);
    }
    t->exit_jmp_set = false;

    rt_obj_lock();
    c->state = NULL;
    c->running = false;
    rt_obj_unlock();
    rt_obj_thread_exited(o, t->tid, code);
    rt_thread_destroy_state(t);
    rt_object_unref(o);
    return NULL;
}

void rt_CreateThread(_cpu *cpu)
{
    uint32_t stack = ARG(1), start = ARG(2), param = ARG(3), flags = ARG(4), tid_ptr = ARG(5);
    rt_object *o;
    thread_ctl *c;
    pthread_attr_t attr;
    uint32_t h;
    int r;

    if (start == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if ((flags & ~(CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION)) != 0)
        rt_fatal("CreateThread flags 0x%x are not supported", flags);

    c = calloc(1, sizeof(*c));
    if (c == NULL) rt_fatal("out of host memory for a thread");
    c->start = start;
    c->param = param;
    c->stack_size = (stack != 0) ? stack : DEFAULT_STACK;
    if (c->stack_size < DEFAULT_STACK && (flags & STACK_SIZE_PARAM_IS_A_RESERVATION) == 0)
    {
        /* stack is the commit size; the reserve size is the default. */
        c->stack_size = DEFAULT_STACK;
    }
    c->tid = rt_thread_alloc_tid();
    c->suspend_count = (flags & CREATE_SUSPENDED) ? 1u : 0u;
    c->running = true;

    o = rt_object_new(OBJ_THREAD);
    o->data = c;
    o->destroy = thread_ctl_destroy;
    o->exit_code = STILL_ACTIVE;

    h = rt_handle_new(o);
    if (h == 0)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(0);
        return;
    }
    /* The native thread holds one reference until it ends. */
    rt_object_ref(o);

    if (pthread_attr_init(&attr) != 0) rt_fatal("pthread_attr_init failed");
    if (pthread_attr_setstacksize(&attr, NATIVE_STACK_SIZE) != 0) rt_fatal("pthread_attr_setstacksize failed");
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) rt_fatal("pthread_attr_setdetachstate failed");
    r = pthread_create(&c->pt, &attr, thread_main, o);
    pthread_attr_destroy(&attr);
    if (r != 0)
    {
        rt_object_unref(o);
        rt_handle_close(h);
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    c->port = pthread_mach_thread_np(c->pt);
    if (tid_ptr != 0) wr32(tid_ptr, c->tid);
    rt_set_last_error(ERROR_SUCCESS);
    RET(h);
}

uint32_t rt_ExitThread(_cpu *cpu)
{
    rt_thread_exit(ARG(0));
}

void rt_GetExitCodeThread(_cpu *cpu)
{
    rt_object *o;
    uint32_t out = ARG(1);
    if (ARG(0) == RT_PSEUDO_THREAD)
    {
        wr32(out, STILL_ACTIVE);
        RET(1);
        return;
    }
    o = rt_handle_get(ARG(0), OBJ_THREAD);
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_obj_lock();
    wr32(out, o->signaled ? o->exit_code : STILL_ACTIVE);
    rt_obj_unlock();
    rt_object_unref(o);
    RET(1);
}

/* Suspend count changes. A thread that was created suspended waits in
   thread_main. A running thread is suspended with the Mach kernel. */
void rt_ResumeThread(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_THREAD);
    thread_ctl *c;
    uint32_t prev;
    bool started;

    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0xFFFFFFFFu);
        return;
    }
    c = ctl_of(o);
    rt_obj_lock();
    prev = c->suspend_count;
    started = c->state != NULL;
    if (prev > 0)
    {
        c->suspend_count--;
        if (started)
        {
            kern_return_t kr = thread_resume(c->port);
            if (kr != KERN_SUCCESS) rt_fatal("thread_resume failed (%d)", kr);
        }
        rt_obj_broadcast();
    }
    rt_obj_unlock();
    rt_object_unref(o);
    RET(prev);
}

void rt_SuspendThread(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_THREAD);
    thread_ctl *c;
    uint32_t prev;

    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0xFFFFFFFFu);
        return;
    }
    c = ctl_of(o);
    rt_obj_lock();
    prev = c->suspend_count;
    if (prev >= 127u)
    {
        rt_obj_unlock();
        rt_object_unref(o);
        rt_set_last_error(156u); /* ERROR_SIGNAL_REFUSED */
        RET(0xFFFFFFFFu);
        return;
    }
    if (c->state != NULL)
    {
        if (c->state == rt_self()) rt_fatal("SuspendThread of the calling thread is not supported");
        kern_return_t kr = thread_suspend(c->port);
        if (kr != KERN_SUCCESS) rt_fatal("thread_suspend failed (%d)", kr);
    }
    c->suspend_count++;
    rt_obj_unlock();
    rt_object_unref(o);
    RET(prev);
}

/* TerminateThread cannot stop a POSIX thread at an arbitrary point without
   leaving runtime locks and memory in an unknown state. The runtime
   supports it only for a thread that has already ended. */
void rt_TerminateThread(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_THREAD);
    bool ended;
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    rt_obj_lock();
    ended = o->signaled;
    rt_obj_unlock();
    rt_object_unref(o);
    if (!ended) rt_fatal("TerminateThread of a running thread is not supported");
    RET(1);
}

void rt_GetCurrentThread(_cpu *cpu) { RET(RT_PSEUDO_THREAD); }
void rt_GetCurrentThreadId(_cpu *cpu) { RET(rt_self()->tid); }
void rt_GetCurrentProcess(_cpu *cpu) { RET(RT_PSEUDO_PROCESS); }
void rt_GetCurrentProcessId(_cpu *cpu) { RET(rt_current_process_id()); }

static bool valid_thread_handle(uint32_t h)
{
    rt_object *o;
    if (h == RT_PSEUDO_THREAD) return true;
    o = rt_handle_get(h, OBJ_THREAD);
    if (o == NULL) return false;
    rt_object_unref(o);
    return true;
}

/* Thread priorities. macOS schedules game threads well with the default
   policy; the runtime keeps the value so that the guest can read it, and
   maps it to the POSIX priority of the thread. */
void rt_SetThreadPriority(_cpu *cpu)
{
    int32_t prio = (int32_t)ARG(1);
    if (!valid_thread_handle(ARG(0)))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (!(prio >= -2 && prio <= 2) && prio != -15 && prio != 15)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    RET(1);
}

/* Processor affinity. The process mask has one bit per logical processor
   (at most 32). macOS has no hard affinity; the mask is checked and
   accepted. */
static uint32_t process_mask(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) rt_fatal("sysconf(_SC_NPROCESSORS_ONLN) failed");
    if (n >= 32) return 0xFFFFFFFFu;
    return (1u << n) - 1u;
}

void rt_GetProcessAffinityMask(_cpu *cpu)
{
    if (ARG(0) != RT_PSEUDO_PROCESS) rt_fatal("GetProcessAffinityMask of another process");
    if (ARG(1) == 0 || ARG(2) == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    wr32(ARG(1), process_mask());
    wr32(ARG(2), process_mask());
    RET(1);
}

void rt_SetThreadAffinityMask(_cpu *cpu)
{
    uint32_t mask = ARG(1);
    if (!valid_thread_handle(ARG(0)))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (mask == 0 || (mask & ~process_mask()) != 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    /* Return value: the previous mask. */
    RET(process_mask());
}

void rt_Sleep(_cpu *cpu)
{
    uint32_t ms = ARG(0);
    if (ms == 0)
    {
        sched_yield();
        return;
    }
    if (ms == 0xFFFFFFFFu) rt_fatal("Sleep(INFINITE)");
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) != 0)
    {
        if (errno != EINTR) rt_fatal("nanosleep failed (%d)", errno);
    }
}

/* ---------------------------------------------------------------- fibers */

#define FIBER_BLOCK_SIZE 0x20u
#define FIBER_MAGIC 0x52424946u /* "FIBR" */
#define MAX_FIBERS 4096u

typedef struct rt_fiber {
    uint32_t guest;          /* guest address of the fiber block */
    uint32_t magic;
    bool converted;          /* made by ConvertThreadToFiber */
    bool started;
    uint32_t start, param;
    uint32_t stack_base, stack_size;
    int call_depth;          /* native call depth when suspended */
    uint32_t r_ebx, r_esi, r_edi, r_ebp, r_esp, r_eip;
    uint32_t st_cw, mxcsr;
    uint32_t seh_list, stack_top, stack_limit;
} rt_fiber;

/* Fiber block in guest memory (the address is the fiber handle):
   +0x00 fiber data (the parameter, read by GetFiberData)
   +0x04 index of the host fiber state */
static pthread_mutex_t fiber_lock = PTHREAD_MUTEX_INITIALIZER;
static rt_fiber *fibers[MAX_FIBERS];

static rt_fiber *fiber_new(uint32_t param)
{
    uint32_t i;
    rt_fiber *f = calloc(1, sizeof(*f));
    if (f == NULL) rt_fatal("out of host memory for a fiber");
    f->guest = rt_heap_calloc(1, FIBER_BLOCK_SIZE);
    if (f->guest == 0)
    {
        free(f);
        return NULL;
    }
    f->magic = FIBER_MAGIC;
    f->param = param;
    if (pthread_mutex_lock(&fiber_lock) != 0) rt_fatal("fiber lock failed");
    for (i = 1; i < MAX_FIBERS && fibers[i] != NULL; i++) {}
    if (i == MAX_FIBERS) rt_fatal("more than %u fibers", MAX_FIBERS - 1);
    fibers[i] = f;
    if (pthread_mutex_unlock(&fiber_lock) != 0) rt_fatal("fiber unlock failed");
    wr32(f->guest + 0, param);
    wr32(f->guest + 4, i);
    return f;
}

static rt_fiber *fiber_of(uint32_t guest)
{
    uint32_t i;
    rt_fiber *f;
    if (guest == 0 || !rt_va_is_committed(guest, FIBER_BLOCK_SIZE)) rt_fatal("bad fiber handle 0x%08x", guest);
    i = rd32(guest + 4);
    if (i == 0 || i >= MAX_FIBERS) rt_fatal("bad fiber handle 0x%08x", guest);
    if (pthread_mutex_lock(&fiber_lock) != 0) rt_fatal("fiber lock failed");
    f = fibers[i];
    if (pthread_mutex_unlock(&fiber_lock) != 0) rt_fatal("fiber unlock failed");
    if (f == NULL || f->guest != guest || f->magic != FIBER_MAGIC) rt_fatal("bad fiber handle 0x%08x", guest);
    return f;
}

static void fiber_free(rt_fiber *f)
{
    uint32_t i = rd32(f->guest + 4);
    if (pthread_mutex_lock(&fiber_lock) != 0) rt_fatal("fiber lock failed");
    fibers[i] = NULL;
    if (pthread_mutex_unlock(&fiber_lock) != 0) rt_fatal("fiber unlock failed");
    if (f->stack_base != 0 && rt_va_release(f->stack_base) != 0) rt_fatal("release of a fiber stack failed");
    rt_heap_free(f->guest);
    f->magic = 0;
    free(f);
}

void rt_ConvertThreadToFiber(_cpu *cpu)
{
    rt_thread *t = rt_self();
    rt_fiber *f;
    if (t->fiber != NULL)
    {
        rt_set_last_error(1280u); /* ERROR_ALREADY_FIBER */
        RET(0);
        return;
    }
    f = fiber_new(ARG(0));
    if (f == NULL)
    {
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    f->converted = true;
    f->started = true;
    t->fiber = f;
    wr32(t->teb + TEB_FIBER_DATA, f->guest);
    RET(f->guest);
}

void rt_ConvertFiberToThread(_cpu *cpu)
{
    rt_thread *t = rt_self();
    rt_fiber *f = t->fiber;
    if (f == NULL)
    {
        rt_set_last_error(1281u); /* ERROR_ALREADY_THREAD */
        RET(0);
        return;
    }
    if (!f->converted) rt_fatal("ConvertFiberToThread on a fiber made by CreateFiber");
    t->fiber = NULL;
    wr32(t->teb + TEB_FIBER_DATA, 0);
    fiber_free(f);
    RET(1);
}

void rt_CreateFiber(_cpu *cpu)
{
    uint32_t size = ARG(0), start = ARG(1), param = ARG(2);
    rt_fiber *f;
    if (start == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (size == 0) size = DEFAULT_STACK;
    if (size > 0x10000000u)
    {
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    size = (size + RT_GRANULE - 1u) & ~(RT_GRANULE - 1u);
    f = fiber_new(param);
    if (f == NULL)
    {
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    f->stack_base = rt_va_reserve(0, size);
    if (f->stack_base == 0 || rt_va_commit(f->stack_base, size) != 0)
    {
        fiber_free(f);
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    f->stack_size = size;
    f->start = start;
    f->st_cw = 0x027F;
    f->mxcsr = 0x1F80;
    f->seh_list = 0xFFFFFFFFu;
    f->stack_top = f->stack_base + size;
    f->stack_limit = f->stack_base;
    RET(f->guest);
}

void rt_DeleteFiber(_cpu *cpu)
{
    rt_thread *t = rt_self();
    rt_fiber *f = fiber_of(ARG(0));
    if (f == t->fiber)
    {
        /* Windows: deleting the running fiber ends the thread. */
        t->fiber = NULL;
        wr32(t->teb + TEB_FIBER_DATA, 0);
        fiber_free(f);
        rt_thread_exit(0);
    }
    fiber_free(f);
}

extern void rt_fiber_exit_asm2c(_cpu *cpu);

/* SwitchToFiber(fiber), jump kind: [esp] return address, [esp+4] fiber. */
uint32_t rt_SwitchToFiber(_cpu *cpu)
{
    rt_thread *t = rt_self();
    rt_fiber *cur = t->fiber;
    rt_fiber *next = fiber_of(ARG(0));
    uint32_t ret = rd32(esp);

    if (cur == NULL) rt_fatal("SwitchToFiber on a thread that is not a fiber");

    cur->r_eip = ret;
    cur->r_esp = esp + 8u;
    cur->r_ebx = ebx;
    cur->r_esi = esi;
    cur->r_edi = edi;
    cur->r_ebp = ebp;
    cur->st_cw = cpu->_st_cw;
    cur->mxcsr = cpu->_mxcsr;
    cur->seh_list = rd32(t->teb + TEB_EXCEPTION_LIST);
    cur->stack_top = rd32(t->teb + TEB_STACK_BASE);
    cur->stack_limit = rd32(t->teb + TEB_STACK_LIMIT);
    cur->call_depth = t->call_depth;

    if (next->started && next->call_depth != t->call_depth)
    {
        rt_fatal("SwitchToFiber to a fiber that was suspended at native call depth %d, the current depth is %d",
                 next->call_depth, t->call_depth);
    }

    t->fiber = next;
    wr32(t->teb + TEB_FIBER_DATA, next->guest);
    wr32(t->teb + TEB_EXCEPTION_LIST, next->seh_list);
    wr32(t->teb + TEB_STACK_BASE, next->stack_top);
    wr32(t->teb + TEB_STACK_LIMIT, next->stack_limit);
    cpu->_st_cw = next->st_cw;
    cpu->_mxcsr = next->mxcsr;

    if (!next->started)
    {
        /* First run: the start routine gets the parameter and returns to
           rt_fiber_exit_asm2c. */
        uint32_t top = next->stack_base + next->stack_size - 16u;
        next->started = true;
        next->call_depth = t->call_depth;
        wr32(top - 4u, next->param);
        wr32(top - 8u, H2G((const void *)&rt_fiber_exit_asm2c));
        esp = top - 8u;
        ebx = esi = edi = ebp = 0;
        eax = ecx = edx = 0;
        eflags = 0x202;
        return next->start;
    }

    esp = next->r_esp;
    ebx = next->r_ebx;
    esi = next->r_esi;
    edi = next->r_edi;
    ebp = next->r_ebp;
    eflags &= ~(uint32_t)DF;
    return next->r_eip;
}

/* A fiber start routine returned (called from rt_fiber_exit_asm2c). On
   Windows the thread ends with the return value as the exit code. */
void rt_fiber_returned(_cpu *cpu)
{
    rt_thread_exit(eax);
}
