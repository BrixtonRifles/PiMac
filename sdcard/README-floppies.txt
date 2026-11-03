Put floppy disk images in this folder (.dsk, .img, .image, .dc42, .diskcopy).

Supported: 400K and 800K Macintosh disks, either as raw sector dumps
(exactly 409600 or 819200 bytes) or as Disk Copy 4.2 images. 1.44 MB
(high-density) disks are not supported yet.

Insert or eject a floppy from the F12 menu (Floppy, Left/Right to choose,
Enter to insert or eject). It goes in at once, no restart needed, and stays in
the drive across power cycles, so the Mac can start up from it.

Changes the Mac makes are written back to the image file a couple of seconds
after it stops writing, and when the disk is ejected. Mark a file read-only
(locked) to protect it: the Mac then sees a write-protected disk.
