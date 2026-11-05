#!/bin/bash
#
# fresh_qemu.sh - Make a fresh QEMU SD image from out/macos9rpi.img (APFS clone,
# instant) and append extra preference lines, e.g.
#   scripts/fresh_qemu.sh "jit true" "jittest 200"
#
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/out/macos9rpi.img"
QIMG="$ROOT/out/qemu-sd.img"
rm -f "$QIMG"
/bin/cp -c "$OUT" "$QIMG" 2>/dev/null || cp "$OUT" "$QIMG"
SZ=$(/usr/bin/stat -f %z "$QIMG")
P=1; while [ $P -lt "$SZ" ]; do P=$((P * 2)); done
python3 -c "import os,sys; os.truncate(sys.argv[1], int(sys.argv[2]))" "$QIMG" $P
IMG="$QIMG@@1048576"
TMP="$(mktemp)"
mtype -i "$IMG" ::macos9rpi.txt > "$TMP"
for l in "$@"; do printf '%s\r\n' "$l" >> "$TMP"; done
mcopy -o -i "$IMG" "$TMP" ::macos9rpi.txt
rm -f "$TMP"
echo "QEMU image: $QIMG"
