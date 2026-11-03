#!/usr/bin/env bash
# Fetches and builds third-party dependencies into deps/:
#   - Circle (bare-metal C++ framework for the Raspberry Pi), pinned tag
#   - Raspberry Pi GPU boot firmware
#   - Musashi (MIT-licensed 68000 CPU core), opcode tables generated on the host
# Re-run safely at any time.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CIRCLE_TAG="${CIRCLE_TAG:-Step51.1.1}"
FIRMWARE_REF="${FIRMWARE_REF:-stable}"
PREFIX="${PREFIX:-arm-none-eabi-}"
MUSASHI_REF="${MUSASHI_REF:-313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd}" # v4.60

CIRCLE="$ROOT/deps/circle"
FIRMWARE="$ROOT/deps/firmware"
MUSASHI="$ROOT/deps/musashi"
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

echo "==> Configuring Circle for Raspberry Pi 3, AArch32"
cat > "$CIRCLE/Config.mk" <<EOF
RASPPI = 3
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
echo "==> Generating Musashi opcode tables"
cc -O2 -o "$MUSASHI/m68kmake" "$MUSASHI/m68kmake.c"
(cd "$MUSASHI" && ./m68kmake . m68k_in.c)

echo "==> Downloading Raspberry Pi firmware ($FIRMWARE_REF)"
mkdir -p "$FIRMWARE"
for f in bootcode.bin start.elf fixup.dat start_cd.elf fixup_cd.dat LICENCE.broadcom; do
	curl -fsSL -o "$FIRMWARE/$f" \
		"https://raw.githubusercontent.com/raspberrypi/firmware/$FIRMWARE_REF/boot/$f"
done

echo "==> Done. Now run: make"
