/*
 *  ppc_selftest.cpp - Quick self test and speed measurement of the PowerPC
 *                     interpreter on the ARM host (runs before Mac OS starts)
 *
 *  Exercises integer arithmetic, branches, big-endian loads/stores (also
 *  unaligned ones), byte reversed loads, load/store multiple and floating
 *  point - the parts where an ARM32 host differs from the usual x86 hosts.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "rpi_glue.h"
#include "prefs.h"
#include "ppc_armjit.h"

extern void init_emul_ppc(void);
extern void exit_emul_ppc(void);
extern int PPCJitSelfTest(uint32 scratch, int iterations);

// Mini PowerPC assembler
static inline uint32 D(int op, int rt, int ra, int d) { return (op << 26) | (rt << 21) | (ra << 16) | (d & 0xffff); }
static inline uint32 X(int rt, int ra, int rb, int xo) { return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1); }
static inline uint32 li(int rt, int v)				{ return D(14, rt, 0, v); }
static inline uint32 addi(int rt, int ra, int v)	{ return D(14, rt, ra, v); }
static inline uint32 lis(int rt, int v)				{ return D(15, rt, 0, v); }
static inline uint32 ori(int ra, int rs, int v)		{ return D(24, rs, ra, v); }
static inline uint32 add(int rt, int ra, int rb)	{ return X(rt, ra, rb, 266); }
static inline uint32 mullw(int rt, int ra, int rb)	{ return X(rt, ra, rb, 235); }
static inline uint32 mulhwu(int rt, int ra, int rb)	{ return X(rt, ra, rb, 11); }
static inline uint32 divw(int rt, int ra, int rb)	{ return X(rt, ra, rb, 491); }
static inline uint32 cntlzw(int ra, int rs)			{ return X(rs, ra, 0, 26); }
static inline uint32 lwbrx(int rt, int ra, int rb)	{ return X(rt, ra, rb, 534); }
static inline uint32 rlwinm(int ra, int rs, int sh, int mb, int me) { return (21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1); }
static inline uint32 mtctr(int rs)					{ return (31 << 26) | (rs << 21) | (9 << 16) | (467 << 1); }
static inline uint32 bdnz(int disp)					{ return (16 << 26) | (16 << 21) | (disp & 0xfffc); }
static inline uint32 stw(int rs, int d, int ra)		{ return D(36, rs, ra, d); }
static inline uint32 lwz(int rt, int d, int ra)		{ return D(32, rt, ra, d); }
static inline uint32 sth(int rs, int d, int ra)		{ return D(44, rs, ra, d); }
static inline uint32 lhz(int rt, int d, int ra)		{ return D(40, rt, ra, d); }
static inline uint32 lbz(int rt, int d, int ra)		{ return D(34, rt, ra, d); }
static inline uint32 stmw(int rs, int d, int ra)	{ return D(47, rs, ra, d); }
static inline uint32 lmw(int rt, int d, int ra)		{ return D(46, rt, ra, d); }
static inline uint32 lfd(int ft, int d, int ra)		{ return D(50, ft, ra, d); }
static inline uint32 stfd(int fs, int d, int ra)	{ return D(54, fs, ra, d); }
static inline uint32 fmul(int ft, int fa, int fc)	{ return (63 << 26) | (ft << 21) | (fa << 16) | (fc << 6) | (25 << 1); }
static inline uint32 fadd(int ft, int fa, int fb)	{ return (63 << 26) | (ft << 21) | (fa << 16) | (fb << 11) | (21 << 1); }
static inline uint32 fdiv(int ft, int fa, int fb)	{ return (63 << 26) | (ft << 21) | (fa << 16) | (fb << 11) | (18 << 1); }
static inline uint32 fctiwz(int ft, int fb)			{ return (63 << 26) | (ft << 21) | (fb << 11) | (15 << 1); }
static const uint32 BLR = 0x4e800020;

static uint64 double_bits(double d)
{
	uint64 v;
	memcpy(&v, &d, 8);
	return v;
}

/*
 *  Run the test program and the benchmark on a fresh CPU (interpreter or
 *  translator, depending on the "jit" preference)
 */

