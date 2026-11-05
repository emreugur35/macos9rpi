//
// rpi_mmu.cpp - MMU mapping of the Mac address space and the data abort
//               handler for macos9rpi
//
// The Mac address space is visible at host = mac + 0x80000000 (32-bit
// wrap-around). Circle identity-maps its own memory and the peripherals
// at the bottom of the address space; everything above that is free for
// us. Mac RAM, ROM, kernel data and SheepShaver's globals are mapped with
// 1 MB sections (and one 4 KB page table for the read-only zero page).
//
// Accesses of the emulated Mac to addresses that are not mapped (e.g.
// probing for hardware) or writes to the read-only ROM raise an ARM data
// abort. Like SheepShaver's SIGSEGV handler we skip such instructions
// (loads return 0).
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "rpi_glue.h"

#include <circle/memorymap.h>
#include <circle/synchronize.h>
#include <circle/exceptionstub.h>
#include <circle/bcm2835.h>
#include <circle/bcm2836.h>
#include <string.h>
#include <stdlib.h>

#if RASPPI == 1
#define CIRCLE_AREA_END		0x21000000U		// ARM_IO_END
#define SECTION_SHARED		0
#define PAGE_SHARED		0
#else
#define CIRCLE_AREA_END		0x40100000U		// ARM_LOCAL_END rounded up
#define SECTION_SHARED		(1U << 16)
#define PAGE_SHARED		(1U << 10)
#endif

// Short descriptor section: AP=01 (privileged RW), TEX=000 C=1 B=1 (write back)
#define SECTION_NORMAL		(0x0040EU | SECTION_SHARED)
// TEX=001 C=1 B=1: write back, write allocate (used for Mac memory and the
// frame buffer; the frame buffer is cleaned from the D-cache at 60 Hz)
#define SECTION_NORMAL_WA	(0x0140EU | SECTION_SHARED)
#define SECTION_XN		(1U << 4)
#define SECTION_APX		(1U << 15)			// with AP=01: read-only

// Small page (ARMv6 extended format, XP=1): AP=01, C=1, B=1
#define PAGE_NORMAL		(0x01EU | PAGE_SHARED)
#define PAGE_XN			(1U << 0)
#define PAGE_APX		(1U << 9)

#define MB			0x100000U

static inline u32 *PageTable (void)
{
	u32 nTTBR;
	asm volatile ("mrc p15, 0, %0, c2, c0, 0" : "=r" (nTTBR));
	return (u32 *) (nTTBR & ~0x3FFFU);
}

static void FlushTLB (void)
{
	DataSyncBarrier ();
	asm volatile ("mcr p15, 0, %0, c8, c7, 0" : : "r" (0) : "memory");	// invalidate unified TLB
#if RASPPI == 1
	asm volatile ("mcr p15, 0, %0, c7, c5, 6" : : "r" (0) : "memory");	// flush branch target cache
	asm volatile ("mcr p15, 0, %0, c7, c10, 4" : : "r" (0) : "memory");	// DSB
	asm volatile ("mcr p15, 0, %0, c7, c5, 4" : : "r" (0) : "memory");	// ISB
#else
	asm volatile ("mcr p15, 0, %0, c7, c5, 6" : : "r" (0) : "memory");	// BPIALL
	asm volatile ("dsb\n isb" ::: "memory");
#endif
}

static void SyncTableEntries (void *pStart, unsigned nBytes)
{
	CleanAndInvalidateDataCacheRange ((uintptr) pStart & ~31U, nBytes + 64);
	FlushTLB ();
}

// Bitmap of 1 MB sections that belong to the Mac (to recognize Mac faults)
static u32 s_MacSections[4096 / 32];

static void MarkMacSection (u32 nVirt)
{
	unsigned n = nVirt >> 20;
	s_MacSections[n / 32] |= 1U << (n % 32);
}

extern "C" int rpi_is_mac_mapping (uint32_t nVirt)
{
	unsigned n = nVirt >> 20;
	return (s_MacSections[n / 32] >> (n % 32)) & 1;
}

