/* Shared declarations of the MSVCR80 implementation. */
#ifndef RT_CRT_H
#define RT_CRT_H

#include "rt.h"
#include "rt_module.h"

#include <stdbool.h>

/* errno of the calling thread (guest variable). */
void rt_crt_set_errno(uint32_t e);
/* The default invalid parameter handler: ends the process. */
_Noreturn void rt_crt_invalid_parameter(const char *function);
/* C runtime locks (_lock numbers). */
void rt_crt_lock(uint32_t n);
void rt_crt_unlock(uint32_t n);
/* The __setusermatherr handler, 0 if none. */
uint32_t rt_crt_matherr_handler(void);
/* 1 console, 2 GUI (__set_app_type). */
uint32_t rt_crt_app_type(void);
/* The default file translation mode (_fmode): 0 text, 0x8000 binary. */
uint32_t rt_crt_fmode(void);
/* The command line (_acmdln). */
void rt_crt_set_command_line(const char *line);
/* Parse a floating point number with the grammar of the Microsoft runtime. */
double rt_crt_parse_double(const char *s, const char **end, bool *range_error);

/* Undecorate a type descriptor name (".?AV...") into the type_info::name
   form. Returns 0, or -1 with *error set (rt_undname.c). */
int rt_undname_type(const char *decorated, char *out, size_t n, const char **error);
#endif
