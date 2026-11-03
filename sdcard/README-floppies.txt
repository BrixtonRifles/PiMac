Put floppy disk images in this folder (.dsk, .img, .image, .dc42, .diskcopy).

Supported: 400K and 800K Macintosh disks and 720K and 1.44 MB (high-density)
disks, either as raw sector dumps (exactly 409600, 819200, 737280 or 1474560
bytes) or as Disk Copy 4.2 images. 720K and 1.44 MB disks need a ROM that
knows the SuperDrive (Classic, SE FDHD); a blank 1474560-byte file can be
initialized by the Mac.

Insert or eject a floppy from the F12 menu (Floppy, Left/Right to choose,
Enter to insert or eject). It goes in at once, no restart needed, and stays in
the drive across power cycles, so the Mac can start up from it.

Changes the Mac makes are written back to the image file a couple of seconds
after it stops writing, and when the disk is ejected. Mark a file read-only
(locked) to protect it: the Mac then sees a write-protected disk.
