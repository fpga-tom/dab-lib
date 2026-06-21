#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Vprs_pilot_refiner.h"
#include "verilated.h"

namespace {

constexpr uint32_t kFftSamples = 2048;
constexpr uint32_t kActiveCarriers = 1537;
constexpr uint32_t kNullCarrier = 768;
constexpr uint32_t kCarrierMask = (1u << 24u) - 1u;
constexpr uint32_t kScoreOneQ30 = 1u << 30u;
constexpr int32_t kCarrierAmplitude = 1 << 20;
constexpr int32_t kTimingFracBits = 12;
constexpr double kTwoPi = 6.28318530717958647692;

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

struct ComplexI {
    int32_t i = 0;
    int32_t q = 0;
};

uint32_t pack24(int32_t value) {
    return static_cast<uint32_t>(value) & kCarrierMask;
}

int32_t sign_extend_24(uint32_t value) {
    value &= kCarrierMask;
    if ((value & (1u << 23u)) != 0u) {
        value |= ~kCarrierMask;
    }
    return static_cast<int32_t>(value);
}

int32_t sign_extend_timing(uint32_t value) {
    constexpr uint32_t mask = (1u << 24u) - 1u;
    value &= mask;
    if ((value & (1u << 23u)) != 0u) {
        value |= ~mask;
    }
    return static_cast<int32_t>(value);
}

int32_t sign_extend_4(uint32_t value) {
    value &= 0x0fu;
    if ((value & 0x08u) != 0u) {
        value |= ~0x0fu;
    }
    return static_cast<int32_t>(value);
}

uint8_t positive_row(uint32_t group_mod) {
    switch (group_mod & 3u) {
        case 0u: return 0u;
        case 1u: return 3u;
        case 2u: return 2u;
        default: return 1u;
    }
}

uint8_t prs_phase_from_active(uint32_t active_index) {
    if (active_index < kNullCarrier) {
        return kH[(active_index / 32u) & 3u][active_index & 31u];
    }
    if (active_index > kNullCarrier) {
        const uint32_t positive_index = active_index - (kNullCarrier + 1u);
        return kH[positive_row(positive_index / 32u)][positive_index & 31u];
    }
    return 0u;
}

uint32_t active_from_fft_bin(uint32_t fft_bin) {
    if (fft_bin >= kFftSamples - kNullCarrier) {
        return fft_bin - (kFftSamples - kNullCarrier);
    }
    return kNullCarrier + fft_bin;
}

uint32_t fft_bin_from_active(uint32_t active_index) {
    if (active_index < kNullCarrier) {
        return (kFftSamples - kNullCarrier) + active_index;
    }
    if (active_index == kNullCarrier) {
        return 0u;
    }
    return active_index - kNullCarrier;
}

bool template_bin_valid(uint32_t fft_bin) {
    return fft_bin >= kFftSamples - kNullCarrier || (fft_bin >= 1u && fft_bin <= kNullCarrier);
}

ComplexI reference_for_fft_bin(uint32_t fft_bin) {
    if (!template_bin_valid(fft_bin)) {
        return {};
    }

    const uint8_t phase = prs_phase_from_active(active_from_fft_bin(fft_bin));
    switch (phase) {
        case 0u: return {kCarrierAmplitude, 0};
        case 1u: return {0, kCarrierAmplitude};
        case 2u: return {-kCarrierAmplitude, 0};
        default: return {0, -kCarrierAmplitude};
    }
}

ComplexI rotate_for_timing(const ComplexI &ref, uint32_t fft_bin, double timing_error) {
    const int32_t signed_bin = fft_bin >= (kFftSamples / 2u)
                                   ? static_cast<int32_t>(fft_bin) -
                                         static_cast<int32_t>(kFftSamples)
                                   : static_cast<int32_t>(fft_bin);
    const double phase = -kTwoPi * static_cast<double>(signed_bin) * timing_error /
                         static_cast<double>(kFftSamples);
    const double rot_re = std::cos(phase);
    const double rot_im = std::sin(phase);
    const double out_i = static_cast<double>(ref.i) * rot_re - static_cast<double>(ref.q) * rot_im;
    const double out_q = static_cast<double>(ref.i) * rot_im + static_cast<double>(ref.q) * rot_re;
    return {static_cast<int32_t>(std::llround(out_i)),
            static_cast<int32_t>(std::llround(out_q))};
}

std::array<ComplexI, kFftSamples> make_shifted_symbol(double timing_error,
                                                       int32_t carrier_offset_bins) {
    std::array<ComplexI, kFftSamples> bins{};

    for (uint32_t template_bin = 0; template_bin < kFftSamples; ++template_bin) {
        if (!template_bin_valid(template_bin)) {
            continue;
        }

        const int32_t rx_bin = static_cast<int32_t>(template_bin) + carrier_offset_bins;
        if (rx_bin < 0 || rx_bin >= static_cast<int32_t>(kFftSamples)) {
            continue;
        }

        const ComplexI ref = reference_for_fft_bin(template_bin);
        bins[static_cast<uint32_t>(rx_bin)] = rotate_for_timing(ref, template_bin, timing_error);
    }

    return bins;
}

void tick(Vprs_pilot_refiner &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vprs_pilot_refiner &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.in_i = 0;
    top.in_q = 0;
    top.in_active_index = 0;
    top.in_symbol_start_sample = 0;
    top.in_symbol_done = 0;
    top.out_ready = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail(const char *message) {
    std::cerr << message << "\n";
    std::exit(1);
}

void fail_value(const char *field, int64_t expected, int64_t actual) {
    std::cerr << field << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void drive_symbol(Vprs_pilot_refiner &top,
                  const std::array<ComplexI, kFftSamples> &bins,
                  uint64_t symbol_start_sample) {
    uint32_t sent = 0;
    uint32_t cycles = 0;

    while (sent < kActiveCarriers) {
        const uint32_t fft_bin = fft_bin_from_active(sent);

        top.out_ready = 0;
        top.in_valid = 1;
        top.in_i = pack24(bins[fft_bin].i);
        top.in_q = pack24(bins[fft_bin].q);
        top.in_active_index = sent;
        top.in_symbol_start_sample = symbol_start_sample;
        top.in_symbol_done = sent == kActiveCarriers - 1u ? 1 : 0;
        top.eval();

        if (top.in_valid && top.in_ready) {
            ++sent;
        }

        tick(top);
        if (++cycles > kActiveCarriers * 4u) {
            fail("Timed out while driving PRS pilot symbol");
        }
    }

    top.in_valid = 0;
    top.in_symbol_done = 0;
    top.eval();
}

void wait_for_output(Vprs_pilot_refiner &top) {
    for (uint32_t i = 0; i < 32u; ++i) {
        top.out_ready = 0;
        top.in_valid = 0;
        top.eval();
        if (top.out_valid) {
            return;
        }
        tick(top);
    }
    fail("Timed out waiting for refiner output");
}

void accept_output(Vprs_pilot_refiner &top) {
    top.out_ready = 1;
    top.in_valid = 0;
    top.eval();
    if (!top.out_valid) {
        fail("No valid output to accept");
    }
    tick(top);
    top.out_ready = 0;
    top.eval();
}

void run_case(Vprs_pilot_refiner &top,
              double timing_error,
              int32_t carrier_offset_bins,
              uint64_t symbol_start_sample) {
    const auto bins = make_shifted_symbol(timing_error, carrier_offset_bins);
    const int32_t expected_timing =
        static_cast<int32_t>(std::llround(timing_error * static_cast<double>(1 << kTimingFracBits)));

    drive_symbol(top, bins, symbol_start_sample);
    wait_for_output(top);

    if (!top.out_valid || !top.out_found) {
        fail("Expected a valid PRS pilot refinement result");
    }
    if (top.out_symbol_start_sample != symbol_start_sample) {
        fail_value("symbol_start_sample", symbol_start_sample, top.out_symbol_start_sample);
    }
    const int32_t actual_offset = sign_extend_4(top.out_carrier_offset_bins);
    if (actual_offset != carrier_offset_bins) {
        fail_value("carrier_offset_bins", carrier_offset_bins, actual_offset);
    }
    if (top.out_score < kScoreOneQ30 - (kScoreOneQ30 / 64u)) {
        std::cerr << "score too low: " << top.out_score << "\n";
        std::exit(1);
    }
    if (top.out_used_pairs < 1500u) {
        fail_value("used_pairs", 1500, top.out_used_pairs);
    }

    const int32_t actual_timing = sign_extend_timing(top.out_fine_timing_error_samples);
    if (std::abs(actual_timing - expected_timing) > (1 << (kTimingFracBits - 4))) {
        std::cerr << "fine_timing_error_samples expected_q12=" << expected_timing
                  << " actual_q12=" << actual_timing << " expected_samples=" << timing_error
                  << " actual_samples="
                  << static_cast<double>(actual_timing) / static_cast<double>(1 << kTimingFracBits)
                  << "\n";
        std::exit(1);
    }

    accept_output(top);
}

void run_backpressure_test(Vprs_pilot_refiner &top) {
    const auto bins = make_shifted_symbol(2.0, 1);

    drive_symbol(top, bins, 333000);
    wait_for_output(top);
    if (top.in_ready) {
        fail("Input ready should stay low while output is pending");
    }
    for (uint32_t i = 0; i < 5u; ++i) {
        top.out_ready = 0;
        top.eval();
        if (!top.out_valid) {
            fail("Output valid dropped under backpressure");
        }
        tick(top);
    }
    accept_output(top);
    if (!top.in_ready) {
        fail("Input ready did not return after accepting output");
    }
}

void run_clear_test(Vprs_pilot_refiner &top) {
    const auto bins = make_shifted_symbol(1.0, 0);

    top.in_valid = 1;
    top.in_i = pack24(bins[fft_bin_from_active(0)].i);
    top.in_q = pack24(bins[fft_bin_from_active(0)].q);
    top.in_active_index = 0;
    top.in_symbol_start_sample = 777;
    top.in_symbol_done = 0;
    top.eval();
    tick(top);

    top.clear = 1;
    top.in_valid = 0;
    top.eval();
    if (top.in_ready) {
        fail("Clear should suppress input ready");
    }
    tick(top);

    top.clear = 0;
    top.eval();
    if (!top.in_ready || top.out_valid) {
        fail("Clear did not return PRS pilot refiner to idle");
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vprs_pilot_refiner top;

    reset(top);
    run_case(top, 3.25, 0, 111000);
    run_case(top, -5.5, 2, 222000);
    run_case(top, 7.0, -3, 444000);
    run_backpressure_test(top);
    run_clear_test(top);

    std::cout << "prs_pilot_refiner: PASS\n";
    return 0;
}
