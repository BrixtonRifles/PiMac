#!/usr/bin/env bash
# Assembles the SD card contents in build/sdcard/, and optionally copies them
# to a mounted FAT32 card:   ./scripts/make-sdcard.sh /Volumes/PIMAC
#
# Never formats anything. Existing roms/, disks/, floppies/, cdroms/ and pimac.cfg on the card are
# left untouched.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$ROOT/build/sdcard"
FIRMWARE="$ROOT/deps/firmware"

# Every board's kernel built so far (make / make PI=4 / make kernels keep
# them in build/kernels): kernel8-32.img = Pi 3 / Zero 2 W, kernel7l.img =
# Pi 4. config.txt makes each board load its own.
KERNELS=("$ROOT"/build/kernels/kernel*.img)
[ -e "${KERNELS[0]}" ] || { echo "error: no kernel image; run make (or make kernels) first" >&2; exit 1; }
FIRMWARE_FILES=(bootcode.bin start_cd.elf fixup_cd.dat start.elf fixup.dat
	start4cd.elf fixup4cd.dat start4.elf fixup4.dat
	bcm2711-rpi-4-b.dtb bcm2711-rpi-400.dtb bcm2711-rpi-cm4.dtb bcm2710-rpi-zero-2-w.dtb
	LICENCE.broadcom COPYING.linux)
for f in "${FIRMWARE_FILES[@]}"; do
	[ -f "$FIRMWARE/$f" ] || { echo "error: firmware file $f missing; run make deps" >&2; exit 1; }
done

rm -rf "$STAGE"
mkdir -p "$STAGE/roms" "$STAGE/disks" "$STAGE/floppies" "$STAGE/cdroms"

for f in "${FIRMWARE_FILES[@]}"; do cp "$FIRMWARE/$f" "$STAGE/"; done
for k in "${KERNELS[@]}"; do
	cp "$k" "$STAGE/"
	echo "==> Kernel $(basename "$k") (built $(date -r "$k" '+%Y-%m-%d %H:%M'))"
done
cp "$ROOT/sdcard/config.txt" "$ROOT/sdcard/cmdline.txt" "$ROOT/sdcard/pimac.cfg" "$STAGE/"
cp "$ROOT/sdcard/README-roms.txt" "$STAGE/roms/README.txt"
cp "$ROOT/sdcard/README-disks.txt" "$STAGE/disks/README.txt"
cp "$ROOT/sdcard/README-floppies.txt" "$STAGE/floppies/README.txt"
cp "$ROOT/sdcard/README-cdroms.txt" "$STAGE/cdroms/README.txt"

echo "==> Staged SD card contents in $STAGE"

if [ $# -ge 1 ]; then
	DEST="$1"
	[ -d "$DEST" ] || { echo "error: $DEST is not a mounted volume" >&2; exit 1; }
	echo "==> Copying to $DEST"
	for f in "$STAGE"/*; do
		name="$(basename "$f")"
		case "$name" in
			roms|disks|floppies|cdroms) mkdir -p "$DEST/$name"; cp -n "$f/README.txt" "$DEST/$name/" 2>/dev/null || true ;;
			pimac.cfg) [ -e "$DEST/pimac.cfg" ] || cp "$f" "$DEST/" ;;
			*) cp "$f" "$DEST/" ;;
		esac
	done
	# Stop macOS littering the card (harmless, but slows directory scans).
	touch "$DEST/.metadata_never_index" 2>/dev/null || true
	echo "==> Done. Eject the card before removing it."
fi
