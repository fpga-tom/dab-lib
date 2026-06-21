#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Vfrequency_deinterleaver.h"
#include "verilated.h"

namespace {

constexpr uint32_t kActiveCarriers = 1537;
constexpr uint32_t kDataCarriers = 1536;
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

int32_t pattern_i(uint32_t active_index, uint32_t seed) {
    const uint32_t value = ((active_index * 8191u) ^ (seed * 123457u) ^ 0x0055aa33u) & kCarrierMask;
    return sign_extend_24(value);
}

int32_t pattern_q(uint32_t active_index, uint32_t seed) {
    const uint32_t value = ((active_index * 3571u) ^ (seed * 76543u) ^ 0x00aa55ccu) & kCarrierMask;
    return sign_extend_24(value);
}

std::array<uint32_t, kDataCarriers> build_map() {
    std::array<uint32_t, kDataCarriers> map{};
    uint32_t p = 0;
    uint32_t k = 0;

    for (uint32_t i = 0; i < 2048; ++i) {
        const uint32_t a = p;
        p = (13u * p + 511u) % 2048u;
        if (a >= 256u && a <= 1792u && a != 1024u) {
            if (k >= kDataCarriers) {
                std::cerr << "Map overflow\n";
                std::exit(1);
            }
            map[k++] = a - 256u;
        }
    }

    if (k != kDataCarriers) {
        std::cerr << "Map length mismatch: " << k << "\n";
        std::exit(1);
    }
    return map;
}

void tick(Vfrequency_deinterleaver &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vfrequency_deinterleaver &top) {
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

void check_output(const Vfrequency_deinterleaver &top,
                  const std::array<uint32_t, kDataCarriers> &map,
                  uint32_t output_index,
                  uint64_t symbol_start_sample,
                  uint32_t seed) {
    const uint32_t source = map[output_index];

    if (static_cast<uint32_t>(top.out_source_index) != source) {
        fail_at("source_index", output_index, source, top.out_source_index);
    }
    if (static_cast<uint32_t>(top.out_carrier_index) != output_index) {
        fail_at("carrier_index", output_index, output_index, top.out_carrier_index);
    }
    if (sign_extend_24(top.out_i) != pattern_i(source, seed)) {
        fail_at("i", output_index, pattern_i(source, seed), sign_extend_24(top.out_i));
    }
    if (sign_extend_24(top.out_q) != pattern_q(source, seed)) {
        fail_at("q", output_index, pattern_q(source, seed), sign_extend_24(top.out_q));
    }
    if (top.out_symbol_start_sample != symbol_start_sample) {
        fail_at("symbol_start_sample", output_index, symbol_start_sample, top.out_symbol_start_sample);
    }
    if (top.out_symbol_done != (output_index == kDataCarriers - 1u ? 1u : 0u)) {
        fail_at("symbol_done",
                output_index,
                output_index == kDataCarriers - 1u ? 1u : 0u,
                top.out_symbol_done);
    }
}

void run_symbol(Vfrequency_deinterleaver &top,
                const std::array<uint32_t, kDataCarriers> &map,
                uint64_t symbol_start_sample,
                uint32_t seed) {
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t cycles = 0;

    while (received < kDataCarriers) {
        top.out_ready = ((cycles % 7u) != 3u) ? 1 : 0;
        top.in_valid = sent < kActiveCarriers ? 1 : 0;
        top.in_i = sent < kActiveCarriers ? pack24(pattern_i(sent, seed)) : 0;
        top.in_q = sent < kActiveCarriers ? pack24(pattern_q(sent, seed)) : 0;
        top.in_symbol_start_sample = sent < kActiveCarriers ? symbol_start_sample : 0;
        top.eval();

        const bool output_fire = top.out_valid && top.out_ready;
        const bool input_fire = top.in_valid && top.in_ready;

        if (output_fire) {
            check_output(top, map, received, symbol_start_sample, seed);
            ++received;
        }

        tick(top);

        if (input_fire) {
            ++sent;
        }

        ++cycles;
        if (cycles > (kActiveCarriers + kDataCarriers) * 4u) {
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

void run_clear_test(Vfrequency_deinterleaver &top) {
    top.in_valid = 1;
    top.out_ready = 1;
    top.in_i = pack24(pattern_i(0, 44));
    top.in_q = pack24(pattern_q(0, 44));
    top.in_symbol_start_sample = 77;
    top.eval();
    tick(top);

    top.clear = 1;
    top.in_valid = 0;
    tick(top);
    top.clear = 0;
    top.eval();
    if (top.out_valid || !top.in_ready) {
        std::cerr << "Clear did not return deinterleaver to load state\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    const auto map = build_map();
    Vfrequency_deinterleaver top;

    reset(top);
    run_symbol(top, map, 1000000ull, 1);
    run_clear_test(top);
    run_symbol(top, map, 2002552ull, 9);

    std::cout << "frequency_deinterleaver: PASS\n";
    return 0;
}
