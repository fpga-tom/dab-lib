`default_nettype none

module qpsk_softbits
  import dab_ofdm_pkg::*;
#(
    parameter int CARRIER_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int SOFTBIT_WIDTH = DAB_OFDM_SOFTBIT_WIDTH,
    parameter int EXACT_NORMALIZE = 1,
    parameter int RAW_SHIFT = CARRIER_WIDTH > SOFTBIT_WIDTH ? CARRIER_WIDTH - SOFTBIT_WIDTH : 0
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
  localparam int MAG_WIDTH = CARRIER_WIDTH;
  localparam int POWER_WIDTH = 2 * CARRIER_WIDTH;
  localparam int ISQRT_ONE_BIT = ((POWER_WIDTH - 1) / 2) * 2;
  localparam int NUM_WIDTH = CARRIER_WIDTH + SOFTBIT_WIDTH + 1;
  localparam int DEN_WIDTH = CARRIER_WIDTH + 2;
  localparam int RAW_WIDTH = CARRIER_WIDTH + 1;

  logic accept;
  logic [MAG_WIDTH-1:0] magnitude;

  assign in_ready = !out_valid || out_ready;
  assign accept = in_valid && in_ready;

  generate
    if (EXACT_NORMALIZE != 0) begin : gen_exact_magnitude
      assign magnitude = complex_magnitude(in_i, in_q);
    end else begin : gen_no_magnitude
      assign magnitude = '0;
    end
  endgenerate

  function automatic logic [MAG_WIDTH-1:0] isqrt_power(
      input logic [POWER_WIDTH-1:0] value
  );
    logic [POWER_WIDTH-1:0] op;
    logic [POWER_WIDTH-1:0] res;
    logic [POWER_WIDTH-1:0] one;
    logic [POWER_WIDTH:0] trial;
    integer step;
    begin
      op = value;
      res = '0;
      one = '0;
      one[ISQRT_ONE_BIT] = 1'b1;

      for (step = 0; step < MAG_WIDTH; step = step + 1) begin
        if (one > op) begin
          one = one >> 2;
        end
      end

      for (step = 0; step < MAG_WIDTH; step = step + 1) begin
        if (one != '0) begin
          trial = {1'b0, res} + {1'b0, one};
          if ({1'b0, op} >= trial) begin
            op = POWER_WIDTH'({1'b0, op} - trial);
            res = (res >> 1) + one;
          end else begin
            res = res >> 1;
          end
          one = one >> 2;
        end
      end

      isqrt_power = res[MAG_WIDTH-1:0];
    end
  endfunction

  function automatic logic [CARRIER_WIDTH-1:0] abs_carrier(
      input logic signed [CARRIER_WIDTH-1:0] value
  );
    logic [CARRIER_WIDTH-1:0] value_u;
    begin
      value_u = value;
      abs_carrier = value[CARRIER_WIDTH-1] ? (~value_u + CARRIER_WIDTH'(1)) : value_u;
    end
  endfunction

  function automatic logic [MAG_WIDTH-1:0] complex_magnitude(
      input logic signed [CARRIER_WIDTH-1:0] carrier_i,
      input logic signed [CARRIER_WIDTH-1:0] carrier_q
  );
    logic [CARRIER_WIDTH-1:0] abs_i;
    logic [CARRIER_WIDTH-1:0] abs_q;
    logic [POWER_WIDTH-1:0] i_sq;
    logic [POWER_WIDTH-1:0] q_sq;
    logic [POWER_WIDTH-1:0] power;
    begin
      abs_i = abs_carrier(carrier_i);
      abs_q = abs_carrier(carrier_q);
      i_sq = POWER_WIDTH'(abs_i * abs_i);
      q_sq = POWER_WIDTH'(abs_q * abs_q);
      power = i_sq + q_sq;
      complex_magnitude = isqrt_power(power);
    end
  endfunction

  function automatic logic signed [SOFTBIT_WIDTH-1:0] softbit_from_component(
      input logic signed [CARRIER_WIDTH-1:0] component,
      input logic [MAG_WIDTH-1:0] carrier_magnitude
  );
    logic signed [NUM_WIDTH-1:0] component_ext;
    logic signed [NUM_WIDTH-1:0] numerator;
    logic signed [DEN_WIDTH-1:0] denominator;
    logic signed [NUM_WIDTH-1:0] denominator_ext;
    logic signed [NUM_WIDTH-1:0] quotient;
    logic signed [NUM_WIDTH-1:0] soft_max;
    logic signed [NUM_WIDTH-1:0] soft_min;
    begin
      soft_max = (NUM_WIDTH'(1) <<< (SOFTBIT_WIDTH - 1)) - NUM_WIDTH'(1);
      soft_min = -(NUM_WIDTH'(1) <<< (SOFTBIT_WIDTH - 1));

      if (carrier_magnitude == '0) begin
        quotient = '0;
      end else begin
        component_ext = {{(NUM_WIDTH - CARRIER_WIDTH){component[CARRIER_WIDTH-1]}}, component};
        numerator = component_ext - (component_ext <<< SOFTBIT_WIDTH);
        denominator = $signed({1'b0, carrier_magnitude, 1'b0});
        denominator_ext = {{(NUM_WIDTH - DEN_WIDTH){1'b0}}, denominator};
        quotient = numerator / denominator_ext;
      end

      if (quotient > soft_max) begin
        softbit_from_component = {1'b0, {(SOFTBIT_WIDTH - 1){1'b1}}};
      end else if (quotient < soft_min) begin
        softbit_from_component = {1'b1, {(SOFTBIT_WIDTH - 1){1'b0}}};
      end else begin
        softbit_from_component = SOFTBIT_WIDTH'(quotient);
      end
    end
  endfunction

  function automatic logic signed [SOFTBIT_WIDTH-1:0] raw_softbit_from_component(
      input logic signed [CARRIER_WIDTH-1:0] component
  );
    logic signed [RAW_WIDTH-1:0] inverted;
    logic signed [RAW_WIDTH-1:0] shifted;
    logic signed [RAW_WIDTH-1:0] soft_max;
    logic signed [RAW_WIDTH-1:0] soft_min;
    begin
      inverted = -({component[CARRIER_WIDTH-1], component});
      if (RAW_SHIFT == 0) begin
        shifted = inverted;
      end else begin
        shifted = inverted >>> RAW_SHIFT;
      end

      soft_max = (RAW_WIDTH'(1) <<< (SOFTBIT_WIDTH - 1)) - RAW_WIDTH'(1);
      soft_min = -(RAW_WIDTH'(1) <<< (SOFTBIT_WIDTH - 1));
      if (shifted > soft_max) begin
        raw_softbit_from_component = {1'b0, {(SOFTBIT_WIDTH - 1){1'b1}}};
      end else if (shifted < soft_min) begin
        raw_softbit_from_component = {1'b1, {(SOFTBIT_WIDTH - 1){1'b0}}};
      end else begin
        raw_softbit_from_component = SOFTBIT_WIDTH'(shifted);
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
        if (EXACT_NORMALIZE != 0) begin
          out_soft_a <= softbit_from_component(in_i, magnitude);
          out_soft_b <= softbit_from_component(in_q, magnitude);
        end else begin
          out_soft_a <= raw_softbit_from_component(in_i);
          out_soft_b <= raw_softbit_from_component(in_q);
        end
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
