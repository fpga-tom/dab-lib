`default_nettype none

module prs_stream_search #(
    parameter int SCORE_WIDTH = 32,
    parameter int CFO_WIDTH = 32,
    parameter int GUARD_SAMPLES = 504
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic in_valid,
    output logic in_ready,
    input  wire logic [63:0] in_null_start_sample,
    input  wire logic [31:0] in_null_length,
    input  wire logic [63:0] in_prs_symbol_start_sample,
    input  wire logic [63:0] in_prs_fft_start_sample,
    input  wire logic [31:0] in_timing_search_start_offset,
    input  wire logic [31:0] in_timing_search_length,
    input  wire logic [63:0] in_search_start_sample,
    input  wire logic [63:0] in_search_end_sample,
    input  wire logic [63:0] in_required_end_sample,

    input  wire logic metric_in_valid,
    input  wire logic metric_in_ok,
    input  wire logic [63:0] metric_in_fft_start_sample,
    input  wire logic [SCORE_WIDTH-1:0] metric_in_score,
    input  wire logic signed [CFO_WIDTH-1:0] metric_in_coarse_freq_error_hz,

    output logic out_valid,
    input  wire logic out_ready,
    output logic out_found,
    output logic [63:0] out_null_start_sample,
    output logic [31:0] out_null_length,
    output logic [63:0] out_candidate_prs_symbol_start_sample,
    output logic [63:0] out_candidate_prs_fft_start_sample,
    output logic [31:0] out_timing_search_start_offset,
    output logic [31:0] out_timing_search_length,
    output logic [63:0] out_search_start_sample,
    output logic [63:0] out_search_end_sample,
    output logic [63:0] out_required_end_sample,
    output logic [63:0] out_prs_peak_sample,
    output logic [63:0] out_prs_symbol_start_sample,
    output logic [63:0] out_prs_fft_start_sample,
    output logic [SCORE_WIDTH-1:0] out_prs_score,
    output logic signed [CFO_WIDTH-1:0] out_coarse_freq_error_hz
);
  logic active_q;
  logic best_found_q;
  logic [SCORE_WIDTH-1:0] best_score_q;
  logic [63:0] best_fft_start_sample_q;
  logic signed [CFO_WIDTH-1:0] best_cfo_q;

  logic [63:0] null_start_sample_q;
  logic [31:0] null_length_q;
  logic [63:0] candidate_prs_symbol_start_sample_q;
  logic [63:0] candidate_prs_fft_start_sample_q;
  logic [31:0] timing_search_start_offset_q;
  logic [31:0] timing_search_length_q;
  logic [63:0] search_start_sample_q;
  logic [63:0] search_end_sample_q;
  logic [63:0] required_end_sample_q;

  logic input_fire;
  logic metric_fire;
  logic metric_in_window;
  logic metric_is_better;
  logic finish_on_metric;
  logic best_found_next;
  logic [SCORE_WIDTH-1:0] best_score_next;
  logic [63:0] best_fft_start_sample_next;
  logic signed [CFO_WIDTH-1:0] best_cfo_next;

  assign in_ready = !clear && !active_q && !out_valid;
  assign input_fire = in_valid && in_ready;
  assign metric_fire = metric_in_valid && !clear && active_q;
  assign metric_in_window =
      metric_in_fft_start_sample >= search_start_sample_q &&
      metric_in_fft_start_sample < search_end_sample_q;
  assign metric_is_better = metric_fire && metric_in_window && metric_in_ok &&
      (!best_found_q || metric_in_score > best_score_q);
  assign finish_on_metric = metric_fire && metric_in_fft_start_sample >=
      (search_end_sample_q == 64'd0 ? 64'd0 : search_end_sample_q - 64'd1);

  assign best_found_next = best_found_q ||
      (metric_fire && metric_in_window && metric_in_ok);
  assign best_score_next = metric_is_better ? metric_in_score : best_score_q;
  assign best_fft_start_sample_next =
      metric_is_better ? metric_in_fft_start_sample : best_fft_start_sample_q;
  assign best_cfo_next =
      metric_is_better ? metric_in_coarse_freq_error_hz : best_cfo_q;

  task automatic clear_outputs;
    begin
      out_valid <= 1'b0;
      out_found <= 1'b0;
      out_null_start_sample <= '0;
      out_null_length <= '0;
      out_candidate_prs_symbol_start_sample <= '0;
      out_candidate_prs_fft_start_sample <= '0;
      out_timing_search_start_offset <= '0;
      out_timing_search_length <= '0;
      out_search_start_sample <= '0;
      out_search_end_sample <= '0;
      out_required_end_sample <= '0;
      out_prs_peak_sample <= '0;
      out_prs_symbol_start_sample <= '0;
      out_prs_fft_start_sample <= '0;
      out_prs_score <= '0;
      out_coarse_freq_error_hz <= '0;
    end
  endtask

  task automatic emit_result(
      input logic found,
      input logic [SCORE_WIDTH-1:0] score,
      input logic [63:0] fft_start_sample,
      input logic signed [CFO_WIDTH-1:0] cfo_hz
  );
    begin
      out_valid <= 1'b1;
      out_found <= found;
      out_null_start_sample <= null_start_sample_q;
      out_null_length <= null_length_q;
      out_candidate_prs_symbol_start_sample <= candidate_prs_symbol_start_sample_q;
      out_candidate_prs_fft_start_sample <= candidate_prs_fft_start_sample_q;
      out_timing_search_start_offset <= timing_search_start_offset_q;
      out_timing_search_length <= timing_search_length_q;
      out_search_start_sample <= search_start_sample_q;
      out_search_end_sample <= search_end_sample_q;
      out_required_end_sample <= required_end_sample_q;
      out_prs_peak_sample <= found ? fft_start_sample : '0;
      out_prs_fft_start_sample <= found ? fft_start_sample : '0;
      out_prs_symbol_start_sample <= found ? fft_start_sample - 64'(GUARD_SAMPLES) : '0;
      out_prs_score <= found ? score : '0;
      out_coarse_freq_error_hz <= found ? cfo_hz : '0;
    end
  endtask

  task automatic emit_input_miss;
    begin
      out_valid <= 1'b1;
      out_found <= 1'b0;
      out_null_start_sample <= in_null_start_sample;
      out_null_length <= in_null_length;
      out_candidate_prs_symbol_start_sample <= in_prs_symbol_start_sample;
      out_candidate_prs_fft_start_sample <= in_prs_fft_start_sample;
      out_timing_search_start_offset <= in_timing_search_start_offset;
      out_timing_search_length <= in_timing_search_length;
      out_search_start_sample <= in_search_start_sample;
      out_search_end_sample <= in_search_end_sample;
      out_required_end_sample <= in_required_end_sample;
      out_prs_peak_sample <= '0;
      out_prs_symbol_start_sample <= '0;
      out_prs_fft_start_sample <= '0;
      out_prs_score <= '0;
      out_coarse_freq_error_hz <= '0;
    end
  endtask

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      active_q <= 1'b0;
      best_found_q <= 1'b0;
      best_score_q <= '0;
      best_fft_start_sample_q <= '0;
      best_cfo_q <= '0;
      null_start_sample_q <= '0;
      null_length_q <= '0;
      candidate_prs_symbol_start_sample_q <= '0;
      candidate_prs_fft_start_sample_q <= '0;
      timing_search_start_offset_q <= '0;
      timing_search_length_q <= '0;
      search_start_sample_q <= '0;
      search_end_sample_q <= '0;
      required_end_sample_q <= '0;
      clear_outputs();
    end else if (clear) begin
      active_q <= 1'b0;
      best_found_q <= 1'b0;
      best_score_q <= '0;
      best_fft_start_sample_q <= '0;
      best_cfo_q <= '0;
      null_start_sample_q <= '0;
      null_length_q <= '0;
      candidate_prs_symbol_start_sample_q <= '0;
      candidate_prs_fft_start_sample_q <= '0;
      timing_search_start_offset_q <= '0;
      timing_search_length_q <= '0;
      search_start_sample_q <= '0;
      search_end_sample_q <= '0;
      required_end_sample_q <= '0;
      clear_outputs();
    end else begin
      if (out_valid && out_ready) begin
        out_valid <= 1'b0;
      end

      if (input_fire) begin
        null_start_sample_q <= in_null_start_sample;
        null_length_q <= in_null_length;
        candidate_prs_symbol_start_sample_q <= in_prs_symbol_start_sample;
        candidate_prs_fft_start_sample_q <= in_prs_fft_start_sample;
        timing_search_start_offset_q <= in_timing_search_start_offset;
        timing_search_length_q <= in_timing_search_length;
        search_start_sample_q <= in_search_start_sample;
        search_end_sample_q <= in_search_end_sample;
        required_end_sample_q <= in_required_end_sample;
        best_found_q <= 1'b0;
        best_score_q <= '0;
        best_fft_start_sample_q <= '0;
        best_cfo_q <= '0;

        if (in_timing_search_length == 32'd0 ||
            in_search_end_sample <= in_search_start_sample) begin
          active_q <= 1'b0;
          emit_input_miss();
        end else begin
          active_q <= 1'b1;
        end
      end

      if (metric_fire) begin
        best_found_q <= best_found_next;
        best_score_q <= best_score_next;
        best_fft_start_sample_q <= best_fft_start_sample_next;
        best_cfo_q <= best_cfo_next;

        if (finish_on_metric) begin
          active_q <= 1'b0;
          emit_result(best_found_next,
                      best_score_next,
                      best_fft_start_sample_next,
                      best_cfo_next);
        end
      end
    end
  end
endmodule

`default_nettype wire
