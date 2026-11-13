/*
 * Core declarations of the Win32 runtime for the translated game code.
 *
 * Guest addresses are 32-bit offsets from rt_ptrofs (the llasm
 * "pointer offset"). All guest-visible memory (the program image, the
 * import thunks, guest stacks and the guest heap) is in the 2 GiB window
 * [rt_ptrofs, rt_ptrofs + RT_WINDOW_SIZE). Guest address 0 is NULL.
 */
#ifndef RT_H
#define RT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "llasm_cpu.h"

#define RT_WINDOW_SIZE UINT64_C(0x80000000)
#define RT_GRANULE 0x10000u /* Windows allocation granularity */

extern uint64_t rt_ptrofs;

/* Print a message and stop the process. Used for every condition that the
   runtime does not support; there is no fallback behavior. */
_Noreturn void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Guest address to host pointer. Guest address 0 gives NULL. */
static inline void *G2H(uint32_t g)
{
    return g ? (void *)(uintptr_t)(rt_ptrofs + g) : NULL;
}

/* Host pointer to guest address. NULL gives 0. A pointer outside the
   window is a runtime defect and stops the process. */
uint32_t rt_h2g_checked(const void *p);
static inline uint32_t H2G(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    if (p == NULL) return 0;
    if (v <= rt_ptrofs || v - rt_ptrofs >= RT_WINDOW_SIZE) return rt_h2g_checked(p);
    return (uint32_t)(v - rt_ptrofs);
}

static inline uint32_t rd32(uint32_t g) { uint32_t v; memcpy(&v, G2H(g), 4); return v; }
static inline uint16_t rd16(uint32_t g) { uint16_t v; memcpy(&v, G2H(g), 2); return v; }
static inline uint8_t rd8(uint32_t g) { return *(uint8_t *)G2H(g); }
static inline uint64_t rd64(uint32_t g) { uint64_t v; memcpy(&v, G2H(g), 8); return v; }
static inline double rdf64(uint32_t g) { double v; memcpy(&v, G2H(g), 8); return v; }
static inline void wr32(uint32_t g, uint32_t v) { memcpy(G2H(g), &v, 4); }
static inline void wr16(uint32_t g, uint16_t v) { memcpy(G2H(g), &v, 2); }
static inline void wr8(uint32_t g, uint8_t v) { *(uint8_t *)G2H(g) = v; }
static inline void wr64(uint32_t g, uint64_t v) { memcpy(G2H(g), &v, 8); }

/* Argument n (0-based) of an import call: [esp + 4 + 4*n]. */
#define ARG(n) rd32(esp + 4u + 4u * (uint32_t)(n))
#define ARGP(n) G2H(ARG(n))
/* Address of argument n on the guest stack (for variable arguments). */
#define ARGADR(n) (esp + 4u + 4u * (uint32_t)(n))

/* Set the 32-bit result. */
#define RET(v) do { eax = (uint32_t)(v); } while (0)
/* Set a 64-bit result (edx:eax). */
#define RET64(v) do { uint64_t rv_ = (uint64_t)(v); eax = (uint32_t)rv_; edx = (uint32_t)(rv_ >> 32); } while (0)

/* The x87 register stack of the guest. A function result of type double
   is in st(0). */
static inline void rt_fpu_push(_cpu *cpu, double v)
{
    cpu->_st_top = (cpu->_st_top + 7u) & 7u;
    cpu->_st[cpu->_st_top & 7u] = v;
}
static inline double rt_fpu_pop(_cpu *cpu)
{
    double v = cpu->_st[cpu->_st_top & 7u];
    cpu->_st_top = (cpu->_st_top + 1u) & 7u;
    return v;
}
static inline double rt_fpu_st(_cpu *cpu, unsigned i)
{
    return cpu->_st[(cpu->_st_top + i) & 7u];
}
#define RETF(v) rt_fpu_push(cpu, (double)(v))

/* A double argument at argument dword n. */
#define ARGF(n) rdf64(esp + 4u + 4u * (uint32_t)(n))

/* ---------------------------------------------------------------- memory */

void rt_mem_init(void);
/* The file of the D3D9 pipeline cache (rt_d3d9_draw.m); before the first
   CreateDevice. */
void rt_d3d9_pipeline_cache_init(const char *path);
/* Address space (Windows VirtualAlloc semantics, 64 KiB granularity). */
uint32_t rt_va_reserve(uint32_t addr, uint32_t size);           /* returns base or 0 */
int rt_va_commit(uint32_t addr, uint32_t size);                 /* 0 on success */
int rt_va_decommit(uint32_t addr, uint32_t size);
int rt_va_release(uint32_t base);
/* Committed and readable guest range check. The program image counts as
   committed. */
int rt_va_is_committed(uint32_t addr, uint32_t size);
uint64_t rt_va_free_bytes(void);

/* Guest heap (16-byte aligned blocks). Return 0 when no memory is left. */
uint32_t rt_heap_alloc(uint32_t size);
uint32_t rt_heap_calloc(uint32_t count, uint32_t size);
uint32_t rt_heap_realloc(uint32_t ptr, uint32_t size);
void rt_heap_free(uint32_t ptr);
uint32_t rt_heap_size(uint32_t ptr);

/* Copy a NUL-terminated guest string with at most max-1 characters into a
   host buffer. Returns 0 on success, -1 if the string is longer. */
int rt_gstr(char *dst, size_t max, uint32_t gstr);

/* Windows-1252 (the ANSI code page) byte to Unicode (rt_kernel_misc.c). */
uint16_t rt_cp1252_to_unicode(uint8_t c);

/* ---------------------------------------------------------------- errors */

void rt_set_last_error(uint32_t err);
uint32_t rt_get_last_error(void);

#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_TOO_MANY_OPEN_FILES 4u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_DATA 13u
#define ERROR_OUTOFMEMORY 14u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_HANDLE_EOF 38u
#define ERROR_NOT_SUPPORTED 50u
#define ERROR_FILE_EXISTS 80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_MOD_NOT_FOUND 126u
#define ERROR_PROC_NOT_FOUND 127u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_MORE_DATA 234u
#define ERROR_NO_MORE_ITEMS 259u

#endif