static bool run_program(uint32 data, uint32 tvect, const char *mode)
{
	init_emul_ppc();
	Mac_memset(data, 0, 0x100);
	WriteMacInt64(data + 0x40, double_bits(1.5));
	WriteMacInt64(data + 0x48, double_bits(2.25));
	WriteMacInt32(data + 0x60, 0x11223344);
	uint32 ret = call_macos1(tvect, data);

	struct { uint32 off; uint32 expected; const char *what; } checks[] = {
		{0x00, 500500, "loop"},
		{0x0c, 500500, "unaligned word"},
		{0x14, 0x12, "byte order"},
		{0x18, 0x1234, "unaligned halfword"},
		{0x1c, 0x44332211, "lwbrx"},
		{0x20, (uint32)(0x12345678u * 1000u), "mullw"},
		{0x24, (uint32)(((uint64)0x12345678u * 1000u) >> 32), "mulhwu"},
		{0x28, 0x12345678 / 1000, "divw"},
		{0x2c, 0x34567812, "rlwinm"},
		{0x30, 27, "cntlzw"},
		{0x34, 0x29 + 0x30 + 0x31, "lmw/stmw"},
	};
	bool ok = (ret == 0x55);
	if (!ok)
		rpi_log("  self test: wrong return value %08x\n", ret);
	for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		uint32 v = ReadMacInt32(data + checks[i].off);
		if (v != checks[i].expected) {
			rpi_log("  self test: %s failed: %08x != %08x\n", checks[i].what, v, checks[i].expected);
			ok = false;
		}
	}
	struct { uint32 off; double expected; const char *what; } fchecks[] = {
		{0x50, 3.375, "fmul"},
		{0x58, 4.875, "fdiv/fadd"},
		{0x78, 3.375, "unaligned lfd/stfd"},
	};
	for (unsigned i = 0; i < sizeof(fchecks) / sizeof(fchecks[0]); i++) {
		uint64 v = ReadMacInt64(data + fchecks[i].off);
		if (v != double_bits(fchecks[i].expected)) {
			rpi_log("  self test: %s failed: %08x%08x\n", fchecks[i].what, (uint32)(v >> 32), (uint32)v);
			ok = false;
		}
	}
	if ((ReadMacInt64(data + 0x68) & 0xffffffff) != 4) {
		rpi_log("  self test: fctiwz failed\n");
		ok = false;
	}

	// Speed
	uint64 t0 = rpi_ticks_usec();
	call_macos(tvect + 8);
	uint64 t1 = rpi_ticks_usec();
	uint32 usec = (uint32)(t1 - t0);
	if (usec == 0)
		usec = 1;
	uint32 kips = (uint32)((uint64)(4 * 1048576 + 5) * 1000 / usec);
	t0 = rpi_ticks_usec();
	call_macos(tvect + 16);
	t1 = rpi_ticks_usec();
	usec = (uint32)(t1 - t0);
	if (usec == 0)
		usec = 1;
	uint32 kips2 = (uint32)((uint64)(4 * 262144 + 8) * 1000 / usec);
	rpi_log("PowerPC self test (%s) %s, speed ~%u.%u MIPS (simple loop), ~%u.%u MIPS (calls)\n",
			mode, ok ? "passed" : "FAILED", kips / 1000, (kips % 1000) / 100, kips2 / 1000, (kips2 % 1000) / 100);
	ppc_armjit::log_stats();

	exit_emul_ppc();
	return ok;
}


/*
 *  Run the self test. code/data: Mac addresses of scratch memory in Mac RAM.
 *  Returns true if all results are correct.
 */

