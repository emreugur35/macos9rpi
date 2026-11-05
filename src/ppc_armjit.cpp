/*
 *  ppc_armjit.cpp - PowerPC to ARMv6 dynamic translator for macos9rpi
 *
 *  Translates basic blocks of PowerPC code into ARM code for the
 *  ARM1176JZF-S of the Raspberry Pi Zero. The design is deliberately
 *  simple:
 *
 *  - The PowerPC registers stay in the powerpc_registers structure in
 *    memory (r11 points to it); every translated instruction loads its
 *    operands and stores its result. Common integer, load/store, compare,
 *    condition register and branch instructions are translated natively,
 *    everything else calls the interpreter's handler for that instruction
 *    (exactly what the decode cache would do), so the translator is never
 *    less complete than the interpreter.
 *  - Blocks end at branches. Branches to constant addresses are linked
 *    directly to the target block once it is known; indirect branches
 *    (blr, bctr) go through a small direct mapped table. Every exit checks
 *    the special CPU flags (interrupts etc.) first.
 *  - Mac memory is at host = mac + 0x80000000 (r10 = 0x80000000), loads
 *    and stores are byte swapped with REV. Accesses to unmapped Mac
 *    addresses fault and are skipped by the data abort handler, as in the
 *    interpreter.
 *  - Any code invalidation that touches a page containing translated code
 *    flushes the whole translation cache (simple and safe).
 *
 *  Register use in translated code:
 *    r0-r3           scratch
 *    r4-r7           register cache (GPRs used within a block)
 *    r12             the condition register (CR)
 *    r8, r9, lr      GPRs 27, 24 and 29, permanently (the ROM's 68k
 *                    emulator keeps its opcode, pc and dispatch address
 *                    there); written back when leaving translated code
 *    r10             0x80000000 (Mac to host address offset)
 *    r11             powerpc_registers *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu/ppc/ppc-cpu.hpp"
#include "cpu/ppc/ppc-instructions.hpp"
#include "rpi_glue.h"
#include "ppc_armjit.h"

#include <string.h>
#include <stddef.h>

#define DEBUG 0
#include "debug.h"


/*
 *  Configuration
 */

#define MAX_BLOCK_INSNS		128			// PowerPC instructions per block
#define MAX_BLOCK_BYTES		3072		// ARM code per block (then split)
#define BLOCK_RESERVE		8192		// free space needed to start a block
#define POOL_DISTANCE		2048		// flush literal pool after this many bytes
#define POOL_MAX			128			// or this many literals
#define HASH_SIZE			65536		// block lookup hash table
#define MAX_BLOCKS			131072
#define JC_BITS				13
#define JC_SIZE				(1 << JC_BITS)	// indirect jump cache
#define MAX_LINKS			262144		// chained exits
#define MAX_INVAL			32			// pending invalidated ranges
#define PAGE_WORDS			(1u << 15)	// bitmap of 4 KB pages with translated code


/*
 *  ARM instruction encoding
 */

enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, RSP, RLR, RPC };
enum { RMEM = R10, RREGS = R11, RCR = R12 };
enum { C_EQ, C_NE, C_CS, C_CC, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE, C_AL };
enum { OP_AND, OP_EOR, OP_SUB, OP_RSB, OP_ADD, OP_ADC, OP_SBC, OP_RSC,
	   OP_TST, OP_TEQ, OP_CMP, OP_CMN, OP_ORR, OP_MOV, OP_BIC, OP_MVN };
enum { SH_LSL, SH_LSR, SH_ASR, SH_ROR };

static uint32 *jp;					// code emission pointer

static inline void emit(uint32 insn) { *jp++ = insn; }

