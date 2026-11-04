# PiMac - bare-metal classic Macintosh emulator for the Raspberry Pi 3 and 4
#
#   make deps     fetch + build Circle and the Pi firmware (once per board:
#                 make deps PI=4 for the Pi 4)
#   make          build the kernel image for PI=3 (default: Pi 3, Zero 2 W ->
#                 kernel8-32.img) or PI=4 (Pi 4, 400, CM4 -> kernel7l.img);
#                 each build is also kept in build/kernels/
#   make kernels  build both
#   make test     build and run host unit tests
#   make hostmac  build the emulator for this computer (tests/hostmac)
#   make preview  render tests/screen*.bmp (what HDMI would show)
#   make sdcard   stage SD card files in build/sdcard, with every kernel in
#                 build/kernels (one card boots both boards)
#                 (make sdcard CARD=/Volumes/PIMAC also copies to the card)
#   make clean

PI ?= 3
ifeq ($(PI),3)
CIRCLE := deps/circle
KERNEL_IMG := kernel8-32.img
else ifeq ($(PI),4)
CIRCLE := deps/circle-pi4
KERNEL_IMG := kernel7l.img
else
$(error PI must be 3 or 4)
endif

# src/ builds in place for one board at a time: switching boards cleans it.
STAMP := build/kernels/.last-pi

.PHONY: all kernel kernels deps test preview sdcard clean hostmac

all: kernel

deps:
	PI=$(PI) ./scripts/fetch-deps.sh

kernel:
	@test -f $(CIRCLE)/lib/libcircle.a || { echo "Circle is not built for PI=$(PI) yet: run 'make deps PI=$(PI)' first."; exit 1; }
	@test -f deps/musashi/m68kops.c || { echo "Musashi is missing: run 'make deps' first."; exit 1; }
	@mkdir -p build/kernels
	@if [ "$$(cat $(STAMP) 2>/dev/null)" != "$(PI)" ]; then \
		echo "==> Building for Raspberry Pi $(PI): cleaning src/"; \
		$(MAKE) -C src clean CIRCLEHOME=../$(CIRCLE) >/dev/null; \
	fi
	$(MAKE) -C src CIRCLEHOME=../$(CIRCLE)
	@echo $(PI) > $(STAMP)
	cp src/$(KERNEL_IMG) build/kernels/

kernels:
	$(MAKE) kernel PI=3
	$(MAKE) kernel PI=4

test:
	$(MAKE) -C tests run

preview:
	$(MAKE) -C tests previews

hostmac:
	$(MAKE) -C tests hostmac

sdcard: kernel
	./scripts/make-sdcard.sh $(CARD)

clean:
	-$(MAKE) -C src clean
	$(MAKE) -C tests clean
	rm -rf build