extern "C" int rpi_mmu_map (uint32_t nVirt, uint32_t nPhys, uint32_t nSize, int bReadOnly)
{
	if ((nVirt | nPhys | nSize) & (MB - 1))
		return 0;
	u32 *pTable = PageTable ();
	unsigned nFirst = nVirt >> 20;
	unsigned nCount = nSize >> 20;
	for (unsigned i = 0; i < nCount; i++)
	{
		u32 nEntryVirt = nVirt + i * MB;
		if (nEntryVirt < CIRCLE_AREA_END)
			return 0;	// never touch Circle's own mappings
		pTable[nFirst + i] =   ((nPhys + i * MB) & 0xFFF00000U)
				     | SECTION_NORMAL_WA | SECTION_XN
				     | (bReadOnly ? SECTION_APX : 0);
		MarkMacSection (nEntryVirt);
	}
	SyncTableEntries (&pTable[nFirst], nCount * 4);
	return 1;
}

extern "C" int rpi_mmu_protect (uint32_t nVirt, uint32_t nSize, int bReadOnly)
{
	u32 *pTable = PageTable ();
	unsigned nFirst = nVirt >> 20;
	unsigned nCount = (nSize + MB - 1) >> 20;
	for (unsigned i = 0; i < nCount; i++)
	{
		u32 nEntry = pTable[nFirst + i];
		if ((nEntry & 3) != 2)		// not a section
			return 0;
		if (bReadOnly)
			nEntry |= SECTION_APX;
		else
			nEntry &= ~SECTION_APX;
		pTable[nFirst + i] = nEntry;
	}
	SyncTableEntries (&pTable[nFirst], nCount * 4);
	return 1;
}

// Make a single 4 KB page read-only. Converts the containing 1 MB section
// into a second level (coarse) page table on first use.
extern "C" int rpi_mmu_protect_page (uint32_t nVirt, int bReadOnly)
{
	u32 *pTable = PageTable ();
	unsigned nSection = nVirt >> 20;
	u32 nEntry = pTable[nSection];
	u32 *pL2;

	if ((nEntry & 3) == 2)		// section: convert to coarse page table
	{
		u8 *pRaw = (u8 *) malloc (2048);	// never freed
		if (pRaw == 0)
			return 0;
		pL2 = (u32 *) (((uintptr) pRaw + 1023) & ~1023U);
		u32 nPhys = nEntry & 0xFFF00000U;
		bool bRO = (nEntry & SECTION_APX) != 0;
		for (unsigned i = 0; i < 256; i++)
			pL2[i] = (nPhys + i * 4096) | PAGE_NORMAL | PAGE_XN | (bRO ? PAGE_APX : 0);
		CleanAndInvalidateDataCacheRange ((uintptr) pL2, 1024);
		pTable[nSection] = ((uintptr) pL2 & 0xFFFFFC00U) | 0x01;	// coarse table, domain 0
	}
	else if ((nEntry & 3) == 1)
	{
		pL2 = (u32 *) (nEntry & 0xFFFFFC00U);
	}
	else
		return 0;

	unsigned nPage = (nVirt >> 12) & 0xFF;
	if (bReadOnly)
		pL2[nPage] |= PAGE_APX;
	else
		pL2[nPage] &= ~PAGE_APX;
	CleanAndInvalidateDataCacheRange ((uintptr) &pL2[nPage] & ~31U, 64);
	SyncTableEntries (&pTable[nSection], 4);
	return 1;
}

// Map a range that already belongs to Circle (the GPU framebuffer, which is
// Device memory by default) as normal write-back cacheable memory: the Mac
// does unaligned accesses (which fault on Device memory) and drawing is much
// faster with caching. rpi_clean_dcache() must be called regularly so the
// GPU sees the pixels.
extern "C" int rpi_mmu_set_fb_cached (uint32_t nAddr, uint32_t nSize)
{
	u32 *pTable = PageTable ();
	unsigned nFirst = nAddr >> 20;
	unsigned nLast = (nAddr + nSize - 1) >> 20;
	for (unsigned n = nFirst; n <= nLast; n++)
		pTable[n] = (n << 20) | SECTION_NORMAL_WA | SECTION_XN;
	SyncTableEntries (&pTable[nFirst], (nLast - nFirst + 1) * 4);
	return 1;
}

