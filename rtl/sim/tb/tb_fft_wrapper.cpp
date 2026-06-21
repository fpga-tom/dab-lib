#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>

#include "Vfft_wrapper.h"
#include "verilated.h"

namespace {

constexpr uint32_t kFftBins = 2048;
constexpr uint32_t kInputMask = (1u << 18u) - 1u;
constexpr uint32_t kOutputMask = (1u << 24u) - 1u;

struct CoreOutput {
    int32_t i;
    int32_t q;
    uint8_t block_exp;
    bool overflow;
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

int32_t input_i(uint32_t sample_index, uint32_t seed) {
    const uint32_t value = ((sample_index * 257u) ^ (seed * 521u) ^ 0x15555u) & kInputMask;
    return sign_extend(value, 18);
}

int32_t input_q(uint32_t sample_index, uint32_t seed) {
    const uint32_t value = ((sample_index * 509u) ^ (seed * 1031u) ^ 0x2aaaau) & kInputMask;
    return sign_extend(value, 18);
}

int32_t output_i(uint32_t bin_index, uint32_t seed) {
    const uint32_t value = ((bin_index * 65537u) ^ (seed * 4099u) ^ 0x0013579bu) & kOutputMask;
    return sign_extend(value, 24);
}

int32_t output_q(uint32_t bin_index, uint32_t seed) {
    const uint32_t value = ((bin_index * 131071u) ^ (seed * 8191u) ^ 0x00eca864u) & kOutputMask;
    return sign_extend(value, 24);
}

void tick(Vfft_wrapper &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vfft_wrapper &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.in_i = 0;
    top.in_q = 0;
    top.in_symbol_start_sample = 0;
    top.core_in_ready = 0;
    top.core_out_valid = 0;
    top.core_out_i = 0;
    top.core_out_q = 0;
    top.core_out_block_exp = 0;
    top.core_out_overflow = 0;
    top.out_ready = 0;
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

void drive_core_output(Vfft_wrapper &top,
                       const std::deque<CoreOutput> &pending,
                       uint32_t cycles) {
    const bool stall_valid = (cycles % 17u) == 9u;
    if (!pending.empty() && !stall_valid) {
        top.core_out_valid = 1;
        top.core_out_i = pack_width(pending.front().i, 24);
        top.core_out_q = pack_width(pending.front().q, 24);
        top.core_out_block_exp = pending.front().block_exp;
        top.core_out_overflow = pending.front().overflow ? 1 : 0;
    } else {
        top.core_out_valid = 0;
        top.core_out_i = 0;
        top.core_out_q = 0;
        top.core_out_block_exp = 0;
        top.core_out_overflow = 0;
    }
}

void check_output(const Vfft_wrapper &top,
                  uint32_t bin_index,
                  uint64_t symbol_start_sample,
                  uint32_t seed) {
    if (top.out_fft_bin_index != bin_index) {
        fail_at("fft_bin_index", bin_index, bin_index, top.out_fft_bin_index);
    }
    if (sign_extend(top.out_i, 24) != output_i(bin_index, seed)) {
        fail_at("i", bin_index, output_i(bin_index, seed), sign_extend(top.out_i, 24));
    }
    if (sign_extend(top.out_q, 24) != output_q(bin_index, seed)) {
        fail_at("q", bin_index, output_q(bin_index, seed), sign_extend(top.out_q, 24));
    }
    if (top.out_symbol_start_sample != symbol_start_sample) {
        fail_at("symbol_start_sample", bin_index, symbol_start_sample, top.out_symbol_start_sample);
    }
    if (top.out_block_exp != static_cast<uint8_t>(3u + (bin_index & 3u))) {
        fail_at("block_exp", bin_index, 3u + (bin_index & 3u), top.out_block_exp);
    }
    if (top.out_overflow != (bin_index == 1234u ? 1u : 0u)) {
        fail_at("overflow", bin_index, bin_index == 1234u ? 1u : 0u, top.out_overflow);
    }
    if (top.out_symbol_done != (bin_index == kFftBins - 1u ? 1u : 0u)) {
        fail_at("symbol_done",
                bin_index,
                bin_index == kFftBins - 1u ? 1u : 0u,
                top.out_symbol_done);
    }
}

void enqueue_fft_symbol(std::deque<CoreOutput> &pending, uint32_t seed) {
    for (uint32_t bin = 0; bin < kFftBins; ++bin) {
        pending.push_back({
            output_i(bin, seed),
            output_q(bin, seed),
            static_cast<uint8_t>(3u + (bin & 3u)),
            bin == 1234u,
        });
    }
}

void run_symbol(Vfft_wrapper &top, uint64_t symbol_start_sample, uint32_t seed) {
    std::deque<CoreOutput> pending_outputs;
    uint32_t sent = 0;
    uint32_t core_received = 0;
    uint32_t received = 0;
    uint32_t cycles = 0;
    bool queued_outputs = false;

    while (received < kFftBins) {
        top.core_in_ready = ((cycles % 11u) != 5u) ? 1 : 0;
        top.out_ready = ((cycles % 13u) != 7u) ? 1 : 0;
        top.in_valid = sent < kFftBins ? 1 : 0;
        top.in_i = sent < kFftBins ? pack_width(input_i(sent, seed), 18) : 0;
        top.in_q = sent < kFftBins ? pack_width(input_q(sent, seed), 18) : 0;
        top.in_symbol_start_sample = sent < kFftBins ? symbol_start_sample : 0;

        if (core_received == kFftBins && !queued_outputs && cycles > (kFftBins + 5u)) {
            enqueue_fft_symbol(pending_outputs, seed);
            queued_outputs = true;
        }

        drive_core_output(top, pending_outputs, cycles);
        top.eval();

        const bool input_fire = top.in_valid && top.in_ready;
        const bool core_input_fire = top.core_in_valid && top.core_in_ready;
        const bool output_fire = top.out_valid && top.out_ready;
        const bool core_output_fire = top.core_out_valid && top.core_out_ready;

        if (core_input_fire) {
            if (core_received != sent) {
                fail_at("core_input_order", core_received, core_received, sent);
            }
            if (sign_extend(top.core_in_i, 18) != input_i(core_received, seed)) {
                fail_at("core_in_i",
                        core_received,
                        input_i(core_received, seed),
                        sign_extend(top.core_in_i, 18));
            }
            if (sign_extend(top.core_in_q, 18) != input_q(core_received, seed)) {
                fail_at("core_in_q",
                        core_received,
                        input_q(core_received, seed),
                        sign_extend(top.core_in_q, 18));
            }
            if (top.core_in_last != (core_received == kFftBins - 1u ? 1u : 0u)) {
                fail_at("core_in_last",
                        core_received,
                        core_received == kFftBins - 1u ? 1u : 0u,
                        top.core_in_last);
            }
            ++core_received;
        }

        if (output_fire) {
            check_output(top, received, symbol_start_sample, seed);
            ++received;
        }

        tick(top);

        if (input_fire) {
            ++sent;
        }
        if (core_output_fire) {
            pending_outputs.pop_front();
        }

        ++cycles;
        if (cycles > kFftBins * 8u) {
            std::cerr << "Timeout sent=" << sent << " core_received=" << core_received
                      << " received=" << received << "\n";
            std::exit(1);
        }
    }

    top.out_ready = 1;
    top.in_valid = 0;
    top.core_out_valid = 0;
    top.eval();
    tick(top);
    top.eval();
    if (!top.in_ready || top.out_valid) {
        std::cerr << "Wrapper did not return to idle after symbol drain\n";
        std::exit(1);
    }
}

void run_clear_test(Vfft_wrapper &top) {
    top.core_in_ready = 1;
    top.out_ready = 1;
    top.in_valid = 1;
    top.in_i = pack_width(input_i(0, 99), 18);
    top.in_q = pack_width(input_q(0, 99), 18);
    top.in_symbol_start_sample = 777;
    top.eval();
    tick(top);

    top.clear = 1;
    top.in_valid = 0;
    top.core_out_valid = 1;
    top.eval();
    if (top.in_ready || top.out_valid || top.core_out_ready) {
        std::cerr << "Clear did not suppress wrapper handshakes\n";
        std::exit(1);
    }
    tick(top);

    top.clear = 0;
    top.core_out_valid = 0;
    top.eval();
    if (!top.in_ready || top.out_valid) {
        std::cerr << "Clear did not return fft_wrapper to idle\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vfft_wrapper top;

    reset(top);
    run_symbol(top, 101010ull, 5);
    run_clear_test(top);
    run_symbol(top, 202020ull, 29);

    std::cout << "fft_wrapper: PASS\n";
    return 0;
}
