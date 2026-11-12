/*
 * Structured exception handling (SEH) for guest code, x86 Windows model.
 *
 * Guest code registers exception frames in the list at fs:[0]
 * (TEB ExceptionList). The runtime implements:
 *   - exception dispatch (RtlDispatchException): RaiseException and
 *     _CxxThrowException;
 *   - unwind (RtlUnwind), used by the MSVC frame handler and longjmp;
 *   - the MSVC frame handler _except_handler4_common (EH4 scope tables:
 *     __try/__except/__finally);
 *   - the top-level filter (SetUnhandledExceptionFilter,
 *     UnhandledExceptionFilter, _XcptFilter, __CppXcptFilter);
 *   - _setjmp3 and longjmp.
 * Exception records and contexts are in guest memory, on the guest stack
 * below the stack pointer of the raise, as on Windows.
 *
 * Hardware exceptions (access violations, division by zero and other
 * processor faults) are not converted to SEH exceptions: the translated
 * code does not keep an exact guest register state at each instruction.
 */
#include "rt_module.h"
#include "rt_object.h"
#include "rt_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* EXCEPTION_RECORD (x86), 80 bytes. */
#define ER_CODE 0x00
#define ER_FLAGS 0x04
#define ER_RECORD 0x08
#define ER_ADDRESS 0x0C
#define ER_NPARAMS 0x10
#define ER_INFO 0x14
#define ER_SIZE 0x50
#define ER_MAX_PARAMS 15u

/* CONTEXT (x86), 0x2CC bytes. */
#define CX_FLAGS 0x00
#define CX_FLOAT_CW 0x1C
#define CX_FLOAT_SW 0x20
#define CX_FLOAT_TW 0x24
#define CX_EDI 0x9C
#define CX_ESI 0xA0
#define CX_EBX 0xA4
#define CX_EDX 0xA8
#define CX_ECX 0xAC
#define CX_EAX 0xB0
#define CX_EBP 0xB4
#define CX_EIP 0xB8
#define CX_SEGCS 0xBC
#define CX_EFLAGS 0xC0
#define CX_ESP 0xC4
#define CX_SEGSS 0xC8
#define CX_EXT 0xCC /* FXSAVE area, 512 bytes */
#define CX_SIZE 0x2CC
#define CONTEXT_FULL_FLOAT 0x1002Fu /* control, integer, segments, floating point, extended */

#define EXCEPTION_NONCONTINUABLE 0x01u
#define EXCEPTION_UNWINDING 0x02u
#define EXCEPTION_EXIT_UNWIND 0x04u
#define EXCEPTION_STACK_INVALID 0x08u
#define EXCEPTION_NESTED_CALL 0x10u
#define EXCEPTION_UNWIND (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)

#define DISP_CONTINUE_EXECUTION 0u
#define DISP_CONTINUE_SEARCH 1u
#define DISP_NESTED 2u
#define DISP_COLLIDED 3u

#define STATUS_NONCONTINUABLE_EXCEPTION 0xC0000025u
#define STATUS_INVALID_DISPOSITION 0xC0000026u
#define STATUS_UNWIND 0xC0000027u
#define STATUS_BAD_STACK 0xC0000028u
#define STATUS_INVALID_UNWIND_TARGET 0xC0000029u
#define STATUS_INVALID_CRUNTIME_PARAMETER 0xC0000417u
#define EH_EXCEPTION_NUMBER 0xE06D7363u /* C++ exception, "msc" | 0xE0000000 */
#define EH_MAGIC_NUMBER1 0x19930520u

#define CHAIN_END 0xFFFFFFFFu

#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)

extern void rt_seh_nested_asm2c(_cpu *cpu);

static pthread_mutex_t filter_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t top_level_filter;
static uint32_t error_mode;


/* ---------------------------------------------------------------- helpers */

static uint32_t seh_head(rt_thread *t) { return rd32(t->teb + TEB_EXCEPTION_LIST); }
static void seh_set_head(rt_thread *t, uint32_t r) { wr32(t->teb + TEB_EXCEPTION_LIST, r); }

