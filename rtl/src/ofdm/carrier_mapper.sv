`default_nettype none

module carrier_mapper
  import dab_ofdm_pkg::*;
#(
    parameter int CARRIER_WIDTH = DAB_OFDM_CARRIER_WIDTH
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic in_valid,
    output logic in_ready,
    input  wire logic signed [CARRIER_WIDTH-1:0] in_i,
    input  wire logic signed [CARRIER_WIDTH-1:0] in_q,
    input  wire logic [63:0] in_symbol_start_sample,

    output logic out_valid,
    input  wire logic out_ready,
    output logic signed [CARRIER_WIDTH-1:0] out_i,
    output logic signed [CARRIER_WIDTH-1:0] out_q,
    output logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] out_active_index,
    output logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] out_fft_bin_index,
    output logic [63:0] out_symbol_start_sample,
    output logic out_symbol_done
);
  typedef enum logic {
    STATE_LOAD = 1'b0,
    STATE_EMIT_LOW = 1'b1
  } state_t;

  localparam logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] LAST_FFT_INDEX =
      DAB_OFDM_FFT_INDEX_WIDTH'(DAB_OFDM_FFT_SAMPLES - 1);
  localparam logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] LOW_LAST_BIN =
      DAB_OFDM_FFT_INDEX_WIDTH'(DAB_OFDM_ACTIVE_CARRIER_HALF);
  localparam logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] HIGH_FIRST_BIN =
      DAB_OFDM_FFT_INDEX_WIDTH'(DAB_OFDM_FFT_SAMPLES - DAB_OFDM_ACTIVE_CARRIER_HALF);
  localparam logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] LOW_OUTPUT_BASE =
      DAB_OFDM_ACTIVE_INDEX_WIDTH'(DAB_OFDM_ACTIVE_CARRIER_HALF);
  localparam int LOW_INDEX_WIDTH = 10;

  state_t state_q;
  logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] fft_bin_index_q;
  logic [LOW_INDEX_WIDTH-1:0] low_emit_index_q;
  logic [63:0] symbol_start_sample_q;
  logic low_data_valid_q;

  (* ram_style = "block", ramstyle = "M10K" *)
  logic signed [CARRIER_WIDTH-1:0] low_i_mem [0:DAB_OFDM_ACTIVE_CARRIER_HALF];
  (* ram_style = "block", ramstyle = "M10K" *)
  logic signed [CARRIER_WIDTH-1:0] low_q_mem [0:DAB_OFDM_ACTIVE_CARRIER_HALF];
  logic signed [CARRIER_WIDTH-1:0] low_i_rd_q;
  logic signed [CARRIER_WIDTH-1:0] low_q_rd_q;

  logic input_fire;
  logic emit_low_step;
  logic pending_output;
  logic in_low_region;
  logic in_high_region;
  logic low_last_emit;
  logic [LOW_INDEX_WIDTH-1:0] low_store_index;
  logic low_first_read;
  logic low_read_fire;
  logic [LOW_INDEX_WIDTH-1:0] low_read_addr;

  assign pending_output = out_valid && !out_ready;
  assign in_ready = (state_q == STATE_LOAD) && !pending_output;
  assign input_fire = in_valid && in_ready;
  assign emit_low_step = (state_q == STATE_EMIT_LOW) && low_data_valid_q && !pending_output;
  assign in_low_region = fft_bin_index_q <= LOW_LAST_BIN;
  assign in_high_region = fft_bin_index_q >= HIGH_FIRST_BIN;
  assign low_last_emit = low_emit_index_q == LOW_INDEX_WIDTH'(DAB_OFDM_ACTIVE_CARRIER_HALF);
  assign low_store_index = fft_bin_index_q[LOW_INDEX_WIDTH-1:0];
  assign low_first_read = input_fire && fft_bin_index_q == LAST_FFT_INDEX;
  assign low_read_fire = low_first_read || (emit_low_step && !low_last_emit);
  assign low_read_addr = low_first_read ? '0 : (low_emit_index_q + 1'b1);

  always_ff @(posedge clk) begin
    if (input_fire && in_low_region) begin
      low_i_mem[low_store_index] <= in_i;
      low_q_mem[low_store_index] <= in_q;
    end

    if (low_read_fire) begin
      low_i_rd_q <= low_i_mem[low_read_addr];
      low_q_rd_q <= low_q_mem[low_read_addr];
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state_q <= STATE_LOAD;
      fft_bin_index_q <= '0;
      low_emit_index_q <= '0;
      symbol_start_sample_q <= '0;
      low_data_valid_q <= 1'b0;
      out_valid <= 1'b0;
      out_i <= '0;
      out_q <= '0;
      out_active_index <= '0;
      out_fft_bin_index <= '0;
      out_symbol_start_sample <= '0;
      out_symbol_done <= 1'b0;
    end else if (clear) begin
      state_q <= STATE_LOAD;
      fft_bin_index_q <= '0;
      low_emit_index_q <= '0;
      symbol_start_sample_q <= '0;
      low_data_valid_q <= 1'b0;
      out_valid <= 1'b0;
      out_i <= '0;
      out_q <= '0;
      out_active_index <= '0;
      out_fft_bin_index <= '0;
      out_symbol_start_sample <= '0;
      out_symbol_done <= 1'b0;
    end else begin
      if (input_fire) begin
        if (fft_bin_index_q == '0) begin
          symbol_start_sample_q <= in_symbol_start_sample;
        end

        if (in_high_region) begin
          out_valid <= 1'b1;
          out_i <= in_i;
          out_q <= in_q;
          out_active_index <= DAB_OFDM_ACTIVE_INDEX_WIDTH'(fft_bin_index_q - HIGH_FIRST_BIN);
          out_fft_bin_index <= fft_bin_index_q;
          out_symbol_start_sample <= symbol_start_sample_q;
          out_symbol_done <= 1'b0;
        end

        if (fft_bin_index_q == LAST_FFT_INDEX) begin
          state_q <= STATE_EMIT_LOW;
          fft_bin_index_q <= '0;
          low_emit_index_q <= '0;
          low_data_valid_q <= 1'b1;
        end else begin
          fft_bin_index_q <= fft_bin_index_q + 1'b1;
        end
      end

      if (emit_low_step) begin
        out_valid <= 1'b1;
        out_i <= low_i_rd_q;
        out_q <= low_q_rd_q;
        out_active_index <= LOW_OUTPUT_BASE + DAB_OFDM_ACTIVE_INDEX_WIDTH'(low_emit_index_q);
        out_fft_bin_index <= DAB_OFDM_FFT_INDEX_WIDTH'(low_emit_index_q);
        out_symbol_start_sample <= symbol_start_sample_q;
        out_symbol_done <= low_last_emit;

        if (low_last_emit) begin
          state_q <= STATE_LOAD;
          low_emit_index_q <= '0;
          low_data_valid_q <= 1'b0;
        end else begin
          low_emit_index_q <= low_emit_index_q + 1'b1;
          low_data_valid_q <= 1'b1;
        end
      end else if (out_ready && !(input_fire && in_high_region)) begin
        out_valid <= 1'b0;
        out_symbol_done <= 1'b0;
      end
    end
  end
endmodule

`default_nettype wire
