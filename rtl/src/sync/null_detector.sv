`default_nettype none

module null_detector #(
    parameter int SAMPLE_WIDTH = 16,
    parameter int POWER_WIDTH = 34,
    parameter int RATIO_WIDTH = 24,
    parameter int RATIO_FRAC_BITS = 22,
    parameter int FAST_WINDOW_SAMPLES = 39,
    parameter int SLOW_WINDOW_SAMPLES = 1999,
    parameter int ENTER_THRESHOLD_NUM = 1,
    parameter int ENTER_THRESHOLD_DEN = 4,
    parameter int EXIT_THRESHOLD_NUM = 9,
    parameter int EXIT_THRESHOLD_DEN = 20,
    parameter int MIN_NULL_SAMPLES = 128,
    parameter int MAX_NULL_SAMPLES = 0,
    parameter int STARTUP_IGNORE_SAMPLES = 0,
    parameter int EXPECTED_INTERVAL_SAMPLES = 0,
    parameter int INTERVAL_TOLERANCE_SAMPLES = 0,
    parameter int MAX_MERGE_GAP_SAMPLES = 0
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic in_valid,
    output logic in_ready,
    input  wire logic signed [SAMPLE_WIDTH-1:0] in_i,
    input  wire logic signed [SAMPLE_WIDTH-1:0] in_q,
    input  wire logic [63:0] in_sample_index,

    output logic out_valid,
    input  wire logic out_ready,
    output logic [63:0] out_sample_index,
    output logic [31:0] out_length,
    output logic [POWER_WIDTH-1:0] out_avg_power,
    output logic [RATIO_WIDTH-1:0] out_min_ratio
);
  localparam int FAST_POS_WIDTH = FAST_WINDOW_SAMPLES <= 1 ? 1 : $clog2(FAST_WINDOW_SAMPLES);
  localparam int SLOW_POS_WIDTH = SLOW_WINDOW_SAMPLES <= 1 ? 1 : $clog2(SLOW_WINDOW_SAMPLES);
  localparam int FAST_COUNT_WIDTH = $clog2(FAST_WINDOW_SAMPLES + 1);
  localparam int SLOW_COUNT_WIDTH = $clog2(SLOW_WINDOW_SAMPLES + 1);
  localparam int POWER_SUM_WIDTH = POWER_WIDTH + $clog2(SLOW_WINDOW_SAMPLES + 1) + 2;

  localparam logic [RATIO_WIDTH-1:0] RATIO_ONE =
      RATIO_WIDTH'(1) << RATIO_FRAC_BITS;

  (* ram_style = "distributed", ramstyle = "MLAB" *)
  logic [POWER_WIDTH-1:0] fast_history [0:FAST_WINDOW_SAMPLES-1];
  (* ram_style = "block", ramstyle = "M10K" *)
  logic [POWER_WIDTH-1:0] slow_history [0:SLOW_WINDOW_SAMPLES-1];

  logic [FAST_POS_WIDTH-1:0] fast_pos_q;
  logic [SLOW_POS_WIDTH-1:0] slow_pos_q;
  logic [FAST_COUNT_WIDTH-1:0] fast_count_q;
  logic [SLOW_COUNT_WIDTH-1:0] slow_count_q;
  logic [POWER_SUM_WIDTH-1:0] fast_sum_q;
  logic [POWER_SUM_WIDTH-1:0] slow_sum_q;

  logic [63:0] run_start_index_q;
  logic [63:0] run_power_sum_q;
  logic [RATIO_WIDTH-1:0] run_min_ratio_q;
  logic [31:0] run_length_q;
  logic [31:0] pending_gap_length_q;
  logic [63:0] pending_gap_power_sum_q;
  logic [63:0] last_null_sample_index_q;
  logic has_last_null_q;

  logic stage_valid_q;
  logic [POWER_WIDTH-1:0] stage_power_q;
  logic stage_fast_remove_valid_q;
  logic stage_slow_remove_valid_q;
  logic [FAST_COUNT_WIDTH-1:0] stage_fast_count_q;
  logic [SLOW_COUNT_WIDTH-1:0] stage_slow_count_q;
  logic [63:0] stage_sample_index_q;

  logic [POWER_WIDTH-1:0] fast_history_rd_q;
  logic [POWER_WIDTH-1:0] slow_history_rd_q;
  logic input_fire;
  logic output_can_advance;
  logic process_stage;
  logic [POWER_WIDTH-1:0] power;
  logic [POWER_WIDTH-1:0] fast_removed;
  logic [POWER_WIDTH-1:0] slow_removed;
  logic [FAST_COUNT_WIDTH-1:0] accept_fast_count_next;
  logic [SLOW_COUNT_WIDTH-1:0] accept_slow_count_next;
  logic [FAST_COUNT_WIDTH-1:0] fast_count_next;
  logic [SLOW_COUNT_WIDTH-1:0] slow_count_next;
  logic [POWER_SUM_WIDTH-1:0] fast_sum_next;
  logic [POWER_SUM_WIDTH-1:0] slow_sum_next;
  logic [RATIO_WIDTH-1:0] ratio_next;
  logic enter_ratio;
  logic exit_ratio;

  logic [31:0] run_length_plus_sample;
  logic [31:0] merged_run_length;
  logic [31:0] pending_gap_length_next;
  logic [63:0] run_power_sum_plus_sample;
  logic [63:0] merged_run_power_sum;
  logic [63:0] pending_gap_power_sum_next;
  logic low_run_too_long;
  logic gap_run_too_long;
  logic finalize_matches;

  assign output_can_advance = !out_valid || out_ready;
  assign in_ready = !clear && (!stage_valid_q || output_can_advance);
  assign input_fire = in_valid && in_ready;
  assign process_stage = stage_valid_q && output_can_advance;

  assign power = sample_power(in_i, in_q);
  assign fast_removed = stage_fast_remove_valid_q ? fast_history_rd_q : '0;
  assign slow_removed = stage_slow_remove_valid_q ? slow_history_rd_q : '0;

  assign accept_fast_count_next = fast_count_q == FAST_COUNT_WIDTH'(FAST_WINDOW_SAMPLES) ?
      fast_count_q : fast_count_q + 1'b1;
  assign accept_slow_count_next = slow_count_q == SLOW_COUNT_WIDTH'(SLOW_WINDOW_SAMPLES) ?
      slow_count_q : slow_count_q + 1'b1;
  assign fast_count_next = stage_fast_count_q;
  assign slow_count_next = stage_slow_count_q;

  assign fast_sum_next = fast_sum_q + POWER_SUM_WIDTH'(stage_power_q) - POWER_SUM_WIDTH'(fast_removed);
  assign slow_sum_next = slow_sum_q + POWER_SUM_WIDTH'(stage_power_q) - POWER_SUM_WIDTH'(slow_removed);
  assign ratio_next = ratio_q(fast_sum_next, fast_count_next, slow_sum_next, slow_count_next);
  assign enter_ratio = ratio_less_than_threshold(fast_sum_next,
                                                 fast_count_next,
                                                 slow_sum_next,
                                                 slow_count_next,
                                                 ENTER_THRESHOLD_NUM,
                                                 ENTER_THRESHOLD_DEN);
  assign exit_ratio = ratio_less_than_threshold(fast_sum_next,
                                                fast_count_next,
                                                slow_sum_next,
                                                slow_count_next,
                                                EXIT_THRESHOLD_NUM,
                                                EXIT_THRESHOLD_DEN);

  assign run_length_plus_sample = run_length_q + 32'd1;
  assign merged_run_length = run_length_q + pending_gap_length_q + 32'd1;
  assign pending_gap_length_next = pending_gap_length_q + 32'd1;
  assign run_power_sum_plus_sample = run_power_sum_q + 64'(stage_power_q);
  assign merged_run_power_sum = run_power_sum_q + pending_gap_power_sum_q + 64'(stage_power_q);
  assign pending_gap_power_sum_next = pending_gap_power_sum_q + 64'(stage_power_q);
  assign low_run_too_long = (MAX_NULL_SAMPLES != 0) &&
      (merged_run_length > 32'(MAX_NULL_SAMPLES + MAX_MERGE_GAP_SAMPLES));
  assign gap_run_too_long = (MAX_NULL_SAMPLES != 0) &&
      ((run_length_q + pending_gap_length_next) > 32'(MAX_NULL_SAMPLES + MAX_MERGE_GAP_SAMPLES));
  assign finalize_matches = run_matches_length(run_length_q) &&
                            run_matches_startup(run_start_index_q) &&
                            run_matches_cadence(run_start_index_q,
                                                last_null_sample_index_q,
                                                has_last_null_q);

  function automatic logic [POWER_WIDTH-1:0] sample_power(
      input logic signed [SAMPLE_WIDTH-1:0] sample_i,
      input logic signed [SAMPLE_WIDTH-1:0] sample_q
  );
    logic signed [63:0] i_wide;
    logic signed [63:0] q_wide;
    logic [63:0] total;
    logic [63:0] max_power;
    begin
      i_wide = {{(64 - SAMPLE_WIDTH){sample_i[SAMPLE_WIDTH-1]}}, sample_i};
      q_wide = {{(64 - SAMPLE_WIDTH){sample_q[SAMPLE_WIDTH-1]}}, sample_q};
      total = 64'((i_wide * i_wide) + (q_wide * q_wide));
      max_power = (64'd1 << POWER_WIDTH) - 64'd1;

      if (total > max_power) begin
        sample_power = max_power[POWER_WIDTH-1:0];
      end else begin
        sample_power = total[POWER_WIDTH-1:0];
      end
    end
  endfunction

  function automatic int unsigned bit_length_u128(input logic [127:0] value);
    integer bit_index;
    begin
      bit_length_u128 = 0;
      for (bit_index = 0; bit_index < 128; bit_index = bit_index + 1) begin
        if (value[bit_index]) begin
          bit_length_u128 = bit_index + 1;
        end
      end
    end
  endfunction

  function automatic logic [63:0] scaled_u128_to_u64(
      input logic [127:0] value,
      input int unsigned scale_shift
  );
    /* verilator lint_off UNUSEDSIGNAL */
    logic [127:0] shifted;
    /* verilator lint_on UNUSEDSIGNAL */
    begin
      shifted = value >> scale_shift;
      scaled_u128_to_u64 = shifted[63:0];
    end
  endfunction

  function automatic logic [RATIO_WIDTH-1:0] ratio_q(
      input logic [POWER_SUM_WIDTH-1:0] fast_sum,
      input logic [FAST_COUNT_WIDTH-1:0] fast_count,
      input logic [POWER_SUM_WIDTH-1:0] slow_sum,
      input logic [SLOW_COUNT_WIDTH-1:0] slow_count
  );
    logic [127:0] numerator;
    logic [127:0] denominator;
    logic [63:0] numerator_64;
    logic [63:0] denominator_64;
    logic [63:0] quotient_64;
    logic [63:0] max_ratio;
    int unsigned numerator_bits;
    int unsigned denominator_bits;
    int unsigned max_bits;
    int unsigned scale_shift;
    begin
      if (fast_count == '0 || slow_count == '0 || slow_sum == '0) begin
        ratio_q = RATIO_ONE;
      end else begin
        numerator = 128'(fast_sum);
        numerator = (numerator * slow_count) << RATIO_FRAC_BITS;
        denominator = 128'(slow_sum);
        denominator = denominator * fast_count;
        numerator_bits = bit_length_u128(numerator);
        denominator_bits = bit_length_u128(denominator);
        max_bits = numerator_bits > denominator_bits ? numerator_bits : denominator_bits;
        scale_shift = max_bits > 64 ? max_bits - 64 : 0;
        numerator_64 = scaled_u128_to_u64(numerator, scale_shift);
        denominator_64 = scaled_u128_to_u64(denominator, scale_shift);
        quotient_64 = denominator_64 == 64'd0 ? {64{1'b1}} : numerator_64 / denominator_64;
        max_ratio = (64'd1 << RATIO_WIDTH) - 64'd1;

        if (quotient_64 > max_ratio) begin
          ratio_q = max_ratio[RATIO_WIDTH-1:0];
        end else begin
          ratio_q = quotient_64[RATIO_WIDTH-1:0];
        end
      end
    end
  endfunction

  function automatic logic ratio_less_than_threshold(
      input logic [POWER_SUM_WIDTH-1:0] fast_sum,
      input logic [FAST_COUNT_WIDTH-1:0] fast_count,
      input logic [POWER_SUM_WIDTH-1:0] slow_sum,
      input logic [SLOW_COUNT_WIDTH-1:0] slow_count,
      input int unsigned threshold_num,
      input int unsigned threshold_den
  );
    logic [127:0] lhs;
    logic [127:0] rhs;
    begin
      if (fast_count == '0 || slow_count == '0 || slow_sum == '0 ||
          threshold_num == 0 || threshold_den == 0) begin
        ratio_less_than_threshold = 1'b0;
      end else begin
        lhs = 128'(fast_sum);
        lhs = lhs * slow_count * threshold_den;
        rhs = 128'(slow_sum);
        rhs = rhs * fast_count * threshold_num;
        ratio_less_than_threshold = lhs < rhs;
      end
    end
  endfunction

  function automatic logic [POWER_WIDTH-1:0] average_power(
      input logic [63:0] sum,
      input logic [31:0] length
  );
    logic [63:0] quotient;
    logic [63:0] max_power;
    begin
      if (length == 32'd0) begin
        average_power = '0;
      end else begin
        quotient = sum / 64'(length);
        max_power = (64'd1 << POWER_WIDTH) - 64'd1;
        if (quotient > max_power) begin
          average_power = max_power[POWER_WIDTH-1:0];
        end else begin
          average_power = quotient[POWER_WIDTH-1:0];
        end
      end
    end
  endfunction

  function automatic logic run_matches_length(input logic [31:0] length);
    begin
      if (length < 32'(MIN_NULL_SAMPLES)) begin
        run_matches_length = 1'b0;
      end else if (MAX_NULL_SAMPLES != 0 && length > 32'(MAX_NULL_SAMPLES)) begin
        run_matches_length = 1'b0;
      end else begin
        run_matches_length = 1'b1;
      end
    end
  endfunction

  function automatic logic run_matches_startup(input logic [63:0] start_index);
    begin
      if (STARTUP_IGNORE_SAMPLES == 0) begin
        run_matches_startup = 1'b1;
      end else begin
        /* verilator lint_off UNSIGNED */
        run_matches_startup = start_index >= 64'(STARTUP_IGNORE_SAMPLES);
        /* verilator lint_on UNSIGNED */
      end
    end
  endfunction

  function automatic logic run_matches_cadence(
      input logic [63:0] start_index,
      input logic [63:0] last_index,
      input logic has_last
  );
    logic [63:0] delta;
    logic [63:0] lower_bound;
    logic [63:0] upper_bound;
    begin
      if (EXPECTED_INTERVAL_SAMPLES == 0 || !has_last) begin
        run_matches_cadence = 1'b1;
      end else begin
        delta = start_index - last_index;
        lower_bound = EXPECTED_INTERVAL_SAMPLES > INTERVAL_TOLERANCE_SAMPLES ?
            64'(EXPECTED_INTERVAL_SAMPLES - INTERVAL_TOLERANCE_SAMPLES) : 64'd0;
        upper_bound = 64'(EXPECTED_INTERVAL_SAMPLES + INTERVAL_TOLERANCE_SAMPLES);
        run_matches_cadence = delta >= lower_bound && delta <= upper_bound;
      end
    end
  endfunction

  task automatic clear_run_state;
    begin
      run_length_q <= '0;
      run_power_sum_q <= '0;
      run_min_ratio_q <= RATIO_ONE;
      pending_gap_length_q <= '0;
      pending_gap_power_sum_q <= '0;
    end
  endtask

  always_ff @(posedge clk) begin
    if (input_fire) begin
      fast_history_rd_q <= fast_history[fast_pos_q];
      slow_history_rd_q <= slow_history[slow_pos_q];
      fast_history[fast_pos_q] <= power;
      slow_history[slow_pos_q] <= power;
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      fast_pos_q <= '0;
      slow_pos_q <= '0;
      fast_count_q <= '0;
      slow_count_q <= '0;
      fast_sum_q <= '0;
      slow_sum_q <= '0;
      run_start_index_q <= '0;
      run_power_sum_q <= '0;
      run_min_ratio_q <= RATIO_ONE;
      run_length_q <= '0;
      pending_gap_length_q <= '0;
      pending_gap_power_sum_q <= '0;
      last_null_sample_index_q <= '0;
      has_last_null_q <= 1'b0;
      stage_valid_q <= 1'b0;
      stage_power_q <= '0;
      stage_fast_remove_valid_q <= 1'b0;
      stage_slow_remove_valid_q <= 1'b0;
      stage_fast_count_q <= '0;
      stage_slow_count_q <= '0;
      stage_sample_index_q <= '0;
      out_valid <= 1'b0;
      out_sample_index <= '0;
      out_length <= '0;
      out_avg_power <= '0;
      out_min_ratio <= RATIO_ONE;
    end else if (clear) begin
      fast_pos_q <= '0;
      slow_pos_q <= '0;
      fast_count_q <= '0;
      slow_count_q <= '0;
      fast_sum_q <= '0;
      slow_sum_q <= '0;
      run_start_index_q <= '0;
      run_power_sum_q <= '0;
      run_min_ratio_q <= RATIO_ONE;
      run_length_q <= '0;
      pending_gap_length_q <= '0;
      pending_gap_power_sum_q <= '0;
      last_null_sample_index_q <= '0;
      has_last_null_q <= 1'b0;
      stage_valid_q <= 1'b0;
      stage_power_q <= '0;
      stage_fast_remove_valid_q <= 1'b0;
      stage_slow_remove_valid_q <= 1'b0;
      stage_fast_count_q <= '0;
      stage_slow_count_q <= '0;
      stage_sample_index_q <= '0;
      out_valid <= 1'b0;
      out_sample_index <= '0;
      out_length <= '0;
      out_avg_power <= '0;
      out_min_ratio <= RATIO_ONE;
    end else begin
      if (out_valid && out_ready) begin
        out_valid <= 1'b0;
      end

      if (process_stage) begin
        fast_sum_q <= fast_sum_next;
        slow_sum_q <= slow_sum_next;
        stage_valid_q <= 1'b0;

        if (run_length_q == 32'd0) begin
          if (enter_ratio) begin
            run_start_index_q <= stage_sample_index_q;
            run_power_sum_q <= 64'(stage_power_q);
            run_min_ratio_q <= ratio_next;
            run_length_q <= 32'd1;
            pending_gap_length_q <= '0;
            pending_gap_power_sum_q <= '0;
          end
        end else if (exit_ratio) begin
          if (low_run_too_long) begin
            clear_run_state();
          end else begin
            run_length_q <= pending_gap_length_q != 32'd0 ?
                merged_run_length : run_length_plus_sample;
            run_power_sum_q <= pending_gap_length_q != 32'd0 ?
                merged_run_power_sum : run_power_sum_plus_sample;
            if (ratio_next < run_min_ratio_q) begin
              run_min_ratio_q <= ratio_next;
            end
            pending_gap_length_q <= '0;
            pending_gap_power_sum_q <= '0;
          end
        end else begin
          if (pending_gap_length_next > 32'(MAX_MERGE_GAP_SAMPLES)) begin
            if (finalize_matches) begin
              out_valid <= 1'b1;
              out_sample_index <= run_start_index_q;
              out_length <= run_length_q;
              out_avg_power <= average_power(run_power_sum_q, run_length_q);
              out_min_ratio <= run_min_ratio_q;
              last_null_sample_index_q <= run_start_index_q;
              has_last_null_q <= 1'b1;
            end
            clear_run_state();
          end else if (gap_run_too_long) begin
            clear_run_state();
          end else begin
            pending_gap_length_q <= pending_gap_length_next;
            pending_gap_power_sum_q <= pending_gap_power_sum_next;
          end
        end
      end

      if (input_fire) begin
        stage_valid_q <= 1'b1;
        stage_power_q <= power;
        stage_fast_remove_valid_q <= fast_count_q == FAST_COUNT_WIDTH'(FAST_WINDOW_SAMPLES);
        stage_slow_remove_valid_q <= slow_count_q == SLOW_COUNT_WIDTH'(SLOW_WINDOW_SAMPLES);
        stage_fast_count_q <= accept_fast_count_next;
        stage_slow_count_q <= accept_slow_count_next;
        stage_sample_index_q <= in_sample_index;

        fast_count_q <= accept_fast_count_next;
        slow_count_q <= accept_slow_count_next;

        if (fast_pos_q == FAST_POS_WIDTH'(FAST_WINDOW_SAMPLES - 1)) begin
          fast_pos_q <= '0;
        end else begin
          fast_pos_q <= fast_pos_q + 1'b1;
        end

        if (slow_pos_q == SLOW_POS_WIDTH'(SLOW_WINDOW_SAMPLES - 1)) begin
          slow_pos_q <= '0;
        end else begin
          slow_pos_q <= slow_pos_q + 1'b1;
        end
      end
    end
  end
endmodule

`default_nettype wire
