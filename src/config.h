/*
 *  config.h - Hand-written configuration for the bare-metal Circle port
 *             of SheepShaver (macos9rpi)
 *
 *  Replaces the autoconf-generated config.h of the Unix build.
 */

#ifndef CONFIG_H
#define CONFIG_H

#define PACKAGE "SheepShaver"
#define PACKAGE_NAME "SheepShaver"
#define PACKAGE_VERSION "2.5-circle"
#define VERSION "2.5"

/* Bare-metal Circle target */
#define CIRCLE_PORT 1

/* Always emulate the PowerPC CPU (kpx_cpu interpreter) */
#define EMULATED_PPC 1

/*
 *  Mac address space is mapped into the ARM virtual address space at
 *  host = mac + NATMEM_OFFSET (mod 2^32). The MMU mapping for the Mac RAM,
 *  ROM, kernel data and SheepShaver globals is set up in main_circle.cpp.
 */
#define NATMEM_OFFSET 0x80000000u

/* Type sizes (ARM AArch32, ILP32) */
#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define SIZEOF_VOID_P 4
#define SIZEOF_FLOAT 4
#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE 8

/* Headers provided by newlib (circle-stdlib) */
#define STDC_HEADERS 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define TIME_WITH_SYS_TIME 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_FENV_H 1
#define HAVE_MALLOC_H 1

/* libm functions */
#define HAVE_EXP2F 1
#define HAVE_EXP2 1
#define HAVE_LOG2F 1
#define HAVE_LOG2 1
#define HAVE_CEILF 1
#define HAVE_CEIL 1
#define HAVE_FLOORF 1
#define HAVE_FLOOR 1
#define HAVE_LRINT 1
#define HAVE_ROUNDF 1
#define HAVE_ROUND 1
#define HAVE_TRUNCF 1
#define HAVE_TRUNC 1

/* We can skip faulting host instructions in our own data abort handler */
#define HAVE_SIGSEGV_SKIP_INSTRUCTION 1

/* No threads, no X11/SDL/GTK, no JIT, no mon */
#undef HAVE_PTHREADS
#undef ENABLE_GTK
#undef USE_SDL
#undef USE_SDL_VIDEO
#undef USE_SDL_AUDIO
#undef ENABLE_MON
#undef ENABLE_DYNGEN
#undef ENABLE_VOSF
#undef ENABLE_XF86_DGA

#endif /* CONFIG_H */