// Allow instruction fetches from a range of Circle's heap (the JIT code
// buffer). Circle maps the heap with 1 MB sections, execute never.
extern "C" int rpi_mmu_set_exec (uint32_t nAddr, uint32_t nSize)
{
	u32 *pTable = PageTable ();
	unsigned nFirst = nAddr >> 20;
	unsigned nLast = (nAddr + nSize - 1) >> 20;
	for (unsigned n = nFirst; n <= nLast; n++)
	{
		if ((pTable[n] & 3) != 2)
			return 0;
		pTable[n] &= ~SECTION_XN;
	}
	SyncTableEntries (&pTable[nFirst], (nLast - nFirst + 1) * 4);
	return 1;
}

// Write back the whole L1 data cache (16 KB, cheap); safe in IRQ context
extern "C" __attribute__((target ("general-regs-only"))) void rpi_clean_dcache (void)
{
#if RASPPI == 1
	asm volatile ("mcr p15, 0, %0, c7, c10, 0" : : "r" (0) : "memory");	// clean entire D-cache
	asm volatile ("mcr p15, 0, %0, c7, c10, 4" : : "r" (0) : "memory");	// DSB
#else
	CleanDataCache ();
#endif
}

// ---------------------------------------------------------------------------
//  Data abort handling
// ---------------------------------------------------------------------------

struct TMacAbortFrame
{
	u32 spsr;
	u32 r[13];	// r0..r12
	u32 pc;		// address of the faulting instruction
};

static volatile unsigned s_nSkippedLoads = 0;
static volatile unsigned s_nSkippedStores = 0;

extern "C" unsigned rpi_abort_stats (unsigned *pLoads, unsigned *pStores)
{
	*pLoads = s_nSkippedLoads;
	*pStores = s_nSkippedStores;
	return s_nSkippedLoads + s_nSkippedStores;
}

static inline u32 ShiftValue (u32 nValue, unsigned nType, unsigned nAmount, u32 nCarry)
{
	switch (nType)
	{
	case 0: return nAmount ? nValue << nAmount : nValue;				// LSL
	case 1: return nAmount ? nValue >> nAmount : 0;					// LSR (#0 = #32)
	case 2: return nAmount ? (u32) ((s32) nValue >> nAmount)
			       : ((s32) nValue < 0 ? 0xFFFFFFFFU : 0);		// ASR
	default: return nAmount ? (nValue >> nAmount) | (nValue << (32 - nAmount))
				: (nCarry << 31) | (nValue >> 1);			// ROR / RRX
	}
}

