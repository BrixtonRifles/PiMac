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
| 5 | 68000 core (Musashi) + Classic/SE hardware: VIA, RTC/PRAM, ADB, SCSI 5380, IWM and SCC stubs | ✅ Classic boots to the Finder on the Pi |
| 6 | SCSI hard disk backed by the disk image, write-back + PRAM/clock saved to SD | ✅ works on the Pi |
| 7 | Sound: HDMI or headphone jack | ✅ works on the Pi |
| 7½ | 800K/400K floppy drive (raw `.dsk` and Disk Copy 4.2 images) | ✅ works on the Pi |
| 7¾ | SuperDrive: 1.44 MB and 720K (MFM) floppies through the SWIM | ✅ host-tested (boots, reads, writes, formats) |
| 8 | Boot-time tuning | ⏳ |
| 9 | More models (Plus, SE; Mac II as a stretch goal) | ⏳ |

With a Classic or SE ROM in `/roms` the Pi boots straight into the emulated
Mac. With any other ROM, or none, it shows a **test screen**: a Mac-style
desktop with the detected ROM and disk, the last key pressed and the mouse
position.

Not emulated yet: the external floppy drive, serial ports/AppleTalk (PRAM starts
with AppleTalk off; turning it on hangs at startup).

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
/floppies/  Tools.dsk         ← floppy images: 400K/800K/720K/1.44 MB, raw or Disk Copy 4.2
/pimac.cfg                    ← written by the F12 menu
```

The script never formats the card, and never overwrites your `roms/`, `disks/`, `floppies/` or `pimac.cfg`.

A 3.3 V USB-to-serial adapter on GPIO 14/15 (pins 8/10, 115200 baud) shows the
boot log.

## Settings menu (F12)

Use the arrow keys to move and change values, Enter to choose, and F12 or Esc to return to the Mac.

| Setting | Values |
|---|---|
| Machine | Macintosh Classic, Plus, SE |
| Memory | 1, 2 or 4 MB |
| ROM | any file in `/roms` |
| Hard disk | any image in `/disks`, or none |
| Floppy | any image in `/floppies`, or empty. **Enter inserts or ejects at once**, no restart |
| Alt key | Command (Mac-like position) or Option |
| Sound | HDMI, headphone jack, or off |

The floppy stays in the drive across power cycles (so the Mac can start up
from it) until you or the Mac eject it. The Mac's changes are written back to
the image file about two seconds after it stops writing, and on eject. Lock a
file (read-only attribute) to make the floppy write-protected.

The menu also has three actions: **Save & restart Mac**, **Restart Raspberry Pi** and **Return to Mac**.

## Layout

```
src/core/   portable C++: builds bare-metal *and* on the host (tests)
src/mac/    the emulated Mac: Musashi glue, VIA, RTC, ADB, SCSI, IWM, SCC
src/host/   Circle glue: framebuffer, USB input, FatFs storage, disk images
src/kernel.cpp  boot sequence and 60.15 Hz main loop
tests/      host unit tests, screen previews, and hostmac (runs the
            emulator on this computer with your ROM/disk, saving screenshots)
sdcard/     config.txt, cmdline.txt and default settings
scripts/    dependency fetch and SD card staging
```

## Licensing

PiMac is free software under the GNU General Public License v3; see
[LICENSE](LICENSE). It builds on Circle (GPLv3) and the Musashi 68000 core
(Musashi licence), which `make deps` downloads; they are not part of this
repository.

Macintosh ROMs and system software are copyrighted by Apple and are not
included: supply your own and never commit them.