static bool frame_valid(rt_thread *t, uint32_t reg)
{
    uint32_t lo = rd32(t->teb + TEB_STACK_LIMIT), hi = rd32(t->teb + TEB_STACK_BASE);
    return (reg & 3u) == 0 && reg >= lo && reg <= hi - 8u;
}

/* Reserve n bytes on the guest stack (16-byte aligned). */
static uint32_t stack_alloc(_cpu *cpu, uint32_t n)
{
    esp = (esp - n) & ~15u;
    memset(G2H(esp), 0, n);
    return esp;
}

/* Write the current guest state into a CONTEXT. eip and the stack pointer
   are the values at the point of the raise. */
static void capture_context(_cpu *cpu, uint32_t ctx, uint32_t ctx_eip, uint32_t ctx_esp)
{
    int i;
    memset(G2H(ctx), 0, CX_SIZE);
    wr32(ctx + CX_FLAGS, CONTEXT_FULL_FLOAT);
    wr32(ctx + CX_FLOAT_CW, cpu->_st_cw | 0xFFFF0000u);
    wr32(ctx + CX_FLOAT_SW, ((cpu->_st_top & 7u) << 11) | 0xFFFF0000u);
    wr32(ctx + CX_FLOAT_TW, 0xFFFFFFFFu);
    wr32(ctx + CX_EDI, edi);
    wr32(ctx + CX_ESI, esi);
    wr32(ctx + CX_EBX, ebx);
    wr32(ctx + CX_EDX, edx);
    wr32(ctx + CX_ECX, ecx);
    wr32(ctx + CX_EAX, eax);
    wr32(ctx + CX_EBP, ebp);
    wr32(ctx + CX_EIP, ctx_eip);
    wr32(ctx + CX_SEGCS, 0x1B);
    wr32(ctx + CX_EFLAGS, eflags);
    wr32(ctx + CX_ESP, ctx_esp);
    wr32(ctx + CX_SEGSS, 0x23);
    /* FXSAVE: FCW +0, FSW +2, MXCSR +24, XMM0 +160. */
    wr16(ctx + CX_EXT + 0, (uint16_t)cpu->_st_cw);
    wr32(ctx + CX_EXT + 24, cpu->_mxcsr);
    wr32(ctx + CX_EXT + 28, 0xFFFFu);
    for (i = 0; i < 8; i++) memcpy(G2H(ctx + CX_EXT + 160u + 16u * (uint32_t)i), cpu->_xmm[i].b, 16);
}

/* Load the guest state from a CONTEXT. Returns the eip. */
static uint32_t load_context(_cpu *cpu, uint32_t ctx)
{
    uint32_t f = rd32(ctx + CX_FLAGS);
    int i;
    if ((f & 0x10002u) == 0x10002u) /* CONTEXT_INTEGER */
    {
        edi = rd32(ctx + CX_EDI);
        esi = rd32(ctx + CX_ESI);
        ebx = rd32(ctx + CX_EBX);
        edx = rd32(ctx + CX_EDX);
        ecx = rd32(ctx + CX_ECX);
        eax = rd32(ctx + CX_EAX);
    }
    if ((f & 0x10008u) == 0x10008u) cpu->_st_cw = rd32(ctx + CX_FLOAT_CW) & 0xFFFFu; /* CONTEXT_FLOATING_POINT */
    if ((f & 0x10020u) == 0x10020u) /* CONTEXT_EXTENDED_REGISTERS */
    {
        cpu->_mxcsr = rd32(ctx + CX_EXT + 24);
        for (i = 0; i < 8; i++) memcpy(cpu->_xmm[i].b, G2H(ctx + CX_EXT + 160u + 16u * (uint32_t)i), 16);
    }
    if ((f & 0x10001u) != 0x10001u) rt_fatal("continue with a CONTEXT without CONTEXT_CONTROL");
    ebp = rd32(ctx + CX_EBP);
    eflags = (rd32(ctx + CX_EFLAGS) & 0x00000CD5u) | 0x202u;
    esp = rd32(ctx + CX_ESP);
    return rd32(ctx + CX_EIP);
}

/* Per-thread dispatch state. */
typedef struct seh_state {
    int active;              /* number of dispatches in progress */
    int depth[64];           /* call depth of each dispatch */
} seh_state;

