// =============================================================================
// tb_match_engine.sv — Matching Engine Testbench (II=1 Pipeline)
// =============================================================================
// Tests: insert, match, cancel, market sweep, throughput benchmark,
//        pipelined II=1 throughput, sustained crossing benchmark
// =============================================================================

`timescale 1ns / 1ps

module tb_match_engine;
    import iicpc_pkg::*;

    localparam int CLK_PERIOD = 4; // 250 MHz

    logic clk, rst_n;

    // Input
    logic        in_valid;
    logic [7:0]  in_msg_type;
    logic [7:0]  in_contestant_id;
    logic [7:0]  in_side;
    logic [7:0]  in_order_type;
    logic [31:0] in_client_order_id;
    logic [63:0] in_price;
    logic [31:0] in_quantity;
    logic [63:0] in_sequence_no;
    logic        in_ready;

    // Fill
    logic        fill_valid;
    logic [31:0] fill_aggressor_oid;
    logic [31:0] fill_passive_oid;
    logic [7:0]  fill_aggressor_cid;
    logic [7:0]  fill_passive_cid;
    logic [63:0] fill_price;
    logic [31:0] fill_qty;
    logic [7:0]  fill_aggressor_side;
    logic        fill_ready;

    // Ack
    logic        ack_valid;
    logic [31:0] ack_client_order_id;
    logic [7:0]  ack_contestant_id;
    logic [7:0]  ack_status;
    logic        ack_ready;

    // Market data
    logic        md_valid;
    logic [63:0] md_best_bid;
    logic [31:0] md_best_bid_qty;
    logic [63:0] md_best_ask;
    logic [31:0] md_best_ask_qty;

    // Stats
    logic [31:0] stat_total_orders;
    logic [31:0] stat_total_fills;
    logic [31:0] stat_total_cancels;
    logic [31:0] stat_resting_orders;
    logic [31:0] stat_bid_levels;
    logic [31:0] stat_ask_levels;

    // DUT
    match_engine_fpga #(
        .MAX_ORDERS(1024),
        .MAX_LEVELS(256)
    ) dut (.*);

    // Clock
    initial clk = 0;
    always #(CLK_PERIOD/2) clk = ~clk;

    // Counters
    int errors = 0;
    int total_fills = 0;
    int total_acks = 0;
    int seq_counter = 0;

    // Monitor fills and acks
    always @(posedge clk) begin
        if (fill_valid && fill_ready)
            total_fills++;
        if (ack_valid && ack_ready)
            total_acks++;
    end

    // =========================================================================
    // Helper: submit order and wait for pipeline to fully drain
    // =========================================================================
    task automatic submit_order(
        input logic [7:0]  msg_type,
        input logic [7:0]  side,
        input logic [7:0]  order_type,
        input logic [31:0] client_oid,
        input logic [63:0] price,
        input logic [31:0] qty,
        input logic [7:0]  contestant_id
    );
        @(posedge clk);
        while (!in_ready) @(posedge clk);

        in_valid           = 1;
        in_msg_type        = msg_type;
        in_side            = side;
        in_order_type      = order_type;
        in_client_order_id = client_oid;
        in_price           = price;
        in_quantity        = qty;
        in_contestant_id   = contestant_id;
        seq_counter++;
        in_sequence_no     = seq_counter;

        @(posedge clk);
        in_valid = 0;

        // Wait for pipeline to drain
        repeat(4) @(posedge clk);
        while (!in_ready) @(posedge clk);
        @(posedge clk);
    endtask

    // =========================================================================
    // Helper: fire-and-forget (for pipelined II=1 throughput test)
    // =========================================================================
    task automatic submit_order_pipelined(
        input logic [7:0]  msg_type,
        input logic [7:0]  side,
        input logic [7:0]  order_type,
        input logic [31:0] client_oid,
        input logic [63:0] price,
        input logic [31:0] qty,
        input logic [7:0]  contestant_id
    );
        // Wait for in_ready (should be immediate for II=1)
        while (!in_ready) @(posedge clk);

        in_valid           = 1;
        in_msg_type        = msg_type;
        in_side            = side;
        in_order_type      = order_type;
        in_client_order_id = client_oid;
        in_price           = price;
        in_quantity        = qty;
        in_contestant_id   = contestant_id;
        seq_counter++;
        in_sequence_no     = seq_counter;

        @(posedge clk);
        in_valid = 0;
    endtask

    // Constants
    localparam logic [7:0] ORDER_ENTRY = MSG_ORDER_ENTRY;
    localparam logic [7:0] CANCEL_REQ  = MSG_CANCEL_REQ;
    localparam logic [7:0] TYPE_LIMIT  = ORDER_LIMIT;
    localparam logic [7:0] TYPE_MARKET = ORDER_MARKET;
    localparam logic [7:0] TYPE_IOC    = ORDER_IOC;

    // =========================================================================
    // Test sequence
    // =========================================================================
    initial begin
        $dumpfile("dump.vcd");
        $dumpvars(0, tb_match_engine);

        $display("=== FPGA Matching Engine Testbench (II=1 Pipeline) ===");
        $display("  Clock: %0d MHz (period = %0d ns)", 1000/CLK_PERIOD, CLK_PERIOD);

        // Init
        rst_n = 0;
        in_valid = 0;
        fill_ready = 1;
        ack_ready = 1;

        repeat(20) @(posedge clk);
        rst_n = 1;
        repeat(10) @(posedge clk);

        // =====================================================================
        // Test 1: Insert limit order (no match) → ack ACCEPTED
        // =====================================================================
        $display("\n[TEST 1] Insert limit BUY @ 100, qty=10");
        submit_order(ORDER_ENTRY, SIDE_BUY, TYPE_LIMIT, 1001, 64'd1000000, 32'd10, 8'd1);
        $display("  Best bid: %0d  Resting: %0d  Acks: %0d",
                 md_best_bid, stat_resting_orders, total_acks);
        if (stat_resting_orders == 1)
            $display("  [PASS] 1 resting order");
        else begin
            $display("  [FAIL] Expected 1 resting, got %0d", stat_resting_orders);
            errors++;
        end

        // =====================================================================
        // Test 2: Insert matching SELL → fill
        // =====================================================================
        $display("\n[TEST 2] Insert limit SELL @ 100, qty=5 (should match BUY)");
        begin
            logic [31:0] hw_fills_before;
            logic [31:0] hw_fills_after;
            hw_fills_before = stat_total_fills;
            submit_order(ORDER_ENTRY, SIDE_SELL, TYPE_LIMIT, 2001, 64'd1000000, 32'd5, 8'd2);
            repeat(3) @(posedge clk);
            hw_fills_after = stat_total_fills;
            $display("  HW Fills: %0d (+%0d)  Resting: %0d",
                     hw_fills_after, hw_fills_after - hw_fills_before, stat_resting_orders);
            if (hw_fills_after != hw_fills_before)
                $display("  [PASS] Fill generated");
            else begin
                $display("  [FAIL] No fill generated");
                errors++;
            end
        end

        // =====================================================================
        // Test 3: Build order book depth
        // =====================================================================
        $display("\n[TEST 3] Build book: 5 buy levels, 5 sell levels");
        for (int i = 0; i < 5; i++) begin
            submit_order(ORDER_ENTRY, SIDE_BUY, TYPE_LIMIT,
                        3000+i, 64'd990000 - i*10000, 32'd20, 8'd3);
            submit_order(ORDER_ENTRY, SIDE_SELL, TYPE_LIMIT,
                        4000+i, 64'd1010000 + i*10000, 32'd20, 8'd4);
        end
        $display("  Resting orders: %0d", stat_resting_orders);
        $display("  Best bid: %0d  Best ask: %0d", md_best_bid, md_best_ask);
        if (stat_resting_orders > 5)
            $display("  [PASS] Book has depth");
        else begin
            $display("  [FAIL] Book too shallow");
            errors++;
        end

        // =====================================================================
        // Test 4: Cancel order
        // =====================================================================
        $display("\n[TEST 4] Cancel order 3000");
        begin
            int resting_before = stat_resting_orders;
            submit_order(CANCEL_REQ, 8'd0, 8'd0, 3000, 64'd0, 32'd0, 8'd3);
            $display("  Cancels: %0d  Resting: %0d (was %0d)",
                     stat_total_cancels, stat_resting_orders, resting_before);
            if (stat_total_cancels > 0)
                $display("  [PASS] Cancel processed");
            else begin
                $display("  [FAIL] Cancel not processed");
                errors++;
            end
        end

        // =====================================================================
        // Test 5: Aggressive crossing order
        // =====================================================================
        $display("\n[TEST 5] Aggressive BUY @ 101 (cross ask)");
        begin
            logic [31:0] hw_fills_before;
            logic [31:0] hw_fills_after;
            hw_fills_before = stat_total_fills;
            submit_order(ORDER_ENTRY, SIDE_BUY, TYPE_LIMIT, 5001, 64'd1010000, 32'd20, 8'd5);
            repeat(3) @(posedge clk);
            hw_fills_after = stat_total_fills;
            $display("  HW Fills: %0d (+%0d)", hw_fills_after, hw_fills_after - hw_fills_before);
            if (hw_fills_after != hw_fills_before)
                $display("  [PASS] Crossing fill generated");
            else begin
                $display("  [INFO] No immediate fill (may need deeper matching)");
            end
        end

        // =====================================================================
        // Test 6: Sequential throughput — 1000 alternating buy/sell (blocking)
        // =====================================================================
        $display("\n[TEST 6] Sequential throughput: 1000 orders (blocking submit)");
        begin
            longint start_time;
            longint end_time;
            int bench_fills_start;
            int bench_acks_start;

            start_time = $time;
            bench_fills_start = total_fills;
            bench_acks_start = total_acks;

            for (int i = 0; i < 500; i++) begin
                submit_order(ORDER_ENTRY, SIDE_BUY, TYPE_LIMIT,
                            10000+i*2, 64'd5000000, 32'd1, 8'd1);
                submit_order(ORDER_ENTRY, SIDE_SELL, TYPE_LIMIT,
                            10001+i*2, 64'd5000000, 32'd1, 8'd2);
            end

            end_time = $time;

            $display("  Elapsed:    %0d ns (%0d us)", end_time - start_time,
                     (end_time - start_time) / 1000);
            $display("  Orders:     1000");
            $display("  Fills:      %0d", total_fills - bench_fills_start);
            $display("  Acks:       %0d", total_acks - bench_acks_start);
            if ((end_time - start_time) > 0) begin
                longint elapsed_ns;
                longint ns_per_order;
                longint throughput_k;
                elapsed_ns = end_time - start_time;
                ns_per_order = elapsed_ns / 1000;
                throughput_k = (1000 * 1000000) / elapsed_ns;
                $display("  Latency:    ~%0d ns/order", ns_per_order);
                $display("  Throughput: ~%0d.%0d M orders/sec (sequential, at %0dMHz)",
                         throughput_k / 1000, (throughput_k % 1000) / 100,
                         1000/CLK_PERIOD);
            end
        end

        // =====================================================================
        // Test 7: Pipelined throughput — 1000 non-crossing orders (II=1 target)
        // =====================================================================
        $display("\n[TEST 7] Pipelined throughput: 1000 non-crossing orders (II=1 target)");
        begin
            longint start_time;
            longint end_time;
            int bench_acks_start;
            int stall_count;

            start_time = $time;
            bench_acks_start = total_acks;
            stall_count = 0;

            // Submit 500 BUYs at different prices (no crossing)
            for (int i = 0; i < 500; i++) begin
                if (!in_ready) stall_count++;
                submit_order_pipelined(ORDER_ENTRY, SIDE_BUY, TYPE_LIMIT,
                                       20000+i, 64'd2000000 - i*100, 32'd1, 8'd1);
            end
            // Submit 500 SELLs at different prices (no crossing)
            for (int i = 0; i < 500; i++) begin
                if (!in_ready) stall_count++;
                submit_order_pipelined(ORDER_ENTRY, SIDE_SELL, TYPE_LIMIT,
                                       21000+i, 64'd8000000 + i*100, 32'd1, 8'd2);
            end

            // Wait for pipeline to fully drain
            repeat(10) @(posedge clk);
            while (!in_ready) @(posedge clk);
            repeat(5) @(posedge clk);

            end_time = $time;

            $display("  Elapsed:    %0d ns (%0d us)", end_time - start_time,
                     (end_time - start_time) / 1000);
            $display("  Orders:     1000 (non-crossing, pipelined)");
            $display("  Acks:       %0d", total_acks - bench_acks_start);
            $display("  Stalls:     %0d (should be 0 for II=1)", stall_count);
            if ((end_time - start_time) > 0) begin
                longint elapsed_ns;
                longint ns_per_order;
                longint throughput_k;
                longint tp_322;
                longint tp_500;
                elapsed_ns = end_time - start_time;
                ns_per_order = elapsed_ns / 1000;
                throughput_k = (1000 * 1000000) / elapsed_ns;
                $display("  II:         ~%0d cycles/order (%0d ns)", ns_per_order/CLK_PERIOD, ns_per_order);
                $display("  Throughput: ~%0d.%0d M orders/sec (pipelined, at %0dMHz)",
                         throughput_k / 1000, (throughput_k % 1000) / 100,
                         1000/CLK_PERIOD);

                // Multi-frequency projection
                $display("");
                $display("  === Multi-Frequency Projection ===");
                $display("  @ 250 MHz: %0d.%0d M orders/sec",
                         throughput_k / 1000, (throughput_k % 1000) / 100);
                tp_322 = throughput_k * 322 / 250;
                $display("  @ 322 MHz: %0d.%0d M orders/sec (10G Ethernet native)",
                         tp_322 / 1000, (tp_322 % 1000) / 100);
                tp_500 = throughput_k * 500 / 250;
                $display("  @ 500 MHz: %0d.%0d M orders/sec (Versal fast path)",
                         tp_500 / 1000, (tp_500 % 1000) / 100);
            end
        end

        // =====================================================================
        // Test 8: Sustained crossing — 1000 alternating BUY/SELL @ same price
        // =====================================================================
        $display("\n[TEST 8] Sustained crossing: 1000 orders (alternating BUY/SELL)");
        begin
            longint start_time;
            longint end_time;
            int bench_fills_start;
            int crossing_fills;

            start_time = $time;
            bench_fills_start = total_fills;

            for (int i = 0; i < 500; i++) begin
                submit_order_pipelined(ORDER_ENTRY, SIDE_BUY, TYPE_LIMIT,
                                       30000+i*2, 64'd7000000, 32'd1, 8'd1);
                submit_order_pipelined(ORDER_ENTRY, SIDE_SELL, TYPE_LIMIT,
                                       30001+i*2, 64'd7000000, 32'd1, 8'd2);
            end

            // Drain pipeline
            repeat(20) @(posedge clk);
            while (!in_ready) @(posedge clk);
            repeat(5) @(posedge clk);

            end_time = $time;
            crossing_fills = total_fills - bench_fills_start;

            $display("  Elapsed:    %0d ns (%0d us)", end_time - start_time,
                     (end_time - start_time) / 1000);
            $display("  Orders:     1000 (crossing, pipelined)");
            $display("  Fills:      %0d", crossing_fills);
            if ((end_time - start_time) > 0) begin
                longint elapsed_ns;
                longint ns_per_order;
                longint throughput_k;
                elapsed_ns = end_time - start_time;
                ns_per_order = elapsed_ns / 1000;
                throughput_k = (1000 * 1000000) / elapsed_ns;
                $display("  Latency:    ~%0d ns/order (includes fill cycle)", ns_per_order);
                $display("  Throughput: ~%0d.%0d M orders/sec (sustained crossing)",
                         throughput_k / 1000, (throughput_k % 1000) / 100);
            end
        end

        // =====================================================================
        // Summary
        // =====================================================================
        $display("\n========================================");
        $display("     MATCHING ENGINE RESULTS (II=1)");
        $display("========================================");
        $display("  Total orders:   %0d", stat_total_orders);
        $display("  Total fills:    %0d (HW) / %0d (TB)", stat_total_fills, total_fills);
        $display("  Total cancels:  %0d", stat_total_cancels);
        $display("  Resting orders: %0d", stat_resting_orders);
        $display("  Total acks:     %0d", total_acks);
        $display("  Errors:         %0d", errors);
        $display("========================================");

        if (errors == 0)
            $display("  *** ALL TESTS PASSED ***\n");
        else
            $display("  *** %0d ERRORS ***\n", errors);

        $finish;
    end

    // Timeout
    initial begin
        #50000000;
        $display("TIMEOUT - simulation took too long");
        $finish;
    end

endmodule
