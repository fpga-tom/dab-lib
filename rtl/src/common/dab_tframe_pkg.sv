// Shared constants for DAB Mode I transmission-frame softbit handling.
package dab_tframe_pkg;
  localparam int DAB_TFRAME_FIC_SYMBOLS = 3;
  localparam int DAB_TFRAME_MSC_SYMBOLS = 72;
  localparam int DAB_TFRAME_TOTAL_SYMBOLS =
      DAB_TFRAME_FIC_SYMBOLS + DAB_TFRAME_MSC_SYMBOLS;

  localparam int DAB_TFRAME_SOFTBITS_PER_HALF = 1536;
  localparam int DAB_TFRAME_SOFTBITS_PER_SYMBOL =
      2 * DAB_TFRAME_SOFTBITS_PER_HALF;
  /* verilator lint_off UNUSEDPARAM */
  localparam int DAB_TFRAME_FIC_SOFTBITS =
      DAB_TFRAME_FIC_SYMBOLS * DAB_TFRAME_SOFTBITS_PER_SYMBOL;
  localparam int DAB_TFRAME_MSC_SOFTBITS =
      DAB_TFRAME_MSC_SYMBOLS * DAB_TFRAME_SOFTBITS_PER_SYMBOL;
  /* verilator lint_on UNUSEDPARAM */

  localparam int DAB_TFRAME_SOFTBIT_WIDTH = 8;
  localparam int DAB_TFRAME_SYMBOL_INDEX_WIDTH = 7;
  localparam int DAB_TFRAME_BYTE_INDEX_WIDTH = 12;
  localparam int DAB_TFRAME_ADDR_WIDTH = 18;

  localparam logic DAB_TFRAME_REGION_FIC = 1'b0;
  localparam logic DAB_TFRAME_REGION_MSC = 1'b1;
endpackage
