`default_nettype none

module cp_metric_engine #(
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

    input  logic metric_req_valid,
    output logic metric_req_ready,
    input  logic [63:0] metric_req_fft_start_sample,
    input  logic [31:0] metric_req_index,
    input  logic metric_req_last,

    output logic hist_read_req_valid,
    input  logic hist_read_req_ready,
    output logic [63:0] hist_read_req_start_sample,
    output logic [31:0] hist_read_req_length,

    input  logic hist_rd_out_valid,
    output logic hist_rd_out_ready,
    input  logic signed [SAMPLE_WIDTH-1:0] hist_rd_out_i,
    input  logic signed [SAMPLE_WIDTH-1:0] hist_rd_out_q,
    input  logic [63:0] hist_rd_out_sample_index,
    input  logic hist_rd_out_first,
    input  logic hist_rd_out_last,
    input  logic hist_rd_out_error,

    output logic metric_resp_valid,
    input  logic metric_resp_ready,
    output logic metric_resp_ok,
    output logic [63:0] metric_resp_fft_start_sample,
    output logic [31:0] metric_resp_index,
    output logic metric_resp_last,
    output logic [SCORE_WIDTH-1:0] metric_resp_score,
    output logic signed [CFO_WIDTH-1:0] metric_resp_coarse_freq_error_hz
);
  localparam int WINDOW_SAMPLES = FFT_SAMPLES + GUARD_SAMPLES;
  localparam int GUARD_ADDR_WIDTH = GUARD_SAMPLES <= 2 ? 1 : $clog2(GUARD_SAMPLES);
  localparam int READ_COUNT_WIDTH = WINDOW_SAMPLES <= 2 ? 1 : $clog2(WINDOW_SAMPLES + 1);

  typedef enum logic [1:0] {
    STATE_IDLE = 2'd0,
    STATE_READ_REQ = 2'd1,
    STATE_READ = 2'd2,
    STATE_RESP = 2'd3
  } state_t;

  state_t state_q;

  logic signed [SAMPLE_WIDTH-1:0] prefix_i_mem [0:GUARD_SAMPLES-1];
  logic signed [SAMPLE_WIDTH-1:0] prefix_q_mem [0:GUARD_SAMPLES-1];

  logic [63:0] fft_start_sample_q;
  logic [31:0] req_index_q;
  logic req_last_q;
  logic read_error_q;
  logic [READ_COUNT_WIDTH-1:0] read_count_q;

  logic signed [63:0] corr_re_q;
  logic signed [63:0] corr_im_q;
  logic [63:0] energy_a_q;
  logic [63:0] energy_b_q;

  logic req_fire;
  logic read_req_fire;
  logic read_fire;
  logic resp_fire;
  logic invalid_fft_start;
  logic read_prefix_sample;
  logic read_tail_sample;
  logic read_prefix_for_next_tail;
  logic [GUARD_ADDR_WIDTH-1:0] prefix_addr;
  logic [GUARD_ADDR_WIDTH-1:0] prefix_read_addr;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [READ_COUNT_WIDTH-1:0] prefix_read_offset;
  /* verilator lint_on UNUSEDSIGNAL */
  logic signed [SAMPLE_WIDTH-1:0] prefix_i_q;
  logic signed [SAMPLE_WIDTH-1:0] prefix_q_q;
  logic signed [63:0] sample_corr_re;
  logic signed [63:0] sample_corr_im;
  logic [63:0] sample_energy_a;
  logic [63:0] sample_energy_b;
  logic response_ok;
  logic history_beat_malformed;
  logic [63:0] expected_hist_sample_index;

  assign metric_req_ready = !clear && state_q == STATE_IDLE;
  assign req_fire = metric_req_valid && metric_req_ready;
  assign invalid_fft_start = metric_req_fft_start_sample < 64'(GUARD_SAMPLES);

  assign hist_read_req_valid = !clear && state_q == STATE_READ_REQ;
  assign hist_read_req_start_sample = fft_start_sample_q - 64'(GUARD_SAMPLES);
  assign hist_read_req_length = 32'(WINDOW_SAMPLES);
  assign read_req_fire = hist_read_req_valid && hist_read_req_ready;

  assign hist_rd_out_ready = !clear && state_q == STATE_READ;
  assign read_fire = hist_rd_out_valid && hist_rd_out_ready;
  assign resp_fire = metric_resp_valid && metric_resp_ready;

  assign read_prefix_sample = read_count_q < READ_COUNT_WIDTH'(GUARD_SAMPLES);
  assign read_tail_sample = read_count_q >= READ_COUNT_WIDTH'(FFT_SAMPLES);
  assign read_prefix_for_next_tail =
      read_count_q >= READ_COUNT_WIDTH'(FFT_SAMPLES - 1) &&
      read_count_q < READ_COUNT_WIDTH'(WINDOW_SAMPLES - 1);
  assign prefix_addr = read_count_q[GUARD_ADDR_WIDTH-1:0];
  assign prefix_read_offset = read_count_q - READ_COUNT_WIDTH'(FFT_SAMPLES - 1);
  assign prefix_read_addr = prefix_read_offset[GUARD_ADDR_WIDTH-1:0];

  assign sample_corr_re = corr_re_sample(prefix_i_q, prefix_q_q, hist_rd_out_i, hist_rd_out_q);
  assign sample_corr_im = corr_im_sample(prefix_i_q, prefix_q_q, hist_rd_out_i, hist_rd_out_q);
  assign sample_energy_a = sample_power(hist_rd_out_i, hist_rd_out_q);
  assign sample_energy_b = sample_power(hist_rd_out_i, hist_rd_out_q);
  assign response_ok = !read_error_q && energy_a_q != 64'd0 && energy_b_q != 64'd0;
  assign expected_hist_sample_index = hist_read_req_start_sample + 64'(read_count_q);
  assign history_beat_malformed =
      !hist_rd_out_error &&
      (hist_rd_out_sample_index != expected_hist_sample_index ||
       hist_rd_out_first != (read_count_q == '0));

  assign metric_resp_ok = response_ok;
  assign metric_resp_fft_start_sample = fft_start_sample_q;
  assign metric_resp_index = req_index_q;
  assign metric_resp_last = req_last_q;
  assign metric_resp_score = response_ok ? score_from_acc(corr_re_q,
                                                          corr_im_q,
                                                          energy_a_q,
                                                          energy_b_q) : '0;
  assign metric_resp_coarse_freq_error_hz = response_ok ? cfo_from_corr(corr_re_q, corr_im_q) : '0;

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

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state_q <= STATE_IDLE;
      fft_start_sample_q <= '0;
      req_index_q <= '0;
      req_last_q <= 1'b0;
      read_error_q <= 1'b0;
      read_count_q <= '0;
      prefix_i_q <= '0;
      prefix_q_q <= '0;
      corr_re_q <= '0;
      corr_im_q <= '0;
      energy_a_q <= '0;
      energy_b_q <= '0;
      metric_resp_valid <= 1'b0;
    end else if (clear) begin
      state_q <= STATE_IDLE;
      fft_start_sample_q <= '0;
      req_index_q <= '0;
      req_last_q <= 1'b0;
      read_error_q <= 1'b0;
      read_count_q <= '0;
      prefix_i_q <= '0;
      prefix_q_q <= '0;
      corr_re_q <= '0;
      corr_im_q <= '0;
      energy_a_q <= '0;
      energy_b_q <= '0;
      metric_resp_valid <= 1'b0;
    end else begin
      if (resp_fire) begin
        metric_resp_valid <= 1'b0;
        state_q <= STATE_IDLE;
      end

      case (state_q)
        STATE_IDLE: begin
          if (req_fire) begin
            fft_start_sample_q <= metric_req_fft_start_sample;
            req_index_q <= metric_req_index;
            req_last_q <= metric_req_last;
            read_error_q <= invalid_fft_start;
            read_count_q <= '0;
            corr_re_q <= '0;
            corr_im_q <= '0;
            energy_a_q <= '0;
            energy_b_q <= '0;

            if (invalid_fft_start) begin
              metric_resp_valid <= 1'b1;
              state_q <= STATE_RESP;
            end else begin
              state_q <= STATE_READ_REQ;
            end
          end
        end

        STATE_READ_REQ: begin
          if (read_req_fire) begin
            state_q <= STATE_READ;
            read_count_q <= '0;
          end
        end

        STATE_READ: begin
          if (read_fire) begin
            if (hist_rd_out_error || history_beat_malformed) begin
              read_error_q <= 1'b1;
            end else begin
              if (read_prefix_for_next_tail) begin
                prefix_i_q <= prefix_i_mem[prefix_read_addr];
                prefix_q_q <= prefix_q_mem[prefix_read_addr];
              end

              if (read_prefix_sample) begin
                prefix_i_mem[prefix_addr] <= hist_rd_out_i;
                prefix_q_mem[prefix_addr] <= hist_rd_out_q;
                energy_a_q <= energy_a_q + sample_energy_a;
              end

              if (read_tail_sample) begin
                corr_re_q <= corr_re_q + sample_corr_re;
                corr_im_q <= corr_im_q + sample_corr_im;
                energy_b_q <= energy_b_q + sample_energy_b;
              end
            end

            read_count_q <= read_count_q + 1'b1;

            if (hist_rd_out_last || hist_rd_out_error) begin
              metric_resp_valid <= 1'b1;
              state_q <= STATE_RESP;
            end
          end
        end

        default: begin
          if (!metric_resp_valid) begin
            state_q <= STATE_IDLE;
          end
        end
      endcase
    end
  end
endmodule

`default_nettype wire
