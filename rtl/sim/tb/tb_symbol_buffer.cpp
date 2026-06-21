#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "Vsymbol_buffer.h"
#include "verilated.h"

namespace {

constexpr uint32_t kSampleMask = (1u << 16u) - 1u;
constexpr uint32_t kFftSamples = 16;
constexpr uint32_t kNoMalformedBeat = 0xffffffffu;
constexpr uint32_t kNoEarlyLastBeat = 0xffffffffu;

struct Sample {
    int32_t i = 0;
    int32_t q = 0;
};

struct HistBeat {
    bool valid = false;
    int32_t i = 0;
    int32_t q = 0;
    uint64_t sample_index = 0;
    bool first = false;
    bool last = false;
    bool error = false;
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
    return {sign_extend_16(i_value), sign_extend_16(q_value)};
}

class HistoryModel {
public:
    void set(uint64_t index, Sample sample) {
        samples_[index] = sample;
    }

    void write_range(uint64_t start, uint32_t count) {
        for (uint32_t n = 0; n < count; ++n) {
            set(start + n, sample_at(start + n));
        }
    }

    std::vector<HistBeat> build_read(uint64_t start,
                                     uint32_t length,
                                     uint32_t malformed_pos = kNoMalformedBeat,
                                     uint32_t early_last_pos = kNoEarlyLastBeat) const {
        std::vector<HistBeat> beats;

        for (uint32_t n = 0; n < length; ++n) {
            if (samples_.find(start + n) == samples_.end()) {
                beats.push_back({true, 0, 0, start, true, true, true});
                return beats;
            }
        }

        beats.reserve(length);
        for (uint32_t n = 0; n < length; ++n) {
            const uint64_t sample_index = malformed_pos == n ? start + n + 9 : start + n;
            const Sample sample = samples_.at(start + n);
            const bool last = early_last_pos == n ? true : n == length - 1u;
            beats.push_back({true,
                             sample.i,
                             sample.q,
                             sample_index,
                             n == 0u,
                             last,
                             false});
        }
        return beats;
    }

private:
    std::unordered_map<uint64_t, Sample> samples_;
};

void tick(Vsymbol_buffer &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vsymbol_buffer &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.in_symbol_start_sample = 0;
    top.hist_read_req_ready = 0;
    top.hist_rd_out_valid = 0;
    top.hist_rd_out_i = 0;
    top.hist_rd_out_q = 0;
    top.hist_rd_out_sample_index = 0;
    top.hist_rd_out_first = 0;
    top.hist_rd_out_last = 0;
    top.hist_rd_out_error = 0;
    top.out_ready = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void drive_history(Vsymbol_buffer &top, const std::vector<HistBeat> &beats, size_t pos) {
    if (pos < beats.size()) {
        const HistBeat &beat = beats[pos];
        top.hist_rd_out_valid = beat.valid ? 1 : 0;
        top.hist_rd_out_i = pack16(beat.i);
        top.hist_rd_out_q = pack16(beat.q);
        top.hist_rd_out_sample_index = beat.sample_index;
        top.hist_rd_out_first = beat.first ? 1 : 0;
        top.hist_rd_out_last = beat.last ? 1 : 0;
        top.hist_rd_out_error = beat.error ? 1 : 0;
    } else {
        top.hist_rd_out_valid = 0;
        top.hist_rd_out_i = 0;
        top.hist_rd_out_q = 0;
        top.hist_rd_out_sample_index = 0;
        top.hist_rd_out_first = 0;
        top.hist_rd_out_last = 0;
        top.hist_rd_out_error = 0;
    }
}

void fail(const char *message) {
    std::cerr << message << "\n";
    std::exit(1);
}

void check_valid_output(const Vsymbol_buffer &top, uint64_t symbol_start, uint32_t offset) {
    const uint64_t sample_index = symbol_start + offset;
    const Sample expected = sample_at(sample_index);

    if (top.out_error) {
        fail("Unexpected symbol_buffer output error");
    }
    if (sign_extend_16(top.out_i) != expected.i) {
        std::cerr << "i mismatch offset=" << offset << " expected=" << expected.i
                  << " actual=" << sign_extend_16(top.out_i) << "\n";
        std::exit(1);
    }
    if (sign_extend_16(top.out_q) != expected.q) {
        std::cerr << "q mismatch offset=" << offset << " expected=" << expected.q
                  << " actual=" << sign_extend_16(top.out_q) << "\n";
        std::exit(1);
    }
    if (top.out_sample_index != sample_index) {
        std::cerr << "sample_index mismatch expected=" << sample_index
                  << " actual=" << top.out_sample_index << "\n";
        std::exit(1);
    }
    if (top.out_symbol_start_sample != symbol_start) {
        std::cerr << "symbol_start mismatch expected=" << symbol_start
                  << " actual=" << top.out_symbol_start_sample << "\n";
        std::exit(1);
    }
    if (top.out_sample_offset != offset) {
        std::cerr << "offset mismatch expected=" << offset
                  << " actual=" << top.out_sample_offset << "\n";
        std::exit(1);
    }
    if (static_cast<bool>(top.out_symbol_done) != (offset == kFftSamples - 1u)) {
        std::cerr << "done mismatch offset=" << offset << "\n";
        std::exit(1);
    }
}

void run_symbol(Vsymbol_buffer &top,
                const HistoryModel &history,
                uint64_t symbol_start,
                bool expect_error = false,
                uint32_t malformed_pos = kNoMalformedBeat,
                uint32_t early_last_pos = kNoEarlyLastBeat) {
    std::vector<HistBeat> beats;
    size_t beat_pos = 0;
    bool request_sent = false;
    bool error_seen = false;
    uint32_t received = 0;
    uint32_t first_error_pos = malformed_pos;

    if (early_last_pos < first_error_pos) {
        first_error_pos = early_last_pos;
    }

    for (uint32_t cycle = 0; cycle < 400u; ++cycle) {
        top.in_valid = !request_sent ? 1 : 0;
        top.in_symbol_start_sample = symbol_start;
        top.hist_read_req_ready = ((cycle % 5u) != 1u) ? 1 : 0;
        top.out_ready = ((cycle % 7u) != 3u) ? 1 : 0;
        drive_history(top, beats, beat_pos);
        top.eval();

        const bool input_fire = top.in_valid && top.in_ready;
        const bool hist_req_fire = top.hist_read_req_valid && top.hist_read_req_ready;
        const bool hist_fire = top.hist_rd_out_valid && top.hist_rd_out_ready;
        const bool output_fire = top.out_valid && top.out_ready;

        if (top.hist_read_req_valid) {
            if (top.hist_read_req_start_sample != symbol_start ||
                top.hist_read_req_length != kFftSamples) {
                fail("Bad symbol_buffer history read request");
            }
        }

        if (output_fire) {
            if (expect_error) {
                if (top.out_error && top.out_symbol_done) {
                    error_seen = true;
                } else if (first_error_pos != kNoMalformedBeat && received < first_error_pos) {
                    check_valid_output(top, symbol_start, received);
                    ++received;
                } else {
                    std::cerr << "Expected symbol_buffer error output symbol_start=" << symbol_start
                              << " offset=" << static_cast<uint32_t>(top.out_sample_offset)
                              << " out_error=" << static_cast<int>(top.out_error)
                              << " out_done=" << static_cast<int>(top.out_symbol_done) << "\n";
                    fail("Expected symbol_buffer error output");
                }
                if (error_seen && top.out_symbol_start_sample != symbol_start) {
                    fail("Error output lost symbol start metadata");
                }
            } else {
                check_valid_output(top, symbol_start, received);
                ++received;
            }
        }

        tick(top);

        if (input_fire) {
            request_sent = true;
        }
        if (hist_req_fire) {
            beats = history.build_read(top.hist_read_req_start_sample,
                                       top.hist_read_req_length,
                                       malformed_pos,
                                       early_last_pos);
            beat_pos = 0;
        }
        if (hist_fire) {
            ++beat_pos;
        }

        top.eval();
        if (!expect_error && received == kFftSamples) {
            return;
        }
        if (expect_error && error_seen && top.in_ready) {
            return;
        }
    }

    std::cerr << "Timed out waiting for symbol_buffer output symbol_start=" << symbol_start
              << " received=" << received << " expect_error=" << expect_error
              << " request_sent=" << request_sent << "\n";
    std::exit(1);
}

void run_clear_test(Vsymbol_buffer &top) {
    top.in_valid = 1;
    top.in_symbol_start_sample = 1234;
    top.hist_read_req_ready = 1;
    top.out_ready = 1;
    top.clear = 1;
    top.eval();

    if (top.in_ready || top.hist_read_req_valid || top.out_valid) {
        fail("Clear did not suppress symbol_buffer handshakes");
    }

    tick(top);
    top.clear = 0;
    top.in_valid = 0;
    top.eval();

    if (!top.in_ready || top.out_valid) {
        fail("Clear did not return symbol_buffer to idle");
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vsymbol_buffer top;
    HistoryModel history;
    history.write_range(1000, 64);
    history.write_range(2000, 64);

    reset(top);
    run_symbol(top, history, 1000);
    run_symbol(top, history, 2000);
    run_symbol(top, history, 3000, true);
    run_symbol(top, history, 1008, true, 5);
    run_symbol(top, history, 1016, true, kNoMalformedBeat, 4);
    run_symbol(top, history, 2016);
    run_clear_test(top);
    run_symbol(top, history, 2024);

    std::cout << "symbol_buffer: PASS\n";
    return 0;
}
