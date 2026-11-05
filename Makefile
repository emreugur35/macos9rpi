#
# Makefile - macos9rpi: Mac OS 9 on bare-metal Raspberry Pi Zero (W)
#
# Builds build/rpi1/kernel.img: Circle + newlib + SheepShaver (kpx_cpu
# PowerPC interpreter) for the BCM2835 / ARM1176JZF-S.
#
#   source env.sh && make -j8          build the kernel
#   make image                          build the SD card image (see scripts/)
#

ROOT	:= $(CURDIR)
STDLIB	:= $(ROOT)/external/circle-stdlib

include $(STDLIB)/Config.mk

SS	:= $(ROOT)/external/macemu/SheepShaver/src
B2	:= $(ROOT)/external/macemu/BasiliskII/src
PORT	:= $(ROOT)/src
BUILD	?= $(ROOT)/build/rpi1

# SheepShaver is old C++ ("register" etc.)
STANDARD   := -std=gnu++14
CHECK_DEPS := 0
TARGET	   := $(BUILD)/kernel

SS_SRCS := \
	main.cpp prefs.cpp prefs_items.cpp rom_patches.cpp rsrc_patches.cpp \
	emul_op.cpp name_registry.cpp macos_util.cpp timer.cpp xpram.cpp \
	adb.cpp sony.cpp disk.cpp cdrom.cpp scsi.cpp gfxaccel.cpp video.cpp \
	audio.cpp ether.cpp thunks.cpp serial.cpp user_strings.cpp

KPX_SRCS := \
	kpx_cpu/src/mathlib/ieeefp.cpp \
	kpx_cpu/src/mathlib/mathlib.cpp \
	kpx_cpu/src/cpu/ppc/ppc-cpu.cpp \
	kpx_cpu/src/cpu/ppc/ppc-decode.cpp \
	kpx_cpu/src/cpu/ppc/ppc-execute.cpp \
	kpx_cpu/src/cpu/ppc/ppc-translate.cpp \
	kpx_cpu/src/utils/utils-cpuinfo.cpp \
	kpx_cpu/sheepshaver_glue.cpp \
	kpx_cpu/ppc-dis.c

DUMMY_SRCS := \
	dummy/audio_dummy.cpp dummy/serial_dummy.cpp \
	dummy/scsi_dummy.cpp dummy/clip_dummy.cpp

PORT_SRCS := \
	main_circle.cpp sys_circle.cpp video_circle.cpp timer_circle.cpp \
	xpram_circle.cpp prefs_circle.cpp user_strings_circle.cpp \
	vm_alloc_circle.cpp extfs_stub.cpp ppc_selftest.cpp rpi_fastmem.c \
	rpi_kernel.cpp rpi_mmu.cpp rpi_abort_stub.S ppc_armjit.cpp \
	ether_circle.cpp rpi_net.cpp

UPSTREAM_SRCS := $(SS_SRCS) $(KPX_SRCS) $(DUMMY_SRCS)
ALL_SRCS := $(UPSTREAM_SRCS) $(PORT_SRCS)

objname = $(BUILD)/$(basename $(notdir $(1))).o
OBJS := $(foreach f,$(ALL_SRCS),$(call objname,$(f)))

vpath %.cpp $(PORT) $(SS) $(addprefix $(SS)/,$(sort $(dir $(KPX_SRCS) $(DUMMY_SRCS))))
vpath %.c   $(PORT) $(SS) $(addprefix $(SS)/,$(sort $(dir $(KPX_SRCS))))
vpath %.S   $(PORT)

include $(CIRCLEHOME)/Rules.mk

INCLUDE := $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)
# Wi-Fi: Circle's WLAN driver + wpa_supplicant (rpi_net.cpp replaces its l2_packet)
WLAN_LIBS := $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
	     $(CIRCLEHOME)/addon/wlan/libwlan.a
LIBS	:= $(WLAN_LIBS) $(CIRCLE_STDLIB_LIBS)

EMU_INCLUDE := -I$(PORT) -I$(SS)/include -I$(SS)/kpx_cpu/include -I$(SS)/kpx_cpu/src \
	       -I$(SS)/kpx_cpu -I$(B2)/include
EMU_DEFINE  := -DCIRCLE_PORT=1 $(EXTRA_DEFINE)

# Warnings: be quiet about upstream code, strict about ours
UPSTREAM_OBJS := $(foreach f,$(UPSTREAM_SRCS),$(call objname,$(f)))
$(UPSTREAM_OBJS): WARN := -w
WARN ?= -Wno-unused-variable -Wno-unused-function

# The interpreter core is the hot path
FAST_OPT ?= -O3 -fomit-frame-pointer
FAST_OBJS := $(BUILD)/ppc-cpu.o $(BUILD)/ppc-execute.o $(BUILD)/ppc-decode.o $(BUILD)/sheepshaver_glue.o
$(BUILD)/ppc_armjit.o: WARN += -Wno-invalid-offsetof -Wno-deprecated-declarations
$(FAST_OBJS): OPT_EXTRA := $(FAST_OPT)

# Fast copies for the native QuickDraw acceleration
$(BUILD)/gfxaccel.o: EMU_DEFINE += -include $(PORT)/rpi_fastmem.h -Dmemmove=rpi_memmove -Dmemset=rpi_memset
$(BUILD)/rpi_fastmem.o: OPT_EXTRA := -O2 -fno-tree-loop-distribute-patterns -fno-builtin

all: $(TARGET).img

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: %.cpp | $(BUILD)
	@echo "  CPP   $(notdir $<)"
	@$(CPP) $(CPPFLAGS) $(EMU_INCLUDE) $(EMU_DEFINE) $(WARN) $(OPT_EXTRA) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.c | $(BUILD)
	@echo "  CC    $(notdir $<)"
	@$(CC) $(CFLAGS) $(C_STANDARD) $(EMU_INCLUDE) $(EMU_DEFINE) $(WARN) $(OPT_EXTRA) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.S | $(BUILD)
	@echo "  AS    $(notdir $<)"
	@$(AS) $(AFLAGS) -c -o $@ $<

# Generated PowerPC instruction dispatcher (see SheepShaver Unix Makefile)
$(BUILD)/ppc-execute-impl.cpp: $(SS)/kpx_cpu/src/cpu/ppc/ppc-decode.cpp $(SS)/kpx_cpu/src/cpu/ppc/genexec.pl | $(BUILD)
	@echo "  GEN   ppc-execute-impl.cpp"
	@$(CPP) $(CPPFLAGS) $(EMU_INCLUDE) $(EMU_DEFINE) -w -DGENEXEC -E $< | perl $(SS)/kpx_cpu/src/cpu/ppc/genexec.pl > $@

$(BUILD)/ppc-execute.o: $(BUILD)/ppc-execute-impl.cpp
$(BUILD)/ppc-execute.o: EMU_INCLUDE += -I$(BUILD)

-include $(OBJS:.o=.d)

image: $(TARGET).img
	@$(ROOT)/scripts/make_image.sh

clean-emu:
	rm -rf $(BUILD)

.PHONY: all image clean-emu
