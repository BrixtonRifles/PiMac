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
- Sound: MacCompact::CaptureSound reads the 370-word buffer at RAM top - 0x300
  (high bytes; lines 0-341 at VBL, the rest at frame end), volume PA0-2, enable PB7
  low. core/audio.cpp resamples to 48 kHz; host/pi_sound.cpp trims the rate to keep
  Circle's queue half full. `hostmac --wav FILE` records exactly what the Pi plays.
- Floppy: mac/iwm.cpp is a SWIM (IWM + ISM) and one internal SuperDrive, at
  the level the controller sees (the ROM's .Sony driver runs unmodified, no
  timing). The current track/side is a ring exactly one revolution long:
  GCR nibbles (core/gcr.cpp) or MFM bytes + a mark bit per byte (core/mfm.cpp,
  12500 bytes HD / 6250 DD). The formatter relies on writes wrapping round.
  Written data is decoded back into sectors when IWM Q7 drops / ISM write
  mode ends (and on eject/Remove).
  SWIM details checked against the Classic ROM (Sony code ~0x435A18-0x436280):
  IWM->ISM = bit 6 of four mode writes going 1,0,1,1 ($57 $17 $57 $57); probe =
  phase reg F5/F6/F7 read back; ISM regs = offset&7, writes 0-7, reads 8-15.
  The ROM reads MFM through the *mark* register, polling handshake bit 7;
  the disk turns one byte per handshake poll while reading, and the ISM
  delivers nothing until a mark. Handshake bit 1 = CRC of the FIFO head byte
  nonzero (checked while CRC2 is still in the FIFO). Formatter waits for the
  index pulse on drive reg CA2(+SEL) in MFM mode (0 for the first 16 bytes of
  the ring). Drive: reg CA2+CA0 = 1 SuperDrive, CA2+CA0+SEL = MFM mode, all
  four = 0 for HD disk; command CA0+SEL: CA2 0 MFM / 1 GCR (MAME numbering
  agrees). Plus/SE ROMs never send the ISM sequence.
  core/floppy_image.cpp: raw 400K/800K/720K/1440K + Disk Copy 4.2 (checksums fixed on save).
  Kernel saves 2 s after the last Mac write, on eject and before restarts, to
  the file name kept in m_FloppyName (never m_Settings.floppy, which may already
  name the next disk). hostmac: --floppy FILE [--floppy-at F] [--floppy-out FILE].
  800K confirmed on the Pi 2026-11-03. 1.44 MB host-tested 2026-11-03 (System
  7.0.1 boots from a padded 1.44 image; a blank image initializes to valid HFS),
  not yet tried on the Pi. Not done: external drive; 720K and 400K untested.
- Status: the Classic ROM boots the user's boot.vhd to the Finder (confirmed on the Pi 2026-11-03), and the ROM
  disk with Cmd-Opt-X-O. boot.vhd has a corrupt catalog B-tree header (free-space
  offset 0x81F8), so creating folders gives error -127: a disk problem, not ours.

## Hardware notes
- The user's board is actually a Raspberry Pi 3 Model B+ (rev a020d4); the same RASPPI=3 build works.
- Keyboards such as the Keychron Q1 Max register their own mouse interface, which can
  take "mouse1". PiInput therefore attaches to every ukbdN / mouseN, never just device 1.
- F12 > USB diagnostics shows the registered devices and the Circle log on screen.