static seh_state *state_of(rt_thread *t)
{
    if (t->seh == NULL)
    {
        t->seh = calloc(1, sizeof(seh_state));
        if (t->seh == NULL) rt_fatal("out of host memory for exception state");
    }
    return t->seh;
}

void rt_seh_levels_left(rt_thread *t, int depth)
{
    seh_state *s;
    if (t->seh == NULL) return;
    s = t->seh;
    while (s->active > 0 && s->depth[s->active - 1] >= depth) s->active--;
}

/* Kind of the runtime frame around a handler call (guest memory):
   +0 Next, +4 Handler (rt_seh_nested_asm2c), +8 establisher frame,
   +12 kind. */
#define RTF_DISPATCH 1u
#define RTF_UNWIND 2u

/* Call handler(rec, reg, ctx, dc) inside a runtime frame. Returns the
   disposition. *dc_out receives the dispatcher context value. */
static uint32_t call_handler(_cpu *cpu, rt_thread *t, uint32_t handler, uint32_t rec, uint32_t reg,
                             uint32_t ctx, uint32_t kind, uint32_t *dc_out)
{
    uint32_t saved_esp = esp, frame, dc, r;

    dc = stack_alloc(cpu, 16);
    frame = stack_alloc(cpu, 16);
    wr32(frame + 0, seh_head(t));
    wr32(frame + 4, H2G((const void *)&rt_seh_nested_asm2c));
    wr32(frame + 8, reg);
    wr32(frame + 12, kind);
    seh_set_head(t, frame);
    r = rt_call(cpu, handler, false, 0, 4, (const uint32_t[]){ rec, reg, ctx, dc });
    if (seh_head(t) != frame) rt_fatal("an exception handler changed the exception list without unwinding");
    seh_set_head(t, rd32(frame + 0));
    *dc_out = rd32(dc);
    esp = saved_esp;
    return r;
}

/* Handler of the runtime frame (cdecl, 4 arguments). */
void rt_seh_nested_handler(_cpu *cpu)
{
    uint32_t rec = ARG(0), reg = ARG(1), dc = ARG(3);
    uint32_t flags = rd32(rec + ER_FLAGS);
    uint32_t kind = rd32(reg + 12);
    if (kind == RTF_DISPATCH)
    {
        if (flags & EXCEPTION_UNWIND)
        {
            RET(DISP_CONTINUE_SEARCH);
            return;
        }
        wr32(dc, rd32(reg + 8));
        RET(DISP_NESTED);
        return;
    }
    if (kind == RTF_UNWIND)
    {
        if (flags & EXCEPTION_UNWIND)
        {
            wr32(dc, rd32(reg + 8));
            RET(DISP_COLLIDED);
            return;
        }
        RET(DISP_CONTINUE_SEARCH);
        return;
    }
    rt_fatal("corrupted runtime exception frame at 0x%08x", reg);
}

static void describe(uint32_t rec, uint32_t ctx)
{
    uint32_t n = rd32(rec + ER_NPARAMS), i;
    rt_log("exception 0x%08x flags 0x%x at 0x%08x, %u parameters", rd32(rec + ER_CODE), rd32(rec + ER_FLAGS),
           rd32(rec + ER_ADDRESS), n);
    for (i = 0; i < n && i < ER_MAX_PARAMS; i++) rt_log("  parameter %u: 0x%08x", i, rd32(rec + ER_INFO + 4u * i));
    rt_log("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x eip=%08x",
           rd32(ctx + CX_EAX), rd32(ctx + CX_EBX), rd32(ctx + CX_ECX), rd32(ctx + CX_EDX), rd32(ctx + CX_ESI),
           rd32(ctx + CX_EDI), rd32(ctx + CX_EBP), rd32(ctx + CX_ESP), rd32(ctx + CX_EIP));
}

/* ---------------------------------------------------------------- unhandled exceptions */

