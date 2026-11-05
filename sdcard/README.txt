macos9rpi - Mac OS 9 on a bare-metal Raspberry Pi Zero / Zero W
================================================================

No Linux: kernel.img boots straight into a PowerPC emulator (SheepShaver
core) and starts Mac OS (up to 9.0.4). The picture goes out over mini-HDMI.

What you need on this partition
-------------------------------
  Mac OS ROM     your Mac ROM file (from SheepShaver), REQUIRED
  macos9rpi.txt  settings (RAM, disk, screen size)

The Mac hard disk lives in partition 2 of the SD card (raw image), or in a
disk image file (max. 4 GB) on this partition.

Hardware
--------
  - Raspberry Pi Zero or Zero W (also works on Pi 1)
  - mini-HDMI to HDMI cable
  - USB OTG adapter + USB hub with keyboard and mouse
  - good 5V power supply

Keys: Alt = Command, Windows key = Option, Ctrl = Control.

Files: macos9rpi.nvram stores the Mac PRAM (delete it to zap PRAM).
       macos9rpi.log has the start messages (self test, speed) and, after a
       clean shutdown, translator statistics.

Speed
-----
The PowerPC code is translated to ARM code while it runs ("jit true" in
macos9rpi.txt, the default). At every start the translator compares
itself with the interpreter and switches itself off if anything differs.
If Mac OS misbehaves, try "jit false" (slow interpreter).
Other tips: "screen 640x480", a plain desktop pattern instead of a
picture, fewer extensions (Extensions Manager), overclocking in config.txt.
