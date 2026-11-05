/*
 *  rpi_fastmem.h - Fast memmove/memset for the ARM1176 (unaligned word
 *                  accesses are allowed on ARMv6 normal memory)
 *
 *  Used by the native QuickDraw acceleration (gfxaccel.cpp), where rows in
 *  the 8 bit frame buffer are rarely 4 byte aligned and Circle's memcpy
 *  falls back to byte copies.
 */

#ifndef RPI_FASTMEM_H
#define RPI_FASTMEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *rpi_memmove(void *dest, const void *src, size_t n);
void *rpi_memset(void *dest, int c, size_t n);

#ifdef __cplusplus
}
#endif

#endif
