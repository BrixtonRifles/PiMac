# PiMac

A bare-metal classic Macintosh emulator for the Raspberry Pi 3 and 4, built
to bring a Macintosh Classic with a failed logic board back to life. The Pi
boots straight into the emulator; a hidden settings screen opens with **F12**.

- Target: Raspberry Pi 3 Model B/B+ (also A+, CM3, Zero 2 W) and, untested so far, Pi 4/400/CM4; AArch32, on [Circle](https://github.com/rsta2/circle). One SD card boots either: each board loads its own kernel
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
| 5 | 68000 core (Musashi) + Classic/SE hardware: VIA, RTC/PRAM, ADB, SCSI 5380, IWM, SCC | ✅ Classic boots to the Finder on the Pi |
| 6 | SCSI hard disk backed by the disk image, write-back + PRAM/clock saved to SD | ✅ works on the Pi |
| 7 | Sound: HDMI or headphone jack | ✅ works on the Pi |
| 7½ | 800K/400K floppy drive (raw `.dsk` and Disk Copy 4.2 images) | ✅ works on the Pi |
| 7¾ | SuperDrive: 1.44 MB and 720K (MFM) floppies through the SWIM | ✅ host-tested (boots, reads, writes, formats) |
| 7⅞ | External floppy drive (a second SuperDrive) | ✅ host-tested |
| 7.9 | Serial ports over Ethernet: network modem (modem port), LocalTalk over UDP (printer port) | ✅ host-tested |
| 7.95 | SCSI CD-ROM drive (AppleCD 300 at ID 3) reading `.toast`/`.iso` images | ✅ host-tested: mounts on the Classic, Classic II and Color Classic; not yet tried on the Pi |
| 8 | Boot-time tuning | ⏳ |
| 9 | Macintosh Classic II: 68030 + MMU, Eagle ASIC, Egret (ADB/clock/PRAM), sound | ✅ boots System 7.5.5 to the Finder on the Pi (54% of each frame) |
| 10 | Color Classic: Spice ASIC, 512×384 colour (1–16 bpp, Ariel CLUT), Sonora sound, Cuda | ✅ host-tested: boots System 7.5.5 to the Finder in 256 colours; not yet tried on the Pi |
| 10½ | Virtual memory on the Classic II / Color Classic (68030 page faults restart the instruction) | ✅ host-tested (24- and 32-bit); about 20% slower while VM is on |

With a Classic, SE, Classic II or Color Classic ROM in `/roms` the Pi boots straight into
the emulated Mac (the ROM decides the model). With any other ROM, or none, it shows a **test screen**: a Mac-style
desktop with the detected ROM and disk, the last key pressed and the mouse
position.

## Serial ports and AppleTalk (Ethernet)

Plug the Pi into Ethernet (DHCP). The F12 diagnostics page shows its address.

**Modem port: network modem.** A Hayes-compatible modem that dials TCP
instead of phone numbers. Use it from any Mac terminal program (ZTerm,
MacTerminal, ...): `ATDT bbs.example.com` connects to telnet port 23,
`ATDT 192.168.1.10:6400` to any port. `+++` (with a second's pause on each
side) returns to command mode, `ATH` hangs up, and `ATI`, `ATE`, `ATV`,
`ATQ`, `ATZ`, `ATO` and `ATS0=n` work as usual. With `modem_listen=PORT` in
`pimac.cfg`, a TCP connection to that port of the Pi rings the modem
(`ATA` answers, `ATS0=1` answers automatically), so BBS software on the Mac
can take calls.

**Printer port: LocalTalk.** AppleTalk over UDP multicast (239.192.76.84,
port 1954, the "LToUDP" format of Mini vMac and others). Emulated Macs on the
same network see each other, and [tashrouter](https://github.com/lampmerchant/tashrouter)
bridges to EtherTalk and Netatalk file servers. Turn AppleTalk on in the
Chooser (it starts off, as on a new Mac).

## Building (macOS)

```sh
brew install --cask gcc-arm-embedded   # arm-none-eabi toolchain
make deps                               # clones Circle, builds it, fetches Pi firmware
make                                    # Pi 3 / Zero 2 W -> build/kernels/kernel8-32.img
make deps PI=4                          # once: a second Circle tree for the Pi 4
make PI=4                               # Pi 4 / 400 / CM4 -> build/kernels/kernel7l.img
make kernels                            # both
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
/cdroms/    Game.toast        ← CD-ROM images: .toast/.iso/.cdr (2048-byte sectors, HFS or ISO 9660)
/pimac.cfg                    ← written by the F12 menu
/pimac.pram                   ← PRAM + clock (Classic); pimac-classic2.pram / pimac-colorclassic.pram for the others
```

The card gets every kernel built so far (`make kernels` for both boards).
On a Pi 4, use the HDMI port next to the power socket (HDMI0).

`./scripts/make-image.sh VERSION` builds both kernels and a flashable image
(`build/release/`), as published on the Releases page.

The script never formats the card, and never overwrites your `roms/`, `disks/`, `floppies/`, `cdroms/` or `pimac.cfg`.

A 3.3 V USB-to-serial adapter on GPIO 14/15 (pins 8/10, 115200 baud) shows the
boot log.

## Settings menu (F12)

Use the arrow keys to move and change values, Enter to choose, and F12 or Esc to return to the Mac.

| Setting | Values |
|---|---|
| ROM | any file in `/roms`. The ROM decides the model |
| Machine | (shown, not changed) the model the selected ROM runs, or "not emulated" |
| Memory | the sizes that model supports: Classic/SE 1, 2 or 4 MB; Classic II 2, 4, 6 or 10 MB; Color Classic 4, 6, 8 or 10 MB |
| CPU speed | Classic II and Color Classic: 100% (real speed), 125% or 150%. Faster helps games that overload the Mac (Monkey Island's intro on the Color Classic under System 7.5 needs 125%), at the cost of more Pi CPU; Diagnostics shows how much of each frame is used |
| Hard disk | any image in `/disks`, or none |
| Floppy | any image in `/floppies`, or empty. **Enter inserts or ejects at once**, no restart |
| Ext. floppy | the same for the external drive (a disk can be in only one drive at a time) |
| CD-ROM | any image in `/cdroms`, or empty. Enter inserts or ejects at once, like a floppy |
| Alt key | Command (Mac-like position) or Option |
| Sound | HDMI, headphone jack, or off |
| Modem port | Network modem, or off |
| Printer | LocalTalk network, or off |

Each floppy stays in its drive across power cycles (so the Mac can start up
from it) until you or the Mac eject it. The Mac's changes are written back to
the image file about two seconds after it stops writing, and on eject. Lock a
file (read-only attribute) to make the floppy write-protected.

A CD stays in the drive the same way. Discs are read-only. The Mac needs the
Apple CD-ROM and Foreign File Access extensions (in System 7.5's Easy
Install; add ISO 9660 File Access for PC discs).

PRAM (the Mac's settings memory) and the clock are saved to the SD card a few
seconds after the Mac changes them. Note that under System 7.5's Sound control
panel, the speaker volume is the **Volumes** page; the **Alert Volume** slider
is a Sound Manager setting that System 7.5 never saves, so it is back at
maximum after every restart (as on a real Mac, as far as the OS code shows).

The menu also has three actions: **Save & restart Mac**, **Restart Raspberry Pi** and **Return to Mac**.

## Layout

```
src/core/   portable C++: builds bare-metal *and* on the host (tests)
src/mac/    the emulated Mac: Musashi glue, VIA, RTC, ADB, SCSI, IWM, SCC,
            LocalTalk, network modem
src/host/   Circle glue: framebuffer, USB input, FatFs storage, disk images,
            network
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
