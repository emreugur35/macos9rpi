#!/bin/bash
#
# run_qemu.sh - Run macos9rpi in QEMU's Raspberry Pi Zero emulation
#
#   scripts/run_qemu.sh [sd-image] [extra qemu args]
#
# Serial output goes to stdout, the QEMU monitor listens on
# $MON (default: out/qemu-monitor.sock) for "screendump" etc.
#
# Network test (Mac OS gets 10.0.2.15 from QEMU's DHCP server, "ether usb"):
#   QEMU_BIN=tools/qemu-circle/bin/qemu-system-arm scripts/run_qemu.sh out/qemu-sd.img \
#     -device usb-net,netdev=n0 -netdev user,id=n0
# Receiving needs a QEMU with Circle's one-line DWC2 fix (see
# scripts/build_qemu_circle.sh); stock QEMU only passes frames out.
#
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SD="${1:-$ROOT/out/qemu-sd.img}"
shift || true
MON="${MON:-$ROOT/out/qemu-monitor.sock}"
DISPLAY_OPT="${DISPLAY_OPT:--display none}"
rm -f "$MON"
exec "${QEMU_BIN:-qemu-system-arm}" -M raspi0 -bios "${KERNEL:-$ROOT/build/rpi1/kernel.img}" \
	-drive file="$SD",if=sd,format=raw \
	-serial stdio $DISPLAY_OPT \
	-monitor unix:"$MON",server,nowait \
	-device usb-kbd -device usb-mouse \
	"$@"
