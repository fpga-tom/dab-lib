#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Vcarrier_mapper.h"
#include "verilated.h"

namespace {

constexpr uint32_t kFftBins = 2048;
constexpr uint32_t kActiveCarriers = 1537;
constexpr uint32_t kActiveHalf = 768;
constexpr uint32_t kHighFirstBin = kFftBins - kActiveHalf;
constexpr uint32_t kCarrierMask = (1u << 24u) - 1u;

int32_t sign_extend_24(uint32_t value) {
    value &= kCarrierMask;
    if ((value & (1u << 23u)) != 0u) {
        value |= ~kCarrierMask;
    }
    return static_cast<int32_t>(value);
}

uint32_t pack24(int32_t value) {
    return static_cast<uint32_t>(value) & kCarrierMask;
}

int32_t pattern_i(uint32_t fft_bin, uint32_t seed) {
    const uint32_t value = ((fft_bin * 104729u) ^ (seed * 8191u) ^ 0x00123456u) & kCarrierMask;
    return sign_extend_24(value);
}

int32_t pattern_q(uint32_t fft_bin, uint32_t seed) {
    const uint32_t value = ((fft_bin * 65537u) ^ (seed * 32749u) ^ 0x00fedcbau) & kCarrierMask;
    return sign_extend_24(value);
}

uint32_t source_bin_for_active(uint32_t active_index) {
    if (active_index < kActiveHalf) {
        return kHighFirstBin + active_index;
    }
    return active_index - kActiveHalf;
}

void tick(Vcarrier_mapper &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vcarrier_mapper &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.out_ready = 0;
    top.in_i = 0;
    top.in_q = 0;
    top.in_symbol_start_sample = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail_at(const char *field, uint32_t index, int64_t expected, int64_t actual) {
    std::cerr << "Mismatch at output " << index << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void check_output(const Vcarrier_mapper &top,
                  uint32_t active_index,
                  uint64_t symbol_start_sample,
                  uint32_t seed) {
    const uint32_t source_bin = source_bin_for_active(active_index);

    if (top.out_active_index != active_index) {
        fail_at("active_index", active_index, active_index, top.out_active_index);
    }
    if (top.out_fft_bin_index != source_bin) {
        fail_at("fft_bin_index", active_index, source_bin, top.out_fft_bin_index);
    }
    if (sign_extend_24(top.out_i) != pattern_i(source_bin, seed)) {
        fail_at("i", active_index, pattern_i(source_bin, seed), sign_extend_24(top.out_i));
    }
    if (sign_extend_24(top.out_q) != pattern_q(source_bin, seed)) {
        fail_at("q", active_index, pattern_q(source_bin, seed), sign_extend_24(top.out_q));
    }
    if (top.out_symbol_start_sample != symbol_start_sample) {
        fail_at("symbol_start_sample", active_index, symbol_start_sample, top.out_symbol_start_sample);
    }
    if (top.out_symbol_done != (active_index == kActiveCarriers - 1u ? 1u : 0u)) {
        fail_at("symbol_done",
                active_index,
                active_index == kActiveCarriers - 1u ? 1u : 0u,
                top.out_symbol_done);
    }
}

void run_symbol(Vcarrier_mapper &top, uint64_t symbol_start_sample, uint32_t seed) {
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t cycles = 0;

    while (received < kActiveCarriers) {
        top.out_ready = ((cycles % 13u) != 6u) ? 1 : 0;
        top.in_valid = sent < kFftBins ? 1 : 0;
        top.in_i = sent < kFftBins ? pack24(pattern_i(sent, seed)) : 0;
        top.in_q = sent < kFftBins ? pack24(pattern_q(sent, seed)) : 0;
        top.in_symbol_start_sample = sent < kFftBins ? symbol_start_sample : 0;
        top.eval();

        const bool output_fire = top.out_valid && top.out_ready;
        const bool input_fire = top.in_valid && top.in_ready;

        if (output_fire) {
            check_output(top, received, symbol_start_sample, seed);
            ++received;
        }

        tick(top);

        if (input_fire) {
            ++sent;
        }

        ++cycles;
        if (cycles > (kFftBins + kActiveCarriers) * 4u) {
            std::cerr << "Timeout sent=" << sent << " received=" << received << "\n";
            std::exit(1);
        }
    }

    top.out_ready = 1;
    top.in_valid = 0;
    tick(top);
    top.eval();
    if (top.out_valid) {
        std::cerr << "Output valid remained asserted after symbol drain\n";
        std::exit(1);
    }
}

void run_clear_test(Vcarrier_mapper &top) {
    top.out_ready = 1;
    top.in_valid = 1;
    top.in_i = pack24(pattern_i(0, 99));
    top.in_q = pack24(pattern_q(0, 99));
    top.in_symbol_start_sample = 44;
    top.eval();
    tick(top);

    top.clear = 1;
    top.in_valid = 0;
    tick(top);
    top.clear = 0;
    top.eval();
    if (top.out_valid || !top.in_ready) {
        std::cerr << "Clear did not return carrier_mapper to load state\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vcarrier_mapper top;

    reset(top);
    run_symbol(top, 111111ull, 3);
    run_clear_test(top);
    run_symbol(top, 222222ull, 17);

    std::cout << "carrier_mapper: PASS\n";
    return 0;
}
