Put hard disk images in this folder (.vhd, .dsk, .img, .hda, .hfv).

Fixed-size VHD files and raw images are supported. Dynamically expanding VHDs
are not yet: convert them to fixed size first (e.g. with qemu-img:
  qemu-img convert -O vpc -o subformat=fixed in.vhd out.vhd).

FAT32 limits individual files to 4 GB.
