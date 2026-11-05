/*
 *  main_circle.cpp - Startup code for the bare-metal Raspberry Pi (Circle)
 *                    port of SheepShaver
 *
 *  Based on SheepShaver/src/Unix/main_unix.cpp
 *  SheepShaver (C) 1997-2008 Christian Bauer and Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Memory layout (Mac address -> ARM virtual address = mac + 0x80000000):
 *
 *    0x00000000  Mac RAM (ramsize)          -> 0x80000000
 *    0x50000000  Mac ROM area (5 MB)        -> 0xD0000000
 *    0x50500000  PPC interrupt stack (64K)  -> 0xD0500000
 *    0x50510000  SheepShaver globals (512K) -> 0xD0510000
 *    0x5fffe000  Kernel Data (alias 1)      -> 0xDFFFE000  \ same physical
 *    0x68ffe000  Kernel Data (alias 2)      -> 0xE8FFE000  / memory
 *    host memory (frame buffer etc.)        <- mac = host - 0x80000000
 */

#include "sysdeps.h"

#include "cpu_emulation.h"
#include "main.h"
#include "xlowmem.h"
#include "xpram.h"
#include "timer.h"
#include "adb.h"
#include "video.h"
#include "sys.h"
#include "prefs.h"
#include "prefs_editor.h"
#include "rom_patches.h"
#include "user_strings.h"
#include "vm_alloc.h"
#include "sigsegv.h"
#include "thunks.h"
#include "version.h"

#include "rpi_glue.h"
#include "ppc_armjit.h"

#define DEBUG 0
#include "debug.h"

extern "C" int rpi_mmu_protect_page(uint32_t virt, int readonly);
extern "C" unsigned rpi_abort_stats(unsigned *loads, unsigned *stores);
extern "C" void rpi_set_swap_opt_cmd(int swap);
extern bool PPCSelfTest(uint32 scratch);
extern void SysPeriodicFlush(void);
extern void SysCacheStats(uint32 *hits, uint32 *misses);
extern void SysReadStats(uint32 *cmds, uint32 *kb);
extern "C" int PPCProfileSample(uint32 *pc, uint32 *r24);
extern uint32 nqd_stats[6];

// Sampling profiler ("profile true" in macos9rpi.txt): one sample per tick
struct profile_sample { uint32 pc, r24, mode; };
static const uint32 PROFILE_MAX = 65536;
static profile_sample *profile_buf = NULL;
static volatile uint32 profile_n = 0;
extern "C" unsigned rpi_mouse_debug(int *dx, int *dy);

// Constants
const char ROM_FILE_NAME[] = "ROM";
const char ROM_FILE_NAME2[] = "Mac OS ROM";

const uintptr ROM_BASE = 0x50000000;		// Base address of ROM
const uint32 SIG_STACK_SIZE = 0x10000;		// Size of signal stack
const uint32 MB = 0x100000;


// Global variables (exported)
uint32 RAMBase;			// Base address of Mac RAM
uint32 RAMSize;			// Size of Mac RAM
uint32 ROMBase;			// Base address of Mac ROM
uint32 KernelDataAddr;	// Address of Kernel Data
uint32 BootGlobsAddr;	// Address of BootGlobs structure at top of Mac RAM
uint32 DRCacheAddr;		// Address of DR Cache
uint32 PVR;				// Theoretical PVR
int64 CPUClockSpeed;	// Processor clock speed (Hz)
int64 BusClockSpeed;	// Bus clock speed (Hz)
int64 TimebaseSpeed;	// Timebase clock speed (Hz)
uint8 *RAMBaseHost;		// Base address of Mac RAM (host address space)
uint8 *ROMBaseHost;		// Base address of Mac ROM (host address space)
uint32 ROMEnd;
char *vde_sock = NULL;

// Global variables
static KernelData *kernel_data;				// Pointer to Kernel Data
static EmulatorData *emulator_data;
static uint8 last_xpram[XPRAM_SIZE];		// Buffer for monitoring XPRAM changes
static uintptr sig_stack = 0;				// Stack for PowerPC interrupt routine
static bool emulator_running = false;

uint32  SheepMem::page_size;				// Size of a native page
uintptr SheepMem::zero_page = 0;			// Address of ro page filled in with zeros
uintptr SheepMem::base = 0x60000000;		// Address of SheepShaver data
uintptr SheepMem::proc;						// Bottom address of SheepShave procedures
uintptr SheepMem::data;						// Top of SheepShaver data (stack like storage)

