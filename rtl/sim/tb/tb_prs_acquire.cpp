#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <vector>

#include "Vprs_acquire.h"
#include "verilated.h"

namespace {

constexpr uint32_t kGuardSamples = 504;
constexpr uint32_t kFftSamples = 2048;
constexpr uint32_t kAcquireWindowSamples = 1201;
constexpr uint32_t kHalfWindow = kAcquireWindowSamples / 2;
constexpr uint32_t kRatioMask = (1u << 24u) - 1u;

struct NullEvent {
    uint64_t start;
    uint32_t length;
    uint32_t min_ratio;
};

void tick(Vprs_acquire &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vprs_acquire &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.in_null_start_sample = 0;
    top.in_null_length = 0;
    top.in_null_min_ratio = 0;
    top.out_ready = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail_at(const char *field, uint32_t index, uint64_t expected, uint64_t actual) {
    std::cerr << "Mismatch at candidate " << index << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void check_candidate(const Vprs_acquire &top, const NullEvent &event, uint32_t index) {
    const uint64_t prs_symbol_start = event.start + event.length;
    const uint64_t prs_fft_start = prs_symbol_start + kGuardSamples;
    const uint64_t search_start = prs_fft_start - kHalfWindow;
    const uint64_t search_end = search_start + kAcquireWindowSamples;
    const uint64_t required_end = search_end + kFftSamples;

    if (top.out_null_start_sample != event.start) {
        fail_at("null_start", index, event.start, top.out_null_start_sample);
    }
    if (top.out_null_length != event.length) {
        fail_at("null_length", index, event.length, top.out_null_length);
    }
    if (top.out_null_min_ratio != event.min_ratio) {
        fail_at("null_min_ratio", index, event.min_ratio, top.out_null_min_ratio);
    }
    if (top.out_prs_symbol_start_sample != prs_symbol_start) {
        fail_at("prs_symbol_start", index, prs_symbol_start, top.out_prs_symbol_start_sample);
    }
    if (top.out_prs_fft_start_sample != prs_fft_start) {
        fail_at("prs_fft_start", index, prs_fft_start, top.out_prs_fft_start_sample);
    }
    if (top.out_timing_search_start_offset != kHalfWindow) {
        fail_at("timing_search_start_offset",
                index,
                kHalfWindow,
                top.out_timing_search_start_offset);
    }
    if (top.out_timing_search_length != kAcquireWindowSamples) {
        fail_at("timing_search_length",
                index,
                kAcquireWindowSamples,
                top.out_timing_search_length);
    }
    if (top.out_search_start_sample != search_start) {
        fail_at("search_start", index, search_start, top.out_search_start_sample);
    }
    if (top.out_search_end_sample != search_end) {
        fail_at("search_end", index, search_end, top.out_search_end_sample);
    }
    if (top.out_required_end_sample != required_end) {
        fail_at("required_end", index, required_end, top.out_required_end_sample);
    }
}

void run_candidates(Vprs_acquire &top) {
    const std::vector<NullEvent> events = {
        {100000u, 2656u, 0x00019999u & kRatioMask},
        {196608u + 200u, 2638u, 0x0002aaaau & kRatioMask},
        {2ull * 196608u + 111u, 2671u, 0x00005555u & kRatioMask},
    };

    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t cycles = 0;

    while (received < events.size()) {
        top.out_ready = ((cycles % 7u) != 3u) ? 1 : 0;
        top.in_valid = sent < events.size() ? 1 : 0;
        top.in_null_start_sample = sent < events.size() ? events[sent].start : 0;
        top.in_null_length = sent < events.size() ? events[sent].length : 0;
        top.in_null_min_ratio = sent < events.size() ? events[sent].min_ratio : 0;
        top.eval();

        const bool input_fire = top.in_valid && top.in_ready;
        const bool output_fire = top.out_valid && top.out_ready;

        if (output_fire) {
            check_candidate(top, events[received], received);
            ++received;
        }

        tick(top);

        if (input_fire) {
            ++sent;
        }

        ++cycles;
        if (cycles > 64u) {
            std::cerr << "Timeout sent=" << sent << " received=" << received << "\n";
            std::exit(1);
        }
    }

    top.in_valid = 0;
    top.out_ready = 1;
    tick(top);
    top.eval();
    if (top.out_valid || !top.in_ready) {
        std::cerr << "prs_acquire did not return to idle\n";
        std::exit(1);
    }
}

void run_clear_test(Vprs_acquire &top) {
    top.out_ready = 0;
    top.in_valid = 1;
    top.in_null_start_sample = 1234;
    top.in_null_length = 2656;
    top.in_null_min_ratio = 0x00012345u;
    top.eval();
    tick(top);

    if (!top.out_valid || top.in_ready) {
        std::cerr << "Clear setup did not create a pending candidate\n";
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
    if (!top.in_ready || top.out_valid) {
        std::cerr << "Clear did not return prs_acquire to idle\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vprs_acquire top;

    reset(top);
    run_candidates(top);
    run_clear_test(top);
    run_candidates(top);

    std::cout << "prs_acquire: PASS\n";
    return 0;
}
