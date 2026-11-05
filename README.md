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
| 1 — FAT32 (256 MB, "MACOS9RPI") | Pi boot firmware, `kernel.img`, `config.txt`, `cmdline.txt`, `macos9rpi.txt` (settings), `Mac OS ROM`, `wpa_supplicant.conf` (Wi-Fi settings), `firmware/` (Wi-Fi firmware) |
| 2 — raw (type 0xAF) | your Mac disk image, copied byte for byte |

The FAT partition is readable on any computer, so you can change the settings
from there.

## Hardware

- Raspberry Pi Zero / Zero W (BCM2835); Wi-Fi needs the Zero W
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
#ether wlan           # network: wlan (Wi-Fi) or usb (USB Ethernet adapter)
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

### Network (Wi-Fi bridge)

> **Status: new, not yet tested on a real Pi Zero W.** The Mac side (Open
> Transport, DHCP, full-size frames) is tested in QEMU over a USB network
> adapter; the Wi-Fi connection itself can only be tested on real hardware.
> Reports are very welcome (see *Contributing*).

With `ether wlan`, Mac OS's built-in Ethernet is bridged to the Pi Zero W's
Wi-Fi. Mac OS gets its own IP address from your router by DHCP and appears on
the network like any other computer, so TCP/IP and AppleTalk should both work.

1. Put your network name, password and country code into
   `wpa_supplicant.conf` on the SD card's FAT partition. Alternatively,
   build the image with `WIFI_SSID=… WIFI_PASSWORD=… WIFI_COUNTRY=TR
   scripts/make_image.sh`, which writes the file (password stored hashed) and
   turns `ether wlan` on.
2. Set `ether wlan` in `macos9rpi.txt`.
3. In Mac OS, open the TCP/IP control panel: Connect via
   "Ethernet built-in", Configure "Using DHCP Server".

How it works: a Wi-Fi client may only send frames with its own MAC address,
so Mac OS simply uses the Wi-Fi chip's MAC address. No Linux and no
Circle TCP/IP stack run underneath, so every frame goes straight between the
radio and Mac OS. Circle's WLAN driver and its port of wpa_supplicant handle
the connection; WPA key-handshake frames are passed to wpa_supplicant and
everything else goes to Mac OS.

- WPA2-Personal (or WPA/WPA2 mixed) and open networks work; WPA3-only
  networks don't.
- The Wi-Fi firmware (`brcmfmac43430-sdio.*`) goes in `/firmware` on the SD
  card. `make_image.sh` downloads it with `scripts/get_wlan_firmware.sh`.
- `ether usb` uses a USB Ethernet adapter instead (SMSC951x, LAN7800 or CDC
  Ethernet; plug it in before switching on).
- Wi-Fi start-up messages ("Wi-Fi: ready, MAC …") appear on screen before
  Mac OS starts; `macos9rpi.log` also has the later ones ("Wi-Fi: connected")
  after a clean shutdown. `etherdebug true` additionally logs the first
  frames and packet counters.
- Expect modest speed: the emulator and the network share one CPU core.
- Mac OS 9's browsers can't open most modern HTTPS websites; plain HTTP,
  FTP and AppleShare/AFP file sharing are the useful things here.

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
WIFI_SSID=name WIFI_PASSWORD=pw WIFI_COUNTRY=TR ROM=newworld86.rom scripts/make_image.sh   # with Wi-Fi
QEMU=1 ROM=newworld86.rom scripts/make_image.sh
scripts/run_qemu.sh out/qemu-sd.img        # test in QEMU raspi0
python3 scripts/screenshot.py out/qemu-monitor.sock shot.png
```

`make_image.sh` downloads the Wi-Fi firmware once
(`scripts/get_wlan_firmware.sh`, into `firmware/wlan/`, not committed).

Network test in QEMU: QEMU has no Wi-Fi chip, so the Mac side is tested over
QEMU's USB network adapter (`ether usb`). Receiving needs a QEMU with Circle's
one-line USB fix, built by `scripts/build_qemu_circle.sh` into
`tools/qemu-circle/`. `scripts/netpeer.py` then acts as a tiny LAN (DHCP
server, ARP) and pings Mac OS with small and full-size packets:

```
python3 scripts/netpeer.py &
scripts/fresh_qemu.sh "ether usb" "etherdebug true"
QEMU_BIN=tools/qemu-circle/bin/qemu-system-arm scripts/run_qemu.sh out/qemu-sd.img \
  -device usb-net,netdev=n0 -netdev dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=10110,remote.type=inet,remote.host=127.0.0.1,remote.port=10111
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
- `src/ether_circle.cpp` — SheepShaver's Ethernet driver backend: passes
  Mac OS's frames to and from `rpi_net.cpp`.
- `src/rpi_net.cpp` — the network device: Circle's WLAN driver and
  wpa_supplicant (Wi-Fi) or a USB Ethernet adapter. Splits off WPA handshake
  frames for wpa_supplicant (replaces hostap's `l2_packet`) and runs the
  network tasks from the emulator's 60 Hz and idle hooks.
- `src/ppc_selftest.cpp` — boot-time PowerPC interpreter self test + MIPS
  measurement.

## Limitations

- Up to Mac OS 9.0.4 (SheepShaver does not emulate the MMU).
- 256 colors; no sound.
- Wi-Fi networking hasn't been tested on a real Pi yet. QEMU has no Wi-Fi
  chip, so only the Mac side has been tested (over QEMU's USB network
  adapter). WPA3-only networks are not supported.
- USB Ethernet adapters must be plugged in before switching on.
- In QEMU, USB mouse movement does not reach Circle (known QEMU/Circle
  limitation; buttons and keyboard work). This is not a problem on real
  hardware.

## Contributing

Contributions are welcome! Bug reports, test results on real hardware
(especially Wi-Fi), fixes and new features (sound, more colors, speed) are
all appreciated. Please open an issue or a pull request.

- When reporting a problem, attach `macos9rpi.log` from the SD card's FAT
  partition and say which Pi, ROM and Mac OS version you used. For network
  problems, set `etherdebug true` first, and remove your Wi-Fi password from
  anything you post.
- If you can, test changes in QEMU (`scripts/run_qemu.sh`) and on a real Pi.
- Changes to Circle or SheepShaver go into `patches/` as an updated diff
  against the pinned commits.
- Never include Mac ROMs or Mac OS disk images in issues or pull requests.
