`default_nettype none

module prs_score_search #(
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

    output logic metric_req_valid,
    input  wire logic metric_req_ready,
    output logic [63:0] metric_req_fft_start_sample,
    output logic [31:0] metric_req_index,
    output logic metric_req_last,

    input  wire logic metric_resp_valid,
    output logic metric_resp_ready,
    input  wire logic metric_resp_ok,
    input  wire logic [63:0] metric_resp_fft_start_sample,
    input  wire logic [SCORE_WIDTH-1:0] metric_resp_score,
    input  wire logic signed [CFO_WIDTH-1:0] metric_resp_coarse_freq_error_hz,

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
  logic [31:0] req_count_q;
  logic [31:0] resp_count_q;

  logic input_fire;
  logic req_fire;
  logic resp_fire;
  logic reqs_done;
  logic last_request;
  logic last_response;
  logic response_is_better;
  logic best_found_next;
  logic [SCORE_WIDTH-1:0] best_score_next;
  logic [63:0] best_fft_start_sample_next;
  logic signed [CFO_WIDTH-1:0] best_cfo_next;

  assign in_ready = !clear && !active_q && !out_valid;
  assign input_fire = in_valid && in_ready;

  assign reqs_done = req_count_q >= timing_search_length_q;
  assign metric_req_valid = !clear && active_q && !reqs_done;
  assign metric_req_fft_start_sample = search_start_sample_q + 64'(req_count_q);
  assign metric_req_index = req_count_q;
  assign metric_req_last = last_request;
  assign last_request = timing_search_length_q != 32'd0 &&
      req_count_q == (timing_search_length_q - 1'b1);
  assign req_fire = metric_req_valid && metric_req_ready;

  assign metric_resp_ready = !clear && active_q;
  assign resp_fire = metric_resp_valid && metric_resp_ready;
  assign last_response = timing_search_length_q != 32'd0 &&
      resp_count_q == (timing_search_length_q - 1'b1);

  assign response_is_better = metric_resp_ok &&
      (!best_found_q || metric_resp_score > best_score_q);
  assign best_found_next = best_found_q || metric_resp_ok;
  assign best_score_next = response_is_better ? metric_resp_score : best_score_q;
  assign best_fft_start_sample_next =
      response_is_better ? metric_resp_fft_start_sample : best_fft_start_sample_q;
  assign best_cfo_next =
      response_is_better ? metric_resp_coarse_freq_error_hz : best_cfo_q;

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
      req_count_q <= '0;
      resp_count_q <= '0;
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
      req_count_q <= '0;
      resp_count_q <= '0;
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
        req_count_q <= '0;
        resp_count_q <= '0;
        best_found_q <= 1'b0;
        best_score_q <= '0;
        best_fft_start_sample_q <= '0;
        best_cfo_q <= '0;

        if (in_timing_search_length == 32'd0) begin
          active_q <= 1'b0;
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
        end else begin
          active_q <= 1'b1;
        end
      end

      if (req_fire) begin
        req_count_q <= req_count_q + 1'b1;
      end

      if (resp_fire) begin
        best_found_q <= best_found_next;
        best_score_q <= best_score_next;
        best_fft_start_sample_q <= best_fft_start_sample_next;
        best_cfo_q <= best_cfo_next;
        resp_count_q <= resp_count_q + 1'b1;

        if (last_response) begin
          active_q <= 1'b0;
          req_count_q <= '0;
          resp_count_q <= '0;
          out_valid <= 1'b1;
          out_found <= best_found_next;
          out_null_start_sample <= null_start_sample_q;
          out_null_length <= null_length_q;
          out_candidate_prs_symbol_start_sample <= candidate_prs_symbol_start_sample_q;
          out_candidate_prs_fft_start_sample <= candidate_prs_fft_start_sample_q;
          out_timing_search_start_offset <= timing_search_start_offset_q;
          out_timing_search_length <= timing_search_length_q;
          out_search_start_sample <= search_start_sample_q;
          out_search_end_sample <= search_end_sample_q;
          out_required_end_sample <= required_end_sample_q;
          out_prs_peak_sample <= best_found_next ? best_fft_start_sample_next : '0;
          out_prs_fft_start_sample <= best_found_next ? best_fft_start_sample_next : '0;
          out_prs_symbol_start_sample <= best_found_next ?
              best_fft_start_sample_next - 64'(GUARD_SAMPLES) : '0;
          out_prs_score <= best_found_next ? best_score_next : '0;
          out_coarse_freq_error_hz <= best_found_next ? best_cfo_next : '0;
        end
      end
    end
  end
endmodule

`default_nettype wire
