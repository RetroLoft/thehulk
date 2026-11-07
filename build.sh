#!/usr/bin/env bash
# Build everything for TheHulk, in dependency order:
#   1. fpga/            gateware (yosys, nextpnr-ice40, icepack)
#   2. atari/gemdrive/  GEMDRIVE cartridge image (stcmd / atarist-toolkit-docker)
#   3. atari/config/    configuration program (m68k-atari-mint-gcc + gemlib)
#      atari/stingins/  STinG installer with CART_NET.STX (m68k-atari-mint-gcc)
#   4. esp32/           ESP32-S3 firmware (ESP-IDF), embeds 1, 2 and 3
#
#   ./build.sh                 build everything
#   ./build.sh flash [PORT]    build, flash the ESP32 and open the serial monitor
#   ./build.sh sim             run the gateware simulation
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

if [ ! -x "$ROOT/.venv-fpga/bin/yowasp-yosys" ] && ! command -v yosys >/dev/null; then
    echo "Installing the FPGA toolchain (YoWASP yosys/nextpnr) in .venv-fpga"
    python3 -m venv "$ROOT/.venv-fpga"
    "$ROOT/.venv-fpga/bin/pip" install -q yowasp-yosys yowasp-nextpnr-ice40
fi

if [ "${1:-}" = "sim" ]; then
    make -C "$ROOT/fpga" sim
    exit
fi

make -C "$ROOT/fpga"
(cd "$ROOT/atari/gemdrive" && ./build.sh "$(pwd)" release)
make -C "$ROOT/atari/config"
make -C "$ROOT/atari/stingins"

: "${IDF_PATH:=$HOME/esp/esp-idf}"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null
cd "$ROOT/esp32"
if [ "${1:-}" = "flash" ]; then
    idf.py ${2:+-p "$2"} build flash monitor
else
    idf.py build
fi
