/*
 * C runtime streams (FILE) of the MSVCR80 implementation.
 *
 * A guest FILE pointer is the guest address of a 32-byte struct _iobuf.
 * The runtime keeps the stream state in a host table. The _file field
 * (offset 16) of the guest structure holds the table index.
 */
#ifndef RT_CRT_FILE_H
#define RT_CRT_FILE_H

#include <stdint.h>

/* Lock and unlock a stream for a sequence of operations (recursive). */
void rt_crt_file_lock(uint32_t file);
void rt_crt_file_unlock(uint32_t file);
/* Write one character with the translation rules of the stream.
   Returns 0, or -1 after an error (the error flag of the stream is set). */
int rt_crt_file_putc(uint32_t file, uint8_t c);

#endif
