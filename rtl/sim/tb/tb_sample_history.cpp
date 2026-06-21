#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <vector>

#include "Vsample_history.h"
#include "verilated.h"

namespace {

constexpr uint32_t kSampleMask = (1u << 16u) - 1u;
constexpr uint32_t kDepth = 16;

struct Sample {
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

Sample sample_at(uint64_t index) {
    const uint32_t i_value =
        (static_cast<uint32_t>(index * 811u) ^ 0x1357u) & kSampleMask;
    const uint32_t q_value =
        (static_cast<uint32_t>(index * 1237u) ^ 0x2468u) & kSampleMask;
    return {sign_extend_16(i_value), sign_extend_16(q_value), index};
}

void tick(Vsample_history &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vsample_history &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.in_i = 0;
    top.in_q = 0;
    top.in_sample_index = 0;
    top.read_req_valid = 0;
    top.read_req_start_sample = 0;
    top.read_req_length = 0;
    top.rd_out_ready = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail_at(const char *field, uint64_t sample_index, int64_t expected, int64_t actual) {
    std::cerr << "Mismatch at sample " << sample_index << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void write_sample(Vsample_history &top, const Sample &sample) {
    top.in_valid = 1;
    top.in_i = pack16(sample.i);
    top.in_q = pack16(sample.q);
    top.in_sample_index = sample.index;
    top.read_req_valid = 0;
    top.rd_out_ready = 0;
    top.eval();

    if (!top.in_ready) {
        std::cerr << "sample_history did not accept write\n";
        std::exit(1);
    }

    tick(top);
    top.in_valid = 0;
    top.eval();
}

void write_range(Vsample_history &top, uint64_t start, uint32_t count) {
    for (uint32_t n = 0; n < count; ++n) {
        write_sample(top, sample_at(start + n));
    }
}

void expect_status(const Vsample_history &top,
                   bool valid,
                   uint64_t oldest,
                   uint64_t next,
                   uint32_t stored,
                   bool full) {
    if (static_cast<bool>(top.history_valid) != valid) {
        fail_at("history_valid", next, valid, top.history_valid);
    }
    if (top.oldest_sample_index != oldest) {
        fail_at("oldest_sample_index", next, oldest, top.oldest_sample_index);
    }
    if (top.next_sample_index != next) {
        fail_at("next_sample_index", next, next, top.next_sample_index);
    }
    if (top.stored_samples != stored) {
        fail_at("stored_samples", next, stored, top.stored_samples);
    }
    if (static_cast<bool>(top.history_full) != full) {
        fail_at("history_full", next, full, top.history_full);
    }
}

void start_read(Vsample_history &top, uint64_t start, uint32_t length) {
    top.read_req_valid = 1;
    top.read_req_start_sample = start;
    top.read_req_length = length;
    top.in_valid = 0;
    top.rd_out_ready = 0;
    top.eval();

    if (!top.read_req_ready) {
        std::cerr << "sample_history did not accept read request\n";
        std::exit(1);
    }

    tick(top);
    top.read_req_valid = 0;
    top.eval();
}

void read_window(Vsample_history &top,
                 uint64_t start,
                 uint32_t length,
                 bool expect_error = false) {
    uint32_t received = 0;
    uint32_t cycles = 0;

    start_read(top, start, length);

    while (received == 0 || (received < length && !expect_error)) {
        top.rd_out_ready = ((cycles % 5u) != 2u) ? 1 : 0;
        top.eval();

        if (top.rd_out_valid && top.rd_out_ready) {
            if (expect_error) {
                if (!top.rd_out_error || !top.rd_out_first || !top.rd_out_last ||
                    top.rd_out_sample_index != start) {
                    std::cerr << "Malformed sample_history error response\n";
                    std::exit(1);
                }
                ++received;
            } else {
                const Sample expected = sample_at(start + received);
                if (top.rd_out_error) {
                    std::cerr << "Unexpected sample_history read error\n";
                    std::exit(1);
                }
                if (sign_extend_16(top.rd_out_i) != expected.i) {
                    fail_at("i", expected.index, expected.i, sign_extend_16(top.rd_out_i));
                }
                if (sign_extend_16(top.rd_out_q) != expected.q) {
                    fail_at("q", expected.index, expected.q, sign_extend_16(top.rd_out_q));
                }
                if (top.rd_out_sample_index != expected.index) {
                    fail_at("sample_index",
                            expected.index,
                            expected.index,
                            top.rd_out_sample_index);
                }
                if (static_cast<bool>(top.rd_out_first) != (received == 0u)) {
                    fail_at("first", expected.index, received == 0u, top.rd_out_first);
                }
                if (static_cast<bool>(top.rd_out_last) != (received == length - 1u)) {
                    fail_at("last", expected.index, received == length - 1u, top.rd_out_last);
                }
                ++received;
            }
        }

        tick(top);

        if (++cycles > length * 8u + 32u) {
            std::cerr << "Timeout reading window start=" << start
                      << " length=" << length << " received=" << received << "\n";
            std::exit(1);
        }
    }

    top.rd_out_ready = 1;
    top.eval();
    tick(top);
    top.rd_out_ready = 0;
    top.eval();
}

void run_basic_test(Vsample_history &top) {
    write_range(top, 100, 10);
    expect_status(top, true, 100, 110, 10, false);
    read_window(top, 103, 4);
    read_window(top, 109, 1);
}

void run_wrap_test(Vsample_history &top) {
    write_range(top, 110, 12);
    expect_status(top, true, 106, 122, kDepth, true);
    read_window(top, 106, 16);
    read_window(top, 105, 1, true);
    read_window(top, 121, 2, true);
    read_window(top, 0, 0, true);
}

void run_discontinuity_test(Vsample_history &top) {
    write_range(top, 5000, 5);
    expect_status(top, true, 5000, 5005, 5, false);
    read_window(top, 5000, 5);
    read_window(top, 110, 1, true);
}

void run_clear_test(Vsample_history &top) {
    write_range(top, 8000, 4);

    top.clear = 1;
    top.in_valid = 1;
    top.read_req_valid = 1;
    top.rd_out_ready = 1;
    top.eval();
    if (top.in_ready || top.read_req_ready || top.rd_out_valid) {
        std::cerr << "Clear did not suppress sample_history handshakes\n";
        std::exit(1);
    }
    tick(top);

    top.clear = 0;
    top.in_valid = 0;
    top.read_req_valid = 0;
    top.rd_out_ready = 0;
    top.eval();
    expect_status(top, false, 0, 0, 0, false);
    read_window(top, 8000, 1, true);
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vsample_history top;

    reset(top);
    run_basic_test(top);
    run_wrap_test(top);
    run_discontinuity_test(top);
    run_clear_test(top);

    std::cout << "sample_history: PASS\n";
    return 0;
}
