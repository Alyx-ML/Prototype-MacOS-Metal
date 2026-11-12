/* Shared parts of the runtime unit tests (tests/harness.c). */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include "rt.h"
#include "rt_module.h"
#include "rt_thread.h"

#include <stdbool.h>

extern _cpu *cpu;
extern int failures, checks;

/* Guest copies of a string (single-byte and UTF-16) and a zeroed buffer. */
uint32_t gs(const char *s);
uint32_t gws(const char *s);
uint32_t gbuf(uint32_t n);

/* Argument words: a 32-bit value, the halves of a double or of a 64-bit
   integer. */
typedef struct { uint32_t w[24]; int n; } argv_t;
void a32(argv_t *a, uint32_t v);
void af(argv_t *a, double d);
void a64(argv_t *a, uint64_t v);

/* Call an import function with the arguments on the guest stack; returns
   eax. */
uint32_t callv(void (*fn)(_cpu *), const argv_t *a);
uint32_t call1(void (*fn)(_cpu *), uint32_t x);
uint32_t call2(void (*fn)(_cpu *), uint32_t x, uint32_t y);
uint32_t call3(void (*fn)(_cpu *), uint32_t x, uint32_t y, uint32_t z);
uint32_t call4(void (*fn)(_cpu *), uint32_t x, uint32_t y, uint32_t z, uint32_t w);

void check_str(const char *what, uint32_t g, const char *want);
void check_u32(const char *what, uint32_t got, uint32_t want);
void check_bytes(const char *what, const void *got, size_t gn, const void *want, size_t wn);

/* Set up the guest memory and the main thread state. */
void harness_init(void);
/* Print the totals; returns the process exit status. */
int harness_report(void);

#endif