/* UnhandledExceptionFilter for EXCEPTION_POINTERS ptrs. */
static int32_t unhandled_filter(_cpu *cpu, uint32_t ptrs)
{
    uint32_t f;
    if (pthread_mutex_lock(&filter_lock) != 0) rt_fatal("filter lock failed");
    f = top_level_filter;
    if (pthread_mutex_unlock(&filter_lock) != 0) rt_fatal("filter unlock failed");
    if (f != 0)
    {
        int32_t r = (int32_t)rt_call_std(cpu, f, 1, ptrs);
        if (r == EXCEPTION_EXECUTE_HANDLER || r == EXCEPTION_CONTINUE_EXECUTION) return r;
    }
    /* Windows shows the error report dialog here unless SEM_NOGPFAULTERRORBOX
       (0x2) is set. The runtime writes the report to the log. */
    rt_log("unhandled guest exception (error mode 0x%x):", error_mode);
    describe(rd32(ptrs + 0), rd32(ptrs + 4));
    return EXCEPTION_EXECUTE_HANDLER;
}

void rt_UnhandledExceptionFilter(_cpu *cpu)
{
    RET((uint32_t)unhandled_filter(cpu, ARG(0)));
}

void rt_SetUnhandledExceptionFilter(_cpu *cpu)
{
    uint32_t prev;
    if (pthread_mutex_lock(&filter_lock) != 0) rt_fatal("filter lock failed");
    prev = top_level_filter;
    top_level_filter = ARG(0);
    if (pthread_mutex_unlock(&filter_lock) != 0) rt_fatal("filter unlock failed");
    RET(prev);
}

void rt_SetErrorMode(_cpu *cpu)
{
    uint32_t prev;
    if (pthread_mutex_lock(&filter_lock) != 0) rt_fatal("filter lock failed");
    prev = error_mode;
    error_mode = ARG(0);
    if (pthread_mutex_unlock(&filter_lock) != 0) rt_fatal("filter unlock failed");
    RET(prev);
}

/* _XcptFilter(code, ptrs): the C runtime maps exceptions to signal
   handlers. No signal handlers can be installed (signal is not imported),
   so every action is SIG_DFL and the result is UnhandledExceptionFilter. */
void rt__XcptFilter(_cpu *cpu)
{
    RET((uint32_t)unhandled_filter(cpu, ARG(1)));
}

/* __CppXcptFilter(code, ptrs): only C++ exceptions go to _XcptFilter. */
void rt___CppXcptFilter(_cpu *cpu)
{
    if (ARG(0) == EH_EXCEPTION_NUMBER)
        RET((uint32_t)unhandled_filter(cpu, ARG(1)));
    else
        RET(EXCEPTION_CONTINUE_SEARCH);
}

/* ---------------------------------------------------------------- dispatch */

static uint32_t dispatch(_cpu *cpu, uint32_t rec, uint32_t ctx);

/* Raise a new exception from inside the dispatcher (invalid disposition,
   noncontinuable continue). Does not return. */
static _Noreturn void raise_status(_cpu *cpu, uint32_t code, uint32_t chained, uint32_t ctx)
{
    uint32_t rec = stack_alloc(cpu, ER_SIZE);
    uint32_t eip;
    wr32(rec + ER_CODE, code);
    wr32(rec + ER_FLAGS, EXCEPTION_NONCONTINUABLE);
    wr32(rec + ER_RECORD, chained);
    wr32(rec + ER_ADDRESS, rd32(ctx + CX_EIP));
    wr32(rec + ER_NPARAMS, 0);
    eip = dispatch(cpu, rec, ctx);
    /* Unreachable: a noncontinuable exception never continues. */
    rt_fatal("noncontinuable exception 0x%08x continued at 0x%08x", code, eip);
}

/* The end of the exception list: the thread start frame of kernel32
   (BaseThreadStart) calls UnhandledExceptionFilter and ends the process. */
static uint32_t end_of_chain(_cpu *cpu, uint32_t rec, uint32_t ctx)
{
    uint32_t ptrs = stack_alloc(cpu, 16);
    int32_t r;
    wr32(ptrs + 0, rec);
    wr32(ptrs + 4, ctx);
    r = unhandled_filter(cpu, ptrs);
    if (r == EXCEPTION_CONTINUE_EXECUTION)
    {
        if (rd32(rec + ER_FLAGS) & EXCEPTION_NONCONTINUABLE) raise_status(cpu, STATUS_NONCONTINUABLE_EXCEPTION, rec, ctx);
        return load_context(cpu, ctx);
    }
    rt_log("the process ends because of unhandled exception 0x%08x", rd32(rec + ER_CODE));
    rt_process_exit(rd32(rec + ER_CODE));
    rt_fatal("process exit returned");
}

