# PiMac

A bare-metal classic Macintosh emulator for the Raspberry Pi 3 Model B, built
to bring a Macintosh Classic with a failed logic board back to life. The Pi
boots straight into the emulator; a hidden settings screen opens with **F12**.

- Target: Raspberry Pi 3 Model B, AArch32, on [Circle](https://github.com/rsta2/circle)
- Display: HDMI 1024×768. The 512×342 Classic screen is doubled to 1024×684, with black bars top and bottom
- Input: USB keyboard and mouse
- Media: your own ROM image and hard disk image (fixed VHD or raw) on the SD card

## Status

| Step | | |
|---|---|---|
| 1 | Boot, HDMI 1024×768, scaled 1-bit screen | ✅ written, host-tested |
| 2 | USB keyboard and mouse → Mac key codes | ✅ written, host-tested |
| 3 | F12 settings menu, `pimac.cfg` on SD | ✅ written, host-tested |
| 4 | SD card: list ROMs/disks, identify ROM, parse VHD footer | ✅ written |
| 5 | 68000 core + Classic hardware (VIA, SCC, IWM, SCSI, ADB, RTC) | ⏳ next |
| 6 | SCSI hard disk backed by the VHD, safe write-back | ⏳ |
| 7 | Sound (PWM / HDMI) | ⏳ |
| 8 | Boot-time tuning | ⏳ |
| 9 | More models (Plus, SE; Mac II as a stretch goal) | ⏳ |

At present the Pi boots to a **test screen**: a Mac-style desktop showing the
detected ROM and disk, the last key pressed and the mouse position. It exercises
every host path the real emulator will use.

## Building (macOS)

```sh
brew install --cask gcc-arm-embedded   # arm-none-eabi toolchain
make deps                               # clones Circle, builds it, fetches Pi firmware
make                                    # -> src/kernel8-32.img
make test                               # host unit tests (no Pi needed)
make preview                            # tests/screen.bmp, tests/screen-menu.bmp
```

## SD card

Format a card as FAT32 (MBR), then:

```sh
make sdcard CARD=/Volumes/PIMAC
```

Then copy your files:

```
/roms/   Mac Classic.rom      ← your ROM
/disks/  System7.vhd          ← your hard disk image
/pimac.cfg                    ← written by the F12 menu
```

The script never formats the card, and never overwrites your `roms/`, `disks/` or `pimac.cfg`.

A 3.3 V USB-to-serial adapter on GPIO 14/15 (pins 8/10, 115200 baud) shows the
boot log.

## Settings menu (F12)

Use the arrow keys to move and change values, Enter to choose, and F12 or Esc to return to the Mac.

| Setting | Values |
|---|---|
| Machine | Macintosh Classic, Plus, SE |
| Memory | 1, 2, 2.5 or 4 MB |
| ROM | any file in `/roms` |
| Hard disk | any image in `/disks`, or none |
| Alt key | Command (Mac-like position) or Option |

The menu also has three actions: **Save & restart Mac**, **Restart Raspberry Pi** and **Return to Mac**.

## Layout

```
src/core/   portable C++: builds bare-metal *and* on the host (tests)
src/host/   Circle glue: framebuffer, USB input, FatFs storage
src/kernel.cpp  boot sequence and 60.15 Hz main loop
tests/      host unit tests and screen preview renderer
sdcard/     config.txt, cmdline.txt and default settings
scripts/    dependency fetch and SD card staging
```

## Licensing

Circle is GPLv3, so PiMac binaries are GPLv3. ROMs and system software are
copyrighted by Apple; supply your own and never commit them.
