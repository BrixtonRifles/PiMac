#!/usr/bin/env bash
# Assembles the SD card contents in build/sdcard/, and optionally copies them
# to a mounted FAT32 card:   ./scripts/make-sdcard.sh /Volumes/PIMAC
#
# Never formats anything. Existing roms/, disks/, floppies/ and pimac.cfg on the card are
# left untouched.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$ROOT/build/sdcard"
FIRMWARE="$ROOT/deps/firmware"

KERNEL="$(ls -t "$ROOT"/src/kernel*.img 2>/dev/null | head -n 1 || true)"
[ -n "$KERNEL" ] || { echo "error: no kernel image; run make first" >&2; exit 1; }
[ -f "$FIRMWARE/bootcode.bin" ] || { echo "error: firmware missing; run make deps" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/roms" "$STAGE/disks" "$STAGE/floppies"

cp "$FIRMWARE"/{bootcode.bin,start_cd.elf,fixup_cd.dat,start.elf,fixup.dat,LICENCE.broadcom} "$STAGE/"
cp "$KERNEL" "$STAGE/"
sed "s/^kernel=.*/kernel=$(basename "$KERNEL")/" "$ROOT/sdcard/config.txt" > "$STAGE/config.txt"
cp "$ROOT/sdcard/cmdline.txt" "$ROOT/sdcard/pimac.cfg" "$STAGE/"
cp "$ROOT/sdcard/README-roms.txt" "$STAGE/roms/README.txt"
cp "$ROOT/sdcard/README-disks.txt" "$STAGE/disks/README.txt"
cp "$ROOT/sdcard/README-floppies.txt" "$STAGE/floppies/README.txt"

echo "==> Staged SD card contents in $STAGE"

if [ $# -ge 1 ]; then
	DEST="$1"
	[ -d "$DEST" ] || { echo "error: $DEST is not a mounted volume" >&2; exit 1; }
	echo "==> Copying to $DEST"
	for f in "$STAGE"/*; do
		name="$(basename "$f")"
		case "$name" in
			roms|disks|floppies) mkdir -p "$DEST/$name"; cp -n "$f/README.txt" "$DEST/$name/" 2>/dev/null || true ;;
			pimac.cfg) [ -e "$DEST/pimac.cfg" ] || cp "$f" "$DEST/" ;;
			*) cp "$f" "$DEST/" ;;
		esac
	done
	# Stop macOS littering the card (harmless, but slows directory scans).
	touch "$DEST/.metadata_never_index" 2>/dev/null || true
	echo "==> Done. Eject the card before removing it."
fi
