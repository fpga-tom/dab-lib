#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Vdab_pipeline_top.h"
#include "verilated.h"

namespace {

uint32_t score_q(double value) {
    constexpr uint32_t kScoreFracBits = 30;
    if (value <= 0.0) {
        return 0;
    }
    const double scaled = value * static_cast<double>(uint64_t{1} << kScoreFracBits);
    if (scaled >= static_cast<double>(0xffffffffu)) {
        return 0xffffffffu;
    }
    return static_cast<uint32_t>(scaled);
}

void tick(Vdab_pipeline_top &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void drive_defaults(Vdab_pipeline_top &top) {
    top.set_sample_index_valid = 0;
    top.set_sample_index_value = 0;
    top.sdr_in_valid = 0;
    top.sdr_in_i = 0;
    top.sdr_in_q = 0;
    top.cfg_enable_tracking = 1;
    top.cfg_min_cp_score = score_q(0.40);
    top.cfg_min_pilot_score = score_q(0.50);
    top.cfg_cfo_bypass = 1;
    top.cfg_cfo_phase_inc = 0;
    top.cfg_cfo_phase_load = 0;
    top.cfg_cfo_phase_seed = 0;
    top.predict_in_valid = 0;
    top.predict_in_prs_fft_start_sample = 0;
    top.data_symbol_cmd_valid = 0;
    top.data_symbol_cmd_fft_start_sample = 0;
    top.fft_core_in_ready = 1;
    top.fft_core_out_valid = 0;
    top.fft_core_out_i = 0;
    top.fft_core_out_q = 0;
    top.fft_core_out_block_exp = 0;
    top.fft_core_out_overflow = 0;
    top.sync_out_ready = 1;
    top.tframe_out_ready = 1;
}

void reset(Vdab_pipeline_top &top) {
    top.rst_n = 0;
    top.clear = 0;
    drive_defaults(top);
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vdab_pipeline_top top;
    reset(top);

    if (!top.sdr_in_ready) {
        std::cerr << "dab_pipeline_top did not accept SDR samples after reset\n";
        return 1;
    }
    if (top.symbol_busy || top.sync_out_valid || top.tframe_out_valid) {
        std::cerr << "dab_pipeline_top produced unexpected output after reset\n";
        return 1;
    }

    top.set_sample_index_valid = 1;
    top.set_sample_index_value = 1000;
    tick(top);

    top.set_sample_index_valid = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        drive_defaults(top);
        top.sdr_in_valid = 1;
        top.sdr_in_i = static_cast<uint32_t>((i * 17u) & 0xffffu);
        top.sdr_in_q = static_cast<uint32_t>((i * 29u) & 0xffffu);
        tick(top);
    }

    if (!top.history_valid || top.history_stored_samples == 0) {
        std::cerr << "dab_pipeline_top did not write ingress samples into history\n";
        return 1;
    }

    std::cout << "dab_pipeline_top: PASS\n";
    return 0;
}