bool tick_inhibit;

// Prototypes
static void Quit(void);
extern void emul_ppc(uint32 start);
extern void init_emul_ppc(void);
extern void exit_emul_ppc(void);


/*
 *  Return signal stack base
 */

uintptr SignalStackBase(void)
{
	return sig_stack + SIG_STACK_SIZE;
}


/*
 *  Atomic operations (IRQ safe)
 */

int atomic_add(int *var, int v)
{
	return __atomic_fetch_add(var, v, __ATOMIC_SEQ_CST);
}

int atomic_and(int *var, int v)
{
	return __atomic_fetch_and(var, v, __ATOMIC_SEQ_CST);
}

int atomic_or(int *var, int v)
{
	return __atomic_fetch_or(var, v, __ATOMIC_SEQ_CST);
}


/*
 *  Map a region of the Mac address space to freshly allocated memory
 */

static inline uint32 host_virt(uint32 mac_addr)
{
	return mac_addr + NATMEM_OFFSET;
}

static uint8 *map_mac_area(uint32 mac_addr, uint32 size, const char *what)
{
	size = (size + MB - 1) & ~(MB - 1);
	uint8 *phys = (uint8 *)rpi_alloc_1mb_aligned(size);
	if (phys == NULL) {
		rpi_fatal("Not enough memory for %s (%u MB)", what, size >> 20);
	}
	if (!rpi_mmu_map(host_virt(mac_addr), (uint32)phys, size, 0)) {
		rpi_fatal("Cannot map %s at Mac address %08x", what, mac_addr);
	}
	memset(Mac2HostAddr(mac_addr), 0, size);
	D(bug("%s: Mac %08x host %08x phys %p (%u MB)\n", what, mac_addr, host_virt(mac_addr), phys, size >> 20));
	return phys;
}


/*
 *  CPU information
 */

static void get_system_info(void)
{
	PVR = 0x000c0000;			// 7400 (with AltiVec)
	CPUClockSpeed = 100000000;	// 100MHz
	BusClockSpeed = 100000000;	// 100MHz
	TimebaseSpeed =  25000000;	// 25MHz

	int pref_cpu_clock = PrefsFindInt32("cpuclock");
	if (pref_cpu_clock)
		CPUClockSpeed = 1000000LL * pref_cpu_clock;

	// Remap any newer G4/G5 processor to plain G4 for compatibility
	switch (PVR >> 16) {
	case 0x8000: case 0x8001: case 0x8002: case 0x8003: case 0x8004:
	case 0x0039: case 0x003c: case 0x0044:
		PVR = 0x000c0000;
		break;
	}
}


/*
 *  Load Mac ROM from the SD card
 */

static bool load_mac_rom(void)
{
	const char *rom_path = PrefsFindString("rom");
	const char *names[4] = { rom_path, "Mac OS ROM", "ROM", "macos9rpi/Mac OS ROM" };
	FILE *f = NULL;
	const char *used = NULL;
	for (int i = 0; i < 4 && f == NULL; i++) {
		if (names[i] && *names[i]) {
			f = fopen(names[i], "rb");
			used = names[i];
		}
	}
	if (f == NULL) {
		ErrorAlert(GetString(STR_NO_ROM_FILE_ERR));
		return false;
	}
	rpi_log("Reading ROM file \"%s\"...\n", used);

	fseek(f, 0, SEEK_END);
	long rom_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	uint8 *rom_tmp = new uint8[ROM_SIZE];
	size_t actual = fread(rom_tmp, 1, ROM_SIZE, f);
	fclose(f);
	rpi_log("ROM file size %ld bytes\n", rom_size);

	// Decode Mac ROM
	if (!DecodeROM(rom_tmp, actual)) {
		delete[] rom_tmp;
		if (rom_size != 4*1024*1024)
			ErrorAlert(GetString(STR_ROM_SIZE_ERR));
		else
			ErrorAlert(GetString(STR_ROM_FILE_READ_ERR));
		return false;
	}
	delete[] rom_tmp;
	return true;
}


/*
 *  Check the configured disks before starting (helps when setting up the SD card)
 */

