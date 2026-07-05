`default_nettype none

module symbol_buffer #(
    parameter int SAMPLE_WIDTH = 16,
    parameter int FFT_SAMPLES = 2048,
    parameter int SAMPLE_OFFSET_WIDTH = FFT_SAMPLES <= 2 ? 1 : $clog2(FFT_SAMPLES)
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic in_valid,
    output logic in_ready,
    input  wire logic [63:0] in_symbol_start_sample,

    output logic hist_read_req_valid,
    input  wire logic hist_read_req_ready,
    output logic [63:0] hist_read_req_start_sample,
    output logic [31:0] hist_read_req_length,

    input  wire logic hist_rd_out_valid,
    output logic hist_rd_out_ready,
    input  wire logic signed [SAMPLE_WIDTH-1:0] hist_rd_out_i,
    input  wire logic signed [SAMPLE_WIDTH-1:0] hist_rd_out_q,
    input  wire logic [63:0] hist_rd_out_sample_index,
    input  wire logic hist_rd_out_first,
    input  wire logic hist_rd_out_last,
    input  wire logic hist_rd_out_error,

    output logic out_valid,
    input  wire logic out_ready,
    output logic signed [SAMPLE_WIDTH-1:0] out_i,
    output logic signed [SAMPLE_WIDTH-1:0] out_q,
    output logic [63:0] out_sample_index,
    output logic [63:0] out_symbol_start_sample,
    output logic [SAMPLE_OFFSET_WIDTH-1:0] out_sample_offset,
    output logic out_symbol_done,
    output logic out_error
);
  typedef enum logic [1:0] {
    STATE_IDLE = 2'd0,
    STATE_READ_REQ = 2'd1,
    STATE_STREAM = 2'd2,
    STATE_DRAIN = 2'd3
  } state_t;

  state_t state_q;
  logic [63:0] symbol_start_sample_q;
  logic [SAMPLE_OFFSET_WIDTH-1:0] sample_offset_q;
  logic error_pending_q;
  logic error_last_q;

  logic input_fire;
  logic hist_req_fire;
  logic hist_fire;
  logic output_fire;
  logic output_from_history;
  logic output_is_error;
  logic history_beat_malformed;
  logic [63:0] expected_sample_index;
  logic expected_first;
  logic expected_last;

  assign in_ready = !clear && state_q == STATE_IDLE;
  assign input_fire = in_valid && in_ready;

  assign hist_read_req_valid = !clear && state_q == STATE_READ_REQ;
  assign hist_read_req_start_sample = symbol_start_sample_q;
  assign hist_read_req_length = 32'(FFT_SAMPLES);
  assign hist_req_fire = hist_read_req_valid && hist_read_req_ready;

  assign expected_sample_index = symbol_start_sample_q + 64'(sample_offset_q);
  assign expected_first = sample_offset_q == '0;
  assign expected_last = sample_offset_q == SAMPLE_OFFSET_WIDTH'(FFT_SAMPLES - 1);
  assign history_beat_malformed =
      hist_rd_out_valid &&
      !hist_rd_out_error &&
      (hist_rd_out_sample_index != expected_sample_index ||
       hist_rd_out_first != expected_first ||
       hist_rd_out_last != expected_last);

  assign output_from_history = state_q == STATE_STREAM && hist_rd_out_valid;
  assign output_is_error = error_pending_q ||
      (output_from_history && (hist_rd_out_error || history_beat_malformed));
  assign out_valid = !clear && (output_from_history || error_pending_q);
  assign out_i = output_is_error ? '0 : hist_rd_out_i;
  assign out_q = output_is_error ? '0 : hist_rd_out_q;
  assign out_sample_index = output_is_error ? expected_sample_index : hist_rd_out_sample_index;
  assign out_symbol_start_sample = symbol_start_sample_q;
  assign out_sample_offset = sample_offset_q;
  assign out_symbol_done = output_is_error ? 1'b1 : hist_rd_out_last;
  assign out_error = output_is_error;
  assign output_fire = out_valid && out_ready;

  assign hist_rd_out_ready = !clear &&
      ((state_q == STATE_STREAM && !error_pending_q && out_ready) ||
       (state_q == STATE_DRAIN));
  assign hist_fire = hist_rd_out_valid && hist_rd_out_ready;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state_q <= STATE_IDLE;
      symbol_start_sample_q <= '0;
      sample_offset_q <= '0;
      error_pending_q <= 1'b0;
      error_last_q <= 1'b0;
    end else if (clear) begin
      state_q <= STATE_IDLE;
      symbol_start_sample_q <= '0;
      sample_offset_q <= '0;
      error_pending_q <= 1'b0;
      error_last_q <= 1'b0;
    end else begin
      case (state_q)
        STATE_IDLE: begin
          error_pending_q <= 1'b0;
          error_last_q <= 1'b0;
          sample_offset_q <= '0;
          if (input_fire) begin
            symbol_start_sample_q <= in_symbol_start_sample;
            state_q <= STATE_READ_REQ;
          end
        end

        STATE_READ_REQ: begin
          if (hist_req_fire) begin
            sample_offset_q <= '0;
            state_q <= STATE_STREAM;
          end
        end

        STATE_STREAM: begin
          if (hist_fire) begin
            if (hist_rd_out_error || history_beat_malformed) begin
              error_pending_q <= 1'b0;
              error_last_q <= hist_rd_out_last;
              if (out_ready) begin
                state_q <= hist_rd_out_last ? STATE_IDLE : STATE_DRAIN;
              end else begin
                error_pending_q <= 1'b1;
              end
            end else if (hist_rd_out_last) begin
              state_q <= STATE_IDLE;
              sample_offset_q <= '0;
            end else begin
              sample_offset_q <= sample_offset_q + 1'b1;
            end
          end

          if (error_pending_q && output_fire) begin
            error_pending_q <= 1'b0;
            state_q <= error_last_q ? STATE_IDLE : STATE_DRAIN;
          end
        end

        default: begin
          if (hist_fire && hist_rd_out_last) begin
            state_q <= STATE_IDLE;
            sample_offset_q <= '0;
            error_last_q <= 1'b0;
          end
        end
      endcase
    end
  end
endmodule

`default_nettype wire
