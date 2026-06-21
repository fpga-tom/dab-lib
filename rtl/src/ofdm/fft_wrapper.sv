`default_nettype none

module fft_wrapper
  import dab_ofdm_pkg::*;
#(
    parameter int INPUT_WIDTH = DAB_OFDM_SAMPLE_WIDTH,
    parameter int OUTPUT_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int BLOCK_EXP_WIDTH = DAB_OFDM_BLOCK_EXP_WIDTH
) (
    input  logic clk,
    input  logic rst_n,
    input  logic clear,

    input  logic in_valid,
    output logic in_ready,
    input  logic signed [INPUT_WIDTH-1:0] in_i,
    input  logic signed [INPUT_WIDTH-1:0] in_q,
    input  logic [63:0] in_symbol_start_sample,

    output logic core_in_valid,
    input  logic core_in_ready,
    output logic signed [INPUT_WIDTH-1:0] core_in_i,
    output logic signed [INPUT_WIDTH-1:0] core_in_q,
    output logic core_in_last,

    input  logic core_out_valid,
    output logic core_out_ready,
    input  logic signed [OUTPUT_WIDTH-1:0] core_out_i,
    input  logic signed [OUTPUT_WIDTH-1:0] core_out_q,
    input  logic [BLOCK_EXP_WIDTH-1:0] core_out_block_exp,
    input  logic core_out_overflow,

    output logic out_valid,
    input  logic out_ready,
    output logic signed [OUTPUT_WIDTH-1:0] out_i,
    output logic signed [OUTPUT_WIDTH-1:0] out_q,
    output logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] out_fft_bin_index,
    output logic [63:0] out_symbol_start_sample,
    output logic [BLOCK_EXP_WIDTH-1:0] out_block_exp,
    output logic out_overflow,
    output logic out_symbol_done
);
  localparam logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] LAST_FFT_INDEX =
      DAB_OFDM_FFT_INDEX_WIDTH'(DAB_OFDM_FFT_SAMPLES - 1);

  logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] input_index_q;
  logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] output_index_q;
  logic [63:0] symbol_start_sample_q;
  logic symbol_active_q;
  logic input_done_q;

  logic input_fire;
  logic output_fire;
  logic input_last;
  logic output_last;
  logic input_completes;

  assign input_last = input_index_q == LAST_FFT_INDEX;
  assign output_last = output_index_q == LAST_FFT_INDEX;

  assign core_in_valid = !clear && !input_done_q && in_valid;
  assign in_ready = !clear && !input_done_q && core_in_ready;
  assign core_in_i = in_i;
  assign core_in_q = in_q;
  assign core_in_last = input_last;

  assign out_valid = !clear && symbol_active_q && core_out_valid;
  assign core_out_ready = !clear && symbol_active_q && out_ready;
  assign out_i = core_out_i;
  assign out_q = core_out_q;
  assign out_fft_bin_index = output_index_q;
  assign out_symbol_start_sample = symbol_start_sample_q;
  assign out_block_exp = core_out_block_exp;
  assign out_overflow = core_out_overflow;
  assign out_symbol_done = output_last;

  assign input_fire = core_in_valid && core_in_ready;
  assign output_fire = out_valid && out_ready;
  assign input_completes = input_fire && input_last;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      input_index_q <= '0;
      output_index_q <= '0;
      symbol_start_sample_q <= '0;
      symbol_active_q <= 1'b0;
      input_done_q <= 1'b0;
    end else if (clear) begin
      input_index_q <= '0;
      output_index_q <= '0;
      symbol_start_sample_q <= '0;
      symbol_active_q <= 1'b0;
      input_done_q <= 1'b0;
    end else begin
      if (input_fire) begin
        if (input_index_q == '0) begin
          symbol_start_sample_q <= in_symbol_start_sample;
          symbol_active_q <= 1'b1;
        end

        if (input_last) begin
          input_index_q <= '0;
          input_done_q <= 1'b1;
        end else begin
          input_index_q <= input_index_q + 1'b1;
        end
      end

      if (output_fire) begin
        if (output_last) begin
          output_index_q <= '0;

          if (input_done_q || input_completes) begin
            symbol_active_q <= 1'b0;
            input_done_q <= 1'b0;
          end
        end else begin
          output_index_q <= output_index_q + 1'b1;
        end
      end
    end
  end
endmodule

`default_nettype wire
