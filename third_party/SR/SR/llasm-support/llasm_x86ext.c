/**
 *  x86 instruction helpers for the SRW llasm backend: SSE/SSE2, locked
 *  (atomic) integer instructions, and x87 instructions that upstream SR
 *  does not support. Written for the macOS ARM64 port of Prototype.
 *
 *  Calling convention: SRW emits "call <helper> $, args..." with every
 *  argument as a 32-bit integer. XMM operands are register numbers 0..7.
 *  Memory operands are guest addresses. llasm saves all guest registers
 *  to the _cpu structure before each call and loads them again after the
 *  call, so these helpers read and write guest registers and EFLAGS in
 *  the _cpu structure directly.
 *
 *  This file is released under the same MIT license as the SR project.
 */

#include "llasm_cpu.h"

#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(__aarch64__)
#include <arm_acle.h>
#endif
#include <os/lock.h>

#define XMM(n) (cpu->_xmm[(n) & 7])

#define FLAGS_ARITH (CF | PF | AF | ZF | SF | OF)

/* ------------------------------------------------------------------ */
/* guest memory access                                                 */
/* ------------------------------------------------------------------ */

static inline uint32_t rd32(CPU, uint32_t adr)
{
    uint32_t v;
    memcpy(&v, REG2PTR(adr), 4);
    return v;
}

static inline uint64_t rd64(CPU, uint32_t adr)
{
    uint64_t v;
    memcpy(&v, REG2PTR(adr), 8);
    return v;
}

static inline void wr32(CPU, uint32_t adr, uint32_t v)
{
    memcpy(REG2PTR(adr), &v, 4);
}

static inline void wr64(CPU, uint32_t adr, uint64_t v)
{
    memcpy(REG2PTR(adr), &v, 8);
}

static inline void rd128(CPU, uint32_t adr, void *dst)
{
    memcpy(dst, REG2PTR(adr), 16);
}

static inline void wr128(CPU, uint32_t adr, const void *src)
{
    memcpy(REG2PTR(adr), src, 16);
}

static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline double q2d(uint64_t q) { double d; memcpy(&d, &q, 8); return d; }

/* ------------------------------------------------------------------ */
/* guest general-purpose registers by x86 encoding                     */
/* ------------------------------------------------------------------ */

static inline uint32_t *gpr32(CPU, uint32_t code)
{
    switch (code & 7)
    {
        case 0: return &cpu->_eax;
        case 1: return &cpu->_ecx;
        case 2: return &cpu->_edx;
        case 3: return &cpu->_ebx;
        case 4: return &cpu->_esp;
        case 5: return &cpu->_ebp;
        case 6: return &cpu->_esi;
        default: return &cpu->_edi;
    }
}

/* size 8: codes 0..3 are AL..BL, codes 4..7 are AH..BH */
static inline uint32_t get_gpr(CPU, uint32_t code, uint32_t size)
{
    if (size == 8)
    {
        if (code & 4) return (*gpr32(cpu, code & 3) >> 8) & 0xff;
        return *gpr32(cpu, code) & 0xff;
    }
    if (size == 16) return *gpr32(cpu, code) & 0xffff;
    return *gpr32(cpu, code);
}

static inline void set_gpr(CPU, uint32_t code, uint32_t size, uint32_t value)
{
    uint32_t *r;

    if (size == 8)
    {
        if (code & 4)
        {
            r = gpr32(cpu, code & 3);
            *r = (*r & 0xffff00ffu) | ((value & 0xff) << 8);
        }
        else
        {
            r = gpr32(cpu, code);
            *r = (*r & 0xffffff00u) | (value & 0xff);
        }
    }
    else if (size == 16)
    {
        r = gpr32(cpu, code);
        *r = (*r & 0xffff0000u) | (value & 0xffff);
    }
    else
    {
        *gpr32(cpu, code) = value;
    }
}

/* ------------------------------------------------------------------ */
/* integer flags                                                       */
/* ------------------------------------------------------------------ */

static inline uint32_t size_mask(uint32_t size)
{
    return (size == 8) ? 0xffu : ((size == 16) ? 0xffffu : 0xffffffffu);
}

static inline uint32_t size_sign(uint32_t size)
{
    return (size == 8) ? 0x80u : ((size == 16) ? 0x8000u : 0x80000000u);
}

static inline uint32_t flags_pzs(uint32_t res, uint32_t size)
{
    uint32_t f = 0;

    res &= size_mask(size);
    if (res == 0) f |= ZF;
    if (res & size_sign(size)) f |= SF;
    if (!(__builtin_popcount(res & 0xff) & 1)) f |= PF;
    return f;
}

static inline uint32_t flags_add(uint32_t a, uint32_t b, uint32_t carry_in, uint32_t size)
{
    uint32_t m = size_mask(size), s = size_sign(size);
    uint64_t full = (uint64_t)(a & m) + (uint64_t)(b & m) + carry_in;
    uint32_t res = (uint32_t)full & m;
    uint32_t f = flags_pzs(res, size);

    if (full > m) f |= CF;
    if (((a ^ res) & (b ^ res)) & s) f |= OF;
    if ((a ^ b ^ res) & 0x10) f |= AF;
    return f;
}

static inline uint32_t flags_sub(uint32_t a, uint32_t b, uint32_t borrow_in, uint32_t size)
{
    uint32_t m = size_mask(size), s = size_sign(size);
    uint32_t res = (a - b - borrow_in) & m;
    uint32_t f = flags_pzs(res, size);

    if ((uint64_t)(a & m) < (uint64_t)(b & m) + borrow_in) f |= CF;
    if (((a ^ b) & (a ^ res)) & s) f |= OF;
    if ((a ^ b ^ res) & 0x10) f |= AF;
    return f;
}

static inline void set_arith_flags(CPU, uint32_t f, uint32_t mask)
{
    cpu->_eflags = (cpu->_eflags & ~mask) | (f & mask);
}

/* x86 condition codes: o no b ae z nz be a s ns p np l ge le g */
static inline int cond_eval(uint32_t f, uint32_t cc)
{
    int r;

    switch ((cc >> 1) & 7)
    {
        case 0: r = (f & OF) != 0; break;
        case 1: r = (f & CF) != 0; break;
        case 2: r = (f & ZF) != 0; break;
        case 3: r = (f & (CF | ZF)) != 0; break;
        case 4: r = (f & SF) != 0; break;
        case 5: r = (f & PF) != 0; break;
        case 6: r = ((f & SF) != 0) != ((f & OF) != 0); break;
        default: r = ((f & ZF) != 0) || (((f & SF) != 0) != ((f & OF) != 0)); break;
    }
    return (cc & 1) ? !r : r;
}

/* cmovcc reg, value (value was read from the source operand by SRW) */
EXTERNC void CCALL x86_cmov(CPU, uint32_t cc, uint32_t size, uint32_t regcode, uint32_t value)
{
    if (cond_eval(cpu->_eflags, cc)) set_gpr(cpu, regcode, size, value);
}

EXTERNC void CCALL x86_setcc_r(CPU, uint32_t cc, uint32_t regcode)
{
    set_gpr(cpu, regcode, 8, (uint32_t)cond_eval(cpu->_eflags, cc));
}

EXTERNC void CCALL x86_setcc_m(CPU, uint32_t cc, uint32_t adr)
{
    *(uint8_t *)REG2PTR(adr) = (uint8_t)cond_eval(cpu->_eflags, cc);
}

/* ------------------------------------------------------------------ */
/* locked integer instructions                                         */
/* ------------------------------------------------------------------ */

enum {
    LOCK_ADD = 0, LOCK_OR = 1, LOCK_ADC = 2, LOCK_SBB = 3,
    LOCK_AND = 4, LOCK_SUB = 5, LOCK_XOR = 6,
    LOCK_INC = 8, LOCK_DEC = 9, LOCK_NOT = 10, LOCK_NEG = 11,
    LOCK_XADD = 12, LOCK_XCHG = 13,
    LOCK_BTS = 14, LOCK_BTR = 15, LOCK_BTC = 16
};

