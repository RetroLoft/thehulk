# TheHulk hardware v2

KiCad project: `hardware/dev-v2/`. Production files (JLCPCB): `hardware/dev-v2/production/`.

## Overview

- **ESP32-S3-DevKitC-1 (N16R8)** on headers: WiFi, web server, SD card, buttons, display,
  loads the FPGA.
- **Lattice iCE40UP5K-SG48** FPGA: Atari ST cartridge port emulation. The 128 KB cartridge
  space (/ROM4 0xFA0000, /ROM3 0xFB0000) lives in the four SPRAM blocks.
- **Level shifters** between the 5 V cartridge port and the 3.3 V FPGA:
  U8 (SN74LVC16T245) address bus + /UDS, Atari → FPGA, always enabled;
  U9 (SN74LVC16T245) data bus, FPGA → Atari, enabled by /ROM3 or /ROM4 (U5);
  U10 (74LVC2T45) /ROM3 and /ROM4.

## FPGA pins

`fpga/thehulk.pcf` is the authoritative pin list (generated from the schematic).

- The UP5K RGB0..2 pads (pins 39–41) are open drain: they cannot drive high, so no data
  bits on them. Their input path works, so they carry **A14, A15** (inputs) and **/IRQ**
  (open drain output, pull-up R9).
- D0–D4 on pins 34–38, the rest of the data and address bus on ordinary I/O.
- SPI bus to the ESP32: SCK 15, MOSI 17 (SPI_SI), MISO 14 (SPI_SO), CS 16 (SPI_SS). The
  same pins load the bitstream (slave SPI, SS held low).
- No clock input: the gateware runs from the internal 48 MHz oscillator (±10 %).

## ESP32 pins

| GPIO | Function |
|---|---|
| 1 / 2 | FPGA CRESET_B / CDONE |
| 10 / 11 / 12 / 13 | FPGA SPI: CS / MOSI / SCK / MISO |
| 14 | /IRQ from the FPGA |
| 40 | PWR_ON: +3V3_LOGIC via R10 (FPGA side powered) |
| 41 | SD card detect (internal pull-up) |
| 4 / 5 / 6 / 7 | SD + display SPI: SCLK / MOSI / MISO / SD_CS |
| 18 / 21 / 47 / 38 | display CS / DC / RST / backlight |
| 15 / 16 / 8 / 9 | buttons 1–4 (active low) |
| 17 | Atari reset via Q2 (J4, optional) |
| 39 | buzzer |
| 48 | DevKitC on-board WS2812 LED (status) |

## Power

- The FPGA side (+5V_SYS → TLV75512 1.2 V / TLV75533 3.3 V) is powered from the cartridge
  port only (+5V_CART → LM66100). The LM66100 has CE on VOUT, as in the datasheet, so the
  board never feeds 5 V back into a switched-off Atari.
- The DevKitC can run from USB while the Atari is off. The firmware watches PWR_ON, drives
  all FPGA lines low while the FPGA side is unpowered, and configures the FPGA as soon as
  the Atari is switched on.

## Boot timing

The fastest case measured is an ST with TOS 1.04 and no hard disk: the first cartridge
check comes ~0.67 s after power-on. The cartridge (GEMDRIVE) is enabled ~0.3 s after
power-on, also when the ESP32 and the Atari are switched on together. TT and Falcon do a
memory test first. A diagnostic cartridge (checked at the first instruction) needs a
reset after power-on.

## The Falcon030

The Falcon reads a wrong word from the cartridge now and then. With gateware v20 (one clock
less from address to data) this went down to about 8 in 4.7 million direct reads, but not to
zero. GEMDRIVE therefore checks every block:
- **Read:** on TOS 4 (the Falcon), the ESP32 switches on `SHARED_VARIABLE_READ_CHECK`. GEMDRIVE
  then compares the sum of the words in each copied block with the sum from the ESP32, and
  copies the block again from the cartridge when they differ. This covers the drives
  (`Fread`) and the floppy emulator.
- **Write:** on every machine, the ESP32 sends back the sum of each 2 KB write block, and
  GEMDRIVE sends the block again when it differs.
- **Retries:** a read block is copied again up to 3 times; a write block is sent up to 3
  times in all. After that GEMDRIVE returns an error rather than wrong data.

With these checks, copies on the Falcon come out complete and correct.

## Parts

- microSD: push-push socket HRO TF-01A (LCSC C91145), footprint `TF-SMD_TF-01A` (locating
  holes NPTH), symbol `TF-01A.kicad_sym`.
- Through-hole parts (DevKitC headers, buttons, buzzer, display and reset connectors) and
  the cartridge edge are not in the JLCPCB BOM.
