`default_nettype none

module prs_acquire #(
    parameter int GUARD_SAMPLES = 504,
    parameter int FFT_SAMPLES = 2048,
    parameter int ACQUIRE_WINDOW_SAMPLES = 1200,
    parameter int RATIO_WIDTH = 24
) (
    input  logic clk,
    input  logic rst_n,
    input  logic clear,

    input  logic in_valid,
    output logic in_ready,
    input  logic [63:0] in_null_start_sample,
    input  logic [31:0] in_null_length,
    input  logic [RATIO_WIDTH-1:0] in_null_min_ratio,

    output logic out_valid,
    input  logic out_ready,
    output logic [63:0] out_null_start_sample,
    output logic [31:0] out_null_length,
    output logic [RATIO_WIDTH-1:0] out_null_min_ratio,
    output logic [63:0] out_prs_symbol_start_sample,
    output logic [63:0] out_prs_fft_start_sample,
    output logic [31:0] out_timing_search_start_offset,
    output logic [31:0] out_timing_search_length,
    output logic [63:0] out_search_start_sample,
    output logic [63:0] out_search_end_sample,
    output logic [63:0] out_required_end_sample
);
  localparam logic [31:0] TIMING_SEARCH_START_OFFSET =
      32'(ACQUIRE_WINDOW_SAMPLES / 2);
  localparam logic [31:0] TIMING_SEARCH_LENGTH =
      32'(ACQUIRE_WINDOW_SAMPLES);

  logic pending_output;
  logic input_fire;
  logic [63:0] prs_symbol_start_sample;
  logic [63:0] prs_fft_start_sample;
  logic [63:0] search_start_sample;
  logic [63:0] search_end_sample;
  logic [63:0] required_end_sample;

  assign pending_output = out_valid && !out_ready;
  assign in_ready = !clear && !pending_output;
  assign input_fire = in_valid && in_ready;

  assign prs_symbol_start_sample = in_null_start_sample + 64'(in_null_length);
  assign prs_fft_start_sample = prs_symbol_start_sample + 64'(GUARD_SAMPLES);
  assign search_start_sample = prs_fft_start_sample - 64'(TIMING_SEARCH_START_OFFSET);
  assign search_end_sample = search_start_sample + 64'(TIMING_SEARCH_LENGTH);
  assign required_end_sample = search_end_sample + 64'(FFT_SAMPLES);

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      out_valid <= 1'b0;
      out_null_start_sample <= '0;
      out_null_length <= '0;
      out_null_min_ratio <= '0;
      out_prs_symbol_start_sample <= '0;
      out_prs_fft_start_sample <= '0;
      out_timing_search_start_offset <= TIMING_SEARCH_START_OFFSET;
      out_timing_search_length <= TIMING_SEARCH_LENGTH;
      out_search_start_sample <= '0;
      out_search_end_sample <= '0;
      out_required_end_sample <= '0;
    end else if (clear) begin
      out_valid <= 1'b0;
      out_null_start_sample <= '0;
      out_null_length <= '0;
      out_null_min_ratio <= '0;
      out_prs_symbol_start_sample <= '0;
      out_prs_fft_start_sample <= '0;
      out_timing_search_start_offset <= TIMING_SEARCH_START_OFFSET;
      out_timing_search_length <= TIMING_SEARCH_LENGTH;
      out_search_start_sample <= '0;
      out_search_end_sample <= '0;
      out_required_end_sample <= '0;
    end else begin
      if (input_fire) begin
        out_valid <= 1'b1;
        out_null_start_sample <= in_null_start_sample;
        out_null_length <= in_null_length;
        out_null_min_ratio <= in_null_min_ratio;
        out_prs_symbol_start_sample <= prs_symbol_start_sample;
        out_prs_fft_start_sample <= prs_fft_start_sample;
        out_timing_search_start_offset <= TIMING_SEARCH_START_OFFSET;
        out_timing_search_length <= TIMING_SEARCH_LENGTH;
        out_search_start_sample <= search_start_sample;
        out_search_end_sample <= search_end_sample;
        out_required_end_sample <= required_end_sample;
      end else if (out_ready) begin
        out_valid <= 1'b0;
      end
    end
  end
endmodule

`default_nettype wire