/* ARMv8.4 LSE2 (all Apple silicon) permits unaligned atomics inside one
   16-byte granule. An atomic access across a granule boundary faults. x86
   permits a locked access at any address (a "split lock").

   A locked access that crosses a granule boundary runs under a lock from
   split_locks. The lock is selected by the boundary that the access
   crosses. Two crossing accesses of 8 bytes or less that overlap always
   cross the same boundary, so they always use the same lock and are
   atomic with respect to each other.

   Limit: such an access is not atomic with respect to a locked access that
   does not cross the boundary, or a plain store, on the same bytes at the
   same time. x86 makes these atomic. UNCONFIRMED: the game does not do
   this. The engine thread at loc_10B82416 uses only cmpxchg8b on its
   crossing 64-bit field. */
#define SPLIT_LOCKS 64u

static os_unfair_lock split_locks[SPLIT_LOCKS]; /* all zero = OS_UNFAIR_LOCK_INIT */

static inline int lock_crosses(uint32_t adr, uint32_t size)
{
    return ((adr & 15) + (size >> 3)) > 16;
}

static os_unfair_lock *split_lock_enter(uint32_t adr, uint32_t size)
{
    uint32_t b = (adr + (size >> 3) - 1) >> 4;
    os_unfair_lock *l = &split_locks[(b ^ (b >> 6)) % SPLIT_LOCKS];
    os_unfair_lock_lock(l);
    /* A locked x86 instruction is a full memory barrier. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return l;
}

static void split_lock_leave(os_unfair_lock *l)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    os_unfair_lock_unlock(l);
}

static inline uint32_t rmw_value(uint32_t op, uint32_t size, uint32_t o, uint32_t src, uint32_t c)
{
    switch (op)
    {
        case LOCK_ADD: return o + src;
        case LOCK_OR: return o | src;
        case LOCK_ADC: return o + src + c;
        case LOCK_SBB: return o - src - c;
        case LOCK_AND: return o & src;
        case LOCK_SUB: return o - src;
        case LOCK_XOR: return o ^ src;
        case LOCK_INC: return o + 1;
        case LOCK_DEC: return o - 1;
        case LOCK_NOT: return ~o;
        case LOCK_NEG: return 0 - o;
        case LOCK_XADD: return o + src;
        case LOCK_XCHG: return src;
        case LOCK_BTS: return o | (1u << (src & (size - 1)));
        case LOCK_BTR: return o & ~(1u << (src & (size - 1)));
        default: return o ^ (1u << (src & (size - 1)));
    }
}

static uint32_t atomic_rmw(CPU, uint32_t op, uint32_t size, uint32_t adr, uint32_t src, uint32_t *pnew)
{
    void *p = REG2PTR(adr);
    uint32_t oldv, newv, m = size_mask(size);

    if (lock_crosses(adr, size))
    {
        os_unfair_lock *l = split_lock_enter(adr, size);
        oldv = 0;
        memcpy(&oldv, p, size >> 3);
        newv = rmw_value(op, size, oldv, src, cpu->_eflags & CF) & m;
        memcpy(p, &newv, size >> 3);
        split_lock_leave(l);
        *pnew = newv;
        return oldv & m;
    }

#define RMW_LOOP(T) { \
        T o = __atomic_load_n((T *)p, __ATOMIC_RELAXED), n; \
        do { \
            uint32_t c = cpu->_eflags & CF; \
            switch (op) { \
                case LOCK_ADD: n = (T)(o + src); break; \
                case LOCK_OR:  n = (T)(o | src); break; \
                case LOCK_ADC: n = (T)(o + src + c); break; \
                case LOCK_SBB: n = (T)(o - src - c); break; \
                case LOCK_AND: n = (T)(o & src); break; \
                case LOCK_SUB: n = (T)(o - src); break; \
                case LOCK_XOR: n = (T)(o ^ src); break; \
                case LOCK_INC: n = (T)(o + 1); break; \
                case LOCK_DEC: n = (T)(o - 1); break; \
                case LOCK_NOT: n = (T)(~o); break; \
                case LOCK_NEG: n = (T)(0 - o); break; \
                case LOCK_XADD: n = (T)(o + src); break; \
                case LOCK_XCHG: n = (T)src; break; \
                case LOCK_BTS: n = (T)(o | ((T)1 << (src & (size - 1)))); break; \
                case LOCK_BTR: n = (T)(o & ~((T)1 << (src & (size - 1)))); break; \
                default: n = (T)(o ^ ((T)1 << (src & (size - 1)))); break; \
            } \
        } while (!__atomic_compare_exchange_n((T *)p, &o, n, 1, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)); \
        oldv = o; newv = n; \
    }

    if (size == 8) RMW_LOOP(uint8_t)
    else if (size == 16) RMW_LOOP(uint16_t)
    else RMW_LOOP(uint32_t)

#undef RMW_LOOP

    *pnew = newv & m;
    return oldv & m;
}

/* lock <op> [adr], src  (add/or/adc/sbb/and/sub/xor/inc/dec/not/neg/bts/btr/btc) */
EXTERNC void CCALL x86_lock_op(CPU, uint32_t op, uint32_t size, uint32_t adr, uint32_t src)
{
    uint32_t oldv, newv, c;

    c = cpu->_eflags & CF;
    src &= size_mask(size);
    oldv = atomic_rmw(cpu, op, size, adr, src, &newv);

    switch (op)
    {
        case LOCK_ADD: set_arith_flags(cpu, flags_add(oldv, src, 0, size), FLAGS_ARITH); break;
        case LOCK_ADC: set_arith_flags(cpu, flags_add(oldv, src, c, size), FLAGS_ARITH); break;
        case LOCK_SUB: set_arith_flags(cpu, flags_sub(oldv, src, 0, size), FLAGS_ARITH); break;
        case LOCK_SBB: set_arith_flags(cpu, flags_sub(oldv, src, c, size), FLAGS_ARITH); break;
        case LOCK_OR:
        case LOCK_AND:
        case LOCK_XOR: set_arith_flags(cpu, flags_pzs(newv, size), FLAGS_ARITH); break;
        case LOCK_INC: set_arith_flags(cpu, flags_add(oldv, 1, 0, size), FLAGS_ARITH & ~CF); break;
        case LOCK_DEC: set_arith_flags(cpu, flags_sub(oldv, 1, 0, size), FLAGS_ARITH & ~CF); break;
        case LOCK_NEG: set_arith_flags(cpu, flags_sub(0, oldv, 0, size) | (oldv ? CF : 0), FLAGS_ARITH); break;
        case LOCK_NOT: break;
        default: /* bts/btr/btc: CF = old bit, other flags undefined (left unchanged) */
            set_arith_flags(cpu, ((oldv >> (src & (size - 1))) & 1) ? CF : 0, CF);
            break;
    }
}

/* lock bts/btr/btc [adr], reg: the register is a signed bit offset that
   can address memory outside the operand at adr */
EXTERNC void CCALL x86_lock_bt_reg(CPU, uint32_t op, uint32_t size, uint32_t adr, uint32_t bitofs)
{
    int32_t ofs = (int32_t)bitofs;
    int32_t shift = (size == 16) ? 4 : 5;

    if (size == 16) ofs = (int16_t)bitofs;
    adr += (uint32_t)((ofs >> shift) * (int32_t)(size >> 3));
    x86_lock_op(cpu, op, size, adr, (uint32_t)ofs & (size - 1));
}

/* xadd [adr], reg  (reg = old value, [adr] = old + reg) */
EXTERNC void CCALL x86_lock_xadd(CPU, uint32_t size, uint32_t adr, uint32_t regcode)
{
    uint32_t src, oldv, newv;

    src = get_gpr(cpu, regcode, size);
    oldv = atomic_rmw(cpu, LOCK_XADD, size, adr, src, &newv);
    set_gpr(cpu, regcode, size, oldv);
    set_arith_flags(cpu, flags_add(oldv, src, 0, size), FLAGS_ARITH);
}

/* xchg [adr], reg  (always locked on x86) */
EXTERNC void CCALL x86_lock_xchg(CPU, uint32_t size, uint32_t adr, uint32_t regcode)
{
    uint32_t src, oldv, newv;

    src = get_gpr(cpu, regcode, size);
    oldv = atomic_rmw(cpu, LOCK_XCHG, size, adr, src, &newv);
    set_gpr(cpu, regcode, size, oldv);
}

/* cmpxchg [adr], reg */
EXTERNC void CCALL x86_lock_cmpxchg(CPU, uint32_t size, uint32_t adr, uint32_t regcode)
{
    void *p = REG2PTR(adr);
    uint32_t acc, src, oldv;
    int ok;

    acc = get_gpr(cpu, 0, size);
    src = get_gpr(cpu, regcode, size);

    if (lock_crosses(adr, size))
    {
        os_unfair_lock *l = split_lock_enter(adr, size);
        oldv = 0;
        memcpy(&oldv, p, size >> 3);
        ok = oldv == acc;
        if (ok) memcpy(p, &src, size >> 3);
        split_lock_leave(l);
    }
    else if (size == 8)
    {
        uint8_t e = (uint8_t)acc;
        ok = __atomic_compare_exchange_n((uint8_t *)p, &e, (uint8_t)src, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        oldv = e;
    }
    else if (size == 16)
    {
        uint16_t e = (uint16_t)acc;
        ok = __atomic_compare_exchange_n((uint16_t *)p, &e, (uint16_t)src, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        oldv = e;
    }
    else
    {
        uint32_t e = acc;
        ok = __atomic_compare_exchange_n((uint32_t *)p, &e, src, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        oldv = e;
    }

    set_arith_flags(cpu, flags_sub(acc, oldv, 0, size), FLAGS_ARITH);
    if (!ok) set_gpr(cpu, 0, size, oldv);
}

/* cmpxchg8b [adr] */
EXTERNC void CCALL x86_lock_cmpxchg8b(CPU, uint32_t adr)
{
    uint64_t *p = (uint64_t *)REG2PTR(adr);
    uint64_t e, n;

    int ok;

    e = ((uint64_t)cpu->_edx << 32) | cpu->_eax;
    n = ((uint64_t)cpu->_ecx << 32) | cpu->_ebx;

    if (lock_crosses(adr, 64))
    {
        os_unfair_lock *l = split_lock_enter(adr, 64);
        uint64_t cur;
        memcpy(&cur, p, 8);
        ok = cur == e;
        if (ok)
            memcpy(p, &n, 8);
        else
            e = cur;
        split_lock_leave(l);
    }
    else
        ok = __atomic_compare_exchange_n(p, &e, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);

    if (ok)
    {
        cpu->_eflags |= ZF;
    }
    else
    {
        cpu->_eflags &= ~ZF;
        cpu->_eax = (uint32_t)e;
        cpu->_edx = (uint32_t)(e >> 32);
    }
}

/* ------------------------------------------------------------------ */
/* string compare / scan with 16-bit and 32-bit elements               */
/* rep: 0 = none, 1 = repe/rep, 2 = repne                              */
/* ------------------------------------------------------------------ */

static inline uint32_t rd_elem(CPU, uint32_t adr, uint32_t size)
{
    if (size == 16)
    {
        uint16_t v;
        memcpy(&v, REG2PTR(adr), 2);
        return v;
    }
    return rd32(cpu, adr);
}

EXTERNC void CCALL x86_cmps(CPU, uint32_t size, uint32_t rep)
{
    uint32_t step = (cpu->_eflags & DF) ? (uint32_t)(0 - (size >> 3)) : (size >> 3);

    if (rep == 0)
    {
        uint32_t a = rd_elem(cpu, cpu->_esi, size), b = rd_elem(cpu, cpu->_edi, size);
        set_arith_flags(cpu, flags_sub(a, b, 0, size), FLAGS_ARITH);
        cpu->_esi += step;
        cpu->_edi += step;
        return;
    }

    while (cpu->_ecx != 0)
    {
        uint32_t a = rd_elem(cpu, cpu->_esi, size), b = rd_elem(cpu, cpu->_edi, size);
        set_arith_flags(cpu, flags_sub(a, b, 0, size), FLAGS_ARITH);
        cpu->_esi += step;
        cpu->_edi += step;
        cpu->_ecx--;
        if ((rep == 1) && !(cpu->_eflags & ZF)) break;
        if ((rep == 2) && (cpu->_eflags & ZF)) break;
    }
}

EXTERNC void CCALL x86_scas(CPU, uint32_t size, uint32_t rep)
{
    uint32_t step = (cpu->_eflags & DF) ? (uint32_t)(0 - (size >> 3)) : (size >> 3);
    uint32_t acc = cpu->_eax & size_mask(size);

    if (rep == 0)
    {
        set_arith_flags(cpu, flags_sub(acc, rd_elem(cpu, cpu->_edi, size), 0, size), FLAGS_ARITH);
        cpu->_edi += step;
        return;
    }

    while (cpu->_ecx != 0)
    {
        set_arith_flags(cpu, flags_sub(acc, rd_elem(cpu, cpu->_edi, size), 0, size), FLAGS_ARITH);
        cpu->_edi += step;
        cpu->_ecx--;
        if ((rep == 1) && !(cpu->_eflags & ZF)) break;
        if ((rep == 2) && (cpu->_eflags & ZF)) break;
    }
}

/* imul r/m8 (one operand): AX = AL * m8; CF = OF = 1 when AH is not the sign extension of AL */
EXTERNC void CCALL x86_imul8_m(CPU, uint32_t adr)
{
    int16_t r = (int16_t)((int8_t)(cpu->_eax & 0xff) * (int8_t)(*(uint8_t *)REG2PTR(adr)));

    cpu->_eax = (cpu->_eax & 0xffff0000u) | (uint16_t)r;
    set_arith_flags(cpu, (r != (int8_t)r) ? (CF | OF) : 0, CF | OF);
}

/* bsr/bsf: source 0 sets ZF and leaves the destination unchanged */
static inline void bit_scan(CPU, uint32_t regcode, uint32_t v, int reverse)
{
    if (v == 0)
    {
        cpu->_eflags |= ZF;
        return;
    }
    cpu->_eflags &= ~ZF;
    set_gpr(cpu, regcode, 32, reverse ? (uint32_t)(31 - __builtin_clz(v)) : (uint32_t)__builtin_ctz(v));
}

EXTERNC void CCALL x86_bsr_m(CPU, uint32_t regcode, uint32_t adr) { bit_scan(cpu, regcode, rd32(cpu, adr), 1); }
EXTERNC void CCALL x86_bsf_m(CPU, uint32_t regcode, uint32_t adr) { bit_scan(cpu, regcode, rd32(cpu, adr), 0); }
EXTERNC void CCALL x86_bsf_v(CPU, uint32_t regcode, uint32_t v) { bit_scan(cpu, regcode, v, 0); }

EXTERNC void CCALL x86_mfence(CPU)
{
    (void)cpu;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

EXTERNC void CCALL x86_pause(CPU)
{
    (void)cpu;
#if defined(__aarch64__)
    __yield();
#endif
}

/* cpuid: report a Pentium 4 class CPU with SSE and SSE2 only, so the game
   selects code paths that this translator supports. */
EXTERNC void CCALL x86_cpuid(CPU)
{
    static const char vendor[12] = { 'G','e','n','u','i','n','e','I','n','t','e','l' };
    uint32_t leaf = cpu->_eax;

    if (leaf == 0)
    {
        cpu->_eax = 1;
        memcpy(&cpu->_ebx, vendor, 4);
        memcpy(&cpu->_edx, vendor + 4, 4);
        memcpy(&cpu->_ecx, vendor + 8, 4);
    }
    else if (leaf == 1)
    {
        cpu->_eax = 0x00000f29;            /* family 15, model 2, stepping 9 */
        cpu->_ebx = 0x00010800;            /* CLFLUSH size 8 qwords, 1 logical CPU */
        cpu->_ecx = 0;
        /* FPU TSC CX8 CMOV MMX FXSR SSE SSE2 */
        cpu->_edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);
    }
    else if (leaf == 0x80000000u)
    {
        cpu->_eax = 0x80000000u;
        cpu->_ebx = 0; cpu->_ecx = 0; cpu->_edx = 0;
    }
    else
    {
        cpu->_eax = 0; cpu->_ebx = 0; cpu->_ecx = 0; cpu->_edx = 0;
    }
}

/* ------------------------------------------------------------------ */
/* MXCSR                                                               */
/* ------------------------------------------------------------------ */

#define MXCSR_RC(cpu) (((cpu)->_mxcsr >> 13) & 3)

static void apply_mxcsr_ftz(CPU)
{
#if defined(__aarch64__)
    /* MXCSR FTZ (bit 15) and DAZ (bit 6). ARM FPCR.FZ (bit 24) flushes
       inputs and outputs, which equals FTZ+DAZ together. */
    uint64_t fpcr = __arm_rsr64("fpcr");
    if ((cpu->_mxcsr & 0x8000) && (cpu->_mxcsr & 0x40)) fpcr |= (1ull << 24);
    else fpcr &= ~(1ull << 24);
    __arm_wsr64("fpcr", fpcr);
#else
    (void)cpu;
#endif
}

EXTERNC void CCALL sse_ldmxcsr(CPU, uint32_t adr)
{
    cpu->_mxcsr = rd32(cpu, adr) & 0xffff;
    apply_mxcsr_ftz(cpu);
}

EXTERNC void CCALL sse_stmxcsr(CPU, uint32_t adr)
{
    wr32(cpu, adr, cpu->_mxcsr);
}

/* ------------------------------------------------------------------ */
/* float to integer conversions with x86 semantics                     */
/* ------------------------------------------------------------------ */

static inline int32_t cvt_trunc_i32(double v)
{
    if (!(v > -2147483649.0 && v < 2147483648.0)) return INT32_MIN; /* NaN or out of range */
    return (int32_t)v;
}

static inline double round_mode(double v, uint32_t rc)
{
    switch (rc)
    {
        case 0: return nearbyint(v);   /* default FP environment rounds to nearest even */
        case 1: return floor(v);
        case 2: return ceil(v);
        default: return trunc(v);
    }
}

static inline int32_t cvt_round_i32(CPU, double v)
{
    if (v != v) return INT32_MIN;
    return cvt_trunc_i32(round_mode(v, MXCSR_RC(cpu)));
}

/* ------------------------------------------------------------------ */
/* moves                                                               */
/* ------------------------------------------------------------------ */

EXTERNC void CCALL sse_mov128_xx(CPU, uint32_t d, uint32_t s) { XMM(d) = XMM(s); }
EXTERNC void CCALL sse_mov128_xm(CPU, uint32_t d, uint32_t adr) { rd128(cpu, adr, &XMM(d)); }
EXTERNC void CCALL sse_mov128_mx(CPU, uint32_t adr, uint32_t s) { wr128(cpu, adr, &XMM(s)); }

EXTERNC void CCALL sse_movss_xx(CPU, uint32_t d, uint32_t s) { XMM(d).u[0] = XMM(s).u[0]; }
EXTERNC void CCALL sse_movss_xm(CPU, uint32_t d, uint32_t adr)
{
    XMM(d).u[0] = rd32(cpu, adr);
    XMM(d).u[1] = 0; XMM(d).u[2] = 0; XMM(d).u[3] = 0;
}
EXTERNC void CCALL sse_movss_mx(CPU, uint32_t adr, uint32_t s) { wr32(cpu, adr, XMM(s).u[0]); }

EXTERNC void CCALL sse_movsd_xx(CPU, uint32_t d, uint32_t s) { XMM(d).q[0] = XMM(s).q[0]; }
EXTERNC void CCALL sse_movsd_xm(CPU, uint32_t d, uint32_t adr) { XMM(d).q[0] = rd64(cpu, adr); XMM(d).q[1] = 0; }
EXTERNC void CCALL sse_movsd_mx(CPU, uint32_t adr, uint32_t s) { wr64(cpu, adr, XMM(s).q[0]); }

EXTERNC void CCALL sse_movq_xx(CPU, uint32_t d, uint32_t s) { XMM(d).q[0] = XMM(s).q[0]; XMM(d).q[1] = 0; }
EXTERNC void CCALL sse_movq_xm(CPU, uint32_t d, uint32_t adr) { XMM(d).q[0] = rd64(cpu, adr); XMM(d).q[1] = 0; }
EXTERNC void CCALL sse_movq_mx(CPU, uint32_t adr, uint32_t s) { wr64(cpu, adr, XMM(s).q[0]); }

EXTERNC void CCALL sse_movd_xv(CPU, uint32_t d, uint32_t v)
{
    XMM(d).u[0] = v; XMM(d).u[1] = 0; XMM(d).u[2] = 0; XMM(d).u[3] = 0;
}
EXTERNC void CCALL sse_movd_xm(CPU, uint32_t d, uint32_t adr) { sse_movd_xv(cpu, d, rd32(cpu, adr)); }
EXTERNC void CCALL sse_movd_rx(CPU, uint32_t regcode, uint32_t s) { set_gpr(cpu, regcode, 32, XMM(s).u[0]); }
EXTERNC void CCALL sse_movd_mx(CPU, uint32_t adr, uint32_t s) { wr32(cpu, adr, XMM(s).u[0]); }

EXTERNC void CCALL sse_movhlps_xx(CPU, uint32_t d, uint32_t s) { XMM(d).q[0] = XMM(s).q[1]; }
EXTERNC void CCALL sse_movlhps_xx(CPU, uint32_t d, uint32_t s) { XMM(d).q[1] = XMM(s).q[0]; }
EXTERNC void CCALL sse_movhps_xm(CPU, uint32_t d, uint32_t adr) { XMM(d).q[1] = rd64(cpu, adr); }
EXTERNC void CCALL sse_movhps_mx(CPU, uint32_t adr, uint32_t s) { wr64(cpu, adr, XMM(s).q[1]); }
EXTERNC void CCALL sse_movlps_xm(CPU, uint32_t d, uint32_t adr) { XMM(d).q[0] = rd64(cpu, adr); }
EXTERNC void CCALL sse_movlps_mx(CPU, uint32_t adr, uint32_t s) { wr64(cpu, adr, XMM(s).q[0]); }

EXTERNC void CCALL sse_movmskps_rx(CPU, uint32_t regcode, uint32_t s)
{
    uint32_t m = (XMM(s).u[0] >> 31) | ((XMM(s).u[1] >> 31) << 1) | ((XMM(s).u[2] >> 31) << 2) | ((XMM(s).u[3] >> 31) << 3);
    set_gpr(cpu, regcode, 32, m);
}

EXTERNC void CCALL sse_movmskpd_rx(CPU, uint32_t regcode, uint32_t s)
{
    uint32_t m = (uint32_t)(XMM(s).q[0] >> 63) | ((uint32_t)(XMM(s).q[1] >> 63) << 1);
    set_gpr(cpu, regcode, 32, m);
}

/* ------------------------------------------------------------------ */
/* arithmetic: generated from one operation table                      */
/* "_xx" = xmm, xmm; "_xm" = xmm, memory                               */
/* ------------------------------------------------------------------ */

static inline float op_min_f(float a, float b) { return (a < b) ? a : b; }   /* x86: returns b on NaN or equal */
static inline float op_max_f(float a, float b) { return (a > b) ? a : b; }
static inline double op_min_d(double a, double b) { return (a < b) ? a : b; }
static inline double op_max_d(double a, double b) { return (a > b) ? a : b; }

#define SS_BINOP(name, expr) \
    static inline float name##_f(float a, float b) { return (expr); } \
    EXTERNC void CCALL sse_##name##ss_xx(CPU, uint32_t d, uint32_t s) { XMM(d).f[0] = name##_f(XMM(d).f[0], XMM(s).f[0]); } \
    EXTERNC void CCALL sse_##name##ss_xm(CPU, uint32_t d, uint32_t adr) { XMM(d).f[0] = name##_f(XMM(d).f[0], u2f(rd32(cpu, adr))); } \
    EXTERNC void CCALL sse_##name##ps_xx(CPU, uint32_t d, uint32_t s) { \
        float b0 = XMM(s).f[0], b1 = XMM(s).f[1], b2 = XMM(s).f[2], b3 = XMM(s).f[3]; \
        XMM(d).f[0] = name##_f(XMM(d).f[0], b0); XMM(d).f[1] = name##_f(XMM(d).f[1], b1); \
        XMM(d).f[2] = name##_f(XMM(d).f[2], b2); XMM(d).f[3] = name##_f(XMM(d).f[3], b3); } \
    EXTERNC void CCALL sse_##name##ps_xm(CPU, uint32_t d, uint32_t adr) { \
        float b[4]; rd128(cpu, adr, b); \
        XMM(d).f[0] = name##_f(XMM(d).f[0], b[0]); XMM(d).f[1] = name##_f(XMM(d).f[1], b[1]); \
        XMM(d).f[2] = name##_f(XMM(d).f[2], b[2]); XMM(d).f[3] = name##_f(XMM(d).f[3], b[3]); }

#define SD_BINOP(name, expr) \
    static inline double name##_d(double a, double b) { return (expr); } \
    EXTERNC void CCALL sse_##name##sd_xx(CPU, uint32_t d, uint32_t s) { XMM(d).d[0] = name##_d(XMM(d).d[0], XMM(s).d[0]); } \
    EXTERNC void CCALL sse_##name##sd_xm(CPU, uint32_t d, uint32_t adr) { XMM(d).d[0] = name##_d(XMM(d).d[0], q2d(rd64(cpu, adr))); } \
    EXTERNC void CCALL sse_##name##pd_xx(CPU, uint32_t d, uint32_t s) { \
        double b0 = XMM(s).d[0], b1 = XMM(s).d[1]; \
        XMM(d).d[0] = name##_d(XMM(d).d[0], b0); XMM(d).d[1] = name##_d(XMM(d).d[1], b1); } \
    EXTERNC void CCALL sse_##name##pd_xm(CPU, uint32_t d, uint32_t adr) { \
        double b[2]; rd128(cpu, adr, b); \
        XMM(d).d[0] = name##_d(XMM(d).d[0], b[0]); XMM(d).d[1] = name##_d(XMM(d).d[1], b[1]); }

SS_BINOP(add, a + b)
SS_BINOP(sub, a - b)
SS_BINOP(mul, a * b)
SS_BINOP(div, a / b)
SS_BINOP(min, op_min_f(a, b))
SS_BINOP(max, op_max_f(a, b))

SD_BINOP(add, a + b)
SD_BINOP(sub, a - b)
SD_BINOP(mul, a * b)
SD_BINOP(div, a / b)
SD_BINOP(min, op_min_d(a, b))
SD_BINOP(max, op_max_d(a, b))

/* Unary: result written to d from source s. rcp/rsqrt return the exact
   value; x86 hardware returns a 12-bit approximation whose exact bits
   differ between CPU vendors. */
#define SS_UNOP(name, expr) \
    static inline float name##_u(float a) { return (expr); } \
    EXTERNC void CCALL sse_##name##ss_xx(CPU, uint32_t d, uint32_t s) { XMM(d).f[0] = name##_u(XMM(s).f[0]); } \
    EXTERNC void CCALL sse_##name##ss_xm(CPU, uint32_t d, uint32_t adr) { XMM(d).f[0] = name##_u(u2f(rd32(cpu, adr))); } \
    EXTERNC void CCALL sse_##name##ps_xx(CPU, uint32_t d, uint32_t s) { \
        float a0 = XMM(s).f[0], a1 = XMM(s).f[1], a2 = XMM(s).f[2], a3 = XMM(s).f[3]; \
        XMM(d).f[0] = name##_u(a0); XMM(d).f[1] = name##_u(a1); XMM(d).f[2] = name##_u(a2); XMM(d).f[3] = name##_u(a3); } \
    EXTERNC void CCALL sse_##name##ps_xm(CPU, uint32_t d, uint32_t adr) { \
        float a[4]; rd128(cpu, adr, a); \
        XMM(d).f[0] = name##_u(a[0]); XMM(d).f[1] = name##_u(a[1]); XMM(d).f[2] = name##_u(a[2]); XMM(d).f[3] = name##_u(a[3]); }

SS_UNOP(sqrt, sqrtf(a))
SS_UNOP(rcp, 1.0f / a)
SS_UNOP(rsqrt, 1.0f / sqrtf(a))

EXTERNC void CCALL sse_sqrtsd_xx(CPU, uint32_t d, uint32_t s) { XMM(d).d[0] = sqrt(XMM(s).d[0]); }
EXTERNC void CCALL sse_sqrtsd_xm(CPU, uint32_t d, uint32_t adr) { XMM(d).d[0] = sqrt(q2d(rd64(cpu, adr))); }
EXTERNC void CCALL sse_sqrtpd_xx(CPU, uint32_t d, uint32_t s) { double a0 = XMM(s).d[0], a1 = XMM(s).d[1]; XMM(d).d[0] = sqrt(a0); XMM(d).d[1] = sqrt(a1); }
EXTERNC void CCALL sse_sqrtpd_xm(CPU, uint32_t d, uint32_t adr) { double a[2]; rd128(cpu, adr, a); XMM(d).d[0] = sqrt(a[0]); XMM(d).d[1] = sqrt(a[1]); }

/* bitwise 128-bit operations (andps/andpd/pand, ...) */
#define BITOP(name, expr) \
    EXTERNC void CCALL sse_##name##_xx(CPU, uint32_t d, uint32_t s) { \
        uint64_t b0 = XMM(s).q[0], b1 = XMM(s).q[1], a; \
        a = XMM(d).q[0]; XMM(d).q[0] = (expr(a, b0)); \
        a = XMM(d).q[1]; XMM(d).q[1] = (expr(a, b1)); } \
    EXTERNC void CCALL sse_##name##_xm(CPU, uint32_t d, uint32_t adr) { \
        uint64_t b[2], a; rd128(cpu, adr, b); \
        a = XMM(d).q[0]; XMM(d).q[0] = (expr(a, b[0])); \
        a = XMM(d).q[1]; XMM(d).q[1] = (expr(a, b[1])); }

#define E_AND(a, b) ((a) & (b))
#define E_ANDN(a, b) ((~(a)) & (b))
#define E_OR(a, b) ((a) | (b))
#define E_XOR(a, b) ((a) ^ (b))

BITOP(and128, E_AND)
BITOP(andn128, E_ANDN)
BITOP(or128, E_OR)
BITOP(xor128, E_XOR)

/* ------------------------------------------------------------------ */
/* shuffles and unpacks                                                */
/* ------------------------------------------------------------------ */

static inline void shufps(CPU, uint32_t d, const uint32_t *b, uint32_t imm)
{
    uint32_t a[4], r[4];

    memcpy(a, XMM(d).u, 16);
    r[0] = a[imm & 3];
    r[1] = a[(imm >> 2) & 3];
    r[2] = b[(imm >> 4) & 3];
    r[3] = b[(imm >> 6) & 3];
    memcpy(XMM(d).u, r, 16);
}

EXTERNC void CCALL sse_shufps_xxi(CPU, uint32_t d, uint32_t s, uint32_t imm) { uint32_t b[4]; memcpy(b, XMM(s).u, 16); shufps(cpu, d, b, imm); }
EXTERNC void CCALL sse_shufps_xmi(CPU, uint32_t d, uint32_t adr, uint32_t imm) { uint32_t b[4]; rd128(cpu, adr, b); shufps(cpu, d, b, imm); }

static inline void shufpd(CPU, uint32_t d, const uint64_t *b, uint32_t imm)
{
    uint64_t r0 = XMM(d).q[imm & 1], r1 = b[(imm >> 1) & 1];
    XMM(d).q[0] = r0; XMM(d).q[1] = r1;
}

EXTERNC void CCALL sse_shufpd_xxi(CPU, uint32_t d, uint32_t s, uint32_t imm) { uint64_t b[2]; memcpy(b, XMM(s).q, 16); shufpd(cpu, d, b, imm); }
EXTERNC void CCALL sse_shufpd_xmi(CPU, uint32_t d, uint32_t adr, uint32_t imm) { uint64_t b[2]; rd128(cpu, adr, b); shufpd(cpu, d, b, imm); }

static inline void pshufd(CPU, uint32_t d, const uint32_t *b, uint32_t imm)
{
    uint32_t r[4];
    r[0] = b[imm & 3]; r[1] = b[(imm >> 2) & 3]; r[2] = b[(imm >> 4) & 3]; r[3] = b[(imm >> 6) & 3];
    memcpy(XMM(d).u, r, 16);
}

EXTERNC void CCALL sse_pshufd_xxi(CPU, uint32_t d, uint32_t s, uint32_t imm) { uint32_t b[4]; memcpy(b, XMM(s).u, 16); pshufd(cpu, d, b, imm); }
EXTERNC void CCALL sse_pshufd_xmi(CPU, uint32_t d, uint32_t adr, uint32_t imm) { uint32_t b[4]; rd128(cpu, adr, b); pshufd(cpu, d, b, imm); }

#define UNPCK(name, T, N, body) \
    static inline void name##_do(CPU, uint32_t d, const T *b) { T a[N], r[N]; memcpy(a, &XMM(d), 16); body; memcpy(&XMM(d), r, 16); } \
    EXTERNC void CCALL sse_##name##_xx(CPU, uint32_t d, uint32_t s) { T b[N]; memcpy(b, &XMM(s), 16); name##_do(cpu, d, b); } \
    EXTERNC void CCALL sse_##name##_xm(CPU, uint32_t d, uint32_t adr) { T b[N]; rd128(cpu, adr, b); name##_do(cpu, d, b); }

UNPCK(unpcklps, uint32_t, 4, { r[0] = a[0]; r[1] = b[0]; r[2] = a[1]; r[3] = b[1]; })
UNPCK(unpckhps, uint32_t, 4, { r[0] = a[2]; r[1] = b[2]; r[2] = a[3]; r[3] = b[3]; })
UNPCK(unpcklpd, uint64_t, 2, { r[0] = a[0]; r[1] = b[0]; })
UNPCK(unpckhpd, uint64_t, 2, { r[0] = a[1]; r[1] = b[1]; })
UNPCK(punpcklbw, uint8_t, 16, { int i; for (i = 0; i < 8; i++) { r[2*i] = a[i]; r[2*i+1] = b[i]; } })
UNPCK(punpckhbw, uint8_t, 16, { int i; for (i = 0; i < 8; i++) { r[2*i] = a[8+i]; r[2*i+1] = b[8+i]; } })
UNPCK(punpcklwd, uint16_t, 8, { int i; for (i = 0; i < 4; i++) { r[2*i] = a[i]; r[2*i+1] = b[i]; } })
UNPCK(punpckhwd, uint16_t, 8, { int i; for (i = 0; i < 4; i++) { r[2*i] = a[4+i]; r[2*i+1] = b[4+i]; } })
UNPCK(punpckldq, uint32_t, 4, { r[0] = a[0]; r[1] = b[0]; r[2] = a[1]; r[3] = b[1]; })
UNPCK(punpckhdq, uint32_t, 4, { r[0] = a[2]; r[1] = b[2]; r[2] = a[3]; r[3] = b[3]; })
UNPCK(punpcklqdq, uint64_t, 2, { r[0] = a[0]; r[1] = b[0]; })
UNPCK(punpckhqdq, uint64_t, 2, { r[0] = a[1]; r[1] = b[1]; })

/* ------------------------------------------------------------------ */
/* packed integer arithmetic and compares                              */
/* ------------------------------------------------------------------ */

#define PINT(name, T, N, expr) \
    static inline void name##_do(CPU, uint32_t d, const T *b) { T a[N]; int i; memcpy(a, &XMM(d), 16); \
        for (i = 0; i < N; i++) { T x = a[i], y = b[i]; a[i] = (T)(expr); } memcpy(&XMM(d), a, 16); } \
    EXTERNC void CCALL sse_##name##_xx(CPU, uint32_t d, uint32_t s) { T b[N]; memcpy(b, &XMM(s), 16); name##_do(cpu, d, b); } \
    EXTERNC void CCALL sse_##name##_xm(CPU, uint32_t d, uint32_t adr) { T b[N]; rd128(cpu, adr, b); name##_do(cpu, d, b); }

PINT(paddb, uint8_t, 16, x + y)
PINT(paddw, uint16_t, 8, x + y)
PINT(paddd, uint32_t, 4, x + y)
PINT(paddq, uint64_t, 2, x + y)
PINT(psubb, uint8_t, 16, x - y)
PINT(psubw, uint16_t, 8, x - y)
PINT(psubd, uint32_t, 4, x - y)
PINT(psubq, uint64_t, 2, x - y)
PINT(pmullw, uint16_t, 8, (uint32_t)x * (uint32_t)y)
PINT(pmulhw, int16_t, 8, ((int32_t)x * (int32_t)y) >> 16)
PINT(pmulhuw, uint16_t, 8, ((uint32_t)x * (uint32_t)y) >> 16)
PINT(pcmpeqb, uint8_t, 16, (x == y) ? 0xff : 0)
PINT(pcmpeqw, uint16_t, 8, (x == y) ? 0xffff : 0)
PINT(pcmpeqd, uint32_t, 4, (x == y) ? 0xffffffffu : 0)
PINT(pcmpgtb, int8_t, 16, (x > y) ? -1 : 0)
PINT(pcmpgtw, int16_t, 8, (x > y) ? -1 : 0)
PINT(pcmpgtd, int32_t, 4, (x > y) ? -1 : 0)

#define PSHIFT(name, T, N, expr, bits) \
    static inline void name##_do(CPU, uint32_t d, uint64_t cnt) { T a[N]; int i; memcpy(a, &XMM(d), 16); \
        for (i = 0; i < N; i++) { T x = a[i]; a[i] = (T)(expr); } memcpy(&XMM(d), a, 16); } \
    EXTERNC void CCALL sse_##name##_xi(CPU, uint32_t d, uint32_t imm) { name##_do(cpu, d, imm & 0xff); } \
    EXTERNC void CCALL sse_##name##_xx(CPU, uint32_t d, uint32_t s) { name##_do(cpu, d, XMM(s).q[0]); } \
    EXTERNC void CCALL sse_##name##_xm(CPU, uint32_t d, uint32_t adr) { name##_do(cpu, d, rd64(cpu, adr)); }

