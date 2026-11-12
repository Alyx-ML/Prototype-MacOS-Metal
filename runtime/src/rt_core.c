/*
 * Runtime core: messages, per-thread guest state, the thread environment
 * block (TEB, reached through the fs segment), and calls into guest code.
 */
#include "rt_thread.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern void c_rt_call_c2asm(_cpu *cpu);

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local rt_thread *self_thread;
static atomic_uint next_tid = 0x100;

/* ---------------------------------------------------------------- messages */

static void vlog(const char *prefix, const char *fmt, va_list ap)
{
    pthread_mutex_lock(&log_lock);
    fputs(prefix, stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&log_lock);
}

void rt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("[rt] ", fmt, ap);
    va_end(ap);
}

_Noreturn void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("[rt] fatal: ", fmt, ap);
    va_end(ap);
    abort();
}

/* ---------------------------------------------------------------- threads */

static pthread_mutex_t thread_list_lock = PTHREAD_MUTEX_INITIALIZER;
static rt_thread *thread_list;
uint32_t rt_peb;

rt_thread *rt_self(void)
{
    rt_thread *t = self_thread;
    if (t == NULL) rt_fatal("a thread without guest state called the runtime");
    return t;
}

rt_thread *rt_self_or_null(void)
{
    return self_thread;
}

uint32_t rt_current_process_id(void)
{
    return (uint32_t)getpid();
}

uint32_t rt_thread_alloc_tid(void)
{
    uint32_t tid = atomic_fetch_add(&next_tid, 4);
    if (tid == 0) rt_fatal("thread id overflow");
    return tid;
}

void rt_thread_foreach(void (*fn)(rt_thread *t, void *ctx), void *ctx)
{
    rt_thread *t;
    if (pthread_mutex_lock(&thread_list_lock) != 0) rt_fatal("thread list lock failed");
    for (t = thread_list; t != NULL; t = t->next) fn(t, ctx);
    if (pthread_mutex_unlock(&thread_list_lock) != 0) rt_fatal("thread list unlock failed");
}

/* Create the guest state for the calling native thread. stack_size is the
   guest stack size in bytes. */
rt_thread *rt_thread_create_state(uint32_t stack_size, uint32_t tid)
{
    rt_thread *t;
    _cpu *cpu;
    uint32_t teb, stack, errno_addr, tm_addr;

    if (stack_size > 0x10000000u) rt_fatal("thread stack size 0x%x is too large", stack_size);
    stack_size = (stack_size + RT_GRANULE - 1) & ~(RT_GRANULE - 1);
    if (stack_size == 0) stack_size = 0x100000;

    t = calloc(1, sizeof(*t));
    if (t == NULL) rt_fatal("out of host memory for a thread");
    /* The cpu state is in guest memory: the SR support helpers return the
       guest address of cpu fields (PTR2REG in llasm_float.c, for example
       x87_fstp_double), so the state must be in the guest window. */
    {
        uint32_t size = (uint32_t)((sizeof(_cpu) + RT_GRANULE - 1) & ~(size_t)(RT_GRANULE - 1));
        uint32_t g = rt_va_reserve(0, size);
        if (g == 0) rt_fatal("no guest memory for a cpu state");
        if (rt_va_commit(g, size) != 0) rt_fatal("commit of a cpu state failed");
        cpu = G2H(g);
        memset(cpu, 0, sizeof(*cpu));
    }

    stack = rt_va_reserve(0, stack_size);
    if (stack == 0) rt_fatal("no guest memory for a %u byte thread stack", stack_size);
    if (rt_va_commit(stack, stack_size) != 0) rt_fatal("commit of a thread stack failed");
    teb = rt_heap_calloc(1, TEB_SIZE);
    if (teb == 0) rt_fatal("no guest memory for a TEB");
    errno_addr = rt_heap_calloc(1, 16);
    tm_addr = rt_heap_calloc(1, 64);
    if (errno_addr == 0 || tm_addr == 0) rt_fatal("no guest memory for C runtime thread data");

    t->cpu = cpu;
    t->teb = teb;
    t->tid = (tid != 0) ? tid : rt_thread_alloc_tid();
    t->stack_base = stack;
    t->stack_size = stack_size;
    t->pt = pthread_self();
    t->crt_errno_addr = errno_addr;
    t->crt_tm = tm_addr;
    t->crt_rand = 1;

    wr32(teb + TEB_EXCEPTION_LIST, 0xFFFFFFFFu);
    wr32(teb + TEB_STACK_BASE, stack + stack_size);
    wr32(teb + TEB_STACK_LIMIT, stack);
    wr32(teb + TEB_SELF, teb);
    wr32(teb + TEB_PROCESS_ID, rt_current_process_id());
    wr32(teb + TEB_THREAD_ID, t->tid);
    wr32(teb + TEB_PEB, rt_peb);

    cpu->_pointer_offset = rt_ptrofs;
    cpu->_st_top = 0;
    cpu->_st_sw_cond = 0;
    cpu->_st_cw = 0x027F; /* Windows default: 53-bit precision, all exceptions masked */
    cpu->_mxcsr = 0x1F80;
    cpu->_esp = stack + stack_size - 16;
    cpu->_eflags = 0x202;
    cpu->stack_bottom = G2H(stack);
    cpu->stack_top = G2H(stack + stack_size);

    if (pthread_mutex_lock(&thread_list_lock) != 0) rt_fatal("thread list lock failed");
    t->next = thread_list;
    thread_list = t;
    if (pthread_mutex_unlock(&thread_list_lock) != 0) rt_fatal("thread list unlock failed");

    self_thread = t;
    return t;
}

