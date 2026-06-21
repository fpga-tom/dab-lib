`default_nettype none

module prs_channel_estimator
  import dab_ofdm_pkg::*;
#(
    parameter int CARRIER_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int COEFF_WIDTH = DAB_OFDM_COEFF_WIDTH,
    parameter int COEFF_FRAC_BITS = DAB_OFDM_COEFF_FRAC_BITS
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

    output logic coeff_valid,
    input  logic coeff_ready,
    output logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] coeff_index,
    output logic signed [COEFF_WIDTH-1:0] coeff_i,
    output logic signed [COEFF_WIDTH-1:0] coeff_q,
    output logic [63:0] coeff_symbol_start_sample,
    output logic coeff_symbol_done,

    output logic ref_valid,
    input  logic ref_ready,
    output logic signed [CARRIER_WIDTH-1:0] ref_i,
    output logic signed [CARRIER_WIDTH-1:0] ref_q,
    output logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] ref_active_index,
    output logic [63:0] ref_symbol_start_sample,
    output logic ref_symbol_done
);
  localparam logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] NULL_ACTIVE_INDEX =
      DAB_OFDM_ACTIVE_INDEX_WIDTH'(DAB_OFDM_ACTIVE_CARRIER_HALF);
  localparam logic signed [CARRIER_WIDTH-1:0] PRS_REF_SCALE =
      CARRIER_WIDTH'(1) <<< COEFF_FRAC_BITS;

  localparam logic [63:0] PRS_H_ROW_0 = 64'h5a0250085a025008;
  localparam logic [63:0] PRS_H_ROW_1 = 64'h3ee634ec3ee634ec;
  localparam logic [63:0] PRS_H_ROW_2 = 64'hd28ad880d28ad880;
  localparam logic [63:0] PRS_H_ROW_3 = 64'hb66ebc64b66ebc64;

  logic input_fire;
  logic pending_coeff;
  logic pending_ref;

  logic signed [CARRIER_WIDTH-1:0] prs_ref_i;
  logic signed [CARRIER_WIDTH-1:0] prs_ref_q;
  logic signed [COEFF_WIDTH-1:0] estimated_coeff_i;
  logic signed [COEFF_WIDTH-1:0] estimated_coeff_q;

  assign pending_coeff = coeff_valid && !coeff_ready;
  assign pending_ref = ref_valid && !ref_ready;
  assign in_ready = !clear && !pending_coeff && !pending_ref;
  assign input_fire = in_valid && in_ready;

  assign prs_ref_i = prs_reference_i(in_active_index);
  assign prs_ref_q = prs_reference_q(in_active_index);
  assign estimated_coeff_i = estimate_coeff_i(in_i, in_q, prs_ref_i, prs_ref_q, in_active_index);
  assign estimated_coeff_q = estimate_coeff_q(in_i, in_q, prs_ref_i, prs_ref_q, in_active_index);

  function automatic logic [1:0] phase_from_row(
      input logic [1:0] row,
      input logic [4:0] offset
  );
    logic [63:0] packed_row;
    logic [5:0] shift;
    begin
      case (row)
        2'd0: packed_row = PRS_H_ROW_0;
        2'd1: packed_row = PRS_H_ROW_1;
        2'd2: packed_row = PRS_H_ROW_2;
        default: packed_row = PRS_H_ROW_3;
      endcase
      shift = {offset, 1'b0};
      phase_from_row = 2'(packed_row >> shift);
    end
  endfunction

  function automatic logic [1:0] positive_prs_row(input logic [1:0] group_mod);
    begin
      case (group_mod)
        2'd0: positive_prs_row = 2'd0;
        2'd1: positive_prs_row = 2'd3;
        2'd2: positive_prs_row = 2'd2;
        default: positive_prs_row = 2'd1;
      endcase
    end
  endfunction

  function automatic logic [1:0] prs_phase(
      input logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index
  );
    logic [1:0] row;
    logic [4:0] offset;
    begin
      if (active_index < NULL_ACTIVE_INDEX) begin
        row = active_index[6:5];
        offset = active_index[4:0];
        prs_phase = phase_from_row(row, offset);
      end else if (active_index > NULL_ACTIVE_INDEX) begin
        row = positive_prs_row(2'((active_index - (NULL_ACTIVE_INDEX + 1'b1)) >> 5));
        offset = 5'(active_index - (NULL_ACTIVE_INDEX + 1'b1));
        prs_phase = phase_from_row(row, offset);
      end else begin
        prs_phase = 2'd0;
      end
    end
  endfunction

  function automatic logic signed [CARRIER_WIDTH-1:0] prs_reference_i(
      input logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index
  );
    begin
      if (active_index == NULL_ACTIVE_INDEX) begin
        prs_reference_i = '0;
      end else begin
        case (prs_phase(active_index))
          2'd0: prs_reference_i = PRS_REF_SCALE;
          2'd2: prs_reference_i = -PRS_REF_SCALE;
          default: prs_reference_i = '0;
        endcase
      end
    end
  endfunction

  function automatic logic signed [CARRIER_WIDTH-1:0] prs_reference_q(
      input logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index
  );
    begin
      if (active_index == NULL_ACTIVE_INDEX) begin
        prs_reference_q = '0;
      end else begin
        case (prs_phase(active_index))
          2'd1: prs_reference_q = PRS_REF_SCALE;
          2'd3: prs_reference_q = -PRS_REF_SCALE;
          default: prs_reference_q = '0;
        endcase
      end
    end
  endfunction

  function automatic logic signed [63:0] sign_extend_carrier(
      input logic signed [CARRIER_WIDTH-1:0] value
  );
    begin
      sign_extend_carrier = {{(64 - CARRIER_WIDTH){value[CARRIER_WIDTH-1]}}, value};
    end
  endfunction

  function automatic logic signed [COEFF_WIDTH-1:0] saturate_coeff(
      input logic signed [95:0] value
  );
    logic signed [95:0] max_value;
    logic signed [95:0] min_value;
    begin
      max_value = (96'sd1 <<< (COEFF_WIDTH - 1)) - 96'sd1;
      min_value = -(96'sd1 <<< (COEFF_WIDTH - 1));

      if (value > max_value) begin
        saturate_coeff = max_value[COEFF_WIDTH-1:0];
      end else if (value < min_value) begin
        saturate_coeff = min_value[COEFF_WIDTH-1:0];
      end else begin
        saturate_coeff = value[COEFF_WIDTH-1:0];
      end
    end
  endfunction

  function automatic logic signed [COEFF_WIDTH-1:0] divide_scaled_coeff(
      input logic signed [95:0] numerator,
      input logic signed [63:0] denominator
  );
    logic signed [95:0] scaled;
    logic signed [95:0] denom_wide;
    logic signed [95:0] quotient;
    begin
      if (denominator <= 64'sd0) begin
        divide_scaled_coeff = '0;
      end else begin
        scaled = numerator <<< COEFF_FRAC_BITS;
        denom_wide = {{32{denominator[63]}}, denominator};
        quotient = scaled / denom_wide;
        divide_scaled_coeff = saturate_coeff(quotient);
      end
    end
  endfunction

  function automatic logic signed [COEFF_WIDTH-1:0] estimate_coeff_i(
      input logic signed [CARRIER_WIDTH-1:0] sym_i,
      input logic signed [CARRIER_WIDTH-1:0] sym_q,
      input logic signed [CARRIER_WIDTH-1:0] ref_re,
      input logic signed [CARRIER_WIDTH-1:0] ref_im,
      input logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index
  );
    logic signed [63:0] sym_re_wide;
    logic signed [63:0] sym_im_wide;
    logic signed [63:0] ref_re_wide;
    logic signed [63:0] ref_im_wide;
    logic signed [63:0] denominator;
    logic signed [95:0] numerator;
    begin
      if (active_index == NULL_ACTIVE_INDEX) begin
        estimate_coeff_i = '0;
      end else begin
        sym_re_wide = sign_extend_carrier(sym_i);
        sym_im_wide = sign_extend_carrier(sym_q);
        ref_re_wide = sign_extend_carrier(ref_re);
        ref_im_wide = sign_extend_carrier(ref_im);
        denominator = (sym_re_wide * sym_re_wide) + (sym_im_wide * sym_im_wide);
        numerator = (ref_re_wide * sym_re_wide) + (ref_im_wide * sym_im_wide);
        estimate_coeff_i = divide_scaled_coeff(numerator, denominator);
      end
    end
  endfunction

  function automatic logic signed [COEFF_WIDTH-1:0] estimate_coeff_q(
      input logic signed [CARRIER_WIDTH-1:0] sym_i,
      input logic signed [CARRIER_WIDTH-1:0] sym_q,
      input logic signed [CARRIER_WIDTH-1:0] ref_re,
      input logic signed [CARRIER_WIDTH-1:0] ref_im,
      input logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index
  );
    logic signed [63:0] sym_re_wide;
    logic signed [63:0] sym_im_wide;
    logic signed [63:0] ref_re_wide;
    logic signed [63:0] ref_im_wide;
    logic signed [63:0] denominator;
    logic signed [95:0] numerator;
    begin
      if (active_index == NULL_ACTIVE_INDEX) begin
        estimate_coeff_q = '0;
      end else begin
        sym_re_wide = sign_extend_carrier(sym_i);
        sym_im_wide = sign_extend_carrier(sym_q);
        ref_re_wide = sign_extend_carrier(ref_re);
        ref_im_wide = sign_extend_carrier(ref_im);
        denominator = (sym_re_wide * sym_re_wide) + (sym_im_wide * sym_im_wide);
        numerator = (ref_im_wide * sym_re_wide) - (ref_re_wide * sym_im_wide);
        estimate_coeff_q = divide_scaled_coeff(numerator, denominator);
      end
    end
  endfunction

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      coeff_valid <= 1'b0;
      coeff_index <= '0;
      coeff_i <= '0;
      coeff_q <= '0;
      coeff_symbol_start_sample <= '0;
      coeff_symbol_done <= 1'b0;
      ref_valid <= 1'b0;
      ref_i <= '0;
      ref_q <= '0;
      ref_active_index <= '0;
      ref_symbol_start_sample <= '0;
      ref_symbol_done <= 1'b0;
    end else if (clear) begin
      coeff_valid <= 1'b0;
      coeff_index <= '0;
      coeff_i <= '0;
      coeff_q <= '0;
      coeff_symbol_start_sample <= '0;
      coeff_symbol_done <= 1'b0;
      ref_valid <= 1'b0;
      ref_i <= '0;
      ref_q <= '0;
      ref_active_index <= '0;
      ref_symbol_start_sample <= '0;
      ref_symbol_done <= 1'b0;
    end else begin
      if (input_fire) begin
        coeff_valid <= 1'b1;
        coeff_index <= in_active_index;
        coeff_i <= estimated_coeff_i;
        coeff_q <= estimated_coeff_q;
        coeff_symbol_start_sample <= in_symbol_start_sample;
        coeff_symbol_done <= in_symbol_done;

        ref_valid <= 1'b1;
        ref_i <= prs_ref_i;
        ref_q <= prs_ref_q;
        ref_active_index <= in_active_index;
        ref_symbol_start_sample <= in_symbol_start_sample;
        ref_symbol_done <= in_symbol_done;
      end else begin
        if (coeff_ready) begin
          coeff_valid <= 1'b0;
          coeff_symbol_done <= 1'b0;
        end
        if (ref_ready) begin
          ref_valid <= 1'b0;
          ref_symbol_done <= 1'b0;
        end
      end
    end
  end
endmodule

`default_nettype wire
