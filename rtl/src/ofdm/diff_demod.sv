`default_nettype none

module diff_demod
  import dab_ofdm_pkg::*;
#(
    parameter int CARRIER_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int DIFF_SHIFT = CARRIER_WIDTH - 1
) (
    input  logic clk,
    input  logic rst_n,
    input  logic clear,

    input  logic in_valid,
    output logic in_ready,
    input  logic signed [CARRIER_WIDTH-1:0] in_i,
    input  logic signed [CARRIER_WIDTH-1:0] in_q,
    input  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] in_active_index,
    input  logic [63:0] in_symbol_start_sample,
    input  logic in_symbol_done,
    input  logic in_seed_only,

    output logic out_valid,
    input  logic out_ready,
    output logic signed [CARRIER_WIDTH-1:0] out_i,
    output logic signed [CARRIER_WIDTH-1:0] out_q,
    output logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] out_active_index,
    output logic [63:0] out_symbol_start_sample,
    output logic out_symbol_done
);
  localparam logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] NULL_ACTIVE_INDEX =
      DAB_OFDM_ACTIVE_INDEX_WIDTH'(DAB_OFDM_ACTIVE_CARRIER_HALF);

  logic have_prev_q;
  logic input_fire;
  logic pending_output;

  logic signed [CARRIER_WIDTH-1:0] prev_i_mem [0:DAB_OFDM_ACTIVE_CARRIERS-1];
  logic signed [CARRIER_WIDTH-1:0] prev_q_mem [0:DAB_OFDM_ACTIVE_CARRIERS-1];

  logic signed [CARRIER_WIDTH-1:0] prev_i;
  logic signed [CARRIER_WIDTH-1:0] prev_q;
  logic signed [CARRIER_WIDTH-1:0] diff_i;
  logic signed [CARRIER_WIDTH-1:0] diff_q;

  assign pending_output = out_valid && !out_ready;
  assign in_ready = !pending_output;
  assign input_fire = in_valid && in_ready;

  assign prev_i = prev_i_mem[in_active_index];
  assign prev_q = prev_q_mem[in_active_index];

  assign diff_i = diff_component_i(in_i, in_q, prev_i, prev_q, in_active_index);
  assign diff_q = diff_component_q(in_i, in_q, prev_i, prev_q, in_active_index);

  function automatic logic signed [63:0] sign_extend_carrier(
      input logic signed [CARRIER_WIDTH-1:0] value
  );
    begin
      sign_extend_carrier = {{(64 - CARRIER_WIDTH){value[CARRIER_WIDTH-1]}}, value};
    end
  endfunction

  function automatic logic signed [CARRIER_WIDTH-1:0] scale_and_saturate(
      input logic signed [63:0] value
  );
    logic signed [63:0] scaled;
    logic signed [63:0] max_value;
    logic signed [63:0] min_value;
    begin
      if (DIFF_SHIFT == 0) begin
        scaled = value;
      end else begin
        scaled = value >>> DIFF_SHIFT;
      end

      max_value = (64'sd1 <<< (CARRIER_WIDTH - 1)) - 64'sd1;
      min_value = -(64'sd1 <<< (CARRIER_WIDTH - 1));

      if (scaled > max_value) begin
        scale_and_saturate = max_value[CARRIER_WIDTH-1:0];
      end else if (scaled < min_value) begin
        scale_and_saturate = min_value[CARRIER_WIDTH-1:0];
      end else begin
        scale_and_saturate = scaled[CARRIER_WIDTH-1:0];
      end
    end
  endfunction

  function automatic logic signed [CARRIER_WIDTH-1:0] diff_component_i(
      input logic signed [CARRIER_WIDTH-1:0] curr_i,
      input logic signed [CARRIER_WIDTH-1:0] curr_q,
      input logic signed [CARRIER_WIDTH-1:0] prev_re,
      input logic signed [CARRIER_WIDTH-1:0] prev_im,
      input logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index
  );
    logic signed [63:0] a_re;
    logic signed [63:0] a_im;
    logic signed [63:0] b_re;
    logic signed [63:0] b_im;
    logic signed [63:0] product;
    begin
      if (active_index == NULL_ACTIVE_INDEX) begin
        diff_component_i = '0;
      end else begin
        a_re = sign_extend_carrier(curr_i);
        a_im = sign_extend_carrier(curr_q);
        b_re = sign_extend_carrier(prev_re);
        b_im = sign_extend_carrier(prev_im);
        product = (a_re * b_re) + (a_im * b_im);
        diff_component_i = scale_and_saturate(product);
      end
    end
  endfunction

  function automatic logic signed [CARRIER_WIDTH-1:0] diff_component_q(
      input logic signed [CARRIER_WIDTH-1:0] curr_i,
      input logic signed [CARRIER_WIDTH-1:0] curr_q,
      input logic signed [CARRIER_WIDTH-1:0] prev_re,
      input logic signed [CARRIER_WIDTH-1:0] prev_im,
      input logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index
  );
    logic signed [63:0] a_re;
    logic signed [63:0] a_im;
    logic signed [63:0] b_re;
    logic signed [63:0] b_im;
    logic signed [63:0] product;
    begin
      if (active_index == NULL_ACTIVE_INDEX) begin
        diff_component_q = '0;
      end else begin
        a_re = sign_extend_carrier(curr_i);
        a_im = sign_extend_carrier(curr_q);
        b_re = sign_extend_carrier(prev_re);
        b_im = sign_extend_carrier(prev_im);
        product = (a_im * b_re) - (a_re * b_im);
        diff_component_q = scale_and_saturate(product);
      end
    end
  endfunction

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      have_prev_q <= 1'b0;
      out_valid <= 1'b0;
      out_i <= '0;
      out_q <= '0;
      out_active_index <= '0;
      out_symbol_start_sample <= '0;
      out_symbol_done <= 1'b0;
    end else if (clear) begin
      have_prev_q <= 1'b0;
      out_valid <= 1'b0;
      out_i <= '0;
      out_q <= '0;
      out_active_index <= '0;
      out_symbol_start_sample <= '0;
      out_symbol_done <= 1'b0;
    end else begin
      if (input_fire) begin
        if (have_prev_q && !in_seed_only) begin
          out_valid <= 1'b1;
          out_i <= diff_i;
          out_q <= diff_q;
          out_active_index <= in_active_index;
          out_symbol_start_sample <= in_symbol_start_sample;
          out_symbol_done <= in_symbol_done;
        end else begin
          out_valid <= 1'b0;
          out_symbol_done <= 1'b0;
        end

        prev_i_mem[in_active_index] <= in_i;
        prev_q_mem[in_active_index] <= in_q;

        if (in_symbol_done) begin
          have_prev_q <= 1'b1;
        end
      end else if (out_ready) begin
        out_valid <= 1'b0;
        out_symbol_done <= 1'b0;
      end
    end
  end
endmodule

`default_nettype wire
