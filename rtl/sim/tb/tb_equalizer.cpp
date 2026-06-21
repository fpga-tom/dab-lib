#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Vequalizer.h"
#include "verilated.h"

namespace {

constexpr uint32_t kActiveCarriers = 1537;
constexpr uint32_t kNullCarrier = 768;
constexpr uint32_t kCarrierMask = (1u << 24u) - 1u;
constexpr int kCarrierWidth = 24;
constexpr int kCoeffFracBits = 22;

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

int32_t carrier_i(uint32_t active_index, uint32_t seed) {
    if ((active_index % 503u) == 1u) {
        return (1 << 22);
    }
    const uint32_t value =
        ((active_index * 4099u) ^ (seed * 16381u) ^ 0x0013579bu) & 0x003fffffu;
    return static_cast<int32_t>(value) - static_cast<int32_t>(0x00200000u);
}

int32_t carrier_q(uint32_t active_index, uint32_t seed) {
    if ((active_index % 503u) == 2u) {
        return -(1 << 22);
    }
    const uint32_t value =
        ((active_index * 2027u) ^ (seed * 32749u) ^ 0x002468acu) & 0x003fffffu;
    return static_cast<int32_t>(value) - static_cast<int32_t>(0x00200000u);
}

int32_t coeff_i(uint32_t active_index, uint32_t seed) {
    if ((active_index % 337u) == 0u) {
        return 1 << kCoeffFracBits;
    }
    const int32_t frac = static_cast<int32_t>(((active_index * 37u) ^ (seed * 251u)) & 0x0007ffffu);
    return (1 << kCoeffFracBits) - (1 << 19) + frac;
}

int32_t coeff_q(uint32_t active_index, uint32_t seed) {
    if ((active_index % 337u) == 0u) {
        return 0;
    }
    const int32_t frac = static_cast<int32_t>(((active_index * 67u) ^ (seed * 109u)) & 0x0003ffffu);
    return frac - (1 << 17);
}

int32_t expected_i(uint32_t active_index, uint32_t seed) {
    if (active_index == kNullCarrier) {
        return 0;
    }
    const int64_t a_re = carrier_i(active_index, seed);
    const int64_t a_im = carrier_q(active_index, seed);
    const int64_t c_re = coeff_i(active_index, seed);
    const int64_t c_im = coeff_q(active_index, seed);
    return saturate_24(((a_re * c_re) - (a_im * c_im)) >> kCoeffFracBits);
}

int32_t expected_q(uint32_t active_index, uint32_t seed) {
    if (active_index == kNullCarrier) {
        return 0;
    }
    const int64_t a_re = carrier_i(active_index, seed);
    const int64_t a_im = carrier_q(active_index, seed);
    const int64_t c_re = coeff_i(active_index, seed);
    const int64_t c_im = coeff_q(active_index, seed);
    return saturate_24(((a_re * c_im) + (a_im * c_re)) >> kCoeffFracBits);
}

void tick(Vequalizer &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vequalizer &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.coeff_we = 0;
    top.coeff_index = 0;
    top.coeff_i = 0;
    top.coeff_q = 0;
    top.in_valid = 0;
    top.out_ready = 0;
    top.in_i = 0;
    top.in_q = 0;
    top.in_active_index = 0;
    top.in_symbol_start_sample = 0;
    top.in_symbol_done = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void write_coefficients(Vequalizer &top, uint32_t seed) {
    top.out_ready = 1;
    top.in_valid = 0;
    for (uint32_t i = 0; i < kActiveCarriers; ++i) {
        top.coeff_we = 1;
        top.coeff_index = i;
        top.coeff_i = pack24(coeff_i(i, seed));
        top.coeff_q = pack24(coeff_q(i, seed));
        tick(top);
    }
    top.coeff_we = 0;
    top.coeff_index = 0;
    top.coeff_i = 0;
    top.coeff_q = 0;
    tick(top);
}

void fail_at(const char *field, uint32_t index, int64_t expected, int64_t actual) {
    std::cerr << "Mismatch at output " << index << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void check_output(const Vequalizer &top,
                  uint32_t active_index,
                  uint64_t symbol_start_sample,
                  uint32_t seed) {
    if (top.out_active_index != active_index) {
        fail_at("active_index", active_index, active_index, top.out_active_index);
    }
    if (sign_extend_24(top.out_i) != expected_i(active_index, seed)) {
        fail_at("i", active_index, expected_i(active_index, seed), sign_extend_24(top.out_i));
    }
    if (sign_extend_24(top.out_q) != expected_q(active_index, seed)) {
        fail_at("q", active_index, expected_q(active_index, seed), sign_extend_24(top.out_q));
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

void run_symbol(Vequalizer &top, uint64_t symbol_start_sample, uint32_t seed) {
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t cycles = 0;

    while (received < kActiveCarriers) {
        top.out_ready = ((cycles % 13u) != 8u) ? 1 : 0;
        top.in_valid = sent < kActiveCarriers ? 1 : 0;
        top.in_i = sent < kActiveCarriers ? pack24(carrier_i(sent, seed)) : 0;
        top.in_q = sent < kActiveCarriers ? pack24(carrier_q(sent, seed)) : 0;
        top.in_active_index = sent;
        top.in_symbol_start_sample = symbol_start_sample;
        top.in_symbol_done = sent == kActiveCarriers - 1u ? 1 : 0;
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
        if (cycles > kActiveCarriers * 4u) {
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

void run_clear_test(Vequalizer &top) {
    top.out_ready = 1;
    top.in_valid = 1;
    top.in_i = pack24(carrier_i(0, 9));
    top.in_q = pack24(carrier_q(0, 9));
    top.in_active_index = 0;
    top.in_symbol_start_sample = 55;
    top.in_symbol_done = 0;
    top.eval();
    tick(top);

    top.clear = 1;
    top.in_valid = 0;
    tick(top);
    top.clear = 0;
    top.eval();
    if (top.out_valid || !top.in_ready) {
        std::cerr << "Clear did not return equalizer to idle state\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vequalizer top;

    reset(top);
    write_coefficients(top, 7);
    run_symbol(top, 555000ull, 7);
    run_clear_test(top);
    write_coefficients(top, 19);
    run_symbol(top, 777000ull, 19);

    std::cout << "equalizer: PASS\n";
    return 0;
}
