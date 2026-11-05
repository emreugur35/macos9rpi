#!/bin/bash
#
# build_qemu_circle.sh - Build a QEMU (ARM target only) with Circle's DWC2 USB
# fix, needed to test the network ("ether usb") in QEMU: stock QEMU retries
# bulk transfers on NAK by itself, so Circle never receives USB Ethernet
# frames (see Circle doc/qemu.txt, smuehlst/qemu commit 62b39b2).
#
# Result: tools/qemu-circle/bin/qemu-system-arm (use with QEMU_BIN=...).
# Needs Homebrew's glib, pixman and libslirp (installed with "brew install
# qemu"); ninja and pkg-config are installed into a private Python venv.
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${QEMU_VERSION:-11.1.3}"
WORK="$ROOT/tools/qemu-build"
PREFIX="$ROOT/tools/qemu-circle"

mkdir -p "$WORK"
cd "$WORK"
[ -x venv/bin/ninja ] || { python3 -m venv venv && venv/bin/pip -q install ninja pkgconf; }
[ -f "qemu-$VER.tar.xz" ] || curl -fL -o "qemu-$VER.tar.xz" "https://download.qemu.org/qemu-$VER.tar.xz"
rm -rf "qemu-$VER"
tar xf "qemu-$VER.tar.xz"
cd "qemu-$VER"

python3 -I - hw/usb/hcd-dwc2.c <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = """            s->hreg1[index + 2] |= intr;
            do_intr = true;
        } else {"""
new = """            s->hreg1[index + 2] |= intr;
            do_intr = true;
            done = true;    /* Circle: complete the transfer on NAK */
        } else {"""
if s.count(old) != 1:
    sys.exit("hcd-dwc2.c: NAK handling not found, patch by hand (see Circle doc/qemu.txt)")
open(p, "w").write(s.replace(old, new))
EOF

mkdir build
cd build
export PATH="$WORK/venv/bin:$PATH"
export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig"
../configure --target-list=arm-softmmu --enable-slirp --disable-docs --disable-werror --prefix="$PREFIX"
ninja
ninja install
echo "Done: $PREFIX/bin/qemu-system-arm"
