`default_nettype none

module frequency_deinterleaver
  import dab_ofdm_pkg::*;
#(
    parameter int CARRIER_WIDTH = DAB_OFDM_CARRIER_WIDTH
) (
    input  logic clk,
    input  logic rst_n,
    input  logic clear,

    input  logic in_valid,
    output logic in_ready,
    input  logic signed [CARRIER_WIDTH-1:0] in_i,
    input  logic signed [CARRIER_WIDTH-1:0] in_q,
    input  logic [63:0] in_symbol_start_sample,

    output logic out_valid,
    input  logic out_ready,
    output logic signed [CARRIER_WIDTH-1:0] out_i,
    output logic signed [CARRIER_WIDTH-1:0] out_q,
    output logic [DAB_OFDM_DATA_INDEX_WIDTH-1:0] out_carrier_index,
    output logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] out_source_index,
    output logic [63:0] out_symbol_start_sample,
    output logic out_symbol_done
);
  typedef enum logic {
    STATE_LOAD = 1'b0,
    STATE_EMIT = 1'b1
  } state_t;

  localparam logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] LAST_ACTIVE_INDEX =
      DAB_OFDM_ACTIVE_INDEX_WIDTH'(DAB_OFDM_ACTIVE_CARRIERS - 1);
  localparam logic [DAB_OFDM_DATA_INDEX_WIDTH-1:0] LAST_DATA_INDEX =
      DAB_OFDM_DATA_INDEX_WIDTH'(DAB_OFDM_DATA_CARRIERS - 1);

  state_t state_q;
  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] load_index_q;
  logic [DAB_OFDM_DATA_INDEX_WIDTH-1:0] emit_index_q;
  logic [63:0] symbol_start_sample_q;
  logic [10:0] lcg_p_q;

  logic signed [CARRIER_WIDTH-1:0] carrier_i_mem [0:DAB_OFDM_ACTIVE_CARRIERS-1];
  logic signed [CARRIER_WIDTH-1:0] carrier_q_mem [0:DAB_OFDM_ACTIVE_CARRIERS-1];

  logic input_fire;
  logic generator_step;
  logic pending_output;
  logic candidate_accepted;
  logic [DAB_OFDM_ACTIVE_INDEX_WIDTH-1:0] mapped_source_index;

  function automatic logic [10:0] lcg_next(input logic [10:0] p);
    logic [10:0] sum;
    begin
      sum = (p << 3) + (p << 2) + p + 11'd511;
      lcg_next = sum;
    end
  endfunction

  assign pending_output = out_valid && !out_ready;
  assign in_ready = (state_q == STATE_LOAD) && !pending_output;
  assign input_fire = in_valid && in_ready;
  assign generator_step = (state_q == STATE_EMIT) && !pending_output;
  assign candidate_accepted = (lcg_p_q >= 11'd256) &&
                              (lcg_p_q <= 11'd1792) &&
                              (lcg_p_q != 11'd1024);
  assign mapped_source_index = DAB_OFDM_ACTIVE_INDEX_WIDTH'(lcg_p_q - 11'd256);

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state_q <= STATE_LOAD;
      load_index_q <= '0;
      emit_index_q <= '0;
      symbol_start_sample_q <= '0;
      lcg_p_q <= '0;
      out_valid <= 1'b0;
      out_i <= '0;
      out_q <= '0;
      out_carrier_index <= '0;
      out_source_index <= '0;
      out_symbol_start_sample <= '0;
      out_symbol_done <= 1'b0;
    end else if (clear) begin
      state_q <= STATE_LOAD;
      load_index_q <= '0;
      emit_index_q <= '0;
      symbol_start_sample_q <= '0;
      lcg_p_q <= '0;
      out_valid <= 1'b0;
      out_i <= '0;
      out_q <= '0;
      out_carrier_index <= '0;
      out_source_index <= '0;
      out_symbol_start_sample <= '0;
      out_symbol_done <= 1'b0;
    end else begin
      if (input_fire) begin
        carrier_i_mem[load_index_q] <= in_i;
        carrier_q_mem[load_index_q] <= in_q;
        if (load_index_q == '0) begin
          symbol_start_sample_q <= in_symbol_start_sample;
        end

        if (load_index_q == LAST_ACTIVE_INDEX) begin
          state_q <= STATE_EMIT;
          load_index_q <= '0;
          emit_index_q <= '0;
          lcg_p_q <= '0;
        end else begin
          load_index_q <= load_index_q + 1'b1;
        end
      end

      if (generator_step) begin
        lcg_p_q <= lcg_next(lcg_p_q);

        if (candidate_accepted) begin
          out_valid <= 1'b1;
          out_i <= carrier_i_mem[mapped_source_index];
          out_q <= carrier_q_mem[mapped_source_index];
          out_carrier_index <= emit_index_q;
          out_source_index <= mapped_source_index;
          out_symbol_start_sample <= symbol_start_sample_q;
          out_symbol_done <= emit_index_q == LAST_DATA_INDEX;

          if (emit_index_q == LAST_DATA_INDEX) begin
            state_q <= STATE_LOAD;
            emit_index_q <= '0;
            lcg_p_q <= '0;
          end else begin
            emit_index_q <= emit_index_q + 1'b1;
          end
        end else begin
          out_valid <= 1'b0;
          out_symbol_done <= 1'b0;
        end
      end else if (out_ready) begin
        out_valid <= 1'b0;
        out_symbol_done <= 1'b0;
      end
    end
  end
endmodule

`default_nettype wire
