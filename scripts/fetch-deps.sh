#!/usr/bin/env bash
# Fetches and builds third-party dependencies into deps/:
#   - Circle (bare-metal C++ framework for the Raspberry Pi), pinned tag, built
#     for one board: PI=3 (default; Pi 3, Zero 2 W) into deps/circle, PI=4
#     (Pi 4, 400, CM4) into deps/circle-pi4. Each needs its own tree, as
#     Circle builds in place.
#   - Raspberry Pi GPU boot firmware
#   - Musashi (MIT-licensed 68000 CPU core), opcode tables generated on the host
#   - SoftFloat 3e with the Bochs transcendental functions, as in MAME (BSD
#     licences), for the SE/30's 68882 FPU (src/fpu)
# Re-run safely at any time.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CIRCLE_TAG="${CIRCLE_TAG:-Step51.1.1}"
FIRMWARE_REF="${FIRMWARE_REF:-stable}"
PREFIX="${PREFIX:-arm-none-eabi-}"
MUSASHI_REF="${MUSASHI_REF:-313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd}" # v4.60
MAME_REF="${MAME_REF:-adb51e24801fa0eba72eab85e0cdac55d5fe7e2d}" # SoftFloat 3 from here

PI="${PI:-3}"
case "$PI" in
	3) CIRCLE="$ROOT/deps/circle" ;;
	4) CIRCLE="$ROOT/deps/circle-pi4" ;;
	*) echo "error: PI must be 3 or 4" >&2; exit 1 ;;
esac
FIRMWARE="$ROOT/deps/firmware"
MUSASHI="$ROOT/deps/musashi"
SOFTFLOAT3="$ROOT/deps/softfloat3"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

command -v "${PREFIX}gcc" >/dev/null || {
	echo "error: ${PREFIX}gcc not found. On macOS: brew install --cask gcc-arm-embedded" >&2
	exit 1
}

mkdir -p "$ROOT/deps"

if [ ! -d "$CIRCLE/.git" ]; then
	echo "==> Cloning Circle $CIRCLE_TAG"
	git clone --depth 1 --branch "$CIRCLE_TAG" https://github.com/rsta2/circle.git "$CIRCLE"
fi

echo "==> Configuring Circle for Raspberry Pi $PI, AArch32"
cat > "$CIRCLE/Config.mk" <<EOF
RASPPI = $PI
AARCH = 32
PREFIX = $PREFIX
EOF

# Fast seek lets the emulator jump around a multi-GB disk image without
# walking the FAT cluster chain on every access.
sed -i.orig 's/^#define FF_USE_FASTSEEK[[:space:]]*0/#define FF_USE_FASTSEEK	1/' "$CIRCLE/addon/fatfs/ffconf.h"
grep -q "FF_USE_FASTSEEK.*1" "$CIRCLE/addon/fatfs/ffconf.h" || { echo "error: could not enable FF_USE_FASTSEEK" >&2; exit 1; }

echo "==> Building Circle libraries"
(cd "$CIRCLE" && ./makeall -j"$JOBS")
make -C "$CIRCLE/addon/SDCard" -j"$JOBS"
make -C "$CIRCLE/addon/fatfs" clean >/dev/null # pick up the ffconf.h change
make -C "$CIRCLE/addon/fatfs" -j"$JOBS"

if [ ! -d "$MUSASHI/.git" ]; then
	echo "==> Cloning Musashi ($MUSASHI_REF)"
	git clone https://github.com/kstenerud/Musashi.git "$MUSASHI"
	(cd "$MUSASHI" && git checkout -q "$MUSASHI_REF")
fi
echo "==> Musashi at $(cd "$MUSASHI" && git rev-parse --short HEAD)"
# PiMac's changes to Musashi (scripts/musashi.patch): the per-instruction
# register snapshot only when asked for (virtual memory), 68030 bus faults
# as format $B frames that RTE restarts; FPU opcodes and the ROM's FPU probe
# (CPU-space MOVES) go to PiMac's 68882 when one is fitted (the SE/30), else
# take the F-line trap / bus error. An older version of the patch is undone
# first (it only touches these four files).
MUSASHI_PATCHED="m68k.h m68k_in.c m68kcpu.c m68kcpu.h"
if ! git -C "$MUSASHI" apply --reverse --check "$ROOT/scripts/musashi.patch" 2>/dev/null; then
	echo "==> Patching Musashi"
	(cd "$MUSASHI" && git checkout -q -- $MUSASHI_PATCHED)
	git -C "$MUSASHI" apply "$ROOT/scripts/musashi.patch"
fi
echo "==> Generating Musashi opcode tables"
cc -O2 -o "$MUSASHI/m68kmake" "$MUSASHI/m68kmake.c"
(cd "$MUSASHI" && ./m68kmake . m68k_in.c)
# PiMac decodes the 68030 MMU instructions itself (src/mac/musashi_mmu.h).
echo '#include "../../src/mac/musashi_mmu.h"' > "$MUSASHI/m68kmmu.h"

# SoftFloat 3e and bochs_ext exactly as MAME carries them (3rdparty/softfloat3),
# for the 68882 ported from MAME. A sparse, blobless fetch of that directory.
if [ ! -f "$SOFTFLOAT3/source/include/softfloat.h" ]; then
	echo "==> Fetching SoftFloat 3 from MAME ($MAME_REF)"
	TMP="$(mktemp -d)"
	git -C "$TMP" init -q
	git -C "$TMP" remote add origin https://github.com/mamedev/mame.git
	git -C "$TMP" sparse-checkout set --no-cone /3rdparty/softfloat3/
	git -C "$TMP" fetch -q --depth 1 --filter=blob:none origin "$MAME_REF"
	git -C "$TMP" checkout -q FETCH_HEAD
	rm -rf "$SOFTFLOAT3"
	mv "$TMP/3rdparty/softfloat3" "$SOFTFLOAT3"
	echo "$MAME_REF" > "$SOFTFLOAT3/MAME_REF"
	rm -rf "$TMP"
fi

echo "==> Downloading Raspberry Pi firmware ($FIRMWARE_REF)"
mkdir -p "$FIRMWARE"
# Pi 3 / Zero 2 W: bootcode.bin, start*.elf, fixup*.dat. Pi 4 / 400 / CM4:
# start4*.elf, fixup4*.dat (bootcode.bin is in its EEPROM). Circle wants the
# device tree for the Pi 4 family and the Zero 2 W.
for f in bootcode.bin start.elf fixup.dat start_cd.elf fixup_cd.dat \
	start4.elf fixup4.dat start4cd.elf fixup4cd.dat \
	bcm2711-rpi-4-b.dtb bcm2711-rpi-400.dtb bcm2711-rpi-cm4.dtb bcm2710-rpi-zero-2-w.dtb \
	LICENCE.broadcom COPYING.linux; do
	curl -fsSL -o "$FIRMWARE/$f" \
		"https://raw.githubusercontent.com/raspberrypi/firmware/$FIRMWARE_REF/boot/$f"
done

echo "==> Done. Now run: make PI=$PI"
