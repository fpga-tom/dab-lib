`default_nettype none

module cp_stream_metric #(
    parameter int SAMPLE_WIDTH = 16,
    parameter int SCORE_WIDTH = 32,
    parameter int SCORE_FRAC_BITS = 30,
    parameter int CFO_WIDTH = 32,
    parameter int CFO_GAIN_FRAC_BITS = 20,
    parameter int CFO_GAIN_Q = 166886054,
    parameter int CFO_MAX_HZ = 500,
    parameter int GUARD_SAMPLES = 504,
    parameter int FFT_SAMPLES = 2048
) (
    input  logic clk,
    input  logic rst_n,
    input  logic clear,

    input  logic in_valid,
    input  logic signed [SAMPLE_WIDTH-1:0] in_i,
    input  logic signed [SAMPLE_WIDTH-1:0] in_q,
    input  logic [63:0] in_sample_index,

    output logic out_valid,
    output logic out_ok,
    output logic [63:0] out_fft_start_sample,
    output logic [SCORE_WIDTH-1:0] out_score,
    output logic signed [CFO_WIDTH-1:0] out_coarse_freq_error_hz
);
  localparam int FFT_ADDR_WIDTH = FFT_SAMPLES <= 2 ? 1 : $clog2(FFT_SAMPLES);
  localparam int GUARD_ADDR_WIDTH = GUARD_SAMPLES <= 2 ? 1 : $clog2(GUARD_SAMPLES);
  localparam int SAMPLE_COUNT_LIMIT = FFT_SAMPLES + GUARD_SAMPLES + 1;

  (* ramstyle = "M10K" *)
  logic signed [SAMPLE_WIDTH-1:0] delay_i_mem [0:FFT_SAMPLES-1];
  (* ramstyle = "M10K" *)
  logic signed [SAMPLE_WIDTH-1:0] delay_q_mem [0:FFT_SAMPLES-1];
  (* ramstyle = "M10K" *)
  logic signed [63:0] prod_re_mem [0:GUARD_SAMPLES-1];
  (* ramstyle = "M10K" *)
  logic signed [63:0] prod_im_mem [0:GUARD_SAMPLES-1];
  (* ramstyle = "M10K" *)
  logic [63:0] energy_a_mem [0:GUARD_SAMPLES-1];
  (* ramstyle = "M10K" *)
  logic [63:0] energy_b_mem [0:GUARD_SAMPLES-1];

  logic signed [SAMPLE_WIDTH-1:0] delay_i_rd_q;
  logic signed [SAMPLE_WIDTH-1:0] delay_q_rd_q;
  logic signed [63:0] old_prod_re_q;
  logic signed [63:0] old_prod_im_q;
  logic [63:0] old_energy_a_q;
  logic [63:0] old_energy_b_q;

  logic range_valid_q;
  logic [63:0] next_sample_index_q;
  logic [31:0] sample_count_q;
  logic [31:0] ring_count_q;
  logic [GUARD_ADDR_WIDTH-1:0] ring_addr_q;

  logic stage_valid_q;
  logic signed [SAMPLE_WIDTH-1:0] stage_i_q;
  logic signed [SAMPLE_WIDTH-1:0] stage_q_q;
  logic [63:0] stage_sample_index_q;

  logic pending_valid_q;
  logic [GUARD_ADDR_WIDTH-1:0] pending_addr_q;
  logic [63:0] pending_sample_index_q;
  logic signed [63:0] pending_prod_re_q;
  logic signed [63:0] pending_prod_im_q;
  logic [63:0] pending_energy_a_q;
  logic [63:0] pending_energy_b_q;

  logic signed [63:0] corr_re_sum_q;
  logic signed [63:0] corr_im_sum_q;
  logic [63:0] energy_a_sum_q;
  logic [63:0] energy_b_sum_q;

  logic input_fire;
  logic input_contiguous;
  logic delayed_sample_valid;
  logic [FFT_ADDR_WIDTH-1:0] delay_addr;
  logic ring_last;
  logic signed [63:0] stage_prod_re;
  logic signed [63:0] stage_prod_im;
  logic [63:0] stage_energy_a;
  logic [63:0] stage_energy_b;

  assign input_fire = in_valid && !clear;
  assign input_contiguous = range_valid_q && in_sample_index == next_sample_index_q;
  assign delayed_sample_valid = input_contiguous && sample_count_q >= 32'(FFT_SAMPLES);
  assign delay_addr = in_sample_index[FFT_ADDR_WIDTH-1:0];
  assign ring_last = ring_addr_q == GUARD_ADDR_WIDTH'(GUARD_SAMPLES - 1);
  assign stage_prod_re = corr_re_sample(delay_i_rd_q, delay_q_rd_q, stage_i_q, stage_q_q);
  assign stage_prod_im = corr_im_sample(delay_i_rd_q, delay_q_rd_q, stage_i_q, stage_q_q);
  assign stage_energy_a = sample_power(delay_i_rd_q, delay_q_rd_q);
  assign stage_energy_b = sample_power(stage_i_q, stage_q_q);

  function automatic logic signed [63:0] extend_sample(
      input logic signed [SAMPLE_WIDTH-1:0] value
  );
    begin
      extend_sample = {{(64 - SAMPLE_WIDTH){value[SAMPLE_WIDTH-1]}}, value};
    end
  endfunction

  function automatic logic [63:0] sample_power(
      input logic signed [SAMPLE_WIDTH-1:0] sample_i,
      input logic signed [SAMPLE_WIDTH-1:0] sample_q
  );
    logic signed [63:0] i_wide;
    logic signed [63:0] q_wide;
    logic signed [127:0] i_sq;
    logic signed [127:0] q_sq;
    logic [127:0] total;
    begin
      i_wide = extend_sample(sample_i);
      q_wide = extend_sample(sample_q);
      i_sq = i_wide * i_wide;
      q_sq = q_wide * q_wide;
      total = 128'(i_sq) + 128'(q_sq);
      if (total > 128'({64{1'b1}})) begin
        sample_power = {64{1'b1}};
      end else begin
        sample_power = total[63:0];
      end
    end
  endfunction

  function automatic logic signed [63:0] corr_re_sample(
      input logic signed [SAMPLE_WIDTH-1:0] a_i,
      input logic signed [SAMPLE_WIDTH-1:0] a_q,
      input logic signed [SAMPLE_WIDTH-1:0] b_i,
      input logic signed [SAMPLE_WIDTH-1:0] b_q
  );
    logic signed [63:0] ai;
    logic signed [63:0] aq;
    logic signed [63:0] bi;
    logic signed [63:0] bq;
    begin
      ai = extend_sample(a_i);
      aq = extend_sample(a_q);
      bi = extend_sample(b_i);
      bq = extend_sample(b_q);
      corr_re_sample = (ai * bi) + (aq * bq);
    end
  endfunction

  function automatic logic signed [63:0] corr_im_sample(
      input logic signed [SAMPLE_WIDTH-1:0] a_i,
      input logic signed [SAMPLE_WIDTH-1:0] a_q,
      input logic signed [SAMPLE_WIDTH-1:0] b_i,
      input logic signed [SAMPLE_WIDTH-1:0] b_q
  );
    logic signed [63:0] ai;
    logic signed [63:0] aq;
    logic signed [63:0] bi;
    logic signed [63:0] bq;
    begin
      ai = extend_sample(a_i);
      aq = extend_sample(a_q);
      bi = extend_sample(b_i);
      bq = extend_sample(b_q);
      corr_im_sample = (aq * bi) - (ai * bq);
    end
  endfunction

  function automatic logic [SCORE_WIDTH-1:0] saturate_score(input logic [127:0] value);
    logic [127:0] max_value;
    begin
      max_value = 128'({SCORE_WIDTH{1'b1}});
      if (value > max_value) begin
        saturate_score = {SCORE_WIDTH{1'b1}};
      end else begin
        saturate_score = value[SCORE_WIDTH-1:0];
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

  function automatic int unsigned bit_length_u96(input logic [95:0] value);
    integer bit_index;
    begin
      bit_length_u96 = 0;
      for (bit_index = 0; bit_index < 96; bit_index = bit_index + 1) begin
        if (value[bit_index]) begin
          bit_length_u96 = bit_index + 1;
        end
      end
    end
  endfunction

  function automatic logic [63:0] scaled_score_numerator_u64(
      input logic [127:0] corr_mag_sq,
      input int unsigned scale_shift
  );
    /* verilator lint_off UNUSEDSIGNAL */
    logic [127:0] shifted;
    /* verilator lint_on UNUSEDSIGNAL */
    begin
      if (scale_shift >= SCORE_FRAC_BITS) begin
        shifted = corr_mag_sq >> (scale_shift - SCORE_FRAC_BITS);
      end else begin
        shifted = corr_mag_sq << (SCORE_FRAC_BITS - scale_shift);
      end
      scaled_score_numerator_u64 = shifted[63:0];
    end
  endfunction

  function automatic logic [63:0] scaled_u128_to_u64(
      input logic [127:0] value,
      input int unsigned scale_shift
  );
    /* verilator lint_off UNUSEDSIGNAL */
    logic [127:0] shifted;
    /* verilator lint_on UNUSEDSIGNAL */
    begin
      shifted = value >> scale_shift;
      scaled_u128_to_u64 = shifted[63:0];
    end
  endfunction

  function automatic logic signed [63:0] scaled_s96_to_s64(
      input logic signed [95:0] value,
      input int unsigned scale_shift
  );
    /* verilator lint_off UNUSEDSIGNAL */
    logic signed [95:0] shifted;
    /* verilator lint_on UNUSEDSIGNAL */
    begin
      shifted = value >>> scale_shift;
      scaled_s96_to_s64 = shifted[63:0];
    end
  endfunction

  function automatic logic [SCORE_WIDTH-1:0] score_from_acc(
      input logic signed [63:0] corr_re,
      input logic signed [63:0] corr_im,
      input logic [63:0] energy_a,
      input logic [63:0] energy_b
  );
    logic signed [127:0] corr_re_sq;
    logic signed [127:0] corr_im_sq;
    logic [127:0] corr_mag_sq;
    logic [127:0] denominator;
    logic [63:0] numerator_64;
    logic [63:0] denominator_64;
    logic [63:0] quotient_64;
    int unsigned numerator_bits;
    int unsigned denominator_bits;
    int unsigned max_bits;
    int unsigned scale_shift;
    begin
      if (energy_a == 64'd0 || energy_b == 64'd0) begin
        score_from_acc = '0;
      end else begin
        corr_re_sq = corr_re * corr_re;
        corr_im_sq = corr_im * corr_im;
        corr_mag_sq = 128'(corr_re_sq) + 128'(corr_im_sq);
        denominator = 128'(energy_a) * 128'(energy_b);
        numerator_bits = bit_length_u128(corr_mag_sq) + SCORE_FRAC_BITS;
        denominator_bits = bit_length_u128(denominator);
        max_bits = numerator_bits > denominator_bits ? numerator_bits : denominator_bits;
        scale_shift = max_bits > 64 ? max_bits - 64 : 0;
        numerator_64 = scaled_score_numerator_u64(corr_mag_sq, scale_shift);
        denominator_64 = scaled_u128_to_u64(denominator, scale_shift);
        quotient_64 = denominator_64 == 64'd0 ? {64{1'b1}} : numerator_64 / denominator_64;
        score_from_acc = saturate_score({64'd0, quotient_64});
      end
    end
  endfunction

  function automatic logic signed [CFO_WIDTH-1:0] saturate_cfo(
      input logic signed [95:0] value
  );
    logic signed [95:0] max_value;
    logic signed [95:0] min_value;
    begin
      max_value = 96'(CFO_MAX_HZ);
      min_value = -(96'(CFO_MAX_HZ));
      if (value > max_value) begin
        saturate_cfo = CFO_WIDTH'(CFO_MAX_HZ);
      end else if (value < min_value) begin
        saturate_cfo = -(CFO_WIDTH'(CFO_MAX_HZ));
      end else begin
        saturate_cfo = value[CFO_WIDTH-1:0];
      end
    end
  endfunction

  function automatic logic signed [CFO_WIDTH-1:0] cfo_from_corr(
      input logic signed [63:0] corr_re,
      input logic signed [63:0] corr_im
  );
    logic signed [95:0] numerator;
    logic signed [95:0] denominator;
    logic signed [95:0] quotient_q;
    logic signed [95:0] rounded_q;
    logic signed [95:0] hz_value;
    logic [95:0] numerator_abs;
    logic [95:0] denominator_abs;
    logic signed [63:0] numerator_64;
    logic signed [63:0] denominator_64;
    logic signed [63:0] quotient_64;
    int unsigned numerator_bits;
    int unsigned denominator_bits;
    int unsigned max_bits;
    int unsigned scale_shift;
    begin
      if (corr_re == 64'sd0) begin
        if (corr_im < 64'sd0) begin
          cfo_from_corr = CFO_WIDTH'(CFO_MAX_HZ);
        end else if (corr_im > 64'sd0) begin
          cfo_from_corr = -(CFO_WIDTH'(CFO_MAX_HZ));
        end else begin
          cfo_from_corr = '0;
        end
      end else if (corr_re < 64'sd0) begin
        cfo_from_corr = corr_im <= 64'sd0 ? CFO_WIDTH'(CFO_MAX_HZ)
                                           : -(CFO_WIDTH'(CFO_MAX_HZ));
      end else begin
        numerator = -(96'(corr_im)) * 96'(CFO_GAIN_Q);
        denominator = 96'(corr_re);
        numerator_abs = numerator[95] ? (~numerator + 96'd1) : numerator;
        denominator_abs = denominator[95:0];
        numerator_bits = bit_length_u96(numerator_abs);
        denominator_bits = bit_length_u96(denominator_abs);
        max_bits = numerator_bits > denominator_bits ? numerator_bits : denominator_bits;
        scale_shift = max_bits > 63 ? max_bits - 63 : 0;
        numerator_64 = scaled_s96_to_s64(numerator, scale_shift);
        denominator_64 = scaled_s96_to_s64(denominator, scale_shift);
        if (denominator_64 == 64'sd0) begin
          quotient_q = numerator[95] ? -(96'(CFO_MAX_HZ) <<< CFO_GAIN_FRAC_BITS)
                                     : (96'(CFO_MAX_HZ) <<< CFO_GAIN_FRAC_BITS);
        end else begin
          quotient_64 = numerator_64 / denominator_64;
          quotient_q = {{32{quotient_64[63]}}, quotient_64};
        end
        rounded_q = quotient_q + (96'sd1 <<< (CFO_GAIN_FRAC_BITS - 1));
        hz_value = rounded_q >>> CFO_GAIN_FRAC_BITS;
        cfo_from_corr = saturate_cfo(hz_value);
      end
    end
  endfunction

  function automatic logic [31:0] sat_inc_count(input logic [31:0] value);
    begin
      sat_inc_count = value >= 32'(SAMPLE_COUNT_LIMIT) ? value : value + 1'b1;
    end
  endfunction

  always_ff @(posedge clk) begin
    if (input_fire) begin
      delay_i_rd_q <= delay_i_mem[delay_addr];
      delay_q_rd_q <= delay_q_mem[delay_addr];
      delay_i_mem[delay_addr] <= in_i;
      delay_q_mem[delay_addr] <= in_q;
    end

    if (stage_valid_q) begin
      old_prod_re_q <= prod_re_mem[ring_addr_q];
      old_prod_im_q <= prod_im_mem[ring_addr_q];
      old_energy_a_q <= energy_a_mem[ring_addr_q];
      old_energy_b_q <= energy_b_mem[ring_addr_q];
    end

    if (pending_valid_q) begin
      prod_re_mem[pending_addr_q] <= pending_prod_re_q;
      prod_im_mem[pending_addr_q] <= pending_prod_im_q;
      energy_a_mem[pending_addr_q] <= pending_energy_a_q;
      energy_b_mem[pending_addr_q] <= pending_energy_b_q;
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      range_valid_q <= 1'b0;
      next_sample_index_q <= '0;
      sample_count_q <= '0;
      ring_count_q <= '0;
      ring_addr_q <= '0;
      stage_valid_q <= 1'b0;
      stage_i_q <= '0;
      stage_q_q <= '0;
      stage_sample_index_q <= '0;
      pending_valid_q <= 1'b0;
      pending_addr_q <= '0;
      pending_sample_index_q <= '0;
      pending_prod_re_q <= '0;
      pending_prod_im_q <= '0;
      pending_energy_a_q <= '0;
      pending_energy_b_q <= '0;
      corr_re_sum_q <= '0;
      corr_im_sum_q <= '0;
      energy_a_sum_q <= '0;
      energy_b_sum_q <= '0;
      out_valid <= 1'b0;
      out_ok <= 1'b0;
      out_fft_start_sample <= '0;
      out_score <= '0;
      out_coarse_freq_error_hz <= '0;
    end else if (clear) begin
      range_valid_q <= 1'b0;
      next_sample_index_q <= '0;
      sample_count_q <= '0;
      ring_count_q <= '0;
      ring_addr_q <= '0;
      stage_valid_q <= 1'b0;
      stage_i_q <= '0;
      stage_q_q <= '0;
      stage_sample_index_q <= '0;
      pending_valid_q <= 1'b0;
      pending_addr_q <= '0;
      pending_sample_index_q <= '0;
      pending_prod_re_q <= '0;
      pending_prod_im_q <= '0;
      pending_energy_a_q <= '0;
      pending_energy_b_q <= '0;
      corr_re_sum_q <= '0;
      corr_im_sum_q <= '0;
      energy_a_sum_q <= '0;
      energy_b_sum_q <= '0;
      out_valid <= 1'b0;
      out_ok <= 1'b0;
      out_fft_start_sample <= '0;
      out_score <= '0;
      out_coarse_freq_error_hz <= '0;
    end else begin
      out_valid <= 1'b0;
      out_ok <= 1'b0;

      if (pending_valid_q) begin
        logic signed [63:0] next_corr_re;
        logic signed [63:0] next_corr_im;
        logic [63:0] next_energy_a;
        logic [63:0] next_energy_b;
        logic [31:0] next_ring_count;
        logic ring_full;
        logic metric_ok;

        ring_full = ring_count_q >= 32'(GUARD_SAMPLES);
        next_corr_re = ring_full ? corr_re_sum_q + pending_prod_re_q - old_prod_re_q :
                                   corr_re_sum_q + pending_prod_re_q;
        next_corr_im = ring_full ? corr_im_sum_q + pending_prod_im_q - old_prod_im_q :
                                   corr_im_sum_q + pending_prod_im_q;
        next_energy_a = ring_full ? energy_a_sum_q + pending_energy_a_q - old_energy_a_q :
                                    energy_a_sum_q + pending_energy_a_q;
        next_energy_b = ring_full ? energy_b_sum_q + pending_energy_b_q - old_energy_b_q :
                                    energy_b_sum_q + pending_energy_b_q;
        next_ring_count = ring_full ? ring_count_q : ring_count_q + 1'b1;
        metric_ok = next_ring_count >= 32'(GUARD_SAMPLES) &&
            next_energy_a != 64'd0 && next_energy_b != 64'd0;

        corr_re_sum_q <= next_corr_re;
        corr_im_sum_q <= next_corr_im;
        energy_a_sum_q <= next_energy_a;
        energy_b_sum_q <= next_energy_b;
        ring_count_q <= next_ring_count;

        if (metric_ok) begin
          out_valid <= 1'b1;
          out_ok <= 1'b1;
          out_fft_start_sample <= pending_sample_index_q - 64'(FFT_SAMPLES) + 64'd1;
          out_score <= score_from_acc(next_corr_re, next_corr_im, next_energy_a, next_energy_b);
          out_coarse_freq_error_hz <= cfo_from_corr(next_corr_re, next_corr_im);
        end
      end

      pending_valid_q <= 1'b0;
      if (stage_valid_q) begin
        pending_valid_q <= 1'b1;
        pending_addr_q <= ring_addr_q;
        pending_sample_index_q <= stage_sample_index_q;
        pending_prod_re_q <= stage_prod_re;
        pending_prod_im_q <= stage_prod_im;
        pending_energy_a_q <= stage_energy_a;
        pending_energy_b_q <= stage_energy_b;
        ring_addr_q <= ring_last ? '0 : ring_addr_q + 1'b1;
      end

      stage_valid_q <= 1'b0;
      if (input_fire) begin
        range_valid_q <= 1'b1;
        next_sample_index_q <= in_sample_index + 1'b1;
        stage_i_q <= in_i;
        stage_q_q <= in_q;
        stage_sample_index_q <= in_sample_index;

        if (!input_contiguous) begin
          sample_count_q <= 32'd1;
          ring_count_q <= '0;
          ring_addr_q <= '0;
          pending_valid_q <= 1'b0;
          corr_re_sum_q <= '0;
          corr_im_sum_q <= '0;
          energy_a_sum_q <= '0;
          energy_b_sum_q <= '0;
        end else begin
          sample_count_q <= sat_inc_count(sample_count_q);
          stage_valid_q <= delayed_sample_valid;
        end
      end
    end
  end
endmodule

`default_nettype wire
