/*
 *  sysdeps.h - System dependent definitions for the bare-metal
 *              Raspberry Pi (Circle) port of SheepShaver
 *
 *  Based on SheepShaver/src/Unix/sysdeps.h
 *  SheepShaver (C) 1997-2008 Christian Bauer and Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef SYSDEPS_H
#define SYSDEPS_H

#include "config.h"
#include "user_strings_circle.h"

#include <sys/types.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

// Define for external components
#define SHEEPSHAVER 1

// Direct Addressing: host = mac + NATMEM_OFFSET (32-bit wrap-around)
#define DIRECT_ADDRESSING 1

// Always use the complete non-stubs Ethernet driver
#define USE_ETHER_FULL_DRIVER 1

#define POWERPC_ROM 1

// Mac ROM is not write protected by the emulator itself; we map it
// read-only in the ARM MMU and skip faulting writes in our abort handler.
#define ROM_IS_WRITE_PROTECTED 0
#define USE_SCRATCHMEM_SUBTERFUGE 1

// Configure PowerPC emulator
#define PPC_REENTRANT_JIT 0
#define PPC_CHECK_INTERRUPTS 1
#define PPC_DECODE_CACHE 1
#define PPC_FLIGHT_RECORDER 0
#define PPC_PROFILE_COMPILE_TIME 0
#define PPC_PROFILE_GENERIC_CALLS 0
#define PPC_PROFILE_REGS_USE 0
#define KPX_MAX_CPUS 1

// Data types
typedef unsigned char uint8;
typedef signed char int8;
typedef unsigned short uint16;
typedef short int16;
typedef unsigned int uint32;
typedef int int32;
typedef unsigned long long uint64;
typedef long long int64;
#define VAL64(a) (a ## LL)
#define UVAL64(a) (a ## uLL)
typedef uint32 uintptr;
typedef int32 intptr;
// 64 bit file offsets (newlib's off_t is only 32 bit; Mac disks can be > 4 GB)
typedef int64 loff_t;


/**
 *		Helper functions to byteswap data
 **/

#define bswap_16(x) ((uint16)__builtin_bswap16((uint16)(x)))
#define bswap_32(x) ((uint32)__builtin_bswap32((uint32)(x)))
#define bswap_64(x) ((uint64)__builtin_bswap64((uint64)(x)))

static inline uint16 generic_bswap_16(uint16 x) { return bswap_16(x); }
static inline uint32 generic_bswap_32(uint32 x) { return bswap_32(x); }
static inline uint64 generic_bswap_64(uint64 x) { return bswap_64(x); }

static inline uint16 tswap16(uint16 x) { return bswap_16(x); }
static inline uint32 tswap32(uint32 x) { return bswap_32(x); }
static inline uint64 tswap64(uint64 x) { return bswap_64(x); }

// ntohl & friends (we have no network stack)
#ifndef ntohl
#define ntohl(x) bswap_32(x)
#define htonl(x) bswap_32(x)
#define ntohs(x) bswap_16(x)
#define htons(x) bswap_16(x)
#endif


/**
 *		Optimized big-endian memory accessors for the ARM host.
 *
 *  ARMv6+ supports unaligned LDR/STR/LDRH/STRH (SCTLR.U=1, set by Circle),
 *  but not unaligned LDRD/LDM. Going through memcpy() lets GCC pick the
 *  right instructions; the byte swap becomes a single REV.
 **/

#define VM_OPTIMIZED_MEMORY_ACCESS_2
#define VM_OPTIMIZED_MEMORY_ACCESS_4
#define VM_OPTIMIZED_MEMORY_ACCESS_8
#define VM_OPTIMIZED_MEMORY_ACCESS_2_REVERSED
#define VM_OPTIMIZED_MEMORY_ACCESS_4_REVERSED

static inline uint32 vm_do_read_memory_2(uint16 *a)
{
	uint16 v; __builtin_memcpy(&v, a, 2); return bswap_16(v);
}
static inline void vm_do_write_memory_2(uint16 *a, uint32 v)
{
	uint16 w = bswap_16(v); __builtin_memcpy(a, &w, 2);
}
static inline uint32 vm_do_read_memory_4(uint32 *a)
{
	uint32 v; __builtin_memcpy(&v, a, 4); return bswap_32(v);
}
static inline void vm_do_write_memory_4(uint32 *a, uint32 v)
{
	uint32 w = bswap_32(v); __builtin_memcpy(a, &w, 4);
}
static inline uint64 vm_do_read_memory_8(uint64 *a)
{
	uint32 hi, lo;
	__builtin_memcpy(&hi, a, 4);
	__builtin_memcpy(&lo, (uint8 *)a + 4, 4);
	return ((uint64)bswap_32(hi) << 32) | bswap_32(lo);
}
static inline void vm_do_write_memory_8(uint64 *a, uint64 v)
{
	uint32 hi = bswap_32((uint32)(v >> 32)), lo = bswap_32((uint32)v);
	__builtin_memcpy(a, &hi, 4);
	__builtin_memcpy((uint8 *)a + 4, &lo, 4);
}
static inline uint32 vm_do_read_memory_2_reversed(uint16 *a)
{
	uint16 v; __builtin_memcpy(&v, a, 2); return v;
}
static inline void vm_do_write_memory_2_reversed(uint16 *a, uint32 v)
{
	uint16 w = v; __builtin_memcpy(a, &w, 2);
}
static inline uint32 vm_do_read_memory_4_reversed(uint32 *a)
{
	uint32 v; __builtin_memcpy(&v, a, 4); return v;
}
static inline void vm_do_write_memory_4_reversed(uint32 *a, uint32 v)
{
	__builtin_memcpy(a, &v, 4);
}


