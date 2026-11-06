# Floppy Browser

A small macOS app for copying files on and off classic Mac floppy disk images,
for use with PiMac. Modern macOS can't read HFS disks, so it has its own HFS code.

- Opens raw images (`.img`, `.dsk`; 400K MFS disks aren't supported), Disk Copy 4.2
  images (`.image`) and Disk Copy 6 images (read only: saved as a new raw image).
- Drag files and folders from the Finder onto the window (or onto a folder in it) to
  copy them onto the disk. MacBinary (`.bin`) and BinHex (`.hqx`) files are decoded;
  files without a type/creator get one from their extension (`.txt` = TEXT/ttxt...).
- Drag items to the Finder to copy them off, keeping resource fork, type and creator.
- Drag within a window to move items, or between windows to copy them to another disk.
- New Folder, Delete, Rename (Return), Get Info (type, creator, locked, invisible),
  Rename Disk, New 1.44 MB / 800K blank disk, Check Disk, full undo.
- Nothing is written until File > Save. Saving writes a fresh, unfragmented volume
  that keeps the boot blocks, item IDs and Finder information (bootable disks stay
  bootable); Disk Copy 4.2 checksums are recalculated.

## Build

    ./build-app.sh        # -> build/Floppy Browser.app and build/hfstool
    swift test            # HFS unit tests (also round-trips ~/Documents/PiMac files/floppies)

`hfstool` does the same from the command line (`ls`, `check`, `extract`, `add`,
`mkdir`, `rm`, `rebuild`, `new`); it never changes its input image.

Testing aids: `FLOPPYBROWSER_SELFTEST=DIR` runs the app's change paths and saves
`DIR/selftest.img`; `FLOPPYBROWSER_SNAPSHOT=out.png` draws the first window and quits.

Checked against real Mac OS in PiMac's hostmac (2026-11-06): System 7.5 boots from
a rebuilt Disk Tools floppy with 120 added files, writes to it, and both that disk
and an app-made blank disk check clean after a Mac shutdown.
