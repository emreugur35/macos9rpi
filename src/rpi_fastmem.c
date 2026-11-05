/*
 *  rpi_fastmem.c - Fast memmove/memset for the ARM1176 (see rpi_fastmem.h)
 *
 *  The destination is aligned first (stores to the frame buffer matter
 *  most), source words are then loaded with unaligned LDRs which ARMv6
 *  handles in hardware.
 */

#include <stdint.h>
#include <string.h>
#include "rpi_fastmem.h"

static inline uint32_t load32(const uint8_t *p)
{
	uint32_t v;
	__builtin_memcpy(&v, p, 4);
	return v;
}

void *rpi_memmove(void *dest, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dest;
	const uint8_t *s = (const uint8_t *)src;

	if (d == s || n == 0)
		return dest;

	if (d < s || d >= s + n) {
		// forward copy
		while (n && ((uintptr_t)d & 3)) {
			*d++ = *s++;
			n--;
		}
		uint32_t *dw = (uint32_t *)d;
		while (n >= 16) {
			uint32_t a = load32(s), b = load32(s + 4), c = load32(s + 8), e = load32(s + 12);
			dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
			dw += 4; s += 16; n -= 16;
		}
		while (n >= 4) {
			*dw++ = load32(s);
			s += 4; n -= 4;
		}
		d = (uint8_t *)dw;
		while (n--)
			*d++ = *s++;
	} else {
		// backward copy (overlapping, dest above source)
		d += n;
		s += n;
		while (n && ((uintptr_t)d & 3)) {
			*--d = *--s;
			n--;
		}
		uint32_t *dw = (uint32_t *)d;
		while (n >= 16) {
			s -= 16;
			uint32_t a = load32(s), b = load32(s + 4), c = load32(s + 8), e = load32(s + 12);
			dw -= 4;
			dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
			n -= 16;
		}
		while (n >= 4) {
			s -= 4;
			*--dw = load32(s);
			n -= 4;
		}
		d = (uint8_t *)dw;
		while (n--)
			*--d = *--s;
	}
	return dest;
}

void *rpi_memset(void *dest, int c, size_t n)
{
	uint8_t *d = (uint8_t *)dest;
	uint8_t b = (uint8_t)c;
	while (n && ((uintptr_t)d & 3)) {
		*d++ = b;
		n--;
	}
	uint32_t w = b * 0x01010101u;
	uint32_t *dw = (uint32_t *)d;
	while (n >= 16) {
		dw[0] = w; dw[1] = w; dw[2] = w; dw[3] = w;
		dw += 4;
		n -= 16;
	}
	while (n >= 4) {
		*dw++ = w;
		n -= 4;
	}
	d = (uint8_t *)dw;
	while (n--)
		*d++ = b;
	return dest;
}
