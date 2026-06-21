`default_nettype none

module prs_pilot_refiner
  import dab_ofdm_pkg::*;
#(
    parameter int CARRIER_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int SCORE_WIDTH = 32,
    parameter int SCORE_FRAC_BITS = 30,
    parameter int TIMING_WIDTH = 24,
    parameter int TIMING_GAIN_Q = 1335091,
    parameter int MAX_OFFSET_BINS = 4
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

    output logic out_valid,
    input  logic out_ready,
    output logic out_found,
    output logic [63:0] out_symbol_start_sample,
    output logic [SCORE_WIDTH-1:0] out_score,
    output logic signed [3:0] out_carrier_offset_bins,
    output logic signed [TIMING_WIDTH-1:0] out_fine_timing_error_samples,
    output logic [11:0] out_used_pairs
);
  localparam int OFFSET_COUNT = (2 * MAX_OFFSET_BINS) + 1;
  localparam int FFT_BIN_WIDTH = 12;
  localparam int DIFF_WIDTH = (2 * CARRIER_WIDTH) + 2;
  localparam int CORR_ACC_WIDTH = DIFF_WIDTH + 16;
  localparam int ENERGY_ACC_WIDTH = (2 * DIFF_WIDTH) + 16;
  localparam int SCORE_NUM_WIDTH = (2 * CORR_ACC_WIDTH) + SCORE_FRAC_BITS + 4;
  localparam int SCORE_DEN_WIDTH = ENERGY_ACC_WIDTH + 16;
  localparam int TIMING_CALC_WIDTH = CORR_ACC_WIDTH + TIMING_WIDTH + 24;

  typedef enum logic [1:0] {
    STATE_ACCUM = 2'd0,
    STATE_FINALIZE = 2'd1,
    STATE_OUTPUT = 2'd2
  } state_t;

  localparam logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] NULL_ACTIVE_INDEX =
      DAB_OFDM_ACTIVE_INDEX_WIDTH'(DAB_OFDM_ACTIVE_CARRIER_HALF);
  localparam logic [FFT_BIN_WIDTH-1:0] HIGH_FIRST_BIN =
      FFT_BIN_WIDTH'(DAB_OFDM_FFT_SAMPLES - DAB_OFDM_ACTIVE_CARRIER_HALF);
  localparam logic [FFT_BIN_WIDTH-1:0] LOW_LAST_BIN =
      FFT_BIN_WIDTH'(DAB_OFDM_ACTIVE_CARRIER_HALF);

  localparam logic [63:0] PRS_H_ROW_0 = 64'h5a0250085a025008;
  localparam logic [63:0] PRS_H_ROW_1 = 64'h3ee634ec3ee634ec;
  localparam logic [63:0] PRS_H_ROW_2 = 64'hd28ad880d28ad880;
  localparam logic [63:0] PRS_H_ROW_3 = 64'hb66ebc64b66ebc64;

  state_t state_q;
  logic symbol_active_q;
  logic [63:0] symbol_start_sample_q;

  logic prev_valid_q [0:OFFSET_COUNT-1];
  logic [FFT_BIN_WIDTH-1:0] prev_template_bin_q [0:OFFSET_COUNT-1];
  logic [1:0] prev_phase_q [0:OFFSET_COUNT-1];
  logic signed [CARRIER_WIDTH-1:0] prev_i_q [0:OFFSET_COUNT-1];
  logic signed [CARRIER_WIDTH-1:0] prev_q_q [0:OFFSET_COUNT-1];
  logic signed [CORR_ACC_WIDTH-1:0] corr_re_q [0:OFFSET_COUNT-1];
  logic signed [CORR_ACC_WIDTH-1:0] corr_im_q [0:OFFSET_COUNT-1];
  logic [ENERGY_ACC_WIDTH-1:0] signal_energy_q [0:OFFSET_COUNT-1];
  logic [11:0] used_pairs_q [0:OFFSET_COUNT-1];

  logic input_fire;
  logic best_found_comb;
  logic [SCORE_WIDTH-1:0] best_score_comb;
  logic signed [3:0] best_offset_comb;
  logic signed [CORR_ACC_WIDTH-1:0] best_corr_re_comb;
  logic signed [CORR_ACC_WIDTH-1:0] best_corr_im_comb;
  logic [11:0] best_used_pairs_comb;
  logic signed [TIMING_WIDTH-1:0] best_timing_comb;

  assign in_ready = !clear && state_q == STATE_ACCUM;
  assign input_fire = in_valid && in_ready;
  assign best_timing_comb = fine_timing_from_corr(best_corr_re_comb, best_corr_im_comb);

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

  function automatic logic [1:0] prs_phase_from_active(
      input logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index
  );
    logic [1:0] row;
    logic [4:0] offset;
    begin
      if (active_index < NULL_ACTIVE_INDEX) begin
        row = active_index[6:5];
        offset = active_index[4:0];
        prs_phase_from_active = phase_from_row(row, offset);
      end else if (active_index > NULL_ACTIVE_INDEX) begin
        row = positive_prs_row(2'((active_index - (NULL_ACTIVE_INDEX + 1'b1)) >> 5));
        offset = 5'(active_index - (NULL_ACTIVE_INDEX + 1'b1));
        prs_phase_from_active = phase_from_row(row, offset);
      end else begin
        prs_phase_from_active = 2'd0;
      end
    end
  endfunction

  function automatic logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_from_template_bin(
      input logic [FFT_BIN_WIDTH-1:0] fft_bin
  );
    begin
      if (fft_bin >= HIGH_FIRST_BIN) begin
        active_from_template_bin = DAB_OFDM_ACTIVE_INDEX_WIDTH'(fft_bin - HIGH_FIRST_BIN);
      end else begin
        active_from_template_bin = DAB_OFDM_ACTIVE_INDEX_WIDTH'(NULL_ACTIVE_INDEX + fft_bin);
      end
    end
  endfunction

  function automatic logic [FFT_BIN_WIDTH-1:0] fft_bin_from_active(
      input logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index
  );
    begin
      if (active_index < NULL_ACTIVE_INDEX) begin
        fft_bin_from_active = HIGH_FIRST_BIN + FFT_BIN_WIDTH'(active_index);
      end else if (active_index == NULL_ACTIVE_INDEX) begin
        fft_bin_from_active = '0;
      end else begin
        fft_bin_from_active = FFT_BIN_WIDTH'(active_index - NULL_ACTIVE_INDEX);
      end
    end
  endfunction

  function automatic logic template_bin_valid(input int signed fft_bin);
    begin
      template_bin_valid =
          (fft_bin >= int'(HIGH_FIRST_BIN) && fft_bin < DAB_OFDM_FFT_SAMPLES) ||
          (fft_bin >= 1 && fft_bin <= int'(LOW_LAST_BIN));
    end
  endfunction

  function automatic logic consecutive_template_bins(
      input logic [FFT_BIN_WIDTH-1:0] previous_bin,
      input logic [FFT_BIN_WIDTH-1:0] current_bin
  );
    begin
      consecutive_template_bins =
          current_bin == previous_bin + 1'b1 ||
          (previous_bin == FFT_BIN_WIDTH'(DAB_OFDM_FFT_SAMPLES - 1) &&
           current_bin == FFT_BIN_WIDTH'(1));
    end
  endfunction

  function automatic logic signed [3:0] offset_from_index(input int unsigned index);
    begin
      offset_from_index = 4'(int'(index) - MAX_OFFSET_BINS);
    end
  endfunction

  function automatic logic signed [CORR_ACC_WIDTH-1:0] extend_diff(
      input logic signed [DIFF_WIDTH-1:0] value
  );
    begin
      extend_diff = {{(CORR_ACC_WIDTH - DIFF_WIDTH){value[DIFF_WIDTH-1]}}, value};
    end
  endfunction

  function automatic logic signed [DIFF_WIDTH-1:0] extend_carrier(
      input logic signed [CARRIER_WIDTH-1:0] value
  );
    begin
      extend_carrier = {{(DIFF_WIDTH - CARRIER_WIDTH){value[CARRIER_WIDTH-1]}}, value};
    end
  endfunction

  function automatic logic [ENERGY_ACC_WIDTH-1:0] diff_energy(
      input logic signed [DIFF_WIDTH-1:0] re,
      input logic signed [DIFF_WIDTH-1:0] im
  );
    logic signed [(2 * DIFF_WIDTH)-1:0] re_sq;
    logic signed [(2 * DIFF_WIDTH)-1:0] im_sq;
    logic [(2 * DIFF_WIDTH):0] sum;
    begin
      re_sq = re * re;
      im_sq = im * im;
      sum = (2 * DIFF_WIDTH + 1)'(re_sq) + (2 * DIFF_WIDTH + 1)'(im_sq);
      diff_energy = ENERGY_ACC_WIDTH'(sum);
    end
  endfunction

  function automatic logic [(2 * CORR_ACC_WIDTH)-1:0] corr_square(
      input logic signed [CORR_ACC_WIDTH-1:0] value
  );
    logic signed [(2 * CORR_ACC_WIDTH)-1:0] product;
    begin
      product = value * value;
      corr_square = (2 * CORR_ACC_WIDTH)'(product);
    end
  endfunction

  function automatic logic [SCORE_WIDTH-1:0] saturate_score(
      input logic [SCORE_NUM_WIDTH-1:0] value
  );
    logic [SCORE_NUM_WIDTH-1:0] max_value;
    begin
      max_value = SCORE_NUM_WIDTH'({SCORE_WIDTH{1'b1}});
      if (value > max_value) begin
        saturate_score = {SCORE_WIDTH{1'b1}};
      end else begin
        saturate_score = value[SCORE_WIDTH-1:0];
      end
    end
  endfunction

  function automatic logic [SCORE_WIDTH-1:0] score_from_acc(
      input logic signed [CORR_ACC_WIDTH-1:0] corr_re,
      input logic signed [CORR_ACC_WIDTH-1:0] corr_im,
      input logic [ENERGY_ACC_WIDTH-1:0] signal_energy,
      input logic [11:0] used_pairs
  );
    logic [(2 * CORR_ACC_WIDTH)-1:0] corr_re_sq;
    logic [(2 * CORR_ACC_WIDTH)-1:0] corr_im_sq;
    logic [(2 * CORR_ACC_WIDTH):0] corr_mag_sq;
    logic [SCORE_DEN_WIDTH-1:0] denominator;
    logic [SCORE_NUM_WIDTH-1:0] numerator;
    logic [SCORE_NUM_WIDTH-1:0] quotient;
    begin
      if (used_pairs == 12'd0 || signal_energy == '0) begin
        score_from_acc = '0;
      end else begin
        corr_re_sq = corr_square(corr_re);
        corr_im_sq = corr_square(corr_im);
        corr_mag_sq = (2 * CORR_ACC_WIDTH + 1)'(corr_re_sq) +
            (2 * CORR_ACC_WIDTH + 1)'(corr_im_sq);
        denominator = SCORE_DEN_WIDTH'(signal_energy) * SCORE_DEN_WIDTH'(used_pairs);
        numerator = SCORE_NUM_WIDTH'(corr_mag_sq) << SCORE_FRAC_BITS;
        quotient = denominator == '0 ? '0 : numerator / SCORE_NUM_WIDTH'(denominator);
        score_from_acc = saturate_score(quotient);
      end
    end
  endfunction

  function automatic logic signed [TIMING_WIDTH-1:0] saturate_timing(
      input logic signed [TIMING_CALC_WIDTH-1:0] value
  );
    logic signed [TIMING_CALC_WIDTH-1:0] max_value;
    logic signed [TIMING_CALC_WIDTH-1:0] min_value;
    begin
      max_value = (TIMING_CALC_WIDTH'(1) <<< (TIMING_WIDTH - 1)) - TIMING_CALC_WIDTH'(1);
      min_value = -(TIMING_CALC_WIDTH'(1) <<< (TIMING_WIDTH - 1));

      if (value > max_value) begin
        saturate_timing = {1'b0, {(TIMING_WIDTH - 1){1'b1}}};
      end else if (value < min_value) begin
        saturate_timing = {1'b1, {(TIMING_WIDTH - 1){1'b0}}};
      end else begin
        saturate_timing = value[TIMING_WIDTH-1:0];
      end
    end
  endfunction

  function automatic logic signed [TIMING_WIDTH-1:0] fine_timing_from_corr(
      input logic signed [CORR_ACC_WIDTH-1:0] corr_re,
      input logic signed [CORR_ACC_WIDTH-1:0] corr_im
  );
    logic signed [TIMING_CALC_WIDTH-1:0] numerator;
    logic signed [TIMING_CALC_WIDTH-1:0] denominator;
    logic signed [TIMING_CALC_WIDTH-1:0] quotient;
    begin
      if (corr_re == {CORR_ACC_WIDTH{1'b0}}) begin
        if (!corr_im[CORR_ACC_WIDTH-1] && corr_im != {CORR_ACC_WIDTH{1'b0}}) begin
          fine_timing_from_corr = {1'b0, {(TIMING_WIDTH - 1){1'b1}}};
        end else if (corr_im[CORR_ACC_WIDTH-1]) begin
          fine_timing_from_corr = {1'b1, {(TIMING_WIDTH - 1){1'b0}}};
        end else begin
          fine_timing_from_corr = '0;
        end
      end else if (corr_re[CORR_ACC_WIDTH-1]) begin
        fine_timing_from_corr = !corr_im[CORR_ACC_WIDTH-1] ?
            {1'b0, {(TIMING_WIDTH - 1){1'b1}}} :
            {1'b1, {(TIMING_WIDTH - 1){1'b0}}};
      end else begin
        numerator = TIMING_CALC_WIDTH'(corr_im) * TIMING_CALC_WIDTH'(TIMING_GAIN_Q);
        denominator = TIMING_CALC_WIDTH'(corr_re);
        quotient = numerator / denominator;
        fine_timing_from_corr = saturate_timing(quotient);
      end
    end
  endfunction

  function automatic logic signed [DIFF_WIDTH-1:0] diff_re(
      input logic signed [CARRIER_WIDTH-1:0] previous_i,
      input logic signed [CARRIER_WIDTH-1:0] previous_q,
      input logic signed [CARRIER_WIDTH-1:0] current_i,
      input logic signed [CARRIER_WIDTH-1:0] current_q
  );
    begin
      diff_re = extend_carrier(previous_i) * extend_carrier(current_i) +
          extend_carrier(previous_q) * extend_carrier(current_q);
    end
  endfunction

  function automatic logic signed [DIFF_WIDTH-1:0] diff_im(
      input logic signed [CARRIER_WIDTH-1:0] previous_i,
      input logic signed [CARRIER_WIDTH-1:0] previous_q,
      input logic signed [CARRIER_WIDTH-1:0] current_i,
      input logic signed [CARRIER_WIDTH-1:0] current_q
  );
    begin
      diff_im = extend_carrier(previous_q) * extend_carrier(current_i) -
          extend_carrier(previous_i) * extend_carrier(current_q);
    end
  endfunction

  always_comb begin
    best_found_comb = 1'b0;
    best_score_comb = '0;
    best_offset_comb = '0;
    best_corr_re_comb = '0;
    best_corr_im_comb = '0;
    best_used_pairs_comb = '0;

    for (int unsigned i = 0; i < OFFSET_COUNT; i++) begin
      logic [SCORE_WIDTH-1:0] candidate_score;

      candidate_score = score_from_acc(corr_re_q[i],
                                       corr_im_q[i],
                                       signal_energy_q[i],
                                       used_pairs_q[i]);
      if (used_pairs_q[i] != 12'd0 && (!best_found_comb || candidate_score > best_score_comb)) begin
        best_found_comb = 1'b1;
        best_score_comb = candidate_score;
        best_offset_comb = offset_from_index(i);
        best_corr_re_comb = corr_re_q[i];
        best_corr_im_comb = corr_im_q[i];
        best_used_pairs_comb = used_pairs_q[i];
      end
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state_q <= STATE_ACCUM;
      symbol_active_q <= 1'b0;
      symbol_start_sample_q <= '0;
      out_valid <= 1'b0;
      out_found <= 1'b0;
      out_symbol_start_sample <= '0;
      out_score <= '0;
      out_carrier_offset_bins <= '0;
      out_fine_timing_error_samples <= '0;
      out_used_pairs <= '0;

      for (int unsigned i = 0; i < OFFSET_COUNT; i++) begin
        prev_valid_q[i] <= 1'b0;
        prev_template_bin_q[i] <= '0;
        prev_phase_q[i] <= '0;
        prev_i_q[i] <= '0;
        prev_q_q[i] <= '0;
        corr_re_q[i] <= '0;
        corr_im_q[i] <= '0;
        signal_energy_q[i] <= '0;
        used_pairs_q[i] <= '0;
      end
    end else if (clear) begin
      state_q <= STATE_ACCUM;
      symbol_active_q <= 1'b0;
      symbol_start_sample_q <= '0;
      out_valid <= 1'b0;
      out_found <= 1'b0;
      out_symbol_start_sample <= '0;
      out_score <= '0;
      out_carrier_offset_bins <= '0;
      out_fine_timing_error_samples <= '0;
      out_used_pairs <= '0;

      for (int unsigned i = 0; i < OFFSET_COUNT; i++) begin
        prev_valid_q[i] <= 1'b0;
        prev_template_bin_q[i] <= '0;
        prev_phase_q[i] <= '0;
        prev_i_q[i] <= '0;
        prev_q_q[i] <= '0;
        corr_re_q[i] <= '0;
        corr_im_q[i] <= '0;
        signal_energy_q[i] <= '0;
        used_pairs_q[i] <= '0;
      end
    end else begin
      case (state_q)
        STATE_ACCUM: begin
          if (input_fire) begin
            logic [FFT_BIN_WIDTH-1:0] current_fft_bin;

            current_fft_bin = fft_bin_from_active(in_active_index);
            if (!symbol_active_q) begin
              symbol_active_q <= 1'b1;
              symbol_start_sample_q <= in_symbol_start_sample;
            end

            for (int unsigned i = 0; i < OFFSET_COUNT; i++) begin
              int signed template_bin_int;

              template_bin_int = int'(current_fft_bin) - int'(offset_from_index(i));
              if (template_bin_valid(template_bin_int)) begin
                logic [FFT_BIN_WIDTH-1:0] current_template_bin;
                logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] current_template_active;
                logic [1:0] current_phase;

                current_template_bin = FFT_BIN_WIDTH'(template_bin_int);
                current_template_active = active_from_template_bin(current_template_bin);
                current_phase = prs_phase_from_active(current_template_active);

                if (prev_valid_q[i] &&
                    consecutive_template_bins(prev_template_bin_q[i], current_template_bin)) begin
                  logic signed [DIFF_WIDTH-1:0] sd_re;
                  logic signed [DIFF_WIDTH-1:0] sd_im;
                  logic signed [DIFF_WIDTH-1:0] aligned_re;
                  logic signed [DIFF_WIDTH-1:0] aligned_im;
                  logic [1:0] rotation;

                  sd_re = diff_re(prev_i_q[i], prev_q_q[i], in_i, in_q);
                  sd_im = diff_im(prev_i_q[i], prev_q_q[i], in_i, in_q);
                  rotation = current_phase - prev_phase_q[i];

                  case (rotation)
                    2'd0: begin
                      aligned_re = sd_re;
                      aligned_im = sd_im;
                    end
                    2'd1: begin
                      aligned_re = -sd_im;
                      aligned_im = sd_re;
                    end
                    2'd2: begin
                      aligned_re = -sd_re;
                      aligned_im = -sd_im;
                    end
                    default: begin
                      aligned_re = sd_im;
                      aligned_im = -sd_re;
                    end
                  endcase

                  corr_re_q[i] <= corr_re_q[i] + extend_diff(aligned_re);
                  corr_im_q[i] <= corr_im_q[i] + extend_diff(aligned_im);
                  signal_energy_q[i] <= signal_energy_q[i] + diff_energy(sd_re, sd_im);
                  used_pairs_q[i] <= used_pairs_q[i] + 1'b1;
                end

                prev_valid_q[i] <= 1'b1;
                prev_template_bin_q[i] <= current_template_bin;
                prev_phase_q[i] <= current_phase;
                prev_i_q[i] <= in_i;
                prev_q_q[i] <= in_q;
              end
            end

            if (in_symbol_done) begin
              state_q <= STATE_FINALIZE;
            end
          end
        end

        STATE_FINALIZE: begin
          out_valid <= 1'b1;
          out_found <= best_found_comb;
          out_symbol_start_sample <= symbol_start_sample_q;
          out_score <= best_found_comb ? best_score_comb : '0;
          out_carrier_offset_bins <= best_found_comb ? best_offset_comb : '0;
          out_fine_timing_error_samples <= best_found_comb ? best_timing_comb : '0;
          out_used_pairs <= best_found_comb ? best_used_pairs_comb : '0;
          state_q <= STATE_OUTPUT;
          symbol_active_q <= 1'b0;
        end

        default: begin
          if (out_valid && out_ready) begin
            out_valid <= 1'b0;
            out_found <= 1'b0;
            out_score <= '0;
            out_carrier_offset_bins <= '0;
            out_fine_timing_error_samples <= '0;
            out_used_pairs <= '0;
            state_q <= STATE_ACCUM;

            for (int unsigned i = 0; i < OFFSET_COUNT; i++) begin
              prev_valid_q[i] <= 1'b0;
              prev_template_bin_q[i] <= '0;
              prev_phase_q[i] <= '0;
              prev_i_q[i] <= '0;
              prev_q_q[i] <= '0;
              corr_re_q[i] <= '0;
              corr_im_q[i] <= '0;
              signal_energy_q[i] <= '0;
              used_pairs_q[i] <= '0;
            end
          end
        end
      endcase
    end
  end
endmodule

`default_nettype wire