PSHIFT(psllw, uint16_t, 8, (cnt > 15) ? 0 : (uint32_t)x << cnt, 16)
PSHIFT(pslld, uint32_t, 4, (cnt > 31) ? 0 : x << cnt, 32)
PSHIFT(psllq, uint64_t, 2, (cnt > 63) ? 0 : x << cnt, 64)
PSHIFT(psrlw, uint16_t, 8, (cnt > 15) ? 0 : x >> cnt, 16)
PSHIFT(psrld, uint32_t, 4, (cnt > 31) ? 0 : x >> cnt, 32)
PSHIFT(psrlq, uint64_t, 2, (cnt > 63) ? 0 : x >> cnt, 64)
PSHIFT(psraw, int16_t, 8, x >> ((cnt > 15) ? 15 : cnt), 16)
PSHIFT(psrad, int32_t, 4, x >> ((cnt > 31) ? 31 : cnt), 32)

EXTERNC void CCALL sse_pslldq_xi(CPU, uint32_t d, uint32_t imm)
{
    uint8_t a[16], r[16];
    uint32_t i, n = imm & 0xff;

    memcpy(a, &XMM(d), 16);
    for (i = 0; i < 16; i++) r[i] = (n > 15 || i < n) ? 0 : a[i - n];
    memcpy(&XMM(d), r, 16);
}

