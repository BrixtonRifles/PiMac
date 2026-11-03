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

## Hardware notes
- The user's board is actually a Raspberry Pi 3 Model B+ (rev a020d4); the same RASPPI=3 build works.
- Keyboards such as the Keychron Q1 Max register their own mouse interface, which can
  take "mouse1". PiInput therefore attaches to every ukbdN / mouseN, never just device 1.
- F12 > USB diagnostics shows the registered devices and the Circle log on screen.
