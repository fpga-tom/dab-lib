`default_nettype none

module tframe_assembler
  import dab_tframe_pkg::*;
(
    input  logic clk,
    input  logic rst_n,
    input  logic clear,

    input  logic in_valid,
    output logic in_ready,
    input  logic signed [DAB_TFRAME_SOFTBIT_WIDTH-1:0] in_softbit,
    input  logic [63:0] in_symbol_start_sample,

    output logic out_valid,
    input  logic out_ready,
    output logic signed [DAB_TFRAME_SOFTBIT_WIDTH-1:0] out_softbit,
    output logic out_region,
    output logic [DAB_TFRAME_ADDR_WIDTH-1:0] out_addr,
    output logic [DAB_TFRAME_SYMBOL_INDEX_WIDTH-1:0] out_symbol_index,
    output logic [DAB_TFRAME_BYTE_INDEX_WIDTH-1:0] out_byte_index,
    output logic [63:0] out_frame_start_sample,
    output logic out_frame_done
);
  localparam logic [DAB_TFRAME_SYMBOL_INDEX_WIDTH-1:0] LAST_SYMBOL_INDEX =
      DAB_TFRAME_SYMBOL_INDEX_WIDTH'(DAB_TFRAME_TOTAL_SYMBOLS - 1);
  localparam logic [DAB_TFRAME_SYMBOL_INDEX_WIDTH-1:0] LAST_FIC_SYMBOL_INDEX =
      DAB_TFRAME_SYMBOL_INDEX_WIDTH'(DAB_TFRAME_FIC_SYMBOLS - 1);
  localparam logic [DAB_TFRAME_SYMBOL_INDEX_WIDTH-1:0] FIC_SYMBOL_COUNT =
      DAB_TFRAME_SYMBOL_INDEX_WIDTH'(DAB_TFRAME_FIC_SYMBOLS);
  localparam logic [DAB_TFRAME_BYTE_INDEX_WIDTH-1:0] LAST_BYTE_INDEX =
      DAB_TFRAME_BYTE_INDEX_WIDTH'(DAB_TFRAME_SOFTBITS_PER_SYMBOL - 1);

  logic [DAB_TFRAME_SYMBOL_INDEX_WIDTH-1:0] symbol_index_q;
  logic [DAB_TFRAME_BYTE_INDEX_WIDTH-1:0] byte_index_q;
  logic [DAB_TFRAME_ADDR_WIDTH-1:0] region_addr_q;
  logic [63:0] frame_start_sample_q;

  logic accept;
  logic current_region;
  logic first_frame_byte;
  logic last_symbol;
  logic last_fic_symbol;
  logic last_byte;

  assign in_ready = !out_valid || out_ready;
  assign accept = in_valid && in_ready;

  assign current_region =
      (symbol_index_q < FIC_SYMBOL_COUNT) ? DAB_TFRAME_REGION_FIC
                                          : DAB_TFRAME_REGION_MSC;
  assign first_frame_byte = (symbol_index_q == '0) && (byte_index_q == '0);
  assign last_symbol = symbol_index_q == LAST_SYMBOL_INDEX;
  assign last_fic_symbol = symbol_index_q == LAST_FIC_SYMBOL_INDEX;
  assign last_byte = byte_index_q == LAST_BYTE_INDEX;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      out_valid <= 1'b0;
      out_softbit <= '0;
      out_region <= DAB_TFRAME_REGION_FIC;
      out_addr <= '0;
      out_symbol_index <= '0;
      out_byte_index <= '0;
      out_frame_start_sample <= '0;
      out_frame_done <= 1'b0;
      symbol_index_q <= '0;
      byte_index_q <= '0;
      region_addr_q <= '0;
      frame_start_sample_q <= '0;
    end else if (clear) begin
      out_valid <= 1'b0;
      out_softbit <= '0;
      out_region <= DAB_TFRAME_REGION_FIC;
      out_addr <= '0;
      out_symbol_index <= '0;
      out_byte_index <= '0;
      out_frame_start_sample <= '0;
      out_frame_done <= 1'b0;
      symbol_index_q <= '0;
      byte_index_q <= '0;
      region_addr_q <= '0;
      frame_start_sample_q <= '0;
    end else begin
      if (accept) begin
        out_valid <= 1'b1;
        out_softbit <= in_softbit;
        out_region <= current_region;
        out_addr <= region_addr_q;
        out_symbol_index <= symbol_index_q;
        out_byte_index <= byte_index_q;
        out_frame_start_sample <= first_frame_byte ? in_symbol_start_sample
                                                   : frame_start_sample_q;
        out_frame_done <= last_symbol && last_byte;

        if (first_frame_byte) begin
          frame_start_sample_q <= in_symbol_start_sample;
        end

        if (last_byte) begin
          byte_index_q <= '0;
          region_addr_q <= (last_symbol || last_fic_symbol) ? '0
                                                            : region_addr_q + 1'b1;
          symbol_index_q <= last_symbol ? '0 : symbol_index_q + 1'b1;
        end else begin
          byte_index_q <= byte_index_q + 1'b1;
          region_addr_q <= region_addr_q + 1'b1;
        end
      end else if (out_ready) begin
        out_valid <= 1'b0;
        out_frame_done <= 1'b0;
      end
    end
  end
endmodule

`default_nettype wire