extern "C" __attribute__((target ("general-regs-only")))
int rpi_data_abort_handler (TMacAbortFrame *pFrame)
{
	u32 nFAR, nFSR;
	asm volatile ("mrc p15, 0, %0, c6, c0, 0" : "=r" (nFAR));
	asm volatile ("mrc p15, 0, %0, c5, c0, 0" : "=r" (nFSR));

	// Only ARM state
	if (pFrame->spsr & (1U << 5))
		return 0;

	// Fault status: translation (5 section, 7 page) or permission (13, 15)
	unsigned nStatus = (nFSR & 0xF) | ((nFSR >> 6) & 0x10);
	if (nStatus != 5 && nStatus != 7 && nStatus != 13 && nStatus != 15)
		return 0;

	// Only addresses outside of Circle's own area belong to the Mac
	if (nFAR < CIRCLE_AREA_END)
		return 0;

	u32 nInsn = *(u32 *) pFrame->pc;
	if ((nInsn >> 28) == 0xF)		// unconditional space (PLD etc.)
	{
		pFrame->pc += 4;
		return 1;
	}

	unsigned nRn = (nInsn >> 16) & 0xF;
	unsigned nRd = (nInsn >> 12) & 0xF;
	bool bLoad = (nInsn >> 20) & 1;
	u32 nCarry = (pFrame->spsr >> 29) & 1;

	if (((nInsn >> 26) & 3) == 1)		// LDR/STR/LDRB/STRB
	{
		bool bP = (nInsn >> 24) & 1, bU = (nInsn >> 23) & 1, bW = (nInsn >> 21) & 1;
		if (nRd >= 13 || (nRn >= 13 && (!bP || bW)))
			return 0;
		u32 nOffset;
		if (nInsn & (1U << 25))		// register offset
		{
			unsigned nRm = nInsn & 0xF;
			if (nRm >= 13)
				return 0;
			nOffset = ShiftValue (pFrame->r[nRm], (nInsn >> 5) & 3, (nInsn >> 7) & 0x1F, nCarry);
		}
		else
			nOffset = nInsn & 0xFFF;
		if (bLoad)
			pFrame->r[nRd] = 0;
		if (!bP || bW)
			pFrame->r[nRn] += bU ? nOffset : -nOffset;
	}
	else if ((nInsn & 0x0E000090) == 0x00000090 && (nInsn & 0x60) != 0)	// halfword / signed / doubleword
	{
		bool bP = (nInsn >> 24) & 1, bU = (nInsn >> 23) & 1, bW = (nInsn >> 21) & 1;
		unsigned nSH = (nInsn >> 5) & 3;
		bool bDouble = !bLoad && (nSH == 2 || nSH == 3);	// LDRD (SH=10) / STRD (SH=11)
		bool bDoubleLoad = bDouble && nSH == 2;
		if (nRd >= 13 || (bDouble && nRd >= 12) || (nRn >= 13 && (!bP || bW)))
			return 0;
		u32 nOffset;
		if (nInsn & (1U << 22))		// immediate
			nOffset = ((nInsn >> 4) & 0xF0) | (nInsn & 0xF);
		else
		{
			unsigned nRm = nInsn & 0xF;
			if (nRm >= 13)
				return 0;
			nOffset = pFrame->r[nRm];
		}
		if (bLoad || bDoubleLoad)
		{
			pFrame->r[nRd] = 0;
			if (bDoubleLoad)
				pFrame->r[nRd + 1] = 0;
		}
		if (!bP || bW)
			pFrame->r[nRn] += bU ? nOffset : -nOffset;
		if (bDouble)
			bLoad = bDoubleLoad;
	}
	else if (((nInsn >> 25) & 7) == 4)	// LDM/STM
	{
		bool bU = (nInsn >> 23) & 1, bW = (nInsn >> 21) & 1;
		u32 nList = nInsn & 0xFFFF;
		if (nList & 0xE000)		// sp, lr or pc in list
			return 0;
		if (nRn >= 13 && bW)
			return 0;
		unsigned nCount = __builtin_popcount (nList);
		if (bLoad)
			for (unsigned i = 0; i < 13; i++)
				if (nList & (1U << i))
					pFrame->r[i] = 0;
		if (bW && !(bLoad && (nList & (1U << nRn))))
			pFrame->r[nRn] += bU ? 4 * nCount : -4 * nCount;
	}
	else if (((nInsn >> 25) & 7) == 6)	// coprocessor load/store (VLDR/VSTR/VLDM/VSTM)
	{
		bool bP = (nInsn >> 24) & 1, bU = (nInsn >> 23) & 1, bW = (nInsn >> 21) & 1;
		if (bW)
		{
			if (nRn >= 13)
				return 0;
			u32 nOffset = (nInsn & 0xFF) * 4;
			if (!bP || bW)
				pFrame->r[nRn] += bU ? nOffset : -nOffset;
		}
		// the VFP register keeps its old contents
	}
	else
		return 0;

	if (bLoad)
		s_nSkippedLoads++;
	else
		s_nSkippedStores++;

	pFrame->pc += 4;
	return 1;
}

extern "C" void rpi_data_abort_stub (void);

extern "C" void rpi_install_abort_handler (void)
{
	TExceptionTable * volatile pTable = (TExceptionTable * volatile) ARM_EXCEPTION_TABLE_BASE;
	pTable->DataAbort = ARM_OPCODE_BRANCH (ARM_DISTANCE (pTable->DataAbort, rpi_data_abort_stub));
	SyncDataAndInstructionCache ();
}
