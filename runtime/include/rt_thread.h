/* Guest threads, thread environment blocks (TEB), fibers and calls into
   guest code. */
#ifndef RT_THREAD_H
#define RT_THREAD_H

#include "rt.h"

#include <pthread.h>
#include <setjmp.h>
#include <stdbool.h>

/* TEB field offsets used by MSVC code (fs:[offset]). */
#define TEB_EXCEPTION_LIST 0x00
#define TEB_STACK_BASE 0x04
#define TEB_STACK_LIMIT 0x08
#define TEB_FIBER_DATA 0x10
#define TEB_SELF 0x18
#define TEB_PROCESS_ID 0x20
#define TEB_THREAD_ID 0x24
#define TEB_TLS_POINTER 0x2C
#define TEB_PEB 0x30
#define TEB_LAST_ERROR 0x34
#define TEB_TLS_SLOTS 0xE10
#define TEB_SIZE 0x1000
#define TLS_SLOT_COUNT 64

/* Maximum nesting depth of runtime-to-guest calls on one thread. */
#define RT_MAX_CALL_DEPTH 256

struct rt_object;
struct rt_fiber;

typedef struct rt_thread {
    _cpu *cpu;
    uint32_t teb;           /* guest address of the TEB */
    uint32_t tid;
    struct rt_object *obj;  /* thread object (NULL for the main thread) */
    uint32_t stack_base;    /* guest stack allocation base */
    uint32_t stack_size;
    int call_depth;         /* nesting depth of runtime-to-guest calls */
    /* Guest esp at the start of each nested runtime-to-guest call. The
       exception code uses it to find frames that are outside the current
       native call level. */
    uint32_t call_esp[RT_MAX_CALL_DEPTH];
    jmp_buf call_jmp[RT_MAX_CALL_DEPTH];
    struct rt_fiber *fiber; /* current fiber, NULL if the thread is not a fiber */
    pthread_t pt;
    jmp_buf exit_jmp;       /* ExitThread target */
    bool exit_jmp_set;
    uint32_t exit_code;
    struct rt_thread *next; /* list of all threads */
    /* Per-thread C runtime state. */
    uint32_t crt_errno_addr; /* guest address of errno */
    uint32_t crt_strtok;     /* strtok position (guest address) */
    uint32_t crt_rand;       /* rand state */
    uint32_t crt_tm;         /* guest buffer for gmtime/localtime */
    void *seh;               /* exception dispatch state (rt_seh.c) */
} rt_thread;

/* The calling thread. Every thread that runs guest code has one. */
rt_thread *rt_self(void);
/* The calling thread or NULL. */
rt_thread *rt_self_or_null(void);

/* Create the state for the calling (main) thread. */
rt_thread *rt_thread_init_main(void);

/* Create the guest state for the calling native thread. tid 0 allocates a
   new thread id. */
rt_thread *rt_thread_create_state(uint32_t stack_size, uint32_t tid);
void rt_thread_destroy_state(rt_thread *t);
uint32_t rt_thread_alloc_tid(void);

/* Call fn(t) for each thread, with the thread list lock held. */
void rt_thread_foreach(void (*fn)(rt_thread *t, void *ctx), void *ctx);

/* The process environment block (PEB), guest address. */
extern uint32_t rt_peb;

/* Call the guest function at address target with nargs 32-bit arguments.
   stdcall: the callee removes the arguments; otherwise the runtime removes
   them. Returns eax. ecx_value is loaded into ecx (for __thiscall). */
uint32_t rt_call(_cpu *cpu, uint32_t target, bool stdcall, uint32_t ecx_value, int nargs, const uint32_t *args);
/* Call a thread start routine with its parameter. Windows ends the thread
   after the return, so a start routine with the cdecl convention (one
   that leaves its parameter on the stack) also works. */
uint32_t rt_call_thread_start(_cpu *cpu, uint32_t target, uint32_t param);
#define rt_call_std(cpu, target, n, ...) rt_call((cpu), (target), true, 0, (n), (const uint32_t[]){__VA_ARGS__})
#define rt_call_cdecl(cpu, target, n, ...) rt_call((cpu), (target), false, 0, (n), (const uint32_t[]){__VA_ARGS__})
#define rt_call_this(cpu, target, this_, n, ...) rt_call((cpu), (target), true, (this_), (n), (const uint32_t[]){__VA_ARGS__})
#define rt_call_std0(cpu, target) rt_call((cpu), (target), true, 0, 0, NULL)
#define rt_call_cdecl0(cpu, target) rt_call((cpu), (target), false, 0, 0, NULL)

/* Call a guest function without arguments with ebp set to frame_ebp
   (exception filters, __finally blocks). ebp is restored after the call. */
uint32_t rt_call_with_frame(_cpu *cpu, uint32_t target, uint32_t frame_ebp, uint32_t ecx_value);

/* Continue guest execution at eip at the call level that holds the guest
   stack address frame_addr. The caller sets all other guest registers
   (esp included). Native frames of deeper call levels are removed; runtime
   functions must not hold locks or host memory across rt_call. */
_Noreturn void rt_transfer(_cpu *cpu, uint32_t eip, uint32_t frame_addr);

/* Exception code: call levels at and above depth were removed. */
void rt_seh_levels_left(rt_thread *t, int depth);

/* Push a 32-bit value on the guest stack. */
static inline void rt_push(_cpu *cpu, uint32_t v)
{
    esp -= 4;
    wr32(esp, v);
}

uint32_t rt_current_process_id(void);

/* End the calling thread (ExitThread). Does not return. */
_Noreturn void rt_thread_exit(uint32_t code);

/* Module notifications for thread start and end (rt_module.c). */
void rt_module_thread_attach(_cpu *cpu);
void rt_module_thread_detach(_cpu *cpu);

#endif
