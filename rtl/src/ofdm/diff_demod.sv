`default_nettype none

module diff_demod
  import dab_ofdm_pkg::*;
#(
    parameter int CARRIER_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int DIFF_SHIFT = CARRIER_WIDTH - 1
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic in_valid,
    output logic in_ready,
    input  wire logic signed [CARRIER_WIDTH-1:0] in_i,
    input  wire logic signed [CARRIER_WIDTH-1:0] in_q,
    input  wire logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] in_active_index,
    input  wire logic [63:0] in_symbol_start_sample,
    input  wire logic in_symbol_done,
    input  wire logic in_seed_only,

    output logic out_valid,
    input  wire logic out_ready,
    output logic signed [CARRIER_WIDTH-1:0] out_i,
    output logic signed [CARRIER_WIDTH-1:0] out_q,
    output logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] out_active_index,
    output logic [63:0] out_symbol_start_sample,
    output logic out_symbol_done
);
  localparam logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] NULL_ACTIVE_INDEX =
      DAB_OFDM_ACTIVE_INDEX_WIDTH'(DAB_OFDM_ACTIVE_CARRIER_HALF);

  logic have_prev_q;
  logic stage_valid_q;
  logic stage_have_prev_q;
  logic stage_seed_only_q;
  logic signed [CARRIER_WIDTH-1:0] stage_i_q;
  logic signed [CARRIER_WIDTH-1:0] stage_q_q;
  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] stage_active_index_q;
  logic [63:0] stage_symbol_start_sample_q;
  logic stage_symbol_done_q;
  logic input_fire;
  logic output_can_advance;
  logic stage_can_accept;

  (* ram_style = "block", ramstyle = "M10K" *)
  logic signed [CARRIER_WIDTH-1:0] prev_i_mem [0:DAB_OFDM_ACTIVE_CARRIERS-1];
  (* ram_style = "block", ramstyle = "M10K" *)
  logic signed [CARRIER_WIDTH-1:0] prev_q_mem [0:DAB_OFDM_ACTIVE_CARRIERS-1];

  logic signed [CARRIER_WIDTH-1:0] prev_i_rd_q;
  logic signed [CARRIER_WIDTH-1:0] prev_q_rd_q;
  logic signed [CARRIER_WIDTH-1:0] diff_i;
  logic signed [CARRIER_WIDTH-1:0] diff_q;

  assign output_can_advance = !out_valid || out_ready;
  assign stage_can_accept = !stage_valid_q || output_can_advance;
  assign in_ready = !clear && stage_can_accept;
  assign input_fire = in_valid && in_ready;

  assign diff_i = diff_component_i(stage_i_q,
                                   stage_q_q,
                                   prev_i_rd_q,
                                   prev_q_rd_q,
                                   stage_active_index_q);
  assign diff_q = diff_component_q(stage_i_q,
                                   stage_q_q,
                                   prev_i_rd_q,
                                   prev_q_rd_q,
                                   stage_active_index_q);

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

  always_ff @(posedge clk) begin
    if (input_fire) begin
      prev_i_rd_q <= prev_i_mem[in_active_index];
      prev_q_rd_q <= prev_q_mem[in_active_index];
      prev_i_mem[in_active_index] <= in_i;
      prev_q_mem[in_active_index] <= in_q;
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      have_prev_q <= 1'b0;
      stage_valid_q <= 1'b0;
      stage_have_prev_q <= 1'b0;
      stage_seed_only_q <= 1'b0;
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
      have_prev_q <= 1'b0;
      stage_valid_q <= 1'b0;
      stage_have_prev_q <= 1'b0;
      stage_seed_only_q <= 1'b0;
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
          if (stage_have_prev_q && !stage_seed_only_q) begin
            out_valid <= 1'b1;
            out_i <= diff_i;
            out_q <= diff_q;
            out_active_index <= stage_active_index_q;
            out_symbol_start_sample <= stage_symbol_start_sample_q;
            out_symbol_done <= stage_symbol_done_q;
          end
          stage_valid_q <= 1'b0;
        end
      end

      if (input_fire) begin
        stage_valid_q <= 1'b1;
        stage_have_prev_q <= have_prev_q;
        stage_seed_only_q <= in_seed_only;
        stage_i_q <= in_i;
        stage_q_q <= in_q;
        stage_active_index_q <= in_active_index;
        stage_symbol_start_sample_q <= in_symbol_start_sample;
        stage_symbol_done_q <= in_symbol_done;

        if (in_symbol_done) begin
          have_prev_q <= 1'b1;
        end
      end
    end
  end
endmodule

`default_nettype wire
