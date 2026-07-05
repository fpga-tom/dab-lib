#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>

#include "Vcfo_corrector.h"
#include "verilated.h"

namespace {

constexpr uint32_t kSampleWidth = 16;
constexpr uint32_t kSampleMask = (1u << kSampleWidth) - 1u;
constexpr uint32_t kOutputWidth = 18;
constexpr uint32_t kCordicStages = 16;
constexpr int64_t kCordicGainScale = 39797;
constexpr uint32_t kGainFracBits = 16;
constexpr uint32_t kAtanTable[20] = {
    0x20000000u, 0x12e4051eu, 0x09fb385bu, 0x051111d4u, 0x028b0d43u,
    0x0145d7e1u, 0x00a2f61eu, 0x00517c55u, 0x0028be53u, 0x00145f2fu,
    0x000a2f98u, 0x000517ccu, 0x00028be6u, 0x000145f3u, 0x0000a2fau,
    0x0000517du, 0x000028beu, 0x0000145fu, 0x00000a30u, 0x00000518u,
};

struct Expected {
    int32_t i = 0;
    int32_t q = 0;
    uint64_t sample_index = 0;
    uint64_t symbol_start_sample = 0;
    uint32_t sample_offset = 0;
    bool symbol_done = false;
    bool error = false;
    uint32_t phase = 0;
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

int32_t sample_i(uint32_t offset, uint32_t seed) {
    const uint32_t value = ((offset * 811u) ^ (seed * 4099u) ^ 0x15555u) & kSampleMask;
    return sign_extend(value, kSampleWidth);
}

int32_t sample_q(uint32_t offset, uint32_t seed) {
    const uint32_t value = ((offset * 1237u) ^ (seed * 8191u) ^ 0x2aaaau) & kSampleMask;
    return sign_extend(value, kSampleWidth);
}

int64_t prescale(int32_t value) {
    return (static_cast<int64_t>(value) * kCordicGainScale) >> kGainFracBits;
}

int32_t saturate_output(int64_t value) {
    constexpr int64_t max_value = (1ll << (kOutputWidth - 1u)) - 1ll;
    constexpr int64_t min_value = -(1ll << (kOutputWidth - 1u));

    if (value > max_value) {
        return static_cast<int32_t>(max_value);
    }
    if (value < min_value) {
        return static_cast<int32_t>(min_value);
    }
    return static_cast<int32_t>(value);
}

Expected expected_for(uint64_t symbol_start_sample,
                      uint32_t offset,
                      uint32_t seed,
                      uint32_t phase,
                      bool bypass,
                      bool error,
                      uint32_t total_samples) {
    Expected expected;
    expected.sample_index = symbol_start_sample + offset;
    expected.symbol_start_sample = symbol_start_sample;
    expected.sample_offset = offset;
    expected.symbol_done = offset == total_samples - 1u;
    expected.error = error;
    expected.phase = phase;

    if (error) {
        expected.i = 0;
        expected.q = 0;
        return expected;
    }

    const int32_t in_i = sample_i(offset, seed);
    const int32_t in_q = sample_q(offset, seed);

    if (bypass) {
        expected.i = saturate_output(in_i);
        expected.q = saturate_output(in_q);
        return expected;
    }

    int64_t x = 0;
    int64_t y = 0;
    const int64_t scaled_i = prescale(in_i);
    const int64_t scaled_q = prescale(in_q);

    switch (phase >> 30u) {
        case 0:
            x = scaled_i;
            y = scaled_q;
            break;
        case 1:
            x = -scaled_q;
            y = scaled_i;
            break;
        case 2:
            x = -scaled_i;
            y = -scaled_q;
            break;
        default:
            x = scaled_q;
            y = -scaled_i;
            break;
    }

    int64_t z = static_cast<int64_t>(phase & 0x3fffffffu);

    for (uint32_t stage = 0; stage < kCordicStages; ++stage) {
        const int64_t x_shift = x >> stage;
        const int64_t y_shift = y >> stage;

        if (z >= 0) {
            x = x - y_shift;
            y = y + x_shift;
            z = z - static_cast<int64_t>(kAtanTable[stage]);
        } else {
            x = x + y_shift;
            y = y - x_shift;
            z = z + static_cast<int64_t>(kAtanTable[stage]);
        }
    }

    expected.i = saturate_output(x);
    expected.q = saturate_output(y);
    return expected;
}

void tick(Vcfo_corrector &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vcfo_corrector &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.cfg_bypass = 0;
    top.cfg_phase_inc = 0;
    top.cfg_phase_load = 0;
    top.cfg_phase_seed = 0;
    top.in_valid = 0;
    top.in_i = 0;
    top.in_q = 0;
    top.in_sample_index = 0;
    top.in_symbol_start_sample = 0;
    top.in_sample_offset = 0;
    top.in_symbol_done = 0;
    top.in_error = 0;
    top.out_ready = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void load_phase(Vcfo_corrector &top, uint32_t phase_seed) {
    top.in_valid = 0;
    top.out_ready = 1;
    top.cfg_phase_seed = phase_seed;
    top.cfg_phase_load = 1;
    tick(top);
    top.cfg_phase_load = 0;
    top.eval();
}

void fail_at(const char *field, uint32_t offset, int64_t expected, int64_t actual) {
    std::cerr << "Mismatch at offset " << offset << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void check_output(const Vcfo_corrector &top, const Expected &expected) {
    if (sign_extend(top.out_i, kOutputWidth) != expected.i) {
        fail_at("i", expected.sample_offset, expected.i, sign_extend(top.out_i, kOutputWidth));
    }
    if (sign_extend(top.out_q, kOutputWidth) != expected.q) {
        fail_at("q", expected.sample_offset, expected.q, sign_extend(top.out_q, kOutputWidth));
    }
    if (top.out_sample_index != expected.sample_index) {
        fail_at("sample_index", expected.sample_offset, expected.sample_index, top.out_sample_index);
    }
    if (top.out_symbol_start_sample != expected.symbol_start_sample) {
        fail_at("symbol_start_sample",
                expected.sample_offset,
                expected.symbol_start_sample,
                top.out_symbol_start_sample);
    }
    if (top.out_sample_offset != expected.sample_offset) {
        fail_at("sample_offset", expected.sample_offset, expected.sample_offset, top.out_sample_offset);
    }
    if (top.out_symbol_done != (expected.symbol_done ? 1u : 0u)) {
        fail_at("symbol_done", expected.sample_offset, expected.symbol_done ? 1u : 0u, top.out_symbol_done);
    }
    if (top.out_error != (expected.error ? 1u : 0u)) {
        fail_at("error", expected.sample_offset, expected.error ? 1u : 0u, top.out_error);
    }
    if (top.out_phase != expected.phase) {
        fail_at("phase", expected.sample_offset, expected.phase, top.out_phase);
    }
}

void run_sequence(Vcfo_corrector &top,
                  uint64_t symbol_start_sample,
                  uint32_t seed,
                  uint32_t phase_seed,
                  uint32_t phase_inc,
                  bool bypass,
                  uint32_t total_samples,
                  uint32_t error_offset) {
    std::deque<Expected> pending;
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t cycles = 0;

    load_phase(top, phase_seed);

    while (received < total_samples) {
        top.cfg_bypass = bypass ? 1 : 0;
        top.cfg_phase_inc = phase_inc;
        top.cfg_phase_load = 0;
        top.out_ready = ((cycles % 11u) != 5u) ? 1 : 0;
        top.in_valid = sent < total_samples ? 1 : 0;
        top.in_i = sent < total_samples ? pack_width(sample_i(sent, seed), kSampleWidth) : 0;
        top.in_q = sent < total_samples ? pack_width(sample_q(sent, seed), kSampleWidth) : 0;
        top.in_sample_index = symbol_start_sample + sent;
        top.in_symbol_start_sample = symbol_start_sample;
        top.in_sample_offset = sent;
        top.in_symbol_done = sent == total_samples - 1u ? 1 : 0;
        top.in_error = sent == error_offset ? 1 : 0;
        top.eval();

        const bool input_fire = top.in_valid && top.in_ready;
        const bool output_fire = top.out_valid && top.out_ready;

        if (output_fire) {
            if (pending.empty()) {
                std::cerr << "Unexpected cfo_corrector output\n";
                std::exit(1);
            }
            check_output(top, pending.front());
            pending.pop_front();
            ++received;
        }

        tick(top);

        if (input_fire) {
            const bool sample_error = sent == error_offset;
            const uint32_t sample_phase = phase_seed + (phase_inc * sent);
            pending.push_back(expected_for(symbol_start_sample,
                                           sent,
                                           seed,
                                           sample_phase,
                                           bypass,
                                           sample_error,
                                           total_samples));
            ++sent;
        }

        ++cycles;
        if (cycles > total_samples * 8u + kCordicStages * 8u) {
            std::cerr << "Timeout sent=" << sent << " received=" << received
                      << " pending=" << pending.size() << "\n";
            std::exit(1);
        }
    }

    top.out_ready = 1;
    top.in_valid = 0;
    top.eval();
    tick(top);
}

void run_clear_test(Vcfo_corrector &top) {
    load_phase(top, 0x12345678u);
    top.cfg_bypass = 0;
    top.cfg_phase_inc = 0x01000000u;
    top.out_ready = 0;
    top.in_valid = 1;
    top.in_i = pack_width(sample_i(0, 9), kSampleWidth);
    top.in_q = pack_width(sample_q(0, 9), kSampleWidth);
    top.in_sample_index = 77;
    top.in_symbol_start_sample = 77;
    top.in_sample_offset = 0;
    top.in_symbol_done = 0;
    top.in_error = 0;
    tick(top);

    top.clear = 1;
    tick(top);
    top.clear = 0;
    top.in_valid = 0;
    top.out_ready = 1;
    top.eval();

    if (top.out_valid || !top.in_ready) {
        std::cerr << "Clear did not flush cfo_corrector pipeline\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vcfo_corrector top;
    reset(top);

    run_sequence(top, 1000, 3, 0x01020304u, 0x01111111u, true, 48, 17);
    run_sequence(top, 2000, 5, 0x08000000u, 0x02222222u, false, 72, 0xffffffffu);
    run_sequence(top, 3000, 7, 0xe0000000u, 0xff300000u, false, 72, 31);
    run_clear_test(top);
    run_sequence(top, 4000, 11, 0x40000000u, 0x00400000u, false, 32, 0xffffffffu);

    std::cout << "cfo_corrector: PASS\n";
    return 0;
}
