#!/usr/bin/env bash
# Fetches and builds third-party dependencies into deps/:
#   - Circle (bare-metal C++ framework for the Raspberry Pi), pinned tag
#   - Raspberry Pi GPU boot firmware
# Re-run safely at any time.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CIRCLE_TAG="${CIRCLE_TAG:-Step51.1.1}"
FIRMWARE_REF="${FIRMWARE_REF:-stable}"
PREFIX="${PREFIX:-arm-none-eabi-}"

CIRCLE="$ROOT/deps/circle"
FIRMWARE="$ROOT/deps/firmware"
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

echo "==> Building Circle libraries"
(cd "$CIRCLE" && ./makeall -j"$JOBS")
make -C "$CIRCLE/addon/SDCard" -j"$JOBS"
make -C "$CIRCLE/addon/fatfs" -j"$JOBS"

echo "==> Downloading Raspberry Pi firmware ($FIRMWARE_REF)"
mkdir -p "$FIRMWARE"
for f in bootcode.bin start.elf fixup.dat start_cd.elf fixup_cd.dat LICENCE.broadcom; do
	curl -fsSL -o "$FIRMWARE/$f" \
		"https://raw.githubusercontent.com/raspberrypi/firmware/$FIRMWARE_REF/boot/$f"
done

echo "==> Done. Now run: make"
