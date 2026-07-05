`default_nettype none

module qpsk_softbits
  import dab_ofdm_pkg::*;
#(
    parameter int CARRIER_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int SOFTBIT_WIDTH = DAB_OFDM_SOFTBIT_WIDTH
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic in_valid,
    output logic in_ready,
    input  wire logic signed [CARRIER_WIDTH-1:0] in_i,
    input  wire logic signed [CARRIER_WIDTH-1:0] in_q,
    input  wire logic [DAB_OFDM_DATA_INDEX_WIDTH-1:0] in_carrier_index,
    input  wire logic [63:0] in_symbol_start_sample,
    input  wire logic in_symbol_done,

    output logic out_valid,
    input  wire logic out_ready,
    output logic signed [SOFTBIT_WIDTH-1:0] out_soft_a,
    output logic signed [SOFTBIT_WIDTH-1:0] out_soft_b,
    output logic [DAB_OFDM_DATA_INDEX_WIDTH-1:0] out_carrier_index,
    output logic [63:0] out_symbol_start_sample,
    output logic out_symbol_done
);
  logic accept;
  logic [31:0] magnitude;

  assign in_ready = !out_valid || out_ready;
  assign accept = in_valid && in_ready;
  assign magnitude = complex_magnitude(in_i, in_q);

  function automatic logic [31:0] isqrt_u64(input logic [63:0] value);
    logic [63:0] op;
    logic [63:0] res;
    logic [63:0] one;
    integer step;
    begin
      op = value;
      res = 64'd0;
      one = 64'h4000_0000_0000_0000;

      for (step = 0; step < 32; step = step + 1) begin
        if (one > op) begin
          one = one >> 2;
        end
      end

      for (step = 0; step < 32; step = step + 1) begin
        if (one != 64'd0) begin
          if (op >= (res + one)) begin
            op = op - (res + one);
            res = (res >> 1) + one;
          end else begin
            res = res >> 1;
          end
          one = one >> 2;
        end
      end

      isqrt_u64 = res[31:0];
    end
  endfunction

  function automatic logic [31:0] complex_magnitude(
      input logic signed [CARRIER_WIDTH-1:0] carrier_i,
      input logic signed [CARRIER_WIDTH-1:0] carrier_q
  );
    longint signed i_ext;
    longint signed q_ext;
    longint unsigned abs_i;
    longint unsigned abs_q;
    longint unsigned power;
    begin
      i_ext = {{(64 - CARRIER_WIDTH){carrier_i[CARRIER_WIDTH-1]}}, carrier_i};
      q_ext = {{(64 - CARRIER_WIDTH){carrier_q[CARRIER_WIDTH-1]}}, carrier_q};
      abs_i = i_ext < 0 ? -i_ext : i_ext;
      abs_q = q_ext < 0 ? -q_ext : q_ext;
      power = (abs_i * abs_i) + (abs_q * abs_q);
      complex_magnitude = isqrt_u64(power);
    end
  endfunction

  function automatic logic signed [SOFTBIT_WIDTH-1:0] softbit_from_component(
      input logic signed [CARRIER_WIDTH-1:0] component,
      input logic [31:0] carrier_magnitude
  );
    longint signed component_ext;
    longint signed numerator;
    longint signed denominator;
    longint signed quotient;
    begin
      if (carrier_magnitude == 32'd0) begin
        quotient = 0;
      end else begin
        component_ext = {{(64 - CARRIER_WIDTH){component[CARRIER_WIDTH-1]}}, component};
        numerator = -component_ext * 64'sd255;
        denominator = {32'd0, carrier_magnitude};
        denominator = denominator <<< 1;
        quotient = numerator / denominator;
      end

      if (quotient > 127) begin
        softbit_from_component = 8'sd127;
      end else if (quotient < -128) begin
        softbit_from_component = {1'b1, {(SOFTBIT_WIDTH - 1){1'b0}}};
      end else begin
        softbit_from_component = SOFTBIT_WIDTH'(quotient);
      end
    end
  endfunction

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      out_valid <= 1'b0;
      out_soft_a <= '0;
      out_soft_b <= '0;
      out_carrier_index <= '0;
      out_symbol_start_sample <= '0;
      out_symbol_done <= 1'b0;
    end else if (clear) begin
      out_valid <= 1'b0;
      out_soft_a <= '0;
      out_soft_b <= '0;
      out_carrier_index <= '0;
      out_symbol_start_sample <= '0;
      out_symbol_done <= 1'b0;
    end else begin
      if (accept) begin
        out_valid <= 1'b1;
        out_soft_a <= softbit_from_component(in_i, magnitude);
        out_soft_b <= softbit_from_component(in_q, magnitude);
        out_carrier_index <= in_carrier_index;
        out_symbol_start_sample <= in_symbol_start_sample;
        out_symbol_done <= in_symbol_done;
      end else if (out_ready) begin
        out_valid <= 1'b0;
        out_symbol_done <= 1'b0;
      end
    end
  end
endmodule

`default_nettype wire