EXTERNC void CCALL sse_psrldq_xi(CPU, uint32_t d, uint32_t imm)
{
    uint8_t a[16], r[16];
    uint32_t i, n = imm & 0xff;

    memcpy(a, &XMM(d), 16);
    for (i = 0; i < 16; i++) r[i] = (n > 15 || i + n > 15) ? 0 : a[i + n];
    memcpy(&XMM(d), r, 16);
}

/* ------------------------------------------------------------------ */
/* float compares                                                      */
/* ------------------------------------------------------------------ */

static inline int cmp_pred_f(double a, double b, uint32_t imm)
{
    switch (imm & 7)
    {
        case 0: return a == b;                      /* eq (false if unordered) */
        case 1: return a < b;                       /* lt */
        case 2: return a <= b;                      /* le */
        case 3: return (a != a) || (b != b);        /* unord */
        case 4: return !(a == b);                   /* neq (true if unordered) */
        case 5: return !(a < b);                    /* nlt */
        case 6: return !(a <= b);                   /* nle */
        default: return (a == a) && (b == b);       /* ord */
    }
}

EXTERNC void CCALL sse_cmpss_xxi(CPU, uint32_t d, uint32_t s, uint32_t imm) { XMM(d).u[0] = cmp_pred_f(XMM(d).f[0], XMM(s).f[0], imm) ? 0xffffffffu : 0; }
EXTERNC void CCALL sse_cmpss_xmi(CPU, uint32_t d, uint32_t adr, uint32_t imm) { XMM(d).u[0] = cmp_pred_f(XMM(d).f[0], u2f(rd32(cpu, adr)), imm) ? 0xffffffffu : 0; }
EXTERNC void CCALL sse_cmpsd_xxi(CPU, uint32_t d, uint32_t s, uint32_t imm) { XMM(d).q[0] = cmp_pred_f(XMM(d).d[0], XMM(s).d[0], imm) ? ~0ull : 0; }
EXTERNC void CCALL sse_cmpsd_xmi(CPU, uint32_t d, uint32_t adr, uint32_t imm) { XMM(d).q[0] = cmp_pred_f(XMM(d).d[0], q2d(rd64(cpu, adr)), imm) ? ~0ull : 0; }

