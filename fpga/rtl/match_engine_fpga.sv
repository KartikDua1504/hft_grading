// =============================================================================
// match_engine_fpga.sv — FPGA Matching Engine (True II=1 Pipeline)
// =============================================================================
// Hardware-accelerated price-time priority limit order book.
//
// *** ARCHITECTURE: 2-Stage Pipeline with BBO Forwarding ***
//
//   Stage 1 (DECODE):
//     - Latch + register input fields
//     - Compute crossing condition using *forwarded* BBO from Stage 2
//     - Hash client_order_id for cancel lookup
//     - Runs concurrently with Stage 2 on different orders
//
//   Stage 2 (EXECUTE + EMIT):
//     - If crossing: execute fill at best opposite price, emit fill
//     - If not crossing: insert as resting order, emit ack  (1 cycle)
//     - If cancel: lookup + remove from cancel map, emit ack (1 cycle)
//     - Deep sweep: loops in S2 for multi-level crossing    (N cycles)
//     - BBO forwarding: S2 updates are visible to S1 combinationally
//
// Performance (250 MHz = 4ns period):
//   - Non-crossing insert: II=1 (4ns)  — 250M orders/sec
//   - Cancel:              II=1 (4ns)  — 250M orders/sec
//   - Single-level cross:  II=2 (8ns)  — 125M orders/sec
//   - Deep match (N lvls): II=1+N      — stalls only during sweep
//
// Key insight: `in_ready` stays high while S2 executes 1-cycle operations
// (inserts, cancels), allowing back-to-back order acceptance. Only crossing
// orders (which need multi-cycle sweep) cause pipeline stalls.
//
// BBO Forwarding: S2's combinational BBO update is fed back to S1's crossing
// detector via a mux, eliminating the 1-cycle RAW hazard that would otherwise
// cause incorrect crossing decisions on consecutive orders.
//
// Target: AMD Xilinx Virtex UltraScale+ (AWS F2)
// =============================================================================

`timescale 1ns / 1ps
`default_nettype none

module match_engine_fpga
    import iicpc_pkg::*;
