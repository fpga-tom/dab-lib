`default_nettype none

module sample_history #(
    parameter int SAMPLE_WIDTH = 16,
    parameter int DEPTH_SAMPLES = 262144
) (
    input  logic clk,
    input  logic rst_n,
    input  logic clear,

    input  logic in_valid,
    output logic in_ready,
    input  logic signed [SAMPLE_WIDTH-1:0] in_i,
    input  logic signed [SAMPLE_WIDTH-1:0] in_q,
    input  logic [63:0] in_sample_index,

    input  logic read_req_valid,
    output logic read_req_ready,
    input  logic [63:0] read_req_start_sample,
    input  logic [31:0] read_req_length,

    output logic rd_out_valid,
    input  logic rd_out_ready,
    output logic signed [SAMPLE_WIDTH-1:0] rd_out_i,
    output logic signed [SAMPLE_WIDTH-1:0] rd_out_q,
    output logic [63:0] rd_out_sample_index,
    output logic rd_out_first,
    output logic rd_out_last,
    output logic rd_out_error,

    output logic history_valid,
    output logic [63:0] oldest_sample_index,
    output logic [63:0] next_sample_index,
    output logic [31:0] stored_samples,
    output logic history_full
);
  localparam int ADDR_WIDTH = DEPTH_SAMPLES <= 2 ? 1 : $clog2(DEPTH_SAMPLES);

  logic signed [SAMPLE_WIDTH-1:0] i_mem [0:DEPTH_SAMPLES-1];
  logic signed [SAMPLE_WIDTH-1:0] q_mem [0:DEPTH_SAMPLES-1];

  logic range_valid_q;
  logic [63:0] oldest_sample_index_q;
  logic [63:0] next_sample_index_q;
  logic [31:0] stored_samples_q;

  logic reading_q;
  logic [63:0] read_sample_index_q;
  logic [31:0] read_remaining_q;
  logic read_first_q;

  logic input_fire;
  logic request_fire;
  logic request_available;
  logic request_overflows;
  logic can_load_read;
  logic read_last_next;
  logic write_is_contiguous;
  logic write_makes_full;
  logic [ADDR_WIDTH-1:0] write_addr;
  logic [ADDR_WIDTH-1:0] request_addr;
  logic [ADDR_WIDTH-1:0] read_addr;
  logic [64:0] request_end_sample;

  assign in_ready = !clear;
  assign read_req_ready = !clear && !reading_q && !rd_out_valid;
  assign input_fire = in_valid && in_ready;
  assign request_fire = read_req_valid && read_req_ready;

  assign request_end_sample = {1'b0, read_req_start_sample} + {33'd0, read_req_length};
  assign request_overflows = request_end_sample[64];
  assign request_available = range_valid_q &&
      read_req_length != 32'd0 &&
      !request_overflows &&
      read_req_start_sample >= oldest_sample_index_q &&
      request_end_sample <= {1'b0, next_sample_index_q};

  assign can_load_read = reading_q && (!rd_out_valid || rd_out_ready);
  assign read_last_next = read_remaining_q == 32'd1;
  assign write_is_contiguous = range_valid_q && in_sample_index == next_sample_index_q;
  assign write_makes_full = stored_samples_q >= 32'(DEPTH_SAMPLES);
  assign write_addr = sample_addr(in_sample_index);
  assign request_addr = sample_addr(read_req_start_sample);
  assign read_addr = sample_addr(read_sample_index_q);

  assign history_valid = range_valid_q;
  assign oldest_sample_index = oldest_sample_index_q;
  assign next_sample_index = next_sample_index_q;
  assign stored_samples = stored_samples_q;
  assign history_full = range_valid_q && write_makes_full;

  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic [ADDR_WIDTH-1:0] sample_addr(input logic [63:0] sample_index);
    begin
      sample_addr = sample_index[ADDR_WIDTH-1:0];
    end
  endfunction
  /* verilator lint_on UNUSEDSIGNAL */

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      range_valid_q <= 1'b0;
      oldest_sample_index_q <= '0;
      next_sample_index_q <= '0;
      stored_samples_q <= '0;
      reading_q <= 1'b0;
      read_sample_index_q <= '0;
      read_remaining_q <= '0;
      read_first_q <= 1'b0;
      rd_out_valid <= 1'b0;
      rd_out_i <= '0;
      rd_out_q <= '0;
      rd_out_sample_index <= '0;
      rd_out_first <= 1'b0;
      rd_out_last <= 1'b0;
      rd_out_error <= 1'b0;
    end else if (clear) begin
      range_valid_q <= 1'b0;
      oldest_sample_index_q <= '0;
      next_sample_index_q <= '0;
      stored_samples_q <= '0;
      reading_q <= 1'b0;
      read_sample_index_q <= '0;
      read_remaining_q <= '0;
      read_first_q <= 1'b0;
      rd_out_valid <= 1'b0;
      rd_out_i <= '0;
      rd_out_q <= '0;
      rd_out_sample_index <= '0;
      rd_out_first <= 1'b0;
      rd_out_last <= 1'b0;
      rd_out_error <= 1'b0;
    end else begin
      if (input_fire) begin
        i_mem[write_addr] <= in_i;
        q_mem[write_addr] <= in_q;

        if (!write_is_contiguous) begin
          range_valid_q <= 1'b1;
          oldest_sample_index_q <= in_sample_index;
          next_sample_index_q <= in_sample_index + 1'b1;
          stored_samples_q <= 32'd1;
        end else begin
          next_sample_index_q <= next_sample_index_q + 1'b1;
          if (write_makes_full) begin
            oldest_sample_index_q <= oldest_sample_index_q + 1'b1;
            stored_samples_q <= 32'(DEPTH_SAMPLES);
          end else begin
            stored_samples_q <= stored_samples_q + 1'b1;
          end
        end
      end

      if (rd_out_valid && rd_out_ready) begin
        rd_out_valid <= 1'b0;
        rd_out_first <= 1'b0;
        rd_out_last <= 1'b0;
        rd_out_error <= 1'b0;
      end

      if (request_fire) begin
        rd_out_valid <= 1'b1;
        rd_out_sample_index <= read_req_start_sample;
        rd_out_first <= 1'b1;

        if (request_available) begin
          rd_out_i <= i_mem[request_addr];
          rd_out_q <= q_mem[request_addr];
          rd_out_last <= read_req_length == 32'd1;
          rd_out_error <= 1'b0;

          reading_q <= read_req_length != 32'd1;
          read_sample_index_q <= read_req_start_sample + 1'b1;
          read_remaining_q <= read_req_length - 1'b1;
          read_first_q <= 1'b0;
        end else begin
          rd_out_i <= '0;
          rd_out_q <= '0;
          rd_out_last <= 1'b1;
          rd_out_error <= 1'b1;

          reading_q <= 1'b0;
          read_sample_index_q <= '0;
          read_remaining_q <= '0;
          read_first_q <= 1'b0;
        end
      end else if (can_load_read) begin
        rd_out_valid <= 1'b1;
        rd_out_i <= i_mem[read_addr];
        rd_out_q <= q_mem[read_addr];
        rd_out_sample_index <= read_sample_index_q;
        rd_out_first <= read_first_q;
        rd_out_last <= read_last_next;
        rd_out_error <= 1'b0;

        read_sample_index_q <= read_sample_index_q + 1'b1;
        read_remaining_q <= read_remaining_q - 1'b1;
        read_first_q <= 1'b0;

        if (read_last_next) begin
          reading_q <= 1'b0;
        end
      end
    end
  end
endmodule

`default_nettype wire