static inline void cmpps(CPU, uint32_t d, const float *b, uint32_t imm)
{
    int i;
    for (i = 0; i < 4; i++) XMM(d).u[i] = cmp_pred_f(XMM(d).f[i], b[i], imm) ? 0xffffffffu : 0;
}

EXTERNC void CCALL sse_cmpps_xxi(CPU, uint32_t d, uint32_t s, uint32_t imm) { float b[4]; memcpy(b, XMM(s).f, 16); cmpps(cpu, d, b, imm); }
EXTERNC void CCALL sse_cmpps_xmi(CPU, uint32_t d, uint32_t adr, uint32_t imm) { float b[4]; rd128(cpu, adr, b); cmpps(cpu, d, b, imm); }

static inline void cmppd(CPU, uint32_t d, const double *b, uint32_t imm)
{
    int i;
    for (i = 0; i < 2; i++) XMM(d).q[i] = cmp_pred_f(XMM(d).d[i], b[i], imm) ? ~0ull : 0;
}

EXTERNC void CCALL sse_cmppd_xxi(CPU, uint32_t d, uint32_t s, uint32_t imm) { double b[2]; memcpy(b, XMM(s).d, 16); cmppd(cpu, d, b, imm); }
EXTERNC void CCALL sse_cmppd_xmi(CPU, uint32_t d, uint32_t adr, uint32_t imm) { double b[2]; rd128(cpu, adr, b); cmppd(cpu, d, b, imm); }

