# TheHulk's drives

GEMDRIVE, the program in the cartridge, gives the Atari up to nine drives:

| Drive | What it is | Writable |
|---|---|---|
| **Flash drive** (`I:` by default) | about 10 MB in the ESP32's own flash; always there, no card or network needed | yes, except the built-in files |
| **SD card** | a folder on the microSD card (FAT, 8.3 names) | yes |
| **TNFS** | a share on a TNFS server in your network | yes |

You set them up with `THEHULK.PRG` on the flash drive. The changes take effect after a
reset of the Atari.

## The flash drive

- It is a FAT volume with wear levelling in the `hulkfs` partition of the ESP32's flash.
  It is formatted the first time the Atari uses it.
- Its root also shows the files built into the firmware: `THEHULK.PRG` (the settings) and
  `STINGINS.TOS` (the STinG installer, see [NETWORK.md](NETWORK.md)). They take no space on
  the drive, cannot be deleted or overwritten, and are always the version of the firmware.
  A file of your own with the same name in the root would be hidden by them.
- Writing is slower than on the SD card: flash is erased in blocks of 4 KB.
- Like a hard disk, do not switch the Atari off while it is writing.
- A firmware update keeps the drive and its files.

## Drive letters

Every drive gets a letter in `THEHULK.PRG`. **Use `C:` to `P:`** on the original TOS:

- The original TOS (GEMDOS, every version up to 4.04) only knows the drives `A:` to `P:`.
  A drive at `Q:` to `Z:` can still be read with full paths (`S:\FILE.PRG`), but everything
  that uses the *current drive* goes to another drive. Example: the desktop copies a folder
  to `S:` and reports a name conflict, because it looked for the folder on the drive it
  came from.
- MiNT, MagiC and EmuTOS know `A:` to `Z:`.

The letter belongs to the cartridge, not to the Atari, so `THEHULK.PRG` allows `Q:` to `Z:`
but warns when you choose one. On the original TOS it says so: "This Atari runs TOS x.xx,
which only knows A: to P:".

A suggestion with a hard disk of six partitions (`C:` to `H:`): the flash drive at `I:`, the
SD card and TNFS drives at `J:` to `P:`.

On an ST without hard disk you can give the flash drive the letter `C:`. Starting the
Atari from it (AUTO folder, `DESKTOP.INF`) is not tested yet.