/* RtlDispatchException. Returns the guest eip where execution continues
   (ExceptionContinueExecution); the guest registers are loaded from ctx.
   A handler that transfers control does not return here. */
static uint32_t dispatch(_cpu *cpu, uint32_t rec, uint32_t ctx)
{
    rt_thread *t = rt_self();
    seh_state *s = state_of(t);
    uint32_t reg = seh_head(t), nested = 0, dc, disp;

    if (s->active >= 64) rt_fatal("exceptions nested more than 64 levels");
    s->depth[s->active++] = t->call_depth;

    while (reg != CHAIN_END)
    {
        if (!frame_valid(t, reg))
        {
            wr32(rec + ER_FLAGS, rd32(rec + ER_FLAGS) | EXCEPTION_STACK_INVALID);
            break;
        }
        disp = call_handler(cpu, t, rd32(reg + 4), rec, reg, ctx, RTF_DISPATCH, &dc);
        if (nested == reg)
        {
            wr32(rec + ER_FLAGS, rd32(rec + ER_FLAGS) & ~EXCEPTION_NESTED_CALL);
            nested = 0;
        }
        switch (disp)
        {
        case DISP_CONTINUE_EXECUTION:
            if (rd32(rec + ER_FLAGS) & EXCEPTION_NONCONTINUABLE) raise_status(cpu, STATUS_NONCONTINUABLE_EXCEPTION, rec, ctx);
            s->active--;
            return load_context(cpu, ctx);
        case DISP_CONTINUE_SEARCH:
            break;
        case DISP_NESTED:
            wr32(rec + ER_FLAGS, rd32(rec + ER_FLAGS) | EXCEPTION_NESTED_CALL);
            if (dc > nested) nested = dc;
            break;
        default:
            raise_status(cpu, STATUS_INVALID_DISPOSITION, rec, ctx);
        }
        if (rd32(rec + ER_FLAGS) & EXCEPTION_STACK_INVALID) break;
        reg = rd32(reg + 0);
    }
    s->active--;
    return end_of_chain(cpu, rec, ctx);
}

/* Prepare a record and a context below the guest stack pointer and
   dispatch. ret: the return address of the raise call; args_end: the
   guest stack pointer after the return from the raise function. */
static uint32_t raise_exception(_cpu *cpu, uint32_t code, uint32_t flags, uint32_t nparams, const uint32_t *params,
                                uint32_t ret, uint32_t args_end)
{
    uint32_t ctx, rec, i;
    if (nparams > ER_MAX_PARAMS) nparams = ER_MAX_PARAMS;
    ctx = stack_alloc(cpu, CX_SIZE);
    capture_context(cpu, ctx, ret, args_end);
    rec = stack_alloc(cpu, ER_SIZE);
    wr32(rec + ER_CODE, code);
    wr32(rec + ER_FLAGS, flags & EXCEPTION_NONCONTINUABLE);
    wr32(rec + ER_RECORD, 0);
    wr32(rec + ER_ADDRESS, ret);
    wr32(rec + ER_NPARAMS, nparams);
    for (i = 0; i < nparams; i++) wr32(rec + ER_INFO + 4u * i, params[i]);
    return dispatch(cpu, rec, ctx);
}

/* RaiseException(code, flags, nparams, params), __stdcall, jump kind. */
uint32_t rt_RaiseException(_cpu *cpu)
{
    uint32_t ret = rd32(esp), code = ARG(0), flags = ARG(1), n = ARG(2), p = ARG(3);
    uint32_t params[ER_MAX_PARAMS], i;
    if (n > ER_MAX_PARAMS) n = ER_MAX_PARAMS;
    if (n > 0 && p == 0) n = 0;
    for (i = 0; i < n; i++) params[i] = rd32(p + 4u * i);
    return raise_exception(cpu, code, flags, n, params, ret, esp + 4u + 16u);
}

