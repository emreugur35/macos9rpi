#!/bin/bash
#
# run_qemu.sh - Run macos9rpi in QEMU's Raspberry Pi Zero emulation
#
#   scripts/run_qemu.sh [sd-image] [extra qemu args]
#
# Serial output goes to stdout, the QEMU monitor listens on
# $MON (default: out/qemu-monitor.sock) for "screendump" etc.
#
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SD="${1:-$ROOT/out/qemu-sd.img}"
shift || true
MON="${MON:-$ROOT/out/qemu-monitor.sock}"
DISPLAY_OPT="${DISPLAY_OPT:--display none}"
rm -f "$MON"
exec qemu-system-arm -M raspi0 -bios "${KERNEL:-$ROOT/build/rpi1/kernel.img}" \
	-drive file="$SD",if=sd,format=raw \
	-serial stdio $DISPLAY_OPT \
	-monitor unix:"$MON",server,nowait \
	-device usb-kbd -device usb-mouse \
	"$@"
