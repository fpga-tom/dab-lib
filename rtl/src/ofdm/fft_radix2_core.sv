`default_nettype none

module fft_radix2_core
  import dab_ofdm_pkg::*;
#(
    parameter int INPUT_WIDTH = DAB_OFDM_SAMPLE_WIDTH,
    parameter int OUTPUT_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int BLOCK_EXP_WIDTH = DAB_OFDM_BLOCK_EXP_WIDTH,
    parameter int INTERNAL_WIDTH = DAB_OFDM_CARRIER_WIDTH,
    parameter int TWIDDLE_WIDTH = 18,
    parameter int TWIDDLE_FRAC_BITS = 16
) (
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear,

    input  wire logic core_in_valid,
    output logic core_in_ready,
    input  wire logic signed [INPUT_WIDTH-1:0] core_in_i,
    input  wire logic signed [INPUT_WIDTH-1:0] core_in_q,
    input  wire logic core_in_last,

    output logic core_out_valid,
    input  wire logic core_out_ready,
    output logic signed [OUTPUT_WIDTH-1:0] core_out_i,
    output logic signed [OUTPUT_WIDTH-1:0] core_out_q,
    output logic [BLOCK_EXP_WIDTH-1:0] core_out_block_exp,
    output logic core_out_overflow
);
  localparam int FFT_SAMPLES = DAB_OFDM_FFT_SAMPLES;
  localparam int FFT_INDEX_WIDTH = DAB_OFDM_FFT_INDEX_WIDTH;
  localparam int FFT_STAGES = FFT_INDEX_WIDTH;
  localparam int STAGE_WIDTH = $clog2(FFT_STAGES);
  localparam logic [FFT_INDEX_WIDTH-1:0] LAST_INDEX =
      FFT_INDEX_WIDTH'(FFT_SAMPLES - 1);

  typedef enum logic [2:0] {
    STATE_LOAD = 3'd0,
    STATE_READ = 3'd1,
    STATE_WRITE = 3'd2,
    STATE_OUTPUT_READ = 3'd3,
    STATE_OUTPUT_SEND = 3'd4
  } state_t;

  state_t state_q;

  logic [FFT_INDEX_WIDTH-1:0] load_index_q;
  logic [STAGE_WIDTH-1:0] stage_q;
  logic [FFT_INDEX_WIDTH-1:0] group_base_q;
  logic [FFT_INDEX_WIDTH-1:0] butterfly_index_q;
  logic [FFT_INDEX_WIDTH-1:0] output_index_q;
  logic [FFT_INDEX_WIDTH-1:0] addr_a_hold_q;
  logic [FFT_INDEX_WIDTH-1:0] addr_b_hold_q;

  logic signed [TWIDDLE_WIDTH-1:0] twiddle_r_q;
  logic signed [TWIDDLE_WIDTH-1:0] twiddle_i_q;
  logic frame_error_q;

  (* ram_style = "block", ramstyle = "M20K" *)
  logic signed [INTERNAL_WIDTH-1:0] mem_i [0:FFT_SAMPLES-1];
  (* ram_style = "block", ramstyle = "M20K" *)
  logic signed [INTERNAL_WIDTH-1:0] mem_q [0:FFT_SAMPLES-1];

  logic port_a_we;
  logic port_b_we;
  logic [FFT_INDEX_WIDTH-1:0] port_a_addr;
  logic [FFT_INDEX_WIDTH-1:0] port_b_addr;
  logic signed [INTERNAL_WIDTH-1:0] port_a_i_wr;
  logic signed [INTERNAL_WIDTH-1:0] port_a_q_wr;
  logic signed [INTERNAL_WIDTH-1:0] port_b_i_wr;
  logic signed [INTERNAL_WIDTH-1:0] port_b_q_wr;
  logic signed [INTERNAL_WIDTH-1:0] port_a_i_rd_q;
  logic signed [INTERNAL_WIDTH-1:0] port_a_q_rd_q;
  logic signed [INTERNAL_WIDTH-1:0] port_b_i_rd_q;
  logic signed [INTERNAL_WIDTH-1:0] port_b_q_rd_q;

  logic input_fire;
  logic output_fire;
  logic input_last_expected;
  logic [FFT_INDEX_WIDTH:0] half_size_ext;
  logic [FFT_INDEX_WIDTH:0] stage_size_ext;
  logic [FFT_INDEX_WIDTH:0] addr_a_ext;
  logic [FFT_INDEX_WIDTH:0] addr_b_ext;
  logic [FFT_INDEX_WIDTH:0] next_group_base_ext;
  logic last_butterfly_in_group;
  logic last_group_in_stage;
  logic last_stage;

  logic signed [INTERNAL_WIDTH-1:0] twiddled_i;
  logic signed [INTERNAL_WIDTH-1:0] twiddled_q;
  logic signed [INTERNAL_WIDTH-1:0] butterfly_a_i;
  logic signed [INTERNAL_WIDTH-1:0] butterfly_a_q;
  logic signed [INTERNAL_WIDTH-1:0] butterfly_b_i;
  logic signed [INTERNAL_WIDTH-1:0] butterfly_b_q;
  logic signed [TWIDDLE_WIDTH-1:0] twiddle_next_r;
  logic signed [TWIDDLE_WIDTH-1:0] twiddle_next_i;

  assign input_fire = core_in_valid && core_in_ready;
  assign output_fire = core_out_valid && core_out_ready;
  assign input_last_expected = load_index_q == LAST_INDEX;

  assign half_size_ext = ({{FFT_INDEX_WIDTH{1'b0}}, 1'b1} << stage_q);
  assign stage_size_ext = ({{(FFT_INDEX_WIDTH - 1){1'b0}}, 2'b10} << stage_q);
  assign addr_a_ext = {1'b0, group_base_q} + {1'b0, butterfly_index_q};
  assign addr_b_ext = addr_a_ext + half_size_ext;
  assign next_group_base_ext = {1'b0, group_base_q} + stage_size_ext;
  assign last_butterfly_in_group = {1'b0, butterfly_index_q} == (half_size_ext - 1'b1);
  assign last_group_in_stage = next_group_base_ext == (FFT_INDEX_WIDTH + 1)'(FFT_SAMPLES);
  assign last_stage = stage_q == STAGE_WIDTH'(FFT_STAGES - 1);

  assign core_in_ready = !clear && state_q == STATE_LOAD;
  assign core_out_valid = !clear && state_q == STATE_OUTPUT_SEND;
  assign core_out_i = resize_output(port_a_i_rd_q);
  assign core_out_q = resize_output(port_a_q_rd_q);
  assign core_out_block_exp = BLOCK_EXP_WIDTH'(FFT_STAGES);
  assign core_out_overflow = frame_error_q;

  assign butterfly_a_i = scale_sum(port_a_i_rd_q, twiddled_i);
  assign butterfly_a_q = scale_sum(port_a_q_rd_q, twiddled_q);
  assign butterfly_b_i = scale_diff(port_a_i_rd_q, twiddled_i);
  assign butterfly_b_q = scale_diff(port_a_q_rd_q, twiddled_q);

  assign twiddled_i = cmul_i(port_b_i_rd_q, port_b_q_rd_q, twiddle_r_q, twiddle_i_q);
  assign twiddled_q = cmul_q(port_b_i_rd_q, port_b_q_rd_q, twiddle_r_q, twiddle_i_q);
  assign twiddle_next_r = twiddle_update_r(twiddle_r_q,
                                           twiddle_i_q,
                                           twiddle_step_r(stage_q),
                                           twiddle_step_i(stage_q));
  assign twiddle_next_i = twiddle_update_i(twiddle_r_q,
                                           twiddle_i_q,
                                           twiddle_step_r(stage_q),
                                           twiddle_step_i(stage_q));

  always_comb begin
    port_a_we = 1'b0;
    port_b_we = 1'b0;
    port_a_addr = '0;
    port_b_addr = '0;
    port_a_i_wr = '0;
    port_a_q_wr = '0;
    port_b_i_wr = '0;
    port_b_q_wr = '0;

    unique case (state_q)
      STATE_LOAD: begin
        port_a_we = input_fire;
        port_a_addr = bit_reverse(load_index_q);
        port_a_i_wr = resize_input(core_in_i);
        port_a_q_wr = resize_input(core_in_q);
      end

      STATE_READ: begin
        port_a_addr = addr_a_ext[FFT_INDEX_WIDTH-1:0];
        port_b_addr = addr_b_ext[FFT_INDEX_WIDTH-1:0];
      end

      STATE_WRITE: begin
        port_a_we = 1'b1;
        port_b_we = 1'b1;
        port_a_addr = addr_a_hold_q;
        port_b_addr = addr_b_hold_q;
        port_a_i_wr = butterfly_a_i;
        port_a_q_wr = butterfly_a_q;
        port_b_i_wr = butterfly_b_i;
        port_b_q_wr = butterfly_b_q;
      end

      STATE_OUTPUT_READ,
      STATE_OUTPUT_SEND: begin
        port_a_addr = output_index_q;
      end

      default: begin
        port_a_addr = '0;
      end
    endcase
  end

  always_ff @(posedge clk) begin
    if (port_a_we) begin
      mem_i[port_a_addr] <= port_a_i_wr;
      mem_q[port_a_addr] <= port_a_q_wr;
    end else begin
      port_a_i_rd_q <= mem_i[port_a_addr];
      port_a_q_rd_q <= mem_q[port_a_addr];
    end

    if (port_b_we) begin
      mem_i[port_b_addr] <= port_b_i_wr;
      mem_q[port_b_addr] <= port_b_q_wr;
    end else begin
      port_b_i_rd_q <= mem_i[port_b_addr];
      port_b_q_rd_q <= mem_q[port_b_addr];
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state_q <= STATE_LOAD;
      load_index_q <= '0;
      stage_q <= '0;
      group_base_q <= '0;
      butterfly_index_q <= '0;
      output_index_q <= '0;
      addr_a_hold_q <= '0;
      addr_b_hold_q <= '0;
      twiddle_r_q <= twiddle_one();
      twiddle_i_q <= '0;
      frame_error_q <= 1'b0;
    end else if (clear) begin
      state_q <= STATE_LOAD;
      load_index_q <= '0;
      stage_q <= '0;
      group_base_q <= '0;
      butterfly_index_q <= '0;
      output_index_q <= '0;
      addr_a_hold_q <= '0;
      addr_b_hold_q <= '0;
      twiddle_r_q <= twiddle_one();
      twiddle_i_q <= '0;
      frame_error_q <= 1'b0;
    end else begin
      unique case (state_q)
        STATE_LOAD: begin
          if (input_fire) begin
            frame_error_q <= frame_error_q || (core_in_last != input_last_expected);

            if (input_last_expected) begin
              state_q <= STATE_READ;
              load_index_q <= '0;
              stage_q <= '0;
              group_base_q <= '0;
              butterfly_index_q <= '0;
              twiddle_r_q <= twiddle_one();
              twiddle_i_q <= '0;
            end else begin
              load_index_q <= load_index_q + 1'b1;
            end
          end
        end

        STATE_READ: begin
          addr_a_hold_q <= addr_a_ext[FFT_INDEX_WIDTH-1:0];
          addr_b_hold_q <= addr_b_ext[FFT_INDEX_WIDTH-1:0];
          frame_error_q <= frame_error_q || addr_b_ext[FFT_INDEX_WIDTH];
          state_q <= STATE_WRITE;
        end

        STATE_WRITE: begin
          if (last_butterfly_in_group) begin
            butterfly_index_q <= '0;
            twiddle_r_q <= twiddle_one();
            twiddle_i_q <= '0;

            if (last_group_in_stage) begin
              group_base_q <= '0;

              if (last_stage) begin
                state_q <= STATE_OUTPUT_READ;
                output_index_q <= '0;
              end else begin
                stage_q <= stage_q + 1'b1;
                state_q <= STATE_READ;
              end
            end else begin
              group_base_q <= next_group_base_ext[FFT_INDEX_WIDTH-1:0];
              state_q <= STATE_READ;
            end
          end else begin
            butterfly_index_q <= butterfly_index_q + 1'b1;
            twiddle_r_q <= twiddle_next_r;
            twiddle_i_q <= twiddle_next_i;
            state_q <= STATE_READ;
          end
        end

        STATE_OUTPUT_READ: begin
          state_q <= STATE_OUTPUT_SEND;
        end

        STATE_OUTPUT_SEND: begin
          if (output_fire) begin
            if (output_index_q == LAST_INDEX) begin
              state_q <= STATE_LOAD;
              output_index_q <= '0;
              frame_error_q <= 1'b0;
            end else begin
              output_index_q <= output_index_q + 1'b1;
              state_q <= STATE_OUTPUT_READ;
            end
          end
        end

        default: begin
          state_q <= STATE_LOAD;
        end
      endcase
    end
  end

  function automatic logic [FFT_INDEX_WIDTH-1:0] bit_reverse(
      input logic [FFT_INDEX_WIDTH-1:0] value
  );
    integer bit_index;
    begin
      for (bit_index = 0; bit_index < FFT_INDEX_WIDTH; bit_index = bit_index + 1) begin
        bit_reverse[bit_index] = value[FFT_INDEX_WIDTH - 1 - bit_index];
      end
    end
  endfunction

  function automatic logic signed [INTERNAL_WIDTH-1:0] resize_input(
      input logic signed [INPUT_WIDTH-1:0] value
  );
    logic signed [63:0] extended;
    begin
      extended = {{(64 - INPUT_WIDTH){value[INPUT_WIDTH-1]}}, value};
      resize_input = saturate_internal(extended);
    end
  endfunction

  function automatic logic signed [OUTPUT_WIDTH-1:0] resize_output(
      input logic signed [INTERNAL_WIDTH-1:0] value
  );
    logic signed [63:0] extended;
    begin
      extended = {{(64 - INTERNAL_WIDTH){value[INTERNAL_WIDTH-1]}}, value};
      resize_output = saturate_output(extended);
    end
  endfunction

  function automatic logic signed [INTERNAL_WIDTH-1:0] scale_sum(
      input logic signed [INTERNAL_WIDTH-1:0] a,
      input logic signed [INTERNAL_WIDTH-1:0] b
  );
    logic signed [63:0] wide;
    begin
      wide = sign_extend_internal(a) + sign_extend_internal(b);
      scale_sum = saturate_internal(wide >>> 1);
    end
  endfunction

  function automatic logic signed [INTERNAL_WIDTH-1:0] scale_diff(
      input logic signed [INTERNAL_WIDTH-1:0] a,
      input logic signed [INTERNAL_WIDTH-1:0] b
  );
    logic signed [63:0] wide;
    begin
      wide = sign_extend_internal(a) - sign_extend_internal(b);
      scale_diff = saturate_internal(wide >>> 1);
    end
  endfunction

  function automatic logic signed [INTERNAL_WIDTH-1:0] cmul_i(
      input logic signed [INTERNAL_WIDTH-1:0] sample_i,
      input logic signed [INTERNAL_WIDTH-1:0] sample_q,
      input logic signed [TWIDDLE_WIDTH-1:0] twiddle_i,
      input logic signed [TWIDDLE_WIDTH-1:0] twiddle_q
  );
    logic signed [63:0] product;
    begin
      product = (sign_extend_internal(sample_i) * sign_extend_twiddle(twiddle_i)) -
                (sign_extend_internal(sample_q) * sign_extend_twiddle(twiddle_q));
      cmul_i = saturate_internal(product >>> TWIDDLE_FRAC_BITS);
    end
  endfunction

  function automatic logic signed [INTERNAL_WIDTH-1:0] cmul_q(
      input logic signed [INTERNAL_WIDTH-1:0] sample_i,
      input logic signed [INTERNAL_WIDTH-1:0] sample_q,
      input logic signed [TWIDDLE_WIDTH-1:0] twiddle_i,
      input logic signed [TWIDDLE_WIDTH-1:0] twiddle_q
  );
    logic signed [63:0] product;
    begin
      product = (sign_extend_internal(sample_i) * sign_extend_twiddle(twiddle_q)) +
                (sign_extend_internal(sample_q) * sign_extend_twiddle(twiddle_i));
      cmul_q = saturate_internal(product >>> TWIDDLE_FRAC_BITS);
    end
  endfunction

  function automatic logic signed [TWIDDLE_WIDTH-1:0] twiddle_update_r(
      input logic signed [TWIDDLE_WIDTH-1:0] current_r,
      input logic signed [TWIDDLE_WIDTH-1:0] current_i,
      input logic signed [TWIDDLE_WIDTH-1:0] step_r,
      input logic signed [TWIDDLE_WIDTH-1:0] step_i
  );
    logic signed [63:0] product;
    begin
      product = (sign_extend_twiddle(current_r) * sign_extend_twiddle(step_r)) -
                (sign_extend_twiddle(current_i) * sign_extend_twiddle(step_i));
      twiddle_update_r = saturate_twiddle(product >>> TWIDDLE_FRAC_BITS);
    end
  endfunction

  function automatic logic signed [TWIDDLE_WIDTH-1:0] twiddle_update_i(
      input logic signed [TWIDDLE_WIDTH-1:0] current_r,
      input logic signed [TWIDDLE_WIDTH-1:0] current_i,
      input logic signed [TWIDDLE_WIDTH-1:0] step_r,
      input logic signed [TWIDDLE_WIDTH-1:0] step_i
  );
    logic signed [63:0] product;
    begin
      product = (sign_extend_twiddle(current_r) * sign_extend_twiddle(step_i)) +
                (sign_extend_twiddle(current_i) * sign_extend_twiddle(step_r));
      twiddle_update_i = saturate_twiddle(product >>> TWIDDLE_FRAC_BITS);
    end
  endfunction

  function automatic logic signed [TWIDDLE_WIDTH-1:0] twiddle_step_r(
      input logic [STAGE_WIDTH-1:0] stage
  );
    begin
      unique case (stage)
        4'd0: twiddle_step_r = twiddle_const(-65536);
        4'd1: twiddle_step_r = twiddle_const(0);
        4'd2: twiddle_step_r = twiddle_const(46341);
        4'd3: twiddle_step_r = twiddle_const(60547);
        4'd4: twiddle_step_r = twiddle_const(64277);
        4'd5: twiddle_step_r = twiddle_const(65220);
        4'd6: twiddle_step_r = twiddle_const(65457);
        4'd7: twiddle_step_r = twiddle_const(65516);
        4'd8: twiddle_step_r = twiddle_const(65531);
        4'd9: twiddle_step_r = twiddle_const(65535);
        default: twiddle_step_r = twiddle_const(65536);
      endcase
    end
  endfunction

  function automatic logic signed [TWIDDLE_WIDTH-1:0] twiddle_step_i(
      input logic [STAGE_WIDTH-1:0] stage
  );
    begin
      unique case (stage)
        4'd0: twiddle_step_i = twiddle_const(0);
        4'd1: twiddle_step_i = twiddle_const(-65536);
        4'd2: twiddle_step_i = twiddle_const(-46341);
        4'd3: twiddle_step_i = twiddle_const(-25080);
        4'd4: twiddle_step_i = twiddle_const(-12785);
        4'd5: twiddle_step_i = twiddle_const(-6424);
        4'd6: twiddle_step_i = twiddle_const(-3216);
        4'd7: twiddle_step_i = twiddle_const(-1608);
        4'd8: twiddle_step_i = twiddle_const(-804);
        4'd9: twiddle_step_i = twiddle_const(-402);
        default: twiddle_step_i = twiddle_const(-201);
      endcase
    end
  endfunction

  function automatic logic signed [TWIDDLE_WIDTH-1:0] twiddle_one();
    begin
      twiddle_one = twiddle_const(1 << TWIDDLE_FRAC_BITS);
    end
  endfunction

  function automatic logic signed [TWIDDLE_WIDTH-1:0] twiddle_const(input int value);
    logic signed [31:0] wide;
    begin
      wide = value;
      twiddle_const = saturate_twiddle({{32{wide[31]}}, wide});
    end
  endfunction

  function automatic logic signed [63:0] sign_extend_internal(
      input logic signed [INTERNAL_WIDTH-1:0] value
  );
    begin
      sign_extend_internal = {{(64 - INTERNAL_WIDTH){value[INTERNAL_WIDTH-1]}}, value};
    end
  endfunction

  function automatic logic signed [63:0] sign_extend_twiddle(
      input logic signed [TWIDDLE_WIDTH-1:0] value
  );
    begin
      sign_extend_twiddle = {{(64 - TWIDDLE_WIDTH){value[TWIDDLE_WIDTH-1]}}, value};
    end
  endfunction

  function automatic logic signed [INTERNAL_WIDTH-1:0] saturate_internal(
      input logic signed [63:0] value
  );
    logic signed [63:0] max_value;
    logic signed [63:0] min_value;
    begin
      max_value = (64'sd1 <<< (INTERNAL_WIDTH - 1)) - 64'sd1;
      min_value = -(64'sd1 <<< (INTERNAL_WIDTH - 1));

      if (value > max_value) begin
        saturate_internal = max_value[INTERNAL_WIDTH-1:0];
      end else if (value < min_value) begin
        saturate_internal = min_value[INTERNAL_WIDTH-1:0];
      end else begin
        saturate_internal = value[INTERNAL_WIDTH-1:0];
      end
    end
  endfunction

  function automatic logic signed [OUTPUT_WIDTH-1:0] saturate_output(
      input logic signed [63:0] value
  );
    logic signed [63:0] max_value;
    logic signed [63:0] min_value;
    begin
      max_value = (64'sd1 <<< (OUTPUT_WIDTH - 1)) - 64'sd1;
      min_value = -(64'sd1 <<< (OUTPUT_WIDTH - 1));

      if (value > max_value) begin
        saturate_output = max_value[OUTPUT_WIDTH-1:0];
      end else if (value < min_value) begin
        saturate_output = min_value[OUTPUT_WIDTH-1:0];
      end else begin
        saturate_output = value[OUTPUT_WIDTH-1:0];
      end
    end
  endfunction

  function automatic logic signed [TWIDDLE_WIDTH-1:0] saturate_twiddle(
      input logic signed [63:0] value
  );
    logic signed [63:0] max_value;
    logic signed [63:0] min_value;
    begin
      max_value = (64'sd1 <<< (TWIDDLE_WIDTH - 1)) - 64'sd1;
      min_value = -(64'sd1 <<< (TWIDDLE_WIDTH - 1));

      if (value > max_value) begin
        saturate_twiddle = max_value[TWIDDLE_WIDTH-1:0];
      end else if (value < min_value) begin
        saturate_twiddle = min_value[TWIDDLE_WIDTH-1:0];
      end else begin
        saturate_twiddle = value[TWIDDLE_WIDTH-1:0];
      end
    end
  endfunction
endmodule

`default_nettype wire