void rt_thread_destroy_state(rt_thread *t)
{
    rt_thread **pp;

    if (pthread_mutex_lock(&thread_list_lock) != 0) rt_fatal("thread list lock failed");
    for (pp = &thread_list; *pp != NULL; pp = &(*pp)->next)
    {
        if (*pp == t)
        {
            *pp = t->next;
            break;
        }
    }
    if (pthread_mutex_unlock(&thread_list_lock) != 0) rt_fatal("thread list unlock failed");

    if (self_thread == t) self_thread = NULL;
    rt_heap_free(t->crt_tm);
    rt_heap_free(t->crt_errno_addr);
    rt_heap_free(t->teb);
    if (rt_va_release(t->stack_base) != 0) rt_fatal("release of a thread stack failed");
    free(t->seh);
    if (rt_va_release(H2G(t->cpu)) != 0) rt_fatal("release of a cpu state failed");
    free(t);
}

rt_thread *rt_thread_init_main(void)
{
    /* The PEB: the game reads BeingDebugged (offset 2) and ProcessHeap
       (offset 0x18) at most. A zeroed block gives "no debugger". */
    rt_peb = rt_heap_calloc(1, 0x1000);
    if (rt_peb == 0) rt_fatal("no guest memory for the PEB");
    /* SizeOfStackReserve of prototypef.exe is 1 MiB. */
    return rt_thread_create_state(0x100000, 0);
}

/* ---------------------------------------------------------------- fs segment */

uint32_t X86_ReadFsDword(uint32_t addr)
{
    rt_thread *t = rt_self();
    if (addr > TEB_SIZE - 4) rt_fatal("guest read of fs:[0x%x] is outside the TEB", addr);
    return rd32(t->teb + addr);
}

void X86_WriteFsDword(uint32_t addr, uint32_t value)
{
    rt_thread *t = rt_self();
    if (addr > TEB_SIZE - 4) rt_fatal("guest write of fs:[0x%x] is outside the TEB", addr);
    wr32(t->teb + addr, value);
}

void rt_set_last_error(uint32_t err)
{
    wr32(rt_self()->teb + TEB_LAST_ERROR, err);
}

uint32_t rt_get_last_error(void)
{
    return rd32(rt_self()->teb + TEB_LAST_ERROR);
}

/* ---------------------------------------------------------------- guest calls */

/* any_convention: accept a return with the stdcall or the cdecl stack. */
static uint32_t call_guest(_cpu *cpu, uint32_t target, bool stdcall, bool any_convention, uint32_t ecx_value, int nargs,
                           const uint32_t *args)
{
    rt_thread *t = rt_self();
    uint32_t saved_esp, expect;
    int i, d;

    if (target == 0) rt_fatal("call of guest NULL function pointer");
    if (nargs < 0 || nargs > 64) rt_fatal("rt_call with %d arguments", nargs);
    if (t->call_depth >= RT_MAX_CALL_DEPTH) rt_fatal("runtime-to-guest calls are nested more than %d levels", RT_MAX_CALL_DEPTH);
    saved_esp = esp;
    expect = stdcall ? saved_esp : saved_esp - 4u * (uint32_t)nargs;
    for (i = nargs - 1; i >= 0; i--) rt_push(cpu, args[i]);
    rt_push(cpu, target);
    ecx = ecx_value;
    eflags &= ~(uint32_t)DF;
    d = t->call_depth;
    t->call_esp[d] = esp;
    t->call_depth = d + 1;
    /* rt_transfer continues guest execution at this call level with a
       longjmp to this point. The guest registers and the target address on
       the guest stack are then already set. */
    (void)_setjmp(t->call_jmp[d]);
    c_rt_call_c2asm(cpu);
    if (t->call_depth != d + 1) rt_fatal("guest call level mismatch (%d, expected %d)", t->call_depth, d + 1);
    t->call_depth = d;
    if (esp != expect && !(any_convention && (esp == saved_esp || esp == saved_esp - 4u * (uint32_t)nargs)))
    {
        rt_fatal("guest function 0x%08x returned with esp 0x%08x, expected 0x%08x (wrong calling convention)",
                 target, esp, expect);
    }
    esp = saved_esp;
    return eax;
}

uint32_t rt_call(_cpu *cpu, uint32_t target, bool stdcall, uint32_t ecx_value, int nargs, const uint32_t *args)
{
    return call_guest(cpu, target, stdcall, false, ecx_value, nargs, args);
}

uint32_t rt_call_thread_start(_cpu *cpu, uint32_t target, uint32_t param)
{
    return call_guest(cpu, target, true, true, 0, 1, &param);
}

uint32_t rt_call_with_frame(_cpu *cpu, uint32_t target, uint32_t frame_ebp, uint32_t ecx_value)
{
    uint32_t saved_ebp = ebp, r;
    ebp = frame_ebp;
    r = rt_call(cpu, target, true, ecx_value, 0, NULL);
    ebp = saved_ebp;
    return r;
}

_Noreturn void rt_transfer(_cpu *cpu, uint32_t eip, uint32_t frame_addr)
{
    rt_thread *t = rt_self();
    int level = t->call_depth - 1;

    if (level < 0) rt_fatal("transfer of control without guest code on the thread");
    /* The deepest call level whose guest stack part holds frame_addr. */
    while (level > 0 && frame_addr > t->call_esp[level]) level--;
    esp -= 4u;
    wr32(esp, eip);
    t->call_depth = level + 1;
    rt_seh_levels_left(t, level + 1);
    _longjmp(t->call_jmp[level], 1);
}
