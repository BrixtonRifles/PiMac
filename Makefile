# PiMac - bare-metal classic Macintosh emulator for the Raspberry Pi 3 Model B
#
#   make deps     fetch + build Circle and the Pi firmware (once)
#   make          build the kernel image (src/kernel*.img)
#   make test     build and run host unit tests
#   make preview  render tests/screen*.bmp (what HDMI would show)
#   make sdcard   stage SD card files in build/sdcard
#                 (make sdcard CARD=/Volumes/PIMAC also copies to the card)
#   make clean

CIRCLE := deps/circle

.PHONY: all kernel deps test preview sdcard clean

all: kernel

deps:
	./scripts/fetch-deps.sh

kernel:
	@test -f $(CIRCLE)/lib/libcircle.a || { echo "Circle is not built yet: run 'make deps' first."; exit 1; }
	$(MAKE) -C src

test:
	$(MAKE) -C tests run

preview:
	$(MAKE) -C tests previews

sdcard: kernel
	./scripts/make-sdcard.sh $(CARD)

clean:
	-$(MAKE) -C src clean
	$(MAKE) -C tests clean
	rm -rf build
