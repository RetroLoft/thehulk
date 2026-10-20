# TheHulk FPGA gateware (iCE40UP5K-SG48, board v2)
# Tools: yosys, nextpnr-ice40, icepack. Uses the YoWASP builds from ../.venv-fpga
# when present (./build.sh installs them), otherwise native tools from PATH.

VENV    := ../.venv-fpga/bin
ifneq ($(wildcard $(VENV)/yowasp-yosys),)
YOSYS   := $(VENV)/yowasp-yosys
NEXTPNR := $(VENV)/yowasp-nextpnr-ice40
ICEPACK := $(VENV)/yowasp-icepack
YOSYS_DATDIR = $(shell $(VENV)/python -c "import yowasp_yosys,os;print(os.path.dirname(yowasp_yosys.__file__))")/share
else
YOSYS   := yosys
NEXTPNR := nextpnr-ice40
ICEPACK := icepack
YOSYS_DATDIR = $(shell yosys-config --datdir)
endif

TOP  := top
SRCS := rtl/top.v rtl/spi_slave.v
PCF  := thehulk.pcf
OUT  := build

all: $(OUT)/thehulk.bin

$(OUT)/thehulk.json: $(SRCS)
	@mkdir -p $(OUT)
	$(YOSYS) -q -l $(OUT)/yosys.log -p "synth_ice40 -top $(TOP) -json $@" $(SRCS)

$(OUT)/thehulk.asc: $(OUT)/thehulk.json $(PCF)
	$(NEXTPNR) --up5k --package sg48 --json $< --pcf $(PCF) --asc $@ \
		--freq 48 --log $(OUT)/nextpnr.log

$(OUT)/thehulk.bin: $(OUT)/thehulk.asc
	$(ICEPACK) $< $@

clean:
	rm -rf $(OUT)

.PHONY: all clean

# Simulation with CXXRTL (yosys) + g++
sim: $(OUT)/sim/tb
	$(OUT)/sim/tb

$(OUT)/sim/top_sim.cpp: $(SRCS) sim/ice40_sim.v
	@mkdir -p $(OUT)/sim
	$(YOSYS) -q -p "read_verilog -DSIM $(SRCS) sim/ice40_sim.v; hierarchy -top $(TOP); proc; flatten; opt -fast; write_cxxrtl -header $@"

$(OUT)/sim/tb: sim/tb.cpp $(OUT)/sim/top_sim.cpp
	g++ -O2 -std=c++14 -I$(YOSYS_DATDIR)/include/backends/cxxrtl/runtime -I$(OUT)/sim \
		sim/tb.cpp $(OUT)/sim/top_sim.cpp -o $@

.PHONY: sim
