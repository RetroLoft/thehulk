# The cartridge emulator

Instead of GEMDRIVE, TheHulk can show the Atari an ordinary cartridge: a game, a tool or a
diagnostic ROM. You choose it on the [web page](WEB.md), on the **Cartridges** tab.

While an image is in the cartridge, GEMDRIVE is not there: no TheHulk drives, no
`THEHULK.PRG`, no clock and no STinG. The web page and AP mode keep working.

**An SD card is required** to add and choose images. Once an image is chosen, it is in
TheHulk's own flash and the card may be taken out.

## Images

The images are in the folder `\CARTS` on the **SD card**. TheHulk creates the folder when
you add the first image. There are three ways to get one there:

- **Add an image...** sends a file from your phone or computer to the card.
- **Catalogue** shows the list of `roms.sidecartridge.com`. **Download** makes TheHulk fetch
  the image itself, so it needs the internet for that.
- Copy a file into `\CARTS` yourself, from the Atari or on a PC.

Names are 8.3 and end in `.STC`, `.IMG`, `.BIN`, `.ROM` or `.CAR`. **Rename** and **Delete**
work on the card.

| File | What TheHulk does with it |
|---|---|
| `.STC` of 128 KB + 4 bytes | leaves out the first 4 bytes |
| up to 128 KB | puts it at `$FA0000`; the rest of the 128 KB is filled with `$FF` |

## Changing the cartridge

1. Press **Use** next to an image. The Atari may be on or off.
2. The page shows *Loading...* and then *... is loaded*.
3. Atari on: press **reset**. Atari off: switch it on.

What runs from the cartridge at that moment may crash when the contents change; the reset
follows anyway. **Use GEMDRIVE** goes back the same way.

A **short press on button 1** also goes back to GEMDRIVE, at once. After a reset the Atari
has its drives again. (With GEMDRIVE in the cartridge, the same press switches the
cartridge off or on, see [WEB.md](WEB.md).)

The chosen image is copied to TheHulk's own flash, so it is there at power-on without the
card or Wi-Fi, and stays chosen over a restart. You can delete it from the card afterwards.

The status LED blinks **green** while an image is in the cartridge.

## Two kinds of cartridge

The page marks the second kind with **reset start**.

| First bytes | Kind | How it starts |
|---|---|---|
| `$ABCDEF42` | normal: most games and tools | at power-on or after a reset |
| `$FA52235F` | diagnostic type: DiagROM, ST Diagnostic, but also EmuTOS and some rippers | with the **reset button** |

TOS looks for the diagnostic type in its very first instructions. At power-on TheHulk is
not ready that early, so switch the Atari on and then press reset.

## Which Atari

Tested on a Mega STE and a Falcon. TheHulk works the same on both, but most cartridges were
written for the ST and its 68000, years before the Falcon came out, and some of them do not
run on a Falcon. That is the software on the cartridge, not TheHulk.