/* _CxxThrowException(object, throw_info), __stdcall, jump kind. */
uint32_t rt__CxxThrowException(_cpu *cpu)
{
    uint32_t ret = rd32(esp);
    uint32_t params[3] = { EH_MAGIC_NUMBER1, ARG(0), ARG(1) };
    return raise_exception(cpu, EH_EXCEPTION_NUMBER, EXCEPTION_NONCONTINUABLE, 3, params, ret, esp + 4u + 8u);
}

/* ---------------------------------------------------------------- unwind */

/* RtlUnwind(target, ..., rec, return value) for the runtime. Calls the
   handlers of all frames above target with an unwind flag and removes them
   from the list. target CHAIN_END unwinds all frames; target 0 is an exit
   unwind of all frames. */
static void unwind(_cpu *cpu, uint32_t target, uint32_t rec_in)
{
    rt_thread *t = rt_self();
    uint32_t saved_esp = esp, rec = rec_in, ctx, reg, dc, disp;

    ctx = stack_alloc(cpu, CX_SIZE);
    capture_context(cpu, ctx, 0, saved_esp);
    if (rec == 0)
    {
        rec = stack_alloc(cpu, ER_SIZE);
        wr32(rec + ER_CODE, STATUS_UNWIND);
    }
    wr32(rec + ER_FLAGS, rd32(rec + ER_FLAGS) | EXCEPTION_UNWINDING | (target == 0 ? EXCEPTION_EXIT_UNWIND : 0u));

    reg = seh_head(t);
    while (reg != CHAIN_END && reg != target)
    {
        if (target != 0 && target < reg) raise_status(cpu, STATUS_INVALID_UNWIND_TARGET, rec, ctx);
        if (!frame_valid(t, reg)) raise_status(cpu, STATUS_BAD_STACK, rec, ctx);
        disp = call_handler(cpu, t, rd32(reg + 4), rec, reg, ctx, RTF_UNWIND, &dc);
        if (disp == DISP_COLLIDED)
        {
            reg = dc;
        }
        else if (disp != DISP_CONTINUE_SEARCH)
        {
            raise_status(cpu, STATUS_INVALID_DISPOSITION, rec, ctx);
        }
        reg = rd32(reg + 0);
        seh_set_head(t, reg);
    }
    if (target != 0 && target != CHAIN_END && reg != target) rt_fatal("unwind target frame 0x%08x is not in the exception list", target);
    esp = saved_esp;
}

/* ---------------------------------------------------------------- EH4 frame handler */

/* EH4 registration node (guest), at EstablisherFrame:
     -8  saved esp           -4  EXCEPTION_POINTERS*
     +0  Next                +4  Handler
     +8  scope table xor __security_cookie
     +12 try level
   The frame pointer (ebp of the function) is EstablisherFrame + 16.
   Scope table: +0 GSCookieOffset, +4 GSCookieXOROffset, +8 EHCookieOffset,
   +12 EHCookieXOROffset, +16 records of 12 bytes:
   EnclosingLevel, FilterFunc (0 for __finally), HandlerFunc. */
#define TOPMOST_TRY_LEVEL 0xFFFFFFFEu
#define NO_GS_COOKIE 0xFFFFFFFEu

static void check_cookies(_cpu *cpu, uint32_t check_fn, uint32_t scope, uint32_t fp)
{
    uint32_t gs_off = rd32(scope + 0), cookie;
    if (gs_off != NO_GS_COOKIE)
    {
        cookie = rd32(fp + gs_off) ^ (fp + rd32(scope + 4));
        rt_call(cpu, check_fn, true, cookie, 0, NULL); /* __fastcall, argument in ecx */
    }
    cookie = rd32(fp + rd32(scope + 8)) ^ (fp + rd32(scope + 12));
    rt_call(cpu, check_fn, true, cookie, 0, NULL);
}

