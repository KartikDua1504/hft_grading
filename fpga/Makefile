# FPGA Makefile — Sequencer + Matching Engine Simulation
# Targets:
#   make sim          — Sequencer testbench (Verilator)
#   make sim_match    — Matching engine testbench (Verilator)
#   make sim_all      — Run both testbenches
#   make lint         — Static analysis
#   make clean        — Clean build artifacts

SHELL := /bin/bash

# RTL sources
RTL_DIR   := rtl
SIM_DIR   := sim

SEQ_RTL   := $(RTL_DIR)/iicpc_pkg.sv \
             $(RTL_DIR)/sequencer_core.sv \
             $(RTL_DIR)/order_parser.sv \
             $(RTL_DIR)/dma_ring.sv \
             $(RTL_DIR)/sequencer_top.sv

ME_RTL    := $(RTL_DIR)/iicpc_pkg.sv \
             $(RTL_DIR)/match_engine_fpga.sv

ALL_RTL   := $(SEQ_RTL) $(ME_RTL)

# Output
BUILD_DIR     := build
VER_SEQ_DIR   := $(BUILD_DIR)/ver_seq
VER_MATCH_DIR := $(BUILD_DIR)/ver_match

# Verilator common flags
VERILATOR := verilator
VER_COMMON := --binary --timing \
              -Wno-fatal \
              -Wno-PINCONNECTEMPTY \
              -Wno-UNUSEDPARAM \
              -Wno-UNUSEDSIGNAL \
              -Wno-IMPLICITSTATIC \
              -Wno-PROCASSINIT \
              -Wno-WIDTHTRUNC \
              -Wno-WIDTHEXPAND \
              -Wno-CASEINCOMPLETE \
              -Wno-LATCH \
              --trace \
              -sv \
              -CFLAGS "-O3"

.PHONY: all sim sim_match sim_all lint clean

all: sim_all

# Sequencer Simulation
sim: $(VER_SEQ_DIR)/Vtb_sequencer
	@echo ""
	@echo "=== Sequencer Testbench ==="
	$(VER_SEQ_DIR)/Vtb_sequencer
	@echo "Waveform: $(VER_SEQ_DIR)/dump.vcd"

$(VER_SEQ_DIR)/Vtb_sequencer: $(SEQ_RTL) $(SIM_DIR)/tb_sequencer.sv
	@mkdir -p $(VER_SEQ_DIR)
	$(VERILATOR) $(VER_COMMON) --top-module tb_sequencer \
	    -Mdir $(VER_SEQ_DIR) \
	    $(SEQ_RTL) $(SIM_DIR)/tb_sequencer.sv
	@$(MAKE) -C $(VER_SEQ_DIR) -f Vtb_sequencer.mk

# Matching Engine Simulation
sim_match: $(VER_MATCH_DIR)/Vtb_match_engine
	@echo ""
	@echo "=== Matching Engine Testbench ==="
	$(VER_MATCH_DIR)/Vtb_match_engine
	@echo "Waveform: $(VER_MATCH_DIR)/dump.vcd"

$(VER_MATCH_DIR)/Vtb_match_engine: $(ME_RTL) $(SIM_DIR)/tb_match_engine.sv
	@mkdir -p $(VER_MATCH_DIR)
	$(VERILATOR) $(VER_COMMON) --top-module tb_match_engine \
	    -Mdir $(VER_MATCH_DIR) \
	    $(ME_RTL) $(SIM_DIR)/tb_match_engine.sv
	@$(MAKE) -C $(VER_MATCH_DIR) -f Vtb_match_engine.mk

# Run All
sim_all: sim sim_match
	@echo ""
	@echo "=== ALL SIMULATIONS COMPLETE ==="

# Lint
lint: $(ALL_RTL)
	$(VERILATOR) --lint-only -Wno-fatal -sv $(ALL_RTL)

# Clean
clean:
	rm -rf $(BUILD_DIR)
	rm -f *.vcd *.wdb *.log *.jou
