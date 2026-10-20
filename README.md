# TheHulk

Network and SD card storage cartridge for the Atari ST, STE, Mega ST(E), TT and Falcon,
built around an ESP32-S3 and a Lattice iCE40UP5K FPGA. Successor of SideTNFS.

## Repository layout

| Path | Contents |
|---|---|
| `hardware/v2/` | KiCad project and JLCPCB production files |
| `fpga/` | FPGA gateware (Verilog), pin constraints, simulation |
| `esp32/` | ESP32-S3 firmware (ESP-IDF); embeds the bitstream and GEMDRIVE |
| `atari/gemdrive/` | GEMDRIVE: the cartridge code the Atari runs (68000 assembly) |
| `atari/config/` | Configuration program for the Atari (GEM, C) |
| `tools/` | Development helpers |
| `docs/` | Documentation, see [docs/hardware-v2.md](docs/hardware-v2.md) |

## Building

```
./build.sh                     # everything
./build.sh flash /dev/ttyACM0  # and flash the ESP32, then open the serial monitor
./build.sh sim                 # gateware simulation
```

Requirements:
- FPGA: yosys, nextpnr-ice40 and icepack. `build.sh` installs the YoWASP builds from PyPI in
  `.venv-fpga` when no native tools are found.
- GEMDRIVE: `stcmd` from atarist-toolkit-docker (Docker image `logronoide/atarist-toolkit-docker`).
- Configuration program: `m68k-atari-mint-gcc` with gemlib.
- ESP32: ESP-IDF 5.5 in `~/esp/esp-idf` (or set `IDF_PATH`).

## How it works

1. The ESP32 waits until the Atari powers the FPGA side of the board, then loads the
   bitstream into the FPGA over SPI (~50 ms at 20 MHz).
2. It writes the cartridge image (GEMDRIVE) into the FPGA's SPRAM and enables the cartridge.
   The FPGA keeps the data bus updated with the word at the current address; the data
   buffer is enabled in hardware by /ROM3 or /ROM4.
3. When the Atari is switched off, the ESP32 drives all FPGA lines low and waits for the
   next power-up.

Status LED (on the DevKitC): blue = waiting for the Atari, yellow = starting, green =
cartridge active, red = error.

## License

GPL-3.0, see [LICENSE](LICENSE). GEMDRIVE is based on the SidecarTridge Multi-device
project by Diego Parrilla Santamaría (GOODDATA LABS SL).