static void local_unwind4(_cpu *cpu, uint32_t reg, uint32_t scope, uint32_t target, uint32_t fp)
{
    uint32_t level;
    while ((level = rd32(reg + 12)) != target && level != TOPMOST_TRY_LEVEL)
    {
        uint32_t r = scope + 16u + 12u * level;
        wr32(reg + 12, rd32(r + 0));
        if (rd32(r + 4) == 0) rt_call_with_frame(cpu, rd32(r + 8), fp, 0); /* __finally block */
    }
}

/* Destroy a C++ exception object before an __except block runs (the
   C runtime does this with __DestructExceptionObject). ThrowInfo:
   +0 attributes, +4 destructor (__thiscall). */
static void destruct_exception_object(_cpu *cpu, uint32_t rec)
{
    uint32_t obj, info, dtor;
    if (rd32(rec + ER_CODE) != EH_EXCEPTION_NUMBER || rd32(rec + ER_NPARAMS) != 3u) return;
    if (rd32(rec + ER_INFO) != EH_MAGIC_NUMBER1) return;
    obj = rd32(rec + ER_INFO + 4);
    info = rd32(rec + ER_INFO + 8);
    if (info == 0) return;
    dtor = rd32(info + 4);
    if (dtor != 0) rt_call(cpu, dtor, true, obj, 0, NULL);
}

/* _except_handler4_common(cookie_ptr, cookie_check, rec, reg, ctx, dc),
   __cdecl. */
void rt__except_handler4_common(_cpu *cpu)
{
    uint32_t cookie_ptr = ARG(0), check_fn = ARG(1), rec = ARG(2), reg = ARG(3), ctx = ARG(4);
    uint32_t saved_esp = esp, scope, fp, level, ptrs, result = DISP_CONTINUE_SEARCH;

    scope = rd32(reg + 8) ^ rd32(cookie_ptr);
    fp = reg + 16u;
    check_cookies(cpu, check_fn, scope, fp);

    if ((rd32(rec + ER_FLAGS) & EXCEPTION_UNWIND) == 0)
    {
        ptrs = stack_alloc(cpu, 16);
        wr32(ptrs + 0, rec);
        wr32(ptrs + 4, ctx);
        wr32(reg - 4u, ptrs);
        for (level = rd32(reg + 12); level != TOPMOST_TRY_LEVEL;)
        {
            uint32_t r = scope + 16u + 12u * level;
            uint32_t enclosing = rd32(r + 0), filter = rd32(r + 4);
            if (filter != 0)
            {
                int32_t fr = (int32_t)rt_call_with_frame(cpu, filter, fp, 0);
                if (fr < 0)
                {
                    result = DISP_CONTINUE_EXECUTION;
                    break;
                }
                if (fr > 0)
                {
                    uint32_t handler = rd32(r + 8);
                    destruct_exception_object(cpu, rec);
                    unwind(cpu, reg, rec);
                    if (rd32(reg + 12) != level) local_unwind4(cpu, reg, scope, level, fp);
                    wr32(reg + 12, enclosing);
                    check_cookies(cpu, check_fn, scope, fp);
                    /* _EH4_TransferToHandler. The __except block loads esp
                       from the frame. */
                    ebp = fp;
                    esi = eax = handler;
                    ebx = ecx = edx = edi = 0;
                    eflags &= ~(uint32_t)DF;
                    rt_transfer(cpu, handler, reg);
                }
            }
            level = enclosing;
        }
    }
    else if (rd32(reg + 12) != TOPMOST_TRY_LEVEL)
    {
        local_unwind4(cpu, reg, scope, TOPMOST_TRY_LEVEL, fp);
        check_cookies(cpu, check_fn, scope, fp);
    }
    esp = saved_esp;
    RET(result);
}

/* ---------------------------------------------------------------- setjmp / longjmp */

/* _JUMP_BUFFER (x86): Ebp, Ebx, Edi, Esi, Esp, Eip, Registration, TryLevel,
   Cookie, UnwindFunc, UnwindData[6]. */
#define JB_COOKIE 0x56433230u /* "VC20" */

