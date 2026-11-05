# macos9rpi — bare-metal Mac OS 9 on the Raspberry Pi Zero W

No Linux: `kernel.img` boots directly from the Raspberry Pi firmware, starts a
PowerPC emulator (SheepShaver core, kpx_cpu interpreter) and boots
Mac OS 9.0.4. Video goes out over mini-HDMI.

## What you need (not included)

This repository contains no Apple software. You need to supply two files
yourself; both are copyrighted by Apple, so please don't open issues or pull
requests asking for them or containing them.

### 1. A Mac ROM file

One of:

- **New World "Mac OS ROM" file** (recommended; this is what the project is
  tested with). It's the ~2 MB file called `Mac OS ROM` in the System Folder
  of a Mac OS 8.6–9.0.4 install CD or of a New World Mac (iMac, blue-and-white
  G3, …). It's a text-headed file that starts with `<CHRP-BOOT>` and
  `MacROM for NewWorld`. The version often found as `newworld86.rom` works.
  Use a ROM file from Mac OS 9.0.4 or earlier; newer ones are untested.
- **4 MB Old World ROM dump** from a PCI Power Mac you own (e.g. Power Mac
  7500/8500/9500 or a beige G3). SheepShaver recognises TNT, Alchemy,
  Zanzibar, Gazelle and Gossamer ROMs. Not yet tested on the Pi.

The ROM goes on the SD card's FAT partition as `Mac OS ROM`
(`ROM=yourfile scripts/make_image.sh` does this), or set the `rom` line in
`macos9rpi.txt` to a different name.

### 2. A Mac OS 9 disk image

- **System:** Mac OS 9.0.4 or earlier installed (tested with 9.0.4).
  SheepShaver can't run Mac OS 9.1 or later.
- **Format:** an uncompressed, raw image of an HFS or HFS+ volume. This is the
  format SheepShaver and Basilisk II create (`.dsk`, `.hfv`, `.img`). Images
  with an Apple partition map are detected too. Compressed or encoded images
  (Disk Copy compressed `.img`, `.dmg`, StuffIt) must first be converted to a
  read/write raw image.
- **Size:** in partition 2 of the SD card any size works, as long as the card is
  larger than the image + 256 MB. As a file on the FAT partition (`disk
  name.dsk`) the limit is 4 GB. Tested with an 8 GB HFS+ volume.
- **Easiest way to make one:** install Mac OS 9.0.4 in SheepShaver on a
  desktop computer from an install CD, then use that disk image.

## SD card image

`scripts/make_image.sh` builds `out/macos9rpi.img` from your ROM and disk
image (see *Building*). Write it straight to an SD card (Raspberry Pi
Imager → "Use custom", or balenaEtcher). With the 8 GB test disk the image is
8.1 GB, so a **16 GB SD card** is needed. In general the card must be larger
than your disk image + 256 MB.

| Partition | Contents |
|-----------|----------|
| 1 — FAT32 (256 MB, "MACOS9RPI") | firmware, `kernel.img`, `config.txt`, `cmdline.txt`, `macos9rpi.txt` (settings), `Mac OS ROM` |
| 2 — raw (type 0xAF) | your Mac disk image, copied byte for byte |

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
and not distributed; put your own copies in the project root (see
*What you need*).

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

## Contributing

Contributions are welcome! Bug reports, test results on real hardware, fixes
and new features (sound, networking, more colors, speed) are all appreciated.
Please open an issue or a pull request.

- When reporting a problem, attach `macos9rpi.log` from the SD card's FAT
  partition and say which Pi, ROM and Mac OS version you used.
- If you can, test changes in QEMU (`scripts/run_qemu.sh`) and on a real Pi.
- Changes to Circle or SheepShaver go into `patches/` as an updated diff
  against the pinned commits.
- Never include Mac ROMs or Mac OS disk images in issues or pull requests.
