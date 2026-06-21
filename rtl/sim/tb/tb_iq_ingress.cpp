#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <vector>

#include "Viq_ingress.h"
#include "verilated.h"

namespace {

constexpr uint32_t kSampleMask = (1u << 16u) - 1u;

struct Sample {
    int32_t i;
    int32_t q;
};

struct Expected {
    int32_t i;
    int32_t q;
    uint64_t index;
};

uint32_t pack16(int32_t value) {
    return static_cast<uint32_t>(value) & kSampleMask;
}

int32_t sign_extend_16(uint32_t value) {
    value &= kSampleMask;
    if ((value & (1u << 15u)) != 0u) {
        value |= ~kSampleMask;
    }
    return static_cast<int32_t>(value);
}

Sample sample_at(uint32_t n) {
    const uint32_t i_value = ((n * 811u) ^ 0x1357u) & kSampleMask;
    const uint32_t q_value = ((n * 1237u) ^ 0x2468u) & kSampleMask;
    return {sign_extend_16(i_value), sign_extend_16(q_value)};
}

void tick(Viq_ingress &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Viq_ingress &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.set_sample_index_valid = 0;
    top.set_sample_index_value = 0;
    top.in_valid = 0;
    top.in_i = 0;
    top.in_q = 0;
    top.out_ready = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail_at(const char *field, uint32_t sample, int64_t expected, int64_t actual) {
    std::cerr << "Mismatch at sample " << sample << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void drive_set_index(Viq_ingress &top, uint64_t index) {
    top.set_sample_index_valid = 1;
    top.set_sample_index_value = index;
    top.in_valid = 1;
    top.in_i = pack16(-123);
    top.in_q = pack16(456);
    top.out_ready = 1;
    top.eval();

    if (top.in_ready || top.out_valid) {
        std::cerr << "set_sample_index did not stall sample stream\n";
        std::exit(1);
    }

    tick(top);
    top.set_sample_index_valid = 0;
    top.in_valid = 0;
    top.eval();
}

void run_stream(Viq_ingress &top, uint64_t start_index, uint32_t count, uint32_t seed) {
    std::deque<Expected> expected;
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t cycles = 0;

    drive_set_index(top, start_index);

    while (sent < count || !expected.empty()) {
        const bool have_input = sent < count;
        const Sample sample = have_input ? sample_at(sent + seed) : Sample{0, 0};

        top.out_ready = ((cycles % 7u) != 3u) ? 1 : 0;
        top.in_valid = have_input ? 1 : 0;
        top.in_i = have_input ? pack16(sample.i) : 0;
        top.in_q = have_input ? pack16(sample.q) : 0;
        top.eval();

        const bool input_fire = top.in_valid && top.in_ready;
        const bool output_fire = top.out_valid && top.out_ready;

        if (input_fire) {
            expected.push_back({sample.i, sample.q, start_index + sent});
            ++sent;
        }

        if (output_fire) {
            if (expected.empty()) {
                std::cerr << "Unexpected ingress output at cycle " << cycles << "\n";
                std::exit(1);
            }

            const Expected &front = expected.front();
            if (sign_extend_16(top.out_i) != front.i) {
                fail_at("i", received, front.i, sign_extend_16(top.out_i));
            }
            if (sign_extend_16(top.out_q) != front.q) {
                fail_at("q", received, front.q, sign_extend_16(top.out_q));
            }
            if (top.out_sample_index != front.index) {
                fail_at("sample_index", received, front.index, top.out_sample_index);
            }

            expected.pop_front();
            ++received;
        }

        tick(top);

        if (++cycles > count * 6u + 32u) {
            std::cerr << "Timeout sent=" << sent << " received=" << received
                      << " pending=" << expected.size() << "\n";
            std::exit(1);
        }
    }
}

void run_clear_test(Viq_ingress &top) {
    drive_set_index(top, 1000);

    top.in_valid = 1;
    top.in_i = pack16(77);
    top.in_q = pack16(-88);
    top.out_ready = 0;
    top.eval();
    if (top.in_ready) {
        std::cerr << "Ingress accepted input while downstream was stalled\n";
        std::exit(1);
    }

    top.clear = 1;
    top.out_ready = 1;
    top.eval();
    if (top.in_ready || top.out_valid) {
        std::cerr << "Clear did not suppress ingress handshakes\n";
        std::exit(1);
    }
    tick(top);

    top.clear = 0;
    top.in_valid = 0;
    top.out_ready = 1;
    top.eval();
    if (!top.in_ready || top.out_valid || top.out_sample_index != 0u) {
        std::cerr << "Clear did not reset iq_ingress to idle index zero\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Viq_ingress top;

    reset(top);
    run_stream(top, 42, 64, 0);
    run_stream(top, 9000000000ull, 37, 1000);
    run_clear_test(top);
    run_stream(top, 1234, 12, 2000);

    std::cout << "iq_ingress: PASS\n";
    return 0;
}