/* comiss/ucomiss/comisd/ucomisd: ZF PF CF from the compare, OF SF AF cleared */
static inline void comi_flags(CPU, double a, double b)
{
    uint32_t f;

    if ((a != a) || (b != b)) f = ZF | PF | CF;
    else if (a < b) f = CF;
    else if (a == b) f = ZF;
    else f = 0;
    set_arith_flags(cpu, f, FLAGS_ARITH);
}

EXTERNC void CCALL sse_comiss_xx(CPU, uint32_t d, uint32_t s) { comi_flags(cpu, XMM(d).f[0], XMM(s).f[0]); }
EXTERNC void CCALL sse_comiss_xm(CPU, uint32_t d, uint32_t adr) { comi_flags(cpu, XMM(d).f[0], u2f(rd32(cpu, adr))); }
EXTERNC void CCALL sse_comisd_xx(CPU, uint32_t d, uint32_t s) { comi_flags(cpu, XMM(d).d[0], XMM(s).d[0]); }
EXTERNC void CCALL sse_comisd_xm(CPU, uint32_t d, uint32_t adr) { comi_flags(cpu, XMM(d).d[0], q2d(rd64(cpu, adr))); }

/* ------------------------------------------------------------------ */
/* conversions                                                         */
/* ------------------------------------------------------------------ */