static void report_disks(void)
{
	const char *name;
	int n = 0;
	for (int i = 0; (name = PrefsFindString("disk", i)) != NULL; i++) {
		n++;
		void *fh = Sys_open(name, true);
		if (fh == NULL) {
			rpi_log("WARNING: disk \"%s\" cannot be opened\n", name);
			continue;
		}
		uint8 buf[512];
		if (Sys_read(fh, buf, 0, 512) == 512 && buf[0] == 'E' && buf[1] == 'R') {
			rpi_log("  -> Apple partitioned disk\n");
		} else if (Sys_read(fh, buf, 1024, 512) == 512) {
			if (buf[0] == 'B' && buf[1] == 'D') {
				char vname[28];
				int len = buf[0x24] < 27 ? buf[0x24] : 27;
				memcpy(vname, buf + 0x25, len);
				vname[len] = 0;
				bool wrapped = buf[0x7c] == 'H' && buf[0x7d] == '+';
				rpi_log("  -> %s volume \"%s\"\n", wrapped ? "HFS+" : "HFS", vname);
			} else if (buf[0] == 'H' && (buf[1] == '+' || buf[1] == 'X')) {
				rpi_log("  -> HFS+ volume\n");
			} else {
				rpi_log("  -> no Mac volume found (Mac OS will offer to initialize it)\n");
			}
		}
		Sys_close(fh);
	}
	if (n == 0)
		rpi_log("WARNING: no Mac disk configured (see macos9rpi.txt)\n");
}


/*
 *  60Hz tick (runs in IRQ context!)
 *  Only touches integer state; everything else happens in CirclePollEvents()
 */

static volatile uint32 tick_count = 0;

extern "C" uint32 IRQReturnAddress;	// Circle: interrupted pc (for profiling)
static bool jit_sampling;

__attribute__((target("general-regs-only")))
static void tick_func(void)
{
	tick_count++;

	if (jit_sampling)
		ppc_armjit::sample(IRQReturnAddress);

	// Push the Mac's drawing in the (cached) frame buffer out to the GPU
	rpi_clean_dcache();

	if (profile_buf && profile_n < PROFILE_MAX) {
		profile_sample *ps = &profile_buf[profile_n];
		if (PPCProfileSample(&ps->pc, &ps->r24)) {
			ps->mode = ReadMacInt32(XLM_RUN_MODE);
			profile_n++;
		}
	}

	if (tick_inhibit)
		return;

	// Trigger 60Hz interrupt
	if (ReadMacInt32(XLM_IRQ_NEST) == 0) {
		SetInterruptFlag(INTFLAG_VIA);
		TriggerInterrupt();
	}
}


/*
 *  Called from HandleInterrupt() in the emulator thread (main context)
 */

static void handle_input_events(void);
extern void EtherCirclePoll(void);	// ether_circle.cpp

void CirclePollEvents(void)
{
	static uint32 last_tick = 0;
	static uint32 last_second = 0;
	static uint32 last_usb_poll = 0;
	static uint32 last_nvram = 0;

	uint32 now = tick_count;

	// Pseudo Mac 1Hz interrupt, update local time
	if (now - last_second >= 60) {
		last_second = now;
		WriteMacInt32(0x20c, TimerDateTime());
	}

	// USB plug and play (every 0.25 s)
	if (now - last_usb_poll >= 15) {
		last_usb_poll = now;
		rpi_usb_poll();
	}

	// Write pending FAT metadata of disk image files (every 2 s)
	static uint32 last_flush = 0;
	if (now - last_flush >= 120) {
		last_flush = now;
		SysPeriodicFlush();
	}

	// QuickDraw acceleration statistics (serial log, every 10 s if changed)
	static uint32 last_nqd = 0, last_nqd_sum = 0;
	static const bool jit_profile = PrefsFindBool("jitprofile") || PrefsFindBool("jitsample");
	if ((profile_buf || jit_profile) && now - last_nqd >= 600) {
		last_nqd = now;
		uint32 sum = nqd_stats[0] + nqd_stats[1] + nqd_stats[3] + nqd_stats[5];
		if (sum != last_nqd_sum) {
			last_nqd_sum = sum;
			rpi_log("NQD: installed %u, bitblt %u/%u, fill %u/%u, other %u\n",
					nqd_stats[0], nqd_stats[2], nqd_stats[1], nqd_stats[4], nqd_stats[3], nqd_stats[5]);
		}
		ppc_armjit::log_stats();
	}

	// NVRAM watchdog (every minute)
	if (now - last_nvram >= 3600) {
		last_nvram = now;
		if (memcmp(last_xpram, XPRAM, XPRAM_SIZE)) {
			memcpy(last_xpram, XPRAM, XPRAM_SIZE);
			SaveXPRAM();
		}
	}

	if (now != last_tick) {
		last_tick = now;
		handle_input_events();
	}

	// Network (Wi-Fi / USB Ethernet): let its tasks run, collect frames
	EtherCirclePoll();
#if INPUT_DEBUG && 0		// (needs the declarations below)
	if (now - dbg_reported >= 120) {
		static unsigned last_reports = 0;
		int dx, dy;
		unsigned reports = rpi_mouse_debug(&dx, &dy);
		dbg_reported = now;
		if (reports != last_reports) {
			last_reports = reports;
			rpi_log("input: %u mouse reports, %u moves, last delta %d,%d -> %d,%d\n", reports, dbg_moves, dx, dy, mouse_x, mouse_y);
		}
	}
#endif
}