bool PPCSelfTest(uint32 scratch)
{
	const uint32 code = scratch;
	const uint32 data = scratch + 0x1000;
	const uint32 tvect = scratch + 0x2000;
	const uint32 bench = scratch + 0x3000;

	Mac_memset(scratch, 0, 0x4000);

	// Input data
	WriteMacInt64(data + 0x40, double_bits(1.5));
	WriteMacInt64(data + 0x48, double_bits(2.25));
	WriteMacInt32(data + 0x60, 0x11223344);

	uint32 p = code;
	auto emit = [&](uint32 insn) { WriteMacInt32(p, insn); p += 4; };

	// r3 = data
	// 1. loop: sum 1..1000
	emit(li(4, 0));
	emit(li(5, 1000));
	emit(mtctr(5));
	emit(li(6, 1));
	emit(add(4, 4, 6));					// loop:
	emit(addi(6, 6, 1));
	emit(bdnz(-8));
	emit(stw(4, 0x00, 3));				// [0x00] = 500500
	// 2. unaligned word store/load
	emit(stw(4, 0x05, 3));				// [0x05..0x08]
	emit(lwz(7, 0x05, 3));
	emit(stw(7, 0x0c, 3));				// [0x0c] = 500500
	// 3. halfword / byte order
	emit(li(8, 0x1234));
	emit(sth(8, 0x11, 3));				// unaligned halfword at 0x11
	emit(lbz(9, 0x11, 3));				// 0x12
	emit(stw(9, 0x14, 3));				// [0x14] = 0x12
	emit(lhz(10, 0x11, 3));
	emit(stw(10, 0x18, 3));				// [0x18] = 0x1234
	// 4. byte reversed load
	emit(li(11, 0x60));
	emit(lwbrx(12, 3, 11));
	emit(stw(12, 0x1c, 3));				// [0x1c] = 0x44332211
	// 5. multiply / divide / rotate / count leading zeros
	emit(lis(13, 0x1234));
	emit(ori(13, 13, 0x5678));			// r13 = 0x12345678
	emit(li(14, 1000));
	emit(mullw(15, 13, 14));
	emit(stw(15, 0x20, 3));				// [0x20] = 0x12345678 * 1000 (low)
	emit(mulhwu(16, 13, 14));
	emit(stw(16, 0x24, 3));				// [0x24] = high word
	emit(divw(17, 13, 14));
	emit(stw(17, 0x28, 3));				// [0x28] = 0x12345678 / 1000
	emit(rlwinm(18, 13, 8, 0, 31));
	emit(stw(18, 0x2c, 3));				// [0x2c] = 0x34567812
	emit(cntlzw(19, 9));
	emit(stw(19, 0x30, 3));				// [0x30] = clz(0x12) = 27
	// 6. load/store multiple (unaligned)
	emit(li(29, 0x29));
	emit(li(30, 0x30));
	emit(li(31, 0x31));
	emit(stmw(29, 0x83, 3));			// 0x83..0x8e
	emit(li(29, 0));
	emit(li(30, 0));
	emit(li(31, 0));
	emit(lmw(29, 0x83, 3));
	emit(add(20, 29, 30));
	emit(add(20, 20, 31));
	emit(stw(20, 0x34, 3));				// [0x34] = 0x8a
	// 7. floating point
	emit(lfd(1, 0x40, 3));				// 1.5
	emit(lfd(2, 0x48, 3));				// 2.25
	emit(fmul(3, 1, 2));				// 3.375
	emit(stfd(3, 0x50, 3));
	emit(fdiv(4, 2, 1));				// 1.5
	emit(fadd(4, 4, 3));				// 4.875
	emit(stfd(4, 0x58, 3));
	emit(fctiwz(5, 4));					// 4
	emit(stfd(5, 0x68, 3));
	emit(stfd(3, 0x71, 3));				// unaligned double store
	emit(lfd(6, 0x71, 3));
	emit(stfd(6, 0x78, 3));
	emit(li(3, 0x55));					// return value
	emit(BLR);

	// Benchmark: 4 instructions per iteration
	p = bench;
	emit(lis(5, 0x0010));				// 1048576 iterations
	emit(mtctr(5));
	emit(li(4, 0));
	emit(addi(4, 4, 3));				// loop:
	emit(rlwinm(6, 4, 1, 0, 31));
	emit(add(4, 4, 6));
	emit(bdnz(-12));
	emit(li(3, 0));
	emit(BLR);

	// Benchmark 2: function calls (bl/blr), 4 instructions per iteration
	p = bench + 0x100;
	emit(0x7D8802A6);					// mflr r12
	emit(lis(5, 0x0004));				// 262144 iterations
	emit(mtctr(5));
	emit(li(4, 0));
	emit(0x48000001 | 20);				// loop: bl func
	emit(bdnz(-4));
	emit(0x7D8803A6);					// mtlr r12
	emit(li(3, 0));
	emit(BLR);
	emit(addi(4, 4, 1));				// func:
	emit(BLR);

	// Transition vectors (code, TOC)
	WriteMacInt32(tvect, code);
	WriteMacInt32(tvect + 4, 0);
	WriteMacInt32(tvect + 8, bench);
	WriteMacInt32(tvect + 12, 0);
	WriteMacInt32(tvect + 16, bench + 0x100);
	WriteMacInt32(tvect + 20, 0);

	// Interpreter
	const bool want_jit = PrefsFindBool("jit");
	PrefsReplaceBool("jit", false);
	bool ok = run_program(data, tvect, "interpreter");
	if (!want_jit)
		return ok;

	// Translator: compare against the interpreter, then run the program
	PrefsReplaceBool("jit", true);
	init_emul_ppc();
	int failures = PPCJitSelfTest(scratch + 0x4000, PrefsFindInt32("jittest") > 0 ? PrefsFindInt32("jittest") : 20);
	exit_emul_ppc();
	bool jit_ok = failures == 0 && run_program(data, tvect, "JIT");
	if (!jit_ok) {
		rpi_log("JIT disabled: it does not match the interpreter\n");
		PrefsReplaceBool("jit", false);
	}
	return ok;
}

