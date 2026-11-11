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

# Licence notices for the third-party code compiled into the kernels (their
# BSD/MIT licences require them alongside the binaries).
SOFTFLOAT3="$ROOT/deps/softfloat3"
for f in "$ROOT/deps/musashi/readme.txt" "$SOFTFLOAT3/COPYING.txt" "$SOFTFLOAT3/bochs_ext/fsincos.c"; do
	[ -f "$f" ] || { echo "error: $f missing; run make deps" >&2; exit 1; }
done
{
	echo "Third-party software in PiMac's kernels (kernel8-32.img, kernel7l.img)"
	echo "======================================================================"
	echo
	echo "PiMac itself is under the GNU GPL v3 (LICENSE-PiMac.txt in release"
	echo "images; https://github.com/BrixtonRifles/PiMac). Circle, the bare-metal"
	echo "framework it is built on, is also GPL v3 (https://github.com/rsta2/circle)."
	echo "The Raspberry Pi firmware files have their own licences: LICENCE.broadcom"
	echo "and COPYING.linux."
	echo
	echo
	echo "Musashi 68000 CPU core (https://github.com/kstenerud/Musashi)"
	echo "-------------------------------------------------------------"
	awk '/^LICENSE AND COPYRIGHT:/{f=1} /^AVAILABILITY:/{f=0} f' "$ROOT/deps/musashi/readme.txt"
	echo
	echo "MC68881/MC68882 FPU (src/fpu/fpu68882.cpp), ported from MAME's m68kfpu.cpp"
	echo "---------------------------------------------------------------------------"
	echo
	cat <<'NOTICE'
Copyright Karl Stenerud, R. Belmont. Licensed under the BSD-3-Clause licence:

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

 3. Neither the name of the copyright holder nor the names of its
    contributors may be used to endorse or promote products derived from this
    software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
NOTICE
	echo
	echo
	echo "Berkeley SoftFloat Release 3e (as carried by MAME; NaN rules modified for"
	echo "the 68881 in PiMac's src/fpu/softfloat-68k)"
	echo "-------------------------------------------------------------------------"
	echo
	cat "$SOFTFLOAT3/COPYING.txt"
	echo
	echo
	echo "Bochs extensions to SoftFloat (transcendental functions, by Stanislav"
	echo "Shwartsman; derivative works of SoftFloat 2b, as carried by MAME)"
	echo "-------------------------------------------------------------------"
	echo
	awk '/^\/\*====/{n++} n<=2' "$SOFTFLOAT3/bochs_ext/fsincos.c" | sed -n '1,/^ \* ====/p'
} > "$STAGE/LICENSES-third-party.txt"

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