/*
 *  Feed USB input into the ADB emulation
 */

/*
 *  The mouse runs in absolute mode: we keep the cursor position ourselves
 *  (with acceleration) and the Mac cursor is set directly every tick. This
 *  follows the hand without the 7 bit/tick limit of relative ADB packets.
 */

static int mouse_x = -1, mouse_y = -1;		// absolute cursor position
static int mouse_fx = 0, mouse_fy = 0;		// sub-pixel remainders (1/256)
static int mouse_sent_x = -1, mouse_sent_y = -1;
static int mouse_speed = 100;				// percent ("mousespeed" pref)

static void mouse_move_by(int dx, int dy, bool accelerate)
{
	int w = VModes[cur_mode].viXsize, h = VModes[cur_mode].viYsize;
	if (mouse_x < 0) {
		mouse_x = w / 2;
		mouse_y = h / 2;
	}
	int gain = 256;
	if (accelerate) {
		int speed = dx < 0 ? -dx : dx;
		int sy = dy < 0 ? -dy : dy;
		if (sy > speed)
			speed = sy;
		gain = 256 + speed * 40;			// 1.0x slow ... 3.0x fast
		if (gain > 768)
			gain = 768;
		gain = gain * mouse_speed / 100;
	}
	mouse_fx += dx * gain;
	mouse_fy += dy * gain;
	int ix = mouse_fx / 256, iy = mouse_fy / 256;
	mouse_fx -= ix * 256;
	mouse_fy -= iy * 256;
	mouse_x += ix;
	mouse_y += iy;
	if (mouse_x < 0) mouse_x = 0; else if (mouse_x >= w) mouse_x = w - 1;
	if (mouse_y < 0) mouse_y = 0; else if (mouse_y >= h) mouse_y = h - 1;
}

#ifndef INPUT_DEBUG
#define INPUT_DEBUG 0
#endif
#if INPUT_DEBUG
static uint32 dbg_moves, dbg_buttons, dbg_keys, dbg_reported;
#endif

