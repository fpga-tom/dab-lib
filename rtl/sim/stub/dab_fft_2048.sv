`default_nettype none

module dab_fft_2048 (
    input  logic clk,
    input  logic reset_n,

    input  logic sink_valid,
    output logic sink_ready,
    input  logic [1:0] sink_error,
    input  logic sink_sop,
    input  logic sink_eop,
    input  logic signed [17:0] sink_real,
    input  logic signed [17:0] sink_imag,
    input  logic inverse,

    output logic source_valid,
    input  logic source_ready,
    output logic [1:0] source_error,
    output logic source_sop,
    output logic source_eop,
    output logic signed [23:0] source_real,
    output logic signed [23:0] source_imag,
    output logic [7:0] source_exp
);
  assign sink_ready = source_ready || !source_valid;

  always_ff @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
      source_valid <= 1'b0;
      source_error <= '0;
      source_sop <= 1'b0;
      source_eop <= 1'b0;
      source_real <= '0;
      source_imag <= '0;
      source_exp <= '0;
    end else if (sink_ready) begin
      source_valid <= sink_valid;
      source_error <= sink_error;
      source_sop <= sink_sop;
      source_eop <= sink_eop;
      source_real <= {{6{sink_real[17]}}, sink_real};
      source_imag <= {{6{sink_imag[17]}}, sink_imag};
      source_exp <= {7'd0, inverse};
    end
  end
endmodule

`default_nettype wire
