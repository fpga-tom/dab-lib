`default_nettype none

module dab_pipeline_top
  import dab_ofdm_pkg::*, dab_tframe_pkg::*;
#(
    parameter int SAMPLE_WIDTH = 16,
    parameter int SAMPLE_HISTORY_DEPTH = 262144,
    parameter int NULL_FAST_WINDOW_SAMPLES = 39,
    parameter int NULL_SLOW_WINDOW_SAMPLES = 1999,
    parameter int NULL_MIN_SAMPLES = 128,
    parameter int SCORE_WIDTH = 32,
    parameter int CFO_WIDTH = 32,
    parameter int RATIO_WIDTH = 24,
    parameter int TIMING_WIDTH = 24,
    parameter int GUARD_SAMPLES = 504,
    parameter int FFT_SAMPLES = DAB_OFDM_FFT_SAMPLES
) (
    input  logic clk,
    input  logic rst_n,
    input  logic clear,

    input  logic set_sample_index_valid,
    input  logic [63:0] set_sample_index_value,

    input  logic sdr_in_valid,
    output logic sdr_in_ready,
    input  logic signed [SAMPLE_WIDTH-1:0] sdr_in_i,
    input  logic signed [SAMPLE_WIDTH-1:0] sdr_in_q,

    input  logic cfg_enable_tracking,
    input  logic [SCORE_WIDTH-1:0] cfg_min_cp_score,
    input  logic [SCORE_WIDTH-1:0] cfg_min_pilot_score,

    input  logic cfg_cfo_bypass,
    input  logic [31:0] cfg_cfo_phase_inc,
    input  logic cfg_cfo_phase_load,
    input  logic [31:0] cfg_cfo_phase_seed,

    input  logic predict_in_valid,
    output logic predict_in_ready,
    input  logic [63:0] predict_in_prs_fft_start_sample,

    input  logic data_symbol_cmd_valid,
    output logic data_symbol_cmd_ready,
    input  logic [63:0] data_symbol_cmd_fft_start_sample,

    output logic fft_core_in_valid,
    input  logic fft_core_in_ready,
    output logic signed [DAB_OFDM_SAMPLE_WIDTH-1:0] fft_core_in_i,
    output logic signed [DAB_OFDM_SAMPLE_WIDTH-1:0] fft_core_in_q,
    output logic fft_core_in_last,

    input  logic fft_core_out_valid,
    output logic fft_core_out_ready,
    input  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] fft_core_out_i,
    input  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] fft_core_out_q,
    input  logic [DAB_OFDM_BLOCK_EXP_WIDTH-1:0] fft_core_out_block_exp,
    input  logic fft_core_out_overflow,

    output logic sync_out_valid,
    input  logic sync_out_ready,
    output logic sync_out_found,
    output logic sync_out_locked,
    output logic [7:0] sync_out_lock_count,
    output logic [7:0] sync_out_miss_count,
    output logic [63:0] sync_out_prs_symbol_start_sample,
    output logic [63:0] sync_out_prs_fft_start_sample,
    output logic [SCORE_WIDTH-1:0] sync_out_prs_score,
    output logic signed [CFO_WIDTH-1:0] sync_out_coarse_freq_error_hz,
    output logic [SCORE_WIDTH-1:0] sync_out_pilot_score,
    output logic signed [3:0] sync_out_carrier_offset_bins,
    output logic signed [TIMING_WIDTH-1:0] sync_out_fine_timing_error_samples,
    output logic [11:0] sync_out_pilot_used_pairs,
    output logic [63:0] sync_out_next_prs_fft_start_sample,

    output logic tframe_out_valid,
    input  logic tframe_out_ready,
    output logic signed [DAB_TFRAME_SOFTBIT_WIDTH-1:0] tframe_out_softbit,
    output logic tframe_out_region,
    output logic [DAB_TFRAME_ADDR_WIDTH-1:0] tframe_out_addr,
    output logic [DAB_TFRAME_SYMBOL_INDEX_WIDTH-1:0] tframe_out_symbol_index,
    output logic [DAB_TFRAME_BYTE_INDEX_WIDTH-1:0] tframe_out_byte_index,
    output logic [63:0] tframe_out_frame_start_sample,
    output logic tframe_out_frame_done,

    output logic history_valid,
    output logic [63:0] history_oldest_sample_index,
    output logic [63:0] history_next_sample_index,
    output logic [31:0] history_stored_samples,
    output logic history_full,
    output logic symbol_busy
);
  typedef enum logic [1:0] {
    READ_OWNER_NONE = 2'd0,
    READ_OWNER_CP = 2'd1,
    READ_OWNER_SYMBOL = 2'd2
  } read_owner_t;

  typedef enum logic {
    SOFT_STATE_LOAD_A = 1'b0,
    SOFT_STATE_EMIT_B = 1'b1
  } soft_state_t;

  localparam int DATA_INDEX_WIDTH = DAB_OFDM_DATA_INDEX_WIDTH;
  localparam logic [DATA_INDEX_WIDTH-1:0] LAST_DATA_INDEX =
      DATA_INDEX_WIDTH'(DAB_OFDM_DATA_CARRIERS - 1);

  read_owner_t read_owner_q;
  soft_state_t soft_state_q;
  logic symbol_active_q;
  logic symbol_is_prs_q;

  logic ingress_out_valid;
  logic ingress_out_ready;
  logic signed [SAMPLE_WIDTH-1:0] ingress_out_i;
  logic signed [SAMPLE_WIDTH-1:0] ingress_out_q;
  logic [63:0] ingress_out_sample_index;
  logic sample_fork_valid;
  logic sample_fork_ready;

  logic history_in_ready;
  logic history_read_req_valid;
  logic history_read_req_ready;
  logic [63:0] history_read_req_start_sample;
  logic [31:0] history_read_req_length;
  logic history_rd_out_valid;
  logic history_rd_out_ready;
  logic signed [SAMPLE_WIDTH-1:0] history_rd_out_i;
  logic signed [SAMPLE_WIDTH-1:0] history_rd_out_q;
  logic [63:0] history_rd_out_sample_index;
  logic history_rd_out_first;
  logic history_rd_out_last;
  logic history_rd_out_error;
  logic history_req_fire;
  logic history_resp_done_fire;

  logic null_in_ready;
  logic null_out_valid;
  logic null_out_ready;
  logic [63:0] null_out_sample_index;
  logic [31:0] null_out_length;
  logic [RATIO_WIDTH-1:0] null_out_min_ratio;
  logic track_null_ready;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [33:0] null_out_avg_power;
  /* verilator lint_on UNUSEDSIGNAL */

  logic search_valid;
  logic search_ready;
  logic search_engine_valid;
  logic search_engine_ready;
  logic search_history_ready;
  logic [63:0] search_null_start_sample;
  logic [31:0] search_null_length;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [RATIO_WIDTH-1:0] search_null_min_ratio;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [63:0] search_prs_symbol_start_sample;
  logic [63:0] search_prs_fft_start_sample;
  logic [31:0] search_timing_search_start_offset;
  logic [31:0] search_timing_search_length;
  logic [63:0] search_search_start_sample;
  logic [63:0] search_search_end_sample;
  logic [63:0] search_required_end_sample;

  logic score_valid;
  logic score_ready;
  logic score_found;
  logic [63:0] score_null_start_sample;
  logic [31:0] score_null_length;
  logic [63:0] score_candidate_prs_symbol_start_sample;
  logic [63:0] score_candidate_prs_fft_start_sample;
  logic [31:0] score_timing_search_start_offset;
  logic [31:0] score_timing_search_length;
  logic [63:0] score_search_start_sample;
  logic [63:0] score_search_end_sample;
  logic [63:0] score_required_end_sample;
  logic [63:0] score_prs_peak_sample;
  logic [63:0] score_prs_symbol_start_sample;
  logic [63:0] score_prs_fft_start_sample;
  logic [SCORE_WIDTH-1:0] score_prs_score;
  logic signed [CFO_WIDTH-1:0] score_coarse_freq_error_hz;

  logic metric_req_valid;
  logic metric_req_ready;
  logic [63:0] metric_req_fft_start_sample;
  logic [31:0] metric_req_index;
  logic metric_req_last;
  logic metric_resp_valid;
  logic metric_resp_ready;
  logic metric_resp_ok;
  logic [63:0] metric_resp_fft_start_sample;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [31:0] metric_resp_index;
  logic metric_resp_last;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [SCORE_WIDTH-1:0] metric_resp_score;
  logic signed [CFO_WIDTH-1:0] metric_resp_coarse_freq_error_hz;

  logic cp_hist_req_valid;
  logic cp_hist_req_ready;
  logic [63:0] cp_hist_req_start_sample;
  logic [31:0] cp_hist_req_length;
  logic cp_hist_rd_valid;
  logic cp_hist_rd_ready;

  logic sym_hist_req_valid;
  logic sym_hist_req_ready;
  logic [63:0] sym_hist_req_start_sample;
  logic [31:0] sym_hist_req_length;
  logic sym_hist_rd_valid;
  logic sym_hist_rd_ready;

  logic prs_symbol_req_valid;
  logic prs_symbol_req_ready;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [63:0] prs_symbol_req_symbol_start_sample;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [63:0] prs_symbol_req_fft_start_sample;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [SCORE_WIDTH-1:0] prs_symbol_req_score;
  logic signed [CFO_WIDTH-1:0] prs_symbol_req_coarse_freq_error_hz;
  /* verilator lint_on UNUSEDSIGNAL */

  logic symbol_cmd_valid;
  logic symbol_cmd_fire;
  logic symbol_cmd_is_prs;
  logic [63:0] symbol_cmd_fft_start_sample;
  logic symbol_in_ready;
  logic symbol_done_fire;
  logic symbol_error_fire;

  logic symbol_out_valid;
  logic symbol_out_ready;
  logic signed [SAMPLE_WIDTH-1:0] symbol_out_i;
  logic signed [SAMPLE_WIDTH-1:0] symbol_out_q;
  logic [63:0] symbol_out_sample_index;
  logic [63:0] symbol_out_symbol_start_sample;
  logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] symbol_out_sample_offset;
  logic symbol_out_done;
  logic symbol_out_error;

  logic cfo_out_valid;
  logic cfo_out_ready;
  logic signed [DAB_OFDM_SAMPLE_WIDTH-1:0] cfo_out_i;
  logic signed [DAB_OFDM_SAMPLE_WIDTH-1:0] cfo_out_q;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [63:0] cfo_out_sample_index;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [63:0] cfo_out_symbol_start_sample;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] cfo_out_sample_offset;
  /* verilator lint_on UNUSEDSIGNAL */
  logic cfo_out_symbol_done;
  logic cfo_out_error;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [31:0] cfo_out_phase;
  logic [DAB_OFDM_BLOCK_EXP_WIDTH-1:0] fft_out_block_exp;
  logic fft_out_overflow;
  logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] fft_out_bin_index;
  logic fft_out_symbol_done;
  logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] active_fft_bin_index;
  logic [DAB_OFDM_DATA_INDEX_WIDTH-1:0] soft_pair_carrier_index;
  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] deint_out_source_index;
  logic [63:0] chan_coeff_symbol_start_sample;
  logic chan_coeff_symbol_done;
  /* verilator lint_on UNUSEDSIGNAL */

  logic fft_in_valid;
  logic fft_in_ready;
  logic fft_out_valid;
  logic fft_out_ready;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] fft_out_i;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] fft_out_q;
  logic [63:0] fft_out_symbol_start_sample;

  logic active_valid;
  logic active_ready;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] active_i;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] active_q;
  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] active_index;
  logic [63:0] active_symbol_start_sample;
  logic active_symbol_done;
  logic prs_active_ready;
  logic prs_active_valid;
  logic data_active_valid;

  logic track_pilot_ready;
  logic track_pilot_valid;
  logic track_pilot_found;
  logic [63:0] track_pilot_symbol_start_sample;
  logic [SCORE_WIDTH-1:0] track_pilot_score;
  logic signed [3:0] track_pilot_carrier_offset_bins;
  logic signed [TIMING_WIDTH-1:0] track_pilot_fine_timing_error_samples;
  logic [11:0] track_pilot_used_pairs;
  logic pilot_valid;
  logic pilot_in_ready;
  logic pilot_out_ready;
  logic pilot_found;
  logic [63:0] pilot_symbol_start_sample;
  logic [SCORE_WIDTH-1:0] pilot_score;
  logic signed [3:0] pilot_carrier_offset_bins;
  logic signed [TIMING_WIDTH-1:0] pilot_fine_timing_error_samples;
  logic [11:0] pilot_used_pairs;

  logic chan_ready;
  logic chan_coeff_valid;
  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] chan_coeff_index;
  logic signed [DAB_OFDM_COEFF_WIDTH-1:0] chan_coeff_i;
  logic signed [DAB_OFDM_COEFF_WIDTH-1:0] chan_coeff_q;
  logic chan_ref_valid;
  logic chan_ref_ready;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] chan_ref_i;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] chan_ref_q;
  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] chan_ref_active_index;
  logic [63:0] chan_ref_symbol_start_sample;
  logic chan_ref_symbol_done;

  logic eq_in_valid;
  logic eq_in_ready;
  logic eq_out_valid;
  logic eq_out_ready;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] eq_out_i;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] eq_out_q;
  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] eq_out_active_index;
  logic [63:0] eq_out_symbol_start_sample;
  logic eq_out_symbol_done;

  logic diff_in_valid;
  logic diff_in_ready;
  logic diff_in_seed_only;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] diff_in_i;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] diff_in_q;
  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] diff_in_active_index;
  logic [63:0] diff_in_symbol_start_sample;
  logic diff_in_symbol_done;
  logic diff_out_valid;
  logic diff_out_ready;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] diff_out_i;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] diff_out_q;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] diff_out_active_index;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [63:0] diff_out_symbol_start_sample;
  /* verilator lint_off UNUSEDSIGNAL */
  logic diff_out_symbol_done;
  /* verilator lint_on UNUSEDSIGNAL */

  logic deint_out_valid;
  logic deint_out_ready;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] deint_out_i;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] deint_out_q;
  logic [DAB_OFDM_DATA_INDEX_WIDTH-1:0] deint_out_carrier_index;
  logic [63:0] deint_out_symbol_start_sample;
  logic deint_out_symbol_done;

  logic soft_pair_valid;
  logic soft_pair_ready;
  logic signed [DAB_OFDM_SOFTBIT_WIDTH-1:0] soft_pair_a;
  logic signed [DAB_OFDM_SOFTBIT_WIDTH-1:0] soft_pair_b;
  logic [63:0] soft_pair_symbol_start_sample;
  logic soft_pair_symbol_done;

  logic signed [DAB_OFDM_SOFTBIT_WIDTH-1:0] soft_b_mem [0:DAB_OFDM_DATA_CARRIERS-1];
  logic [DATA_INDEX_WIDTH-1:0] soft_b_emit_index_q;
  logic [63:0] soft_b_symbol_start_sample_q;
  logic tframe_in_valid;
  logic tframe_in_ready;
  logic signed [DAB_TFRAME_SOFTBIT_WIDTH-1:0] tframe_in_softbit;
  logic [63:0] tframe_in_symbol_start_sample;

  assign sample_fork_ready = history_in_ready && null_in_ready;
  assign sample_fork_valid = ingress_out_valid && sample_fork_ready;
  assign ingress_out_ready = sample_fork_ready;
  assign null_out_ready = track_null_ready;

  assign search_history_ready =
      history_valid &&
      search_search_start_sample >= history_oldest_sample_index &&
      search_required_end_sample <= history_next_sample_index;
  assign search_engine_valid = search_valid && search_history_ready;
  assign search_ready = search_engine_ready && search_history_ready;

  assign history_read_req_valid =
      read_owner_q == READ_OWNER_NONE && (cp_hist_req_valid || sym_hist_req_valid);
  assign history_read_req_start_sample =
      cp_hist_req_valid ? cp_hist_req_start_sample : sym_hist_req_start_sample;
  assign history_read_req_length =
      cp_hist_req_valid ? cp_hist_req_length : sym_hist_req_length;
  assign cp_hist_req_ready =
      read_owner_q == READ_OWNER_NONE && cp_hist_req_valid && history_read_req_ready;
  assign sym_hist_req_ready =
      read_owner_q == READ_OWNER_NONE && !cp_hist_req_valid && sym_hist_req_valid &&
      history_read_req_ready;
  assign cp_hist_rd_valid = history_rd_out_valid && read_owner_q == READ_OWNER_CP;
  assign sym_hist_rd_valid = history_rd_out_valid && read_owner_q == READ_OWNER_SYMBOL;
  assign history_rd_out_ready =
      read_owner_q == READ_OWNER_CP ? cp_hist_rd_ready :
      read_owner_q == READ_OWNER_SYMBOL ? sym_hist_rd_ready : 1'b0;
  assign history_req_fire = history_read_req_valid && history_read_req_ready;
  assign history_resp_done_fire = history_rd_out_valid && history_rd_out_ready &&
      (history_rd_out_last || history_rd_out_error);

  assign symbol_cmd_is_prs = prs_symbol_req_valid;
  assign symbol_cmd_valid = !symbol_active_q && (prs_symbol_req_valid || data_symbol_cmd_valid);
  assign symbol_cmd_fft_start_sample =
      prs_symbol_req_valid ? prs_symbol_req_fft_start_sample : data_symbol_cmd_fft_start_sample;
  assign symbol_cmd_fire = symbol_cmd_valid && symbol_in_ready;
  assign prs_symbol_req_ready = !symbol_active_q && symbol_in_ready;
  assign data_symbol_cmd_ready = !prs_symbol_req_valid && !symbol_active_q && symbol_in_ready;
  assign symbol_busy = symbol_active_q;

  assign cfo_out_ready = cfo_out_error ? 1'b1 : fft_in_ready;
  assign fft_in_valid = cfo_out_valid && !cfo_out_error;
  assign symbol_error_fire = cfo_out_valid && cfo_out_ready && cfo_out_error && cfo_out_symbol_done;

  assign active_ready = symbol_is_prs_q ? prs_active_ready : eq_in_ready;
  assign prs_active_ready = pilot_in_ready && chan_ready;
  assign prs_active_valid = active_valid && symbol_is_prs_q && prs_active_ready;
  assign data_active_valid = active_valid && !symbol_is_prs_q;
  assign eq_in_valid = data_active_valid;
  assign symbol_done_fire = active_valid && active_ready && active_symbol_done;

  assign track_pilot_valid = symbol_error_fire && symbol_is_prs_q ? 1'b1 : pilot_valid;
  assign track_pilot_found = symbol_error_fire && symbol_is_prs_q ? 1'b0 : pilot_found;
  assign track_pilot_symbol_start_sample =
      symbol_error_fire && symbol_is_prs_q ? cfo_out_symbol_start_sample : pilot_symbol_start_sample;
  assign track_pilot_score = symbol_error_fire && symbol_is_prs_q ? '0 : pilot_score;
  assign track_pilot_carrier_offset_bins =
      symbol_error_fire && symbol_is_prs_q ? '0 : pilot_carrier_offset_bins;
  assign track_pilot_fine_timing_error_samples =
      symbol_error_fire && symbol_is_prs_q ? '0 : pilot_fine_timing_error_samples;
  assign track_pilot_used_pairs = symbol_error_fire && symbol_is_prs_q ? '0 : pilot_used_pairs;
  assign pilot_out_ready = track_pilot_ready && !(symbol_error_fire && symbol_is_prs_q);

  assign diff_in_valid = chan_ref_valid || eq_out_valid;
  assign diff_in_seed_only = chan_ref_valid;
  assign diff_in_i = chan_ref_valid ? chan_ref_i : eq_out_i;
  assign diff_in_q = chan_ref_valid ? chan_ref_q : eq_out_q;
  assign diff_in_active_index = chan_ref_valid ? chan_ref_active_index : eq_out_active_index;
  assign diff_in_symbol_start_sample =
      chan_ref_valid ? chan_ref_symbol_start_sample : eq_out_symbol_start_sample;
  assign diff_in_symbol_done = chan_ref_valid ? chan_ref_symbol_done : eq_out_symbol_done;
  assign chan_ref_ready = chan_ref_valid && diff_in_ready;
  assign eq_out_ready = !chan_ref_valid && diff_in_ready;

  assign soft_pair_ready = soft_state_q == SOFT_STATE_LOAD_A && tframe_in_ready;
  assign tframe_in_valid =
      (soft_state_q == SOFT_STATE_LOAD_A && soft_pair_valid) ||
      (soft_state_q == SOFT_STATE_EMIT_B);
  assign tframe_in_softbit =
      soft_state_q == SOFT_STATE_LOAD_A ? soft_pair_a : soft_b_mem[soft_b_emit_index_q];
  assign tframe_in_symbol_start_sample =
      soft_state_q == SOFT_STATE_LOAD_A ? soft_pair_symbol_start_sample :
                                          soft_b_symbol_start_sample_q;

  iq_ingress #(
      .SAMPLE_WIDTH(SAMPLE_WIDTH)
  ) u_iq_ingress (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .set_sample_index_valid(set_sample_index_valid),
      .set_sample_index_value(set_sample_index_value),
      .in_valid(sdr_in_valid),
      .in_ready(sdr_in_ready),
      .in_i(sdr_in_i),
      .in_q(sdr_in_q),
      .out_valid(ingress_out_valid),
      .out_ready(ingress_out_ready),
      .out_i(ingress_out_i),
      .out_q(ingress_out_q),
      .out_sample_index(ingress_out_sample_index)
  );

  sample_history #(
      .SAMPLE_WIDTH(SAMPLE_WIDTH),
      .DEPTH_SAMPLES(SAMPLE_HISTORY_DEPTH)
  ) u_sample_history (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(sample_fork_valid),
      .in_ready(history_in_ready),
      .in_i(ingress_out_i),
      .in_q(ingress_out_q),
      .in_sample_index(ingress_out_sample_index),
      .read_req_valid(history_read_req_valid),
      .read_req_ready(history_read_req_ready),
      .read_req_start_sample(history_read_req_start_sample),
      .read_req_length(history_read_req_length),
      .rd_out_valid(history_rd_out_valid),
      .rd_out_ready(history_rd_out_ready),
      .rd_out_i(history_rd_out_i),
      .rd_out_q(history_rd_out_q),
      .rd_out_sample_index(history_rd_out_sample_index),
      .rd_out_first(history_rd_out_first),
      .rd_out_last(history_rd_out_last),
      .rd_out_error(history_rd_out_error),
      .history_valid(history_valid),
      .oldest_sample_index(history_oldest_sample_index),
      .next_sample_index(history_next_sample_index),
      .stored_samples(history_stored_samples),
      .history_full(history_full)
  );

  null_detector #(
      .SAMPLE_WIDTH(SAMPLE_WIDTH),
      .RATIO_WIDTH(RATIO_WIDTH),
      .FAST_WINDOW_SAMPLES(NULL_FAST_WINDOW_SAMPLES),
      .SLOW_WINDOW_SAMPLES(NULL_SLOW_WINDOW_SAMPLES),
      .MIN_NULL_SAMPLES(NULL_MIN_SAMPLES)
  ) u_null_detector (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(sample_fork_valid),
      .in_ready(null_in_ready),
      .in_i(ingress_out_i),
      .in_q(ingress_out_q),
      .in_sample_index(ingress_out_sample_index),
      .out_valid(null_out_valid),
      .out_ready(null_out_ready),
      .out_sample_index(null_out_sample_index),
      .out_length(null_out_length),
      .out_avg_power(null_out_avg_power),
      .out_min_ratio(null_out_min_ratio)
  );

  prs_acquire_track #(
      .SCORE_WIDTH(SCORE_WIDTH),
      .CFO_WIDTH(CFO_WIDTH),
      .RATIO_WIDTH(RATIO_WIDTH),
      .TIMING_WIDTH(TIMING_WIDTH),
      .GUARD_SAMPLES(GUARD_SAMPLES),
      .FFT_SAMPLES(FFT_SAMPLES)
  ) u_prs_acquire_track (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .cfg_enable_tracking(cfg_enable_tracking),
      .cfg_min_cp_score(cfg_min_cp_score),
      .cfg_min_pilot_score(cfg_min_pilot_score),
      .null_in_valid(null_out_valid),
      .null_in_ready(track_null_ready),
      .null_in_start_sample(null_out_sample_index),
      .null_in_length(null_out_length),
      .null_in_min_ratio(null_out_min_ratio),
      .predict_in_valid(predict_in_valid),
      .predict_in_ready(predict_in_ready),
      .predict_in_prs_fft_start_sample(predict_in_prs_fft_start_sample),
      .search_out_valid(search_valid),
      .search_out_ready(search_ready),
      .search_out_null_start_sample(search_null_start_sample),
      .search_out_null_length(search_null_length),
      .search_out_null_min_ratio(search_null_min_ratio),
      .search_out_prs_symbol_start_sample(search_prs_symbol_start_sample),
      .search_out_prs_fft_start_sample(search_prs_fft_start_sample),
      .search_out_timing_search_start_offset(search_timing_search_start_offset),
      .search_out_timing_search_length(search_timing_search_length),
      .search_out_search_start_sample(search_search_start_sample),
      .search_out_search_end_sample(search_search_end_sample),
      .search_out_required_end_sample(search_required_end_sample),
      .score_in_valid(score_valid),
      .score_in_ready(score_ready),
      .score_in_found(score_found),
      .score_in_null_start_sample(score_null_start_sample),
      .score_in_null_length(score_null_length),
      .score_in_candidate_prs_symbol_start_sample(score_candidate_prs_symbol_start_sample),
      .score_in_candidate_prs_fft_start_sample(score_candidate_prs_fft_start_sample),
      .score_in_timing_search_start_offset(score_timing_search_start_offset),
      .score_in_timing_search_length(score_timing_search_length),
      .score_in_search_start_sample(score_search_start_sample),
      .score_in_search_end_sample(score_search_end_sample),
      .score_in_required_end_sample(score_required_end_sample),
      .score_in_prs_peak_sample(score_prs_peak_sample),
      .score_in_prs_symbol_start_sample(score_prs_symbol_start_sample),
      .score_in_prs_fft_start_sample(score_prs_fft_start_sample),
      .score_in_prs_score(score_prs_score),
      .score_in_coarse_freq_error_hz(score_coarse_freq_error_hz),
      .prs_symbol_req_valid(prs_symbol_req_valid),
      .prs_symbol_req_ready(prs_symbol_req_ready),
      .prs_symbol_req_symbol_start_sample(prs_symbol_req_symbol_start_sample),
      .prs_symbol_req_fft_start_sample(prs_symbol_req_fft_start_sample),
      .prs_symbol_req_score(prs_symbol_req_score),
      .prs_symbol_req_coarse_freq_error_hz(prs_symbol_req_coarse_freq_error_hz),
      .pilot_in_valid(track_pilot_valid),
      .pilot_in_ready(track_pilot_ready),
      .pilot_in_found(track_pilot_found),
      .pilot_in_symbol_start_sample(track_pilot_symbol_start_sample),
      .pilot_in_score(track_pilot_score),
      .pilot_in_carrier_offset_bins(track_pilot_carrier_offset_bins),
      .pilot_in_fine_timing_error_samples(track_pilot_fine_timing_error_samples),
      .pilot_in_used_pairs(track_pilot_used_pairs),
      .out_valid(sync_out_valid),
      .out_ready(sync_out_ready),
      .out_found(sync_out_found),
      .out_locked(sync_out_locked),
      .out_lock_count(sync_out_lock_count),
      .out_miss_count(sync_out_miss_count),
      .out_prs_symbol_start_sample(sync_out_prs_symbol_start_sample),
      .out_prs_fft_start_sample(sync_out_prs_fft_start_sample),
      .out_prs_score(sync_out_prs_score),
      .out_coarse_freq_error_hz(sync_out_coarse_freq_error_hz),
      .out_pilot_score(sync_out_pilot_score),
      .out_carrier_offset_bins(sync_out_carrier_offset_bins),
      .out_fine_timing_error_samples(sync_out_fine_timing_error_samples),
      .out_pilot_used_pairs(sync_out_pilot_used_pairs),
      .out_next_prs_fft_start_sample(sync_out_next_prs_fft_start_sample)
  );

  prs_score_search #(
      .SCORE_WIDTH(SCORE_WIDTH),
      .CFO_WIDTH(CFO_WIDTH),
      .GUARD_SAMPLES(GUARD_SAMPLES)
  ) u_prs_score_search (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(search_engine_valid),
      .in_ready(search_engine_ready),
      .in_null_start_sample(search_null_start_sample),
      .in_null_length(search_null_length),
      .in_prs_symbol_start_sample(search_prs_symbol_start_sample),
      .in_prs_fft_start_sample(search_prs_fft_start_sample),
      .in_timing_search_start_offset(search_timing_search_start_offset),
      .in_timing_search_length(search_timing_search_length),
      .in_search_start_sample(search_search_start_sample),
      .in_search_end_sample(search_search_end_sample),
      .in_required_end_sample(search_required_end_sample),
      .metric_req_valid(metric_req_valid),
      .metric_req_ready(metric_req_ready),
      .metric_req_fft_start_sample(metric_req_fft_start_sample),
      .metric_req_index(metric_req_index),
      .metric_req_last(metric_req_last),
      .metric_resp_valid(metric_resp_valid),
      .metric_resp_ready(metric_resp_ready),
      .metric_resp_ok(metric_resp_ok),
      .metric_resp_fft_start_sample(metric_resp_fft_start_sample),
      .metric_resp_score(metric_resp_score),
      .metric_resp_coarse_freq_error_hz(metric_resp_coarse_freq_error_hz),
      .out_valid(score_valid),
      .out_ready(score_ready),
      .out_found(score_found),
      .out_null_start_sample(score_null_start_sample),
      .out_null_length(score_null_length),
      .out_candidate_prs_symbol_start_sample(score_candidate_prs_symbol_start_sample),
      .out_candidate_prs_fft_start_sample(score_candidate_prs_fft_start_sample),
      .out_timing_search_start_offset(score_timing_search_start_offset),
      .out_timing_search_length(score_timing_search_length),
      .out_search_start_sample(score_search_start_sample),
      .out_search_end_sample(score_search_end_sample),
      .out_required_end_sample(score_required_end_sample),
      .out_prs_peak_sample(score_prs_peak_sample),
      .out_prs_symbol_start_sample(score_prs_symbol_start_sample),
      .out_prs_fft_start_sample(score_prs_fft_start_sample),
      .out_prs_score(score_prs_score),
      .out_coarse_freq_error_hz(score_coarse_freq_error_hz)
  );

  cp_metric_engine #(
      .SAMPLE_WIDTH(SAMPLE_WIDTH),
      .SCORE_WIDTH(SCORE_WIDTH),
      .CFO_WIDTH(CFO_WIDTH),
      .GUARD_SAMPLES(GUARD_SAMPLES),
      .FFT_SAMPLES(FFT_SAMPLES)
  ) u_cp_metric_engine (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .metric_req_valid(metric_req_valid),
      .metric_req_ready(metric_req_ready),
      .metric_req_fft_start_sample(metric_req_fft_start_sample),
      .metric_req_index(metric_req_index),
      .metric_req_last(metric_req_last),
      .hist_read_req_valid(cp_hist_req_valid),
      .hist_read_req_ready(cp_hist_req_ready),
      .hist_read_req_start_sample(cp_hist_req_start_sample),
      .hist_read_req_length(cp_hist_req_length),
      .hist_rd_out_valid(cp_hist_rd_valid),
      .hist_rd_out_ready(cp_hist_rd_ready),
      .hist_rd_out_i(history_rd_out_i),
      .hist_rd_out_q(history_rd_out_q),
      .hist_rd_out_sample_index(history_rd_out_sample_index),
      .hist_rd_out_first(history_rd_out_first),
      .hist_rd_out_last(history_rd_out_last),
      .hist_rd_out_error(history_rd_out_error),
      .metric_resp_valid(metric_resp_valid),
      .metric_resp_ready(metric_resp_ready),
      .metric_resp_ok(metric_resp_ok),
      .metric_resp_fft_start_sample(metric_resp_fft_start_sample),
      .metric_resp_index(metric_resp_index),
      .metric_resp_last(metric_resp_last),
      .metric_resp_score(metric_resp_score),
      .metric_resp_coarse_freq_error_hz(metric_resp_coarse_freq_error_hz)
  );

  symbol_buffer #(
      .SAMPLE_WIDTH(SAMPLE_WIDTH),
      .FFT_SAMPLES(FFT_SAMPLES),
      .SAMPLE_OFFSET_WIDTH(DAB_OFDM_FFT_INDEX_WIDTH)
  ) u_symbol_buffer (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(symbol_cmd_valid),
      .in_ready(symbol_in_ready),
      .in_symbol_start_sample(symbol_cmd_fft_start_sample),
      .hist_read_req_valid(sym_hist_req_valid),
      .hist_read_req_ready(sym_hist_req_ready),
      .hist_read_req_start_sample(sym_hist_req_start_sample),
      .hist_read_req_length(sym_hist_req_length),
      .hist_rd_out_valid(sym_hist_rd_valid),
      .hist_rd_out_ready(sym_hist_rd_ready),
      .hist_rd_out_i(history_rd_out_i),
      .hist_rd_out_q(history_rd_out_q),
      .hist_rd_out_sample_index(history_rd_out_sample_index),
      .hist_rd_out_first(history_rd_out_first),
      .hist_rd_out_last(history_rd_out_last),
      .hist_rd_out_error(history_rd_out_error),
      .out_valid(symbol_out_valid),
      .out_ready(symbol_out_ready),
      .out_i(symbol_out_i),
      .out_q(symbol_out_q),
      .out_sample_index(symbol_out_sample_index),
      .out_symbol_start_sample(symbol_out_symbol_start_sample),
      .out_sample_offset(symbol_out_sample_offset),
      .out_symbol_done(symbol_out_done),
      .out_error(symbol_out_error)
  );

  cfo_corrector #(
      .INPUT_WIDTH(SAMPLE_WIDTH),
      .OUTPUT_WIDTH(DAB_OFDM_SAMPLE_WIDTH),
      .SAMPLE_OFFSET_WIDTH(DAB_OFDM_FFT_INDEX_WIDTH)
  ) u_cfo_corrector (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .cfg_bypass(cfg_cfo_bypass),
      .cfg_phase_inc(cfg_cfo_phase_inc),
      .cfg_phase_load(cfg_cfo_phase_load),
      .cfg_phase_seed(cfg_cfo_phase_seed),
      .in_valid(symbol_out_valid),
      .in_ready(symbol_out_ready),
      .in_i(symbol_out_i),
      .in_q(symbol_out_q),
      .in_sample_index(symbol_out_sample_index),
      .in_symbol_start_sample(symbol_out_symbol_start_sample),
      .in_sample_offset(symbol_out_sample_offset),
      .in_symbol_done(symbol_out_done),
      .in_error(symbol_out_error),
      .out_valid(cfo_out_valid),
      .out_ready(cfo_out_ready),
      .out_i(cfo_out_i),
      .out_q(cfo_out_q),
      .out_sample_index(cfo_out_sample_index),
      .out_symbol_start_sample(cfo_out_symbol_start_sample),
      .out_sample_offset(cfo_out_sample_offset),
      .out_symbol_done(cfo_out_symbol_done),
      .out_error(cfo_out_error),
      .out_phase(cfo_out_phase)
  );

  fft_wrapper u_fft_wrapper (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(fft_in_valid),
      .in_ready(fft_in_ready),
      .in_i(cfo_out_i),
      .in_q(cfo_out_q),
      .in_symbol_start_sample(cfo_out_symbol_start_sample),
      .core_in_valid(fft_core_in_valid),
      .core_in_ready(fft_core_in_ready),
      .core_in_i(fft_core_in_i),
      .core_in_q(fft_core_in_q),
      .core_in_last(fft_core_in_last),
      .core_out_valid(fft_core_out_valid),
      .core_out_ready(fft_core_out_ready),
      .core_out_i(fft_core_out_i),
      .core_out_q(fft_core_out_q),
      .core_out_block_exp(fft_core_out_block_exp),
      .core_out_overflow(fft_core_out_overflow),
      .out_valid(fft_out_valid),
      .out_ready(fft_out_ready),
      .out_i(fft_out_i),
      .out_q(fft_out_q),
      .out_fft_bin_index(fft_out_bin_index),
      .out_symbol_start_sample(fft_out_symbol_start_sample),
      .out_block_exp(fft_out_block_exp),
      .out_overflow(fft_out_overflow),
      .out_symbol_done(fft_out_symbol_done)
  );

  carrier_mapper u_carrier_mapper (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(fft_out_valid),
      .in_ready(fft_out_ready),
      .in_i(fft_out_i),
      .in_q(fft_out_q),
      .in_symbol_start_sample(fft_out_symbol_start_sample),
      .out_valid(active_valid),
      .out_ready(active_ready),
      .out_i(active_i),
      .out_q(active_q),
      .out_active_index(active_index),
      .out_fft_bin_index(active_fft_bin_index),
      .out_symbol_start_sample(active_symbol_start_sample),
      .out_symbol_done(active_symbol_done)
  );

  prs_pilot_refiner #(
      .CARRIER_WIDTH(DAB_OFDM_CARRIER_WIDTH),
      .SCORE_WIDTH(SCORE_WIDTH),
      .TIMING_WIDTH(TIMING_WIDTH)
  ) u_prs_pilot_refiner (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(prs_active_valid),
      .in_ready(pilot_in_ready),
      .in_i(active_i),
      .in_q(active_q),
      .in_active_index(active_index),
      .in_symbol_start_sample(active_symbol_start_sample),
      .in_symbol_done(active_symbol_done),
      .out_valid(pilot_valid),
      .out_ready(pilot_out_ready),
      .out_found(pilot_found),
      .out_symbol_start_sample(pilot_symbol_start_sample),
      .out_score(pilot_score),
      .out_carrier_offset_bins(pilot_carrier_offset_bins),
      .out_fine_timing_error_samples(pilot_fine_timing_error_samples),
      .out_used_pairs(pilot_used_pairs)
  );

  prs_channel_estimator u_prs_channel_estimator (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(prs_active_valid),
      .in_ready(chan_ready),
      .in_i(active_i),
      .in_q(active_q),
      .in_active_index(active_index),
      .in_symbol_start_sample(active_symbol_start_sample),
      .in_symbol_done(active_symbol_done),
      .coeff_valid(chan_coeff_valid),
      .coeff_ready(1'b1),
      .coeff_index(chan_coeff_index),
      .coeff_i(chan_coeff_i),
      .coeff_q(chan_coeff_q),
      .coeff_symbol_start_sample(chan_coeff_symbol_start_sample),
      .coeff_symbol_done(chan_coeff_symbol_done),
      .ref_valid(chan_ref_valid),
      .ref_ready(chan_ref_ready),
      .ref_i(chan_ref_i),
      .ref_q(chan_ref_q),
      .ref_active_index(chan_ref_active_index),
      .ref_symbol_start_sample(chan_ref_symbol_start_sample),
      .ref_symbol_done(chan_ref_symbol_done)
  );

  equalizer u_equalizer (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .coeff_we(chan_coeff_valid),
      .coeff_index(chan_coeff_index),
      .coeff_i(chan_coeff_i),
      .coeff_q(chan_coeff_q),
      .in_valid(eq_in_valid),
      .in_ready(eq_in_ready),
      .in_i(active_i),
      .in_q(active_q),
      .in_active_index(active_index),
      .in_symbol_start_sample(active_symbol_start_sample),
      .in_symbol_done(active_symbol_done),
      .out_valid(eq_out_valid),
      .out_ready(eq_out_ready),
      .out_i(eq_out_i),
      .out_q(eq_out_q),
      .out_active_index(eq_out_active_index),
      .out_symbol_start_sample(eq_out_symbol_start_sample),
      .out_symbol_done(eq_out_symbol_done)
  );

  diff_demod u_diff_demod (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(diff_in_valid),
      .in_ready(diff_in_ready),
      .in_i(diff_in_i),
      .in_q(diff_in_q),
      .in_active_index(diff_in_active_index),
      .in_symbol_start_sample(diff_in_symbol_start_sample),
      .in_symbol_done(diff_in_symbol_done),
      .in_seed_only(diff_in_seed_only),
      .out_valid(diff_out_valid),
      .out_ready(diff_out_ready),
      .out_i(diff_out_i),
      .out_q(diff_out_q),
      .out_active_index(diff_out_active_index),
      .out_symbol_start_sample(diff_out_symbol_start_sample),
      .out_symbol_done(diff_out_symbol_done)
  );

  frequency_deinterleaver u_frequency_deinterleaver (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(diff_out_valid),
      .in_ready(diff_out_ready),
      .in_i(diff_out_i),
      .in_q(diff_out_q),
      .in_symbol_start_sample(diff_out_symbol_start_sample),
      .out_valid(deint_out_valid),
      .out_ready(deint_out_ready),
      .out_i(deint_out_i),
      .out_q(deint_out_q),
      .out_carrier_index(deint_out_carrier_index),
      .out_source_index(deint_out_source_index),
      .out_symbol_start_sample(deint_out_symbol_start_sample),
      .out_symbol_done(deint_out_symbol_done)
  );

  qpsk_softbits u_qpsk_softbits (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(deint_out_valid),
      .in_ready(deint_out_ready),
      .in_i(deint_out_i),
      .in_q(deint_out_q),
      .in_carrier_index(deint_out_carrier_index),
      .in_symbol_start_sample(deint_out_symbol_start_sample),
      .in_symbol_done(deint_out_symbol_done),
      .out_valid(soft_pair_valid),
      .out_ready(soft_pair_ready),
      .out_soft_a(soft_pair_a),
      .out_soft_b(soft_pair_b),
      .out_carrier_index(soft_pair_carrier_index),
      .out_symbol_start_sample(soft_pair_symbol_start_sample),
      .out_symbol_done(soft_pair_symbol_done)
  );

  tframe_assembler u_tframe_assembler (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .in_valid(tframe_in_valid),
      .in_ready(tframe_in_ready),
      .in_softbit(tframe_in_softbit),
      .in_symbol_start_sample(tframe_in_symbol_start_sample),
      .out_valid(tframe_out_valid),
      .out_ready(tframe_out_ready),
      .out_softbit(tframe_out_softbit),
      .out_region(tframe_out_region),
      .out_addr(tframe_out_addr),
      .out_symbol_index(tframe_out_symbol_index),
      .out_byte_index(tframe_out_byte_index),
      .out_frame_start_sample(tframe_out_frame_start_sample),
      .out_frame_done(tframe_out_frame_done)
  );

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      read_owner_q <= READ_OWNER_NONE;
    end else if (clear) begin
      read_owner_q <= READ_OWNER_NONE;
    end else begin
      if (history_resp_done_fire) begin
        read_owner_q <= READ_OWNER_NONE;
      end else if (history_req_fire) begin
        read_owner_q <= cp_hist_req_valid ? READ_OWNER_CP : READ_OWNER_SYMBOL;
      end
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      symbol_active_q <= 1'b0;
      symbol_is_prs_q <= 1'b0;
    end else if (clear) begin
      symbol_active_q <= 1'b0;
      symbol_is_prs_q <= 1'b0;
    end else begin
      if (symbol_cmd_fire) begin
        symbol_active_q <= 1'b1;
        symbol_is_prs_q <= symbol_cmd_is_prs;
      end else if (symbol_done_fire || symbol_error_fire) begin
        symbol_active_q <= 1'b0;
      end
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      soft_state_q <= SOFT_STATE_LOAD_A;
      soft_b_emit_index_q <= '0;
      soft_b_symbol_start_sample_q <= '0;
    end else if (clear) begin
      soft_state_q <= SOFT_STATE_LOAD_A;
      soft_b_emit_index_q <= '0;
      soft_b_symbol_start_sample_q <= '0;
    end else begin
      if (soft_state_q == SOFT_STATE_LOAD_A) begin
        if (soft_pair_valid && soft_pair_ready) begin
          soft_b_mem[soft_pair_carrier_index] <= soft_pair_b;
          if (soft_pair_carrier_index == '0) begin
            soft_b_symbol_start_sample_q <= soft_pair_symbol_start_sample;
          end
          if (soft_pair_symbol_done) begin
            soft_state_q <= SOFT_STATE_EMIT_B;
            soft_b_emit_index_q <= '0;
          end
        end
      end else if (tframe_in_ready) begin
        if (soft_b_emit_index_q == LAST_DATA_INDEX) begin
          soft_state_q <= SOFT_STATE_LOAD_A;
          soft_b_emit_index_q <= '0;
        end else begin
          soft_b_emit_index_q <= soft_b_emit_index_q + 1'b1;
        end
      end
    end
  end
endmodule

`default_nettype wire
