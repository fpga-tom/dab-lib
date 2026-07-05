`default_nettype none

module dab_pipeline_top_quartus_fft
  import dab_ofdm_pkg::*, dab_tframe_pkg::*;
#(
    parameter int SAMPLE_WIDTH = 16,
    parameter int SAMPLE_HISTORY_DEPTH = 32768,
    parameter int ENABLE_INTERNAL_DATA_SCHEDULER = 1,
    parameter int ENABLE_STREAMING_CP_SEARCH = 1,
    parameter int NULL_FAST_WINDOW_SAMPLES = 39,
    parameter int NULL_SLOW_WINDOW_SAMPLES = 1999,
    parameter int NULL_MIN_SAMPLES = 128,
    parameter int SCORE_WIDTH = 32,
    parameter int CFO_WIDTH = 32,
    parameter int RATIO_WIDTH = 24,
    parameter int TIMING_WIDTH = 24,
    parameter int QPSK_EXACT_NORMALIZE = 0,
    parameter int ENABLE_CHANNEL_EQUALIZER = 0,
    parameter int GUARD_SAMPLES = 504,
    parameter int FFT_SAMPLES = DAB_OFDM_FFT_SAMPLES,
    parameter int FFT_IP_INPUT_WIDTH = DAB_OFDM_SAMPLE_WIDTH,
    parameter int FFT_IP_OUTPUT_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int FFT_IP_EXP_WIDTH = DAB_OFDM_BLOCK_EXP_WIDTH,
    parameter int FFT_IP_ERROR_WIDTH = 2
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic set_sample_index_valid,
    input  wire logic [63:0] set_sample_index_value,

    input  wire logic sdr_in_valid,
    output logic sdr_in_ready,
    input  wire logic signed [SAMPLE_WIDTH-1:0] sdr_in_i,
    input  wire logic signed [SAMPLE_WIDTH-1:0] sdr_in_q,

    input  wire logic cfg_enable_tracking,
    input  wire logic [SCORE_WIDTH-1:0] cfg_min_cp_score,
    input  wire logic [SCORE_WIDTH-1:0] cfg_min_pilot_score,

    input  wire logic cfg_cfo_bypass,
    input  wire logic [31:0] cfg_cfo_phase_inc,
    input  wire logic cfg_cfo_phase_load,
    input  wire logic [31:0] cfg_cfo_phase_seed,

    input  wire logic predict_in_valid,
    output logic predict_in_ready,
    input  wire logic [63:0] predict_in_prs_fft_start_sample,

    input  wire logic data_symbol_cmd_valid,
    output logic data_symbol_cmd_ready,
    input  wire logic [63:0] data_symbol_cmd_fft_start_sample,

    output logic sync_out_valid,
    input  wire logic sync_out_ready,
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
    input  wire logic tframe_out_ready,
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
  logic fft_core_in_valid;
  logic fft_core_in_ready;
  logic signed [DAB_OFDM_SAMPLE_WIDTH-1:0] fft_core_in_i;
  logic signed [DAB_OFDM_SAMPLE_WIDTH-1:0] fft_core_in_q;
  logic fft_core_in_last;

  logic fft_core_out_valid;
  logic fft_core_out_ready;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] fft_core_out_i;
  logic signed [DAB_OFDM_CARRIER_WIDTH-1:0] fft_core_out_q;
  logic [DAB_OFDM_BLOCK_EXP_WIDTH-1:0] fft_core_out_block_exp;
  logic fft_core_out_overflow;

  dab_pipeline_top #(
      .SAMPLE_WIDTH(SAMPLE_WIDTH),
      .SAMPLE_HISTORY_DEPTH(SAMPLE_HISTORY_DEPTH),
      .ENABLE_INTERNAL_DATA_SCHEDULER(ENABLE_INTERNAL_DATA_SCHEDULER),
      .ENABLE_STREAMING_CP_SEARCH(ENABLE_STREAMING_CP_SEARCH),
      .NULL_FAST_WINDOW_SAMPLES(NULL_FAST_WINDOW_SAMPLES),
      .NULL_SLOW_WINDOW_SAMPLES(NULL_SLOW_WINDOW_SAMPLES),
      .NULL_MIN_SAMPLES(NULL_MIN_SAMPLES),
      .SCORE_WIDTH(SCORE_WIDTH),
      .CFO_WIDTH(CFO_WIDTH),
      .RATIO_WIDTH(RATIO_WIDTH),
      .TIMING_WIDTH(TIMING_WIDTH),
      .QPSK_EXACT_NORMALIZE(QPSK_EXACT_NORMALIZE),
      .ENABLE_CHANNEL_EQUALIZER(ENABLE_CHANNEL_EQUALIZER),
      .GUARD_SAMPLES(GUARD_SAMPLES),
      .FFT_SAMPLES(FFT_SAMPLES)
  ) u_pipeline (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
      .set_sample_index_valid(set_sample_index_valid),
      .set_sample_index_value(set_sample_index_value),
      .sdr_in_valid(sdr_in_valid),
      .sdr_in_ready(sdr_in_ready),
      .sdr_in_i(sdr_in_i),
      .sdr_in_q(sdr_in_q),
      .cfg_enable_tracking(cfg_enable_tracking),
      .cfg_min_cp_score(cfg_min_cp_score),
      .cfg_min_pilot_score(cfg_min_pilot_score),
      .cfg_cfo_bypass(cfg_cfo_bypass),
      .cfg_cfo_phase_inc(cfg_cfo_phase_inc),
      .cfg_cfo_phase_load(cfg_cfo_phase_load),
      .cfg_cfo_phase_seed(cfg_cfo_phase_seed),
      .predict_in_valid(predict_in_valid),
      .predict_in_ready(predict_in_ready),
      .predict_in_prs_fft_start_sample(predict_in_prs_fft_start_sample),
      .data_symbol_cmd_valid(data_symbol_cmd_valid),
      .data_symbol_cmd_ready(data_symbol_cmd_ready),
      .data_symbol_cmd_fft_start_sample(data_symbol_cmd_fft_start_sample),
      .fft_core_in_valid(fft_core_in_valid),
      .fft_core_in_ready(fft_core_in_ready),
      .fft_core_in_i(fft_core_in_i),
      .fft_core_in_q(fft_core_in_q),
      .fft_core_in_last(fft_core_in_last),
      .fft_core_out_valid(fft_core_out_valid),
      .fft_core_out_ready(fft_core_out_ready),
      .fft_core_out_i(fft_core_out_i),
      .fft_core_out_q(fft_core_out_q),
      .fft_core_out_block_exp(fft_core_out_block_exp),
      .fft_core_out_overflow(fft_core_out_overflow),
      .sync_out_valid(sync_out_valid),
      .sync_out_ready(sync_out_ready),
      .sync_out_found(sync_out_found),
      .sync_out_locked(sync_out_locked),
      .sync_out_lock_count(sync_out_lock_count),
      .sync_out_miss_count(sync_out_miss_count),
      .sync_out_prs_symbol_start_sample(sync_out_prs_symbol_start_sample),
      .sync_out_prs_fft_start_sample(sync_out_prs_fft_start_sample),
      .sync_out_prs_score(sync_out_prs_score),
      .sync_out_coarse_freq_error_hz(sync_out_coarse_freq_error_hz),
      .sync_out_pilot_score(sync_out_pilot_score),
      .sync_out_carrier_offset_bins(sync_out_carrier_offset_bins),
      .sync_out_fine_timing_error_samples(sync_out_fine_timing_error_samples),
      .sync_out_pilot_used_pairs(sync_out_pilot_used_pairs),
      .sync_out_next_prs_fft_start_sample(sync_out_next_prs_fft_start_sample),
      .tframe_out_valid(tframe_out_valid),
      .tframe_out_ready(tframe_out_ready),
      .tframe_out_softbit(tframe_out_softbit),
      .tframe_out_region(tframe_out_region),
      .tframe_out_addr(tframe_out_addr),
      .tframe_out_symbol_index(tframe_out_symbol_index),
      .tframe_out_byte_index(tframe_out_byte_index),
      .tframe_out_frame_start_sample(tframe_out_frame_start_sample),
      .tframe_out_frame_done(tframe_out_frame_done),
      .history_valid(history_valid),
      .history_oldest_sample_index(history_oldest_sample_index),
      .history_next_sample_index(history_next_sample_index),
      .history_stored_samples(history_stored_samples),
      .history_full(history_full),
      .symbol_busy(symbol_busy)
  );

  altera_fft_avalon_core #(
      .INPUT_WIDTH(DAB_OFDM_SAMPLE_WIDTH),
      .OUTPUT_WIDTH(DAB_OFDM_CARRIER_WIDTH),
      .BLOCK_EXP_WIDTH(DAB_OFDM_BLOCK_EXP_WIDTH),
      .IP_INPUT_WIDTH(FFT_IP_INPUT_WIDTH),
      .IP_OUTPUT_WIDTH(FFT_IP_OUTPUT_WIDTH),
      .IP_EXP_WIDTH(FFT_IP_EXP_WIDTH),
      .IP_ERROR_WIDTH(FFT_IP_ERROR_WIDTH),
      .INVERSE(1'b0)
  ) u_fft_core (
      .clk(clk),
      .rst_n(rst_n),
      .clear(clear),
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
      .core_out_overflow(fft_core_out_overflow)
  );
endmodule

`default_nettype wire
