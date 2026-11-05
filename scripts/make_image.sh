#!/bin/bash
#
# make_image.sh - Build a bootable SD card image for macos9rpi
#
# Layout:
#   partition 1  FAT32 (BOOT_MB)   firmware, kernel.img, config, ROM, prefs
#   partition 2  type 0xAF (raw)   Mac disk image, copied byte for byte
#                                  (only if DISK is given / exists)
#
# Environment variables:
#   DISK=path      Mac disk image for partition 2 (default: ./OS9.dsk if present)
#   ROM=path       Mac ROM file to put on the FAT partition as "Mac OS ROM"
#   OUT=path       output image (default: out/macos9rpi.img)
#   BOOT_MB=n      size of the FAT partition in MB (default 256)
#   QEMU=1         also create out/qemu-sd.img (size padded to a power of 2)
#   WIFI_SSID=name      Wi-Fi network: writes wpa_supplicant.conf and turns
#   WIFI_PASSWORD=pw    on "ether wlan" (no password = open network;
#   WIFI_COUNTRY=XX     the password is stored hashed, country default US)
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/out/macos9rpi.img}"
BOOT_MB="${BOOT_MB:-256}"
KERNEL="$ROOT/build/rpi1/kernel.img"
FW="$ROOT/firmware"
TEMPLATE="$ROOT/sdcard"

if [ -z "${DISK+x}" ]; then
	if [ -f "$ROOT/OS9.dsk" ]; then DISK="$ROOT/OS9.dsk"; else DISK=""; fi
fi
ROM="${ROM:-}"

