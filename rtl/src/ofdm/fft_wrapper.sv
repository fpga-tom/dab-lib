`default_nettype none

module fft_wrapper
  import dab_ofdm_pkg::*;
#(
    parameter int INPUT_WIDTH = DAB_OFDM_SAMPLE_WIDTH,
    parameter int OUTPUT_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int BLOCK_EXP_WIDTH = DAB_OFDM_BLOCK_EXP_WIDTH,
    parameter int APPLY_BLOCK_EXP = 0,
    parameter int META_FIFO_DEPTH = 4
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic in_valid,
    output logic in_ready,
    input  wire logic signed [INPUT_WIDTH-1:0] in_i,
    input  wire logic signed [INPUT_WIDTH-1:0] in_q,
    input  wire logic [63:0] in_symbol_start_sample,

    output logic core_in_valid,
    input  wire logic core_in_ready,
    output logic signed [INPUT_WIDTH-1:0] core_in_i,
    output logic signed [INPUT_WIDTH-1:0] core_in_q,
    output logic core_in_last,

    input  wire logic core_out_valid,
    output logic core_out_ready,
    input  wire logic signed [OUTPUT_WIDTH-1:0] core_out_i,
    input  wire logic signed [OUTPUT_WIDTH-1:0] core_out_q,
    input  wire logic [BLOCK_EXP_WIDTH-1:0] core_out_block_exp,
    input  wire logic core_out_overflow,

    output logic out_valid,
    input  wire logic out_ready,
    output logic signed [OUTPUT_WIDTH-1:0] out_i,
    output logic signed [OUTPUT_WIDTH-1:0] out_q,
    output logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] out_fft_bin_index,
    output logic [63:0] out_symbol_start_sample,
    output logic [BLOCK_EXP_WIDTH-1:0] out_block_exp,
    output logic out_overflow,
    output logic out_symbol_done
);
  localparam logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] LAST_FFT_INDEX =
      DAB_OFDM_FFT_INDEX_WIDTH'(DAB_OFDM_FFT_SAMPLES - 1);
  localparam int META_ADDR_WIDTH = META_FIFO_DEPTH <= 2 ? 1 : $clog2(META_FIFO_DEPTH);
  localparam int META_COUNT_WIDTH = $clog2(META_FIFO_DEPTH + 1);

  logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] input_index_q;
  logic [DAB_OFDM_FFT_INDEX_WIDTH-1:0] output_index_q;
  logic [63:0] meta_symbol_start_mem [0:META_FIFO_DEPTH-1];
  logic [META_ADDR_WIDTH-1:0] meta_wr_ptr_q;
  logic [META_ADDR_WIDTH-1:0] meta_rd_ptr_q;
  logic [META_COUNT_WIDTH-1:0] meta_count_q;

  logic input_fire;
  logic output_fire;
  logic input_last;
  logic output_last;
  logic input_completes;
  logic output_completes;
  logic meta_full;
  logic meta_empty;
  logic meta_push;
  logic meta_pop;
  logic meta_can_push;
  logic input_allowed;

  assign input_last = input_index_q == LAST_FFT_INDEX;
  assign output_last = output_index_q == LAST_FFT_INDEX;
  assign meta_full = meta_count_q == META_COUNT_WIDTH'(META_FIFO_DEPTH);
  assign meta_empty = meta_count_q == '0;
  assign meta_pop = output_completes;
  assign meta_can_push = !meta_full || meta_pop;
  assign input_allowed = !input_last || meta_can_push;

  assign core_in_valid = !clear && input_allowed && in_valid;
  assign in_ready = !clear && input_allowed && core_in_ready;
  assign core_in_i = in_i;
  assign core_in_q = in_q;
  assign core_in_last = input_last;

  assign out_valid = !clear && !meta_empty && core_out_valid;
  assign core_out_ready = !clear && !meta_empty && out_ready;
  assign out_i = rescale_output(core_out_i, core_out_block_exp);
  assign out_q = rescale_output(core_out_q, core_out_block_exp);
  assign out_fft_bin_index = output_index_q;
  assign out_symbol_start_sample = meta_symbol_start_mem[meta_rd_ptr_q];
  assign out_block_exp = core_out_block_exp;
  assign out_overflow = core_out_overflow;
  assign out_symbol_done = output_last;

  assign input_fire = core_in_valid && core_in_ready;
  assign output_fire = out_valid && out_ready;
  assign input_completes = input_fire && input_last;
  assign output_completes = output_fire && output_last;
  assign meta_push = input_completes;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      input_index_q <= '0;
      output_index_q <= '0;
      meta_wr_ptr_q <= '0;
      meta_rd_ptr_q <= '0;
      meta_count_q <= '0;
    end else if (clear) begin
      input_index_q <= '0;
      output_index_q <= '0;
      meta_wr_ptr_q <= '0;
      meta_rd_ptr_q <= '0;
      meta_count_q <= '0;
    end else begin
      if (input_fire) begin
        if (input_last) begin
          input_index_q <= '0;
        end else begin
          input_index_q <= input_index_q + 1'b1;
        end
      end

      if (output_fire) begin
        if (output_last) begin
          output_index_q <= '0;
        end else begin
          output_index_q <= output_index_q + 1'b1;
        end
      end

      if (meta_push) begin
        meta_symbol_start_mem[meta_wr_ptr_q] <= in_symbol_start_sample;
        meta_wr_ptr_q <=
            meta_wr_ptr_q == META_ADDR_WIDTH'(META_FIFO_DEPTH - 1) ? '0 :
                                                                  meta_wr_ptr_q + 1'b1;
      end

      if (meta_pop) begin
        meta_rd_ptr_q <=
            meta_rd_ptr_q == META_ADDR_WIDTH'(META_FIFO_DEPTH - 1) ? '0 :
                                                                  meta_rd_ptr_q + 1'b1;
      end

      case ({meta_push, meta_pop})
        2'b10: meta_count_q <= meta_count_q + 1'b1;
        2'b01: meta_count_q <= meta_count_q - 1'b1;
        default: meta_count_q <= meta_count_q;
      endcase
    end
  end

  function automatic logic signed [OUTPUT_WIDTH-1:0] rescale_output(
      input logic signed [OUTPUT_WIDTH-1:0] value,
      input logic [BLOCK_EXP_WIDTH-1:0] block_exp
  );
    logic signed [63:0] extended;
    logic signed [63:0] scaled;
    logic signed [63:0] max_value;
    logic signed [63:0] min_value;
    logic signed [63:0] max_pre_shift;
    logic signed [63:0] min_pre_shift;
    int unsigned shift_amount;
    begin
      extended = {{(64 - OUTPUT_WIDTH){value[OUTPUT_WIDTH-1]}}, value};
      max_value = (64'sd1 <<< (OUTPUT_WIDTH - 1)) - 64'sd1;
      min_value = -(64'sd1 <<< (OUTPUT_WIDTH - 1));

      if (APPLY_BLOCK_EXP != 0) begin
        shift_amount = int'(block_exp);
        if (shift_amount >= 63) begin
          if (extended == 64'sd0) begin
            scaled = 64'sd0;
          end else begin
            scaled = extended[63] ? min_value : max_value;
          end
        end else begin
          max_pre_shift = max_value >>> shift_amount;
          min_pre_shift = min_value >>> shift_amount;
          if (extended > max_pre_shift) begin
            scaled = max_value;
          end else if (extended < min_pre_shift) begin
            scaled = min_value;
          end else begin
            scaled = extended <<< shift_amount;
          end
        end
      end else begin
        scaled = extended;
      end

      if (scaled > max_value) begin
        rescale_output = max_value[OUTPUT_WIDTH-1:0];
      end else if (scaled < min_value) begin
        rescale_output = min_value[OUTPUT_WIDTH-1:0];
      end else begin
        rescale_output = scaled[OUTPUT_WIDTH-1:0];
      end
    end
  endfunction
endmodule

`default_nettype wire