EXTERNC void CCALL sse_cvtsi2ss_xv(CPU, uint32_t d, uint32_t v) { XMM(d).f[0] = (float)(int32_t)v; }
EXTERNC void CCALL sse_cvtsi2sd_xv(CPU, uint32_t d, uint32_t v) { XMM(d).d[0] = (double)(int32_t)v; }

EXTERNC void CCALL sse_cvttss2si_rx(CPU, uint32_t r, uint32_t s) { set_gpr(cpu, r, 32, (uint32_t)cvt_trunc_i32(XMM(s).f[0])); }
EXTERNC void CCALL sse_cvttss2si_rm(CPU, uint32_t r, uint32_t adr) { set_gpr(cpu, r, 32, (uint32_t)cvt_trunc_i32(u2f(rd32(cpu, adr)))); }
EXTERNC void CCALL sse_cvtss2si_rx(CPU, uint32_t r, uint32_t s) { set_gpr(cpu, r, 32, (uint32_t)cvt_round_i32(cpu, XMM(s).f[0])); }
EXTERNC void CCALL sse_cvtss2si_rm(CPU, uint32_t r, uint32_t adr) { set_gpr(cpu, r, 32, (uint32_t)cvt_round_i32(cpu, u2f(rd32(cpu, adr)))); }
EXTERNC void CCALL sse_cvttsd2si_rx(CPU, uint32_t r, uint32_t s) { set_gpr(cpu, r, 32, (uint32_t)cvt_trunc_i32(XMM(s).d[0])); }
EXTERNC void CCALL sse_cvttsd2si_rm(CPU, uint32_t r, uint32_t adr) { set_gpr(cpu, r, 32, (uint32_t)cvt_trunc_i32(q2d(rd64(cpu, adr)))); }
EXTERNC void CCALL sse_cvtsd2si_rx(CPU, uint32_t r, uint32_t s) { set_gpr(cpu, r, 32, (uint32_t)cvt_round_i32(cpu, XMM(s).d[0])); }
EXTERNC void CCALL sse_cvtsd2si_rm(CPU, uint32_t r, uint32_t adr) { set_gpr(cpu, r, 32, (uint32_t)cvt_round_i32(cpu, q2d(rd64(cpu, adr)))); }

