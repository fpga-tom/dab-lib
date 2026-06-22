`default_nettype none

module equalizer
  import dab_ofdm_pkg::*;
#(
    parameter int CARRIER_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int COEFF_WIDTH = DAB_OFDM_COEFF_WIDTH,
    parameter int COEFF_FRAC_BITS = DAB_OFDM_COEFF_FRAC_BITS
) (
    input  logic clk,
    input  logic rst_n,
    input  logic clear,

    input  logic coeff_we,
    input  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] coeff_index,
    input  logic signed [COEFF_WIDTH-1:0] coeff_i,
    input  logic signed [COEFF_WIDTH-1:0] coeff_q,

    input  logic in_valid,
    output logic in_ready,
    input  logic signed [CARRIER_WIDTH-1:0] in_i,
    input  logic signed [CARRIER_WIDTH-1:0] in_q,
    input  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] in_active_index,
    input  logic [63:0] in_symbol_start_sample,
    input  logic in_symbol_done,

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

  logic stage_valid_q;
  logic signed [CARRIER_WIDTH-1:0] stage_i_q;
  logic signed [CARRIER_WIDTH-1:0] stage_q_q;
  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] stage_active_index_q;
  logic [63:0] stage_symbol_start_sample_q;
  logic stage_symbol_done_q;
  logic input_fire;
  logic output_can_advance;
  logic stage_can_accept;

  logic signed [COEFF_WIDTH-1:0] channel_i_mem [0:DAB_OFDM_ACTIVE_CARRIERS-1];
  logic signed [COEFF_WIDTH-1:0] channel_q_mem [0:DAB_OFDM_ACTIVE_CARRIERS-1];

  logic signed [COEFF_WIDTH-1:0] channel_i_rd_q;
  logic signed [COEFF_WIDTH-1:0] channel_q_rd_q;
  logic signed [CARRIER_WIDTH-1:0] eq_i;
  logic signed [CARRIER_WIDTH-1:0] eq_q;

  assign output_can_advance = !out_valid || out_ready;
  assign stage_can_accept = !stage_valid_q || output_can_advance;
  assign in_ready = !clear && stage_can_accept;
  assign input_fire = in_valid && in_ready;

  assign eq_i = equalized_i(stage_i_q,
                            stage_q_q,
                            channel_i_rd_q,
                            channel_q_rd_q,
                            stage_active_index_q);
  assign eq_q = equalized_q(stage_i_q,
                            stage_q_q,
                            channel_i_rd_q,
                            channel_q_rd_q,
                            stage_active_index_q);

  function automatic logic signed [63:0] sign_extend_carrier(
      input logic signed [CARRIER_WIDTH-1:0] value
  );
    begin
      sign_extend_carrier = {{(64 - CARRIER_WIDTH){value[CARRIER_WIDTH-1]}}, value};
    end
  endfunction

  function automatic logic signed [63:0] sign_extend_coeff(
      input logic signed [COEFF_WIDTH-1:0] value
  );
    begin
      sign_extend_coeff = {{(64 - COEFF_WIDTH){value[COEFF_WIDTH-1]}}, value};
    end
  endfunction

  function automatic logic signed [CARRIER_WIDTH-1:0] scale_and_saturate(
      input logic signed [63:0] value
  );
    logic signed [63:0] scaled;
    logic signed [63:0] max_value;
    logic signed [63:0] min_value;
    begin
      if (COEFF_FRAC_BITS == 0) begin
        scaled = value;
      end else begin
        scaled = value >>> COEFF_FRAC_BITS;
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

  function automatic logic signed [CARRIER_WIDTH-1:0] equalized_i(
      input logic signed [CARRIER_WIDTH-1:0] carrier_i,
      input logic signed [CARRIER_WIDTH-1:0] carrier_q,
      input logic signed [COEFF_WIDTH-1:0] coeff_re,
      input logic signed [COEFF_WIDTH-1:0] coeff_im,
      input logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index
  );
    logic signed [63:0] a_re;
    logic signed [63:0] a_im;
    logic signed [63:0] c_re;
    logic signed [63:0] c_im;
    logic signed [63:0] product;
    begin
      if (active_index == NULL_ACTIVE_INDEX) begin
        equalized_i = '0;
      end else begin
        a_re = sign_extend_carrier(carrier_i);
        a_im = sign_extend_carrier(carrier_q);
        c_re = sign_extend_coeff(coeff_re);
        c_im = sign_extend_coeff(coeff_im);
        product = (a_re * c_re) - (a_im * c_im);
        equalized_i = scale_and_saturate(product);
      end
    end
  endfunction

  function automatic logic signed [CARRIER_WIDTH-1:0] equalized_q(
      input logic signed [CARRIER_WIDTH-1:0] carrier_i,
      input logic signed [CARRIER_WIDTH-1:0] carrier_q,
      input logic signed [COEFF_WIDTH-1:0] coeff_re,
      input logic signed [COEFF_WIDTH-1:0] coeff_im,
      input logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index
  );
    logic signed [63:0] a_re;
    logic signed [63:0] a_im;
    logic signed [63:0] c_re;
    logic signed [63:0] c_im;
    logic signed [63:0] product;
    begin
      if (active_index == NULL_ACTIVE_INDEX) begin
        equalized_q = '0;
      end else begin
        a_re = sign_extend_carrier(carrier_i);
        a_im = sign_extend_carrier(carrier_q);
        c_re = sign_extend_coeff(coeff_re);
        c_im = sign_extend_coeff(coeff_im);
        product = (a_re * c_im) + (a_im * c_re);
        equalized_q = scale_and_saturate(product);
      end
    end
  endfunction

  always_ff @(posedge clk) begin
    if (coeff_we) begin
      channel_i_mem[coeff_index] <= coeff_i;
      channel_q_mem[coeff_index] <= coeff_q;
    end

    if (input_fire) begin
      channel_i_rd_q <= channel_i_mem[in_active_index];
      channel_q_rd_q <= channel_q_mem[in_active_index];
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      stage_valid_q <= 1'b0;
      stage_i_q <= '0;
      stage_q_q <= '0;
      stage_active_index_q <= '0;
      stage_symbol_start_sample_q <= '0;
      stage_symbol_done_q <= 1'b0;
      out_valid <= 1'b0;
      out_i <= '0;
      out_q <= '0;
      out_active_index <= '0;
      out_symbol_start_sample <= '0;
      out_symbol_done <= 1'b0;
    end else if (clear) begin
      stage_valid_q <= 1'b0;
      stage_i_q <= '0;
      stage_q_q <= '0;
      stage_active_index_q <= '0;
      stage_symbol_start_sample_q <= '0;
      stage_symbol_done_q <= 1'b0;
      out_valid <= 1'b0;
      out_i <= '0;
      out_q <= '0;
      out_active_index <= '0;
      out_symbol_start_sample <= '0;
      out_symbol_done <= 1'b0;
    end else begin
      if (output_can_advance) begin
        out_valid <= 1'b0;
        out_symbol_done <= 1'b0;

        if (stage_valid_q) begin
          out_valid <= 1'b1;
          out_i <= eq_i;
          out_q <= eq_q;
          out_active_index <= stage_active_index_q;
          out_symbol_start_sample <= stage_symbol_start_sample_q;
          out_symbol_done <= stage_symbol_done_q;
          stage_valid_q <= 1'b0;
        end
      end

      if (input_fire) begin
        stage_valid_q <= 1'b1;
        stage_i_q <= in_i;
        stage_q_q <= in_q;
        stage_active_index_q <= in_active_index;
        stage_symbol_start_sample_q <= in_symbol_start_sample;
        stage_symbol_done_q <= in_symbol_done;
      end
    end
  end
endmodule

`default_nettype wire
