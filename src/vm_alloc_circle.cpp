/*
 *  vm_alloc_circle.cpp - Memory allocation helpers for the bare-metal port
 */

#include "sysdeps.h"
#include "vm_alloc.h"
#include <stdlib.h>
#include <string.h>

int vm_init(void)
{
	return 0;
}

void vm_exit(void)
{
}

void *vm_acquire(size_t size, int options)
{
	size = (size + 4095) & ~4095;
	void *p = malloc(size);	// Circle's heap returns cache line aligned blocks
	if (p == NULL)
		return VM_MAP_FAILED;
	memset(p, 0, size);
	return p;
}

int vm_acquire_fixed(void *addr, size_t size, int options)
{
	// Fixed Mac areas are mapped by main_circle.cpp
	return -1;
}

int vm_release(void *addr, size_t size)
{
	if (addr != VM_MAP_FAILED && addr != NULL)
		free(addr);
	return 0;
}

int vm_protect(void *addr, size_t size, int prot)
{
	return 0;
}

int vm_get_page_size(void)
{
	return 4096;
}