static void handle_input_events(void)
{
	rpi_input_event_t ev;
	while (rpi_input_get(&ev)) {
#if INPUT_DEBUG
		if (ev.type == RPI_EV_MOUSE_MOVE) dbg_moves++;
		else if (ev.type == RPI_EV_MOUSE_DOWN || ev.type == RPI_EV_MOUSE_UP) {
			dbg_buttons++;
			rpi_log("input: mouse button %d %s\n", ev.code, ev.type == RPI_EV_MOUSE_DOWN ? "down" : "up");
		} else if (ev.type == RPI_EV_KEY_DOWN || ev.type == RPI_EV_KEY_UP) {
			dbg_keys++;
			rpi_log("input: key %02x %s\n", ev.code, ev.type == RPI_EV_KEY_DOWN ? "down" : "up");
		}
		// Debug hotkeys (QEMU's usb-mouse does not deliver motion to Circle):
		// F11 = cursor to top left, F12 = move by (+100,+60), F10 = move by (+8,+4)
		if (ev.type == RPI_EV_KEY_DOWN && ev.code == 0x67) { mouse_move_by(-5000, -5000, false); continue; }
		if (ev.type == RPI_EV_KEY_DOWN && ev.code == 0x6f) { mouse_move_by(100, 60, false); continue; }
		if (ev.type == RPI_EV_KEY_DOWN && ev.code == 0x6d) { mouse_move_by(8, 4, false); continue; }
		// F5/F6 = x/y +50, F7/F8 = x/y +5
		if (ev.type == RPI_EV_KEY_DOWN && ev.code == 0x60) { mouse_move_by(50, 0, false); continue; }
		if (ev.type == RPI_EV_KEY_DOWN && ev.code == 0x61) { mouse_move_by(0, 50, false); continue; }
		if (ev.type == RPI_EV_KEY_DOWN && ev.code == 0x62) { mouse_move_by(5, 0, false); continue; }
		if (ev.type == RPI_EV_KEY_DOWN && ev.code == 0x64) { mouse_move_by(0, 5, false); continue; }
		if ((ev.type == RPI_EV_KEY_UP) && (ev.code == 0x67 || ev.code == 0x6f || ev.code == 0x6d ||
										   ev.code == 0x60 || ev.code == 0x61 || ev.code == 0x62 || ev.code == 0x64)) continue;
#endif
		switch (ev.type) {
		case RPI_EV_KEY_DOWN:
			ADBKeyDown(ev.code);
			break;
		case RPI_EV_KEY_UP:
			ADBKeyUp(ev.code);
			break;
		case RPI_EV_MOUSE_MOVE:
			mouse_move_by(ev.dx, ev.dy, true);
			break;
		case RPI_EV_MOUSE_DOWN:
			ADBMouseDown(ev.code);
			break;
		case RPI_EV_MOUSE_UP:
			ADBMouseUp(ev.code);
			break;
		case RPI_EV_MOUSE_WHEEL: {
			// Mouse wheel -> cursor up/down keys
			int code = ev.dy > 0 ? 0x3e : 0x3d;
			int n = ev.dy > 0 ? ev.dy : -ev.dy;
			for (int i = 0; i < n && i < 3; i++) {
				ADBKeyDown(code);
				ADBKeyUp(code);
			}
			break;
		}
		}
	}

	if (mouse_x >= 0 && (mouse_x != mouse_sent_x || mouse_y != mouse_sent_y)) {
		mouse_sent_x = mouse_x;
		mouse_sent_y = mouse_y;
		ADBMouseMoved(mouse_x, mouse_y);
	}
}


/*
 *  Main program (called from rpi_kernel.cpp on a private stack)
 */

