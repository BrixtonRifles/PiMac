#!/usr/bin/env bash
# Builds a flashable SD card image (for balenaEtcher, Raspberry Pi Imager's
# "Use custom", dd...) from the staged card contents, on macOS:
#
#   ./scripts/make-image.sh [VERSION]      -> build/release/
#
# Builds both kernels first, so one card boots a Pi 3 / Zero 2 W or a Pi 4.
#
# Output:
#   pimac-VERSION.img.zip       MBR disk image, one 7.0 GB FAT32 partition
#                               (fits any "8 GB" card; most of it is empty
#                               room for disk images, and zips to a few MB)
#   pimac-VERSION-files.zip     the same files, to copy onto any FAT32 card
#
# No ROMs or disk images are included: the folders hold only their READMEs.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-dev}"
STAGE="$ROOT/build/sdcard"
OUT="$ROOT/build/release"
WORK="$ROOT/build/image"
NAME="pimac-$VERSION"
SECTORS=13671875 # 7,000,000,000 bytes

command -v hdiutil >/dev/null || { echo "error: needs macOS (hdiutil)" >&2; exit 1; }

make -C "$ROOT" kernels >/dev/null
"$ROOT/scripts/make-sdcard.sh" >/dev/null
cp "$ROOT/LICENSE" "$STAGE/LICENSE-PiMac.txt"
for d in roms disks floppies cdroms; do
	if [ -n "$(ls "$STAGE/$d" | grep -v '^README.txt$' || true)" ]; then
		echo "error: $STAGE/$d holds more than its README" >&2
		exit 1
	fi
done

rm -rf "$WORK" "$OUT"
mkdir -p "$WORK" "$OUT"

# Blank MBR disk with one FAT32 partition, filled through a private mount.
hdiutil create -sectors "$SECTORS" -fs "MS-DOS FAT32" -layout MBRSPUD -volname PIMAC \
	-o "$WORK/pimac.dmg" >/dev/null
MOUNT="$WORK/mnt"
mkdir -p "$MOUNT"
hdiutil attach "$WORK/pimac.dmg" -nobrowse -noautoopen -mountpoint "$MOUNT" >/dev/null
trap 'hdiutil detach "$MOUNT" -force >/dev/null 2>&1 || true' EXIT
mkdir -p "$MOUNT/.fseventsd" && touch "$MOUNT/.fseventsd/no_log" "$MOUNT/.metadata_never_index"
cp -RX "$STAGE"/ "$MOUNT"/
rm -rf "$MOUNT/.fseventsd" "$MOUNT/.Trashes" "$MOUNT/.Spotlight-V100"
find "$MOUNT" -name '._*' -delete
echo "==> Image contents:"
(cd "$MOUNT" && find . -type f | sort | sed 's/^/    /')
hdiutil detach "$MOUNT" >/dev/null
trap - EXIT

# Raw image (UDTO = plain sectors), then zip.
hdiutil convert "$WORK/pimac.dmg" -format UDTO -o "$WORK/raw" >/dev/null
mv "$WORK/raw.cdr" "$WORK/$NAME.img"
(cd "$WORK" && zip -q -9 "$OUT/$NAME.img.zip" "$NAME.img")
(cd "$STAGE" && zip -q -9 -r -X "$OUT/$NAME-files.zip" .)

echo "==> $(ls -l "$WORK/$NAME.img" | awk '{print $5}') byte image"
ls -l "$OUT"
(cd "$OUT" && shasum -a 256 *.zip | tee SHA256SUMS.txt)
