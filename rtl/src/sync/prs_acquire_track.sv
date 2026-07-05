`default_nettype none

module prs_acquire_track #(
    parameter int SCORE_WIDTH = 32,
    parameter int CFO_WIDTH = 32,
    parameter int RATIO_WIDTH = 24,
    parameter int TIMING_WIDTH = 24,
    parameter int TIMING_FRAC_BITS = 12,
    parameter int GUARD_SAMPLES = 504,
    parameter int FFT_SAMPLES = 2048,
    parameter int FRAME_SAMPLES = 196608,
    parameter int ACQUIRE_WINDOW_SAMPLES = 1200,
    parameter int TRACK_WINDOW_SAMPLES = 65,
    parameter int LOCK_CONFIRM_COUNT = 2,
    parameter int UNLOCK_THRESHOLD = 2
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic cfg_enable_tracking,
    input  wire logic [SCORE_WIDTH-1:0] cfg_min_cp_score,
    input  wire logic [SCORE_WIDTH-1:0] cfg_min_pilot_score,

    input  wire logic null_in_valid,
    output logic null_in_ready,
    input  wire logic [63:0] null_in_start_sample,
    input  wire logic [31:0] null_in_length,
    input  wire logic [RATIO_WIDTH-1:0] null_in_min_ratio,

    input  wire logic predict_in_valid,
    output logic predict_in_ready,
    input  wire logic [63:0] predict_in_prs_fft_start_sample,

    output logic search_out_valid,
    input  wire logic search_out_ready,
    output logic [63:0] search_out_null_start_sample,
    output logic [31:0] search_out_null_length,
    output logic [RATIO_WIDTH-1:0] search_out_null_min_ratio,
    output logic [63:0] search_out_prs_symbol_start_sample,
    output logic [63:0] search_out_prs_fft_start_sample,
    output logic [31:0] search_out_timing_search_start_offset,
    output logic [31:0] search_out_timing_search_length,
    output logic [63:0] search_out_search_start_sample,
    output logic [63:0] search_out_search_end_sample,
    output logic [63:0] search_out_required_end_sample,

    input  wire logic score_in_valid,
    output logic score_in_ready,
    input  wire logic score_in_found,
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire logic [63:0] score_in_null_start_sample,
    input  wire logic [31:0] score_in_null_length,
    input  wire logic [63:0] score_in_candidate_prs_symbol_start_sample,
    input  wire logic [63:0] score_in_candidate_prs_fft_start_sample,
    input  wire logic [31:0] score_in_timing_search_start_offset,
    input  wire logic [31:0] score_in_timing_search_length,
    input  wire logic [63:0] score_in_search_start_sample,
    input  wire logic [63:0] score_in_search_end_sample,
    input  wire logic [63:0] score_in_required_end_sample,
    input  wire logic [63:0] score_in_prs_peak_sample,
    /* verilator lint_on UNUSEDSIGNAL */
    input  wire logic [63:0] score_in_prs_symbol_start_sample,
    input  wire logic [63:0] score_in_prs_fft_start_sample,
    input  wire logic [SCORE_WIDTH-1:0] score_in_prs_score,
    input  wire logic signed [CFO_WIDTH-1:0] score_in_coarse_freq_error_hz,

    output logic prs_symbol_req_valid,
    input  wire logic prs_symbol_req_ready,
    output logic [63:0] prs_symbol_req_symbol_start_sample,
    output logic [63:0] prs_symbol_req_fft_start_sample,
    output logic [SCORE_WIDTH-1:0] prs_symbol_req_score,
    output logic signed [CFO_WIDTH-1:0] prs_symbol_req_coarse_freq_error_hz,

    input  wire logic pilot_in_valid,
    output logic pilot_in_ready,
    input  wire logic pilot_in_found,
    input  wire logic [63:0] pilot_in_symbol_start_sample,
    input  wire logic [SCORE_WIDTH-1:0] pilot_in_score,
    input  wire logic signed [3:0] pilot_in_carrier_offset_bins,
    input  wire logic signed [TIMING_WIDTH-1:0] pilot_in_fine_timing_error_samples,
    input  wire logic [11:0] pilot_in_used_pairs,

    output logic out_valid,
    input  wire logic out_ready,
    output logic out_found,
    output logic out_locked,
    output logic [7:0] out_lock_count,
    output logic [7:0] out_miss_count,
    output logic [63:0] out_prs_symbol_start_sample,
    output logic [63:0] out_prs_fft_start_sample,
    output logic [SCORE_WIDTH-1:0] out_prs_score,
    output logic signed [CFO_WIDTH-1:0] out_coarse_freq_error_hz,
    output logic [SCORE_WIDTH-1:0] out_pilot_score,
    output logic signed [3:0] out_carrier_offset_bins,
    output logic signed [TIMING_WIDTH-1:0] out_fine_timing_error_samples,
    output logic [11:0] out_pilot_used_pairs,
    output logic [63:0] out_next_prs_fft_start_sample
);
  typedef enum logic [2:0] {
    STATE_IDLE = 3'd0,
    STATE_SEARCH_OUT = 3'd1,
    STATE_WAIT_SCORE = 3'd2,
    STATE_PRS_SYMBOL_REQ = 3'd3,
    STATE_WAIT_PILOT = 3'd4,
    STATE_OUTPUT = 3'd5
  } state_t;

  state_t state_q;

  logic locked_q;
  logic [7:0] lock_count_q;
  logic [7:0] miss_count_q;
  logic [63:0] next_prs_fft_start_sample_q;

  logic [63:0] score_prs_symbol_start_sample_q;
  logic [63:0] score_prs_fft_start_sample_q;
  logic [SCORE_WIDTH-1:0] score_prs_score_q;
  logic signed [CFO_WIDTH-1:0] score_cfo_q;

  logic predict_fire;
  logic null_fire;
  logic search_fire;
  logic score_fire;
  logic prs_symbol_fire;
  logic pilot_fire;
  logic output_fire;
  logic score_pass;
  logic pilot_success;

  assign predict_in_ready = !clear && state_q == STATE_IDLE && cfg_enable_tracking && locked_q;
  assign null_in_ready = !clear && state_q == STATE_IDLE &&
      !(predict_in_valid && predict_in_ready);
  assign predict_fire = predict_in_valid && predict_in_ready;
  assign null_fire = null_in_valid && null_in_ready;

  assign search_out_valid = !clear && state_q == STATE_SEARCH_OUT;
  assign search_fire = search_out_valid && search_out_ready;

  assign score_in_ready = !clear && state_q == STATE_WAIT_SCORE;
  assign score_fire = score_in_valid && score_in_ready;
  assign score_pass = score_in_found && score_in_prs_score >= cfg_min_cp_score;

  assign prs_symbol_req_valid = !clear && state_q == STATE_PRS_SYMBOL_REQ;
  assign prs_symbol_req_symbol_start_sample = score_prs_symbol_start_sample_q;
  assign prs_symbol_req_fft_start_sample = score_prs_fft_start_sample_q;
  assign prs_symbol_req_score = score_prs_score_q;
  assign prs_symbol_req_coarse_freq_error_hz = score_cfo_q;
  assign prs_symbol_fire = prs_symbol_req_valid && prs_symbol_req_ready;

  assign pilot_in_ready = !clear && state_q == STATE_WAIT_PILOT;
  assign pilot_fire = pilot_in_valid && pilot_in_ready;
  assign pilot_success = pilot_in_found &&
      pilot_in_score >= cfg_min_pilot_score &&
      pilot_in_symbol_start_sample == score_prs_fft_start_sample_q;

  assign output_fire = out_valid && out_ready;

  function automatic logic [63:0] sat_sub64(
      input logic [63:0] value,
      input logic [63:0] decrement
  );
    begin
      sat_sub64 = value > decrement ? value - decrement : '0;
    end
  endfunction

  function automatic logic signed [31:0] timing_to_integer_samples(
      input logic signed [TIMING_WIDTH-1:0] timing_q
  );
    logic signed [TIMING_WIDTH:0] wide;
    logic signed [TIMING_WIDTH:0] rounded;
    logic signed [TIMING_WIDTH:0] half_lsb;
    begin
      wide = {timing_q[TIMING_WIDTH-1], timing_q};
      half_lsb = (TIMING_WIDTH + 1)'(1) <<< (TIMING_FRAC_BITS - 1);
      if (timing_q[TIMING_WIDTH-1]) begin
        rounded = wide - half_lsb;
      end else begin
        rounded = wide + half_lsb;
      end
      timing_to_integer_samples = 32'(rounded >>> TIMING_FRAC_BITS);
    end
  endfunction

  function automatic logic [63:0] add_signed_samples(
      input logic [63:0] value,
      input logic signed [31:0] delta
  );
    logic [31:0] magnitude;
    begin
      if (delta[31]) begin
        magnitude = 32'(-delta);
        add_signed_samples = value > 64'(magnitude) ? value - 64'(magnitude) : '0;
      end else begin
        add_signed_samples = value + 64'(delta);
      end
    end
  endfunction

  task automatic set_search_window(
      input logic [63:0] null_start,
      input logic [31:0] null_length,
      input logic [RATIO_WIDTH-1:0] null_min_ratio,
      input logic [63:0] prs_fft_start,
      input logic [31:0] window_samples
  );
    logic [31:0] half_window;
    logic [63:0] search_start;
    begin
      half_window = window_samples >> 1;
      search_start = sat_sub64(prs_fft_start, 64'(half_window));

      search_out_null_start_sample <= null_start;
      search_out_null_length <= null_length;
      search_out_null_min_ratio <= null_min_ratio;
      search_out_prs_symbol_start_sample <= sat_sub64(prs_fft_start, 64'(GUARD_SAMPLES));
      search_out_prs_fft_start_sample <= prs_fft_start;
      search_out_timing_search_start_offset <= half_window;
      search_out_timing_search_length <= window_samples;
      search_out_search_start_sample <= search_start;
      search_out_search_end_sample <= search_start + 64'(window_samples);
      search_out_required_end_sample <= search_start + 64'(window_samples) + 64'(FFT_SAMPLES);
    end
  endtask

  task automatic clear_outputs;
    begin
      search_out_null_start_sample <= '0;
      search_out_null_length <= '0;
      search_out_null_min_ratio <= '0;
      search_out_prs_symbol_start_sample <= '0;
      search_out_prs_fft_start_sample <= '0;
      search_out_timing_search_start_offset <= '0;
      search_out_timing_search_length <= '0;
      search_out_search_start_sample <= '0;
      search_out_search_end_sample <= '0;
      search_out_required_end_sample <= '0;
      score_prs_symbol_start_sample_q <= '0;
      score_prs_fft_start_sample_q <= '0;
      score_prs_score_q <= '0;
      score_cfo_q <= '0;
      out_valid <= 1'b0;
      out_found <= 1'b0;
      out_locked <= 1'b0;
      out_lock_count <= '0;
      out_miss_count <= '0;
      out_prs_symbol_start_sample <= '0;
      out_prs_fft_start_sample <= '0;
      out_prs_score <= '0;
      out_coarse_freq_error_hz <= '0;
      out_pilot_score <= '0;
      out_carrier_offset_bins <= '0;
      out_fine_timing_error_samples <= '0;
      out_pilot_used_pairs <= '0;
      out_next_prs_fft_start_sample <= '0;
    end
  endtask

  task automatic emit_miss;
    logic [7:0] next_miss_count;
    logic next_locked;
    begin
      next_miss_count = miss_count_q == 8'hff ? miss_count_q : miss_count_q + 1'b1;
      next_locked = locked_q && next_miss_count < 8'(UNLOCK_THRESHOLD);

      locked_q <= next_locked;
      lock_count_q <= '0;
      miss_count_q <= next_miss_count;

      out_valid <= 1'b1;
      out_found <= 1'b0;
      out_locked <= next_locked;
      out_lock_count <= '0;
      out_miss_count <= next_miss_count;
      out_prs_symbol_start_sample <= '0;
      out_prs_fft_start_sample <= '0;
      out_prs_score <= '0;
      out_coarse_freq_error_hz <= '0;
      out_pilot_score <= '0;
      out_carrier_offset_bins <= '0;
      out_fine_timing_error_samples <= '0;
      out_pilot_used_pairs <= '0;
      out_next_prs_fft_start_sample <= next_prs_fft_start_sample_q;
    end
  endtask

  task automatic emit_success;
    logic [7:0] next_lock_count;
    logic [63:0] next_fft_start;
    logic signed [31:0] fine_timing_integer;
    begin
      next_lock_count = lock_count_q == 8'hff ? lock_count_q : lock_count_q + 1'b1;
      fine_timing_integer = timing_to_integer_samples(pilot_in_fine_timing_error_samples);
      next_fft_start =
          add_signed_samples(score_prs_fft_start_sample_q + 64'(FRAME_SAMPLES),
                             fine_timing_integer);

      locked_q <= next_lock_count >= 8'(LOCK_CONFIRM_COUNT);
      lock_count_q <= next_lock_count;
      miss_count_q <= '0;
      next_prs_fft_start_sample_q <= next_fft_start;

      out_valid <= 1'b1;
      out_found <= 1'b1;
      out_locked <= next_lock_count >= 8'(LOCK_CONFIRM_COUNT);
      out_lock_count <= next_lock_count;
      out_miss_count <= '0;
      out_prs_symbol_start_sample <= score_prs_symbol_start_sample_q;
      out_prs_fft_start_sample <= score_prs_fft_start_sample_q;
      out_prs_score <= score_prs_score_q;
      out_coarse_freq_error_hz <= score_cfo_q;
      out_pilot_score <= pilot_in_score;
      out_carrier_offset_bins <= pilot_in_carrier_offset_bins;
      out_fine_timing_error_samples <= pilot_in_fine_timing_error_samples;
      out_pilot_used_pairs <= pilot_in_used_pairs;
      out_next_prs_fft_start_sample <= next_fft_start;
    end
  endtask

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state_q <= STATE_IDLE;
      locked_q <= 1'b0;
      lock_count_q <= '0;
      miss_count_q <= '0;
      next_prs_fft_start_sample_q <= '0;
      clear_outputs();
    end else if (clear) begin
      state_q <= STATE_IDLE;
      locked_q <= 1'b0;
      lock_count_q <= '0;
      miss_count_q <= '0;
      next_prs_fft_start_sample_q <= '0;
      clear_outputs();
    end else begin
      if (output_fire) begin
        out_valid <= 1'b0;
      end

      case (state_q)
        STATE_IDLE: begin
          if (predict_fire) begin
            set_search_window('0,
                              '0,
                              '0,
                              predict_in_prs_fft_start_sample == 64'd0 ?
                                  next_prs_fft_start_sample_q : predict_in_prs_fft_start_sample,
                              32'(TRACK_WINDOW_SAMPLES));
            state_q <= STATE_SEARCH_OUT;
          end else if (null_fire) begin
            if (cfg_enable_tracking && locked_q && next_prs_fft_start_sample_q != 64'd0) begin
              set_search_window(null_in_start_sample,
                                null_in_length,
                                null_in_min_ratio,
                                next_prs_fft_start_sample_q,
                                32'(TRACK_WINDOW_SAMPLES));
            end else begin
              set_search_window(null_in_start_sample,
                                null_in_length,
                                null_in_min_ratio,
                                null_in_start_sample + 64'(null_in_length) + 64'(GUARD_SAMPLES),
                                32'(ACQUIRE_WINDOW_SAMPLES));
            end
            state_q <= STATE_SEARCH_OUT;
          end
        end

        STATE_SEARCH_OUT: begin
          if (search_fire) begin
            state_q <= STATE_WAIT_SCORE;
          end
        end

        STATE_WAIT_SCORE: begin
          if (score_fire) begin
            if (score_pass) begin
              score_prs_symbol_start_sample_q <= score_in_prs_symbol_start_sample;
              score_prs_fft_start_sample_q <= score_in_prs_fft_start_sample;
              score_prs_score_q <= score_in_prs_score;
              score_cfo_q <= score_in_coarse_freq_error_hz;
              state_q <= STATE_PRS_SYMBOL_REQ;
            end else begin
              emit_miss();
              state_q <= STATE_OUTPUT;
            end
          end
        end

        STATE_PRS_SYMBOL_REQ: begin
          if (prs_symbol_fire) begin
            state_q <= STATE_WAIT_PILOT;
          end
        end

        STATE_WAIT_PILOT: begin
          if (pilot_fire) begin
            if (pilot_success) begin
              emit_success();
            end else begin
              emit_miss();
            end
            state_q <= STATE_OUTPUT;
          end
        end

        default: begin
          if (output_fire) begin
            state_q <= STATE_IDLE;
          end
        end
      endcase
    end
  end
endmodule

`default_nettype wire