extern "C" int sheepshaver_main(void)
{
	char str[256];

	rpi_log(GetString(STR_ABOUT_TEXT1), VERSION_MAJOR, VERSION_MINOR);
	rpi_log(" %s\n", GetString(STR_ABOUT_TEXT2));

	// Read preferences
	int argc = 1;
	static char prg_name[] = "SheepShaver";
	static char *argv_buf[2] = { prg_name, NULL };
	char **argv = argv_buf;
	PrefsInit(NULL, argc, argv);

	rpi_set_swap_opt_cmd(PrefsFindBool("swap_opt_cmd"));
	mouse_speed = PrefsFindInt32("mousespeed");
	if (mouse_speed < 10 || mouse_speed > 1000)
		mouse_speed = 100;

	// Catch accesses of the emulated Mac to unmapped memory
	rpi_install_abort_handler();

	// PowerPC to ARM translator: the code buffer must be allocated first so
	// it is close enough to the kernel code for direct calls
	if (PrefsFindBool("jit") && !ppc_armjit::reserve(16 * MB)) {
		rpi_log("JIT: cannot allocate code buffer, using the interpreter\n");
		PrefsReplaceBool("jit", false);
	}

	// Initialize VM system
	vm_init();

	// Get system info
	get_system_info();

	// Init system routines
	SysInit();

	// Create areas for Kernel Data: two aliases of the same physical memory
	{
		uint8 *phys = (uint8 *)rpi_alloc_1mb_aligned(MB);
		if (phys == NULL)
			rpi_fatal("Not enough memory for Kernel Data");
		if (!rpi_mmu_map(host_virt(KERNEL_DATA_BASE & ~(MB - 1)), (uint32)phys, MB, 0) ||
			!rpi_mmu_map(host_virt(KERNEL_DATA2_BASE & ~(MB - 1)), (uint32)phys, MB, 0))
			rpi_fatal("Cannot map Kernel Data");
		memset(phys, 0, MB);
	}
	kernel_data = (KernelData *)Mac2HostAddr(KERNEL_DATA_BASE);
	emulator_data = &kernel_data->ed;
	KernelDataAddr = KERNEL_DATA_BASE;

	// Areas for the ROM's DR (dynamic recompilation) 68k emulator: it
	// translates 68k code into PowerPC code, which is much faster to run
	// than the interpretive 68k emulator ("jit68k true" in macos9rpi.txt)
	DRCacheAddr = 0;
	if (PrefsFindBool("jit68k")) {
		map_mac_area(DR_EMULATOR_BASE & ~(MB - 1), MB, "DR emulator");
		map_mac_area(DR_CACHE_BASE & ~(MB - 1), (DR_CACHE_SIZE + MB - 1) & ~(MB - 1), "DR cache");
		DRCacheAddr = DR_CACHE_BASE;
		rpi_log("68k DR emulator enabled\n");
	}

	// Create area for Mac RAM
	RAMSize = PrefsFindInt32("ramsize");
	if (RAMSize <= 1000)
		RAMSize *= 1024 * 1024;
	if (RAMSize < 16 * 1024 * 1024) {
		WarningAlert(GetString(STR_SMALL_RAM_WARN));
		RAMSize = 16 * 1024 * 1024;
	}
	RAMSize &= ~(MB - 1);
	uint32 max_ram = (rpi_arm_memory_size() & ~(MB - 1));
	max_ram = max_ram > 64 * MB ? max_ram - 64 * MB : 16 * MB;	// leave room for the emulator
	if (max_ram > 0x30000000)
		max_ram = 0x30000000;
	if (RAMSize > max_ram) {
		rpi_log("Reducing Mac RAM from %u MB to %u MB\n", RAMSize >> 20, max_ram >> 20);
		RAMSize = max_ram;
	}
	RAMBase = 0;
	map_mac_area(RAMBase, RAMSize, "Mac RAM");
	RAMBaseHost = Mac2HostAddr(RAMBase);
	rpi_log("Mac RAM: %u MB\n", RAMSize >> 20);

	// Create area for Mac ROM, PPC interrupt stack and SheepShaver globals
	ROMBase = ROM_BASE;
	map_mac_area(ROMBase, ROM_AREA_SIZE + SIG_STACK_SIZE + 0x80000, "Mac ROM");
	ROMBaseHost = Mac2HostAddr(ROMBase);
	ROMEnd = ROMBase + ROM_AREA_SIZE;

	// Create area for SheepShaver data
	if (!SheepMem::Init()) {
		sprintf(str, GetString(STR_SHEEP_MEM_MMAP_ERR), "no memory");
		ErrorAlert(str);
		goto quit;
	}

	// Show which disks will be used
	report_disks();

	// Check the PowerPC interpreter on this host and measure its speed
	{
		if (!PPCSelfTest(RAMBase + 0x100000))
			rpi_log("WARNING: the PowerPC self test failed, Mac OS may not work\n");
	}

	// Load Mac ROM
	if (!load_mac_rom())
		goto quit;

	// Initialize everything
	if (!InitAll(NULL))
		goto quit;
	D(bug("Initialization complete\n"));

	// Write protect ROM
	rpi_mmu_protect(host_virt(ROMBase), ROM_AREA_SIZE, 1);

	// Sampling profiler
	if (PrefsFindBool("profile")) {
		profile_buf = new profile_sample[PROFILE_MAX];
		rpi_log("Profiling enabled\n");
	}

	jit_sampling = PrefsFindBool("jitprofile") || PrefsFindBool("jitsample");

	// Start 60Hz tick
	if (!rpi_tick_start(tick_func, 16625)) {
		ErrorAlert("Cannot start 60Hz timer");
		goto quit;
	}

	// NVRAM watchdog state
	memcpy(last_xpram, XPRAM, XPRAM_SIZE);

	// Jump to ROM boot routine
	rpi_log("Starting Mac OS...\n");
	rpi_log_save();
	emulator_running = true;
	init_emul_ppc();
	emul_ppc(ROMBase + 0x310000);
	emulator_running = false;

quit:
	Quit();
	return 0;
}


/*
 *  Cleanup and quit
 */