[ -f "$KERNEL" ] || { echo "kernel.img missing - run make first" >&2; exit 1; }
command -v mformat >/dev/null || { echo "mtools missing (brew install mtools)" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"

START1=2048					# sectors (1 MB)
SIZE1=$((BOOT_MB * 2048))
START2=$((START1 + SIZE1))
SIZE2=0
if [ -n "$DISK" ]; then
	DISK_BYTES=$(/usr/bin/stat -f %z "$DISK")
	SIZE2=$(( (DISK_BYTES + 1048575) / 1048576 * 2048 ))
fi
TOTAL=$((START2 + SIZE2))

echo "Creating $OUT ($((TOTAL / 2048)) MB)"
rm -f "$OUT"
mkfile -n $((TOTAL * 512)) "$OUT" 2>/dev/null || truncate -s $((TOTAL * 512)) "$OUT"

# MBR partition table
python3 -I - "$OUT" "$START1" "$SIZE1" "$START2" "$SIZE2" <<'EOF'
import struct, sys
path, s1, n1, s2, n2 = sys.argv[1], *map(int, sys.argv[2:])
def entry(boot, ptype, start, count):
    if count == 0:
        return b'\0' * 16
    # CHS fields set to the "LBA only" marker values
    return struct.pack('<B3sB3sII', boot, b'\xfe\xff\xff', ptype, b'\xfe\xff\xff', start, count)
mbr = bytearray(512)
struct.pack_into('<I', mbr, 440, 0x4d414339)		# disk signature "MAC9"
mbr[446:462] = entry(0x80, 0x0c, s1, n1)		# FAT32 LBA
mbr[462:478] = entry(0x00, 0xaf, s2, n2)		# Apple HFS/HFS+ (raw Mac disk)
mbr[510] = 0x55; mbr[511] = 0xaa
with open(path, 'r+b') as f:
    f.write(mbr)
EOF

# FAT32 boot partition
IMG="$OUT@@$((START1 * 512))"
mformat -i "$IMG" -F -T "$SIZE1" -h 64 -s 32 -H "$START1" -v MACOS9RPI ::
MC() { mcopy -i "$IMG" -o "$@"; }
for f in bootcode.bin start.elf fixup.dat LICENCE.broadcom; do
	MC "$FW/$f" ::/
done
MC "$KERNEL" ::/kernel.img
MC "$TEMPLATE/config.txt" ::/
MC "$TEMPLATE/cmdline.txt" ::/
MC "$TEMPLATE/README.txt" ::/

# Wi-Fi firmware for "ether wlan" (downloaded once into firmware/wlan/)
WLAN_FW="$FW/wlan"
if [ ! -f "$WLAN_FW/brcmfmac43430-sdio.bin" ]; then
	"$ROOT/scripts/get_wlan_firmware.sh" || echo "WARNING: no Wi-Fi firmware, Wi-Fi will not work" >&2
fi
if [ -f "$WLAN_FW/brcmfmac43430-sdio.bin" ]; then
	mmd -i "$IMG" ::/firmware
	for f in "$WLAN_FW"/brcmfmac43430-sdio.*; do
		MC "$f" ::/firmware/
	done
fi

# Settings and Wi-Fi configuration
TMPD="$(mktemp -d)"
trap 'rm -rf "$TMPD"' EXIT
cp "$TEMPLATE/macos9rpi.txt" "$TMPD/macos9rpi.txt"
if [ -n "${WIFI_SSID:-}" ]; then
	python3 -I - "$TMPD/wpa_supplicant.conf" <<'EOF'
import hashlib, os, sys
ssid = os.environ['WIFI_SSID']
pw = os.environ.get('WIFI_PASSWORD', '')
country = os.environ.get('WIFI_COUNTRY', 'US').upper()
safe = all(32 <= ord(c) < 127 and c != '"' for c in ssid)
ssid_line = 'ssid="%s"' % ssid if safe else 'ssid=' + ssid.encode().hex()
lines = ['# Written by make_image.sh', 'country=' + country, '', 'network={', '\t' + ssid_line]
if pw:
    if not 8 <= len(pw) <= 63:
        sys.exit('WIFI_PASSWORD must be 8..63 characters')
    psk = hashlib.pbkdf2_hmac('sha1', pw.encode(), ssid.encode(), 4096, 32).hex()
    lines += ['\t# password, hashed with the network name', '\tpsk=' + psk, '\tkey_mgmt=WPA-PSK']
else:
    lines += ['\tkey_mgmt=NONE']
lines += ['\tscan_ssid=1', '}', '']
open(sys.argv[1], 'w').write('\n'.join(lines))
EOF
	printf '\n# Added by make_image.sh (WIFI_SSID)\nether wlan\n' >> "$TMPD/macos9rpi.txt"
	echo "Wi-Fi: network \"$WIFI_SSID\", \"ether wlan\" enabled"
else
	cp "$TEMPLATE/wpa_supplicant.conf" "$TMPD/wpa_supplicant.conf"
fi
MC "$TMPD/macos9rpi.txt" ::/
MC "$TMPD/wpa_supplicant.conf" ::/
if [ -n "$ROM" ]; then
	MC "$ROM" "::/Mac OS ROM"
	echo "Added ROM: $ROM"
fi
if [ -n "$DISK" ]; then
	echo "Copying Mac disk $DISK into partition 2 ..."
	/bin/dd if="$DISK" of="$OUT" bs=1m seek=$((START2 / 2048)) conv=notrunc status=none
fi
mdir -i "$IMG" ::/

if [ "${QEMU:-0}" = "1" ]; then
	QIMG="$(dirname "$OUT")/qemu-sd.img"
	rm -f "$QIMG"
	/bin/cp -c "$OUT" "$QIMG" 2>/dev/null || cp "$OUT" "$QIMG"
	SZ=$(/usr/bin/stat -f %z "$QIMG")
	P=1; while [ $P -lt "$SZ" ]; do P=$((P * 2)); done
	truncate -s $P "$QIMG" 2>/dev/null || python3 -c "import os,sys; os.truncate(sys.argv[1], int(sys.argv[2]))" "$QIMG" $P
	echo "QEMU image: $QIMG ($((P >> 20)) MB)"
fi

echo "Done: $OUT"
