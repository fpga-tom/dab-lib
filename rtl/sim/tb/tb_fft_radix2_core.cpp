#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "Vfft_radix2_core.h"
#include "verilated.h"

namespace {

constexpr uint32_t kFftBins = 2048;
constexpr uint32_t kInputWidth = 18;
constexpr uint32_t kOutputWidth = 24;
constexpr uint32_t kInputMask = (1u << kInputWidth) - 1u;
constexpr uint32_t kOutputMask = (1u << kOutputWidth) - 1u;
constexpr double kPi = 3.141592653589793238462643383279502884;

struct Sample {
    int32_t i;
    int32_t q;
};

int32_t sign_extend(uint32_t value, uint32_t width) {
    const uint32_t mask = (1u << width) - 1u;
    value &= mask;
    if ((value & (1u << (width - 1u))) != 0u) {
        value |= ~mask;
    }
    return static_cast<int32_t>(value);
}

uint32_t pack_width(int32_t value, uint32_t width) {
    return static_cast<uint32_t>(value) & ((1u << width) - 1u);
}

void tick(Vfft_radix2_core &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vfft_radix2_core &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.core_in_valid = 0;
    top.core_in_i = 0;
    top.core_in_q = 0;
    top.core_in_last = 0;
    top.core_out_ready = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail(const std::string &message) {
    std::cerr << message << "\n";
    std::exit(1);
}

std::vector<Sample> run_fft(Vfft_radix2_core &top, const std::vector<Sample> &input) {
    if (input.size() != kFftBins) {
        fail("test input size must be 2048");
    }

    std::vector<Sample> output;
    output.reserve(kFftBins);

    uint32_t sent = 0;
    uint32_t cycles = 0;
    while (output.size() < kFftBins) {
        top.core_in_valid = sent < kFftBins ? 1 : 0;
        top.core_in_i = sent < kFftBins ? pack_width(input[sent].i, kInputWidth) : 0;
        top.core_in_q = sent < kFftBins ? pack_width(input[sent].q, kInputWidth) : 0;
        top.core_in_last = sent == kFftBins - 1u ? 1 : 0;
        top.core_out_ready = 1;
        top.eval();

        const bool input_fire = top.core_in_valid && top.core_in_ready;
        const bool output_fire = top.core_out_valid && top.core_out_ready;

        if (output_fire) {
            output.push_back({
                sign_extend(top.core_out_i, kOutputWidth),
                sign_extend(top.core_out_q, kOutputWidth),
            });

            if (top.core_out_block_exp != 11u) {
                fail("unexpected FFT block exponent");
            }
            if (top.core_out_overflow != 0u) {
                fail("unexpected FFT overflow flag");
            }
        }

        tick(top);

        if (input_fire) {
            ++sent;
        }

        ++cycles;
        if (cycles > 80000u) {
            fail("FFT core timeout");
        }
    }

    if (sent != kFftBins) {
        fail("FFT core did not consume the full input frame");
    }

    return output;
}

void expect_close(const std::vector<Sample> &actual,
                  const std::vector<Sample> &expected,
                  int32_t tolerance,
                  const std::string &name) {
    int64_t worst_error = 0;
    uint32_t worst_bin = 0;
    for (uint32_t bin = 0; bin < kFftBins; ++bin) {
        const int64_t err_i = std::llabs(static_cast<int64_t>(actual[bin].i) - expected[bin].i);
        const int64_t err_q = std::llabs(static_cast<int64_t>(actual[bin].q) - expected[bin].q);
        if (err_i > worst_error) {
            worst_error = err_i;
            worst_bin = bin;
        }
        if (err_q > worst_error) {
            worst_error = err_q;
            worst_bin = bin;
        }
        if (err_i > tolerance || err_q > tolerance) {
            std::cerr << name << " mismatch bin=" << bin
                      << " expected=(" << expected[bin].i << "," << expected[bin].q << ")"
                      << " actual=(" << actual[bin].i << "," << actual[bin].q << ")"
                      << " tolerance=" << tolerance << "\n";
            std::exit(1);
        }
    }

    std::cout << name << " worst_error=" << worst_error << " bin=" << worst_bin << "\n";
}

std::vector<Sample> dft_scaled_reference(const std::vector<Sample> &input) {
    std::vector<Sample> reference(kFftBins);
    for (uint32_t bin = 0; bin < kFftBins; ++bin) {
        std::complex<double> acc(0.0, 0.0);
        for (uint32_t n = 0; n < kFftBins; ++n) {
            const double angle = -2.0 * kPi * static_cast<double>(bin) *
                                 static_cast<double>(n) / static_cast<double>(kFftBins);
            const std::complex<double> sample(static_cast<double>(input[n].i),
                                              static_cast<double>(input[n].q));
            acc += sample * std::complex<double>(std::cos(angle), std::sin(angle));
        }
        acc /= static_cast<double>(kFftBins);
        reference[bin] = {
            static_cast<int32_t>(std::llround(acc.real())),
            static_cast<int32_t>(std::llround(acc.imag())),
        };
    }
    return reference;
}

std::vector<Sample> impulse_input(int32_t amplitude) {
    std::vector<Sample> input(kFftBins, {0, 0});
    input[0].i = amplitude;
    return input;
}

std::vector<Sample> dc_input(int32_t amplitude) {
    return std::vector<Sample>(kFftBins, {amplitude, 0});
}

std::vector<Sample> nyquist_input(int32_t amplitude) {
    std::vector<Sample> input(kFftBins);
    for (uint32_t n = 0; n < kFftBins; ++n) {
        input[n].i = (n & 1u) == 0u ? amplitude : -amplitude;
        input[n].q = 0;
    }
    return input;
}

std::vector<Sample> tone_input(uint32_t bin, int32_t amplitude) {
    std::vector<Sample> input(kFftBins);
    for (uint32_t n = 0; n < kFftBins; ++n) {
        const double angle = 2.0 * kPi * static_cast<double>(bin) *
                             static_cast<double>(n) / static_cast<double>(kFftBins);
        input[n].i = static_cast<int32_t>(std::llround(amplitude * std::cos(angle)));
        input[n].q = static_cast<int32_t>(std::llround(amplitude * std::sin(angle)));
    }
    return input;
}

void run_case(Vfft_radix2_core &top,
              const std::string &name,
              const std::vector<Sample> &input,
              int32_t tolerance) {
    const auto actual = run_fft(top, input);
    const auto expected = dft_scaled_reference(input);
    expect_close(actual, expected, tolerance, name);
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Vfft_radix2_core top;
    reset(top);

    run_case(top, "impulse", impulse_input(65536), 0);
    run_case(top, "dc", dc_input(1024), 0);
    run_case(top, "nyquist", nyquist_input(2048), 0);
    run_case(top, "tone5", tone_input(5, 32768), 900);

    std::cout << "fft_radix2_core: PASS\n";
    return 0;
}
