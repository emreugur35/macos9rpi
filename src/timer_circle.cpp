/*
 *  timer_circle.cpp - Time Manager emulation, bare-metal Circle specific stuff
 *
 *  Based on BasiliskII/src/Unix/timer_unix.cpp
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  tm_time_t is a signed 64 bit count of microseconds since boot.
 */

#include "sysdeps.h"
#include "macos_util.h"
#include "timer.h"
#include "rpi_glue.h"

#define DEBUG 0
#include "debug.h"


/*
 *  Return microseconds since boot (64 bit)
 */

void Microseconds(uint32 &hi, uint32 &lo)
{
	uint64 tl = rpi_ticks_usec();
	hi = tl >> 32;
	lo = tl;
}


/*
 *  Return local date/time in Mac format (seconds since 1.1.1904)
 */

uint32 TimerDateTime(void)
{
	// Mac epoch is 1.1.1904, Unix epoch 1.1.1970 (UTC, we have no time zone)
	return rpi_unix_time() + 2082844800u;
}


/*
 *  Get current time
 */

void timer_current_time(tm_time_t &t)
{
	t = (tm_time_t)rpi_ticks_usec();
}


/*
 *  Add times
 */

void timer_add_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res = a + b;
}


/*
 *  Subtract times
 */

void timer_sub_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res = a - b;
}


/*
 *  Compare times (<0: a < b, =0: a = b, >0: a > b)
 */

int timer_cmp_time(tm_time_t a, tm_time_t b)
{
	tm_time_t r = a - b;
	return r < 0 ? -1 : (r > 0 ? 1 : 0);
}


/*
 *  Convert Mac time value (>0: milliseconds, <0: microseconds) to tm_time_t
 */

void timer_mac2host_time(tm_time_t &res, int32 mactime)
{
	if (mactime > 0)
		res = (tm_time_t)mactime * 1000;	// Time in milliseconds
	else
		res = -(tm_time_t)mactime;		// Time in negative microseconds
}


/*
 *  Convert positive tm_time_t to Mac time value (>0: milliseconds, <0: microseconds)
 *  A negative input value for hosttime results in a zero return value
 *  As long as the microseconds value fits in 32 bit, it must not be converted to milliseconds!
 */

int32 timer_host2mac_time(tm_time_t hosttime)
{
	if (hosttime < 0)
		return 0;
	uint64 t = hosttime;
	if (t > 0x7fffffff)
		return t / 1000;	// Time in milliseconds
	else
		return -(int32)t;	// Time in negative microseconds
}


/*
 *  Get current value of microsecond timer
 */

uint64 GetTicks_usec(void)
{
	return rpi_ticks_usec();
}


/*
 *  Delay by specified number of microseconds (<1 second)
 */

void Delay_usec(uint64 usec)
{
	rpi_delay_usec(usec);
}


/*
 *  Suspend emulator thread, virtual CPU in idle mode
 *
 *  Mac OS calls this when it has nothing to do. We wait for the next
 *  interrupt (60Hz tick, USB) which saves power and lets the Pi run cooler.
 */

extern void EtherCirclePoll(void);

void idle_wait(void)
{
	asm volatile ("mcr p15, 0, %0, c7, c0, 4" : : "r" (0));	// ARMv6 wait for interrupt

	// Woken up, maybe by the network: deliver frames without waiting for the
	// next 60 Hz tick (keeps network latency low while Mac OS waits for data)
	EtherCirclePoll();
}


/*
 *  Resume execution of emulator thread, events just arrived
 */

void idle_resume(void)
{
}
