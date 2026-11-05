#!/bin/bash
#
# get_wlan_firmware.sh - Download the Wi-Fi firmware for the Pi Zero W's
# CYW43438 chip into firmware/wlan/ (used by "ether wlan").
#
# Same files and revision as Circle's addon/wlan/firmware/Makefile
# (RPi-Distro/firmware-nonfree). make_image.sh copies them to /firmware/
# on the SD card's FAT partition.
#
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/firmware/wlan"
REV=c9d3ae6584ab79d19a4f94ccf701e888f9f87a53
BASEURL=https://github.com/RPi-Distro/firmware-nonfree/raw/$REV/debian/config/brcm80211

mkdir -p "$DEST"
fetch() {
	curl -fsSL -o "$DEST/$1" "$BASEURL/$2"
	echo "  $1 ($(wc -c < "$DEST/$1" | tr -d ' ') bytes)"
}
echo "Downloading Wi-Fi firmware to $DEST"
fetch brcmfmac43430-sdio.bin      cypress/cyfmac43430-sdio.bin
fetch brcmfmac43430-sdio.txt      brcm/brcmfmac43430-sdio.txt
fetch brcmfmac43430-sdio.clm_blob cypress/cyfmac43430-sdio.clm_blob
