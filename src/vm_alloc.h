/*
 *  vm_alloc.h - Memory allocation helpers for the bare-metal Circle port
 *               (replaces BasiliskII/src/CrossPlatform/vm_alloc.h)
 *
 *  There is no virtual memory manager: Mac visible memory is mapped
 *  explicitly in main_circle.cpp. vm_acquire() simply returns page
 *  aligned heap memory, which is Mac addressable through Host2MacAddr().
 */

#ifndef VM_ALLOC_H
#define VM_ALLOC_H

#include <stddef.h>

#define VM_MAP_FAILED			((void *)-1)

#define VM_MAP_SHARED			0x01
#define VM_MAP_PRIVATE			0x02
#define VM_MAP_FIXED			0x04
#define VM_MAP_32BIT			0x08
#define VM_MAP_WRITE_WATCH		0x10
#define VM_MAP_DEFAULT			(VM_MAP_PRIVATE)

#define VM_PAGE_NOACCESS		0x0
#define VM_PAGE_READ			0x1
#define VM_PAGE_WRITE			0x2
#define VM_PAGE_EXECUTE			0x4
#define VM_PAGE_DEFAULT			(VM_PAGE_READ | VM_PAGE_WRITE)

extern int vm_init(void);
extern void vm_exit(void);
extern void * vm_acquire(size_t size, int options = VM_MAP_DEFAULT);
extern int vm_acquire_fixed(void * addr, size_t size, int options = VM_MAP_DEFAULT);
extern int vm_release(void * addr, size_t size);
extern int vm_protect(void * addr, size_t size, int prot);
extern int vm_get_page_size(void);

#endif /* VM_ALLOC_H */
