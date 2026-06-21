#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Vprs_channel_estimator.h"
#include "verilated.h"

namespace {

constexpr uint32_t kActiveCarriers = 1537;
constexpr uint32_t kNullCarrier = 768;
constexpr uint32_t kCarrierMask = (1u << 24u) - 1u;
constexpr int32_t kRefScale = 1 << 22;
constexpr int kCoeffFracBits = 22;

const uint8_t kH[4][32] = {
    {0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1,
     0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1},
    {0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0,
     0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0},
    {0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3,
     0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3},
    {0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2,
     0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2},
};

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

uint8_t positive_row(uint32_t group_mod) {
    switch (group_mod & 3u) {
        case 0u: return 0u;
        case 1u: return 3u;
        case 2u: return 2u;
        default: return 1u;
    }
}

uint8_t prs_phase(uint32_t active_index) {
    if (active_index < kNullCarrier) {
        return kH[(active_index / 32u) & 3u][active_index & 31u];
    }
    if (active_index > kNullCarrier) {
        const uint32_t positive_index = active_index - (kNullCarrier + 1u);
        return kH[positive_row(positive_index / 32u)][positive_index & 31u];
    }
    return 0u;
}

int32_t ref_i(uint32_t active_index) {
    if (active_index == kNullCarrier) {
        return 0;
    }
    switch (prs_phase(active_index)) {
        case 0u: return kRefScale;
        case 2u: return -kRefScale;
        default: return 0;
    }
}

int32_t ref_q(uint32_t active_index) {
    if (active_index == kNullCarrier) {
        return 0;
    }
    switch (prs_phase(active_index)) {
        case 1u: return kRefScale;
        case 3u: return -kRefScale;
        default: return 0;
    }
}

int32_t saturate_24(__int128 value) {
    constexpr __int128 max_value = (__int128{1} << 23) - 1;
    constexpr __int128 min_value = -(__int128{1} << 23);

    if (value > max_value) {
        return static_cast<int32_t>(max_value);
    }
    if (value < min_value) {
        return static_cast<int32_t>(min_value);
    }
    return static_cast<int32_t>(value);
}

int32_t rx_i(uint32_t active_index, uint32_t seed) {
    if (active_index == 0u) {
        return 0;
    }
    if (active_index == 1u) {
        return 1;
    }
    const uint32_t value =
        ((active_index * 8191u) ^ (seed * 65537u) ^ 0x0013579bu) & 0x003fffffu;
    return static_cast<int32_t>(value) - static_cast<int32_t>(0x00200000u);
}

int32_t rx_q(uint32_t active_index, uint32_t seed) {
    if (active_index == 0u || active_index == 1u) {
        return 0;
    }
    const uint32_t value =
        ((active_index * 4099u) ^ (seed * 32749u) ^ 0x002468acu) & 0x003fffffu;
    return static_cast<int32_t>(value) - static_cast<int32_t>(0x00200000u);
}

int32_t expected_coeff_i(uint32_t active_index, uint32_t seed) {
    if (active_index == kNullCarrier) {
        return 0;
    }

    const int64_t sym_re = rx_i(active_index, seed);
    const int64_t sym_im = rx_q(active_index, seed);
    const int64_t denom = (sym_re * sym_re) + (sym_im * sym_im);
    if (denom <= 0) {
        return 0;
    }

    const int64_t re = ref_i(active_index);
    const int64_t im = ref_q(active_index);
    const __int128 numerator = (__int128{re} * sym_re) + (__int128{im} * sym_im);
    return saturate_24((numerator << kCoeffFracBits) / denom);
}

int32_t expected_coeff_q(uint32_t active_index, uint32_t seed) {
    if (active_index == kNullCarrier) {
        return 0;
    }

    const int64_t sym_re = rx_i(active_index, seed);
    const int64_t sym_im = rx_q(active_index, seed);
    const int64_t denom = (sym_re * sym_re) + (sym_im * sym_im);
    if (denom <= 0) {
        return 0;
    }

    const int64_t re = ref_i(active_index);
    const int64_t im = ref_q(active_index);
    const __int128 numerator = (__int128{im} * sym_re) - (__int128{re} * sym_im);
    return saturate_24((numerator << kCoeffFracBits) / denom);
}

void tick(Vprs_channel_estimator &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vprs_channel_estimator &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.in_i = 0;
    top.in_q = 0;
    top.in_active_index = 0;
    top.in_symbol_start_sample = 0;
    top.in_symbol_done = 0;
    top.coeff_ready = 0;
    top.ref_ready = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail_at(const char *field, uint32_t index, int64_t expected, int64_t actual) {
    std::cerr << "Mismatch at index " << index << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void check_coeff_output(const Vprs_channel_estimator &top,
                        uint32_t active_index,
                        uint64_t symbol_start_sample,
                        uint32_t seed) {
    if (top.coeff_index != active_index) {
        fail_at("coeff_index", active_index, active_index, top.coeff_index);
    }
    if (sign_extend_24(top.coeff_i) != expected_coeff_i(active_index, seed)) {
        fail_at("coeff_i",
                active_index,
                expected_coeff_i(active_index, seed),
                sign_extend_24(top.coeff_i));
    }
    if (sign_extend_24(top.coeff_q) != expected_coeff_q(active_index, seed)) {
        fail_at("coeff_q",
                active_index,
                expected_coeff_q(active_index, seed),
                sign_extend_24(top.coeff_q));
    }
    if (top.coeff_symbol_start_sample != symbol_start_sample) {
        fail_at("coeff_symbol_start_sample",
                active_index,
                symbol_start_sample,
                top.coeff_symbol_start_sample);
    }
    if (top.coeff_symbol_done != (active_index == kActiveCarriers - 1u ? 1u : 0u)) {
        fail_at("coeff_symbol_done",
                active_index,
                active_index == kActiveCarriers - 1u ? 1u : 0u,
                top.coeff_symbol_done);
    }
}

void check_ref_output(const Vprs_channel_estimator &top,
                      uint32_t active_index,
                      uint64_t symbol_start_sample) {
    if (top.ref_active_index != active_index) {
        fail_at("ref_active_index", active_index, active_index, top.ref_active_index);
    }
    if (sign_extend_24(top.ref_i) != ref_i(active_index)) {
        fail_at("ref_i", active_index, ref_i(active_index), sign_extend_24(top.ref_i));
    }
    if (sign_extend_24(top.ref_q) != ref_q(active_index)) {
        fail_at("ref_q", active_index, ref_q(active_index), sign_extend_24(top.ref_q));
    }
    if (top.ref_symbol_start_sample != symbol_start_sample) {
        fail_at("ref_symbol_start_sample",
                active_index,
                symbol_start_sample,
                top.ref_symbol_start_sample);
    }
    if (top.ref_symbol_done != (active_index == kActiveCarriers - 1u ? 1u : 0u)) {
        fail_at("ref_symbol_done",
                active_index,
                active_index == kActiveCarriers - 1u ? 1u : 0u,
                top.ref_symbol_done);
    }
}

void run_symbol(Vprs_channel_estimator &top, uint64_t symbol_start_sample, uint32_t seed) {
    uint32_t sent = 0;
    uint32_t coeff_received = 0;
    uint32_t ref_received = 0;
    uint32_t cycles = 0;

    while (coeff_received < kActiveCarriers || ref_received < kActiveCarriers) {
        top.coeff_ready = ((cycles % 11u) != 4u) ? 1 : 0;
        top.ref_ready = ((cycles % 13u) != 7u) ? 1 : 0;
        top.in_valid = sent < kActiveCarriers ? 1 : 0;
        top.in_i = sent < kActiveCarriers ? pack24(rx_i(sent, seed)) : 0;
        top.in_q = sent < kActiveCarriers ? pack24(rx_q(sent, seed)) : 0;
        top.in_active_index = sent;
        top.in_symbol_start_sample = symbol_start_sample;
        top.in_symbol_done = sent == kActiveCarriers - 1u ? 1 : 0;
        top.eval();

        const bool input_fire = top.in_valid && top.in_ready;
        const bool coeff_fire = top.coeff_valid && top.coeff_ready;
        const bool ref_fire = top.ref_valid && top.ref_ready;

        if (coeff_fire) {
            check_coeff_output(top, coeff_received, symbol_start_sample, seed);
            ++coeff_received;
        }
        if (ref_fire) {
            check_ref_output(top, ref_received, symbol_start_sample);
            ++ref_received;
        }

        tick(top);

        if (input_fire) {
            ++sent;
        }

        ++cycles;
        if (cycles > kActiveCarriers * 8u) {
            std::cerr << "Timeout sent=" << sent << " coeff_received=" << coeff_received
                      << " ref_received=" << ref_received << "\n";
            std::exit(1);
        }
    }

    top.coeff_ready = 1;
    top.ref_ready = 1;
    top.in_valid = 0;
    tick(top);
    top.eval();
    if (top.coeff_valid || top.ref_valid || !top.in_ready) {
        std::cerr << "Estimator did not return to idle after symbol drain\n";
        std::exit(1);
    }
}

void run_clear_test(Vprs_channel_estimator &top) {
    top.coeff_ready = 0;
    top.ref_ready = 0;
    top.in_valid = 1;
    top.in_i = pack24(rx_i(2, 99));
    top.in_q = pack24(rx_q(2, 99));
    top.in_active_index = 2;
    top.in_symbol_start_sample = 333;
    top.in_symbol_done = 0;
    top.eval();
    tick(top);

    if (!top.coeff_valid || !top.ref_valid || top.in_ready) {
        std::cerr << "Clear setup did not create pending outputs\n";
        std::exit(1);
    }

    top.clear = 1;
    top.in_valid = 0;
    top.eval();
    if (top.in_ready) {
        std::cerr << "Clear did not suppress input ready\n";
        std::exit(1);
    }
    tick(top);

    top.clear = 0;
    top.eval();
    if (top.coeff_valid || top.ref_valid || !top.in_ready) {
        std::cerr << "Clear did not return prs_channel_estimator to idle\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vprs_channel_estimator top;

    reset(top);
    run_symbol(top, 444000ull, 7);
    run_clear_test(top);
    run_symbol(top, 555000ull, 19);

    std::cout << "prs_channel_estimator: PASS\n";
    return 0;
}