/**
 *		Locks
 *
 *  Everything SheepShaver does runs on one core in one thread; only the
 *  60Hz tick and USB input run in IRQ context and they use the atomic
 *  helpers below. Spin locks are therefore no-ops.
 **/

typedef volatile int spinlock_t;
static const spinlock_t SPIN_LOCK_UNLOCKED = 0;
static inline void spin_lock(spinlock_t *lock) { (void)lock; }
static inline void spin_unlock(spinlock_t *lock) { (void)lock; }
static inline int spin_trylock(spinlock_t *lock) { (void)lock; return 1; }

// IRQ-safe atomic helpers (LDREX/STREX on ARMv6+)
static inline uint32 atomic_or_u32(volatile uint32 *p, uint32 v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
static inline uint32 atomic_and_u32(volatile uint32 *p, uint32 v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }

// Time data type for Time Manager emulation: microseconds since boot
typedef int64 tm_time_t;

// Precise Time Manager: a hardware timer interrupt fires at the next task's
// wakeup time (timer.cpp PRECISE_TIMING_CIRCLE, rpi_kernel.cpp)
#define PRECISE_TIMING 1
#define PRECISE_TIMING_CIRCLE 1

/* Define codes for all the float formats that we know of.
 * Though we only handle IEEE format.  */
#define UNKNOWN_FLOAT_FORMAT 0
#define IEEE_FLOAT_FORMAT 1
#define VAX_FLOAT_FORMAT 2
#define IBM_FLOAT_FORMAT 3
#define C4X_FLOAT_FORMAT 4

// Timing functions
extern uint64 GetTicks_usec(void);
extern void Delay_usec(uint64 usec);

// Various definitions
typedef struct rgb_color {
	uint8		red;
	uint8		green;
	uint8		blue;
	uint8		alpha;
} rgb_color;

// Macro for calling MacOS routines
#define CallMacOS(type, tvect) call_macos((uintptr)tvect)
#define CallMacOS1(type, tvect, arg1) call_macos1((uintptr)tvect, (uintptr)arg1)
#define CallMacOS2(type, tvect, arg1, arg2) call_macos2((uintptr)tvect, (uintptr)arg1, (uintptr)arg2)
#define CallMacOS3(type, tvect, arg1, arg2, arg3) call_macos3((uintptr)tvect, (uintptr)arg1, (uintptr)arg2, (uintptr)arg3)
#define CallMacOS4(type, tvect, arg1, arg2, arg3, arg4) call_macos4((uintptr)tvect, (uintptr)arg1, (uintptr)arg2, (uintptr)arg3, (uintptr)arg4)
#define CallMacOS5(type, tvect, arg1, arg2, arg3, arg4, arg5) call_macos5((uintptr)tvect, (uintptr)arg1, (uintptr)arg2, (uintptr)arg3, (uintptr)arg4, (uintptr)arg5)
#define CallMacOS6(type, tvect, arg1, arg2, arg3, arg4, arg5, arg6) call_macos6((uintptr)tvect, (uintptr)arg1, (uintptr)arg2, (uintptr)arg3, (uintptr)arg4, (uintptr)arg5, (uintptr)arg6)
#define CallMacOS7(type, tvect, arg1, arg2, arg3, arg4, arg5, arg6, arg7) call_macos7((uintptr)tvect, (uintptr)arg1, (uintptr)arg2, (uintptr)arg3, (uintptr)arg4, (uintptr)arg5, (uintptr)arg6, (uintptr)arg7)

#ifdef __cplusplus
extern "C" {
#endif
extern uint32 call_macos(uint32 tvect);
extern uint32 call_macos1(uint32 tvect, uint32 arg1);
extern uint32 call_macos2(uint32 tvect, uint32 arg1, uint32 arg2);
extern uint32 call_macos3(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3);
extern uint32 call_macos4(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4);
extern uint32 call_macos5(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5);
extern uint32 call_macos6(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6);
extern uint32 call_macos7(uint32 tvect, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6, uint32 arg7);
#ifdef __cplusplus
}
#endif

// Platform hooks implemented in main_circle.cpp / input_circle.cpp
#ifdef __cplusplus
extern "C" {
#endif
extern void CirclePollEvents(void);		// called from HandleInterrupt() in main context
#ifdef __cplusplus
}
#endif

#endif
