/*
 *  rpi_glue.h - Interface between the SheepShaver core (which must not see
 *               Circle headers) and the Circle bare-metal side (rpi_*.cpp)
 *
 *  macos9rpi - Mac OS 9 on bare-metal Raspberry Pi
 */

#ifndef RPI_GLUE_H
#define RPI_GLUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Boot console / logging ------------------------------------------ */

// printf-style output to serial and (while enabled) the boot screen
void rpi_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rpi_log_save(void);	// write the messages so far to macos9rpi.log
// Stop drawing log text on screen (called when the Mac takes over the display)
void rpi_console_detach(void);
// Show "safe to switch off" on screen after Mac OS shut down
void rpi_show_shutdown_screen(void);
// Show a fatal error on screen + serial and halt (never returns)
void rpi_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* ---- Time ------------------------------------------------------------- */

uint64_t rpi_ticks_usec(void);			// free running 1 MHz counter
void rpi_delay_usec(uint64_t usec);		// busy wait (with WFI when long)
uint32_t rpi_unix_time(void);			// seconds since 1970 (best effort)

/* ---- Memory / MMU ------------------------------------------------------ */

// Allocate physically contiguous, 1MB aligned memory (never freed)
void *rpi_alloc_1mb_aligned(uint32_t size);
// Map host virtual address range [virt, virt+size) to physical memory at phys.
// virt, phys, size must be 1MB aligned. readonly maps privileged read-only.
int rpi_mmu_map(uint32_t virt, uint32_t phys, uint32_t size, int readonly);
// Change protection of an already mapped range (1MB granularity)
int rpi_mmu_protect(uint32_t virt, uint32_t size, int readonly);
// Total ARM memory in bytes
uint32_t rpi_arm_memory_size(void);
// Install the data abort handler that skips faulting Mac memory accesses
void rpi_install_abort_handler(void);
// Map the GPU frame buffer cached; rpi_clean_dcache() pushes pixels out
int rpi_mmu_set_fb_cached(uint32_t addr, uint32_t size);
int rpi_mmu_set_exec(uint32_t addr, uint32_t size);	// clear XN (JIT code buffer)
void rpi_clean_dcache(void);
// Is the given host virtual address inside one of the Mac mappings?
int rpi_is_mac_mapping(uint32_t virt);

/* ---- Framebuffer ------------------------------------------------------- */

// Allocate a framebuffer; returns host pointer or NULL. Out: actual geometry.
uint8_t *rpi_fb_init(unsigned width, unsigned height, unsigned depth,
		     unsigned *out_width, unsigned *out_height, unsigned *out_pitch);
// Set 8-bit palette entries (rgb: 3 bytes per entry)
void rpi_fb_set_palette(unsigned first, unsigned count, const uint8_t *rgb);
// Preferred display size (from cmdline.txt width=/height= or the display)
void rpi_fb_preferred_size(unsigned *width, unsigned *height);

/* ---- 60Hz tick ----------------------------------------------------------- */

typedef void (*rpi_tick_handler_t)(void);
// Start periodic timer calling handler in IRQ context every period_usec
int rpi_tick_start(rpi_tick_handler_t handler, unsigned period_usec);
void rpi_tick_stop(void);
// Precise Mac Time Manager: handler runs in IRQ context at the wakeup time
void rpi_tm_set_handler(rpi_tick_handler_t handler);
void rpi_tm_set_wakeup(int64_t wakeup_usec);	// INT64_MAX = none

/* ---- Input ------------------------------------------------------------------ */

enum {
	RPI_EV_KEY_DOWN = 1,	// code = Mac ADB key code
	RPI_EV_KEY_UP,
	RPI_EV_MOUSE_MOVE,		// dx, dy relative
	RPI_EV_MOUSE_DOWN,		// code = button (0 left, 1 right, 2 middle)
	RPI_EV_MOUSE_UP,
	RPI_EV_MOUSE_WHEEL		// dy
};

typedef struct {
	uint8_t type;
	uint8_t code;
	int16_t dx, dy;
} rpi_input_event_t;

// Fetch next queued input event (main context). Returns 0 if none.
int rpi_input_get(rpi_input_event_t *ev);
// Do USB housekeeping (plug and play) - call periodically from main context
void rpi_usb_poll(void);
// Is there a keyboard / mouse?
int rpi_have_keyboard(void);
int rpi_have_mouse(void);

/* ---- Power ---------------------------------------------------------------- */

void rpi_halt(void) __attribute__((noreturn));
void rpi_reboot(void) __attribute__((noreturn));

/* ---- Files (FatFs on the SD card's first partition) ------------------------- */

// Path prefix of the boot partition, e.g. "SD:/"
const char *rpi_sd_root(void);

/* ---- Raw SD card access (byte offsets / lengths must be multiples of 512) -- */

int rpi_sd_read(uint64_t offset, void *buf, uint32_t len);	// returns bytes or <0
int rpi_sd_write(uint64_t offset, const void *buf, uint32_t len);
uint64_t rpi_sd_size(void);

#ifdef __cplusplus
}
#endif

#endif /* RPI_GLUE_H */
