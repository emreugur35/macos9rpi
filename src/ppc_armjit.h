/*
 *  ppc_armjit.h - PowerPC to ARMv6 dynamic translator for macos9rpi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef PPC_ARMJIT_H
#define PPC_ARMJIT_H

class powerpc_cpu;
struct powerpc_registers;

class ppc_armjit
{
public:
	// Decoded instruction (from the interpreter's decoder)
	struct insn_info {
		uint32 mnemo;			// PPC_I(...)
		bool end_block;			// ends a basic block (branch, trap, ...)
		void *handler;			// interpreter handler: void (*)(powerpc_cpu *, uint32)
		const char *name;
	};
	static void decode(powerpc_cpu *cpu, uint32 op, insn_info &info);
	static powerpc_registers *registers(powerpc_cpu *cpu);

	// Translator in use for the main (outermost) execution loop?
	static bool active;

	// Allocate the code buffer (call early: it must be close to the kernel code)
	static bool reserve(uint32 size);

	// A CPU was created / destroyed. profile: count executed instructions
	static void attach(powerpc_cpu *cpu, bool enable, bool profile = false, bool sample = false);
	static void detach(void);

	// Execution loop for powerpc_cpu::execute(). Returns true when execute()
	// should return, false if the interpreter must take over (the
	// translation cache can't be flushed while translated code is active
	// further up the call stack).
	static bool run(powerpc_cpu *cpu, bool &invalidated_cache);

	// Code was modified
	static void invalidate_range(uint32 start, uint32 end);
	static void invalidate_all(void);

	// Compare translated code against the interpreter with random inputs.
	// Returns the number of mismatches (0 = OK).
	static int selftest(powerpc_cpu *cpu, uint32 scratch, int iterations);

	// Statistics for the log
	static void log_stats(void);

	// Sampling profiler (jitprofile): called at 60 Hz in IRQ context with
	// the interrupted ARM pc
	static void sample(uint32 arm_pc);
};

#endif /* PPC_ARMJIT_H */
