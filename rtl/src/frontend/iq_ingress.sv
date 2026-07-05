`default_nettype none

module iq_ingress #(
    parameter int SAMPLE_WIDTH = 16
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic set_sample_index_valid,
    input  wire logic [63:0] set_sample_index_value,

    input  wire logic in_valid,
    output logic in_ready,
    input  wire logic signed [SAMPLE_WIDTH-1:0] in_i,
    input  wire logic signed [SAMPLE_WIDTH-1:0] in_q,

    output logic out_valid,
    input  wire logic out_ready,
    output logic signed [SAMPLE_WIDTH-1:0] out_i,
    output logic signed [SAMPLE_WIDTH-1:0] out_q,
    output logic [63:0] out_sample_index
);
  logic [63:0] sample_index_q;
  logic sample_fire;
  logic accepting_samples;

  assign accepting_samples = !clear && !set_sample_index_valid;
  assign in_ready = accepting_samples && out_ready;
  assign out_valid = accepting_samples && in_valid;
  assign out_i = in_i;
  assign out_q = in_q;
  assign out_sample_index = sample_index_q;
  assign sample_fire = in_valid && in_ready;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      sample_index_q <= '0;
    end else if (clear) begin
      sample_index_q <= '0;
    end else if (set_sample_index_valid) begin
      sample_index_q <= set_sample_index_value;
    end else if (sample_fire) begin
      sample_index_q <= sample_index_q + 1'b1;
    end
  end
endmodule

`default_nettype wire