#(
    parameter int MAX_ORDERS      = 16384,
    parameter int MAX_LEVELS      = 4096,
    parameter int ORDER_ADDR_W    = $clog2(MAX_ORDERS),
    parameter int LEVEL_ADDR_W    = $clog2(MAX_LEVELS),
    parameter int PRICE_WIDTH     = 64,
    parameter int QTY_WIDTH       = 32,
    parameter int ORDER_ID_WIDTH  = 32,
    parameter int CONTESTANT_W    = 8
)(
    input  wire                         clk,
    input  wire                         rst_n,

    // Sequenced order input (from sequencer_core)
    input  wire                         in_valid,
    input  wire [7:0]                   in_msg_type,
    input  wire [CONTESTANT_W-1:0]      in_contestant_id,
    input  wire [7:0]                   in_side,
    input  wire [7:0]                   in_order_type,
    input  wire [ORDER_ID_WIDTH-1:0]    in_client_order_id,
    input  wire [PRICE_WIDTH-1:0]       in_price,
    input  wire [QTY_WIDTH-1:0]         in_quantity,
    input  wire [63:0]                  in_sequence_no,
    output logic                        in_ready,

    // Fill output
    output logic                        fill_valid,
    output logic [ORDER_ID_WIDTH-1:0]   fill_aggressor_oid,
    output logic [ORDER_ID_WIDTH-1:0]   fill_passive_oid,
    output logic [CONTESTANT_W-1:0]     fill_aggressor_cid,
    output logic [CONTESTANT_W-1:0]     fill_passive_cid,
    output logic [PRICE_WIDTH-1:0]      fill_price,
    output logic [QTY_WIDTH-1:0]        fill_qty,
    output logic [7:0]                  fill_aggressor_side,
    input  wire                         fill_ready,

    // Ack output
    output logic                        ack_valid,
    output logic [ORDER_ID_WIDTH-1:0]   ack_client_order_id,
    output logic [CONTESTANT_W-1:0]     ack_contestant_id,
    output logic [7:0]                  ack_status,
    input  wire                         ack_ready,

    // Market data output (best bid/ask)
    output logic                        md_valid,
    output logic [PRICE_WIDTH-1:0]      md_best_bid,
    output logic [QTY_WIDTH-1:0]        md_best_bid_qty,
    output logic [PRICE_WIDTH-1:0]      md_best_ask,
    output logic [QTY_WIDTH-1:0]        md_best_ask_qty,

    // Statistics
    output logic [31:0]                 stat_total_orders,
    output logic [31:0]                 stat_total_fills,
    output logic [31:0]                 stat_total_cancels,
    output logic [31:0]                 stat_resting_orders,
    output logic [31:0]                 stat_bid_levels,
    output logic [31:0]                 stat_ask_levels
);

    // =========================================================================
    // Order pool (BRAM)
    // =========================================================================
    localparam int ORDER_ENTRY_W = 192;

    (* ram_style = "block" *)
    logic [ORDER_ENTRY_W-1:0] order_pool [0:MAX_ORDERS-1];

    // Free list (simple stack)
    logic [ORDER_ADDR_W-1:0] free_stack [0:MAX_ORDERS-1];
    logic [ORDER_ADDR_W-1:0] free_top;

    // =========================================================================
    // Register-mapped BBO — O(1) access, flip-flop storage
    // =========================================================================
    logic [PRICE_WIDTH-1:0]    best_bid_price;
    logic [QTY_WIDTH-1:0]      best_bid_qty;
    logic [ORDER_ADDR_W-1:0]   best_bid_head;
    logic                       bid_valid_flag;

    logic [PRICE_WIDTH-1:0]    best_ask_price;
    logic [QTY_WIDTH-1:0]      best_ask_qty;
    logic [ORDER_ADDR_W-1:0]   best_ask_head;
    logic                       ask_valid_flag;

    // =========================================================================
    // Cancel lookup map — 2-way set-associative (prevents hash collisions)
    // Each set has 2 ways. Each way stores: {valid, order_id_tag, pool_index}
    // =========================================================================
    localparam int CANCEL_MAP_SETS = 4096;
    localparam int CANCEL_MAP_W = $clog2(CANCEL_MAP_SETS);

    // Way 0
    (* ram_style = "block" *)
    logic [ORDER_ADDR_W-1:0]   cancel_map_val0  [0:CANCEL_MAP_SETS-1];
    (* ram_style = "block" *)
    logic [ORDER_ID_WIDTH-1:0] cancel_map_tag0  [0:CANCEL_MAP_SETS-1];
    (* ram_style = "block" *)
    logic                      cancel_map_v0    [0:CANCEL_MAP_SETS-1];

    // Way 1
    (* ram_style = "block" *)
    logic [ORDER_ADDR_W-1:0]   cancel_map_val1  [0:CANCEL_MAP_SETS-1];
    (* ram_style = "block" *)
    logic [ORDER_ID_WIDTH-1:0] cancel_map_tag1  [0:CANCEL_MAP_SETS-1];
    (* ram_style = "block" *)
    logic                      cancel_map_v1    [0:CANCEL_MAP_SETS-1];

    // =========================================================================
    // Pipeline Stage 2 state
    // =========================================================================
    typedef enum logic [1:0] {
        S2_IDLE,        // No work in S2 — pipeline empty
        S2_EXECUTE,     // Executing 1-cycle op (insert/cancel) — can accept next
        S2_CROSSING,    // Multi-cycle crossing sweep — stalls input
        S2_FILL_OUT     // Emitting fill, then check for more sweep
    } s2_state_t;

    s2_state_t s2_state;

    // =========================================================================
    // Stage 2 pipeline registers (latched from input)
    // =========================================================================
    logic                      s2_valid;
    logic [7:0]                s2_msg_type;
    logic [CONTESTANT_W-1:0]   s2_contestant_id;
    logic [7:0]                s2_side;
    logic [7:0]                s2_order_type;
    logic [ORDER_ID_WIDTH-1:0] s2_client_oid;
    logic [PRICE_WIDTH-1:0]    s2_price;
    logic [QTY_WIDTH-1:0]      s2_remaining_qty;
    logic [63:0]               s2_seq_no;
    logic                      s2_is_crossing;   // Latched crossing decision from S1

    // Sweep state
    logic [PRICE_WIDTH-1:0]    s2_match_price;
    logic [QTY_WIDTH-1:0]      s2_match_qty;
    logic [ORDER_ADDR_W-1:0]   s2_match_cursor;
    logic [4:0]                s2_sweep_depth;

    // =========================================================================
    // Fibonacci hash for cancel map
    // =========================================================================
    function automatic logic [CANCEL_MAP_W-1:0] cancel_hash(
        input logic [ORDER_ID_WIDTH-1:0] oid
    );
        return (oid * 32'h9E3779B9) >> (32 - CANCEL_MAP_W);
    endfunction

    // =========================================================================
    // Combinational: BBO forwarding from S2 → S1
    // =========================================================================
    // When S2 is executing an insert THIS cycle, the BBO update hasn't been
    // committed yet (non-blocking <=). We forward the *intended* update
    // combinationally so S1's crossing detection sees the correct BBO.

    // --- S2 insert detection (combinational, based on s2 pipeline regs) ---
    wire s2_doing_insert = s2_valid && (s2_state == S2_EXECUTE) &&
                           (s2_msg_type == MSG_ORDER_ENTRY) && !s2_is_crossing &&
                           (s2_order_type == ORDER_LIMIT) && (s2_remaining_qty > 0);

    wire s2_insert_improves_bid = s2_doing_insert && (s2_side == SIDE_BUY) &&
                                  (!bid_valid_flag || s2_price > best_bid_price);
    wire s2_insert_at_bid      = s2_doing_insert && (s2_side == SIDE_BUY) &&
                                  bid_valid_flag && (s2_price == best_bid_price);

    wire s2_insert_improves_ask = s2_doing_insert && (s2_side == SIDE_SELL) &&
                                  (!ask_valid_flag || s2_price < best_ask_price);
    wire s2_insert_at_ask      = s2_doing_insert && (s2_side == SIDE_SELL) &&
                                  ask_valid_flag && (s2_price == best_ask_price);

    // --- S2 fill detection (BBO consumed during crossing) ---
    wire s2_doing_fill = s2_valid && (s2_state == S2_CROSSING);

    wire s2_fill_exhausts_ask = s2_doing_fill && (s2_side == SIDE_BUY) &&
                                 (best_ask_qty <= s2_match_qty);
    wire s2_fill_exhausts_bid = s2_doing_fill && (s2_side == SIDE_SELL) &&
                                 (best_bid_qty <= s2_match_qty);

    // --- Forwarded BBO values ---
    wire [PRICE_WIDTH-1:0] fwd_best_bid_price =
        s2_insert_improves_bid ? s2_price : best_bid_price;

    wire [QTY_WIDTH-1:0] fwd_best_bid_qty =
        s2_insert_improves_bid ? s2_remaining_qty :
        s2_insert_at_bid       ? (best_bid_qty + s2_remaining_qty) :
        s2_fill_exhausts_bid   ? '0 :
        (s2_doing_fill && s2_side == SIDE_SELL) ? (best_bid_qty - s2_match_qty) :
        best_bid_qty;

    wire fwd_bid_valid =
        s2_insert_improves_bid ? 1'b1 :
        s2_fill_exhausts_bid   ? 1'b0 :
        bid_valid_flag;

    wire [PRICE_WIDTH-1:0] fwd_best_ask_price =
        s2_insert_improves_ask ? s2_price : best_ask_price;

    wire [QTY_WIDTH-1:0] fwd_best_ask_qty =
        s2_insert_improves_ask ? s2_remaining_qty :
        s2_insert_at_ask       ? (best_ask_qty + s2_remaining_qty) :
        s2_fill_exhausts_ask   ? '0 :
        (s2_doing_fill && s2_side == SIDE_BUY) ? (best_ask_qty - s2_match_qty) :
        best_ask_qty;

    wire fwd_ask_valid =
        s2_insert_improves_ask ? 1'b1 :
        s2_fill_exhausts_ask   ? 1'b0 :
        ask_valid_flag;

    // =========================================================================
    // Combinational: S1 crossing detection (uses forwarded BBO)
    // =========================================================================
    wire s1_can_match_bid = (in_side == SIDE_BUY)  && fwd_ask_valid &&
                            (in_price >= fwd_best_ask_price);
    wire s1_can_match_ask = (in_side == SIDE_SELL) && fwd_bid_valid &&
                            (in_price <= fwd_best_bid_price || in_order_type == ORDER_MARKET);
    wire s1_is_crossing = in_valid && (in_msg_type == MSG_ORDER_ENTRY) &&
                          (s1_can_match_bid || s1_can_match_ask);

    // =========================================================================
    // Pipeline control: when can S1 accept a new order?
    // =========================================================================
    // Accept when:
    //  - S2 is idle (pipeline empty), OR
    //  - S2 is executing a 1-cycle op (insert/cancel) and will complete this cycle
    // Do NOT accept when:
    //  - S2 is in multi-cycle crossing/sweep
    //  - Output backpressure (fill/ack not ready)
    wire output_stall = (fill_valid && !fill_ready) || (ack_valid && !ack_ready);

    wire s2_completes_this_cycle = (s2_state == S2_EXECUTE) && !output_stall;

    assign in_ready = !output_stall &&
                      (s2_state == S2_IDLE || s2_completes_this_cycle);

    // The actual accept signal
    wire accept = in_valid && in_ready;

    // =========================================================================
    // Market data output — uses forwarded BBO for lowest latency
    // =========================================================================
    assign md_valid     = 1'b1;
    assign md_best_bid     = fwd_bid_valid ? fwd_best_bid_price : '0;
    assign md_best_bid_qty = fwd_bid_valid ? fwd_best_bid_qty   : '0;
    assign md_best_ask     = fwd_ask_valid ? fwd_best_ask_price : {PRICE_WIDTH{1'b1}};
    assign md_best_ask_qty = fwd_ask_valid ? fwd_best_ask_qty   : '0;

    // =========================================================================
    // Main pipeline logic
    // =========================================================================
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s2_state          <= S2_IDLE;
            s2_valid          <= 1'b0;
            fill_valid        <= 1'b0;
            ack_valid         <= 1'b0;
            stat_total_orders <= '0;
            stat_total_fills  <= '0;
            stat_total_cancels <= '0;
            stat_resting_orders <= '0;
            stat_bid_levels   <= '0;
            stat_ask_levels   <= '0;
            best_bid_price    <= '0;
            best_bid_qty      <= '0;
            best_ask_price    <= {PRICE_WIDTH{1'b1}};
            best_ask_qty      <= '0;
            bid_valid_flag    <= 1'b0;
            ask_valid_flag    <= 1'b0;
            free_top          <= '0;
            s2_sweep_depth    <= '0;
            s2_is_crossing    <= 1'b0;

            for (int i = 0; i < MAX_ORDERS; i++)
                free_stack[i] <= i[ORDER_ADDR_W-1:0];
            for (int i = 0; i < CANCEL_MAP_SETS; i++) begin
                cancel_map_v0[i] <= 1'b0;
                cancel_map_v1[i] <= 1'b0;
            end

        end else begin
            // Default: deassert outputs when downstream accepts
            if (fill_ready) fill_valid <= 1'b0;
            if (ack_ready)  ack_valid  <= 1'b0;

            case (s2_state)

                // =============================================================
                // S2_IDLE: Pipeline empty — just latch input if available
                // =============================================================
                S2_IDLE: begin
                    if (accept) begin
                        // Latch input into S2 pipeline registers
                        s2_valid         <= 1'b1;
                        s2_msg_type      <= in_msg_type;
                        s2_contestant_id <= in_contestant_id;
                        s2_side          <= in_side;
                        s2_order_type    <= in_order_type;
                        s2_client_oid    <= in_client_order_id;
                        s2_price         <= in_price;
                        s2_remaining_qty <= in_quantity;
                        s2_seq_no        <= in_sequence_no;
                        s2_is_crossing   <= s1_is_crossing;
                        s2_sweep_depth   <= '0;

                        if (in_msg_type == MSG_ORDER_ENTRY)
                            stat_total_orders <= stat_total_orders + 1;
                        else if (in_msg_type == MSG_CANCEL_REQ)
                            stat_total_cancels <= stat_total_cancels + 1;

                        // If crossing, go to multi-cycle crossing state
                        // Otherwise, go to 1-cycle execute (can accept next cycle)
                        if (s1_is_crossing)
                            s2_state <= S2_CROSSING;
                        else
                            s2_state <= S2_EXECUTE;
                    end
                end

                // =============================================================
                // S2_EXECUTE: 1-cycle insert/cancel — can accept next input
                // =============================================================
                S2_EXECUTE: begin
                    if (s2_msg_type == MSG_CANCEL_REQ) begin
                        // ── Cancel path: 2-way set-associative lookup ──
                        // Check both ways for tag match
                        if (cancel_map_v0[cancel_hash(s2_client_oid)] &&
                            cancel_map_tag0[cancel_hash(s2_client_oid)] == s2_client_oid) begin
                            cancel_map_v0[cancel_hash(s2_client_oid)] <= 1'b0;
                            stat_resting_orders <= stat_resting_orders - 1;
                            ack_valid           <= 1'b1;
                            ack_client_order_id <= s2_client_oid;
                            ack_contestant_id   <= s2_contestant_id;
                            ack_status          <= ACK_CANCELLED;
                        end else if (cancel_map_v1[cancel_hash(s2_client_oid)] &&
                                    cancel_map_tag1[cancel_hash(s2_client_oid)] == s2_client_oid) begin
                            cancel_map_v1[cancel_hash(s2_client_oid)] <= 1'b0;
                            stat_resting_orders <= stat_resting_orders - 1;
                            ack_valid           <= 1'b1;
                            ack_client_order_id <= s2_client_oid;
                            ack_contestant_id   <= s2_contestant_id;
                            ack_status          <= ACK_CANCELLED;
                        end else begin
                            ack_valid           <= 1'b1;
                            ack_client_order_id <= s2_client_oid;
                            ack_contestant_id   <= s2_contestant_id;
                            ack_status          <= ACK_REJECTED_UNKNOWN;
                        end

                    end else begin
                        // ── Insert path (non-crossing limit order) ──
                        if (s2_order_type == ORDER_LIMIT && s2_remaining_qty > 0) begin
                            if ({1'b0, free_top} < MAX_ORDERS) begin
                                order_pool[free_stack[free_top]] <= {
                                    s2_price,
                                    s2_remaining_qty,
                                    {(192-PRICE_WIDTH-QTY_WIDTH){1'b0}}
                                };
                                // Register in cancel map (2-way: pick first empty, else evict way 0)
                                if (!cancel_map_v0[cancel_hash(s2_client_oid)]) begin
                                    cancel_map_val0[cancel_hash(s2_client_oid)] <= free_stack[free_top];
                                    cancel_map_tag0[cancel_hash(s2_client_oid)] <= s2_client_oid;
                                    cancel_map_v0[cancel_hash(s2_client_oid)]   <= 1'b1;
                                end else if (!cancel_map_v1[cancel_hash(s2_client_oid)]) begin
                                    cancel_map_val1[cancel_hash(s2_client_oid)] <= free_stack[free_top];
                                    cancel_map_tag1[cancel_hash(s2_client_oid)] <= s2_client_oid;
                                    cancel_map_v1[cancel_hash(s2_client_oid)]   <= 1'b1;
                                end else begin
                                    // Both ways full — evict way 0 (LRU approximation)
                                    cancel_map_val0[cancel_hash(s2_client_oid)] <= free_stack[free_top];
                                    cancel_map_tag0[cancel_hash(s2_client_oid)] <= s2_client_oid;
                                end

                                // Update BBO
                                if (s2_side == SIDE_BUY) begin
                                    if (!bid_valid_flag || s2_price > best_bid_price) begin
                                        best_bid_price <= s2_price;
                                        best_bid_qty   <= s2_remaining_qty;
                                        best_bid_head  <= free_stack[free_top];
                                        bid_valid_flag <= 1'b1;
                                    end else if (s2_price == best_bid_price) begin
                                        best_bid_qty <= best_bid_qty + s2_remaining_qty;
                                    end
                                end else begin
                                    if (!ask_valid_flag || s2_price < best_ask_price) begin
                                        best_ask_price <= s2_price;
                                        best_ask_qty   <= s2_remaining_qty;
                                        best_ask_head  <= free_stack[free_top];
                                        ask_valid_flag <= 1'b1;
                                    end else if (s2_price == best_ask_price) begin
                                        best_ask_qty <= best_ask_qty + s2_remaining_qty;
                                    end
                                end

                                free_top            <= free_top + 1;
                                stat_resting_orders <= stat_resting_orders + 1;
                            end
                        end

                        // Emit ack for the insert
                        ack_valid           <= 1'b1;
                        ack_client_order_id <= s2_client_oid;
                        ack_contestant_id   <= s2_contestant_id;
                        ack_status          <= ACK_ACCEPTED;
                    end

                    // ── Simultaneously accept next order (II=1) ──
                    if (accept) begin
                        s2_valid         <= 1'b1;
                        s2_msg_type      <= in_msg_type;
                        s2_contestant_id <= in_contestant_id;
                        s2_side          <= in_side;
                        s2_order_type    <= in_order_type;
                        s2_client_oid    <= in_client_order_id;
                        s2_price         <= in_price;
                        s2_remaining_qty <= in_quantity;
                        s2_seq_no        <= in_sequence_no;
                        s2_is_crossing   <= s1_is_crossing;
                        s2_sweep_depth   <= '0;

                        if (in_msg_type == MSG_ORDER_ENTRY)
                            stat_total_orders <= stat_total_orders + 1;
                        else if (in_msg_type == MSG_CANCEL_REQ)
                            stat_total_cancels <= stat_total_cancels + 1;

                        if (s1_is_crossing)
                            s2_state <= S2_CROSSING;
                        else
                            s2_state <= S2_EXECUTE;  // Stay in execute for next
                    end else begin
                        s2_valid <= 1'b0;
                        s2_state <= S2_IDLE;
                    end
                end

                // =============================================================
                // S2_CROSSING: Multi-cycle crossing — compute fill, go to emit
                // =============================================================
                S2_CROSSING: begin
                    if (s2_remaining_qty > 0 && s2_sweep_depth < MAX_SWEEP_DEPTH) begin
                        // Compute fill quantity
                        if (s2_side == SIDE_BUY) begin
                            if (fwd_ask_valid && s2_price >= fwd_best_ask_price) begin
                                s2_match_price  <= best_ask_price;
                                s2_match_cursor <= best_ask_head;
                                s2_match_qty    <= (s2_remaining_qty < best_ask_qty)
                                                   ? s2_remaining_qty : best_ask_qty;
                                s2_state <= S2_FILL_OUT;
                            end else begin
                                // No more crossing — insert remainder
                                s2_is_crossing <= 1'b0;
                                s2_state <= S2_EXECUTE;
                            end
                        end else begin
                            if (fwd_bid_valid && (s2_price <= fwd_best_bid_price || s2_order_type == ORDER_MARKET)) begin
                                s2_match_price  <= best_bid_price;
                                s2_match_cursor <= best_bid_head;
                                s2_match_qty    <= (s2_remaining_qty < best_bid_qty)
                                                   ? s2_remaining_qty : best_bid_qty;
                                s2_state <= S2_FILL_OUT;
                            end else begin
                                s2_is_crossing <= 1'b0;
                                s2_state <= S2_EXECUTE;
                            end
                        end
                    end else begin
                        // Sweep exhausted — insert remainder or just ack
                        s2_is_crossing <= 1'b0;
                        s2_state <= S2_EXECUTE;
                    end
                end

                // =============================================================
                // S2_FILL_OUT: Emit fill, update BBO, loop for more sweep
                // =============================================================
                S2_FILL_OUT: begin
                    // Drive fill outputs
                    fill_valid          <= 1'b1;
                    fill_aggressor_oid  <= s2_client_oid;
                    fill_aggressor_cid  <= s2_contestant_id;
                    fill_aggressor_side <= s2_side;
                    fill_price          <= s2_match_price;
                    fill_qty            <= s2_match_qty;
                    fill_passive_oid    <= s2_match_cursor;
                    fill_passive_cid    <= '0;

                    // Update remaining quantity
                    s2_remaining_qty <= s2_remaining_qty - s2_match_qty;

                    // Update BBO after fill
                    if (s2_side == SIDE_BUY) begin
                        best_ask_qty <= best_ask_qty - s2_match_qty;
                        if (best_ask_qty <= s2_match_qty)
                            ask_valid_flag <= 1'b0;
                    end else begin
                        best_bid_qty <= best_bid_qty - s2_match_qty;
                        if (best_bid_qty <= s2_match_qty)
                            bid_valid_flag <= 1'b0;
                    end

                    stat_total_fills    <= stat_total_fills + 1;
                    stat_resting_orders <= stat_resting_orders - 1;
                    s2_sweep_depth      <= s2_sweep_depth + 1;

                    // Check if more to sweep
                    if ((s2_remaining_qty - s2_match_qty) > 0)
                        s2_state <= S2_CROSSING;  // Loop for next level
                    else begin
                        // Done crossing — emit ack via S2_EXECUTE
                        s2_is_crossing <= 1'b0;
                        s2_state <= S2_EXECUTE;
                    end
                end

                default: s2_state <= S2_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
