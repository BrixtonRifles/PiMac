# PiMac — notes for Claude

Bare-metal Mac Classic emulator for the Raspberry Pi 3B (Circle, AArch32). See README.md.

## Rules
- `src/core/` must stay host-buildable: include only `core/compat.h` for
  platform types/functions, never a Circle header. No libc beyond
  memset/memcpy/memcmp/strlen. No exceptions, RTTI or STL.
- Anything touching Circle lives in `src/host/` or `kernel.cpp`.
- Every core change: run `make test` (and `make preview` for anything visual,
  then convert the .bmp files with `sips -s format png` and look at them).
- The kernel object lives on the small boot stack: allocate large buffers with `new`.
- USB handlers run in IRQ context: they only push to `InputQueue`.
- Key codes passed to machines are Mac virtual key codes (`core/keymap.h`).
- Never commit ROMs or disk images.

## Status
The Circle-dependent code builds cleanly against Circle Step51.1.1 (kernel8-32.img).

## Emulator
- src/mac/ is portable like src/core/ (only core/compat.h), plus Musashi via mac/cpu.cpp.
- Debug boot problems on the host first: `make hostmac`, then
  `tests/hostmac ROM DISK --frames N --shot-every K --trace` and look at tests/out/*.bmp
  (convert with sips). hostmac never writes to the disk image.
- The user's files: ~/Documents/PiMac files/ (Classic ROM A49F9914 with ROM disk;
  boot.vhd is a raw 1.75 GB disk with an Apple partition map + Apple_Driver43, no VHD footer).
- Timing: 7.8336 MHz, 352 cycles/line, 370 lines/frame (60.15 Hz). VIA ticks = cycles/10.
- Musashi: deps/musashi, pinned to 313ebf1 (v4.60) in scripts/fetch-deps.sh.
  Built with M68K_EMULATE_INT_ACK=2 (both makefiles) so IRQs are level-triggered;
  the default drops the IRQ once taken and the VIA interrupt is lost.
  Musashi's libc references are stubbed for the Pi in src/host/libc_stubs.cpp.
- Disassemble ROM/RAM with Musashi's m68kdasm.c (hostmac writes out/ram.bin).
- Hard-won hardware details: the RTC seconds answer on z00xaa01 (x ignored; the
  ROM reads both and compares). The Classic ROM sets BoxFlag 0x0B only if VIA PA3
  reads low as an input. ADB service requests in idle are answered at the next
  VBL, not instantly (the ROM drops one that lands during ADB init). Default PRAM
  has SPConfig 0x22 (AppleTalk off) because the SCC has no interrupts.
- Status: the Classic ROM boots the user's boot.vhd to the Finder, and the ROM
  disk with Cmd-Opt-X-O. boot.vhd has a corrupt catalog B-tree header (free-space
  offset 0x81F8), so creating folders gives error -127: a disk problem, not ours.

## Hardware notes
- The user's board is actually a Raspberry Pi 3 Model B+ (rev a020d4); the same RASPPI=3 build works.
- Keyboards such as the Keychron Q1 Max register their own mouse interface, which can
  take "mouse1". PiInput therefore attaches to every ukbdN / mouseN, never just device 1.
- F12 > USB diagnostics shows the registered devices and the Circle log on screen.
