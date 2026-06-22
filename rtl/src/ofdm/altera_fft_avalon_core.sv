`default_nettype none

`ifndef DAB_ALTERA_FFT_IP_MODULE
`define DAB_ALTERA_FFT_IP_MODULE dab_fft_2048
`endif

module altera_fft_avalon_core #(
    parameter int INPUT_WIDTH = 18,
    parameter int OUTPUT_WIDTH = 24,
    parameter int BLOCK_EXP_WIDTH = 8,
    parameter int IP_INPUT_WIDTH = INPUT_WIDTH,
    parameter int IP_OUTPUT_WIDTH = OUTPUT_WIDTH,
    parameter int IP_EXP_WIDTH = BLOCK_EXP_WIDTH,
    parameter int IP_ERROR_WIDTH = 2,
    parameter bit INVERSE = 1'b0
) (
    input  logic clk,
    input  logic rst_n,
    input  logic clear,

    input  logic core_in_valid,
    output logic core_in_ready,
    input  logic signed [INPUT_WIDTH-1:0] core_in_i,
    input  logic signed [INPUT_WIDTH-1:0] core_in_q,
    input  logic core_in_last,

    output logic core_out_valid,
    input  logic core_out_ready,
    output logic signed [OUTPUT_WIDTH-1:0] core_out_i,
    output logic signed [OUTPUT_WIDTH-1:0] core_out_q,
    output logic [BLOCK_EXP_WIDTH-1:0] core_out_block_exp,
    output logic core_out_overflow
);
  logic sink_ready;
  logic source_valid;
  logic source_sop;
  logic source_eop;
  logic [IP_ERROR_WIDTH-1:0] source_error;
  logic signed [IP_OUTPUT_WIDTH-1:0] source_real;
  logic signed [IP_OUTPUT_WIDTH-1:0] source_imag;
  logic [IP_EXP_WIDTH-1:0] source_exp;

  logic [10:0] input_index_q;
  logic [10:0] output_index_q;
  logic input_fire;
  logic output_fire;
  logic sink_sop;
  logic source_frame_error;

  assign input_fire = core_in_valid && core_in_ready;
  assign output_fire = source_valid && core_out_ready && !clear;
  assign sink_sop = input_index_q == 11'd0;
  assign source_frame_error =
      source_valid &&
      (source_sop != (output_index_q == 11'd0) ||
       source_eop != (output_index_q == 11'd2047));
  assign core_in_ready = !clear && sink_ready;

  assign core_out_valid = !clear && source_valid;
  assign core_out_i = resize_output(source_real);
  assign core_out_q = resize_output(source_imag);
  assign core_out_block_exp = resize_exp(source_exp);
  assign core_out_overflow = |source_error || source_frame_error;

  function automatic logic signed [OUTPUT_WIDTH-1:0] resize_output(
      input logic signed [IP_OUTPUT_WIDTH-1:0] value
  );
    begin
      resize_output = $signed(value);
    end
  endfunction

  function automatic logic [BLOCK_EXP_WIDTH-1:0] resize_exp(
      input logic [IP_EXP_WIDTH-1:0] value
  );
    begin
      resize_exp = value;
    end
  endfunction

  function automatic logic signed [IP_INPUT_WIDTH-1:0] resize_input(
      input logic signed [INPUT_WIDTH-1:0] value
  );
    begin
      resize_input = $signed(value);
    end
  endfunction

  `DAB_ALTERA_FFT_IP_MODULE u_fft_ip (
      .clk(clk),
      .reset_n(rst_n && !clear),
      .sink_valid(core_in_valid && !clear),
      .sink_ready(sink_ready),
      .sink_error({IP_ERROR_WIDTH{1'b0}}),
      .sink_sop(sink_sop),
      .sink_eop(core_in_last),
      .sink_real(resize_input(core_in_i)),
      .sink_imag(resize_input(core_in_q)),
      .inverse(INVERSE),
      .source_valid(source_valid),
      .source_ready(core_out_ready && !clear),
      .source_error(source_error),
      .source_sop(source_sop),
      .source_eop(source_eop),
      .source_real(source_real),
      .source_imag(source_imag),
      .source_exp(source_exp)
  );

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      input_index_q <= '0;
      output_index_q <= '0;
    end else if (clear) begin
      input_index_q <= '0;
      output_index_q <= '0;
    end else begin
      if (input_fire) begin
        if (core_in_last) begin
          input_index_q <= '0;
        end else begin
          input_index_q <= input_index_q + 1'b1;
        end
      end

      if (output_fire) begin
        if (source_eop) begin
          output_index_q <= '0;
        end else begin
          output_index_q <= output_index_q + 1'b1;
        end
      end
    end
  end
endmodule

`default_nettype wire
