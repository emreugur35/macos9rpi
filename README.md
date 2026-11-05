# macos9rpi — bare-metal Mac OS 9 on the Raspberry Pi Zero W

No Linux: `kernel.img` boots directly from the Raspberry Pi firmware, starts a
PowerPC emulator (SheepShaver core, kpx_cpu interpreter) and boots
Mac OS 9.0.4. Video goes out over mini-HDMI.

## Ready-made image

`out/macos9rpi.img` — write it straight to an SD card (Raspberry Pi Imager →
"Use custom", or balenaEtcher). **An SD card of at least 16 GB** is required
(the image is 8.1 GB).

| Partition | Contents |
|-----------|----------|
| 1 — FAT32 (256 MB, "MACOS9RPI") | firmware, `kernel.img`, `config.txt`, `cmdline.txt`, `macos9rpi.txt` (settings), `Mac OS ROM` |
| 2 — raw (type 0xAF) | exact copy of `OS9.dsk` (HFS+ "System 9") |

The FAT partition is readable on any computer, so you can change the settings
from there.

## Hardware

- Raspberry Pi Zero / Zero W (BCM2835)
- mini-HDMI → HDMI cable
- USB OTG adapter + USB hub, keyboard and mouse
- a good 5V power supply

Keys: **Alt = Command**, **Windows key = Option**, Ctrl = Control
(`swap_opt_cmd true` swaps Alt and Windows).

## Settings (`macos9rpi.txt`, SheepShaver format)

```
rom Mac OS ROM        # ROM file (on the FAT partition)
disk sd:p2            # Mac disk: partition 2 of the SD card (or a ≤4 GB image file on FAT)
#cdrom MacOS9.iso     # CD image
ramsize 128           # Mac RAM (MB)
screen 800x600        # resolution (256 colors; the GPU scales it to the monitor)
jit true              # PowerPC→ARM translator (JIT); false = interpreter
```

### JIT (PowerPC → ARM translator)

With `jit true` (the default), PowerPC code is translated to ARM code as it
runs; this is several times faster than the interpreter. At every boot the
translator checks ~120 instruction forms and random instruction sequences
against the interpreter; if anything doesn't match it switches itself off and
the interpreter is used instead (`JIT self test ... failures` in the serial
log). If Mac OS behaves strangely, try `jit false`.

Experimental: `jit68k true` turns on the ROM's Apple DR 68k emulator (a
68k→PowerPC compiler) on top of the JIT. It works, but booting is slower and
no measurable gain was seen; it is off by default.

Development: `jitsample true` writes to the serial log where time is being
spent (60 Hz sampling, hottest blocks); `jitprofile true` also counts the
instruction mix (slow).

`macos9rpi.nvram` is the Mac's PRAM (deleting it resets PRAM).
`macos9rpi.log` contains the boot messages (self-test results, MIPS
measurement) and, after a clean shutdown, JIT statistics; it is useful when
reporting problems.

## Speed tips

- **Reduce extensions:** hold Shift at boot ("Extensions Off"), or choose the
  "Mac OS 9.0.4 Base" set in Extensions Manager. This disk has ~150
  extensions; they are what slows booting down the most.
- Use a plain pattern instead of a desktop picture, and turn off After
  Dark/Launcher/Control Strip.
- Remove the `#` in front of the overclock lines in `config.txt`.
- `macos9rpi.txt`: `screen 640x480` draws fewer pixels; `diskcache 32`
  (the default) caches SD card reads in RAM.

## Building (macOS, Apple Silicon)

```
source env.sh
make -j8                                   # build/rpi1/kernel.img
ROM=newworld86.rom scripts/make_image.sh   # out/macos9rpi.img (DISK=OS9.dsk by default)
QEMU=1 ROM=newworld86.rom scripts/make_image.sh
scripts/run_qemu.sh out/qemu-sd.img        # test in QEMU raspi0
python3 scripts/screenshot.py out/qemu-monitor.sock shot.png
```

Dependencies: Arm GNU Toolchain 14.3 (arm-none-eabi) under `tools/`;
Homebrew: `qemu mtools gnu-getopt make gnu-sed coreutils texinfo bash`.
circle-stdlib (Circle + newlib) and macemu (SheepShaver) under `external/`;
the small changes made to upstream are in `patches/`.

### Setting up from scratch

Not in this repository: `tools/` (toolchain), `external/` (upstream sources),
the Mac OS disk (`OS9.dsk`) and the Mac ROM. The disk and ROM are copyrighted
and not distributed; put your own copies in the project root.

```
# Toolchain: developer.arm.com → Arm GNU Toolchain 14.3.rel1,
# darwin-arm64, arm-none-eabi → extract under tools/.

git clone https://github.com/smuehlst/circle-stdlib.git external/circle-stdlib
git -C external/circle-stdlib checkout $(cat patches/circle-stdlib.commit)
git -C external/circle-stdlib submodule update --init --recursive
git -C external/circle-stdlib/libs/circle checkout $(cat patches/circle.commit)
git -C external/circle-stdlib/libs/circle apply ../../../../patches/circle.patch

git clone https://github.com/kanjitalk755/macemu.git external/macemu
git -C external/macemu checkout $(cat patches/macemu.commit)
git -C external/macemu apply ../../patches/macemu.patch

source env.sh
(cd external/circle-stdlib && /opt/homebrew/bin/bash ./configure -r 1 -p arm-none-eabi- && make -j8)
make -j8
```

## Architecture

- `src/rpi_kernel.cpp` — the Circle side: interrupts, timer, serial port,
  8-bit framebuffer (Mac OS draws directly into GPU memory, no copy),
  SD card + FAT, USB keyboard/mouse (HID → ADB key codes), boot console.
- `src/rpi_mmu.cpp` — the Mac address space is mapped with the ARM MMU as
  `host = mac + 0x80000000` (RAM, ROM, kernel data double mapping, read-only
  zero page); Mac accesses to hardware addresses that don't exist are skipped
  in the data-abort handler (the bare-metal equivalent of SheepShaver's
  SIGSEGV logic).
- `src/main_circle.cpp` — SheepShaver startup, 60 Hz tick (IRQ), input events.
- `src/sys_circle.cpp` — disk access: FAT file (fast-seek) or raw SD partition.
- `src/video_circle.cpp` — 8-bit direct framebuffer, palette sent to the GPU
  via mailbox.
- `src/ppc_armjit.cpp` — PowerPC→ARMv6 translator: basic blocks are
  translated to ARM code and chained together, PowerPC registers are kept in
  ARM registers within a block, untranslated instructions call the
  interpreter's handler; boot-time self test against the interpreter.
- `src/ppc_selftest.cpp` — boot-time PowerPC interpreter self test + MIPS
  measurement.

## Limitations

- Up to Mac OS 9.0.4 (SheepShaver does not emulate the MMU).
- 256 colors; no sound or networking (first version).
- In QEMU, USB mouse movement does not reach Circle (known QEMU/Circle
  limitation; buttons and keyboard work). This is not a problem on real
  hardware.
