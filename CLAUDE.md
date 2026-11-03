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
  has SPConfig 0x22 (AppleTalk off until the Chooser turns it on; 0x21 = on).
- Sound: MacCompact::CaptureSound reads the 370-word buffer at RAM top - 0x300
  (high bytes; lines 0-341 at VBL, the rest at frame end), volume PA0-2, enable PB7
  low. core/audio.cpp resamples to 48 kHz; host/pi_sound.cpp trims the rate to keep
  Circle's queue half full. `hostmac --wav FILE` records exactly what the Pi plays.
- Floppy: mac/iwm.cpp is a SWIM (IWM + ISM) and two SuperDrives (Iwm::Drive:
  0 internal, 1 external, each with its own track ring; IWM picks one with
  the SELECT line, the ISM with mode bits 1/2 + motor-on bit 7), at
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
  name the next disk). Settings.floppy[2] (pimac.cfg floppy= / floppy_external=);
  the kernel's floppy members are arrays by drive, and refuses the same file
  in both drives. hostmac: --floppy FILE [--floppy-at F] [--floppy-out FILE],
  and --floppy2* for the external drive.
  800K confirmed on the Pi 2026-11-03. 1.44 MB host-tested 2026-11-03 (System
  7.0.1 boots from a padded 1.44 image; a blank image initializes to valid HFS),
  not yet tried on the Pi. External drive host-tested 2026-11-03 (boots 800K and
  1.44 MB; formats with the internal drive in use); not yet tried on the Pi.
  720K and 400K untested.
- Serial: mac/scc.cpp is a Z8530 (channel A = modem port, B = printer;
  A1 = channel, A2 = data; IRQ level 2, VIA level 1). Async bytes are paced at
  the programmed baud rate; SDLC is whole frames: bytes between WR0 $80 and
  $C0 are one frame (how the ROM's LAP sends, ~0x4337AC-0x433A40); received
  frames are fed byte by byte + 2 CRC bytes, EOF (RR1 bit 7) on the last, with
  WR6 address search. SccPort = what is plugged in. mac/localtalk.cpp: LToUDP
  (4-byte sender ID + LLAP frame, 239.192.76.84:1954); RTS/CTS stay local (a
  unicast RTS gets an instant fake CTS). mac/modem.cpp: Hayes modem over
  core/net.h StreamLink (telnet IAC handling on port 23, +++ guard 1 s).
  Pi: host/pi_net.cpp (CNetSubSystem DHCP, Initialize(FALSE); DNS/connect/
  accept run in CTasks; the main loop Yields while pacing frames). hostmac:
  --localtalk [--realtime] (LTDBG=1 dumps frames), --modem, --modem-listen P.
  Host-tested 2026-11-03: two hostmac instances resolve a node-address clash
  (ENQ/ACK) and exchange DDP. Not yet tried on the Pi; the modem has only
  unit tests (no terminal program on boot.vhd).
- Classic II (mac/mac_v8.cpp, ROM 3193670E): 68030 at 15.6672 MHz, Eagle ASIC
  as MAME's v8.cpp (704 cycles/line, 370 lines: same 60.15 Hz, VIA tick =
  cycles/20). 2 MB on board + SIMMs (2/4/6/10 MB); the SIMM maps at 0, board RAM
  after it and always at 0x800000; screen = board RAM + 0x1F9A80 (the ROM
  reaches it through logical 0xE0xxxx). Egret (mac/egret.cpp) at packet level;
  PRAM is also Egret memory 0x100-0x1FF (pseudo commands 02/08), which is how
  the ROM checks 'NuMc' and writes defaults - XPRAM 0x77 must be 1 (Mac OS) or
  SCSILoad (ROM 0x71F0) skips the disk's driver. Kernel PRAM file:
  pimac-classic2.pram. Boots boot.vhd (7.5.5) to the Finder on the host
  2026-11-03 (~45x real time on the dev Mac vs ~220x for the Classic). On the
  Pi (10 MB): 54% of each frame at 1400 MHz, SoC ~52 C (2026-11-03). Typing,
  both floppy drives, Restart/Shut Down confirmed on the Pi. Sound: the ASC
  FIFO interrupt is level-triggered on V8 (edge-triggered = only the chime).
  Every byte of a word/long write must reach the ASC: the Sound Manager's
  fill loop is move.l (A1)+,(FIFO), and dropping bytes halved the samples
  (all Sound Manager audio an octave high and twice as fast; the ROM chime
  uses byte writes and was fine). Check: Simple Beep = 440 Hz.
  F18000 writes (Color Classic brightness) are ignored.
  PRAM checked 2026-11-03 with hostmac --pram: the Sound control panel's
  Volumes slider writes XPRAM 0x08 (persists); its Alert Volume slider only
  calls SetSysBeepVolume (a Sound Manager global, never saved by 7.5.5 - no
  PRAM, resource or file write even at shutdown), so it is max after each boot.
  The Egret power-down (0x0A) at Shut Down is ignored. LogHex stops after 400
  lines (core/log.cpp) - raise it when tracing long runs.
  The model comes from the ROM (core/rom.h RomFamily, also each model's RAM
  sizes); there is no machine setting (old pimac.cfg machine= is ignored).
- 68030 MMU: Musashi's PMMU is off (M68K_EMULATE_PMMU=0); deps/musashi/m68kmmu.h
  just includes src/mac/musashi_mmu.h (installed by fetch-deps and both
  makefiles), which decodes PMOVE/PFLUSH/PLOAD/PTEST into mac/mmu030.cpp.
  Translation happens in mac/cpu.cpp's bus callbacks (inline cache hit path).
  Details the ROM depends on: limits in long descriptors; early termination
  *adds* the remaining logical bits to an unaligned page address; it switches
  24/32-bit tables with PLOAD + PMOVE-with-FD (so the ATC must survive a
  CRP write with FD set); and after a PMOVE that changes the mapping, the
  next instructions run from the 68030 I-cache (emulated: fetches from the
  same 256-byte block may stay untranslated). scripts/musashi.patch drops
  Musashi's per-instruction register snapshot (~20% faster; bus errors no
  longer roll registers back, as on a real 030).
- Status: the Classic ROM boots the user's boot.vhd to the Finder (confirmed on the Pi 2026-11-03), and the ROM
  disk with Cmd-Opt-X-O. boot.vhd has a corrupt catalog B-tree header (free-space
  offset 0x81F8), so creating folders gives error -127: a disk problem, not ours.

## Hardware notes
- The user's board is actually a Raspberry Pi 3 Model B+ (rev a020d4); the same RASPPI=3 build works.
- Bare metal starts the Pi 3 at 600 MHz: the kernel's CCPUThrottle asks for
  1400 MHz (back to 600 above socmaxtemp=75 in cmdline.txt). Classic at the
  Finder: 27% of each frame at 600 MHz, 12% at 1400 MHz (2026-11-03).
  Benchmark (hostmac --cpu 68030): Musashi's 68030 costs ~1.94x the 68000 per
  cycle, so a 16 MHz Classic II / Color Classic should need ~4x the Classic
  (~50-55% with an MMU page cache). Musashi's own PMMU is replaced (see
  "68030 MMU" above).
- Keyboards such as the Keychron Q1 Max register their own mouse interface, which can
  take "mouse1". PiInput therefore attaches to every ukbdN / mouseN, never just device 1.
- F12 > USB diagnostics shows the registered devices and the Circle log on screen.
