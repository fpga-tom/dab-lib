`default_nettype none

module data_symbol_scheduler
  import dab_ofdm_pkg::*, dab_tframe_pkg::*;
#(
    parameter int GUARD_SAMPLES = 504,
    parameter int FFT_SAMPLES = DAB_OFDM_FFT_SAMPLES
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic sync_valid,
    input  wire logic sync_found,
    input  wire logic [63:0] sync_prs_fft_start_sample,

    input  wire logic history_valid,
    input  wire logic [63:0] history_oldest_sample_index,
    input  wire logic [63:0] history_next_sample_index,

    output logic cmd_valid,
    input  wire logic cmd_ready,
    output logic [63:0] cmd_fft_start_sample,

    /* verilator lint_off UNUSEDSIGNAL */
    input  wire logic frame_done
    /* verilator lint_on UNUSEDSIGNAL */
);
  localparam int SYMBOL_SAMPLES = GUARD_SAMPLES + FFT_SAMPLES;
  localparam logic [DAB_TFRAME_SYMBOL_INDEX_WIDTH-1:0] LAST_SYMBOL_INDEX =
      DAB_TFRAME_SYMBOL_INDEX_WIDTH'(DAB_TFRAME_TOTAL_SYMBOLS - 1);

  typedef enum logic [1:0] {
    STATE_IDLE = 2'd0,
    STATE_SCHEDULE = 2'd1
  } state_t;

  state_t state_q;
  logic [DAB_TFRAME_SYMBOL_INDEX_WIDTH-1:0] next_symbol_q;
  logic [63:0] next_fft_start_sample_q;
  logic pending_valid_q;
  logic [63:0] pending_prs_fft_start_sample_q;

  logic [64:0] next_required_end_sample;
  logic current_window_available;
  logic current_window_stale;
  logic cmd_fire;
  logic sync_fire;
  logic duplicate_current;
  logic duplicate_pending;

  assign sync_fire = sync_valid && sync_found;
  assign cmd_fire = cmd_valid && cmd_ready;
  assign cmd_fft_start_sample = next_fft_start_sample_q;
  assign next_required_end_sample = {1'b0, next_fft_start_sample_q} + 65'(FFT_SAMPLES);
  assign current_window_available =
      history_valid &&
      next_fft_start_sample_q >= history_oldest_sample_index &&
      !next_required_end_sample[64] &&
      next_required_end_sample <= {1'b0, history_next_sample_index};
  assign current_window_stale =
      history_valid && next_fft_start_sample_q < history_oldest_sample_index;
  assign cmd_valid = state_q == STATE_SCHEDULE && current_window_available;
  assign duplicate_current =
      state_q != STATE_IDLE &&
      sync_prs_fft_start_sample + 64'(SYMBOL_SAMPLES) <= next_fft_start_sample_q;
  assign duplicate_pending =
      pending_valid_q && pending_prs_fft_start_sample_q == sync_prs_fft_start_sample;

  function automatic logic [63:0] first_data_fft_start(input logic [63:0] prs_fft_start);
    begin
      first_data_fft_start = prs_fft_start + 64'(SYMBOL_SAMPLES);
    end
  endfunction

  task automatic start_frame(input logic [63:0] prs_fft_start);
    begin
      state_q <= STATE_SCHEDULE;
      next_symbol_q <= '0;
      next_fft_start_sample_q <= first_data_fft_start(prs_fft_start);
    end
  endtask

  task automatic start_pending_or_idle;
    begin
      if (pending_valid_q) begin
        start_frame(pending_prs_fft_start_sample_q);
        pending_valid_q <= 1'b0;
        pending_prs_fft_start_sample_q <= '0;
      end else begin
        state_q <= STATE_IDLE;
        next_symbol_q <= '0;
        next_fft_start_sample_q <= '0;
      end
    end
  endtask

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state_q <= STATE_IDLE;
      next_symbol_q <= '0;
      next_fft_start_sample_q <= '0;
      pending_valid_q <= 1'b0;
      pending_prs_fft_start_sample_q <= '0;
    end else if (clear) begin
      state_q <= STATE_IDLE;
      next_symbol_q <= '0;
      next_fft_start_sample_q <= '0;
      pending_valid_q <= 1'b0;
      pending_prs_fft_start_sample_q <= '0;
    end else begin
      if (sync_fire && !duplicate_current && !duplicate_pending) begin
        if (state_q == STATE_IDLE) begin
          start_frame(sync_prs_fft_start_sample);
        end else if (!pending_valid_q) begin
          pending_valid_q <= 1'b1;
          pending_prs_fft_start_sample_q <= sync_prs_fft_start_sample;
        end
      end

      case (state_q)
        STATE_IDLE: begin
          if (pending_valid_q) begin
            start_pending_or_idle();
          end
        end

        STATE_SCHEDULE: begin
          if (current_window_stale) begin
            start_pending_or_idle();
          end else if (cmd_fire) begin
            if (next_symbol_q == LAST_SYMBOL_INDEX) begin
              start_pending_or_idle();
            end else begin
              next_symbol_q <= next_symbol_q + 1'b1;
              next_fft_start_sample_q <= next_fft_start_sample_q + 64'(SYMBOL_SAMPLES);
            end
          end
        end

        default: begin
          start_pending_or_idle();
        end
      endcase
    end
  end
endmodule

`default_nettype wire
