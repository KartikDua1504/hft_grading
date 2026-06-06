// =============================================================================
// dma_ring.sv — PCIe DMA Ring Buffer (SystemVerilog)
// =============================================================================
// FPGA-side ring buffer for sequenced orders.
// FPGA writes sequenced orders → Host CPU reads via PCIe BAR MMIO.
//
// Architecture:
//   - Dual-port BRAM: FPGA writes, host reads
//   - Write pointer managed by FPGA (auto-increment)
//   - Read pointer managed by host via MMIO register
//   - Overflow detection: if write catches read → drop + flag
//
// Memory layout (visible to host via PCIe BAR):
//   0x0000: write_ptr   (RO from host, 32-bit)
//   0x0004: read_ptr    (RW from host, 32-bit)
//   0x0008: capacity    (RO, 32-bit)
//   0x000C: drops       (RO, 32-bit)
//   0x0010: status      (RO, 32-bit)
//   0x1000: ring_data[] (RO, sequenced order entries)
//
// Each ring entry: 128 bytes (seq_no + timestamp + contestant_id + order)
// =============================================================================

`timescale 1ns / 1ps
`default_nettype none

module dma_ring #(
    parameter int DEPTH       = 16384,      // Ring depth (power of 2)
    parameter int ENTRY_WIDTH = 1024,       // Bits per entry (128 bytes)
    parameter int ADDR_WIDTH  = $clog2(DEPTH)
)(
    input  wire                     clk,
    input  wire                     rst_n,

    // =========================================================================
    // Write port (from sequencer_core)
    // =========================================================================
    input  wire                     wr_valid,
    input  wire [ENTRY_WIDTH-1:0]   wr_data,
    output logic                    wr_ready,

    // =========================================================================
    // Host MMIO interface (simplified AXI-Lite slave)
    // =========================================================================
    input  wire                     host_rd_en,
    input  wire [15:0]              host_rd_addr,
    output logic [31:0]             host_rd_data,
    output logic                    host_rd_valid,

    input  wire                     host_wr_en,
    input  wire [15:0]              host_wr_addr,
    input  wire [31:0]              host_wr_data,

    // =========================================================================
    // Status
    // =========================================================================
    output logic [31:0]             stat_write_ptr,
    output logic [31:0]             stat_occupancy,
    output logic [31:0]             stat_drops,
    output logic                    stat_full,
    output logic                    stat_empty
);

    // =========================================================================
    // Ring pointers
    // =========================================================================
    logic [ADDR_WIDTH-1:0] write_ptr;
    logic [ADDR_WIDTH-1:0] read_ptr;     // Written by host via MMIO
    logic [31:0]           drop_count;

    // =========================================================================
    // Ring memory (BRAM)
    // =========================================================================
    // Inferred as Block RAM by synthesis
    (* ram_style = "block" *)
    logic [ENTRY_WIDTH-1:0] ring_mem [0:DEPTH-1];

    // =========================================================================
    // Occupancy calculation
    // =========================================================================
    wire [ADDR_WIDTH:0] occupancy = write_ptr - read_ptr;
    wire                is_full   = (occupancy >= DEPTH - 1);
    wire                is_empty  = (write_ptr == read_ptr);

    assign wr_ready       = !is_full;
    assign stat_write_ptr = {{(32-ADDR_WIDTH){1'b0}}, write_ptr};
    assign stat_occupancy = occupancy[31:0];
    assign stat_drops     = drop_count;
    assign stat_full      = is_full;
    assign stat_empty     = is_empty;

    // =========================================================================
    // Write logic (FPGA side)
    // =========================================================================
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            write_ptr  <= '0;
            drop_count <= '0;
        end else begin
            if (wr_valid) begin
                if (!is_full) begin
                    ring_mem[write_ptr] <= wr_data;
                    write_ptr <= write_ptr + 1'b1;
                end else begin
                    drop_count <= drop_count + 1'b1;
                end
            end
        end
    end

    // =========================================================================
    // Host read logic (MMIO BAR)
    // =========================================================================
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            host_rd_data  <= '0;
            host_rd_valid <= 1'b0;
            read_ptr      <= '0;
        end else begin
            host_rd_valid <= 1'b0;

            // Host writes to read_ptr register
            if (host_wr_en && host_wr_addr == 16'h0004) begin
                read_ptr <= host_wr_data[ADDR_WIDTH-1:0];
            end

            // Host reads
            if (host_rd_en) begin
                host_rd_valid <= 1'b1;
                case (host_wr_addr)
                    16'h0000: host_rd_data <= {{(32-ADDR_WIDTH){1'b0}}, write_ptr};
                    16'h0004: host_rd_data <= {{(32-ADDR_WIDTH){1'b0}}, read_ptr};
                    16'h0008: host_rd_data <= DEPTH;
                    16'h000C: host_rd_data <= drop_count;
                    16'h0010: host_rd_data <= {30'b0, is_full, is_empty};
                    default:  host_rd_data <= '0;
                endcase
            end
        end
    end

endmodule

`default_nettype wire
