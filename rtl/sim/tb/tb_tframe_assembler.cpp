#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Vtframe_assembler.h"
#include "verilated.h"

namespace {

constexpr uint32_t kFicSymbols = 3;
constexpr uint32_t kMscSymbols = 72;
constexpr uint32_t kTotalSymbols = kFicSymbols + kMscSymbols;
constexpr uint32_t kSoftbitsPerSymbol = 3072;
constexpr uint32_t kTotalSoftbits = kTotalSymbols * kSoftbitsPerSymbol;

int8_t pattern(uint32_t index, uint32_t seed) {
    return static_cast<int8_t>(((index * 17u) + (seed * 29u) + 3u) & 0xffu);
}

void tick(Vtframe_assembler &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vtframe_assembler &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.out_ready = 0;
    top.in_softbit = 0;
    top.in_symbol_start_sample = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail_at(const char *field, uint32_t expected_index, uint64_t expected, uint64_t actual) {
    std::cerr << "Mismatch at output " << expected_index << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void check_output(const Vtframe_assembler &top,
                  uint32_t output_index,
                  uint64_t frame_start_sample,
                  uint32_t seed) {
    const uint32_t symbol = output_index / kSoftbitsPerSymbol;
    const uint32_t byte = output_index % kSoftbitsPerSymbol;
    const uint32_t expected_region = symbol < kFicSymbols ? 0u : 1u;
    const uint32_t expected_addr =
        symbol < kFicSymbols ? output_index : ((symbol - kFicSymbols) * kSoftbitsPerSymbol) + byte;
    const uint32_t expected_done = output_index == (kTotalSoftbits - 1u) ? 1u : 0u;
    const int8_t expected_softbit = pattern(output_index, seed);

    if (static_cast<int8_t>(top.out_softbit) != expected_softbit) {
        fail_at("softbit",
                output_index,
                static_cast<uint8_t>(expected_softbit),
                static_cast<uint8_t>(top.out_softbit));
    }
    if (top.out_region != expected_region) {
        fail_at("region", output_index, expected_region, top.out_region);
    }
    if (top.out_addr != expected_addr) {
        fail_at("addr", output_index, expected_addr, top.out_addr);
    }
    if (top.out_symbol_index != symbol) {
        fail_at("symbol_index", output_index, symbol, top.out_symbol_index);
    }
    if (top.out_byte_index != byte) {
        fail_at("byte_index", output_index, byte, top.out_byte_index);
    }
    if (top.out_frame_start_sample != frame_start_sample) {
        fail_at("frame_start_sample", output_index, frame_start_sample, top.out_frame_start_sample);
    }
    if (top.out_frame_done != expected_done) {
        fail_at("frame_done", output_index, expected_done, top.out_frame_done);
    }
}

void run_frame(Vtframe_assembler &top, uint64_t frame_start_sample, uint32_t seed) {
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t cycles = 0;

    while (received < kTotalSoftbits) {
        const bool have_input = sent < kTotalSoftbits;
        top.out_ready = ((cycles % 11u) != 4u) ? 1 : 0;
        top.in_valid = have_input ? 1 : 0;
        top.in_softbit = have_input ? pattern(sent, seed) : 0;
        top.in_symbol_start_sample =
            have_input ? frame_start_sample + ((sent / kSoftbitsPerSymbol) * 2552ull) : 0;
        top.eval();

        const bool output_fire = top.out_valid && top.out_ready;
        const bool input_fire = top.in_valid && top.in_ready;

        if (output_fire) {
            check_output(top, received, frame_start_sample, seed);
            ++received;
        }

        tick(top);

        if (input_fire) {
            ++sent;
        }

        ++cycles;
        if (cycles > (kTotalSoftbits * 3u)) {
            std::cerr << "Timeout sent=" << sent << " received=" << received << "\n";
            std::exit(1);
        }
    }

    top.out_ready = 1;
    top.in_valid = 0;
    tick(top);
    top.eval();
    if (top.out_valid) {
        std::cerr << "Output valid remained asserted after frame drain\n";
        std::exit(1);
    }
}

void run_clear_test(Vtframe_assembler &top) {
    top.out_ready = 1;
    top.in_valid = 1;
    top.in_softbit = pattern(0, 99);
    top.in_symbol_start_sample = 42;
    top.eval();
    tick(top);

    top.clear = 1;
    top.in_valid = 0;
    tick(top);
    top.clear = 0;
    top.eval();
    if (top.out_valid || !top.in_ready) {
        std::cerr << "Clear did not return assembler to idle state\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vtframe_assembler top;
    reset(top);
    run_frame(top, 123456789ull, 1);
    run_clear_test(top);
    run_frame(top, 987654321ull, 7);

    std::cout << "tframe_assembler: PASS\n";
    return 0;
}
