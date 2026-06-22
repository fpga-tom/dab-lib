#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Vdiff_demod.h"
#include "verilated.h"

namespace {

constexpr uint32_t kActiveCarriers = 1537;
constexpr uint32_t kNullCarrier = 768;
constexpr uint32_t kCarrierMask = (1u << 24u) - 1u;
constexpr int kCarrierWidth = 24;
constexpr int kDiffShift = kCarrierWidth - 1;

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

int32_t carrier_i(uint32_t active_index, uint32_t symbol, uint32_t seed) {
    if ((active_index % 509u) == 1u) {
        return symbol == 0u ? (1 << 22) : -(1 << 22);
    }
    const uint32_t value =
        ((active_index * 4093u) ^ (symbol * 262147u) ^ (seed * 65537u) ^ 0x00342111u) & 0x003fffffu;
    return static_cast<int32_t>(value) - static_cast<int32_t>(0x00200000u);
}

int32_t carrier_q(uint32_t active_index, uint32_t symbol, uint32_t seed) {
    if ((active_index % 509u) == 2u) {
        return symbol == 0u ? -(1 << 22) : (1 << 22);
    }
    const uint32_t value =
        ((active_index * 2281u) ^ (symbol * 131071u) ^ (seed * 32749u) ^ 0x0009abcdu) & 0x003fffffu;
    return static_cast<int32_t>(value) - static_cast<int32_t>(0x00200000u);
}

int32_t saturate_24(int64_t value) {
    constexpr int64_t max_value = (1ll << 23) - 1ll;
    constexpr int64_t min_value = -(1ll << 23);

    if (value > max_value) {
        return static_cast<int32_t>(max_value);
    }
    if (value < min_value) {
        return static_cast<int32_t>(min_value);
    }
    return static_cast<int32_t>(value);
}

int32_t scale_product(int64_t value) {
    return saturate_24(value >> kDiffShift);
}

int32_t expected_diff_i(uint32_t active_index, uint32_t curr_symbol, uint32_t seed) {
    if (active_index == kNullCarrier) {
        return 0;
    }
    const int64_t a_re = carrier_i(active_index, curr_symbol, seed);
    const int64_t a_im = carrier_q(active_index, curr_symbol, seed);
    const int64_t b_re = carrier_i(active_index, curr_symbol - 1u, seed);
    const int64_t b_im = carrier_q(active_index, curr_symbol - 1u, seed);
    return scale_product((a_re * b_re) + (a_im * b_im));
}

int32_t expected_diff_q(uint32_t active_index, uint32_t curr_symbol, uint32_t seed) {
    if (active_index == kNullCarrier) {
        return 0;
    }
    const int64_t a_re = carrier_i(active_index, curr_symbol, seed);
    const int64_t a_im = carrier_q(active_index, curr_symbol, seed);
    const int64_t b_re = carrier_i(active_index, curr_symbol - 1u, seed);
    const int64_t b_im = carrier_q(active_index, curr_symbol - 1u, seed);
    return scale_product((a_im * b_re) - (a_re * b_im));
}

void tick(Vdiff_demod &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vdiff_demod &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.out_ready = 0;
    top.in_i = 0;
    top.in_q = 0;
    top.in_active_index = 0;
    top.in_symbol_start_sample = 0;
    top.in_symbol_done = 0;
    top.in_seed_only = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail_at(const char *field, uint32_t symbol, uint32_t index, int64_t expected, int64_t actual) {
    std::cerr << "Mismatch at symbol " << symbol << " output " << index << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void check_output(const Vdiff_demod &top,
                  uint32_t symbol,
                  uint32_t active_index,
                  uint64_t symbol_start_sample,
                  uint32_t seed) {
    const int32_t expected_i = expected_diff_i(active_index, symbol, seed);
    const int32_t expected_q = expected_diff_q(active_index, symbol, seed);

    if (top.out_active_index != active_index) {
        fail_at("active_index", symbol, active_index, active_index, top.out_active_index);
    }
    if (sign_extend_24(top.out_i) != expected_i) {
        fail_at("i", symbol, active_index, expected_i, sign_extend_24(top.out_i));
    }
    if (sign_extend_24(top.out_q) != expected_q) {
        fail_at("q", symbol, active_index, expected_q, sign_extend_24(top.out_q));
    }
    if (top.out_symbol_start_sample != symbol_start_sample) {
        fail_at("symbol_start_sample",
                symbol,
                active_index,
                symbol_start_sample,
                top.out_symbol_start_sample);
    }
    if (top.out_symbol_done != (active_index == kActiveCarriers - 1u ? 1u : 0u)) {
        fail_at("symbol_done",
                symbol,
                active_index,
                active_index == kActiveCarriers - 1u ? 1u : 0u,
                top.out_symbol_done);
    }
}

void run_symbol(Vdiff_demod &top,
                uint32_t symbol,
                uint64_t symbol_start_sample,
                uint32_t seed,
                bool expect_output,
                bool seed_only = false) {
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t cycles = 0;

    while (sent < kActiveCarriers || (expect_output && received < kActiveCarriers)) {
        top.out_ready = ((cycles % 11u) != 7u) ? 1 : 0;
        top.in_valid = sent < kActiveCarriers ? 1 : 0;
        top.in_i = sent < kActiveCarriers ? pack24(carrier_i(sent, symbol, seed)) : 0;
        top.in_q = sent < kActiveCarriers ? pack24(carrier_q(sent, symbol, seed)) : 0;
        top.in_active_index = sent;
        top.in_symbol_start_sample = symbol_start_sample;
        top.in_symbol_done = sent == kActiveCarriers - 1u ? 1 : 0;
        top.in_seed_only = seed_only ? 1 : 0;
        top.eval();

        const bool output_fire = top.out_valid && top.out_ready;
        const bool input_fire = top.in_valid && top.in_ready;

        if (output_fire) {
            if (!expect_output) {
                std::cerr << "Unexpected output while loading seed-only/first symbol\n";
                std::exit(1);
            }
            check_output(top, symbol, received, symbol_start_sample, seed);
            ++received;
        }

        tick(top);

        if (input_fire) {
            ++sent;
        }

        ++cycles;
        if (cycles > kActiveCarriers * 4u) {
            std::cerr << "Timeout symbol=" << symbol << " sent=" << sent
                      << " received=" << received << "\n";
            std::exit(1);
        }
    }

    top.out_ready = 1;
    top.in_valid = 0;
    top.in_seed_only = 0;
    tick(top);
    top.eval();
    if (top.out_valid) {
        std::cerr << "Output valid remained asserted after symbol drain\n";
        std::exit(1);
    }
}

void run_clear_test(Vdiff_demod &top) {
    top.out_ready = 1;
    top.in_valid = 1;
    top.in_i = pack24(carrier_i(0, 9, 4));
    top.in_q = pack24(carrier_q(0, 9, 4));
    top.in_active_index = 0;
    top.in_symbol_start_sample = 55;
    top.in_symbol_done = 0;
    top.in_seed_only = 0;
    top.eval();
    tick(top);

    top.clear = 1;
    top.in_valid = 0;
    tick(top);
    top.clear = 0;
    top.eval();
    if (top.out_valid || !top.in_ready) {
        std::cerr << "Clear did not return diff_demod to idle state\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vdiff_demod top;

    reset(top);
    run_symbol(top, 0, 100000ull, 5, false);
    run_symbol(top, 1, 102552ull, 5, true);
    run_symbol(top, 2, 105104ull, 5, true);
    run_symbol(top, 3, 107656ull, 5, false, true);
    run_symbol(top, 4, 110208ull, 5, true);
    run_clear_test(top);
    run_symbol(top, 0, 200000ull, 17, false);
    run_symbol(top, 1, 202552ull, 17, true);

    std::cout << "diff_demod: PASS\n";
    return 0;
}
