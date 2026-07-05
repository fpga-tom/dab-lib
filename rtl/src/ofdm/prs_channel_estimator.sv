`default_nettype none

module prs_channel_estimator
  import dab_ofdm_pkg::*;
#(
    parameter int CARRIER_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int COEFF_WIDTH = DAB_OFDM_COEFF_WIDTH,
    parameter int COEFF_FRAC_BITS = DAB_OFDM_COEFF_FRAC_BITS,
    parameter int ENABLE_COEFF_OUTPUT = 1
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic in_valid,
    output logic in_ready,
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire logic signed [CARRIER_WIDTH-1:0] in_i,
    input  wire logic signed [CARRIER_WIDTH-1:0] in_q,
    /* verilator lint_on UNUSEDSIGNAL */
    input  wire logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] in_active_index,
    input  wire logic [63:0] in_symbol_start_sample,
    input  wire logic in_symbol_done,

    output logic coeff_valid,
    input  wire logic coeff_ready,
    output logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] coeff_index,
    output logic signed [COEFF_WIDTH-1:0] coeff_i,
    output logic signed [COEFF_WIDTH-1:0] coeff_q,
    output logic [63:0] coeff_symbol_start_sample,
    output logic coeff_symbol_done,

    output logic ref_valid,
    input  wire logic ref_ready,
    output logic signed [CARRIER_WIDTH-1:0] ref_i,
    output logic signed [CARRIER_WIDTH-1:0] ref_q,
    output logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] ref_active_index,
    output logic [63:0] ref_symbol_start_sample,
    output logic ref_symbol_done
);
  localparam bit COEFF_OUTPUT_ENABLED = ENABLE_COEFF_OUTPUT != 0;
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

  assign pending_coeff = COEFF_OUTPUT_ENABLED && coeff_valid && !coeff_ready;
  assign pending_ref = ref_valid && !ref_ready;
  assign in_ready = !clear && !pending_coeff && !pending_ref;
  assign input_fire = in_valid && in_ready;

  assign prs_ref_i = prs_reference_i(in_active_index);
  assign prs_ref_q = prs_reference_q(in_active_index);

  generate
    if (ENABLE_COEFF_OUTPUT != 0) begin : gen_coeff_estimate
      assign estimated_coeff_i = estimate_coeff_i(in_i,
                                                  in_q,
                                                  prs_ref_i,
                                                  prs_ref_q,
                                                  in_active_index);
      assign estimated_coeff_q = estimate_coeff_q(in_i,
                                                  in_q,
                                                  prs_ref_i,
                                                  prs_ref_q,
                                                  in_active_index);
    end else begin : gen_ref_only
      assign estimated_coeff_i = '0;
      assign estimated_coeff_q = '0;
    end
  endgenerate

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

  function automatic int unsigned bit_length_u128(input logic [127:0] value);
    integer bit_index;
    begin
      bit_length_u128 = 0;
      for (bit_index = 0; bit_index < 128; bit_index = bit_index + 1) begin
        if (value[bit_index]) begin
          bit_length_u128 = bit_index + 1;
        end
      end
    end
  endfunction

  function automatic logic signed [63:0] scaled_s128_to_s64(
      input logic signed [127:0] value,
      input int unsigned scale_shift
  );
    /* verilator lint_off UNUSEDSIGNAL */
    logic signed [127:0] shifted;
    /* verilator lint_on UNUSEDSIGNAL */
    begin
      shifted = value >>> scale_shift;
      scaled_s128_to_s64 = shifted[63:0];
    end
  endfunction

  function automatic logic signed [COEFF_WIDTH-1:0] divide_scaled_coeff(
      input logic signed [95:0] numerator,
      input logic signed [63:0] denominator
  );
    logic signed [127:0] scaled;
    logic signed [127:0] denom_wide;
    logic [127:0] scaled_abs;
    logic [127:0] denom_abs;
    logic signed [63:0] scaled_64;
    logic signed [63:0] denom_64;
    logic signed [63:0] quotient_64;
    logic signed [95:0] quotient;
    int unsigned numerator_bits;
    int unsigned denominator_bits;
    int unsigned max_bits;
    int unsigned scale_shift;
    begin
      if (denominator <= 64'sd0) begin
        divide_scaled_coeff = '0;
      end else begin
        scaled = {{32{numerator[95]}}, numerator} <<< COEFF_FRAC_BITS;
        denom_wide = {{64{denominator[63]}}, denominator};
        scaled_abs = scaled[127] ? (~scaled + 128'd1) : scaled;
        denom_abs = denom_wide[127] ? (~denom_wide + 128'd1) : denom_wide;
        numerator_bits = bit_length_u128(scaled_abs);
        denominator_bits = bit_length_u128(denom_abs);
        max_bits = numerator_bits > denominator_bits ? numerator_bits : denominator_bits;
        scale_shift = max_bits > 63 ? max_bits - 63 : 0;
        scaled_64 = scaled_s128_to_s64(scaled, scale_shift);
        denom_64 = scaled_s128_to_s64(denom_wide, scale_shift);
        if (denom_64 == 64'sd0) begin
          quotient = scaled[127] ? -(96'sd1 <<< (COEFF_WIDTH - 1))
                                 : ((96'sd1 <<< (COEFF_WIDTH - 1)) - 96'sd1);
        end else begin
          quotient_64 = scaled_64 / denom_64;
          quotient = {{32{quotient_64[63]}}, quotient_64};
        end
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
        if (COEFF_OUTPUT_ENABLED) begin
          coeff_valid <= 1'b1;
          coeff_index <= in_active_index;
          coeff_i <= estimated_coeff_i;
          coeff_q <= estimated_coeff_q;
          coeff_symbol_start_sample <= in_symbol_start_sample;
          coeff_symbol_done <= in_symbol_done;
        end else begin
          coeff_valid <= 1'b0;
          coeff_index <= '0;
          coeff_i <= '0;
          coeff_q <= '0;
          coeff_symbol_start_sample <= '0;
          coeff_symbol_done <= 1'b0;
        end

        ref_valid <= 1'b1;
        ref_i <= prs_ref_i;
        ref_q <= prs_ref_q;
        ref_active_index <= in_active_index;
        ref_symbol_start_sample <= in_symbol_start_sample;
        ref_symbol_done <= in_symbol_done;
      end else begin
        if (COEFF_OUTPUT_ENABLED && coeff_ready) begin
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