static void Quit(void)
{
	rpi_tick_stop();
	ppc_armjit::log_stats();

	// Exit PowerPC emulation
	exit_emul_ppc();

	// Deinitialize everything
	ExitAll();

	// Delete SheepShaver globals
	SheepMem::Exit();

	// Exit system routines
	uint32 hits, misses;
	SysCacheStats(&hits, &misses);
	uint32 cmds, kb;
	SysReadStats(&cmds, &kb);
	rpi_log("Disk cache: %u hits, %u misses; %u device reads (%u KB)\n", hits, misses, cmds, kb);
	SysPeriodicFlush();
	if (profile_buf && profile_n) {
		FILE *pf = fopen("macos9rpi.prof", "wb");
		if (pf) {
			fwrite(profile_buf, sizeof(profile_sample), profile_n, pf);
			fclose(pf);
			rpi_log("Wrote %u profile samples\n", profile_n);
		}
	}
	SysExit();

	// Exit preferences
	PrefsExit();

	unsigned loads, stores;
	if (rpi_abort_stats(&loads, &stores))
		rpi_log("Skipped %u loads / %u stores to unmapped Mac memory\n", loads, stores);

	rpi_log_save();
	rpi_show_shutdown_screen();
	rpi_halt();
}


/*
 *  Quit emulator
 */

void QuitEmulator(void)
{
	Quit();
}


/*
 *  Dump 68k registers
 */

void Dump68kRegs(M68kRegisters *r)
{
	for (int i=0; i<8; i++) {
		printf("d%d: %08x", i, r->d[i]);
		if (i == 3 || i == 7)
			printf("\n");
		else
			printf(", ");
	}
	for (int i=0; i<8; i++) {
		printf("a%d: %08x", i, r->a[i]);
		if (i == 3 || i == 7)
			printf("\n");
		else
			printf(", ");
	}
}


/*
 *  Make code executable
 */

void MakeExecutable(int dummy, uint32 start, uint32 length)
{
	if ((start >= ROMBase) && (start < (ROMBase + ROM_SIZE)))
		return;
	FlushCodeCache(start, start + length);
}


/*
 *  Mutexes (single threaded: no-ops)
 */

struct B2_mutex {
	int dummy;
};

B2_mutex *B2_create_mutex(void)
{
	return new B2_mutex;
}

void B2_lock_mutex(B2_mutex *mutex)
{
}

void B2_unlock_mutex(B2_mutex *mutex)
{
}

void B2_delete_mutex(B2_mutex *mutex)
{
	delete mutex;
}


/*
 *  Interrupt flags (must be handled atomically!)
 */

volatile uint32 InterruptFlags = 0;

void SetInterruptFlag(uint32 flag)
{
	atomic_or_u32(&InterruptFlags, flag);
}

void ClearInterruptFlag(uint32 flag)
{
	atomic_and_u32(&InterruptFlags, ~flag);
}


/*
 *  Disable interrupts
 */

void DisableInterrupt(void)
{
	WriteMacInt32(XLM_IRQ_NEST, int32(ReadMacInt32(XLM_IRQ_NEST)) + 1);
}


/*
 *  Enable interrupts
 */

void EnableInterrupt(void)
{
	WriteMacInt32(XLM_IRQ_NEST, int32(ReadMacInt32(XLM_IRQ_NEST)) - 1);
}


/*
 *  Helpers to share 32-bit addressable data with MacOS
 */

bool SheepMem::Init(void)
{
	// Size of a native page
	page_size = 4096;

	// SheepShaver globals live right after the ROM area and the interrupt stack
	uint8 *adr = Mac2HostAddr(ROM_BASE + ROM_AREA_SIZE + SIG_STACK_SIZE);
	proc = base = Host2MacAddr(adr);

	// Allocate page with all bits set to 0, right in the middle
	// This is also used to catch undesired overlaps between proc and data areas
	zero_page = proc + (size / 2);
	Mac_memset(zero_page, 0, page_size);
	rpi_mmu_protect_page(host_virt(zero_page), 1);

	sig_stack = ROMEnd;
	data = base + size;
	return true;
}

void SheepMem::Exit(void)
{
}


/*
 *  Display alerts
 */

void ErrorAlert(const char *text)
{
	if (!emulator_running) {
		// Fatal during startup: show it on screen and halt
		rpi_fatal("%s", text);
	}
	rpi_log(GetString(STR_SHELL_ERROR_PREFIX), text);
}

void WarningAlert(const char *text)
{
	rpi_log(GetString(STR_SHELL_WARNING_PREFIX), text);
}

bool ChoiceAlert(const char *text, const char *pos, const char *neg)
{
	rpi_log(GetString(STR_SHELL_WARNING_PREFIX), text);
	return false;
}


/*
 *  Prefs editor (none)
 */

bool PrefsEditor(void)
{
	return true;
}
