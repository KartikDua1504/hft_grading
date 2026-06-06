// =============================================================================
// order_parser.sv — Binary Protocol Parser (SystemVerilog)
// =============================================================================
// Extracts OrderEntry/CancelRequest fields from raw network payload.
// Matches the C++ protocol.hpp wire format exactly.
//
// Input: raw byte stream from network (UDP payload or MMIO write)
// Output: parsed fields ready for sequencer_core
//
// OrderEntry layout (64 bytes):
//   [0]    msg_type     (uint8)
//   [1]    side         (uint8)
//   [2]    order_type   (uint8)
//   [3]    instrument_id(uint8)
//   [4:7]  client_order_id (uint32, LE)
//   [8:15] price        (int64, LE)
//   [16:19] quantity    (int32, LE)
//   [20:63] padding
//
// CancelRequest layout (32 bytes):
//   [0]    msg_type     (uint8)
//   [1:3]  padding
//   [4:7]  client_order_id (uint32, LE)
//   [8:15] exchange_order_id (uint64, LE)
//   [16:31] padding
// =============================================================================

`timescale 1ns / 1ps
`default_nettype none

module order_parser #(
    parameter int DATA_WIDTH = 512   // 64 bytes = 512 bits
)(
    input  wire                     clk,
    input  wire                     rst_n,

    // Raw input
    input  wire                     in_valid,
    input  wire [DATA_WIDTH-1:0]    in_data,
    output logic                    in_ready,

    // Parsed output
    output logic                    out_valid,
    output logic [7:0]              out_msg_type,
    output logic [7:0]              out_side,
    output logic [7:0]              out_order_type,
    output logic [7:0]              out_instrument_id,
    output logic [31:0]             out_client_order_id,
    output logic [63:0]             out_price,
    output logic [31:0]             out_quantity,
    output logic [63:0]             out_exchange_order_id,
    output logic                    out_is_cancel,
    output logic [DATA_WIDTH-1:0]   out_raw,        // Pass through raw data
    input  wire                     out_ready,

    // Error
    output logic                    err_invalid_msg
);

    // Message type constants (match protocol.hpp)
    localparam logic [7:0] MSG_ORDER_ENTRY    = 8'd10;
    localparam logic [7:0] MSG_CANCEL_REQUEST = 8'd11;

    // Extract fields from little-endian packed data
    // Byte 0 is bits [7:0], byte 1 is bits [15:8], etc.
    wire [7:0]  raw_msg_type         = in_data[7:0];
    wire [7:0]  raw_side             = in_data[15:8];
    wire [7:0]  raw_order_type       = in_data[23:16];
    wire [7:0]  raw_instrument_id    = in_data[31:24];
    wire [31:0] raw_client_order_id  = in_data[63:32];
    wire [63:0] raw_price            = in_data[127:64];
    wire [31:0] raw_quantity         = in_data[159:128];
    wire [63:0] raw_exchange_oid     = in_data[127:64]; // Cancel uses same offset

    // Validate message type
    wire is_order  = (raw_msg_type == MSG_ORDER_ENTRY);
    wire is_cancel = (raw_msg_type == MSG_CANCEL_REQUEST);
    wire msg_valid = is_order || is_cancel;

    // Backpressure
    assign in_ready = out_ready || !out_valid;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out_valid            <= 1'b0;
            out_msg_type         <= '0;
            out_side             <= '0;
            out_order_type       <= '0;
            out_instrument_id    <= '0;
            out_client_order_id  <= '0;
            out_price            <= '0;
            out_quantity         <= '0;
            out_exchange_order_id <= '0;
            out_is_cancel        <= 1'b0;
            out_raw              <= '0;
            err_invalid_msg      <= 1'b0;
        end else begin
            err_invalid_msg <= 1'b0;

            if (in_valid && in_ready) begin
                if (msg_valid) begin
                    out_valid            <= 1'b1;
                    out_msg_type         <= raw_msg_type;
                    out_side             <= raw_side;
                    out_order_type       <= raw_order_type;
                    out_instrument_id    <= raw_instrument_id;
                    out_client_order_id  <= raw_client_order_id;
                    out_is_cancel        <= is_cancel;
                    out_raw              <= in_data;

                    if (is_order) begin
                        out_price            <= raw_price;
                        out_quantity         <= raw_quantity;
                        out_exchange_order_id <= '0;
                    end else begin
                        // Cancel
                        out_price            <= '0;
                        out_quantity         <= '0;
                        out_exchange_order_id <= raw_exchange_oid;
                    end
                end else begin
                    out_valid       <= 1'b0;
                    err_invalid_msg <= 1'b1;
                end
            end else if (out_ready) begin
                out_valid <= 1'b0;
            end
        end
    end

endmodule

`default_nettype wire
