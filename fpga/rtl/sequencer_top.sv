// =============================================================================
// sequencer_top.sv — Top-Level FPGA Sequencer Module (SystemVerilog)
// =============================================================================
// Wires together: order_parser → sequencer_core → dma_ring
//
// This is the top module instantiated in the AWS FPGA Shell (CL region).
//
// Interfaces:
//   - AXI-Stream inputs from network (one per bot gateway)
//   - AXI-Lite MMIO for host CPU to read sequenced orders
//   - Status registers for monitoring
// =============================================================================

`timescale 1ns / 1ps
`default_nettype none

module sequencer_top #(
    parameter int NUM_PORTS   = 8,
    parameter int ORDER_BITS  = 512,    // 64 bytes
    parameter int RING_DEPTH  = 16384
)(
    input  wire                     clk_250mhz,     // 250 MHz from Shell
    input  wire                     rst_n,

    // =========================================================================
    // AXI-Stream inputs (one per gateway port)
    // =========================================================================
    input  wire [NUM_PORTS-1:0]             s_axis_tvalid,
    input  wire [NUM_PORTS-1:0][ORDER_BITS-1:0] s_axis_tdata,
    input  wire [NUM_PORTS-1:0][7:0]        s_axis_tuser_contestant_id,
    output logic [NUM_PORTS-1:0]            s_axis_tready,

    // =========================================================================
    // Host MMIO (AXI-Lite slave)
    // =========================================================================
    input  wire                     mmio_rd_en,
    input  wire [15:0]              mmio_rd_addr,
    output logic [31:0]             mmio_rd_data,
    output logic                    mmio_rd_valid,

    input  wire                     mmio_wr_en,
    input  wire [15:0]              mmio_wr_addr,
    input  wire [31:0]              mmio_wr_data,

    // =========================================================================
    // Status LEDs / debug
    // =========================================================================
    output logic [3:0]              status_leds
);

    // =========================================================================
    // Internal signals
    // =========================================================================

    // Parser outputs (one parser per port)
    logic [NUM_PORTS-1:0]           parser_valid;
    logic [NUM_PORTS-1:0][7:0]      parser_msg_type;
    logic [NUM_PORTS-1:0][ORDER_BITS-1:0] parser_raw;
    logic [NUM_PORTS-1:0]           parser_ready;

    // Sequencer → DMA ring
    logic                           seq_out_valid;
    logic [63:0]                    seq_out_seq_no;
    logic [63:0]                    seq_out_timestamp;
    logic [7:0]                     seq_out_contestant_id;
    logic [7:0]                     seq_out_msg_type;
    logic [ORDER_BITS-1:0]          seq_out_data;
    logic                           seq_out_ready;

    // Sequencer stats
    logic [63:0]                    seq_total;
    logic [31:0]                    seq_drops;
    logic [NUM_PORTS-1:0]           seq_port_active;

    // DMA ring stats
    logic [31:0]                    dma_write_ptr;
    logic [31:0]                    dma_occupancy;
    logic [31:0]                    dma_drops;
    logic                           dma_full;
    logic                           dma_empty;

    // =========================================================================
    // Instantiate parsers (one per input port)
    // =========================================================================
    genvar gi;
    generate
        for (gi = 0; gi < NUM_PORTS; gi++) begin : gen_parsers
            order_parser #(
                .DATA_WIDTH(ORDER_BITS)
            ) u_parser (
                .clk        (clk_250mhz),
                .rst_n      (rst_n),
                .in_valid   (s_axis_tvalid[gi]),
                .in_data    (s_axis_tdata[gi]),
                .in_ready   (s_axis_tready[gi]),
                .out_valid  (parser_valid[gi]),
                .out_msg_type(parser_msg_type[gi]),
                .out_raw    (parser_raw[gi]),
                .out_ready  (parser_ready[gi]),
                // Unused parsed fields (sequencer takes raw)
                .out_side              (),
                .out_order_type        (),
                .out_instrument_id     (),
                .out_client_order_id   (),
                .out_price             (),
                .out_quantity          (),
                .out_exchange_order_id (),
                .out_is_cancel         (),
                .err_invalid_msg       ()
            );
        end
    endgenerate

    // =========================================================================
    // Sequencer Core
    // =========================================================================
    sequencer_core #(
        .NUM_PORTS  (NUM_PORTS),
        .ORDER_WIDTH(ORDER_BITS),
        .RING_DEPTH (RING_DEPTH)
    ) u_sequencer (
        .clk                (clk_250mhz),
        .rst_n              (rst_n),
        // Inputs from parsers
        .port_valid         (parser_valid),
        .port_data          (parser_raw),
        .port_contestant_id (s_axis_tuser_contestant_id),
        .port_msg_type      (parser_msg_type),
        .port_ready         (parser_ready),
        // Output
        .out_valid          (seq_out_valid),
        .out_seq_no         (seq_out_seq_no),
        .out_timestamp      (seq_out_timestamp),
        .out_contestant_id  (seq_out_contestant_id),
        .out_msg_type       (seq_out_msg_type),
        .out_data           (seq_out_data),
        .out_ready          (seq_out_ready),
        // Stats
        .stat_total_sequenced(seq_total),
        .stat_total_drops   (seq_drops),
        .stat_port_active   (seq_port_active)
    );

    // =========================================================================
    // Pack sequenced order into DMA ring entry (128 bytes = 1024 bits)
    // Layout: [seq_no:64][timestamp:64][contestant_id:8][msg_type:8][pad:48][order:512][pad:320]
    // =========================================================================
    localparam int ENTRY_BITS = 1024;
    logic [ENTRY_BITS-1:0] dma_entry;

    always_comb begin
        dma_entry = '0;
        dma_entry[63:0]     = seq_out_seq_no;
        dma_entry[127:64]   = seq_out_timestamp;
        dma_entry[135:128]  = seq_out_contestant_id;
        dma_entry[143:136]  = seq_out_msg_type;
        // Bits [191:144] = padding
        dma_entry[703:192]  = seq_out_data;
        // Bits [1023:704] = padding
    end

    // =========================================================================
    // DMA Ring Buffer
    // =========================================================================
    dma_ring #(
        .DEPTH      (RING_DEPTH),
        .ENTRY_WIDTH(ENTRY_BITS)
    ) u_dma_ring (
        .clk            (clk_250mhz),
        .rst_n          (rst_n),
        .wr_valid       (seq_out_valid),
        .wr_data        (dma_entry),
        .wr_ready       (seq_out_ready),
        // Host MMIO
        .host_rd_en     (mmio_rd_en),
        .host_rd_addr   (mmio_rd_addr),
        .host_rd_data   (mmio_rd_data),
        .host_rd_valid  (mmio_rd_valid),
        .host_wr_en     (mmio_wr_en),
        .host_wr_addr   (mmio_wr_addr),
        .host_wr_data   (mmio_wr_data),
        // Stats
        .stat_write_ptr (dma_write_ptr),
        .stat_occupancy (dma_occupancy),
        .stat_drops     (dma_drops),
        .stat_full      (dma_full),
        .stat_empty     (dma_empty)
    );

    // =========================================================================
    // Status LEDs
    // =========================================================================
    assign status_leds[0] = |seq_port_active;   // Any port active
    assign status_leds[1] = !dma_empty;          // Ring has data
    assign status_leds[2] = dma_full;            // Ring full (bad)
    assign status_leds[3] = (seq_drops > 0);     // Drops occurred (bad)

endmodule

`default_nettype wire