EXTERNC void CCALL sse_cvtss2sd_xx(CPU, uint32_t d, uint32_t s) { XMM(d).d[0] = (double)XMM(s).f[0]; }
EXTERNC void CCALL sse_cvtss2sd_xm(CPU, uint32_t d, uint32_t adr) { XMM(d).d[0] = (double)u2f(rd32(cpu, adr)); }
EXTERNC void CCALL sse_cvtsd2ss_xx(CPU, uint32_t d, uint32_t s) { XMM(d).f[0] = (float)XMM(s).d[0]; }
EXTERNC void CCALL sse_cvtsd2ss_xm(CPU, uint32_t d, uint32_t adr) { XMM(d).f[0] = (float)q2d(rd64(cpu, adr)); }

static inline void cvtdq2ps(CPU, uint32_t d, const int32_t *a) { int i; for (i = 0; i < 4; i++) XMM(d).f[i] = (float)a[i]; }
EXTERNC void CCALL sse_cvtdq2ps_xx(CPU, uint32_t d, uint32_t s) { int32_t a[4]; memcpy(a, XMM(s).i, 16); cvtdq2ps(cpu, d, a); }
EXTERNC void CCALL sse_cvtdq2ps_xm(CPU, uint32_t d, uint32_t adr) { int32_t a[4]; rd128(cpu, adr, a); cvtdq2ps(cpu, d, a); }

static inline void cvttps2dq(CPU, uint32_t d, const float *a) { int i; for (i = 0; i < 4; i++) XMM(d).i[i] = cvt_trunc_i32(a[i]); }
EXTERNC void CCALL sse_cvttps2dq_xx(CPU, uint32_t d, uint32_t s) { float a[4]; memcpy(a, XMM(s).f, 16); cvttps2dq(cpu, d, a); }
EXTERNC void CCALL sse_cvttps2dq_xm(CPU, uint32_t d, uint32_t adr) { float a[4]; rd128(cpu, adr, a); cvttps2dq(cpu, d, a); }

static inline void cvtps2dq(CPU, uint32_t d, const float *a) { int i; for (i = 0; i < 4; i++) XMM(d).i[i] = cvt_round_i32(cpu, a[i]); }
EXTERNC void CCALL sse_cvtps2dq_xx(CPU, uint32_t d, uint32_t s) { float a[4]; memcpy(a, XMM(s).f, 16); cvtps2dq(cpu, d, a); }
EXTERNC void CCALL sse_cvtps2dq_xm(CPU, uint32_t d, uint32_t adr) { float a[4]; rd128(cpu, adr, a); cvtps2dq(cpu, d, a); }

static inline void cvtps2pd(CPU, uint32_t d, const float *a) { double r0 = a[0], r1 = a[1]; XMM(d).d[0] = r0; XMM(d).d[1] = r1; }
EXTERNC void CCALL sse_cvtps2pd_xx(CPU, uint32_t d, uint32_t s) { float a[2]; memcpy(a, XMM(s).f, 8); cvtps2pd(cpu, d, a); }
EXTERNC void CCALL sse_cvtps2pd_xm(CPU, uint32_t d, uint32_t adr) { float a[2]; memcpy(a, REG2PTR(adr), 8); cvtps2pd(cpu, d, a); }

static inline void cvtpd2ps(CPU, uint32_t d, const double *a) { float r0 = (float)a[0], r1 = (float)a[1]; XMM(d).f[0] = r0; XMM(d).f[1] = r1; XMM(d).u[2] = 0; XMM(d).u[3] = 0; }
EXTERNC void CCALL sse_cvtpd2ps_xx(CPU, uint32_t d, uint32_t s) { double a[2]; memcpy(a, XMM(s).d, 16); cvtpd2ps(cpu, d, a); }
EXTERNC void CCALL sse_cvtpd2ps_xm(CPU, uint32_t d, uint32_t adr) { double a[2]; rd128(cpu, adr, a); cvtpd2ps(cpu, d, a); }

/* ------------------------------------------------------------------ */
/* x87 additions                                                       */
/* ------------------------------------------------------------------ */

#define X87_ST(i) cpu->_st[(cpu->_st_top + (i)) & 7]
#define X87_POP { cpu->_st_top = (cpu->_st_top + 1) & 7; cpu->_st_sw_cond &= ~0x0200u & 0x4700u; }
#define X87_PUSH { cpu->_st_sw_cond &= ~0x0200u & 0x4700u; cpu->_st_top = (cpu->_st_top + 7) & 7; }

/* fcomi/fucomi st0, st(i) [p]: ZF PF CF from the compare, OF SF AF cleared */
EXTERNC void CCALL x87_fcomi_st(CPU, uint32_t num, uint32_t pop)
{
    comi_flags(cpu, X87_ST(0), X87_ST(num));
    cpu->_st_sw_cond &= ~0x0200u & 0x4700u;
    if (pop) X87_POP
}

/* fcmovcc st0, st(i); cc is the x86 condition code (0..15) */
EXTERNC void CCALL x87_fcmov_st(CPU, uint32_t num, uint32_t cc)
{
    if (cond_eval(cpu->_eflags, cc)) X87_ST(0) = X87_ST(num);
}

/* ffreep st(i): llasm keeps no x87 tag word, so only the pop is visible */
EXTERNC void CCALL x87_ffreep_st(CPU, uint32_t num)
{
    (void)num;
    X87_POP
}

EXTERNC void CCALL x87_fpatan_void(CPU)
{
    X87_ST(1) = atan2(X87_ST(1), X87_ST(0));
    X87_POP
}

EXTERNC void CCALL x87_fsincos_void(CPU)
{
    double v = X87_ST(0);
    X87_ST(0) = sin(v);
    X87_PUSH
    X87_ST(0) = cos(v);
    cpu->_st_sw_cond &= ~0x0400u & 0x4700u;   /* C2 = 0: operand in range */
}

EXTERNC void CCALL x87_fscale_void(CPU)
{
    double s = trunc(X87_ST(1));
    X87_ST(0) = ldexp(X87_ST(0), (s > 65536.0) ? 65536 : ((s < -65536.0) ? -65536 : (int)s));
    cpu->_st_sw_cond &= ~0x0200u & 0x4700u;
}

EXTERNC void CCALL x87_frndint_void(CPU)
{
    X87_ST(0) = round_mode(X87_ST(0), (cpu->_st_cw >> 10) & 3);
    cpu->_st_sw_cond &= ~0x0200u & 0x4700u;
}

EXTERNC void CCALL x87_f2xm1_void(CPU)
{
    X87_ST(0) = exp2(X87_ST(0)) - 1.0;
    cpu->_st_sw_cond &= ~0x0200u & 0x4700u;
}

EXTERNC void CCALL x87_fldl2e_void(CPU)
{
    X87_PUSH
    X87_ST(0) = 1.44269504088896340736; /* log2(e) */
}

EXTERNC void CCALL x87_fldl2t_void(CPU)
{
    X87_PUSH
    X87_ST(0) = 3.32192809488736234787; /* log2(10) */
}

EXTERNC void CCALL x87_fldpi_void(CPU)
{
    X87_PUSH
    X87_ST(0) = M_PI;
}

/* fisttp: store with truncation, then pop (SSE3) */
EXTERNC uint32_t CCALL x87_fisttp_int32(CPU)
{
    uint32_t v = (uint32_t)cvt_trunc_i32(X87_ST(0));
    X87_POP
    return v;
}