/* _setjmp3(buf, count, ...), __cdecl. */
void rt__setjmp3(_cpu *cpu)
{
    rt_thread *t = rt_self();
    uint32_t jb = ARG(0), count = ARG(1), reg = seh_head(t), i;

    wr32(jb + 0, ebp);
    wr32(jb + 4, ebx);
    wr32(jb + 8, edi);
    wr32(jb + 12, esi);
    wr32(jb + 16, esp); /* points to the return address, as in the C runtime */
    wr32(jb + 20, rd32(esp)); /* the return address */
    wr32(jb + 24, reg);
    wr32(jb + 32, JB_COOKIE);
    wr32(jb + 36, 0);
    if (reg == CHAIN_END)
    {
        wr32(jb + 28, 0xFFFFFFFFu);
    }
    else if (count == 0)
    {
        wr32(jb + 28, rd32(reg + 12));
    }
    else
    {
        wr32(jb + 36, ARG(2));
        wr32(jb + 28, count > 1 ? ARG(3) : rd32(reg + 12));
        for (i = 2; i < count && i < 8u; i++) wr32(jb + 40u + 4u * (i - 2u), ARG(2u + i));
    }
    RET(0);
}

/* longjmp(buf, value), __cdecl, jump kind. */
uint32_t rt_longjmp(_cpu *cpu)
{
    rt_thread *t = rt_self();
    uint32_t jb = ARG(0), value = ARG(1), reg, target_esp;

    if (value == 0) value = 1;
    reg = rd32(jb + 24);
    target_esp = rd32(jb + 16) + 4u;
    if (reg != seh_head(t)) unwind(cpu, reg, 0);
    if (reg != 0 && rd32(jb + 32) == JB_COOKIE)
    {
        uint32_t fn = rd32(jb + 36);
        if (fn != 0) rt_call_std(cpu, fn, 1, jb);
    }
    else if (reg != 0)
    {
        rt_fatal("longjmp with a jump buffer without the VC20 cookie (local unwind of an old frame type)");
    }
    ebp = rd32(jb + 0);
    ebx = rd32(jb + 4);
    edi = rd32(jb + 8);
    esi = rd32(jb + 12);
    esp = target_esp;
    eax = value;
    eflags &= ~(uint32_t)DF;
    /* The target can be at an outer call level. */
    rt_transfer(cpu, rd32(jb + 20), target_esp);
}

/* ---------------------------------------------------------------- C runtime termination */

static void crt_message(const char *msg)
{
    rt_log("%s", msg);
}

void rt_abort(_cpu *cpu)
{
    (void)cpu;
    crt_message("R6010 - abort() has been called");
    rt_process_exit(3);
}

/* terminate(): set_terminate is not imported, so the handler is the
   default handler, abort(). */
void rt_terminate(_cpu *cpu)
{
    rt_abort(cpu);
}

void rt__amsg_exit(_cpu *cpu)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "runtime error R60%02u", ARG(0) % 100u);
    crt_message(buf);
    rt_process_exit(255);
}

static pthread_mutex_t purecall_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t purecall_handler;

void rt__set_purecall_handler(_cpu *cpu)
{
    uint32_t prev;
    if (pthread_mutex_lock(&purecall_lock) != 0) rt_fatal("lock failed");
    prev = purecall_handler;
    purecall_handler = ARG(0);
    if (pthread_mutex_unlock(&purecall_lock) != 0) rt_fatal("unlock failed");
    RET(prev);
}

void rt__purecall(_cpu *cpu)
{
    uint32_t h;
    if (pthread_mutex_lock(&purecall_lock) != 0) rt_fatal("lock failed");
    h = purecall_handler;
    if (pthread_mutex_unlock(&purecall_lock) != 0) rt_fatal("unlock failed");
    if (h != 0) rt_call_cdecl0(cpu, h);
    crt_message("R6025 - pure virtual function call");
    rt_process_exit(255);
}

void rt__invoke_watson(_cpu *cpu)
{
    (void)cpu;
    crt_message("invalid parameter passed to a C runtime function (_invoke_watson)");
    rt_process_exit(STATUS_INVALID_CRUNTIME_PARAMETER);
}

/* A hook for debuggers; it has no effect without a debugger. */
void rt__crt_debugger_hook(_cpu *cpu)
{
    (void)cpu;
}

void rt_IsDebuggerPresent(_cpu *cpu)
{
    RET(0);
}
