# The floppy emulator

TheHulk can show the Atari a disk image as drive `A:` or `B:`. You choose it on the
[web page](WEB.md), on the **Floppies** tab. The Atari boots from it like
from a real disk.

**An SD card is required.** The images are on the card, and TheHulk cannot use one without
it.

## Images

The images are in the folder `\FLOPPIES` on the SD card. TheHulk creates the folder when
you add the first image. There are two ways to get one there:

- **Add an image...** sends a file from your phone or computer to the card.
- Copy a file into `\FLOPPIES` yourself, on a PC.

Names are 8.3 and end in `.ST` or `.MSA`. **Rename** and **Delete** work on the card.

| File | What it is |
|---|---|
| `.ST` | a plain copy of the disk, sector by sector |
| `.MSA` | the same, compressed (Magic Shadow Archiver). TheHulk unpacks it when you choose it |

Other formats are not supported. Unpack a `.ZIP` or `.GZ` first.

TheHulk takes the layout (tracks, sides, sectors per track) from the boot sector, or from
the size of the file when the boot sector is of no use, as on many game disks. Disks of
360 KB up to 1.44 MB work. The page marks a file it cannot use with *not usable*.

## Using a floppy

1. Choose the drive (`A:` to boot from it) and whether you want **TheHulk's drives next to
   the floppy**.
2. Press **Use** next to an image.
3. Atari on: press **reset**. Atari off: switch it on.

**Changing the disk.** While the Atari runs, press **Use** next to another image, with the
same drive and the same choice for the drives. That is like changing a floppy: no reset,
and the Atari sees the other disk at once. This is how you play a game with several disks.

**Taking it out.** **Take it out** on the page, or a **short press on button 1**, goes back
to GEMDRIVE from the next reset.

**With a bootable hard disk** on the Atari (ACSI, SCSI or IDE), TOS starts the hard disk
driver after the floppy, and the `AUTO` folder of the hard disk runs instead of the one on
the floppy, as with a real disk. Hold **Alternate** while the Atari starts to skip the hard
disk.

The choice is kept over a restart. TheHulk reads the image from the card when the Atari
starts, so the card must stay in. The status LED blinks **green** while a floppy is chosen.

## With or without TheHulk's drives

Without them, the default, nothing else of TheHulk is installed: no drives, no clock, no
STinG, and all the memory is free. That is best for games, and the Atari starts faster.

With them, GEMDRIVE starts as usual and the floppy is there too, for instance to copy
files from the floppy to a TheHulk drive.

## What it can and cannot do

The emulator works at the level of the operating system (the BIOS and XBIOS calls of TOS),
not at the level of the floppy controller chip.

- Programs that load through TOS work: GEMDOS programs and disks, and most boot-sector
  loaders that use `Floprd`.
- Programs that talk to the floppy controller themselves do **not** work, because they do not
  go through TOS. That is the case for many copy-protected games, and it is also why only
  plain images are supported: formats that hold the copy protection would not help.
- A program that replaces the system vectors loses the emulator at that moment.
- The floppy is **read-only**, like a write-protected disk. A game cannot save to it.

Where there is a real drive, the other letter stays the real drive: with the image in `A:`,
`B:` is still what it was.

## Which Atari

Works on the ST computers (ST, STE, Mega ST(E)). Not tested yet on the TT and the Falcon.
