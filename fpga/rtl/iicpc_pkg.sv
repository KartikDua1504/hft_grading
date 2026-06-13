// iicpc_pkg.sv — Shared Protocol Constants (SystemVerilog Package)
// All magic numbers for protocol decoding are defined here.
// Shared between sequencer_core, match_engine_fpga, and testbenches.

`timescale 1ns / 1ps

package iicpc_pkg;

    // Message Types (matches sdk/protocol.hpp MsgType enum)
    localparam logic [7:0] MSG_ORDER_ENTRY   = 8'd10;
    localparam logic [7:0] MSG_CANCEL_REQ    = 8'd11;

    // Side
    localparam logic [7:0] SIDE_BUY  = 8'd0;
    localparam logic [7:0] SIDE_SELL = 8'd1;

    // Order Type
    localparam logic [7:0] ORDER_LIMIT  = 8'd0;
    localparam logic [7:0] ORDER_MARKET = 8'd1;
    localparam logic [7:0] ORDER_IOC    = 8'd2;

    // Ack Status
    localparam logic [7:0] ACK_ACCEPTED         = 8'd0;
    localparam logic [7:0] ACK_REJECTED_PRICE   = 8'd1;
    localparam logic [7:0] ACK_REJECTED_QTY     = 8'd2;
    localparam logic [7:0] ACK_REJECTED_LIMIT   = 8'd3;
    localparam logic [7:0] ACK_REJECTED_SIDE    = 8'd4;
    localparam logic [7:0] ACK_REJECTED_UNKNOWN = 8'd5;
    localparam logic [7:0] ACK_CANCELLED        = 8'd6;

    // Pipeline / Performance constants
    localparam int MAX_SWEEP_DEPTH = 16;  // Max price levels consumed per aggressor

    // Clock frequency targets (for testbench reporting)
    localparam int CLK_FREQ_250MHZ = 250;  // 4ns period — base target
    localparam int CLK_FREQ_322MHZ = 322;  // 3.1ns — 10G Ethernet MAC native
    localparam int CLK_FREQ_500MHZ = 500;  // 2ns — Versal/UltraScale+ fast path

    // Pipeline stage count (for latency calculation)
    localparam int PIPELINE_STAGES_INSERT = 2;  // S1_DECODE + S2_INSERT
    localparam int PIPELINE_STAGES_MATCH  = 3;  // S1_DECODE + S2_MATCH + S3_EMIT
    localparam int PIPELINE_STAGES_CANCEL = 2;  // S1_DECODE + S2_CANCEL

endpackage