static inline uint32 rotl32(uint32 v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

// Encode a data processing immediate (8 bit value rotated right by an even amount)
static bool imm_ok(uint32 v, uint32 *enc = NULL)
{
	for (int rot = 0; rot < 16; rot++) {
		uint32 r = rotl32(v, 2 * rot);
		if (r < 256) {
			if (enc)
				*enc = (rot << 8) | r;
			return true;
		}
	}
	return false;
}

static inline uint32 imm(uint32 v)
{
	uint32 enc = 0;
	if (!imm_ok(v, &enc))
		rpi_fatal("armjit: bad immediate %08x", v);
	return enc;
}

// Data processing with immediate operand
static inline void dpi(int cond, int op, int s, int rd, int rn, uint32 value)
{
	emit((cond << 28) | (1 << 25) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | imm(value));
}

// Data processing with (shifted) register operand
static inline void dpr(int cond, int op, int s, int rd, int rn, int rm, int sh = SH_LSL, int amt = 0)
{
	amt &= 31;
	if (amt == 0)
		sh = SH_LSL;		// LSR/ASR #0 mean #32, ROR #0 means RRX
	emit((cond << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | (amt << 7) | (sh << 5) | rm);
}

// Data processing with register shifted by register
static inline void dprs(int cond, int op, int s, int rd, int rn, int rm, int sh, int rs)
{
	emit((cond << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | (rs << 8) | (sh << 5) | (1 << 4) | rm);
}

static inline void mov(int rd, int rm, int sh = SH_LSL, int amt = 0) { dpr(C_AL, OP_MOV, 0, rd, 0, rm, sh, amt); }
static inline void cmpi(int rn, uint32 v) { dpi(C_AL, OP_CMP, 1, 0, rn, v); }

// LDR/STR/LDRB/STRB with immediate offset
static inline void ldst(int cond, int load, int byte, int rd, int rn, int off)
{
	int u = 1;
	if (off < 0) {
		u = 0;
		off = -off;
	}
	if (off > 4095)
		rpi_fatal("armjit: bad offset %d", off);
	emit((cond << 28) | (1 << 26) | (1 << 24) | (u << 23) | (byte << 22) | (load << 20) | (rn << 16) | (rd << 12) | off);
}
static inline void ldr(int rd, int rn, int off) { ldst(C_AL, 1, 0, rd, rn, off); }
static inline void str(int rd, int rn, int off) { ldst(C_AL, 0, 0, rd, rn, off); }
static inline void ldrb(int rd, int rn, int off) { ldst(C_AL, 1, 1, rd, rn, off); }
static inline void strb(int rd, int rn, int off) { ldst(C_AL, 0, 1, rd, rn, off); }

// LDR/STR/LDRB/STRB rd, [rn, rm]
static inline void ldst_r(int load, int byte, int rd, int rn, int rm)
{
	emit((C_AL << 28) | (3 << 25) | (1 << 24) | (1 << 23) | (byte << 22) | (load << 20) | (rn << 16) | (rd << 12) | rm);
}

// LDR/STR rd, [rn], #4
static inline void ldst_post4(int load, int rd, int rn)
{
	emit((C_AL << 28) | (1 << 26) | (1 << 23) | (load << 20) | (rn << 16) | (rd << 12) | 4);
}

// LDRH/STRH rd, [rn, rm]
static inline void ldsth_r(int load, int rd, int rn, int rm)
{
	emit((C_AL << 28) | (1 << 24) | (1 << 23) | (load << 20) | (rn << 16) | (rd << 12) | (1 << 7) | (1 << 5) | (1 << 4) | rm);
}

static inline void rev(int rd, int rm)   { emit(0xE6BF0F30 | (rd << 12) | rm); }
static inline void rev16(int rd, int rm) { emit(0xE6BF0FB0 | (rd << 12) | rm); }
static inline void revsh(int rd, int rm) { emit(0xE6FF0FB0 | (rd << 12) | rm); }
static inline void sxtb(int rd, int rm)  { emit(0xE6AF0070 | (rd << 12) | rm); }
static inline void sxth(int rd, int rm)  { emit(0xE6BF0070 | (rd << 12) | rm); }
static inline void clz(int rd, int rm)   { emit(0xE16F0F10 | (rd << 12) | rm); }
static inline void mul(int rd, int rm, int rs) { emit(0xE0000090 | (rd << 16) | (rs << 8) | rm); }
static inline void smull(int lo, int hi, int rm, int rs) { emit(0xE0C00090 | (hi << 16) | (lo << 12) | (rs << 8) | rm); }
static inline void umull(int lo, int hi, int rm, int rs) { emit(0xE0800090 | (hi << 16) | (lo << 12) | (rs << 8) | rm); }

static inline bool branch_ok(uint32 *at, const void *target)
{
	int32 off = (int32)((uintptr)target - ((uintptr)at + 8));
	return off >= -(32 << 20) && off < (32 << 20);
}

static inline uint32 b_insn(int cond, uint32 *at, const void *target, int link = 0)
{
	int32 off = (int32)((uintptr)target - ((uintptr)at + 8)) >> 2;
	return (cond << 28) | ((link ? 0x0B : 0x0A) << 24) | (off & 0x00FFFFFF);
}

static inline void b(int cond, const void *target) { emit(b_insn(cond, jp, target)); }


/*
 *  Literal pool
 */

struct lit_ref {
	uint32 *insn;
	uint32 value;
};
static lit_ref lit_refs[POOL_MAX + 8];
static int lit_count;
static uint32 *lit_first;

// ldr rd, =value
static void ldr_lit(int rd, uint32 value)
{
	if (lit_count == 0)
		lit_first = jp;
	lit_refs[lit_count].insn = jp;
	lit_refs[lit_count].value = value;
	lit_count++;
	emit(0xE59F0000 | (rd << 12));		// ldr rd, [pc, #0] (offset patched later)
}

// ldr<cond> rd, =value
static void ldr_lit_cond(int cond, int rd, uint32 value)
{
	ldr_lit(rd, value);
	jp[-1] = (jp[-1] & 0x0fffffff) | (cond << 28);
}

static void flush_pool(bool branch_over)
{
	if (lit_count == 0)
		return;
	uint32 *br = NULL;
	if (branch_over) {
		br = jp;
		emit(0);
	}
	uint32 *pool = jp;
	int n = 0;
	for (int i = 0; i < lit_count; i++) {
		int k;
		for (k = 0; k < n; k++)
			if (pool[k] == lit_refs[i].value)
				break;
		if (k == n) {
			emit(lit_refs[i].value);
			n++;
		}
		int32 off = (int32)((uintptr)&pool[k] - ((uintptr)lit_refs[i].insn + 8));
		if (off < 0 || off > 4095)
			rpi_fatal("armjit: literal out of range (%d)", off);
		*lit_refs[i].insn |= off;
	}
	if (br)
		*br = b_insn(C_AL, br, jp);
	lit_count = 0;
}

static inline void maybe_flush_pool(void)
{
	if (lit_count && ((jp - lit_first) * 4 > POOL_DISTANCE || lit_count >= POOL_MAX))
		flush_pool(true);
}

static void load_const(int rd, uint32 v)
{
	if (imm_ok(v))
		dpi(C_AL, OP_MOV, 0, rd, 0, v);
	else if (imm_ok(~v))
		dpi(C_AL, OP_MVN, 0, rd, 0, ~v);
	else
		ldr_lit(rd, v);
}

// rd = rn + v (rd may be rn); uses r0 for odd constants
static void add_const(int rd, int rn, int32 v)
{
	if (v == 0) {
		if (rd != rn)
			mov(rd, rn);
		return;
	}
	uint32 a = v < 0 ? -(uint32)v : (uint32)v;
	int op = v < 0 ? OP_SUB : OP_ADD;
	if (imm_ok((uint32)v))
		dpi(C_AL, OP_ADD, 0, rd, rn, (uint32)v);
	else if (imm_ok(-(uint32)v))
		dpi(C_AL, OP_SUB, 0, rd, rn, -(uint32)v);
	else if (a < 0x10000) {
		dpi(C_AL, op, 0, rd, rn, a & 0xff00);
		dpi(C_AL, op, 0, rd, rd, a & 0x00ff);
	}
	else {
		load_const(R0, (uint32)v);
		dpr(C_AL, OP_ADD, 0, rd, rn, R0);
	}
}


/*
 *  Code buffer and caches
 */

struct jit_link;

struct jit_block {
	uint32 pc;				// 1 when the block was invalidated
	uint32 end_pc;			// address of the last instruction
	uint32 *entry;
	jit_block *next;		// hash chain
	jit_link *in_links;		// exits of other blocks chained to this one
};

struct jit_link {
	uint32 *site;			// patched branch
	jit_link *next_in;		// next link to the same target block
};

// Index of blocks by 4 KB page (to find the blocks hit by an invalidation)
struct page_node {
	uint32 page;
	jit_block *b;
	page_node *next;
};
#define PAGE_HASH			16384
#define MAX_PAGE_NODES		(MAX_BLOCKS * 2)
static page_node **page_hash;
static page_node *page_pool;
static int page_count;

struct jc_entry {
	uint32 pc;
	uint32 *entry;
};

typedef uint32 (*enter_fn)(void *regs, void *entry, void *cpu, void *jc);

static uint32 *buf_start, *buf_end;		// code buffer
static uint32 *blocks_start;			// first block (after the stubs)
static uint32 *stub_exit;				// return to the dispatcher, r0 = link site
static uint32 *stub_exit0;				// same with r0 = 0
static uint32 *stub_exit_miss;			// same with r0 = 1 (indirect jump cache miss)
static uint32 *stub_exit_miss_r3;		// regs.pc = r3, then as stub_exit_miss
static uint32 *stub_exit0_r3;			// regs.pc = r3, then as stub_exit0
static enter_fn stub_enter;

static jit_block **blk_hash;			// HASH_SIZE chains
static jit_block *blk_pool;
static int blk_count;
static jit_link *link_pool;
static int link_count;
static uint32 inval_start[MAX_INVAL], inval_end[MAX_INVAL];
static int inval_count;
static jc_entry *jcache;
// Flat table of native entry points for every instruction of the ROM area
// (indirect branches into the ROM, e.g. the 68k emulator's dispatch, need
// no hashing). r8 points to it; the jump cache follows it in memory.
#define ROM_TABLE_BYTES		0x500000	// = ROM_AREA_SIZE
static uint32 **rom_table;
static uint32 rom_base, rom_size;		// rom_size = 0: no table
static uint32 *page_bits;
static bool flush_pending;
static powerpc_cpu *the_cpu;
static int native_depth;				// active calls into translated code

// Range of freshly written code to synchronize with the I-cache
static uintptr dirty_lo = ~(uintptr)0, dirty_hi = 0;

// Statistics
static uint32 st_compiles, st_flushes, st_links, st_dispatch, st_invalidates, st_insns, st_fallbacks;
static uint32 st_jcmiss, st_spcexit, st_linkexit, st_killed;
static uint32 miss_ring[1024];			// recent jump cache miss targets (profiling)
#define ARM_SAMPLES 8192
static uint32 arm_samples[ARM_SAMPLES];	// interrupted ARM pcs (profiling)
static volatile uint32 arm_sample_n;

// Instruction mix profile (jitprofile): executions and fallbacks per mnemonic
#define MAX_MNEMO 512
static bool profiling;
static bool sampling;					// 60 Hz ARM pc samples (jitprofile / jitsample)
static uint32 prof_exec[MAX_MNEMO], prof_fallback[MAX_MNEMO];
static uint32 prof_spr_fb[2][1024];		// mfspr/mtspr fallbacks per SPR
static const char *prof_name[MAX_MNEMO];

// Register structure offsets
static int OFS_CR, OFS_SO, OFS_OV, OFS_CA, OFS_COUNT, OFS_LR, OFS_CTR, OFS_PC, OFS_SPC, OFS_FPR, OFS_RESV, OFS_RESA;

bool ppc_armjit::active = false;

static void mark_dirty(void *lo, void *hi)
{
	if ((uintptr)lo < dirty_lo)
		dirty_lo = (uintptr)lo;
	if ((uintptr)hi > dirty_hi)
		dirty_hi = (uintptr)hi;
}

// Make freshly written code visible to instruction fetches (ARM1176)
static void sync_code(void)
{
	if (dirty_hi <= dirty_lo)
		return;
	if (dirty_hi - dirty_lo > 16384)
		asm volatile ("mcr p15, 0, %0, c7, c10, 0" : : "r" (0) : "memory");	// clean entire D-cache (16 KB)
	else
		for (uintptr a = dirty_lo & ~31u; a < dirty_hi; a += 32)
			asm volatile ("mcr p15, 0, %0, c7, c10, 1" : : "r" (a) : "memory");	// clean D line
	asm volatile ("mcr p15, 0, %0, c7, c10, 4" : : "r" (0) : "memory");		// DSB
	// Invalidate entire I-cache, ARM1176 erratum 411920 workaround
	uint32 flags;
	asm volatile (
		"mrs %0, cpsr\n"
		"cpsid if\n"
		"mcr p15, 0, %1, c7, c5, 0\n"
		"mcr p15, 0, %1, c7, c5, 0\n"
		"mcr p15, 0, %1, c7, c5, 0\n"
		"mcr p15, 0, %1, c7, c5, 0\n"
		"msr cpsr_c, %0\n"
		"nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"
		: "=&r" (flags) : "r" (0) : "memory");
	asm volatile ("mcr p15, 0, %0, c7, c5, 6" : : "r" (0) : "memory");		// flush BTB
	asm volatile ("mcr p15, 0, %0, c7, c10, 4" : : "r" (0) : "memory");		// DSB
	asm volatile ("mcr p15, 0, %0, c7, c5, 4" : : "r" (0) : "memory");		// flush prefetch buffer
	dirty_lo = ~(uintptr)0;
	dirty_hi = 0;
}

static void jc_clear(void)
{
	for (int i = 0; i < JC_SIZE; i++) {
		jcache[i].pc = 1;		// never matches an aligned pc
		jcache[i].entry = stub_exit0;
	}
}

static inline bool in_rom(uint32 pc)
{
	return pc - rom_base < rom_size;
}

static void rom_clear(void)
{
	for (uint32 i = 0; i < ROM_TABLE_BYTES / 4; i++)
		rom_table[i] = stub_exit_miss_r3;
}

static void flush_all(void)
{
	jp = blocks_start;
	blk_count = 0;
	memset(blk_hash, 0, HASH_SIZE * sizeof(jit_block *));
	jc_clear();
	rom_clear();
	memset(page_bits, 0, PAGE_WORDS * 4);
	flush_pending = false;
	inval_count = 0;
	link_count = 0;
	page_count = 0;
	memset(page_hash, 0, PAGE_HASH * sizeof(page_node *));
	lit_count = 0;
	st_flushes++;
}

static inline jit_block *find_block(uint32 pc);

// Kill the translated blocks in the pending invalidated ranges. The code
// stays in the buffer (it may still be running further up the call stack);
// branches into it are unlinked and the lookup tables forget it.
static inline int jc_index(uint32 pc);

// Forget a translated block: unlink the branches into it, remove it from
// the lookup tables. Its code stays in the buffer.
static void kill_block(jit_block *b)
{
	const uint32 pc = b->pc;
	for (jit_block **pp = &blk_hash[(pc >> 2) & (HASH_SIZE - 1)]; *pp; pp = &(*pp)->next) {
		if (*pp == b) {
			*pp = b->next;
			break;
		}
	}
	if (in_rom(pc))
		rom_table[(pc - rom_base) >> 2] = stub_exit_miss_r3;
	else {
		jc_entry *e = &jcache[jc_index(pc)];
		for (int w = 0; w < 2; w++) {
			if (e[w].pc == pc) {
				e[w].pc = 1;
				e[w].entry = stub_exit0;
			}
		}
	}
	for (jit_link *l = b->in_links; l; l = l->next_in) {
		uint32 *site = l->site;
		*site = b_insn(*site >> 28, site, site + 1);
		mark_dirty(site, site + 1);
	}
	b->in_links = NULL;
	b->pc = 1;
	st_killed++;
}

// Kill the translated blocks in the pending invalidated ranges
static void process_invalidations(void)
{
	for (int k = 0; k < inval_count; k++) {
		const uint32 start = inval_start[k], end = inval_end[k];
		for (uint32 p = start >> 12; p <= ((end - 1) >> 12) && p < (PAGE_WORDS << 5); p++) {
			if ((p & 31) == 0 && page_bits[p >> 5] == 0 && p + 31 <= ((end - 1) >> 12)) {
				p += 31;			// 32 pages without code
				continue;
			}
			if (!(page_bits[p >> 5] & (1u << (p & 31))))
				continue;
			for (page_node *n = page_hash[p & (PAGE_HASH - 1)]; n; n = n->next) {
				jit_block *b = n->b;
				if (n->page == p && b->pc != 1 && b->pc < end && b->end_pc >= start)
					kill_block(b);
			}
		}
	}
	inval_count = 0;
}

static inline jit_block *find_block(uint32 pc)
{
	for (jit_block *b = blk_hash[(pc >> 2) & (HASH_SIZE - 1)]; b; b = b->next)
		if (b->pc == pc)
			return b;
	return NULL;
}

// Multiplicative hash: the 68k emulator's dispatch table (8 byte entries
// indexed by the 68k opcode) defeats simple bit selection
#define JC_HASH 0x9E3779B1u
// Two way set associative: entries 2n and 2n+1 form a set
static inline int jc_index(uint32 pc)
{
	return ((pc * JC_HASH) >> (32 - JC_BITS + 1)) << 1;
}

static inline void jc_set(uint32 pc, uint32 *entry)
{
	if (in_rom(pc)) {
		rom_table[(pc - rom_base) >> 2] = entry;
		return;
	}
	jc_entry *e = &jcache[jc_index(pc)];
	if (e[0].pc == pc) {
		e[0].entry = entry;
		return;
	}
	if (e[1].pc != pc)
		e[1] = e[0];		// most recent in way 0, previous in way 1
	e[0].pc = pc;
	e[0].entry = entry;
}

static void mark_pages(uint32 start, uint32 end)
{
	for (uint32 p = start >> 12; p <= (end >> 12); p++)
		page_bits[p >> 5] |= 1u << (p & 31);
}


/*
 *  Translation helpers (PowerPC state access)
 */

static inline void ld_gpr(int rd, int n) { ldr(rd, RREGS, 4 * n); }
static inline void st_gpr(int rs, int n) { str(rs, RREGS, 4 * n); }

// Set CR field crf from the ARM flags of a compare (signed or unsigned)
static void cr_from_flags(int crf, bool sign)
{
	dpi(sign ? C_LT : C_CC, OP_MOV, 0, R1, 0, 8);
	dpi(sign ? C_GT : C_HI, OP_MOV, 0, R1, 0, 4);
	dpi(C_EQ, OP_MOV, 0, R1, 0, 2);
	ldrb(R2, RREGS, OFS_SO);
	dpr(C_AL, OP_ORR, 0, R1, R1, R2);
	const int sh = 28 - 4 * crf;
	dpi(C_AL, OP_BIC, 0, RCR, RCR, 0xfu << sh);
	dpr(C_AL, OP_ORR, 0, RCR, RCR, R1, SH_LSL, sh);
	// ARM flags are preserved: a following bc can use them directly
}

// record_cr0(result in register r)
static inline void record_cr0(int r)
{
	cmpi(r, 0);
	cr_from_flags(0, true);
}

// XER[CA] = ARM carry flag
static inline void store_carry(void)
{
	dpi(C_AL, OP_MOV, 0, R2, 0, 0);
	dpi(C_AL, OP_ADC, 0, R2, R2, 0);
	strb(R2, RREGS, OFS_CA);
}

// ARM carry flag = XER[CA]
static inline void load_carry(void)
{
	ldrb(R2, RREGS, OFS_CA);
	dpr(C_AL, OP_MOV, 1, R2, 0, R2, SH_LSR, 1);
}

// XER[OV] = condition (C_VS: ARM overflow flag), XER[SO] |= XER[OV]
static void store_overflow(int cond)
{
	dpi(cond, OP_MOV, 0, R2, 0, 1);
	dpi(cond ^ 1, OP_MOV, 0, R2, 0, 0);
	strb(R2, RREGS, OFS_OV);
	ldrb(R3, RREGS, OFS_SO);
	dpr(C_AL, OP_ORR, 0, R3, R3, R2);
	strb(R3, RREGS, OFS_SO);
}

static inline uint32 ppc_mask(uint32 mb, uint32 me)
{
	uint32 m = ((uint32)-1 >> mb) ^ ((me >= 31) ? 0 : (uint32)-1 >> (me + 1));
	return (mb > me) ? ~m : m;
}

// rd = rs & mask (rd may be rs, not r1), using r1 if needed
static void and_mask(int rd, int rs, uint32 mask)
{
	if (mask == 0xffffffff) {
		if (rd != rs)
			mov(rd, rs);
	}
	else if (imm_ok(mask))
		dpi(C_AL, OP_AND, 0, rd, rs, mask);
	else if (imm_ok(~mask))
		dpi(C_AL, OP_BIC, 0, rd, rs, ~mask);
	else if ((mask & (mask + 1)) == 0) {			// low bits
		int n = __builtin_clz(mask);
		mov(rd, rs, SH_LSL, n);
		mov(rd, rd, SH_LSR, n);
	}
	else if ((~mask & (~mask + 1)) == 0) {		// high bits
		int n = __builtin_ctz(mask);
		mov(rd, rs, SH_LSR, n);
		mov(rd, rd, SH_LSL, n);
	}
	else {
		load_const(R1, mask);
		dpr(C_AL, OP_AND, 0, rd, rs, R1);
	}
}


// counter++ (keeps r0, r1, the register cache and the flags)
static void emit_count(uint32 *counter)
{
	ldr_lit(R3, (uint32)(uintptr)counter);
	ldr(R2, R3, 0);
	dpi(C_AL, OP_ADD, 0, R2, R2, 1);
	str(R2, R3, 0);
}


/*
 *  Block translator state
 */

struct xlate_state {
	uint32 pc;				// address of the instruction being translated
	uint32 mem_pc;			// value of regs.pc in memory (if known)
	bool mem_pc_valid;
	int flags_crf;			// CR field whose value is in the ARM flags (-1 none)
	bool flags_signed;
	int lr_gpr, ctr_gpr;	// GPR last moved to LR/CTR in this block (-1 none)
	uint32 next_pc;			// continue here (followed branch), 0 = pc + 4
	int follows;			// number of followed branches
	uint32 max_pc;			// highest translated address
};

static xlate_state xs;
static bool xlate_follow = true;	// follow unconditional forward branches
static powerpc_cpu *xlate_cpu;		// CPU the code is translated for

/*
 *  Register cache: PowerPC GPRs live in r4-r7 and lr within a block.
 *  Dirty registers are written back before exits and interpreter calls.
 */

#define RC_N 4
static const int rc_host[RC_N] = { R4, R5, R6, R7 };

// GPRs that always live in host registers inside translated code
#define PIN_N 3
static const int pin_gpr[PIN_N] = { 24, 27, 29 };
static const int pin_host[PIN_N] = { R9, R8, RLR };
static int pin_of[32];					// host register of a pinned GPR, or -1

static void init_pins(void)
{
	for (int i = 0; i < 32; i++)
		pin_of[i] = -1;
	for (int i = 0; i < PIN_N; i++)
		pin_of[pin_gpr[i]] = pin_host[i];
}

// Store / load the pinned GPRs (around interpreter calls, entry and exit)
static void spill_pinned(void)
{
	for (int i = 0; i < PIN_N; i++)
		str(pin_host[i], RREGS, 4 * pin_gpr[i]);
	str(RCR, RREGS, offsetof(powerpc_registers, cr));
}

static void reload_pinned(void)
{
	for (int i = 0; i < PIN_N; i++)
		ldr(pin_host[i], RREGS, 4 * pin_gpr[i]);
	ldr(RCR, RREGS, offsetof(powerpc_registers, cr));
}
static int rc_ppc[RC_N];				// cached GPR (-1 = free)
static bool rc_dirty[RC_N];
static bool rc_pinned[RC_N];			// used by the current instruction
static uint32 rc_age[RC_N], rc_clock;

static void rc_reset(void)
{
	for (int i = 0; i < RC_N; i++) {
		rc_ppc[i] = -1;
		rc_dirty[i] = rc_pinned[i] = false;
	}
}

static inline int rc_slot(int n)
{
	for (int i = 0; i < RC_N; i++)
		if (rc_ppc[i] == n)
			return i;
	return -1;
}

static int rc_alloc(int n)
{
	int s = -1;
	for (int i = 0; i < RC_N; i++) {
		if (rc_ppc[i] < 0) {
			s = i;
			break;
		}
	}
	if (s < 0) {
		for (int i = 0; i < RC_N; i++)
			if (!rc_pinned[i] && (s < 0 || rc_age[i] < rc_age[s]))
				s = i;
		if (s < 0)
			rpi_fatal("armjit: register cache full");
		if (rc_dirty[s])
			st_gpr(rc_host[s], rc_ppc[s]);
	}
	rc_ppc[s] = n;
	rc_dirty[s] = false;
	return s;
}

// Host register holding GPR n (loaded if necessary)
static int rc_use(int n)
{
	if (pin_of[n] >= 0)
		return pin_of[n];
	int s = rc_slot(n);
	if (s < 0) {
		s = rc_alloc(n);
		ld_gpr(rc_host[s], n);
	}
	rc_pinned[s] = true;
	rc_age[s] = ++rc_clock;
	return rc_host[s];
}

// Host register that receives a new value for GPR n
static int rc_def(int n)
{
	if (n == xs.lr_gpr)
		xs.lr_gpr = -1;
	if (n == xs.ctr_gpr)
		xs.ctr_gpr = -1;
	if (pin_of[n] >= 0)
		return pin_of[n];
	int s = rc_slot(n);
	if (s < 0)
		s = rc_alloc(n);
	rc_pinned[s] = true;
	rc_dirty[s] = true;
	rc_age[s] = ++rc_clock;
	return rc_host[s];
}

// Write back dirty registers (they stay cached)
static void rc_flush(void)
{
	for (int i = 0; i < RC_N; i++) {
		if (rc_dirty[i]) {
			st_gpr(rc_host[i], rc_ppc[i]);
			rc_dirty[i] = false;
		}
	}
}

static inline void rc_unpin(void)
{
	for (int i = 0; i < RC_N; i++)
		rc_pinned[i] = false;
}

// Forget cached GPRs >= first (after rc_flush)
static void rc_forget(int first)
{
	for (int i = 0; i < RC_N; i++)
		if (rc_ppc[i] >= first)
			rc_ppc[i] = -1;
}


// Call a function (in range: bl, otherwise via r2)
static void emit_call(const void *fn)
{
	if (branch_ok(jp, fn))
		emit(b_insn(C_AL, jp, fn, 1));
	else {
		load_const(R2, (uint32)(uintptr)fn);
		emit(0xE12FFF32);		// blx r2
	}
}

static inline void sync_pc(void)
{
	if (!xs.mem_pc_valid || xs.mem_pc != xs.pc) {
		load_const(R0, xs.pc);
		str(R0, RREGS, OFS_PC);
	}
}

// Exit to a constant PowerPC address: chained to the target block once
// the dispatcher has resolved it
static void exit_const(uint32 target)
{
	// Only backward branches check for interrupts etc.: any endless loop
	// contains one (or an indirect branch, which checks too)
	const bool check = target <= xs.pc;
	if (check) {
		ldr(R1, RREGS, OFS_SPC);
		cmpi(R1, 0);
	}
	uint32 *site = jp;
	emit(b_insn(check ? C_EQ : C_AL, site, site + 1));	// patched to "b(eq) target"
	load_const(R0, target);
	str(R0, RREGS, OFS_PC);
	dpi(C_AL, OP_SUB, 0, R0, RPC, (uint32)((jp + 2 - site) * 4));	// r0 = site
	b(C_AL, stub_exit);
}

// Exit to the PowerPC address in r3. aligned: r3 is known to be a multiple
// of 4 (needed for the ROM table). regs.pc is only written when leaving
// translated code.
static void exit_dynamic(bool aligned = true)
{
	ldr(R1, RREGS, OFS_SPC);
	cmpi(R1, 0);
	b(C_NE, stub_exit0_r3);
	if (aligned && rom_size) {
		dpi(C_AL, OP_SUB, 0, R1, R3, rom_base);
		dpi(C_AL, OP_CMP, 1, 0, R1, rom_size);
		ldr_lit_cond(C_CC, R2, (uint32)(uintptr)rom_table);
		emit((C_CC << 28) | (3 << 25) | (1 << 24) | (1 << 23) | (1 << 20) | (R2 << 16) | (RPC << 12) | R1);	// ldrlo pc, [r2, r1]
	}
	ldr_lit(R1, JC_HASH);								// &jcache[jc_index(pc)]
	mul(R2, R3, R1);
	mov(R2, R2, SH_LSR, 32 - JC_BITS + 1);
	ldr_lit(R1, (uint32)(uintptr)jcache);
	dpr(C_AL, OP_ADD, 0, R1, R1, R2, SH_LSL, 4);
	ldr(R2, R1, 0);
	dpr(C_AL, OP_CMP, 1, 0, R2, R3);
	ldst(C_EQ, 1, 0, RPC, R1, 4);					// ldreq pc, [r1, #4]
	ldr(R2, R1, 8);
	dpr(C_AL, OP_CMP, 1, 0, R2, R3);
	ldst(C_EQ, 1, 0, RPC, R1, 12);					// ldreq pc, [r1, #12]
	b(C_AL, stub_exit_miss_r3);
}


/*
 *  Translate one block
 */

// Instruction fields
#define F_RD	((op >> 21) & 31)
#define F_RS	((op >> 21) & 31)
#define F_RA	((op >> 16) & 31)
#define F_RB	((op >> 11) & 31)
#define F_SIMM	((int32)(int16)(op & 0xffff))
#define F_UIMM	(op & 0xffff)
#define F_RC	(op & 1)
#define F_OE	((op >> 10) & 1)
#define F_SH	((op >> 11) & 31)
#define F_MB	((op >> 6) & 31)
#define F_ME	((op >> 1) & 31)
#define F_CRFD	((op >> 23) & 7)
#define F_CRFS	((op >> 18) & 7)
#define F_BO	((op >> 21) & 31)
#define F_BI	((op >> 16) & 31)
#define F_LK	(op & 1)
#define F_AA	((op >> 1) & 1)

typedef void (*handler_fn)(powerpc_cpu *, uint32);
static handler_fn nop_handler;

// Evaluate the BO/BI condition of a conditional branch. Emits code that
// branches to the returned list of "not taken" sites (up to 2), which the
// caller patches. Returns the number of sites.
static int emit_branch_cond(uint32 op, uint32 *nt_sites[2])
{
	const int bo = F_BO, bi = F_BI;
	int n = 0;
	const bool dec = (bo & 0x04) == 0;
	const bool cond = (bo & 0x10) == 0;

	if (dec) {
		ldr(R0, RREGS, OFS_CTR);
		dpi(C_AL, OP_SUB, 1, R0, R0, 1);
		str(R0, RREGS, OFS_CTR);
		// taken if (ctr == 0) == (bo & 2)
		nt_sites[n++] = jp;
		emit((bo & 0x02) ? C_NE : C_EQ);	// condition only, completed by caller
	}
	if (cond) {
		const int crf = bi >> 2, bit = bi & 3;
		int cc;
		if (!dec && xs.flags_crf == crf && bit != 3) {
			// Flags of the compare that set this field are still live
			static const int scc[3] = { C_LT, C_GT, C_EQ };
			static const int ucc[3] = { C_CC, C_HI, C_EQ };
			cc = xs.flags_signed ? scc[bit] : ucc[bit];
		}
		else {
			dpi(C_AL, OP_TST, 1, 0, RCR, 1u << (31 - bi));
			cc = C_NE;
		}
		if ((bo & 0x08) == 0)
			cc ^= 1;
		// not taken if !cc
		nt_sites[n++] = jp;
		emit(cc ^ 1);
	}
	return n;
}

static void patch_branch_sites(uint32 **sites, int n, uint32 *target)
{
	for (int i = 0; i < n; i++) {
		uint32 cond = *sites[i];
		*sites[i] = b_insn(cond, sites[i], target);
	}
}

static bool translate_insn(powerpc_cpu *cpu, uint32 op, const ppc_armjit::insn_info *ii, bool &end_block);

static uint32 *translate_block(powerpc_cpu *cpu, uint32 pc0, uint32 &end_pc, bool single = false)
{
	uint32 *entry = jp;
	xlate_cpu = cpu;
	xs.pc = pc0;
	xs.mem_pc_valid = false;
	xs.flags_crf = -1;
	xs.lr_gpr = xs.ctr_gpr = -1;
	xs.follows = 0;
	xs.max_pc = pc0;
	lit_count = 0;
	rc_reset();

	int n = 0;
	for (;;) {
		maybe_flush_pool();
		if (n >= MAX_BLOCK_INSNS || (jp - entry) * 4 > MAX_BLOCK_BYTES || (single && n > 0)) {
			rc_flush();
			exit_const(xs.pc);
			break;
		}
		uint32 op = vm_read_memory_4(xs.pc);
		ppc_armjit::insn_info info;
		ppc_armjit::decode(cpu, op, info);
		bool end_block = false;
		xs.next_pc = 0;
		translate_insn(cpu, op, &info, end_block);
		rc_unpin();
		n++;
		if (xs.pc > xs.max_pc)
			xs.max_pc = xs.pc;
		if (end_block)
			break;
		xs.pc = xs.next_pc ? xs.next_pc : xs.pc + 4;
		if (single)
			xs.max_pc = xs.pc - 4;
	}
	flush_pool(false);
	end_pc = xs.max_pc;
	st_insns += n;
	return entry;
}

// Fallback: call the interpreter's handler for this instruction
static void emit_fallback(uint32 op, const ppc_armjit::insn_info *ii, bool &end_block)
{
	handler_fn fn = (handler_fn)ii->handler;
	if (fn == nop_handler)
		return;
	if (profiling && ii->mnemo < MAX_MNEMO) {
		emit_count(&prof_fallback[ii->mnemo]);
		if (ii->mnemo == PPC_I(MFSPR) || ii->mnemo == PPC_I(MTSPR))
			emit_count(&prof_spr_fb[ii->mnemo == PPC_I(MTSPR)][((op >> 16) & 31) | ((op >> 6) & 0x3e0)]);
	}
	rc_flush();
	spill_pinned();
	sync_pc();
	ldr_lit(R0, (uint32)(uintptr)xlate_cpu);
	load_const(R1, op);
	emit_call((const void *)fn);
	rc_reset();				// the handler may have changed any register
	reload_pinned();
	xs.lr_gpr = xs.ctr_gpr = -1;
	xs.mem_pc = xs.pc + 4;
	xs.mem_pc_valid = true;
	st_fallbacks++;
	if (ii->end_block) {
		// The handler set the new pc
		ldr(R3, RREGS, OFS_PC);
		exit_dynamic(false);
		end_block = true;
	}
}

static int ea_reg = R1;		// register holding the effective address

// Effective address of a D-form access into r1
static void ea_d(int ra, int32 d, bool ra_or_0)
{
	ea_reg = R1;
	if (ra == 0 && ra_or_0)
		load_const(R1, (uint32)d);
	else
		add_const(R1, rc_use(ra), d);
}

// Effective address of an X-form access into r1
static void ea_x(int ra, int rb, bool ra_or_0)
{
	ea_reg = R1;
	if (ra == 0 && ra_or_0)
		mov(R1, rc_use(rb));
	else {
		const int a = rc_use(ra);
		const int b = rc_use(rb);
		dpr(C_AL, OP_ADD, 0, R1, a, b);
	}
}

// Update forms: rA += d, then access [rA] (EA in r1 = host register of rA)
static void ea_update_d(int ra, int32 d)
{
	rc_use(ra);
	const int h = rc_def(ra);
	add_const(h, h, d);
	ea_reg = h;
}

static void ea_update_x(int ra, int rb)
{
	rc_use(ra);
	const int b = rc_use(rb);
	const int h = rc_def(ra);
	dpr(C_AL, OP_ADD, 0, h, h, b);
	ea_reg = h;
}

enum { MEM_W, MEM_WBR, MEM_B, MEM_H, MEM_HA, MEM_HBR };

// Load from [r1] into gpr rd, optionally update ra with r1. Loads go to
// r0..r12 only (the data abort handler can't handle lr)
static void emit_load(int size, int rd, int ra, bool update)
{
	switch (size) {
	case MEM_W:
	case MEM_WBR: ldst_r(1, 0, R0, ea_reg, RMEM); break;
	case MEM_B:   ldst_r(1, 1, R0, ea_reg, RMEM); break;
	default:      ldsth_r(1, R0, ea_reg, RMEM); break;
	}
	const int d = rc_def(rd);
	switch (size) {
	case MEM_W:   rev(d, R0); break;
	case MEM_H:   rev16(d, R0); break;
	case MEM_HA:  revsh(d, R0); break;
	default:      mov(d, R0); break;
	}
	if (update)
		mov(rc_def(ra), R1);
}

// Store gpr rs to [r1], optionally update ra with r1
static void emit_store(int size, int rs, int ra, bool update)
{
	const int s = rc_use(rs);
	switch (size) {
	case MEM_W:   rev(R0, s); ldst_r(0, 0, R0, ea_reg, RMEM); break;
	case MEM_WBR: mov(R0, s); ldst_r(0, 0, R0, ea_reg, RMEM); break;
	case MEM_B:   mov(R0, s); ldst_r(0, 1, R0, ea_reg, RMEM); break;
	case MEM_H:   rev16(R0, s); ldsth_r(0, R0, ea_reg, RMEM); break;
	case MEM_HBR: mov(R0, s); ldsth_r(0, R0, ea_reg, RMEM); break;
	}
	if (update)
		mov(rc_def(ra), R1);
}

// d = rotl(s, sh) & MASK(mb, me)
static void emit_rotate_mask(int d, int s, uint32 sh, uint32 mb, uint32 me)
{
	const uint32 mask = ppc_mask(mb, me);
	const int rot = (32 - sh) & 31;
	if (mask == 0xffffffff)
		mov(d, s, SH_ROR, rot);
	else if (sh != 0 && mb == 0 && me == 31 - sh)
		mov(d, s, SH_LSL, sh);						// slwi
	else if (sh != 0 && me == 31 && mb == 32 - sh)
		mov(d, s, SH_LSR, mb);						// srwi
	else if (rot == 0)
		and_mask(d, s, mask);
	else if (imm_ok(mask) || imm_ok(~mask)) {
		mov(d, s, SH_ROR, rot);
		and_mask(d, d, mask);
	}
	else {
		load_const(R1, mask);
		dpr(C_AL, OP_AND, 0, d, R1, s, SH_ROR, rot);
	}
}

static bool translate_insn(powerpc_cpu *cpu, uint32 op, const ppc_armjit::insn_info *ii, bool &end_block)
{
	// Flags from a compare are only reused by the instruction right after it
	const int prev_flags_crf = xs.flags_crf;
	xs.flags_crf = -1;

	if (profiling && ii->mnemo < MAX_MNEMO) {
		prof_name[ii->mnemo] = ii->name;
		emit_count(&prof_exec[ii->mnemo]);
	}

	switch (ii->mnemo) {

	// --- Integer arithmetic

	case PPC_I(ADDI):
	case PPC_I(ADDIS): {
		int32 v = ii->mnemo == PPC_I(ADDIS) ? (int32)(F_UIMM << 16) : F_SIMM;
		if (F_RA == 0)
			load_const(rc_def(F_RD), (uint32)v);
		else {
			const int a = rc_use(F_RA);
			add_const(rc_def(F_RD), a, v);
		}
		return true;
	}

	case PPC_I(ADDIC):
	case PPC_I(ADDIC_): {
		const int a = rc_use(F_RA);
		load_const(R1, (uint32)F_SIMM);
		const int d = rc_def(F_RD);
		dpr(C_AL, OP_ADD, 1, d, a, R1);
		store_carry();
		if (ii->mnemo == PPC_I(ADDIC_))
			record_cr0(d);
		return true;
	}

	case PPC_I(SUBFIC): {
		const int a = rc_use(F_RA);
		load_const(R1, (uint32)F_SIMM);
		const int d = rc_def(F_RD);
		dpr(C_AL, OP_SUB, 1, d, R1, a);
		store_carry();
		return true;
	}

	case PPC_I(MULLI): {
		const int a = rc_use(F_RA);
		load_const(R1, (uint32)F_SIMM);
		mul(R2, a, R1);
		mov(rc_def(F_RD), R2);
		return true;
	}

	case PPC_I(ADD):
	case PPC_I(ADDC):
	case PPC_I(ADDE):
	case PPC_I(SUBF):
	case PPC_I(SUBFC):
	case PPC_I(SUBFE):
	case PPC_I(MULLW):
	case PPC_I(MULHW):
	case PPC_I(MULHWU): {
		const int oe = F_OE;
		if (oe && (ii->mnemo == PPC_I(MULHW) || ii->mnemo == PPC_I(MULHWU)))
			break;
		const int a = rc_use(F_RA);
		const int b = rc_use(F_RB);
		int ov_cond = C_VS;
		int d;
		switch (ii->mnemo) {
		case PPC_I(ADD):   d = rc_def(F_RD); dpr(C_AL, OP_ADD, oe, d, a, b); break;
		case PPC_I(ADDC):  d = rc_def(F_RD); dpr(C_AL, OP_ADD, 1, d, a, b); store_carry(); break;
		case PPC_I(ADDE):  load_carry(); d = rc_def(F_RD); dpr(C_AL, OP_ADC, 1, d, a, b); store_carry(); break;
		case PPC_I(SUBF):  d = rc_def(F_RD); dpr(C_AL, OP_SUB, oe, d, b, a); break;
		case PPC_I(SUBFC): d = rc_def(F_RD); dpr(C_AL, OP_SUB, 1, d, b, a); store_carry(); break;
		case PPC_I(SUBFE): load_carry(); d = rc_def(F_RD); dpr(C_AL, OP_SBC, 1, d, b, a); store_carry(); break;
		case PPC_I(MULLW):
			if (oe) {
				// overflow if the high word is not the sign extension of the low word
				smull(R2, R3, a, b);
				d = rc_def(F_RD);
				mov(d, R2);
				dpr(C_AL, OP_CMP, 1, 0, R3, R2, SH_ASR, 31);
				ov_cond = C_NE;
			}
			else {
				mul(R2, a, b);
				d = rc_def(F_RD);
				mov(d, R2);
			}
			break;
		case PPC_I(MULHW):  smull(R2, R3, a, b); d = rc_def(F_RD); mov(d, R3); break;
		default:            umull(R2, R3, a, b); d = rc_def(F_RD); mov(d, R3); break;
		}
		if (oe)
			store_overflow(ov_cond);
		if (F_RC)
			record_cr0(d);
		return true;
	}

	case PPC_I(ADDZE):
	case PPC_I(ADDME):
	case PPC_I(SUBFZE):
	case PPC_I(SUBFME):
	case PPC_I(NEG): {
		const int oe = F_OE;
		const int a = rc_use(F_RA);
		int d;
		switch (ii->mnemo) {
		case PPC_I(ADDZE):  load_carry(); d = rc_def(F_RD); dpi(C_AL, OP_ADC, 1, d, a, 0); store_carry(); break;
		case PPC_I(ADDME):  load_carry(); d = rc_def(F_RD); dpi(C_AL, OP_SBC, 1, d, a, 0); store_carry(); break;
		case PPC_I(SUBFZE): load_carry(); d = rc_def(F_RD); dpi(C_AL, OP_RSC, 1, d, a, 0); store_carry(); break;
		case PPC_I(SUBFME): load_carry(); dpr(C_AL, OP_MVN, 0, R0, 0, a); d = rc_def(F_RD); dpi(C_AL, OP_SBC, 1, d, R0, 0); store_carry(); break;
		default:            d = rc_def(F_RD); dpi(C_AL, OP_RSB, oe, d, a, 0); break;
		}
		if (oe)
			store_overflow(C_VS);
		if (F_RC)
			record_cr0(d);
		return true;
	}

	// --- Logical

	case PPC_I(AND):
	case PPC_I(ANDC):
	case PPC_I(OR):
	case PPC_I(ORC):
	case PPC_I(XOR):
	case PPC_I(NOR):
	case PPC_I(NAND):
	case PPC_I(EQV): {
		const int s = rc_use(F_RS);
		int d;
		if (ii->mnemo == PPC_I(OR) && F_RS == F_RB) {
			d = rc_def(F_RA);						// mr
			if (d != s)
				mov(d, s);
		}
		else {
			const int b = rc_use(F_RB);
			d = rc_def(F_RA);
			switch (ii->mnemo) {
			case PPC_I(AND):  dpr(C_AL, OP_AND, 0, d, s, b); break;
			case PPC_I(ANDC): dpr(C_AL, OP_BIC, 0, d, s, b); break;
			case PPC_I(OR):   dpr(C_AL, OP_ORR, 0, d, s, b); break;
			case PPC_I(ORC):  dpr(C_AL, OP_MVN, 0, R1, 0, b); dpr(C_AL, OP_ORR, 0, d, s, R1); break;
			case PPC_I(XOR):  dpr(C_AL, OP_EOR, 0, d, s, b); break;
			case PPC_I(NOR):  dpr(C_AL, OP_ORR, 0, d, s, b); dpr(C_AL, OP_MVN, 0, d, 0, d); break;
			case PPC_I(NAND): dpr(C_AL, OP_AND, 0, d, s, b); dpr(C_AL, OP_MVN, 0, d, 0, d); break;
			default:          dpr(C_AL, OP_EOR, 0, d, s, b); dpr(C_AL, OP_MVN, 0, d, 0, d); break;
			}
		}
		if (F_RC)
			record_cr0(d);
		return true;
	}

	case PPC_I(ORI):
	case PPC_I(ORIS):
	case PPC_I(XORI):
	case PPC_I(XORIS):
	case PPC_I(ANDI):
	case PPC_I(ANDIS): {
		const bool hi = ii->mnemo == PPC_I(ORIS) || ii->mnemo == PPC_I(XORIS) || ii->mnemo == PPC_I(ANDIS);
		const uint32 v = hi ? F_UIMM << 16 : F_UIMM;
		const bool is_and = ii->mnemo == PPC_I(ANDI) || ii->mnemo == PPC_I(ANDIS);
		if (!is_and && v == 0 && F_RS == F_RA)
			return true;					// nop
		const int aop = is_and ? OP_AND : (ii->mnemo == PPC_I(ORI) || ii->mnemo == PPC_I(ORIS)) ? OP_ORR : OP_EOR;
		const int s = rc_use(F_RS);
		const int d = rc_def(F_RA);
		if (imm_ok(v))
			dpi(C_AL, aop, 0, d, s, v);
		else if (is_and)
			and_mask(d, s, v);
		else {
			dpi(C_AL, aop, 0, d, s, v & 0xff00ff00);
			dpi(C_AL, aop, 0, d, d, v & 0x00ff00ff);
		}
		if (is_and)
			record_cr0(d);
		return true;
	}

	case PPC_I(EXTSB):
	case PPC_I(EXTSH):
	case PPC_I(CNTLZW): {
		const int s = rc_use(F_RS);
		const int d = rc_def(F_RA);
		if (ii->mnemo == PPC_I(EXTSB))
			sxtb(d, s);
		else if (ii->mnemo == PPC_I(EXTSH))
			sxth(d, s);
		else
			clz(d, s);
		if (F_RC)
			record_cr0(d);
		return true;
	}

	// --- Rotate and shift

	case PPC_I(RLWINM): {
		const int s = rc_use(F_RS);
		const int d = rc_def(F_RA);
		emit_rotate_mask(d, s, F_SH, F_MB, F_ME);
		if (F_RC)
			record_cr0(d);
		return true;
	}

	case PPC_I(RLWNM): {
		const int s = rc_use(F_RS);
		const int b = rc_use(F_RB);
		dpi(C_AL, OP_AND, 0, R1, b, 31);
		dpi(C_AL, OP_RSB, 0, R1, R1, 32);
		dprs(C_AL, OP_MOV, 0, R0, 0, s, SH_ROR, R1);
		const int d = rc_def(F_RA);
		and_mask(d, R0, ppc_mask(F_MB, F_ME));
		if (F_RC)
			record_cr0(d);
		return true;
	}

	case PPC_I(RLWIMI): {
		const uint32 mask = ppc_mask(F_MB, F_ME);
		const int s = rc_use(F_RS);
		const int a = rc_use(F_RA);
		const int rot = (32 - F_SH) & 31;
		if (imm_ok(mask)) {
			mov(R0, s, SH_ROR, rot);
			dpi(C_AL, OP_AND, 0, R0, R0, mask);
			dpi(C_AL, OP_BIC, 0, R3, a, mask);
		}
		else if (imm_ok(~mask)) {
			mov(R0, s, SH_ROR, rot);
			dpi(C_AL, OP_BIC, 0, R0, R0, ~mask);
			dpi(C_AL, OP_AND, 0, R3, a, ~mask);
		}
		else {
			load_const(R1, mask);
			dpr(C_AL, OP_AND, 0, R0, R1, s, SH_ROR, rot);
			dpr(C_AL, OP_BIC, 0, R3, a, R1);
		}
		const int d = rc_def(F_RA);
		dpr(C_AL, OP_ORR, 0, d, R0, R3);
		if (F_RC)
			record_cr0(d);
		return true;
	}

	case PPC_I(SLW):
	case PPC_I(SRW): {
		const int s = rc_use(F_RS);
		const int b = rc_use(F_RB);
		dpi(C_AL, OP_AND, 0, R1, b, 0x3f);
		const int d = rc_def(F_RA);
		dprs(C_AL, OP_MOV, 0, d, 0, s, ii->mnemo == PPC_I(SLW) ? SH_LSL : SH_LSR, R1);
		if (F_RC)
			record_cr0(d);
		return true;
	}

	case PPC_I(SRAWI): {
		const int n = F_SH;
		const int s = rc_use(F_RS);
		if (n == 0)
			dpi(C_AL, OP_MOV, 0, R2, 0, 0);
		else {
			// CA = negative && any 1 bits shifted out
			dpr(C_AL, OP_MOV, 1, R2, 0, s, SH_LSL, 32 - n);
			dpr(C_NE, OP_MOV, 0, R2, 0, s, SH_LSR, 31);
		}
		strb(R2, RREGS, OFS_CA);
		const int d = rc_def(F_RA);
		mov(d, s, SH_ASR, n);
		if (F_RC)
			record_cr0(d);
		return true;
	}

	case PPC_I(SRAW): {
		const int s = rc_use(F_RS);
		const int b = rc_use(F_RB);
		dpi(C_AL, OP_AND, 0, R1, b, 0x3f);
		// CA = negative && (rs & ~(-1 << n)) != 0 (n >= 32: all bits)
		dpi(C_AL, OP_MVN, 0, R2, 0, 0);
		dprs(C_AL, OP_BIC, 0, R2, s, R2, SH_LSL, R1);
		cmpi(R2, 0);
		dpr(C_NE, OP_MOV, 0, R2, 0, s, SH_LSR, 31);
		strb(R2, RREGS, OFS_CA);
		const int d = rc_def(F_RA);
		dprs(C_AL, OP_MOV, 0, d, 0, s, SH_ASR, R1);
		if (F_RC)
			record_cr0(d);
		return true;
	}

	// --- Compare

	case PPC_I(CMP):
	case PPC_I(CMPL): {
		const int a = rc_use(F_RA);
		const int b = rc_use(F_RB);
		dpr(C_AL, OP_CMP, 1, 0, a, b);
		cr_from_flags(F_CRFD, ii->mnemo == PPC_I(CMP));
		xs.flags_crf = F_CRFD;
		xs.flags_signed = ii->mnemo == PPC_I(CMP);
		return true;
	}

	case PPC_I(CMPI):
	case PPC_I(CMPLI): {
		const bool sign = ii->mnemo == PPC_I(CMPI);
		const uint32 v = sign ? (uint32)F_SIMM : F_UIMM;
		const int a = rc_use(F_RA);
		if (imm_ok(v))
			dpi(C_AL, OP_CMP, 1, 0, a, v);
		else if (sign && imm_ok(-v))
			dpi(C_AL, OP_CMN, 1, 0, a, -v);
		else {
			load_const(R1, v);
			dpr(C_AL, OP_CMP, 1, 0, a, R1);
		}
		cr_from_flags(F_CRFD, sign);
		xs.flags_crf = F_CRFD;
		xs.flags_signed = sign;
		return true;
	}

	// --- Condition register

	case PPC_I(CRAND):
	case PPC_I(CRANDC):
	case PPC_I(CREQV):
	case PPC_I(CRNAND):
	case PPC_I(CRNOR):
	case PPC_I(CROR):
	case PPC_I(CRORC):
	case PPC_I(CRXOR): {
		const int ba = F_RA, bb = F_RB, bd = F_RD;
		mov(R0, RCR);
		mov(R1, R0, SH_LSR, 31 - ba);
		mov(R2, R0, SH_LSR, 31 - bb);
		switch (ii->mnemo) {
		case PPC_I(CRAND):  dpr(C_AL, OP_AND, 0, R1, R1, R2); break;
		case PPC_I(CRANDC): dpr(C_AL, OP_BIC, 0, R1, R1, R2); break;
		case PPC_I(CREQV):  dpr(C_AL, OP_EOR, 0, R1, R1, R2); dpr(C_AL, OP_MVN, 0, R1, 0, R1); break;
		case PPC_I(CRNAND): dpr(C_AL, OP_AND, 0, R1, R1, R2); dpr(C_AL, OP_MVN, 0, R1, 0, R1); break;
		case PPC_I(CRNOR):  dpr(C_AL, OP_ORR, 0, R1, R1, R2); dpr(C_AL, OP_MVN, 0, R1, 0, R1); break;
		case PPC_I(CROR):   dpr(C_AL, OP_ORR, 0, R1, R1, R2); break;
		case PPC_I(CRORC):  dpr(C_AL, OP_MVN, 0, R2, 0, R2); dpr(C_AL, OP_ORR, 0, R1, R1, R2); break;
		default:            dpr(C_AL, OP_EOR, 0, R1, R1, R2); break;
		}
		dpi(C_AL, OP_AND, 0, R1, R1, 1);
		dpi(C_AL, OP_BIC, 0, R0, R0, 1u << (31 - bd));
		dpr(C_AL, OP_ORR, 0, R0, R0, R1, SH_LSL, 31 - bd);
		mov(RCR, R0);
		return true;
	}

	case PPC_I(MCRF): {
		const int sd = 28 - 4 * F_CRFD;
		mov(R0, RCR);
		mov(R1, R0, SH_LSL, 4 * F_CRFS);
		mov(R1, R1, SH_LSR, 28);
		dpi(C_AL, OP_BIC, 0, R0, R0, 0xfu << sd);
		dpr(C_AL, OP_ORR, 0, R0, R0, R1, SH_LSL, sd);
		mov(RCR, R0);
		return true;
	}

	case PPC_I(MFCR):
		mov(rc_def(F_RD), RCR);
		return true;

	case PPC_I(MTCRF): {
		const uint32 crm = (op >> 12) & 0xff;
		uint32 mask = 0;
		for (int i = 0; i < 8; i++)
			if (crm & (1 << i))
				mask |= 0xfu << (4 * i);
		const int s = rc_use(F_RS);
		if (mask == 0xffffffff)
			mov(RCR, s);
		else {
			mov(R1, RCR);
			load_const(R2, mask);
			dpr(C_AL, OP_AND, 0, R0, s, R2);
			dpr(C_AL, OP_BIC, 0, R1, R1, R2);
			dpr(C_AL, OP_ORR, 0, R0, R0, R1);
			mov(RCR, R0);
		}
		return true;
	}

	// --- Special purpose registers

	case PPC_I(MFSPR): {
		const uint32 spr = ((op >> 16) & 31) | ((op >> 6) & 0x3e0);
		if (spr == 8 || spr == 9) {
			ldr(rc_def(F_RD), RREGS, spr == 8 ? OFS_LR : OFS_CTR);
			return true;
		}
		if (spr == 1) {				// mfxer
			ldrb(R0, RREGS, OFS_SO);
			ldrb(R1, RREGS, OFS_OV);
			ldrb(R2, RREGS, OFS_CA);
			ldrb(R3, RREGS, OFS_COUNT);
			dpr(C_AL, OP_ORR, 0, R3, R3, R0, SH_LSL, 31);
			dpr(C_AL, OP_ORR, 0, R3, R3, R1, SH_LSL, 30);
			dpr(C_AL, OP_ORR, 0, R3, R3, R2, SH_LSL, 29);
			mov(rc_def(F_RD), R3);
			return true;
		}
		break;
	}

	case PPC_I(MTSPR): {
		const uint32 spr = ((op >> 16) & 31) | ((op >> 6) & 0x3e0);
		if (spr == 8 || spr == 9) {
			str(rc_use(F_RS), RREGS, spr == 8 ? OFS_LR : OFS_CTR);
			if (spr == 8)
				xs.lr_gpr = F_RS;
			else
				xs.ctr_gpr = F_RS;
			return true;
		}
		if (spr == 1) {				// mtxer
			const int s = rc_use(F_RS);
			mov(R1, s, SH_LSR, 31);
			strb(R1, RREGS, OFS_SO);
			mov(R1, s, SH_LSR, 30);
			dpi(C_AL, OP_AND, 0, R1, R1, 1);
			strb(R1, RREGS, OFS_OV);
			mov(R1, s, SH_LSR, 29);
			dpi(C_AL, OP_AND, 0, R1, R1, 1);
			strb(R1, RREGS, OFS_CA);
			dpi(C_AL, OP_AND, 0, R1, s, 0x7f);
			strb(R1, RREGS, OFS_COUNT);
			return true;
		}
		break;
	}

	case PPC_I(MCRXR): {
		const int sh = 28 - 4 * F_CRFD;
		ldrb(R0, RREGS, OFS_SO);
		ldrb(R1, RREGS, OFS_OV);
		ldrb(R2, RREGS, OFS_CA);
		mov(R0, R0, SH_LSL, 3);
		dpr(C_AL, OP_ORR, 0, R0, R0, R1, SH_LSL, 2);
		dpr(C_AL, OP_ORR, 0, R0, R0, R2, SH_LSL, 1);
		dpi(C_AL, OP_MOV, 0, R1, 0, 0);
		strb(R1, RREGS, OFS_SO);
		strb(R1, RREGS, OFS_OV);
		strb(R1, RREGS, OFS_CA);
		dpi(C_AL, OP_BIC, 0, RCR, RCR, 0xfu << sh);
		dpr(C_AL, OP_ORR, 0, RCR, RCR, R0, SH_LSL, sh);
		return true;
	}

	// --- Floating point loads and stores (double: just 64 bits, byte swapped)

	case PPC_I(LFD):
	case PPC_I(LFDU):
	case PPC_I(LFDX):
	case PPC_I(LFDUX):
	case PPC_I(STFD):
	case PPC_I(STFDU):
	case PPC_I(STFDX):
	case PPC_I(STFDUX): {
		const bool x = ii->mnemo == PPC_I(LFDX) || ii->mnemo == PPC_I(LFDUX) || ii->mnemo == PPC_I(STFDX) || ii->mnemo == PPC_I(STFDUX);
		const bool up = ii->mnemo == PPC_I(LFDU) || ii->mnemo == PPC_I(LFDUX) || ii->mnemo == PPC_I(STFDU) || ii->mnemo == PPC_I(STFDUX);
		const bool load = ii->mnemo == PPC_I(LFD) || ii->mnemo == PPC_I(LFDU) || ii->mnemo == PPC_I(LFDX) || ii->mnemo == PPC_I(LFDUX);
		const int fofs = OFS_FPR + 8 * F_RD;
		if (x)
			ea_x(F_RA, F_RB, !up);
		else
			ea_d(F_RA, F_SIMM, !up);
		dpr(C_AL, OP_ADD, 0, R3, R1, RMEM);
		if (load) {
			ldr(R0, R3, 0);
			ldr(R2, R3, 4);
			rev(R0, R0);
			rev(R2, R2);
			str(R2, RREGS, fofs);
			str(R0, RREGS, fofs + 4);
		}
		else {
			ldr(R0, RREGS, fofs + 4);
			ldr(R2, RREGS, fofs);
			rev(R0, R0);
			rev(R2, R2);
			str(R0, R3, 0);
			str(R2, R3, 4);
		}
		if (up)
			mov(rc_def(F_RA), R1);
		return true;
	}

	// --- Loads and stores

#define LOAD_D(MN, SZ, UP)	case PPC_I(MN): if (UP && F_RD != F_RA) { ea_update_d(F_RA, F_SIMM); emit_load(SZ, F_RD, F_RA, false); } \
							else { ea_d(F_RA, F_SIMM, !UP); emit_load(SZ, F_RD, F_RA, UP); } return true;
#define LOAD_X(MN, SZ, UP)	case PPC_I(MN): if (UP && F_RD != F_RA) { ea_update_x(F_RA, F_RB); emit_load(SZ, F_RD, F_RA, false); } \
							else { ea_x(F_RA, F_RB, !UP); emit_load(SZ, F_RD, F_RA, UP); } return true;
#define STORE_D(MN, SZ, UP)	case PPC_I(MN): if (UP && F_RS != F_RA) { ea_update_d(F_RA, F_SIMM); emit_store(SZ, F_RS, F_RA, false); } \
							else { ea_d(F_RA, F_SIMM, !UP); emit_store(SZ, F_RS, F_RA, UP); } return true;
#define STORE_X(MN, SZ, UP)	case PPC_I(MN): if (UP && F_RS != F_RA) { ea_update_x(F_RA, F_RB); emit_store(SZ, F_RS, F_RA, false); } \
							else { ea_x(F_RA, F_RB, !UP); emit_store(SZ, F_RS, F_RA, UP); } return true;

	LOAD_D(LWZ, MEM_W, false)   LOAD_D(LWZU, MEM_W, true)
	LOAD_D(LBZ, MEM_B, false)   LOAD_D(LBZU, MEM_B, true)
	LOAD_D(LHZ, MEM_H, false)   LOAD_D(LHZU, MEM_H, true)
	LOAD_D(LHA, MEM_HA, false)  LOAD_D(LHAU, MEM_HA, true)
	LOAD_X(LWZX, MEM_W, false)  LOAD_X(LWZUX, MEM_W, true)
	LOAD_X(LBZX, MEM_B, false)  LOAD_X(LBZUX, MEM_B, true)
	LOAD_X(LHZX, MEM_H, false)  LOAD_X(LHZUX, MEM_H, true)
	LOAD_X(LHAX, MEM_HA, false) LOAD_X(LHAUX, MEM_HA, true)
	LOAD_X(LWBRX, MEM_WBR, false)
	LOAD_X(LHBRX, MEM_HBR, false)
	STORE_D(STW, MEM_W, false)  STORE_D(STWU, MEM_W, true)
	STORE_D(STB, MEM_B, false)  STORE_D(STBU, MEM_B, true)
	STORE_D(STH, MEM_H, false)  STORE_D(STHU, MEM_H, true)
	STORE_X(STWX, MEM_W, false) STORE_X(STWUX, MEM_W, true)
	STORE_X(STBX, MEM_B, false) STORE_X(STBUX, MEM_B, true)
	STORE_X(STHX, MEM_H, false) STORE_X(STHUX, MEM_H, true)
	STORE_X(STWBRX, MEM_WBR, false)
	STORE_X(STHBRX, MEM_HBR, false)

	// --- Reservations (uniprocessor: as in the interpreter)

	case PPC_I(LWARX): {
		ea_x(F_RA, F_RB, true);
		ldst_r(1, 0, R0, R1, RMEM);
		dpi(C_AL, OP_MOV, 0, R2, 0, 1);
		str(R2, RREGS, OFS_RESV);
		str(R1, RREGS, OFS_RESA);
		rev(rc_def(F_RD), R0);
		return true;
	}

	case PPC_I(STWCX): {
		const int s = rc_use(F_RS);
		ea_x(F_RA, F_RB, true);
		dpi(C_AL, OP_BIC, 0, RCR, RCR, 0xf0000000);
		ldr(R2, RREGS, OFS_RESV);
		ldr(R3, RREGS, OFS_RESA);
		dpr(C_AL, OP_CMP, 1, 0, R3, R1);
		dpi(C_NE, OP_MOV, 0, R2, 0, 0);			// reserved address must match
		cmpi(R2, 0);
		emit((C_NE << 28) | 0x06BF0F30 | (R0 << 12) | s);	// revne r0, s
		emit((C_NE << 28) | (3 << 25) | (1 << 24) | (1 << 23) | (R1 << 16) | (R0 << 12) | RMEM);	// strne r0, [r1, r10]
		dpi(C_NE, OP_ORR, 0, RCR, RCR, 0x20000000);	// cr0.eq = success
		dpi(C_AL, OP_MOV, 0, R2, 0, 0);
		str(R2, RREGS, OFS_RESV);
		ldrb(R2, RREGS, OFS_SO);
		dpr(C_AL, OP_ORR, 0, RCR, RCR, R2, SH_LSL, 28);
		return true;
	}

	case PPC_I(LMW):
	case PPC_I(STMW): {
		const bool load = ii->mnemo == PPC_I(LMW);
		ea_d(F_RA, F_SIMM, true);
		rc_flush();							// work on the registers in memory
		spill_pinned();
		dpr(C_AL, OP_ADD, 0, R1, R1, RMEM);
		for (int r = F_RD; r <= 31; r++) {
			if (load) {
				ldst_post4(1, R0, R1);
				rev(R0, R0);
				st_gpr(R0, r);
			}
			else {
				ld_gpr(R0, r);
				rev(R0, R0);
				ldst_post4(0, R0, R1);
			}
		}
		if (load) {
			rc_forget(F_RD);
			reload_pinned();
			xs.lr_gpr = xs.ctr_gpr = -1;
		}
		return true;
	}

	// --- Branches

	case PPC_I(B): {
		int32 li = (int32)(op << 6) >> 6 & ~3;
		uint32 target = (F_AA ? 0 : xs.pc) + li;
		if (!F_LK && xlate_follow && target > xs.pc && target - xs.pc <= 4096 && xs.follows < 4) {
			// Continue translating at the target (no block exit)
			xs.follows++;
			xs.next_pc = target;
			return true;
		}
		rc_flush();
		if (F_LK) {
			load_const(R0, xs.pc + 4);
			str(R0, RREGS, OFS_LR);
		}
		exit_const(target);
		end_block = true;
		return true;
	}

	case PPC_I(BC): {
		int32 bd = (int32)(int16)(op & 0xfffc);
		uint32 target = ((F_AA ? 0 : xs.pc) + bd) & ~3u;
		rc_flush();
		if (F_LK) {
			load_const(R0, xs.pc + 4);
			str(R0, RREGS, OFS_LR);
		}
		xs.flags_crf = prev_flags_crf;
		uint32 *nt[2];
		int n = emit_branch_cond(op, nt);
		xs.flags_crf = -1;
		exit_const(target);
		patch_branch_sites(nt, n, jp);
		if (n)
			exit_const(xs.pc + 4);
		end_block = true;
		return true;
	}

	case PPC_I(BCLR):
	case PPC_I(BCCTR): {
		const bool ctr = ii->mnemo == PPC_I(BCCTR);
		if (ctr && (F_BO & 0x04) == 0)
			break;							// bcctr with CTR decrement: interpreter
		const int g = ctr ? xs.ctr_gpr : xs.lr_gpr;
		rc_flush();
		if (g >= 0)
			dpi(C_AL, OP_BIC, 0, R3, rc_use(g), 3);		// value moved to LR/CTR in this block
		else {
			ldr(R3, RREGS, ctr ? OFS_CTR : OFS_LR);
			dpi(C_AL, OP_BIC, 0, R3, R3, 3);
		}
		if (F_LK) {
			load_const(R0, xs.pc + 4);
			str(R0, RREGS, OFS_LR);
		}
		xs.flags_crf = prev_flags_crf;
		uint32 *nt[2];
		int n = emit_branch_cond(op, nt);
		xs.flags_crf = -1;
		exit_dynamic();
		patch_branch_sites(nt, n, jp);
		if (n)
			exit_const(xs.pc + 4);
		end_block = true;
		return true;
	}

	default:
		break;
	}

	emit_fallback(op, ii, end_block);
	return false;
}


/*
 *  Public interface
 */

void ppc_armjit::decode(powerpc_cpu *cpu, uint32 op, insn_info &info)
{
	const powerpc_cpu::instr_info_t *ii = cpu->decode(op);
	info.mnemo = ii->mnemo;
	info.end_block = (ii->cflow & powerpc_cpu::CFLOW_END_BLOCK) != 0;
	info.handler = (void *)ii->execute.ptr();
	info.name = ii->name;
}

powerpc_registers *ppc_armjit::registers(powerpc_cpu *cpu)
{
	return &cpu->regs();
}

bool ppc_armjit::reserve(uint32 size)
{
	if (buf_start)
		return true;
	uint8 *p = (uint8 *)rpi_alloc_1mb_aligned(size);
	if (p == NULL)
		return false;
	if (!rpi_mmu_set_exec((uint32)(uintptr)p, size)) {
		rpi_log("JIT: cannot make code buffer executable\n");
		return false;
	}
	buf_start = (uint32 *)p;
	buf_end = (uint32 *)(p + size);
	blk_pool = new jit_block[MAX_BLOCKS];
	blk_hash = new jit_block *[HASH_SIZE];
	link_pool = new jit_link[MAX_LINKS];
	page_pool = new page_node[MAX_PAGE_NODES];
	page_hash = new page_node *[PAGE_HASH];
	rom_table = new uint32 *[ROM_TABLE_BYTES / 4 + JC_SIZE * 2];
	jcache = (jc_entry *)(rom_table + ROM_TABLE_BYTES / 4);
	page_bits = new uint32[PAGE_WORDS];
	if (!blk_pool || !blk_hash || !link_pool || !rom_table || !page_bits || !page_pool || !page_hash)
		return false;

	// Entry/exit stubs at the start of the buffer
	jp = buf_start;
	stub_enter = (enter_fn)jp;
	init_pins();
	emit(0xE92D4FF8);						// push {r3-r11, lr}
	mov(RREGS, R0);
	reload_pinned();
	dpi(C_AL, OP_MOV, 0, RMEM, 0, 0x80000000);
	emit(0xE12FFF11);						// bx r1
	const int ofs_pc = offsetof(powerpc_registers, pc);
	stub_exit_miss_r3 = jp;
	str(R3, RREGS, ofs_pc);
	stub_exit_miss = jp;
	dpi(C_AL, OP_MOV, 0, R0, 0, 1);
	b(C_AL, jp + 3);
	stub_exit0_r3 = jp;
	str(R3, RREGS, ofs_pc);
	stub_exit0 = jp;
	dpi(C_AL, OP_MOV, 0, R0, 0, 0);
	stub_exit = jp;
	spill_pinned();
	emit(0xE8BD8FF8);						// pop {r3-r11, pc}
	blocks_start = jp + 8;
	mark_dirty(buf_start, blocks_start);
	sync_code();

	const void *kernel_code = (const void *)&rpi_log;
	rpi_log("JIT: %u KB code buffer at %p, %s\n", size / 1024, p,
			branch_ok(buf_end, kernel_code) && branch_ok(buf_start, kernel_code) ? "direct calls" : "far calls");
	return true;
}

void ppc_armjit::attach(powerpc_cpu *cpu, bool enable, bool profile, bool sample)
{
	profiling = profile;
	sampling = profile || sample;
	the_cpu = cpu;
	active = false;
	if (!enable || buf_start == NULL)
		return;

	// Offsets into the register structure
	powerpc_registers *r = &cpu->regs();
	OFS_CR = offsetof(powerpc_registers, cr);
	OFS_LR = offsetof(powerpc_registers, lr);
	OFS_CTR = offsetof(powerpc_registers, ctr);
	OFS_PC = offsetof(powerpc_registers, pc);
	OFS_SPC = offsetof(powerpc_registers, spcflags);
	OFS_FPR = offsetof(powerpc_registers, fpr);
	OFS_RESV = offsetof(powerpc_registers, reserve_valid);
	OFS_RESA = offsetof(powerpc_registers, reserve_addr);
	const int xer = offsetof(powerpc_registers, xer);
	OFS_SO = OFS_OV = OFS_CA = -1;
	const uint32 saved_xer = r->xer.get();
	static const uint32 bits[3] = { 0x80000000, 0x40000000, 0x20000000 };
	int *ofs[3] = { &OFS_SO, &OFS_OV, &OFS_CA };
	for (int i = 0; i < 3; i++) {
		r->xer.set(bits[i]);
		for (int j = 0; j < 4; j++)
			if (((uint8 *)r)[xer + j] == 1)
				*ofs[i] = xer + j;
	}
	OFS_COUNT = -1;
	r->xer.set(0x55);
	for (int j = 0; j < 4; j++)
		if (((uint8 *)r)[xer + j] == 0x55)
			OFS_COUNT = xer + j;
	r->xer.set(saved_xer);
	if (OFS_SO < 0 || OFS_OV < 0 || OFS_CA < 0 || OFS_COUNT < 0 || OFS_SPC > 4095) {
		rpi_log("JIT: unexpected register layout, disabled\n");
		return;
	}
	if (*(uint32 *)((uint8 *)r + OFS_SPC) != cpu->spcflags().get()) {
		rpi_log("JIT: unexpected spcflags layout, disabled\n");
		return;
	}

	// ROM table: ROMBase must be an ARM immediate
	rom_base = ROMBase;
	rom_size = imm_ok(rom_base) ? ROM_AREA_SIZE : 0;

	powerpc_cpu::execute_fn nop_fn(&powerpc_cpu::execute_nop);
	nop_handler = (handler_fn)nop_fn.ptr();

	flush_all();
	st_flushes = 0;
	active = true;
}

void ppc_armjit::detach(void)
{
	active = false;
	the_cpu = NULL;
}

void ppc_armjit::invalidate_range(uint32 start, uint32 end)
{
	if (!active || flush_pending || end <= start)
		return;
	st_invalidates++;
	for (uint32 p = start >> 12; p <= ((end - 1) >> 12) && p < (PAGE_WORDS << 5); p++) {
		if (page_bits[p >> 5] & (1u << (p & 31))) {
			if (inval_count == MAX_INVAL)
				flush_pending = true;
			else {
				inval_start[inval_count] = start;
				inval_end[inval_count] = end;
				inval_count++;
			}
			return;
		}
	}
}

void ppc_armjit::invalidate_all(void)
{
	if (active)
		flush_pending = true;
}

static uint32 *compile_block(powerpc_cpu *cpu, uint32 pc)
{
	if (jp + BLOCK_RESERVE / 4 > buf_end || blk_count >= MAX_BLOCKS || page_count + 16 > MAX_PAGE_NODES) {
		if (native_depth > 0)
			return NULL;			// can't flush code that is still running
		flush_all();
	}
	uint32 end_pc;
	uint32 *entry = translate_block(cpu, pc, end_pc);
	mark_dirty(entry, jp);
	mark_pages(pc, end_pc);
	jit_block *b = &blk_pool[blk_count++];
	b->pc = pc;
	b->end_pc = end_pc;
	b->entry = entry;
	b->in_links = NULL;
	for (uint32 p = pc >> 12; p <= (end_pc >> 12) && page_count < MAX_PAGE_NODES; p++) {
		page_node *n = &page_pool[page_count++];
		n->page = p;
		n->b = b;
		n->next = page_hash[p & (PAGE_HASH - 1)];
		page_hash[p & (PAGE_HASH - 1)] = n;
	}
	const int h = (pc >> 2) & (HASH_SIZE - 1);
	b->next = blk_hash[h];
	blk_hash[h] = b;
	st_compiles++;
	return entry;
}

bool ppc_armjit::run(powerpc_cpu *cpu, bool &invalidated_cache)
{
	powerpc_registers *regs = &cpu->regs();
	for (;;) {
		if (link_count >= MAX_LINKS)
			flush_pending = true;			// out of link records
		if (flush_pending) {
			if (native_depth > 0) {
				sync_code();
				return false;
			}
			flush_all();
		}
		if (inval_count)
			process_invalidations();
		const uint32 pc = regs->pc;
		jit_block *b = find_block(pc);
		uint32 *entry = b ? b->entry : compile_block(cpu, pc);
		if (entry == NULL) {
			sync_code();
			return false;
		}
		jc_set(pc, entry);
		sync_code();
		st_dispatch++;

		native_depth++;
		uint32 *site = (uint32 *)stub_enter(regs, entry, cpu, rom_table);
		native_depth--;
		if (site == (uint32 *)1) {
			miss_ring[st_jcmiss & 1023] = regs->pc;
			st_jcmiss++;
			site = NULL;
		}
		else if (site)
			st_linkexit++;
		else
			st_spcexit++;
		const uint32 exit_pc = regs->pc;
		const uint32 generation = st_flushes;

		if (!cpu->spcflags().empty()) {
			if (!cpu->check_spcflags()) {
				sync_code();
				return true;
			}
			if (cpu->spcflags().test(SPCFLAG_JIT_EXEC_RETURN)) {
				cpu->spcflags().clear(SPCFLAG_JIT_EXEC_RETURN);
				invalidated_cache = true;
			}
		}

		// Chain the exit we came from to its target block (unless the cache
		// was flushed meanwhile, e.g. by an interrupt routine)
		if (site && !flush_pending && generation == st_flushes) {
			jit_block *t = find_block(exit_pc);
			uint32 *target = t ? t->entry : compile_block(cpu, exit_pc);
			if (target == NULL || generation != st_flushes)
				continue;
			if (inval_count)
				process_invalidations();
			if ((*site & 0x0f000000) == 0x0a000000 && link_count < MAX_LINKS &&
				(uintptr)site >= (uintptr)blocks_start && (uintptr)site < (uintptr)jp) {
				jit_block *tb = find_block(exit_pc);
				if (tb && tb->entry == target) {
					*site = b_insn(*site >> 28, site, target);
					mark_dirty(site, site + 1);
					jit_link *l = &link_pool[link_count++];
					l->site = site;
					l->next_in = tb->in_links;
					tb->in_links = l;
					st_links++;
				}
			}
			jc_set(exit_pc, target);
		}
	}
}

__attribute__((target ("general-regs-only")))
void ppc_armjit::sample(uint32 arm_pc)
{
	uint32 n = arm_sample_n;
	if (sampling && n < ARM_SAMPLES) {
		arm_samples[n] = arm_pc;
		arm_sample_n = n + 1;
	}
}

// Report the hottest translated blocks (from the 60 Hz samples)
static void log_hot_blocks(powerpc_cpu *cpu)
{
	const uint32 n = arm_sample_n;
	if (n < 200)
		return;
	static uint32 *hits;
	if (hits == NULL)
		hits = new uint32[MAX_BLOCKS];
	memset(hits, 0, MAX_BLOCKS * sizeof(uint32));
	uint32 in_jit = 0, in_stubs = 0;
	for (uint32 i = 0; i < n; i++) {
		uint32 *a = (uint32 *)arm_samples[i];
		if (a < buf_start || a >= jp)
			continue;
		in_jit++;
		if (a < blocks_start) {
			in_stubs++;
			continue;
		}
		// Blocks are allocated in increasing address order
		int lo = 0, hi = blk_count - 1, found = -1;
		while (lo <= hi) {
			int mid = (lo + hi) / 2;
			if (blk_pool[mid].entry <= a) {
				found = mid;
				lo = mid + 1;
			}
			else
				hi = mid - 1;
		}
		if (found >= 0)
			hits[found]++;
	}
	rpi_log("JIT samples: %u, %u%% in translated code (%u in stubs), rest in the kernel/interpreter\n",
			n, in_jit * 100 / n, in_stubs);
	// Most frequent ARM addresses outside translated code (64 byte buckets)
	{
		static uint32 v[ARM_SAMPLES];
		uint32 m = 0;
		for (uint32 i = 0; i < n; i++) {
			uint32 *a = (uint32 *)arm_samples[i];
			if (a < buf_start || a >= jp)
				v[m++] = arm_samples[i] & ~63u;
		}
		for (int k = 0; k < 10; k++) {
			uint32 best = 0, best_count = 0;
			for (uint32 i = 0; i < m; i++) {
				if (v[i] == 0)
					continue;
				uint32 c = 0;
				for (uint32 j = i; j < m; j++)
					if (v[j] == v[i])
						c++;
				if (c > best_count) {
					best_count = c;
					best = v[i];
				}
			}
			if (best_count == 0)
				break;
			for (uint32 i = 0; i < m; i++)
				if (v[i] == best)
					v[i] = 0;
			rpi_log("  arm %08x: %u%%\n", best, best_count * 100 / n);
		}
	}
	for (int k = 0; k < 12; k++) {
		int best = -1;
		for (int i = 0; i < blk_count; i++)
			if (hits[i] && (best < 0 || hits[i] > hits[best]))
				best = i;
		if (best < 0)
			break;
		jit_block *b = &blk_pool[best];
		rpi_log(" %2u%% block %08x-%08x%s\n", hits[best] * 100 / n, b->pc == 1 ? 0 : b->pc, b->end_pc,
				b->pc == 1 ? " (dead)" : "");
		hits[best] = 0;
		if (b->pc != 1 && k < 6) {
			for (uint32 pc = b->pc; pc <= b->end_pc && pc < b->pc + 4 * 24; pc += 4) {
				uint32 op = vm_read_memory_4(pc);
				ppc_armjit::insn_info info;
				ppc_armjit::decode(cpu, op, info);
				rpi_log("      %08x %08x %s\n", pc, op, info.name);
			}
			if (k < 4) {
				// ARM code of the hottest blocks (for objdump -b binary)
				uint32 *end = (best + 1 < blk_count) ? blk_pool[best + 1].entry : jp;
				if (end - b->entry > 96)
					end = b->entry + 96;
				char line[100];
				int pos = 0;
				for (uint32 *a = b->entry; a < end; a++) {
					pos += sprintf(line + pos, "%08x ", *a);
					if (pos > 80 || a + 1 == end) {
						rpi_log("    ARM %s\n", line);
						pos = 0;
					}
				}
			}
		}
	}
	arm_sample_n = 0;
}

void ppc_armjit::log_stats(void)
{
	if (!active)
		return;
	if (sampling)
		log_hot_blocks(the_cpu);
	if (profiling) {
		// Top instructions by execution count
		uint64 total = 0, total_fb = 0;
		for (int i = 0; i < MAX_MNEMO; i++) {
			total += prof_exec[i];
			total_fb += prof_fallback[i];
		}
		rpi_log("JIT profile: %u M instructions, %u.%u%% via interpreter\n", (uint32)(total / 1000000),
				total ? (uint32)(total_fb * 100 / total) : 0, total ? (uint32)(total_fb * 1000 / total % 10) : 0);
		static bool shown[MAX_MNEMO];
		memset(shown, 0, sizeof(shown));
		for (int n = 0; n < 40 && total; n++) {
			int best = -1;
			for (int i = 0; i < MAX_MNEMO; i++)
				if (!shown[i] && prof_exec[i] && (best < 0 || prof_exec[i] > prof_exec[best]))
					best = i;
			if (best < 0)
				break;
			shown[best] = true;
			const uint32 fb = (uint32)((uint64)prof_fallback[best] * 1000 / total);
			rpi_log("  %-10s %5u.%u%%  %s%u.%u%%\n", prof_name[best] ? prof_name[best] : "?",
					(uint32)((uint64)prof_exec[best] * 100 / total), (uint32)((uint64)prof_exec[best] * 1000 / total % 10),
					fb ? "interpreted " : "", fb / 10, fb % 10);
		}
		for (int k = 0; k < 2; k++)
			for (int i = 0; i < 1024; i++)
				if (prof_spr_fb[k][i] && (uint64)prof_spr_fb[k][i] * 1000 / total)
					rpi_log("  %s spr %d: %u.%u%%\n", k ? "mtspr" : "mfspr", i,
							(uint32)((uint64)prof_spr_fb[k][i] * 100 / total), (uint32)((uint64)prof_spr_fb[k][i] * 1000 / total % 10));
	}
	rpi_log("JIT: %u blocks (%u insns, %u fallbacks), %u KB code, %u flushes, %u links, %u invalidations (%u blocks)\n",
			st_compiles, st_insns, st_fallbacks, (uint32)((jp - blocks_start) * 4 / 1024),
			st_flushes, st_links, st_invalidates, st_killed);
	rpi_log("JIT: %u dispatches: %u jump cache misses, %u link exits, %u other\n",
			st_dispatch, st_jcmiss, st_linkexit, st_spcexit);
	if (profiling && st_jcmiss >= 1024) {
		// Most frequent recent miss targets
		static uint32 v[1024];
		memcpy(v, miss_ring, sizeof(v));
		for (int n = 0; n < 8; n++) {
			uint32 best = 0, best_count = 0;
			for (int i = 0; i < 1024; i++) {
				if (v[i] == 0)
					continue;
				uint32 c = 0;
				for (int j = i; j < 1024; j++)
					if (v[j] == v[i])
						c++;
				if (c > best_count) {
					best_count = c;
					best = v[i];
				}
			}
			if (best_count == 0)
				break;
			for (int i = 0; i < 1024; i++)
				if (v[i] == best)
					v[i] = 0;
			jit_block *b = find_block(best);
			rpi_log("  miss %08x x%u (jc slot %d, %s)\n", best, best_count, jc_index(best),
					b ? (jcache[jc_index(best)].pc == best || jcache[jc_index(best) + 1].pc == best ? "cached now" : "compiled") : "not compiled");
		}
	}
}


/*
 *  Self test: translated code vs. interpreter with random inputs
 */

static uint32 rnd_state = 0x12345678;

static uint32 rnd(void)
{
	uint32 x = rnd_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return rnd_state = x;
}

static uint32 rnd_value(void)
{
	switch (rnd() % 10) {
	case 0: return 0;
	case 1: return 0xffffffff;
	case 2: return 0x80000000;
	case 3: return 0x7fffffff;
	case 4: return rnd() % 64;
	case 5: return -(rnd() % 64);
	case 6: return rnd() & 0xffff;
	default: return rnd();
	}
}

enum { T_ALU, T_LOAD_D, T_LOAD_X, T_MULTI, T_BRANCH, T_SPR };

struct test_template {
	const char *name;
	uint32 base;		// fixed bits
	uint32 rmask;		// random bits
	int kind;
};

#define OPD(op)			((uint32)(op) << 26)
#define OPX(op, xo)		(((uint32)(op) << 26) | ((uint32)(xo) << 1))

static const test_template templates[] = {
	{ "addi",   OPD(14), 0x03ffffff, T_ALU },
	{ "addis",  OPD(15), 0x03ffffff, T_ALU },
	{ "addic",  OPD(12), 0x03ffffff, T_ALU },
	{ "addic.", OPD(13), 0x03ffffff, T_ALU },
	{ "subfic", OPD(8),  0x03ffffff, T_ALU },
	{ "mulli",  OPD(7),  0x03ffffff, T_ALU },
	{ "cmpi",   OPD(11), 0x039fffff, T_ALU },
	{ "cmpli",  OPD(10), 0x039fffff, T_ALU },
	{ "ori",    OPD(24), 0x03ffffff, T_ALU },
	{ "oris",   OPD(25), 0x03ffffff, T_ALU },
	{ "xori",   OPD(26), 0x03ffffff, T_ALU },
	{ "xoris",  OPD(27), 0x03ffffff, T_ALU },
	{ "andi.",  OPD(28), 0x03ffffff, T_ALU },
	{ "andis.", OPD(29), 0x03ffffff, T_ALU },
	{ "rlwimi", OPD(20), 0x03ffffff, T_ALU },
	{ "rlwinm", OPD(21), 0x03ffffff, T_ALU },
	{ "rlwnm",  OPD(23), 0x03ffffff, T_ALU },
	{ "add",    OPX(31, 266), 0x03fff801, T_ALU },
	{ "addc",   OPX(31, 10),  0x03fff801, T_ALU },
	{ "adde",   OPX(31, 138), 0x03fff801, T_ALU },
	{ "addze",  OPX(31, 202), 0x03fff801, T_ALU },
	{ "addme",  OPX(31, 234), 0x03fff801, T_ALU },
	{ "subf",   OPX(31, 40),  0x03fff801, T_ALU },
	{ "subfc",  OPX(31, 8),   0x03fff801, T_ALU },
	{ "subfe",  OPX(31, 136), 0x03fff801, T_ALU },
	{ "subfze", OPX(31, 200), 0x03fff801, T_ALU },
	{ "subfme", OPX(31, 232), 0x03fff801, T_ALU },
	{ "neg",    OPX(31, 104), 0x03fff801, T_ALU },
	{ "mullw",  OPX(31, 235), 0x03fff801, T_ALU },
	{ "mulhw",  OPX(31, 75),  0x03fff801, T_ALU },
	{ "mulhwu", OPX(31, 11),  0x03fff801, T_ALU },
	{ "divw",   OPX(31, 491), 0x03fff801, T_ALU },
	{ "divwu",  OPX(31, 459), 0x03fff801, T_ALU },
	{ "and",    OPX(31, 28),  0x03fff801, T_ALU },
	{ "andc",   OPX(31, 60),  0x03fff801, T_ALU },
	{ "or",     OPX(31, 444), 0x03fff801, T_ALU },
	{ "orc",    OPX(31, 412), 0x03fff801, T_ALU },
	{ "xor",    OPX(31, 316), 0x03fff801, T_ALU },
	{ "nor",    OPX(31, 124), 0x03fff801, T_ALU },
	{ "nand",   OPX(31, 476), 0x03fff801, T_ALU },
	{ "eqv",    OPX(31, 284), 0x03fff801, T_ALU },
	{ "slw",    OPX(31, 24),  0x03fff801, T_ALU },
	{ "srw",    OPX(31, 536), 0x03fff801, T_ALU },
	{ "sraw",   OPX(31, 792), 0x03fff801, T_ALU },
	{ "srawi",  OPX(31, 824), 0x03fff801, T_ALU },
	{ "cntlzw", OPX(31, 26),  0x03ff0001, T_ALU },
	{ "extsb",  OPX(31, 954), 0x03ff0001, T_ALU },
	{ "extsh",  OPX(31, 922), 0x03ff0001, T_ALU },
	{ "cmp",    OPX(31, 0),   0x039ff800, T_ALU },
	{ "cmpl",   OPX(31, 32),  0x039ff800, T_ALU },
	{ "crand",  OPX(19, 257), 0x03fff800, T_ALU },
	{ "crandc", OPX(19, 129), 0x03fff800, T_ALU },
	{ "creqv",  OPX(19, 289), 0x03fff800, T_ALU },
	{ "crnand", OPX(19, 225), 0x03fff800, T_ALU },
	{ "crnor",  OPX(19, 33),  0x03fff800, T_ALU },
	{ "cror",   OPX(19, 449), 0x03fff800, T_ALU },
	{ "crorc",  OPX(19, 417), 0x03fff800, T_ALU },
	{ "crxor",  OPX(19, 193), 0x03fff800, T_ALU },
	{ "mcrf",   OPX(19, 0),   0x039c0000, T_ALU },
	{ "mfcr",   OPX(31, 19),  0x03e00000, T_ALU },
	{ "mtcrf",  OPX(31, 144), 0x03eff000, T_ALU },
	{ "addo",   OPX(31, 266) | 0x400, 0x03fff801, T_ALU },
	{ "addco",  OPX(31, 10) | 0x400,  0x03fff801, T_ALU },
	{ "addeo",  OPX(31, 138) | 0x400, 0x03fff801, T_ALU },
	{ "subfo",  OPX(31, 40) | 0x400,  0x03fff801, T_ALU },
	{ "subfco", OPX(31, 8) | 0x400,   0x03fff801, T_ALU },
	{ "subfeo", OPX(31, 136) | 0x400, 0x03fff801, T_ALU },
	{ "addzeo", OPX(31, 202) | 0x400, 0x03fff801, T_ALU },
	{ "addmeo", OPX(31, 234) | 0x400, 0x03fff801, T_ALU },
	{ "subfzeo", OPX(31, 200) | 0x400, 0x03fff801, T_ALU },
	{ "subfmeo", OPX(31, 232) | 0x400, 0x03fff801, T_ALU },
	{ "nego",   OPX(31, 104) | 0x400, 0x03fff801, T_ALU },
	{ "mullwo", OPX(31, 235) | 0x400, 0x03fff801, T_ALU },
	{ "mcrxr",  OPX(31, 512), 0x03800000, T_ALU },
	{ "mfspr",  OPX(31, 339), 0x03e00000, T_SPR },
	{ "mtspr",  OPX(31, 467), 0x03e00000, T_SPR },
	{ "lwz",    OPD(32), 0x03ffffff, T_LOAD_D },
	{ "lwzu",   OPD(33), 0x03ffffff, T_LOAD_D },
	{ "lbz",    OPD(34), 0x03ffffff, T_LOAD_D },
	{ "lbzu",   OPD(35), 0x03ffffff, T_LOAD_D },
	{ "stw",    OPD(36), 0x03ffffff, T_LOAD_D },
	{ "stwu",   OPD(37), 0x03ffffff, T_LOAD_D },
	{ "stb",    OPD(38), 0x03ffffff, T_LOAD_D },
	{ "stbu",   OPD(39), 0x03ffffff, T_LOAD_D },
	{ "lhz",    OPD(40), 0x03ffffff, T_LOAD_D },
	{ "lhzu",   OPD(41), 0x03ffffff, T_LOAD_D },
	{ "lha",    OPD(42), 0x03ffffff, T_LOAD_D },
	{ "lhau",   OPD(43), 0x03ffffff, T_LOAD_D },
	{ "sth",    OPD(44), 0x03ffffff, T_LOAD_D },
	{ "sthu",   OPD(45), 0x03ffffff, T_LOAD_D },
	{ "lfd",    OPD(50), 0x03ffffff, T_LOAD_D },
	{ "lfdu",   OPD(51), 0x03ffffff, T_LOAD_D },
	{ "stfd",   OPD(54), 0x03ffffff, T_LOAD_D },
	{ "stfdu",  OPD(55), 0x03ffffff, T_LOAD_D },
	{ "lfdx",   OPX(31, 599), 0x03fff800, T_LOAD_X },
	{ "lfdux",  OPX(31, 631), 0x03fff800, T_LOAD_X },
	{ "stfdx",  OPX(31, 727), 0x03fff800, T_LOAD_X },
	{ "stfdux", OPX(31, 759), 0x03fff800, T_LOAD_X },
	{ "lmw",    OPD(46), 0x03ffffff, T_MULTI },
	{ "stmw",   OPD(47), 0x03ffffff, T_MULTI },
	{ "lwzx",   OPX(31, 23),  0x03fff800, T_LOAD_X },
	{ "lwzux",  OPX(31, 55),  0x03fff800, T_LOAD_X },
	{ "lbzx",   OPX(31, 87),  0x03fff800, T_LOAD_X },
	{ "lbzux",  OPX(31, 119), 0x03fff800, T_LOAD_X },
	{ "stwx",   OPX(31, 151), 0x03fff800, T_LOAD_X },
	{ "stwux",  OPX(31, 183), 0x03fff800, T_LOAD_X },
	{ "stbx",   OPX(31, 215), 0x03fff800, T_LOAD_X },
	{ "stbux",  OPX(31, 247), 0x03fff800, T_LOAD_X },
	{ "lhzx",   OPX(31, 279), 0x03fff800, T_LOAD_X },
	{ "lhzux",  OPX(31, 311), 0x03fff800, T_LOAD_X },
	{ "lhax",   OPX(31, 343), 0x03fff800, T_LOAD_X },
	{ "lhaux",  OPX(31, 375), 0x03fff800, T_LOAD_X },
	{ "sthx",   OPX(31, 407), 0x03fff800, T_LOAD_X },
	{ "sthux",  OPX(31, 439), 0x03fff800, T_LOAD_X },
	{ "lwbrx",  OPX(31, 534), 0x03fff800, T_LOAD_X },
	{ "lwarx",  OPX(31, 20),  0x03fff800, T_LOAD_X },
	{ "stwcx.", OPX(31, 150) | 1, 0x03fff800, T_LOAD_X },
	{ "stwbrx", OPX(31, 662), 0x03fff800, T_LOAD_X },
	{ "lhbrx",  OPX(31, 790), 0x03fff800, T_LOAD_X },
	{ "sthbrx", OPX(31, 918), 0x03fff800, T_LOAD_X },
	{ "b",      OPD(18), 0x03ffffff, T_BRANCH },
	{ "bc",     OPD(16), 0x03ffffff, T_BRANCH },
	{ "bclr",   OPX(19, 16),  0x03ff0001, T_BRANCH },
	{ "bcctr",  OPX(19, 528), 0x03ff0001, T_BRANCH },
};

#define TEST_WINDOW	4096

int ppc_armjit::selftest(powerpc_cpu *cpu, uint32 scratch, int iterations)
{
	if (!active)
		return 0;

	powerpc_registers *regs = &cpu->regs();
	powerpc_registers saved, s0, s_int, s_jit;
	memcpy(&saved, regs, sizeof(saved));

	const uint32 code_pc = scratch;				// pc used for the tested instruction
	const uint32 win = scratch + 0x1000;		// data window
	static uint8 m0[TEST_WINDOW], m_int[TEST_WINDOW];
	uint8 *mem = Mac2HostAddr(win);

	int failures = 0;
	uint64 t0 = rpi_ticks_usec();
	const int ntemplates = sizeof(templates) / sizeof(templates[0]);
	for (int t = 0; t < ntemplates; t++) {
		const test_template &tt = templates[t];
		int tfail = 0;
		for (int it = 0; it < iterations; it++) {
			uint32 op = tt.base | (rnd() & tt.rmask);

			// Random state
			memcpy(&s0, &saved, sizeof(s0));
			for (int i = 0; i < 32; i++) {
				s0.gpr[i] = rnd_value();
				s0.fpr[i].j = ((uint64)rnd() << 32) | rnd();
			}
			s0.cr.set(rnd());
			s0.xer.set(rnd() & 0xe000007f);
			s0.lr = rnd_value();
			s0.ctr = rnd() % 4 == 0 ? rnd() % 3 : rnd_value();
			s0.pc = code_pc;
			s0.spcflags.init();
			for (int i = 0; i < TEST_WINDOW; i++)
				m0[i] = rnd();

			switch (tt.kind) {
			case T_SPR: {
				static const uint32 sprs[3] = { 1, 8, 9 };
				uint32 spr = sprs[rnd() % 3];
				op |= ((spr & 31) << 16) | ((spr >> 5) << 11);
				break;
			}
			case T_LOAD_D:
			case T_MULTI: {
				int ra = (op >> 16) & 31;
				if (ra == 0) {
					ra = 1 + rnd() % 31;
					op |= ra << 16;
				}
				int32 d = (int32)(int16)(op & 0xffff);
				d = (d % 1024);
				op = (op & 0xffff0000) | (d & 0xffff);
				uint32 ea = win + 512 + rnd() % (TEST_WINDOW - 1024);
				if (tt.kind == T_MULTI)
					ea = win + 512 + (rnd() % 256) * 4 + (rnd() % 8 == 0 ? rnd() % 4 : 0);
				s0.gpr[ra] = ea - d;
				break;
			}
			case T_LOAD_X: {
				int ra = (op >> 16) & 31, rb = (op >> 11) & 31;
				if (ra == 0) {
					ra = 1 + rnd() % 31;
					op |= ra << 16;
				}
				if (rb == ra) {
					rb = (ra % 31) + 1;
					op = (op & ~(31u << 11)) | (rb << 11);
				}
				uint32 ea = win + 512 + rnd() % (TEST_WINDOW - 1024);
				uint32 off = rnd() % 512;
				s0.gpr[rb] = off;
				s0.gpr[ra] = ea - off;
				s0.reserve_valid = rnd() & 1;
				s0.reserve_addr = (rnd() & 1) ? ea : ea + 4 * (1 + rnd() % 4);
				break;
			}
			}

			insn_info info;
			decode(cpu, op, info);

			// Interpreter
			memcpy(regs, &s0, sizeof(s0));
			memcpy(mem, m0, TEST_WINDOW);
			((handler_fn)info.handler)(cpu, op);
			memcpy(&s_int, regs, sizeof(s_int));
			memcpy(m_int, mem, TEST_WINDOW);

			// Translated code (one instruction, then exit)
			memcpy(regs, &s0, sizeof(s0));
			memcpy(mem, m0, TEST_WINDOW);
			WriteMacInt32(code_pc, op);
			jp = blocks_start;
			uint32 end_pc;
			uint32 *entry = translate_block(cpu, code_pc, end_pc, true);
			mark_dirty(entry, jp);
			sync_code();
			jc_clear();
			stub_enter(regs, entry, cpu, rom_table);
			memcpy(&s_jit, regs, sizeof(s_jit));

			// Compare
			char why[96];
			why[0] = 0;
			for (int i = 0; i < 32 && !why[0]; i++)
				if (s_int.gpr[i] != s_jit.gpr[i])
					sprintf(why, "r%d %08x/%08x", i, s_int.gpr[i], s_jit.gpr[i]);
			for (int i = 0; i < 32 && !why[0]; i++)
				if (s_int.fpr[i].j != s_jit.fpr[i].j)
					sprintf(why, "f%d", i);
			for (int i = 0; i < 32 && !why[0]; i++)
			if (s_int.fpr[i].j != s_jit.fpr[i].j)
				sprintf(why, "f%d", i);
		if (!why[0] && s_int.cr.get() != s_jit.cr.get())
				sprintf(why, "cr %08x/%08x", s_int.cr.get(), s_jit.cr.get());
			if (!why[0] && s_int.xer.get() != s_jit.xer.get())
				sprintf(why, "xer %08x/%08x", s_int.xer.get(), s_jit.xer.get());
			if (!why[0] && s_int.lr != s_jit.lr)
				sprintf(why, "lr %08x/%08x", s_int.lr, s_jit.lr);
			if (!why[0] && s_int.ctr != s_jit.ctr)
				sprintf(why, "ctr %08x/%08x", s_int.ctr, s_jit.ctr);
			if (!why[0] && (s_int.reserve_valid != s_jit.reserve_valid || (s_int.reserve_valid && s_int.reserve_addr != s_jit.reserve_addr)))
				sprintf(why, "reservation %u %08x/%u %08x", s_int.reserve_valid, s_int.reserve_addr, s_jit.reserve_valid, s_jit.reserve_addr);
			if (!why[0] && s_int.pc != s_jit.pc)
				sprintf(why, "pc %08x/%08x", s_int.pc, s_jit.pc);
			if (!why[0] && memcmp(m_int, mem, TEST_WINDOW) != 0)
				sprintf(why, "memory");
			if (why[0]) {
				if (tfail < 3)
					rpi_log("JIT test %s %08x: %s\n", tt.name, op, why);
				tfail++;
			}
		}
		failures += tfail;
	}
	// Random instruction sequences: register reuse, compare + branch
	// fusion, block exits
	int seq_fail = 0;
	const int seq_count = iterations * 20;
	for (int it = 0; it < seq_count; it++) {
		uint32 ops[40];
		int len = 1 + rnd() % 12;
		for (int k = 0; k < len; k++) {
			if (rnd() % 6 == 0 && k + 2 < len) {
				// Forward branch over an instruction that must not run
				ops[k++] = OPD(18) | 8;									// b .+8
				ops[k] = OPD(14) | (3 << 21) | (3 << 16) | 0x1234;		// addi r3,r3,0x1234
				continue;
			}
			const test_template *tt;
			bool update;
			for (;;) {
				tt = &templates[rnd() % ntemplates];
				if (tt->kind == T_BRANCH || tt->kind == T_MULTI)
					continue;
				size_t nl = strlen(tt->name);
				update = (tt->kind == T_LOAD_D || tt->kind == T_LOAD_X) &&
						 (tt->name[nl - 1] == 'u' || (nl > 2 && tt->name[nl - 2] == 'u' && tt->name[nl - 1] == 'x'));
				if (update && tt->kind == T_LOAD_X)
					continue;
				break;
			}
			uint32 op = tt->base | (rnd() & tt->rmask);
			const uint32 primary = op >> 26;
			static const uint32 pool[10] = { 3, 4, 5, 6, 7, 8, 9, 10, 24, 27 };
			const uint32 rd = pool[rnd() % 10], ra = pool[rnd() % 10], rb = pool[rnd() % 10];
			switch (tt->kind) {
			case T_ALU:
				if (primary == 19)
					break;			// CR bit numbers: any
				op = (op & ~(31u << 21)) | (rd << 21);
				op = (op & ~(31u << 16)) | (ra << 16);
				if (primary == 31)
					op = (op & ~(31u << 11)) | (rb << 11);
				break;
			case T_SPR: {
				const uint32 spr = rnd() & 1 ? 8 : 9;
				op = (op & ~(31u << 21)) | ((spr & 31) << 16) | (rd << 21);
				break;
			}
			case T_LOAD_D:
				op = (op & 0xfc000000) | (rd << 21) | ((30 + (rnd() & 1)) << 16) | (rnd() % (update ? 64 : 1024));
				break;
			case T_LOAD_X:
				op = (op & 0xfc0007ff) | (rd << 21) | ((30 + (rnd() & 1)) << 16) | (29 << 11);
				break;
			}
			ops[k] = op;
		}
		// Terminating branch
		switch (rnd() % 4) {
		case 0:  ops[len] = OPD(18) | 4; break;									// b .+4
		case 1:  ops[len] = OPD(16) | (rnd() & 0x03ff0000) | ((1 + rnd() % 63) * 4) | (rnd() & 1); break;	// bc
		case 2:  ops[len] = OPX(19, 16) | (rnd() & 0x03ff0001); break;			// bclr
		default: ops[len] = OPD(16) | (12 << 21) | ((rnd() % 12) << 16) | 8; break;	// bc on cr0-2
		}
		len++;

		memcpy(&s0, &saved, sizeof(s0));
		for (int i = 0; i < 32; i++) {
			s0.gpr[i] = rnd_value();
			s0.fpr[i].j = ((uint64)rnd() << 32) | rnd();
		}
		s0.gpr[29] = rnd() % 256;
		s0.gpr[30] = win + 512;
		s0.gpr[31] = win + 1536;
		s0.reserve_valid = rnd() & 1;
		s0.reserve_addr = win + 512 + 4 * (rnd() % 64);
		s0.cr.set(rnd());
		s0.xer.set(rnd() & 0xe000007f);
		s0.lr = rnd_value();
		if (s0.lr - code_pc < 4u * len)
			s0.lr = code_pc + 0x800;
		s0.ctr = rnd() % 4 == 0 ? rnd() % 3 : rnd_value();
		s0.pc = code_pc;
		s0.spcflags.init();
		for (int i = 0; i < TEST_WINDOW; i++)
			m0[i] = rnd();
		for (int k = 0; k < len; k++)
			WriteMacInt32(code_pc + 4 * k, ops[k]);
		WriteMacInt32(code_pc + 4 * len, OPD(18));		// b . (stops followed branches)

		// Interpreter (follows the control flow inside the sequence)
		memcpy(regs, &s0, sizeof(s0));
		memcpy(mem, m0, TEST_WINDOW);
		for (int steps = 0; regs->pc - code_pc < 4u * len && steps < 100; steps++) {
			uint32 op = ReadMacInt32(regs->pc);
			insn_info info;
			decode(cpu, op, info);
			((handler_fn)info.handler)(cpu, op);
		}
		memcpy(&s_int, regs, sizeof(s_int));
		memcpy(m_int, mem, TEST_WINDOW);

		// Translated blocks
		memcpy(regs, &s0, sizeof(s0));
		memcpy(mem, m0, TEST_WINDOW);
		for (int steps = 0; regs->pc - code_pc < 4u * len && steps < 100; steps++) {
			jp = blocks_start;
			uint32 end_pc;
			uint32 *entry = translate_block(cpu, regs->pc, end_pc);
			mark_dirty(entry, jp);
			sync_code();
			jc_clear();
			stub_enter(regs, entry, cpu, rom_table);
		}
		memcpy(&s_jit, regs, sizeof(s_jit));

		char why[96];
		why[0] = 0;
		for (int i = 0; i < 32 && !why[0]; i++)
			if (s_int.gpr[i] != s_jit.gpr[i])
				sprintf(why, "r%d %08x/%08x", i, s_int.gpr[i], s_jit.gpr[i]);
		for (int i = 0; i < 32 && !why[0]; i++)
			if (s_int.fpr[i].j != s_jit.fpr[i].j)
				sprintf(why, "f%d", i);
		if (!why[0] && s_int.cr.get() != s_jit.cr.get())
			sprintf(why, "cr %08x/%08x", s_int.cr.get(), s_jit.cr.get());
		if (!why[0] && s_int.xer.get() != s_jit.xer.get())
			sprintf(why, "xer %08x/%08x", s_int.xer.get(), s_jit.xer.get());
		if (!why[0] && s_int.lr != s_jit.lr)
			sprintf(why, "lr %08x/%08x", s_int.lr, s_jit.lr);
		if (!why[0] && s_int.ctr != s_jit.ctr)
			sprintf(why, "ctr %08x/%08x", s_int.ctr, s_jit.ctr);
		if (!why[0] && (s_int.reserve_valid != s_jit.reserve_valid || (s_int.reserve_valid && s_int.reserve_addr != s_jit.reserve_addr)))
			sprintf(why, "reservation");
		if (!why[0] && s_int.pc != s_jit.pc)
			sprintf(why, "pc %08x/%08x", s_int.pc, s_jit.pc);
		if (!why[0] && memcmp(m_int, mem, TEST_WINDOW) != 0)
			sprintf(why, "memory");
		if (why[0]) {
			if (seq_fail < 4) {
				rpi_log("JIT sequence test: %s\n", why);
				for (int k = 0; k < len; k++) {
					insn_info info;
					decode(cpu, ops[k], info);
					rpi_log("   %-8s %08x\n", info.name, ops[k]);
				}
			}
			seq_fail++;
		}
	}
	failures += seq_fail;

	uint64 t1 = rpi_ticks_usec();

	memcpy(regs, &saved, sizeof(saved));
	flush_all();
	st_flushes = st_compiles = st_insns = st_fallbacks = st_links = st_dispatch = st_invalidates = 0;
	st_jcmiss = st_spcexit = st_linkexit = 0;
	rpi_log("JIT self test: %d instruction forms x %d, %d sequences, %d failures (%u ms)\n",
			ntemplates, iterations, seq_count, failures, (uint32)((t1 - t0) / 1000));
	return failures;
}
