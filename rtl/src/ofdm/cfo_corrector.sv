`default_nettype none

module cfo_corrector
  import dab_ofdm_pkg::*;
#(
    parameter int INPUT_WIDTH = 16,
    parameter int OUTPUT_WIDTH = DAB_OFDM_SAMPLE_WIDTH,
    parameter int PHASE_WIDTH = 32,
    parameter int CORDIC_STAGES = 16,
    parameter int SAMPLE_OFFSET_WIDTH = DAB_OFDM_FFT_INDEX_WIDTH
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic cfg_bypass,
    input  wire logic [PHASE_WIDTH-1:0] cfg_phase_inc,
    input  wire logic cfg_phase_load,
    input  wire logic [PHASE_WIDTH-1:0] cfg_phase_seed,

    input  wire logic in_valid,
    output logic in_ready,
    input  wire logic signed [INPUT_WIDTH-1:0] in_i,
    input  wire logic signed [INPUT_WIDTH-1:0] in_q,
    input  wire logic [63:0] in_sample_index,
    input  wire logic [63:0] in_symbol_start_sample,
    input  wire logic [SAMPLE_OFFSET_WIDTH-1:0] in_sample_offset,
    input  wire logic in_symbol_done,
    input  wire logic in_error,

    output logic out_valid,
    input  wire logic out_ready,
    output logic signed [OUTPUT_WIDTH-1:0] out_i,
    output logic signed [OUTPUT_WIDTH-1:0] out_q,
    output logic [63:0] out_sample_index,
    output logic [63:0] out_symbol_start_sample,
    output logic [SAMPLE_OFFSET_WIDTH-1:0] out_sample_offset,
    output logic out_symbol_done,
    output logic out_error,
    output logic [PHASE_WIDTH-1:0] out_phase
);
  localparam int BASE_WIDTH = INPUT_WIDTH > OUTPUT_WIDTH ? INPUT_WIDTH : OUTPUT_WIDTH;
  localparam int INTERNAL_WIDTH = BASE_WIDTH + CORDIC_STAGES + 3;
  localparam int GAIN_FRAC_BITS = 16;
  localparam int GAIN_WIDTH = GAIN_FRAC_BITS + 1;
  localparam int PRODUCT_WIDTH = INPUT_WIDTH + GAIN_WIDTH;
  localparam logic signed [GAIN_WIDTH-1:0] CORDIC_GAIN_SCALE = GAIN_WIDTH'(39797);

  logic valid_q [0:CORDIC_STAGES];
  logic bypass_q [0:CORDIC_STAGES];
  logic error_q [0:CORDIC_STAGES];
  logic done_q [0:CORDIC_STAGES];
  logic signed [INTERNAL_WIDTH-1:0] x_q [0:CORDIC_STAGES];
  logic signed [INTERNAL_WIDTH-1:0] y_q [0:CORDIC_STAGES];
  logic signed [PHASE_WIDTH:0] z_q [0:CORDIC_STAGES];
  logic [63:0] sample_index_q [0:CORDIC_STAGES];
  logic [63:0] symbol_start_sample_q [0:CORDIC_STAGES];
  logic [SAMPLE_OFFSET_WIDTH-1:0] sample_offset_q [0:CORDIC_STAGES];
  logic [PHASE_WIDTH-1:0] phase_used_q [0:CORDIC_STAGES];

  logic [PHASE_WIDTH-1:0] phase_q;
  logic [PHASE_WIDTH-1:0] phase_for_input;
  logic [PHASE_WIDTH-1:0] residual_phase;
  logic signed [INTERNAL_WIDTH-1:0] input_i_scaled;
  logic signed [INTERNAL_WIDTH-1:0] input_q_scaled;
  logic signed [INTERNAL_WIDTH-1:0] input_i_extended;
  logic signed [INTERNAL_WIDTH-1:0] input_q_extended;
  logic signed [INTERNAL_WIDTH-1:0] prerot_i;
  logic signed [INTERNAL_WIDTH-1:0] prerot_q;
  logic advance_pipeline;
  logic input_fire;

  assign out_valid = !clear && valid_q[CORDIC_STAGES];
  assign advance_pipeline = !out_valid || out_ready;
  assign in_ready = !clear && advance_pipeline;
  assign input_fire = in_valid && in_ready;

  assign out_i = out_error ? '0 : saturate_output(x_q[CORDIC_STAGES]);
  assign out_q = out_error ? '0 : saturate_output(y_q[CORDIC_STAGES]);
  assign out_sample_index = sample_index_q[CORDIC_STAGES];
  assign out_symbol_start_sample = symbol_start_sample_q[CORDIC_STAGES];
  assign out_sample_offset = sample_offset_q[CORDIC_STAGES];
  assign out_symbol_done = done_q[CORDIC_STAGES];
  assign out_error = error_q[CORDIC_STAGES];
  assign out_phase = phase_used_q[CORDIC_STAGES];

  assign phase_for_input = cfg_phase_load ? cfg_phase_seed : phase_q;
  assign residual_phase = {{2{1'b0}}, phase_for_input[PHASE_WIDTH-3:0]};
  assign input_i_scaled = prescale_input(in_i);
  assign input_q_scaled = prescale_input(in_q);
  assign input_i_extended = extend_input(in_i);
  assign input_q_extended = extend_input(in_q);

  always_comb begin
    prerot_i = '0;
    prerot_q = '0;

    if (in_error) begin
      prerot_i = '0;
      prerot_q = '0;
    end else if (cfg_bypass) begin
      prerot_i = input_i_extended;
      prerot_q = input_q_extended;
    end else begin
      unique case (phase_for_input[PHASE_WIDTH-1 -: 2])
        2'b00: begin
          prerot_i = input_i_scaled;
          prerot_q = input_q_scaled;
        end
        2'b01: begin
          prerot_i = -input_q_scaled;
          prerot_q = input_i_scaled;
        end
        2'b10: begin
          prerot_i = -input_i_scaled;
          prerot_q = -input_q_scaled;
        end
        default: begin
          prerot_i = input_q_scaled;
          prerot_q = -input_i_scaled;
        end
      endcase
    end
  end

  function automatic logic signed [INTERNAL_WIDTH-1:0] extend_input(
      input logic signed [INPUT_WIDTH-1:0] value
  );
    begin
      extend_input = {{(INTERNAL_WIDTH - INPUT_WIDTH){value[INPUT_WIDTH-1]}}, value};
    end
  endfunction

  function automatic logic signed [INTERNAL_WIDTH-1:0] prescale_input(
      input logic signed [INPUT_WIDTH-1:0] value
  );
    logic signed [PRODUCT_WIDTH-1:0] value_ext;
    logic signed [PRODUCT_WIDTH-1:0] gain_ext;
    logic signed [PRODUCT_WIDTH-1:0] product;
    logic signed [PRODUCT_WIDTH-1:0] shifted;
    begin
      value_ext = {{(PRODUCT_WIDTH - INPUT_WIDTH){value[INPUT_WIDTH-1]}}, value};
      gain_ext = {{(PRODUCT_WIDTH - GAIN_WIDTH){CORDIC_GAIN_SCALE[GAIN_WIDTH-1]}},
                  CORDIC_GAIN_SCALE};
      product = value_ext * gain_ext;
      shifted = product >>> GAIN_FRAC_BITS;
      prescale_input = {{(INTERNAL_WIDTH - PRODUCT_WIDTH){shifted[PRODUCT_WIDTH-1]}},
                        shifted};
    end
  endfunction

  function automatic logic signed [PHASE_WIDTH:0] atan_phase(
      input int unsigned stage
  );
    logic [31:0] value;
    begin
      unique case (stage)
        0: value = 32'h20000000;
        1: value = 32'h12e4051e;
        2: value = 32'h09fb385b;
        3: value = 32'h051111d4;
        4: value = 32'h028b0d43;
        5: value = 32'h0145d7e1;
        6: value = 32'h00a2f61e;
        7: value = 32'h00517c55;
        8: value = 32'h0028be53;
        9: value = 32'h00145f2f;
        10: value = 32'h000a2f98;
        11: value = 32'h000517cc;
        12: value = 32'h00028be6;
        13: value = 32'h000145f3;
        14: value = 32'h0000a2fa;
        15: value = 32'h0000517d;
        16: value = 32'h000028be;
        17: value = 32'h0000145f;
        18: value = 32'h00000a30;
        19: value = 32'h00000518;
        default: value = 32'h00000000;
      endcase

      atan_phase = {1'b0, PHASE_WIDTH'(value)};
    end
  endfunction

  function automatic logic signed [OUTPUT_WIDTH-1:0] saturate_output(
      input logic signed [INTERNAL_WIDTH-1:0] value
  );
    logic signed [63:0] wide;
    logic signed [63:0] max_value;
    logic signed [63:0] min_value;
    begin
      wide = {{(64 - INTERNAL_WIDTH){value[INTERNAL_WIDTH-1]}}, value};
      max_value = (64'sd1 <<< (OUTPUT_WIDTH - 1)) - 64'sd1;
      min_value = -(64'sd1 <<< (OUTPUT_WIDTH - 1));

      if (wide > max_value) begin
        saturate_output = max_value[OUTPUT_WIDTH-1:0];
      end else if (wide < min_value) begin
        saturate_output = min_value[OUTPUT_WIDTH-1:0];
      end else begin
        saturate_output = wide[OUTPUT_WIDTH-1:0];
      end
    end
  endfunction

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      phase_q <= '0;
      for (int i = 0; i <= CORDIC_STAGES; i++) begin
        valid_q[i] <= 1'b0;
        bypass_q[i] <= 1'b0;
        error_q[i] <= 1'b0;
        done_q[i] <= 1'b0;
        x_q[i] <= '0;
        y_q[i] <= '0;
        z_q[i] <= '0;
        sample_index_q[i] <= '0;
        symbol_start_sample_q[i] <= '0;
        sample_offset_q[i] <= '0;
        phase_used_q[i] <= '0;
      end
    end else if (clear) begin
      phase_q <= cfg_phase_load ? cfg_phase_seed : '0;
      for (int i = 0; i <= CORDIC_STAGES; i++) begin
        valid_q[i] <= 1'b0;
        bypass_q[i] <= 1'b0;
        error_q[i] <= 1'b0;
        done_q[i] <= 1'b0;
        x_q[i] <= '0;
        y_q[i] <= '0;
        z_q[i] <= '0;
        sample_index_q[i] <= '0;
        symbol_start_sample_q[i] <= '0;
        sample_offset_q[i] <= '0;
        phase_used_q[i] <= '0;
      end
    end else begin
      if (cfg_phase_load) begin
        phase_q <= cfg_phase_seed;
      end

      if (input_fire && !in_error) begin
        phase_q <= phase_for_input + cfg_phase_inc;
      end

      if (advance_pipeline) begin
        valid_q[0] <= input_fire;
        bypass_q[0] <= cfg_bypass;
        error_q[0] <= input_fire && in_error;
        done_q[0] <= input_fire && in_symbol_done;
        x_q[0] <= input_fire ? prerot_i : '0;
        y_q[0] <= input_fire ? prerot_q : '0;
        z_q[0] <= input_fire && !cfg_bypass && !in_error ? {1'b0, residual_phase} : '0;
        sample_index_q[0] <= input_fire ? in_sample_index : '0;
        symbol_start_sample_q[0] <= input_fire ? in_symbol_start_sample : '0;
        sample_offset_q[0] <= input_fire ? in_sample_offset : '0;
        phase_used_q[0] <= input_fire ? phase_for_input : '0;

        for (int i = 0; i < CORDIC_STAGES; i++) begin
          valid_q[i + 1] <= valid_q[i];
          bypass_q[i + 1] <= bypass_q[i];
          error_q[i + 1] <= error_q[i];
          done_q[i + 1] <= done_q[i];
          sample_index_q[i + 1] <= sample_index_q[i];
          symbol_start_sample_q[i + 1] <= symbol_start_sample_q[i];
          sample_offset_q[i + 1] <= sample_offset_q[i];
          phase_used_q[i + 1] <= phase_used_q[i];

          if (!valid_q[i] || bypass_q[i] || error_q[i]) begin
            x_q[i + 1] <= x_q[i];
            y_q[i + 1] <= y_q[i];
            z_q[i + 1] <= z_q[i];
          end else if (!z_q[i][PHASE_WIDTH]) begin
            x_q[i + 1] <= x_q[i] - (y_q[i] >>> i);
            y_q[i + 1] <= y_q[i] + (x_q[i] >>> i);
            z_q[i + 1] <= z_q[i] - atan_phase(i);
          end else begin
            x_q[i + 1] <= x_q[i] + (y_q[i] >>> i);
            y_q[i + 1] <= y_q[i] - (x_q[i] >>> i);
            z_q[i + 1] <= z_q[i] + atan_phase(i);
          end
        end
      end
    end
  end
endmodule

`default_nettype wire
