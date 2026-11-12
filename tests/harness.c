/*
 * Shared parts of the runtime unit tests: the guest symbols that the
 * runtime refers to (the test programs have no translated game code; these
 * definitions stop the test if a test reaches them) and helpers that call
 * import functions with arguments on the guest stack.
 *
 * The tests link the runtime llasm objects (build/rt/calls.o and
 * build/rt/com_thunks.o), so rt_call can call the COM method thunks
 * through guest vtables.
 */
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- guest symbols of the test program */

char launcher_image[16];
char engine_image[16];
const rt_import_entry rt_import_table[1];
const unsigned rt_import_table_count = 0;
const rt_export_entry rt_engine_exports[1];
const unsigned rt_engine_exports_count = 0;
void engine_entry(void *c) { (void)c; rt_fatal("test: engine_entry reached"); }
void launcher_entry(void *c) { (void)c; rt_fatal("test: launcher_entry reached"); }

/* ---------------------------------------------------------------- helpers */

_cpu *cpu;
int failures, checks;

uint32_t gs(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t g = rt_heap_alloc((uint32_t)n);
    if (g == 0) rt_fatal("test: no guest memory");
    memcpy(G2H(g), s, n);
    return g;
}

uint32_t gws(const char *s)
{
    size_t n = strlen(s) + 1, i;
    uint32_t g = rt_heap_alloc((uint32_t)(2 * n));
    if (g == 0) rt_fatal("test: no guest memory");
    for (i = 0; i < n; i++) wr16(g + 2u * (uint32_t)i, (uint8_t)s[i]);
    return g;
}

uint32_t gbuf(uint32_t n)
{
    uint32_t g = rt_heap_calloc(1, n);
    if (g == 0) rt_fatal("test: no guest memory");
    return g;
}

/* Argument words: a 32-bit value, or the low and high halves of a double. */

void a32(argv_t *a, uint32_t v) { a->w[a->n++] = v; }
void af(argv_t *a, double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    a->w[a->n++] = (uint32_t)b;
    a->w[a->n++] = (uint32_t)(b >> 32);
}
void a64(argv_t *a, uint64_t v)
{
    a->w[a->n++] = (uint32_t)v;
    a->w[a->n++] = (uint32_t)(v >> 32);
}

uint32_t callv(void (*fn)(_cpu *), const argv_t *a)
{
    uint32_t saved = cpu->_esp, r;
    int i;
    cpu->_esp = (saved - 0x200u) & ~15u;
    wr32(cpu->_esp, 0xDEAD0000u); /* return address (not used) */
    for (i = 0; i < a->n; i++) wr32(cpu->_esp + 4u + 4u * (uint32_t)i, a->w[i]);
    fn(cpu);
    r = cpu->_eax;
    cpu->_esp = saved;
    return r;
}

void check_str(const char *what, uint32_t g, const char *want)
{
    const char *got = G2H(g);
    checks++;
    if (strcmp(got, want) != 0)
    {
        failures++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

void check_u32(const char *what, uint32_t got, uint32_t want)
{
    checks++;
    if (got != want)
    {
        failures++;
        printf("FAIL %s: got %d (0x%x), want %d (0x%x)\n", what, (int32_t)got, got, (int32_t)want, want);
    }
}

void check_bytes(const char *what, const void *got, size_t gn, const void *want, size_t wn)
{
    checks++;
    if (gn != wn || memcmp(got, want, wn) != 0)
    {
        failures++;
        printf("FAIL %s: got %zu bytes, want %zu bytes\n", what, gn, wn);
    }
}


uint32_t call1(void (*fn)(_cpu *), uint32_t x)
{
    argv_t a = { .n = 0 };
    a32(&a, x);
    return callv(fn, &a);
}

uint32_t call2(void (*fn)(_cpu *), uint32_t x, uint32_t y)
{
    argv_t a = { .n = 0 };
    a32(&a, x);
    a32(&a, y);
    return callv(fn, &a);
}

uint32_t call3(void (*fn)(_cpu *), uint32_t x, uint32_t y, uint32_t z)
{
    argv_t a = { .n = 0 };
    a32(&a, x);
    a32(&a, y);
    a32(&a, z);
    return callv(fn, &a);
}

uint32_t call4(void (*fn)(_cpu *), uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    argv_t a = { .n = 0 };
    a32(&a, x);
    a32(&a, y);
    a32(&a, z);
    a32(&a, w);
    return callv(fn, &a);
}

void harness_init(void)
{
    rt_mem_init();
    cpu = rt_thread_init_main()->cpu;
}

int harness_report(void)
{
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
