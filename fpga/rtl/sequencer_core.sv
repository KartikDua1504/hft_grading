// sequencer_core.sv — FPGA Order Sequencer Core (SystemVerilog)
// Deterministic order sequencing in hardware. Assigns strictly monotonic
// 64-bit sequence numbers to incoming orders with sub-10ns latency.
//
// Architecture:
//   - N input ports (one per gateway/bot)
//   - Round-robin arbiter with priority override for fairness
//   - PTP-grade 64-bit timestamp counter (free-running, synced to clock)
//   - Single-cycle sequence assignment
//   - Output to DMA ring buffer for host CPU consumption
//
// Target: AMD Xilinx Virtex UltraScale+ (AWS F2)
// Clock: 250 MHz → 4ns per cycle → <10ns sequencing latency (2-3 cycles)

`timescale 1ns / 1ps
`default_nettype none

module sequencer_core #(
    parameter int NUM_PORTS     = 8,        // Number of input gateways
    parameter int ORDER_WIDTH   = 512,      // Bits per order message (64 bytes)
    parameter int SEQ_WIDTH     = 64,       // Sequence number width
    parameter int TIMESTAMP_W   = 64,       // Timestamp width
    parameter int RING_DEPTH    = 16384,    // Output ring buffer depth (power of 2)
    parameter int RING_ADDR_W   = $clog2(RING_DEPTH)
)(
    input  wire                     clk,
    input  wire                     rst_n,      // Active-low reset

    // Input ports (one per gateway)
    input  wire [NUM_PORTS-1:0]             port_valid,     // Order valid
    input  wire [NUM_PORTS-1:0][ORDER_WIDTH-1:0] port_data, // Order payload
    input  wire [NUM_PORTS-1:0][7:0]        port_contestant_id, // Which contestant
    input  wire [NUM_PORTS-1:0][7:0]        port_msg_type,  // ORDER_ENTRY=10, CANCEL=11
    output logic [NUM_PORTS-1:0]            port_ready,     // Backpressure

    // Sequenced output (to DMA ring buffer)
    output logic                            out_valid,
    output logic [SEQ_WIDTH-1:0]            out_seq_no,
    output logic [TIMESTAMP_W-1:0]          out_timestamp,
    output logic [7:0]                      out_contestant_id,
    output logic [7:0]                      out_msg_type,
    output logic [ORDER_WIDTH-1:0]          out_data,
    input  wire                             out_ready,      // Consumer backpressure

    // Status / debug
    output logic [SEQ_WIDTH-1:0]            stat_total_sequenced,
    output logic [31:0]                     stat_total_drops,
    output logic [NUM_PORTS-1:0]            stat_port_active
);

    // Free-running timestamp counter (PTP-grade, synced to clock)
    // At 250 MHz: increments every 4ns
    logic [TIMESTAMP_W-1:0] timestamp_counter;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            timestamp_counter <= '0;
        else
            timestamp_counter <= timestamp_counter + 1'b1;
    end

    // Sequence number counter — strictly monotonic
    logic [SEQ_WIDTH-1:0] seq_counter;

    // Round-Robin Arbiter
    // Grants one port per cycle. If multiple ports have valid data,
    // round-robin ensures fairness (no starvation).
    logic [$clog2(NUM_PORTS)-1:0] rr_priority;
    logic [$clog2(NUM_PORTS)-1:0] granted_port;
    logic                          grant_valid;

    // Find the next valid port starting from rr_priority
    always_comb begin
        grant_valid = 1'b0;
        granted_port = '0;

        // Two-pass round-robin: first from priority to end, then wrap
        for (int pass = 0; pass < 2; pass++) begin
            for (int i = 0; i < NUM_PORTS; i++) begin
                int idx;
                if (pass == 0)
                    idx = (int'(rr_priority) + i) % NUM_PORTS;
                else
                    idx = i;

                if (!grant_valid && port_valid[idx]) begin
                    granted_port = idx[$clog2(NUM_PORTS)-1:0];
                    grant_valid = 1'b1;
                end
            end
        end
    end

    // Backpressure: only the granted port is "ready" this cycle
    always_comb begin
        for (int i = 0; i < NUM_PORTS; i++) begin
            port_ready[i] = grant_valid &&
                            (granted_port == i[$clog2(NUM_PORTS)-1:0]) &&
                            out_ready;
        end
    end

    // Sequencing pipeline — single-cycle assignment
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            seq_counter          <= '0;
            out_valid            <= 1'b0;
            out_seq_no           <= '0;
            out_timestamp        <= '0;
            out_contestant_id    <= '0;
            out_msg_type         <= '0;
            out_data             <= '0;
            rr_priority          <= '0;
            stat_total_sequenced <= '0;
            stat_total_drops     <= '0;
            stat_port_active     <= '0;
        end else begin
            // Default: no output
            out_valid <= 1'b0;

            // Track which ports are currently active
            stat_port_active <= port_valid;

            if (grant_valid && out_ready) begin
                // Assign sequence number (strictly monotonic)
                seq_counter <= seq_counter + 1'b1;

                // Output the sequenced order
                out_valid         <= 1'b1;
                out_seq_no        <= seq_counter + 1'b1;
                out_timestamp     <= timestamp_counter;
                out_contestant_id <= port_contestant_id[granted_port];
                out_msg_type      <= port_msg_type[granted_port];
                out_data          <= port_data[granted_port];

                // Advance round-robin priority
                if (granted_port == NUM_PORTS[$clog2(NUM_PORTS)-1:0] - 1)
                    rr_priority <= '0;
                else
                    rr_priority <= granted_port + 1'b1;

                // Stats
                stat_total_sequenced <= stat_total_sequenced + 1'b1;
            end else if (grant_valid && !out_ready) begin
                // Consumer backpressure — drop
                stat_total_drops <= stat_total_drops + 1'b1;
            end
        end
    end

endmodule

`default_nettype wire
