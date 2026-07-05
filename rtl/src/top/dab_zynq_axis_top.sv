`default_nettype none

// Zynq-friendly shell around dab_pipeline_top_custom_fft.
//
// Register map, 32-bit AXI-Lite words:
//   0x00 CONTROL
//        bit 0  run_enable RW
//        bit 1  tracking_enable RW
//        bit 2  clear_pipeline W1P
//        bit 3  set_sample_index W1P
//        bit 4  clear_status W1P
//        bit 8  read-only ENABLE_CFO_CORRECTOR generic
//        bit 9  read-only ENABLE_INTERNAL_CFO_CONTROL generic
//        bit 10 read-only ENABLE_INTERNAL_DATA_SCHEDULER generic
//   0x04 STATUS
//        bit 0  run_enable
//        bit 1  history_valid
//        bit 2  history_full
//        bit 3  symbol_busy
//        bit 4  softbit output word pending
//        bit 5  sync event sticky
//        bit 6  found-sync sticky
//        bit 7  frame-done sticky
//        bit 8  softbit output backpressured
//        bit 9  IQ input backpressured
//   0x08 MIN_CP_SCORE_Q30
//   0x0c MIN_PILOT_SCORE_Q30
//   0x10 CFO_CONTROL
//        bit 0  cfg_cfo_bypass RW
//        bit 1  cfg_cfo_phase_load W1P
//   0x14 CFO_PHASE_INC
//   0x18 CFO_PHASE_SEED
//   0x1c SET_SAMPLE_INDEX_LO
//   0x20 SET_SAMPLE_INDEX_HI
//   0x24 SAMPLE_COUNT_LO
//   0x28 SAMPLE_COUNT_HI
//   0x2c SOFTBIT_COUNT_LO
//   0x30 SOFTBIT_COUNT_HI
//   0x34 SOFTWORD_COUNT_LO
//   0x38 SOFTWORD_COUNT_HI
//   0x3c FRAME_COUNT
//   0x40 SYNC_COUNT
//   0x44 SYNC_FOUND_COUNT
//   0x48 LATEST_SYNC_FLAGS
//   0x4c LATEST_PRS_SCORE
//   0x50 LATEST_PILOT_SCORE
//   0x54 LATEST_CFO_HZ
//   0x58 LATEST_FINE_TIMING_Q12
//   0x5c LATEST_PRS_FFT_START_LO
//   0x60 LATEST_PRS_FFT_START_HI
//   0x64 LATEST_NEXT_PRS_FFT_START_LO
//   0x68 LATEST_NEXT_PRS_FFT_START_HI
//   0x6c LATEST_FRAME_START_LO
//   0x70 LATEST_FRAME_START_HI
//   0x74 HISTORY_STORED_SAMPLES
//   0x78 HISTORY_OLDEST_SAMPLE_LO
//   0x7c HISTORY_OLDEST_SAMPLE_HI
//   0x80 HISTORY_NEXT_SAMPLE_LO
//   0x84 HISTORY_NEXT_SAMPLE_HI
//   0x88 LATEST_PILOT_USED_PAIRS

module dab_zynq_axis_top
  import dab_ofdm_pkg::*, dab_tframe_pkg::*;
#(
    parameter int AXIL_ADDR_WIDTH = 8,
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
    parameter int QPSK_RAW_SHIFT = 9,
    parameter int ENABLE_CHANNEL_EQUALIZER = 0,
    parameter int ENABLE_CFO_CORRECTOR = 0,
    parameter int ENABLE_INTERNAL_CFO_CONTROL = 0,
    parameter int CFO_CONTROL_GAIN_FRAC_BITS = 16,
    parameter int CFO_CONTROL_GAIN_SCALE_Q = 68719477,
    parameter int GUARD_SAMPLES = 504,
    parameter int FFT_SAMPLES = DAB_OFDM_FFT_SAMPLES,
    parameter int FFT_APPLY_BLOCK_EXP = 1,
    parameter int FFT_INTERNAL_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int FFT_TWIDDLE_WIDTH = 18,
    parameter int FFT_TWIDDLE_FRAC_BITS = 16
) (
    input  wire logic s_axi_aclk,
    input  wire logic s_axi_aresetn,

    input  wire logic [AXIL_ADDR_WIDTH-1:0] s_axi_awaddr,
    input  wire logic [2:0] s_axi_awprot,
    input  wire logic s_axi_awvalid,
    output logic s_axi_awready,
    input  wire logic [31:0] s_axi_wdata,
    input  wire logic [3:0] s_axi_wstrb,
    input  wire logic s_axi_wvalid,
    output logic s_axi_wready,
    output logic [1:0] s_axi_bresp,
    output logic s_axi_bvalid,
    input  wire logic s_axi_bready,
    input  wire logic [AXIL_ADDR_WIDTH-1:0] s_axi_araddr,
    input  wire logic [2:0] s_axi_arprot,
    input  wire logic s_axi_arvalid,
    output logic s_axi_arready,
    output logic [31:0] s_axi_rdata,
    output logic [1:0] s_axi_rresp,
    output logic s_axi_rvalid,
    input  wire logic s_axi_rready,

    input  wire logic s_axis_iq_tvalid,
    output logic s_axis_iq_tready,
    input  wire logic [2*SAMPLE_WIDTH-1:0] s_axis_iq_tdata,
    input  wire logic [(2*SAMPLE_WIDTH+7)/8-1:0] s_axis_iq_tkeep,
    input  wire logic s_axis_iq_tlast,

    output logic m_axis_softbit_tvalid,
    input  wire logic m_axis_softbit_tready,
    output logic [31:0] m_axis_softbit_tdata,
    output logic [3:0] m_axis_softbit_tkeep,
    output logic m_axis_softbit_tlast
);
  localparam logic [5:0] REG_CONTROL = 6'h00;
  localparam logic [5:0] REG_STATUS = 6'h01;
  localparam logic [5:0] REG_MIN_CP_SCORE = 6'h02;
  localparam logic [5:0] REG_MIN_PILOT_SCORE = 6'h03;
  localparam logic [5:0] REG_CFO_CONTROL = 6'h04;
  localparam logic [5:0] REG_CFO_PHASE_INC = 6'h05;
  localparam logic [5:0] REG_CFO_PHASE_SEED = 6'h06;
  localparam logic [5:0] REG_SET_SAMPLE_INDEX_LO = 6'h07;
  localparam logic [5:0] REG_SET_SAMPLE_INDEX_HI = 6'h08;
  localparam logic [5:0] REG_SAMPLE_COUNT_LO = 6'h09;
  localparam logic [5:0] REG_SAMPLE_COUNT_HI = 6'h0a;
  localparam logic [5:0] REG_SOFTBIT_COUNT_LO = 6'h0b;
  localparam logic [5:0] REG_SOFTBIT_COUNT_HI = 6'h0c;
  localparam logic [5:0] REG_SOFTWORD_COUNT_LO = 6'h0d;
  localparam logic [5:0] REG_SOFTWORD_COUNT_HI = 6'h0e;
  localparam logic [5:0] REG_FRAME_COUNT = 6'h0f;
  localparam logic [5:0] REG_SYNC_COUNT = 6'h10;
  localparam logic [5:0] REG_SYNC_FOUND_COUNT = 6'h11;
  localparam logic [5:0] REG_LATEST_SYNC_FLAGS = 6'h12;
  localparam logic [5:0] REG_LATEST_PRS_SCORE = 6'h13;
  localparam logic [5:0] REG_LATEST_PILOT_SCORE = 6'h14;
  localparam logic [5:0] REG_LATEST_CFO_HZ = 6'h15;
  localparam logic [5:0] REG_LATEST_FINE_TIMING = 6'h16;
  localparam logic [5:0] REG_LATEST_PRS_FFT_START_LO = 6'h17;
  localparam logic [5:0] REG_LATEST_PRS_FFT_START_HI = 6'h18;
  localparam logic [5:0] REG_LATEST_NEXT_PRS_FFT_START_LO = 6'h19;
  localparam logic [5:0] REG_LATEST_NEXT_PRS_FFT_START_HI = 6'h1a;
  localparam logic [5:0] REG_LATEST_FRAME_START_LO = 6'h1b;
  localparam logic [5:0] REG_LATEST_FRAME_START_HI = 6'h1c;
  localparam logic [5:0] REG_HISTORY_STORED_SAMPLES = 6'h1d;
  localparam logic [5:0] REG_HISTORY_OLDEST_SAMPLE_LO = 6'h1e;
  localparam logic [5:0] REG_HISTORY_OLDEST_SAMPLE_HI = 6'h1f;
  localparam logic [5:0] REG_HISTORY_NEXT_SAMPLE_LO = 6'h20;
  localparam logic [5:0] REG_HISTORY_NEXT_SAMPLE_HI = 6'h21;
  localparam logic [5:0] REG_LATEST_PILOT_USED_PAIRS = 6'h22;

  localparam logic [31:0] DEFAULT_MIN_CP_SCORE_Q30 = 32'h1999_999a;
  localparam logic [31:0] DEFAULT_MIN_PILOT_SCORE_Q30 = 32'h2000_0000;

  logic aw_pending_q;
  logic [5:0] awaddr_word_q;
  logic w_pending_q;
  logic [31:0] wdata_q;
  logic [3:0] wstrb_q;

  logic run_enable_q;
  logic tracking_enable_q;
  logic clear_pulse_q;
  logic clear_status_pulse_q;
  logic set_sample_index_pulse_q;
  logic [63:0] set_sample_index_value_q;
  logic [31:0] cfg_min_cp_score_q;
  logic [31:0] cfg_min_pilot_score_q;
  logic cfg_cfo_bypass_q;
  logic [31:0] cfg_cfo_phase_inc_q;
  logic cfg_cfo_phase_load_pulse_q;
  logic [31:0] cfg_cfo_phase_seed_q;

  logic sdr_in_valid;
  logic sdr_in_ready;
  logic signed [SAMPLE_WIDTH-1:0] sdr_in_i;
  logic signed [SAMPLE_WIDTH-1:0] sdr_in_q;

  logic sync_out_valid;
  logic sync_out_found;
  logic sync_out_locked;
  logic [7:0] sync_out_lock_count;
  logic [7:0] sync_out_miss_count;
  logic [63:0] sync_out_prs_symbol_start_sample;
  logic [63:0] sync_out_prs_fft_start_sample;
  logic [SCORE_WIDTH-1:0] sync_out_prs_score;
  logic signed [CFO_WIDTH-1:0] sync_out_coarse_freq_error_hz;
  logic [SCORE_WIDTH-1:0] sync_out_pilot_score;
  logic signed [3:0] sync_out_carrier_offset_bins;
  logic signed [TIMING_WIDTH-1:0] sync_out_fine_timing_error_samples;
  logic [11:0] sync_out_pilot_used_pairs;
  logic [63:0] sync_out_next_prs_fft_start_sample;

  logic pipe_tframe_out_valid;
  logic pipe_tframe_out_ready;
  logic signed [DAB_TFRAME_SOFTBIT_WIDTH-1:0] pipe_tframe_out_softbit;
  /* verilator lint_off UNUSEDSIGNAL */
  logic pipe_tframe_out_region;
  logic [DAB_TFRAME_ADDR_WIDTH-1:0] pipe_tframe_out_addr;
  logic [DAB_TFRAME_SYMBOL_INDEX_WIDTH-1:0] pipe_tframe_out_symbol_index;
  logic [DAB_TFRAME_BYTE_INDEX_WIDTH-1:0] pipe_tframe_out_byte_index;
  logic predict_in_ready_unused;
  logic data_symbol_cmd_ready_unused;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [63:0] pipe_tframe_out_frame_start_sample;
  logic pipe_tframe_out_frame_done;

  logic history_valid;
  logic [63:0] history_oldest_sample_index;
  logic [63:0] history_next_sample_index;
  logic [31:0] history_stored_samples;
  logic history_full;
  logic symbol_busy;

  logic [1:0] soft_pack_count_q;
  logic [31:0] soft_pack_data_q;
  logic soft_word_valid_q;
  logic [31:0] soft_word_data_q;
  logic [3:0] soft_word_keep_q;
  logic soft_word_last_q;

  logic sync_seen_sticky_q;
  logic sync_found_sticky_q;
  logic frame_done_sticky_q;
  logic [63:0] sample_count_q;
  logic [63:0] softbit_count_q;
  logic [63:0] softword_count_q;
  logic [31:0] frame_count_q;
  logic [31:0] sync_count_q;
  logic [31:0] sync_found_count_q;
  logic latest_sync_found_q;
  logic latest_sync_locked_q;
  logic [7:0] latest_sync_lock_count_q;
  logic [7:0] latest_sync_miss_count_q;
  logic [11:0] latest_sync_pilot_used_pairs_q;
  logic signed [3:0] latest_sync_carrier_offset_q;
  logic [31:0] latest_sync_prs_score_q;
  logic [31:0] latest_sync_pilot_score_q;
  logic signed [31:0] latest_sync_cfo_hz_q;
  logic signed [31:0] latest_sync_fine_timing_q;
  logic [63:0] latest_sync_prs_symbol_start_q;
  logic [63:0] latest_sync_prs_fft_start_q;
  logic [63:0] latest_sync_next_prs_fft_start_q;
  logic [63:0] latest_frame_start_q;

  wire logic sample_fire = sdr_in_valid && sdr_in_ready;
  wire logic softbit_fire = pipe_tframe_out_valid && pipe_tframe_out_ready;
  wire logic softword_fire = soft_word_valid_q && m_axis_softbit_tready;
  wire logic sync_fire = sync_out_valid;

  /* verilator lint_off UNUSEDSIGNAL */
  wire logic [1:0] unused_awaddr_lsb = s_axi_awaddr[1:0];
  wire logic [1:0] unused_araddr_lsb = s_axi_araddr[1:0];
  wire logic [2:0] unused_awprot = s_axi_awprot;
  wire logic [2:0] unused_arprot = s_axi_arprot;
  wire logic [(2*SAMPLE_WIDTH+7)/8-1:0] unused_iq_tkeep = s_axis_iq_tkeep;
  wire logic unused_iq_tlast = s_axis_iq_tlast;
  wire logic [63:0] unused_latest_sync_prs_symbol_start =
      latest_sync_prs_symbol_start_q;
  /* verilator lint_on UNUSEDSIGNAL */

  function automatic logic [31:0] apply_wstrb(
      input logic [31:0] prior,
      input logic [31:0] data,
      input logic [3:0] strb
  );
    integer i;
    begin
      apply_wstrb = prior;
      for (i = 0; i < 4; i = i + 1) begin
        if (strb[i]) begin
          apply_wstrb[i*8 +: 8] = data[i*8 +: 8];
        end
      end
    end
  endfunction

  function automatic logic [3:0] keep_for_last_index(input logic [1:0] index);
    begin
      unique case (index)
        2'd0: keep_for_last_index = 4'b0001;
        2'd1: keep_for_last_index = 4'b0011;
        2'd2: keep_for_last_index = 4'b0111;
        default: keep_for_last_index = 4'b1111;
      endcase
    end
  endfunction

  function automatic logic [31:0] control_read_value();
    begin
      control_read_value = 32'd0;
      control_read_value[0] = run_enable_q;
      control_read_value[1] = tracking_enable_q;
      control_read_value[8] = ENABLE_CFO_CORRECTOR != 0;
      control_read_value[9] = ENABLE_INTERNAL_CFO_CONTROL != 0;
      control_read_value[10] = ENABLE_INTERNAL_DATA_SCHEDULER != 0;
    end
  endfunction

  function automatic logic [31:0] cfo_control_read_value();
    begin
      cfo_control_read_value = 32'd0;
      cfo_control_read_value[0] = cfg_cfo_bypass_q;
    end
  endfunction

  function automatic logic [31:0] status_read_value();
    begin
      status_read_value = 32'd0;
      status_read_value[0] = run_enable_q;
      status_read_value[1] = history_valid;
      status_read_value[2] = history_full;
      status_read_value[3] = symbol_busy;
      status_read_value[4] = soft_word_valid_q;
      status_read_value[5] = sync_seen_sticky_q;
      status_read_value[6] = sync_found_sticky_q;
      status_read_value[7] = frame_done_sticky_q;
      status_read_value[8] = soft_word_valid_q && !m_axis_softbit_tready;
      status_read_value[9] = s_axis_iq_tvalid && !s_axis_iq_tready;
    end
  endfunction

  function automatic logic [31:0] latest_sync_flags_read_value();
    begin
      latest_sync_flags_read_value = 32'd0;
      latest_sync_flags_read_value[0] = latest_sync_found_q;
      latest_sync_flags_read_value[1] = latest_sync_locked_q;
      latest_sync_flags_read_value[15:8] = latest_sync_lock_count_q;
      latest_sync_flags_read_value[23:16] = latest_sync_miss_count_q;
      latest_sync_flags_read_value[27:24] = latest_sync_carrier_offset_q;
      latest_sync_flags_read_value[31:28] = latest_sync_pilot_used_pairs_q[3:0];
    end
  endfunction

  function automatic logic [31:0] read_reg(input logic [5:0] addr_word);
    begin
      unique case (addr_word)
        REG_CONTROL: read_reg = control_read_value();
        REG_STATUS: read_reg = status_read_value();
        REG_MIN_CP_SCORE: read_reg = cfg_min_cp_score_q;
        REG_MIN_PILOT_SCORE: read_reg = cfg_min_pilot_score_q;
        REG_CFO_CONTROL: read_reg = cfo_control_read_value();
        REG_CFO_PHASE_INC: read_reg = cfg_cfo_phase_inc_q;
        REG_CFO_PHASE_SEED: read_reg = cfg_cfo_phase_seed_q;
        REG_SET_SAMPLE_INDEX_LO: read_reg = set_sample_index_value_q[31:0];
        REG_SET_SAMPLE_INDEX_HI: read_reg = set_sample_index_value_q[63:32];
        REG_SAMPLE_COUNT_LO: read_reg = sample_count_q[31:0];
        REG_SAMPLE_COUNT_HI: read_reg = sample_count_q[63:32];
        REG_SOFTBIT_COUNT_LO: read_reg = softbit_count_q[31:0];
        REG_SOFTBIT_COUNT_HI: read_reg = softbit_count_q[63:32];
        REG_SOFTWORD_COUNT_LO: read_reg = softword_count_q[31:0];
        REG_SOFTWORD_COUNT_HI: read_reg = softword_count_q[63:32];
        REG_FRAME_COUNT: read_reg = frame_count_q;
        REG_SYNC_COUNT: read_reg = sync_count_q;
        REG_SYNC_FOUND_COUNT: read_reg = sync_found_count_q;
        REG_LATEST_SYNC_FLAGS: read_reg = latest_sync_flags_read_value();
        REG_LATEST_PRS_SCORE: read_reg = latest_sync_prs_score_q;
        REG_LATEST_PILOT_SCORE: read_reg = latest_sync_pilot_score_q;
        REG_LATEST_CFO_HZ: read_reg = latest_sync_cfo_hz_q;
        REG_LATEST_FINE_TIMING: read_reg = latest_sync_fine_timing_q;
        REG_LATEST_PRS_FFT_START_LO: read_reg = latest_sync_prs_fft_start_q[31:0];
        REG_LATEST_PRS_FFT_START_HI: read_reg = latest_sync_prs_fft_start_q[63:32];
        REG_LATEST_NEXT_PRS_FFT_START_LO:
          read_reg = latest_sync_next_prs_fft_start_q[31:0];
        REG_LATEST_NEXT_PRS_FFT_START_HI:
          read_reg = latest_sync_next_prs_fft_start_q[63:32];
        REG_LATEST_FRAME_START_LO: read_reg = latest_frame_start_q[31:0];
        REG_LATEST_FRAME_START_HI: read_reg = latest_frame_start_q[63:32];
        REG_HISTORY_STORED_SAMPLES: read_reg = history_stored_samples;
        REG_HISTORY_OLDEST_SAMPLE_LO: read_reg = history_oldest_sample_index[31:0];
        REG_HISTORY_OLDEST_SAMPLE_HI: read_reg = history_oldest_sample_index[63:32];
        REG_HISTORY_NEXT_SAMPLE_LO: read_reg = history_next_sample_index[31:0];
        REG_HISTORY_NEXT_SAMPLE_HI: read_reg = history_next_sample_index[63:32];
        REG_LATEST_PILOT_USED_PAIRS: read_reg = {20'd0, latest_sync_pilot_used_pairs_q};
        default: read_reg = 32'd0;
      endcase
    end
  endfunction

  assign s_axi_awready = !aw_pending_q && !s_axi_bvalid;
  assign s_axi_wready = !w_pending_q && !s_axi_bvalid;
  assign s_axi_bresp = 2'b00;
  assign s_axi_arready = !s_axi_rvalid;
  assign s_axi_rresp = 2'b00;

  always_ff @(posedge s_axi_aclk or negedge s_axi_aresetn) begin
    if (!s_axi_aresetn) begin
      aw_pending_q <= 1'b0;
      awaddr_word_q <= '0;
      w_pending_q <= 1'b0;
      wdata_q <= '0;
      wstrb_q <= '0;
      s_axi_bvalid <= 1'b0;
      s_axi_rvalid <= 1'b0;
      s_axi_rdata <= '0;
      run_enable_q <= 1'b0;
      tracking_enable_q <= 1'b1;
      clear_pulse_q <= 1'b0;
      clear_status_pulse_q <= 1'b0;
      set_sample_index_pulse_q <= 1'b0;
      set_sample_index_value_q <= '0;
      cfg_min_cp_score_q <= DEFAULT_MIN_CP_SCORE_Q30;
      cfg_min_pilot_score_q <= DEFAULT_MIN_PILOT_SCORE_Q30;
      cfg_cfo_bypass_q <= 1'b1;
      cfg_cfo_phase_inc_q <= '0;
      cfg_cfo_phase_load_pulse_q <= 1'b0;
      cfg_cfo_phase_seed_q <= '0;
    end else begin
      clear_pulse_q <= 1'b0;
      clear_status_pulse_q <= 1'b0;
      set_sample_index_pulse_q <= 1'b0;
      cfg_cfo_phase_load_pulse_q <= 1'b0;

      if (s_axi_awready && s_axi_awvalid) begin
        aw_pending_q <= 1'b1;
        awaddr_word_q <= s_axi_awaddr[7:2];
      end

      if (s_axi_wready && s_axi_wvalid) begin
        w_pending_q <= 1'b1;
        wdata_q <= s_axi_wdata;
        wstrb_q <= s_axi_wstrb;
      end

      if (!s_axi_bvalid && aw_pending_q && w_pending_q) begin
        unique case (awaddr_word_q)
          REG_CONTROL: begin
            if (wstrb_q[0]) begin
              run_enable_q <= wdata_q[0];
              tracking_enable_q <= wdata_q[1];
              clear_pulse_q <= wdata_q[2];
              set_sample_index_pulse_q <= wdata_q[3];
              clear_status_pulse_q <= wdata_q[4];
            end
          end
          REG_MIN_CP_SCORE: begin
            cfg_min_cp_score_q <= apply_wstrb(cfg_min_cp_score_q, wdata_q, wstrb_q);
          end
          REG_MIN_PILOT_SCORE: begin
            cfg_min_pilot_score_q <= apply_wstrb(cfg_min_pilot_score_q, wdata_q, wstrb_q);
          end
          REG_CFO_CONTROL: begin
            if (wstrb_q[0]) begin
              cfg_cfo_bypass_q <= wdata_q[0];
              cfg_cfo_phase_load_pulse_q <= wdata_q[1];
            end
          end
          REG_CFO_PHASE_INC: begin
            cfg_cfo_phase_inc_q <= apply_wstrb(cfg_cfo_phase_inc_q, wdata_q, wstrb_q);
          end
          REG_CFO_PHASE_SEED: begin
            cfg_cfo_phase_seed_q <= apply_wstrb(cfg_cfo_phase_seed_q, wdata_q, wstrb_q);
          end
          REG_SET_SAMPLE_INDEX_LO: begin
            set_sample_index_value_q[31:0] <=
                apply_wstrb(set_sample_index_value_q[31:0], wdata_q, wstrb_q);
          end
          REG_SET_SAMPLE_INDEX_HI: begin
            set_sample_index_value_q[63:32] <=
                apply_wstrb(set_sample_index_value_q[63:32], wdata_q, wstrb_q);
          end
          default: begin
          end
        endcase
        aw_pending_q <= 1'b0;
        w_pending_q <= 1'b0;
        s_axi_bvalid <= 1'b1;
      end else if (s_axi_bvalid && s_axi_bready) begin
        s_axi_bvalid <= 1'b0;
      end

      if (s_axi_arready && s_axi_arvalid) begin
        s_axi_rvalid <= 1'b1;
        s_axi_rdata <= read_reg(s_axi_araddr[7:2]);
      end else if (s_axi_rvalid && s_axi_rready) begin
        s_axi_rvalid <= 1'b0;
      end
    end
  end

  assign sdr_in_valid = run_enable_q && !clear_pulse_q && s_axis_iq_tvalid;
  assign s_axis_iq_tready = run_enable_q && !clear_pulse_q && sdr_in_ready;
  assign sdr_in_i = s_axis_iq_tdata[SAMPLE_WIDTH-1:0];
  assign sdr_in_q = s_axis_iq_tdata[2*SAMPLE_WIDTH-1:SAMPLE_WIDTH];

  assign pipe_tframe_out_ready =
      run_enable_q && !clear_pulse_q && !soft_word_valid_q;
  assign m_axis_softbit_tvalid = soft_word_valid_q;
  assign m_axis_softbit_tdata = soft_word_data_q;
  assign m_axis_softbit_tkeep = soft_word_keep_q;
  assign m_axis_softbit_tlast = soft_word_last_q;

  always_ff @(posedge s_axi_aclk or negedge s_axi_aresetn) begin
    logic [31:0] next_soft_pack_data;

    if (!s_axi_aresetn) begin
      soft_pack_count_q <= '0;
      soft_pack_data_q <= '0;
      soft_word_valid_q <= 1'b0;
      soft_word_data_q <= '0;
      soft_word_keep_q <= '0;
      soft_word_last_q <= 1'b0;
    end else if (clear_pulse_q) begin
      soft_pack_count_q <= '0;
      soft_pack_data_q <= '0;
      soft_word_valid_q <= 1'b0;
      soft_word_data_q <= '0;
      soft_word_keep_q <= '0;
      soft_word_last_q <= 1'b0;
    end else begin
      if (softword_fire) begin
        soft_word_valid_q <= 1'b0;
        soft_word_last_q <= 1'b0;
      end

      if (softbit_fire) begin
        next_soft_pack_data = soft_pack_data_q;
        unique case (soft_pack_count_q)
          2'd0: next_soft_pack_data[7:0] = pipe_tframe_out_softbit;
          2'd1: next_soft_pack_data[15:8] = pipe_tframe_out_softbit;
          2'd2: next_soft_pack_data[23:16] = pipe_tframe_out_softbit;
          default: next_soft_pack_data[31:24] = pipe_tframe_out_softbit;
        endcase

        if (soft_pack_count_q == 2'd3 || pipe_tframe_out_frame_done) begin
          soft_word_valid_q <= 1'b1;
          soft_word_data_q <= next_soft_pack_data;
          soft_word_keep_q <= keep_for_last_index(soft_pack_count_q);
          soft_word_last_q <= pipe_tframe_out_frame_done;
          soft_pack_count_q <= '0;
          soft_pack_data_q <= '0;
        end else begin
          soft_pack_count_q <= soft_pack_count_q + 2'd1;
          soft_pack_data_q <= next_soft_pack_data;
        end
      end
    end
  end

  always_ff @(posedge s_axi_aclk or negedge s_axi_aresetn) begin
    if (!s_axi_aresetn) begin
      sync_seen_sticky_q <= 1'b0;
      sync_found_sticky_q <= 1'b0;
      frame_done_sticky_q <= 1'b0;
      sample_count_q <= '0;
      softbit_count_q <= '0;
      softword_count_q <= '0;
      frame_count_q <= '0;
      sync_count_q <= '0;
      sync_found_count_q <= '0;
      latest_sync_found_q <= 1'b0;
      latest_sync_locked_q <= 1'b0;
      latest_sync_lock_count_q <= '0;
      latest_sync_miss_count_q <= '0;
      latest_sync_pilot_used_pairs_q <= '0;
      latest_sync_carrier_offset_q <= '0;
      latest_sync_prs_score_q <= '0;
      latest_sync_pilot_score_q <= '0;
      latest_sync_cfo_hz_q <= '0;
      latest_sync_fine_timing_q <= '0;
      latest_sync_prs_symbol_start_q <= '0;
      latest_sync_prs_fft_start_q <= '0;
      latest_sync_next_prs_fft_start_q <= '0;
      latest_frame_start_q <= '0;
    end else if (clear_pulse_q || clear_status_pulse_q) begin
      sync_seen_sticky_q <= 1'b0;
      sync_found_sticky_q <= 1'b0;
      frame_done_sticky_q <= 1'b0;
      sample_count_q <= '0;
      softbit_count_q <= '0;
      softword_count_q <= '0;
      frame_count_q <= '0;
      sync_count_q <= '0;
      sync_found_count_q <= '0;
      latest_sync_found_q <= 1'b0;
      latest_sync_locked_q <= 1'b0;
      latest_sync_lock_count_q <= '0;
      latest_sync_miss_count_q <= '0;
      latest_sync_pilot_used_pairs_q <= '0;
      latest_sync_carrier_offset_q <= '0;
      latest_sync_prs_score_q <= '0;
      latest_sync_pilot_score_q <= '0;
      latest_sync_cfo_hz_q <= '0;
      latest_sync_fine_timing_q <= '0;
      latest_sync_prs_symbol_start_q <= '0;
      latest_sync_prs_fft_start_q <= '0;
      latest_sync_next_prs_fft_start_q <= '0;
      latest_frame_start_q <= '0;
    end else begin
      if (sample_fire) begin
        sample_count_q <= sample_count_q + 64'd1;
      end
      if (softbit_fire) begin
        softbit_count_q <= softbit_count_q + 64'd1;
        if (pipe_tframe_out_frame_done) begin
          frame_done_sticky_q <= 1'b1;
          frame_count_q <= frame_count_q + 32'd1;
          latest_frame_start_q <= pipe_tframe_out_frame_start_sample;
        end
      end
      if (softword_fire) begin
        softword_count_q <= softword_count_q + 64'd1;
      end
      if (sync_fire) begin
        sync_seen_sticky_q <= 1'b1;
        sync_count_q <= sync_count_q + 32'd1;
        latest_sync_found_q <= sync_out_found;
        latest_sync_locked_q <= sync_out_locked;
        latest_sync_lock_count_q <= sync_out_lock_count;
        latest_sync_miss_count_q <= sync_out_miss_count;
        latest_sync_pilot_used_pairs_q <= sync_out_pilot_used_pairs;
        latest_sync_carrier_offset_q <= sync_out_carrier_offset_bins;
        latest_sync_prs_score_q <= sync_out_prs_score[31:0];
        latest_sync_pilot_score_q <= sync_out_pilot_score[31:0];
        latest_sync_cfo_hz_q <= sync_out_coarse_freq_error_hz[31:0];
        latest_sync_fine_timing_q <=
            {{(32-TIMING_WIDTH){sync_out_fine_timing_error_samples[TIMING_WIDTH-1]}},
             sync_out_fine_timing_error_samples};
        latest_sync_prs_symbol_start_q <= sync_out_prs_symbol_start_sample;
        latest_sync_prs_fft_start_q <= sync_out_prs_fft_start_sample;
        latest_sync_next_prs_fft_start_q <= sync_out_next_prs_fft_start_sample;
        if (sync_out_found) begin
          sync_found_sticky_q <= 1'b1;
          sync_found_count_q <= sync_found_count_q + 32'd1;
        end
      end
    end
  end

  dab_pipeline_top_custom_fft #(
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
      .QPSK_RAW_SHIFT(QPSK_RAW_SHIFT),
      .ENABLE_CHANNEL_EQUALIZER(ENABLE_CHANNEL_EQUALIZER),
      .ENABLE_CFO_CORRECTOR(ENABLE_CFO_CORRECTOR),
      .ENABLE_INTERNAL_CFO_CONTROL(ENABLE_INTERNAL_CFO_CONTROL),
      .CFO_CONTROL_GAIN_FRAC_BITS(CFO_CONTROL_GAIN_FRAC_BITS),
      .CFO_CONTROL_GAIN_SCALE_Q(CFO_CONTROL_GAIN_SCALE_Q),
      .GUARD_SAMPLES(GUARD_SAMPLES),
      .FFT_SAMPLES(FFT_SAMPLES),
      .FFT_APPLY_BLOCK_EXP(FFT_APPLY_BLOCK_EXP),
      .FFT_INTERNAL_WIDTH(FFT_INTERNAL_WIDTH),
      .FFT_TWIDDLE_WIDTH(FFT_TWIDDLE_WIDTH),
      .FFT_TWIDDLE_FRAC_BITS(FFT_TWIDDLE_FRAC_BITS)
  ) u_dab (
      .clk(s_axi_aclk),
      .rst_n(s_axi_aresetn),
      .clear(clear_pulse_q),
      .set_sample_index_valid(set_sample_index_pulse_q),
      .set_sample_index_value(set_sample_index_value_q),
      .sdr_in_valid(sdr_in_valid),
      .sdr_in_ready(sdr_in_ready),
      .sdr_in_i(sdr_in_i),
      .sdr_in_q(sdr_in_q),
      .cfg_enable_tracking(tracking_enable_q),
      .cfg_min_cp_score(cfg_min_cp_score_q[SCORE_WIDTH-1:0]),
      .cfg_min_pilot_score(cfg_min_pilot_score_q[SCORE_WIDTH-1:0]),
      .cfg_cfo_bypass(cfg_cfo_bypass_q),
      .cfg_cfo_phase_inc(cfg_cfo_phase_inc_q),
      .cfg_cfo_phase_load(cfg_cfo_phase_load_pulse_q),
      .cfg_cfo_phase_seed(cfg_cfo_phase_seed_q),
      .predict_in_valid(1'b0),
      .predict_in_ready(predict_in_ready_unused),
      .predict_in_prs_fft_start_sample(64'd0),
      .data_symbol_cmd_valid(1'b0),
      .data_symbol_cmd_ready(data_symbol_cmd_ready_unused),
      .data_symbol_cmd_fft_start_sample(64'd0),
      .sync_out_valid(sync_out_valid),
      .sync_out_ready(1'b1),
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
      .tframe_out_valid(pipe_tframe_out_valid),
      .tframe_out_ready(pipe_tframe_out_ready),
      .tframe_out_softbit(pipe_tframe_out_softbit),
      .tframe_out_region(pipe_tframe_out_region),
      .tframe_out_addr(pipe_tframe_out_addr),
      .tframe_out_symbol_index(pipe_tframe_out_symbol_index),
      .tframe_out_byte_index(pipe_tframe_out_byte_index),
      .tframe_out_frame_start_sample(pipe_tframe_out_frame_start_sample),
      .tframe_out_frame_done(pipe_tframe_out_frame_done),
      .history_valid(history_valid),
      .history_oldest_sample_index(history_oldest_sample_index),
      .history_next_sample_index(history_next_sample_index),
      .history_stored_samples(history_stored_samples),
      .history_full(history_full),
      .symbol_busy(symbol_busy)
  );
endmodule

`default_nettype wire
